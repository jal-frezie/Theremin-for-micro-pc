VOICE = autotune.wav prange.wav standby.wav tuning.wav \
	play.wav pslope.wav tone.wav vrange.wav

all: mts $(VOICE)

mts: mts.o mtp.o
	gcc -o mts mts.o mtp.o -lpigpio -lasound -lm

mtp.o: mtp.c
	gcc -c -o mtp.o mtp.c

mts.o: mts.c
	gcc -c -o mts.o mts.c

install:
	chown root:audio mts
	chmod a+s mts
	mv mts /usr/local/bin
	mkdir -p /usr/local/lib/mts
	cp $(VOICE) /usr/local/lib/mts
