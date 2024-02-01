obj-m := vsnd.o
KDIR = /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

TEST_WAV=CantinaBand3.wav
$(TEST_WAV):
	curl -o $@ https://www2.cs.uic.edu/~i101/SoundFiles/CantinaBand3.wav

check: all $(TEST_WAV)
	scripts/verify.sh

clean:
	make -C $(KDIR) M=$(PWD) clean

distclean: clean
	$(RM) $(TEST_WAV)
	$(RM) out.pcm out.wav
