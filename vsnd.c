#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#define BITS_PER_BYTE 8

#define DRIVER_NAME "vsnd"
#define PCM_NAME DRIVER_NAME " PCM"
#define DEFAULT_PCM_FORMAT SNDRV_PCM_FMTBIT_S16_LE
#define DEFAULT_PCM_FREQ 22050
#define DEFAULT_PCM_RATE SNDRV_PCM_RATE_22050
#define DEFAULT_PCM_CHANNELS 1

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("Audio loopback device");
MODULE_VERSION("0.1");

static int is_fifo_file(char *filename)
{
    struct path path;
    struct inode *inode;
    int err;

    if (!filename) {
        err = -EIO;
        goto finally;
    }

    err = kern_path(filename, LOOKUP_OPEN, &path);
    if (err < 0)
        goto finally;

    inode = path.dentry->d_inode;
    err = ((inode->i_mode & S_IFMT) == S_IFIFO ? 0 : -EIO);

    path_put(&path);

finally:
    return !(err < 0);
}

/* NOTE: SNDRV_CARDS is the maximum number of cards supported by this module,
 * default value 8
 */
static int index[SNDRV_CARDS] =
    SNDRV_DEFAULT_IDX; /* Index [0...(SNDRV_CARDS - 1)] */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR; /* ID for this card */
static bool enable[SNDRV_CARDS] =
    SNDRV_DEFAULT_ENABLE; /* Enable the first card */
static int input_pcm_type[SNDRV_CARDS] = {
    [0 ...(SNDRV_CARDS - 1)] = DEFAULT_PCM_FORMAT,
};
static int input_pcm_freq[SNDRV_CARDS] = {
    [0 ...(SNDRV_CARDS - 1)] = DEFAULT_PCM_FREQ,
};
static char *out_fifo_name[SNDRV_CARDS] = SNDRV_DEFAULT_STR;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for sound card");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for sound card");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable this sound card");
module_param_array(input_pcm_type, int, NULL, 0444);
MODULE_PARM_DESC(input_pcm_type, "Supported input PCM format");
module_param_array(input_pcm_freq, int, NULL, 0444);
MODULE_PARM_DESC(input_pcm_freq, "Supported input PCM frequency");
module_param_array(out_fifo_name, charp, NULL, 0444);
MODULE_PARM_DESC(out_fifo_name, "FIFO file to write output to");

/* Data structure definitions */
struct vsnd_setup {
    snd_pcm_format_t format;
    unsigned int rate;
    unsigned int channels;
};

struct vsnd {
    struct snd_card *card;
    struct mutex lock;
    spinlock_t spin_lock;
    struct snd_pcm *pcm;
    struct vsnd_setup setup;
    void *fifo_fp;
    unsigned int running : 1;
    unsigned int paused : 1;
};

struct vsnd_pcm {
    struct vsnd *vsnd;
    struct snd_pcm_substream *substream;
    unsigned int pcm_buffer_size;
    unsigned int buf_pos;
    unsigned int silent_size;
    unsigned int pcm_period_size;
    unsigned int pcm_bps;
    unsigned int pcm_salign;
    unsigned int period_update_pending : 1;
    unsigned int irq_pos;
    unsigned int period_size_frac; /* period size in jiffies ticks */
    unsigned int last_drift;
    unsigned long last_jiffies;
    struct timer_list timer;
};

/* Function declarations */
static int vsnd_probe(struct platform_device *);
static int vsnd_open(struct snd_pcm_substream *);
static int vsnd_close(struct snd_pcm_substream *);
static int vsnd_hw_free(struct snd_pcm_substream *);
static int vsnd_prepare(struct snd_pcm_substream *);
static int vsnd_trigger(struct snd_pcm_substream *, int);
static snd_pcm_uframes_t vsnd_pointer(struct snd_pcm_substream *);
static int vsnd_suspend(struct device *);
static int vsnd_resume(struct device *);
static void vsnd_unregister_all(void);
static void vsnd_runtime_free(struct snd_pcm_runtime *);

/* Global variables */
static struct platform_device *devices[SNDRV_CARDS] = {
    [0 ...(SNDRV_CARDS - 1)] = NULL};
static SIMPLE_DEV_PM_OPS(vsnd_pm, vsnd_suspend, vsnd_resume);

static struct platform_driver vsnd_driver = {
    .probe = vsnd_probe,
    .driver = {.name = DRIVER_NAME, .owner = THIS_MODULE, .pm = &vsnd_pm},
};

static const struct snd_pcm_ops vsnd_pcm_ops = {
    .open = vsnd_open,
    .close = vsnd_close,
    .hw_free = vsnd_hw_free,
    .prepare = vsnd_prepare,
    .trigger = vsnd_trigger,
    .pointer = vsnd_pointer,
};

static const struct snd_pcm_hardware vsnd_pcm_hardware = {
    .info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
             SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID),
    .formats = DEFAULT_PCM_FORMAT,
    .rates = DEFAULT_PCM_RATE,
    .rate_min = DEFAULT_PCM_FREQ,
    .rate_max = DEFAULT_PCM_FREQ,
    .channels_min = DEFAULT_PCM_CHANNELS,
    .channels_max = DEFAULT_PCM_CHANNELS,
    .buffer_bytes_max = 2 * 1024 * 1024,
    .period_bytes_min = 4096,
    .period_bytes_max = 1024 * 1024,
    .periods_min = 1,
    .periods_max = 1024,
    .fifo_size = 0,
};

static void vsnd_timer_set(struct vsnd_pcm *pcm_data)
{
    unsigned long tick;

    pcm_data->period_size_frac = pcm_data->pcm_period_size * HZ;

    if (pcm_data->period_size_frac <= pcm_data->irq_pos) {
        pcm_data->irq_pos %= pcm_data->period_size_frac;
        pcm_data->period_update_pending = 1;
    }

    tick = pcm_data->period_size_frac - pcm_data->irq_pos;
    tick = DIV_ROUND_UP(tick, pcm_data->pcm_bps);
    mod_timer(&pcm_data->timer, jiffies + tick);
}

static inline unsigned int byte_pos_calc(struct vsnd_pcm *pcm_data,
                                         unsigned int x)
{
    x /= HZ;
    return x - (x % pcm_data->pcm_salign);
}

static inline unsigned int vsnd_pos_calc(struct vsnd_pcm *pcm_data,
                                         unsigned int delta)
{
    unsigned int last_pos;

    last_pos = byte_pos_calc(pcm_data, pcm_data->irq_pos);
    pcm_data->irq_pos += delta * pcm_data->pcm_bps;
    delta = byte_pos_calc(pcm_data, pcm_data->irq_pos) - last_pos;
    if (delta >= pcm_data->last_drift)
        delta -= pcm_data->last_drift;

    pcm_data->last_drift = 0;
    if (pcm_data->irq_pos >= pcm_data->period_size_frac) {
        pcm_data->irq_pos %= pcm_data->period_size_frac;
        pcm_data->period_update_pending = 1;
    }

    return delta;
}

static unsigned int vsnd_pos_update(struct vsnd_pcm *pcm_data)
{
    struct vsnd *vsnd = pcm_data->vsnd;
    unsigned int running = vsnd->running ^ vsnd->paused;
    char *src_data = pcm_data->substream->runtime->dma_area;
    unsigned int src_off = pcm_data->buf_pos;
    unsigned int delta = 0, bytes_to_write, byte_left_to_write;

    if (running) {
        delta = jiffies - pcm_data->last_jiffies;
        pcm_data->last_jiffies += delta;
    }

    if (delta == 0)
        goto finally;

    bytes_to_write = vsnd_pos_calc(pcm_data, delta);
    byte_left_to_write = bytes_to_write;

    do {
        unsigned int size = byte_left_to_write;
        if (src_off + size > pcm_data->pcm_buffer_size)
            size = pcm_data->pcm_buffer_size - src_off;
        kernel_write(vsnd->fifo_fp, src_data + src_off, size, NULL);
        byte_left_to_write -= size;
        src_off = (src_off + size) % pcm_data->pcm_buffer_size;
    } while (byte_left_to_write > 0);

    pcm_data->buf_pos += bytes_to_write;
    pcm_data->buf_pos %= pcm_data->pcm_buffer_size;

finally:
    return running;
}

static void vsnd_timer_elapsed(struct timer_list *tl)
{
    struct vsnd_pcm *pcm_data = from_timer(pcm_data, tl, timer);
    struct vsnd *vsnd = pcm_data->vsnd;

    if (vsnd_pos_update(pcm_data)) {
        unsigned long flags;
        spin_lock_irqsave(&vsnd->spin_lock, flags);

        vsnd_timer_set(pcm_data);

        if (pcm_data->period_update_pending) {
            pcm_data->period_update_pending = 0;
            spin_unlock_irqrestore(&vsnd->spin_lock, flags);
            snd_pcm_period_elapsed(pcm_data->substream);
            return;
        }

        spin_unlock_irqrestore(&vsnd->spin_lock, flags);
    }
}

static inline void vsnd_timer_delete(struct vsnd_pcm *pcm_data, int sync)
{
    if (sync) {
        del_timer_sync(&pcm_data->timer);
    } else {
        del_timer(&pcm_data->timer);
        pcm_data->timer.expires = 0;
    }
}

static int vsnd_probe(struct platform_device *devptr)
{
    /* Create a fake sound card, a PCM stream for it and then a FIFO file */
    struct snd_card *card;
    struct vsnd *vsnd;
    struct snd_pcm *pcm;
    int err;

    err = snd_devm_card_new(&devptr->dev, index[devptr->id], id[devptr->id],
                            THIS_MODULE, sizeof(struct vsnd), &card);
    if (err < 0) {
        pr_err("Failed to create a new soundcard.");
        return err;
    }

    /* Set details of this new sound card */
    strcpy(card->driver, DRIVER_NAME);
    strcpy(card->shortname, DRIVER_NAME);
    sprintf(card->longname, DRIVER_NAME "%i", devptr->id + 1);

    vsnd = card->private_data;

    mutex_init(&vsnd->lock);
    spin_lock_init(&vsnd->spin_lock);

    vsnd->card = card;

    /* Then create a new PCM stream and attach it to this card */
    err = snd_pcm_new(card, PCM_NAME, devptr->id, 1, 0, &pcm);
    if (err < 0) {
        pr_err("Failed to create a new PCM stream for soundcard.");
        return err;
    }

    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &vsnd_pcm_ops);
    snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);
    pcm->private_data = vsnd;
    pcm->info_flags = 0;
    strcpy(pcm->name, PCM_NAME);
    vsnd->pcm = pcm;

    err = snd_card_register(card);
    if (err < 0) {
        pr_err("Failed to register sound card.");
        return err;
    }

    platform_set_drvdata(devptr, card);

    vsnd->fifo_fp = NULL;
    vsnd->running = vsnd->paused = 0;

    return 0;
}

static int vsnd_open(struct snd_pcm_substream *substream)
{
    struct vsnd *vsnd = substream->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct vsnd_pcm *pcm_data;
    int dev_id = substream->pcm->device;
    void *fifo_fp;
    int err = 0;

    /* Create and configure PCM data structures */
    pcm_data = kzalloc(sizeof(*pcm_data), GFP_KERNEL);
    if (!pcm_data)
        return -ENOMEM;

    mutex_lock(&vsnd->lock);

    /* Open FIFO output file (writable only by us but readable by everyone else)
     * where we will write sound data */
    /* CAUTION: We need to open this FIFO pipe as for both read-write as opening
     * for write-only will cause 'flip_close()' to crash.
     */
    fifo_fp = filp_open(out_fifo_name[dev_id], O_RDWR | O_NONBLOCK, 0);
    if (IS_ERR(fifo_fp)) {
        pr_err("Failed to open FIFO file.");
        err = -EIO;
        goto finally;
    }
    vsnd->fifo_fp = fifo_fp;

    /* Set PCM data */
    pcm_data->vsnd = vsnd;
    pcm_data->substream = substream;

    /* Set runtime properties */
    runtime->private_data = pcm_data;
    runtime->private_free = vsnd_runtime_free;
    runtime->hw = vsnd_pcm_hardware;

    /* Number of periods must always be an integer */
    err = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
    if (err < 0)
        goto finally;

    /* Create jiffies timer */
    timer_setup(&pcm_data->timer, vsnd_timer_elapsed, 0);

finally:
    if (err < 0)
        kfree(pcm_data);

    mutex_unlock(&vsnd->lock);

    return err;
}

static int vsnd_close(struct snd_pcm_substream *substream)
{
    struct vsnd *vsnd = substream->private_data;
    struct vsnd_pcm *pcm_data = substream->runtime->private_data;

    vsnd_timer_delete(pcm_data, 1);

    mutex_lock(&vsnd->lock);
    if (vsnd->fifo_fp) {
        filp_close(vsnd->fifo_fp, NULL);
        vsnd->fifo_fp = NULL;
    }
    mutex_unlock(&vsnd->lock);

    return 0;
}

static int vsnd_hw_free(struct snd_pcm_substream *substream)
{
    return 0;
}

static int vsnd_prepare(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct vsnd_pcm *pcm_data = runtime->private_data;
    int salign, bits_per_secs;
    int err = 0;

    vsnd_timer_delete(pcm_data, 1);

    salign =
        (snd_pcm_format_physical_width(runtime->format) * runtime->channels) /
        BITS_PER_BYTE;
    bits_per_secs = salign * runtime->rate;
    if (salign <= 0 || bits_per_secs <= 0) {
        err = -EINVAL;
        goto finally;
    }

    /* Apply new settings to PCM data structure */
    pcm_data->buf_pos = 0;
    pcm_data->pcm_buffer_size = frames_to_bytes(runtime, runtime->buffer_size);
    pcm_data->irq_pos = 0;
    pcm_data->period_update_pending = 0;
    pcm_data->pcm_bps = bits_per_secs;
    pcm_data->pcm_salign = salign;
    pcm_data->pcm_period_size = frames_to_bytes(runtime, runtime->period_size);

finally:
    return err;
}

static int vsnd_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct vsnd_pcm *pcm_data = runtime->private_data;
    struct vsnd *vsnd = pcm_data->vsnd;
    int err = 0;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        pr_info("START: channels %d rate %d", runtime->channels, runtime->rate);
        pcm_data->last_jiffies = jiffies;
        pcm_data->last_drift = 0;
        spin_lock(&vsnd->spin_lock);
        vsnd->running = 1;
        vsnd->paused = 0;
        vsnd_timer_set(pcm_data);
        spin_unlock(&vsnd->spin_lock);
        break;

    case SNDRV_PCM_TRIGGER_STOP:
        pr_info("STOP");
        spin_lock(&vsnd->spin_lock);
        vsnd->running = 0;
        vsnd->paused = 0;
        vsnd_timer_delete(pcm_data, 0);
        spin_unlock(&vsnd->spin_lock);
        break;

    case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
    case SNDRV_PCM_TRIGGER_SUSPEND:
        spin_lock(&vsnd->spin_lock);
        vsnd->paused = 1;
        vsnd_timer_delete(pcm_data, 0);
        spin_unlock(&vsnd->spin_lock);
        break;

    case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
    case SNDRV_PCM_TRIGGER_RESUME:
        spin_lock(&vsnd->spin_lock);
        pcm_data->last_jiffies = jiffies;
        vsnd->paused = 0;
        vsnd_timer_set(pcm_data);
        spin_unlock(&vsnd->spin_lock);
        break;

    default:
        err = -EINVAL;
        goto finally;
    }

finally:
    return err;
}

static snd_pcm_uframes_t vsnd_pointer(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct vsnd_pcm *pcm_data = runtime->private_data;
    struct vsnd *vsnd = pcm_data->vsnd;
    snd_pcm_uframes_t pos;

    spin_lock(&vsnd->spin_lock);
    pos = pcm_data->buf_pos;
    spin_unlock(&vsnd->spin_lock);
    return bytes_to_frames(runtime, pos);
}

static int vsnd_suspend(struct device *pdev)
{
    struct snd_card *card = dev_get_drvdata(pdev);

    snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);
    return 0;
}

static int vsnd_resume(struct device *pdev)
{
    struct snd_card *card = dev_get_drvdata(pdev);

    snd_power_change_state(card, SNDRV_CTL_POWER_D0);
    return 0;
}

static void vsnd_unregister_all(void)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(devices); ++i) {
        if (devices[i])
            platform_device_unregister(devices[i]);
    }

    platform_driver_unregister(&vsnd_driver);
}

static void vsnd_runtime_free(struct snd_pcm_runtime *runtime)
{
    struct vsnd_pcm *pcm_data = runtime->private_data;
    kfree(pcm_data);
}

static int __init vsnd_init(void)
{
    /* Register our driver with Platform Device */
    int i, err, device_count;

    err = platform_driver_register(&vsnd_driver);
    if (err < 0) {
        pr_err("Failed to register.");
        return err;
    }

    device_count = 0;
    for (i = 0; i < SNDRV_CARDS; i++) {
        /* Register a device with the driver */
        struct platform_device *device;

        if (!enable[i])
            continue;

        if (!is_fifo_file(out_fifo_name[i])) {
            pr_err("FIFO filename is not provided or its type is no FIFO.");
            continue;
        }

        device = platform_device_register_simple(DRIVER_NAME, i, NULL, 0);
        if (IS_ERR(device))
            continue;

        if (!platform_get_drvdata(device)) {
            platform_device_unregister(device);
            continue;
        }

        devices[i] = device;
        ++device_count;
    }

    if (!device_count) {
        pr_err("Failed to register any device.");
        vsnd_unregister_all();
        return -ENODEV;
    }

    return 0;
}

static void __exit vsnd_exit(void)
{
    vsnd_unregister_all();
}

module_init(vsnd_init);
module_exit(vsnd_exit);
