# Virtual Soundcard Driver for Linux Kernel

`vsnd` implements a Linux device driver that introduces a virtual soundcard.
Typically, a sound card is a hardware component that enables computers to play
audio files. This virtual soundcard, however, is designed to transmit audio
PCM data received from various programs directly into a FIFO file.

## Prerequisites

The following packages must be installed before building `vsnd`.

To compile the kernel driver successfully, package versions of currently used
kernel, kernel-devel and kernel-headers need to be matched.

```shell
$ sudo apt install linux-headers-$(uname -r)
```

Additional packages are required for verification purpose.

```shell
$ sudo apt install alsa-utils ffmpeg
```

## Build and Run

After running make, you should be able to generate the file `vsnd.ko`.

Before loading this kernel module, you have to satisfy its dependency:

```shell
$ sudo modprobe snd_pcm
```

A FIFO file is required during kernel module initialization and is used for
transmitting audio PCM data.

```shell
$ mkfifo /tmp/audio.pcm
```

The module can be loaded to Linux kernel by runnning the command:

```shell
$ sudo insmod vsnd.ko out_fifo_name=/tmp/audio.pcm
```

Then, use [aplay](https://manpages.org/aplay) to check the soundcard device
provided by `vsnd`.

```shell
$ aplay -l
```

Reference output:

```
card 0: vsnd [vsnd], device 0: vsnd PCM [vsnd PCM]
  Subdevices: 1/1
  Subdevice #0: subdevice #0
```

See [scripts/verify.sh](scripts/verify.sh) for automated test.

### Run on PulseAudio-enabled environment

If your system uses PulseAudio to control the sound ouput
device (i.e., sink), you may undergo
with the following error when you select the sink
as `vsnd` from PulseAudio then you try to remove `vsnd`:

```shell
$ sudo rmmod vsnd
rmmod: ERROR: Module vsnd is in use
```

The reason is that PulseAudio occupies `vsnd`. Though you can
tell PulseAudio to disable `vsnd` so that you can remove `vsnd`,
we suggest you to disable temporarily when using `vsnd` by the following
command:

```shell
$ systemctl --user stop pulseaudio.socket
$ systemctl --user stop pulseaudio.service
```

After using `vsnd`, you can execute the following command to bring
back PulseAudio:

```shell
$ systemctl --user start pulseaudio.service
$ systemctl --user start pulseaudio.socket
```

## License

`vsnd`is released under the MIT license. Use of this source code is governed by
a MIT-style license that can be found in the LICENSE file.

## Reference

* [The ALSA Driver API](https://www.kernel.org/doc/html/latest/sound/kernel-api/alsa-driver-api.html)
