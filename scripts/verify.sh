#!/bin/bash

set -x

sudo modprobe snd_pcm
sudo rmmod vsnd

rm -f /tmp/audio.pcm
mkfifo /tmp/audio.pcm
sudo insmod vsnd.ko out_fifo_name=/tmp/audio.pcm

tee out.pcm < /tmp/audio.pcm >/dev/null &
sleep 1
aplay -D plughw:CARD=vsnd,DEV=0 CantinaBand3.wav
sudo rmmod vsnd

ffmpeg -f s16le -ar 22.05k -ac 1 -loglevel 8 -i out.pcm out.wav
