#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <byteswap.h>
#include <sys/param.h>

#include <unistd.h>
#include <wiringPiSPI.h>

#define SPI_BUF 1024
#define FASTCLK 200000000
#define TIMECONST 0.04

// for custom hardware
#define UNCERTAINTY 	50000 // of osc freqs
#define IF_MIN 		3000 // freq offset of reference oscs at idle
#define IF_MAX 		25000 // biggest offset it can cope with
#define SUBSAMPLE 	1 // set higher to check osc cycles periodically

volatile int rateP, rateV;
volatile double pitch_if = 3000, vol_if = 3000;
volatile struct timespec pitch_ts, vol_ts;

// avoid need to link all of wiringPi
int wiringPiFailure(int msg, char* full, char* rest) {
  fprintf(stderr, "Error %d us %s them %s\n", msg, full, rest);
  exit(msg);
}

// stuff for clean shutdown, needed by gpio
void sig_handler(int signo)
{
  if (signo == SIGHUP || signo == SIGINT ||
      signo == SIGCONT || signo == SIGTERM) {
    fprintf(stderr, "received a %d, shutting down\n", signo);
    fprintf(stderr, "\n");

    exit(signo);
  }
}

// return posn of 1st bit in buffer >= toChk not current
int find_transition(int side, char bufr[], int toChk, int current) {
  int wrd, slot, bit;
  int masks[3] = {0x000000ff, 0xffff0000, 0x00ffffff};

  while (toChk < 8*SPI_BUF) {
    wrd = toChk/32;

    slot = __bswap_32(((int*)bufr)[wrd]) ^ -current;
    if (side) {
      slot ^= masks[wrd%3]; // when using spi1, phase reverses every 24 bits
    }
    if (slot) { // not whole word of current value
      bit = toChk%32;
      bit = __builtin_clz(slot<<bit);
      if (bit<32) return toChk+bit;
    }
    toChk = 32*(wrd+1);
  }
  return 0; // transition not found in remainder of buffer
}

void* readOscs (void* dump) {
  int side, chnl, res, rate, current, toChk, cycles, period;
  struct timespec *tv;
  volatile double *freq;
  unsigned char bufr[SPI_BUF];

  side = (int)dump; // it fits -- wear it
  for (;;) {
    if (side) {
      rate = rateV;
      freq = &vol_if;
      tv = &vol_ts;
    } else {
      rate = rateP;
      freq = &pitch_if;
      tv = &pitch_ts;
    }

// get data from my wiringpi -- side now selects module not chip
   chnl = wiringPiSPISetup(side, FASTCLK/rate);
   res = wiringPiSPIDataRW(side, bufr, SPI_BUF);
   close(chnl);
   clock_gettime(CLOCK_MONOTONIC_RAW, tv);

    if (res < SPI_BUF) {
      printf("Only got %d bytes!\n", res);
      continue;
    }
    toChk = 0;
    int last[2] = {0, 0};
    current = bufr[0] >> 7; // first bit in buffer

    while (toChk = find_transition(side, bufr, toChk+1, current)) {
      current = !current;
      if (side) {
	cycles = 0.5+26.5*(toChk/24)-1.25*(!toChk%24); 
//		adjust for 2.5-bit gaps 
      } else {
	cycles = toChk;
      }
      if (last[current]) { // full cycle read -- update freq estimate
        period = rate*(cycles - last[current]);
        *freq = (1 - period/(TIMECONST*FASTCLK))*(*freq) + 1/TIMECONST;
      }
      last[current] = cycles;

      toChk += FASTCLK*0.05/(*freq)/rate; // jump over jitter
    }
    // TODO if no last, reduce freq to show it out of range!

  }
}

void getIFs(int *p, int *v) {
  *p = (int)pitch_if;
  *v = (int)vol_if;
}

void getTSs(int *p, int *v) {
  *p = pitch_ts.tv_nsec;
  *v = vol_ts.tv_nsec;
}

void calibrate(int guess0, int guess1) {  // try to find osc freq
  struct timespec tv;
  double freq[2], low[2] = {0,0}, high[2] = {0,0};
  int base[2] = {0,0}, top[2] = {0,0}, i, b, s;
  tv.tv_sec = 0;
  tv.tv_nsec = 0.2e9;

  printf("P clock beat    loAlias hiAlias V clock beat    loAlias hiAlias\n");
  for (i=-UNCERTAINTY; i<=UNCERTAINTY;i += IF_MAX-IF_MIN) { 
    // range over which to search --
    // increment equal to useful range so one reading will be within
    rateP = FASTCLK/(guess0+i);
    rateV = FASTCLK/(guess1+i);
    pitch_if = vol_if = 50000;
    nanosleep(&tv, NULL); // delay 0.2 sec to home to frequency

    freq[0] = pitch_if;
    freq[1] = vol_if;
    for (s=0;s<2;++s) { // calibrate both oscs at once
      b = FASTCLK/(s?rateV:rateP); // actual rather than chosen clock
      printf("%6d  %6.0lf  %6.0lf  %.0lf  ", b, freq[s], b-freq[s], b+freq[s]);
      if (freq[s] >= IF_MIN && freq[s] < IF_MAX) { // a valid reading
	if (low[s] == 0) { // first one, clock is below osc freq
	  low[s] = freq[s];
	  base[s] = b;
	} else if (b+freq[s] > base[s]+low[s]+IF_MIN) { 
	  // clock now above osc freq
	  high[s] = freq[s];
          top[s] = b;
	}
      }
    }
    printf("\n");
  }
  for (s=0;s<2;++s) {
    i = (top[s]*low[s] + base[s]*high[s])/(low[s]+high[s]) + IF_MIN; 
    // set clk to osc freq plus quiescent IF
    b = FASTCLK/i;
    if (s) rateV = b; else rateP = b;
    fprintf(stderr, "Osc %d %d at %d\n", s, b, i);
  }
  //  pitch_if = vol_if = IF_MIN;
}

void setupSensing () {
  pthread_t threadId;

  // Prepare clean shutdown
  if (signal(SIGHUP, sig_handler) == SIG_ERR)
    fprintf(stderr, "\ncan't catch SIGHUP\n");
  if (signal(SIGINT, sig_handler) == SIG_ERR)
    fprintf(stderr, "\ncan't catch SIGINT\n");
  if (signal(SIGCONT, sig_handler) == SIG_ERR)
    fprintf(stderr, "\ncan't catch SIGCONT\n");
  if (signal(SIGTERM, sig_handler) == SIG_ERR)
    fprintf(stderr, "\ncan't catch SIGTERM\n");

  pitch_if = 570000; // guesses around which to search
  vol_if = 520000;
  pthread_create(&threadId, NULL, readOscs, (void*)0);
  pthread_create(&threadId, NULL, readOscs, (void*)1);

  calibrate(pitch_if, vol_if);
}

/* Include this to serve sensed values to stdin/stdout
int main () {
  struct timespec tv;
  char q;
  int p, v;

  setupSensing();

  tv.tv_sec = 0;
  tv.tv_nsec = 1e8;
  nanosleep(&tv, NULL);

  for (;;) {
    scanf("%c\n", &q);
    if (q == '?') continue; // sent as filler
    if (q == '!') sig_handler(SIGCONT); // leave
    getIFs(&p, &v);
    printf("%c v%d p%d f%d\n? 0 0\n", q, v, p, fails);
    fflush(stdout);
  }
}
*/
