// minimal theremin system
// copyright simulistics ltd
// install libasound2-dev and pigpio
// compile: gcc -o mts mts.c -lpigpio -lasound -lm
// set permissions for gpio and audio:
// sudo chown root:audio mts;sudo chmod u+s mts

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <pigpio.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/param.h>

// for custom hardware
#define UNCERTAINTY 	50000 // of osc freqs
#define IF_MIN 		3000 // freq offset of reference oscs at idle
#define IF_MAX 		25000 // biggest offset it can cope with
#define SUBSAMPLE 	1 // set higher to check osc cycles periodically

// gpio pins etc
#define REF_P 4
#define REF_V 6
#define SENS_P 10
#define SENS_V 27
#define PLLD_OLD 500000000

// smoothing
#define TIMECONST 0.04

double pitch_if, vol_if;
struct timespec pitch_ts, vol_ts;

void logTrans(int pin, int edge, unsigned int actTime) {
  static struct timing_t {
    int lastEdge;
    unsigned int lastUp, lastDown;
  } timings[2] = {{0,0,0}, {0,0,0}}, *timing;
  int lastPeriod;
  double *freq;
  struct timespec *tv;

  // load context appropriate to current pin
  if (pin==SENS_P) {
    freq = &pitch_if;
    timing = timings;
    tv = &pitch_ts;
  } else {
    freq = &vol_if;
    timing = timings + 1;
    tv = &vol_ts;
  }
  
  if (timing->lastUp == timing->lastDown) // first go, set up context
    timing->lastUp = timing->lastDown = actTime - 1e6/IF_MAX;
  
  if (edge==timing->lastEdge || // old debounce clock jitter
      actTime - (edge==RISING_EDGE?timing->lastDown:timing->lastUp) < 0.1e6/(*freq))
    return;

  clock_gettime(CLOCK_MONOTONIC_RAW, tv);

  timing->lastEdge = edge;
  if (edge == RISING_EDGE) {
    lastPeriod = actTime - timing->lastUp;
    timing->lastUp = actTime;
  } else {
    lastPeriod = actTime - timing->lastDown;
    timing->lastDown = actTime;
  }

  // adjust frequency estimate
  if (lastPeriod > 1e6*TIMECONST)
    *freq = 1e6/lastPeriod;
  else
    *freq = (1-(1e-6*lastPeriod/TIMECONST))*(*freq) + 1/TIMECONST;
// debounce clock jitter v2
//  gpioGlitchFilter(pin, (int)(0.05e6/(IF_MIN>*freq?IF_MIN:*freq)));
}

int setFreq(int pin, int freq) {
  if (pin==13 || pin==18) { // using PWM, we are on a Model A/B
    gpioHardwarePWM(pin, freq, 500000);
    if (!freq) return 0;
    return PLLD_OLD/(2.0*(int)(PLLD_OLD/(2.0*freq)+0.5))+0.5;
  } else {
    gpioHardwareClock(pin, freq);
    return freq;
  }
}

void calibrate(int pin, int guess, double* freq) {  // try to find osc freq
  struct timespec tv;
  double low = 0, high = 0;
  int base = 0, top = 0, i, b;
  tv.tv_sec = 0;
  tv.tv_nsec = (int)(4e9*TIMECONST);
  
  fprintf(stderr, "Calibrating pin %d to %d\n", pin, guess);
  for (i=guess-UNCERTAINTY; i<=guess+UNCERTAINTY;i += IF_MAX-IF_MIN) { 
    // range over which to search --
    // increment equal to useful range so one reading will be within
    *freq = UNCERTAINTY;
    b = setFreq(pin, i/SUBSAMPLE);
    nanosleep(&tv, NULL); // delay 0.1 sec to home to frequency

    fprintf(stderr, "Hit %lf with %d at %d\n", *freq, b, i);
    if (*freq >= IF_MIN && *freq < IF_MAX) { // a valid reading
      if (low == 0) { // first one, clock is below osc freq
	low = *freq;
	base = b;
      } else if (b+*freq > base+low+IF_MIN) { // clock now above osc freq
	high = *freq;
        top = b;
	break;
      }
    }
  }
  i = (top*low + base*high)/(low+high) + IF_MIN; // osc freq plus quiescent IF
  // i = 750000000/((1500000000/i)+1)/2; // go to exact factor of PLLD
  b = setFreq(pin, i/SUBSAMPLE);
  
  *freq = IF_MIN;
  fprintf(stderr, "Osc %d at %d\n", b, i);
}

// stuff for clean shutdown, needed by gpio
void sig_handler(int signo)
{
  if (signo == SIGHUP || signo == SIGINT ||
      signo == SIGCONT || signo == SIGTERM) {
    fprintf(stderr, "received a %d, shutting down\n", signo);
    fprintf(stderr, "\n");

    setFreq(REF_P, 0);
    setFreq(REF_V, 0);
    gpioTerminate();
    exit(signo);
  }
}

void setupSensing() {
  gpioInitialise();
  
  // Prepare clean shutdown
  if (signal(SIGHUP, sig_handler) == SIG_ERR)
    fprintf(stderr, "\ncan't catch SIGHUP\n");
  if (signal(SIGINT, sig_handler) == SIG_ERR)
    fprintf(stderr, "\ncan't catch SIGINT\n");
  if (signal(SIGCONT, sig_handler) == SIG_ERR)
    fprintf(stderr, "\ncan't catch SIGCONT\n");
  if (signal(SIGTERM, sig_handler) == SIG_ERR)
    fprintf(stderr, "\ncan't catch SIGTERM\n");

  gpioSetMode(SENS_P, PI_INPUT);
  gpioSetMode(SENS_V, PI_INPUT);

  gpioSetAlertFunc(SENS_P, logTrans);
  calibrate(REF_P, 550000, &pitch_if);

  gpioSetAlertFunc(SENS_V, logTrans);
  calibrate(REF_V, 500000, &vol_if);
}

void getIFs(int *p, int *v) {
  *p = pitch_if;
  *v = vol_if;
}

void getTSs(int *p, int *v) {
  *p = pitch_ts.tv_nsec;
  *v = vol_ts.tv_nsec;
}
/* Include this to serve sensed values to stdin/stdout 
int main () {
  setupSensing();
  char q;
  int p, v;
    
  for (;;) {
    scanf("%c\n", &q);
    if (q == '?') continue; // sent as filler
    if (q == '!') sig_handler(SIGCONT); // leave
    getIFs(&p, &v);
    printf("%c %d %d\n? 0 0\n", q, p, v);
    fflush(stdout);
  }
}
*/
