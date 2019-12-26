VOICE = autotune.wav prange.wav standby.wav tuning.wav \
	play.wav pslope.wav tone.wav vrange.wav

all: mts $(VOICE)

ultra: umts $(VOICE)

umts: mtp.o uts.o wiringPiSPI.o
	gcc -o umts uts.o mtp.o wiringPiSPI.o -lpthread -lasound -lm

mts: mts.o mtp.o
	gcc -o mts mts.o mtp.o -lpigpio -lasound -lm

%.o: %.c
	gcc -c $<

install:
	chown root:audio mts
	chmod a+s mts
	mv mts /usr/local/bin
	mkdir -p /usr/local/lib/mts
	cp $(VOICE) /usr/local/lib/mts

install_ultra:
        mv umts /usr/local/bin/mts
        mkdir -p /usr/local/lib/mts
        cp $(VOICE) /usr/local/lib/mts


