// minimal theremin system
// copyright simulistics ltd
// install libasound2-dev and pigpio
// compile: gcc -o mts mts.c -lpigpio -lasound -lm
// set permissions for gpio and audio:
// sudo chown root:audio mts;sudo chmod u+s mts

#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <pthread.h>
#include <pigpio.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/param.h>
  // from pcm_min.c
#include <alsa/asoundlib.h>
static char *device = "default";            /* playback device */
snd_output_t *output = NULL;
#define PCM_RATE 44100
#define TWEET 311 // no common factor with alsa bite of 441
#define IF_MIN 2000 // freq offset of reference oscs at idle
#define IF_MAX 20000 // biggest offset it can cope with
#define SUBSAMPLE 1 // set higher to check osc cycles periodically

// gpio pins
#define REF_P 4
#define REF_V 6
#define SENS_P 10
#define SENS_V 27

double pitch_if = IF_MIN, vol_if = IF_MIN;
snd_pcm_t *handle;

void logTrans(int pin, int edge, unsigned int actTime) {
  static struct timing_t {
    int lastEdge;
    unsigned int lastUp, lastDown;
  } timings[2] = {{0,0,0}, {0,0,0}}, *timing;
  int lastPeriod;
  double *freq;
  static double timeConst = 0.02;

  // load context appropriate to current pin
  if (pin==SENS_P) {
    freq = &pitch_if;
    timing = timings;
  } else {
    freq = &vol_if;
    timing = timings + 1;
  }
  
  if (timing->lastUp == timing->lastDown) // first go, set up context
    timing->lastUp = timing->lastDown = actTime - 1e6/IF_MIN;
  
  if (edge==timing->lastEdge || // debounce clock jitter
      actTime - (edge==RISING_EDGE?timing->lastDown:timing->lastUp) < 0.05e6/(*freq)) return;
  
  timing->lastEdge = edge;
  if (edge == RISING_EDGE) {
    lastPeriod = actTime - timing->lastUp;
    timing->lastUp = actTime;
  } else {
    lastPeriod = actTime - timing->lastDown;
    timing->lastDown = actTime;
  }

  // adjust frequency estimate
  if (lastPeriod > 1e6*timeConst)
    *freq = 1e6/lastPeriod;
  else
    *freq = (1-(1e-6*lastPeriod/timeConst))*(*freq) + 1/timeConst;
}

double calibrate(int pin, int guess, double* freq) {  // try to find osc freq
  struct timespec tv;
  double low = 0, high = 0;
  int base = 0, top = 0, i, b;
  tv.tv_sec = 0;
  tv.tv_nsec = 100000000;
  
  b = guess;
  printf("Calibrating pin %d to %d\n", pin, guess);
  *freq = IF_MAX;
  for (i=b-50000; i<=b+50000;i += IF_MAX-IF_MIN) { // range to search --
    // increment equal to useful range so one reading will be within
    gpioHardwareClock(pin, i/SUBSAMPLE);
    nanosleep(&tv, NULL); // delay 0.1 sec to home to frequency

    if (*freq >= IF_MIN && *freq < IF_MAX) { // a valid reading
      printf("Hit %lf at %d\n", *freq, i);
      if (low == 0) { // first one, clock is below osc freq
	low = *freq;
	base = i;
      } else if (i+*freq > base+low+IF_MIN) { // clock now above osc freq
	high = *freq;
        top = i;
	break;
      }
    }
  }
  i = (top*low + base*high)/(low+high) + IF_MIN; // osc freq plus quiescent IF
  
  gpioHardwareClock(pin, i/SUBSAMPLE);
  
  *freq = IF_MIN;
  nanosleep(&tv, NULL); // delay 0.1 sec to home to quiescent
  low = *freq; // calibrate to actual offset
  printf("Osc %d, base %lf\n", i, low);
  return low;
}

// stuff for clean shutdown, needed by gpio
void sig_handler(int signo)
{
  if (signo == SIGHUP || signo == SIGINT ||
      signo == SIGCONT || signo == SIGTERM) {
    printf("received a %d, shutting down\n", signo);
    printf("\n");
    snd_pcm_close(handle);

    gpioHardwareClock(REF_P, 0);
    gpioHardwareClock(REF_V, 0);
    gpioTerminate();
    exit(signo);
  }
}

int main(int argc, char* argv[]) {
  double baseLineP, baseLineV, phase = 0, phaseIncr,
    curVol = 0, volAdj, vol = 0.5;
  // pthread_t thread_id;
  int b, t, off;
  int16_t buffer[TWEET];

  // from pcm_min.c
  int err;
  unsigned int i, old_ns = 0;
  snd_pcm_sframes_t frames;

  gpioInitialise();
  
  // Prepare clean shutdown
  if (signal(SIGHUP, sig_handler) == SIG_ERR)
    printf("\ncan't catch SIGHUP\n");
  if (signal(SIGINT, sig_handler) == SIG_ERR)
    printf("\ncan't catch SIGINT\n");
  if (signal(SIGCONT, sig_handler) == SIG_ERR)
    printf("\ncan't catch SIGCONT\n");
  if (signal(SIGTERM, sig_handler) == SIG_ERR)
    printf("\ncan't catch SIGTERM\n");

  gpioSetMode(SENS_P, PI_INPUT);
  gpioSetMode(SENS_V, PI_INPUT);

  gpioSetAlertFunc(SENS_P, logTrans);
  gpioSetAlertFunc(SENS_V, logTrans);

  baseLineP = calibrate(REF_P, 550000, &pitch_if);
  baseLineV = calibrate(REF_V, 500000, &vol_if);
  
  if ((err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
    printf("Playback open error: %s\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }
  if ((err = snd_pcm_set_params(handle,
				SND_PCM_FORMAT_S16_LE,
				SND_PCM_ACCESS_RW_INTERLEAVED,
				1, // channels
				PCM_RATE,
				1,
				50000)) < 0) {   /* 0.05sec */
    printf("Playback open error: %s\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  for (;;) {
    struct timespec tv;
    clock_gettime(CLOCK_MONOTONIC_RAW, &tv);
    if ((tv.tv_nsec-old_ns)%1000000000 < 1e6) { // if less than 1ms elapsed...
      tv.tv_sec = 0;
      tv.tv_nsec = (1e9/PCM_RATE)*(TWEET*2/3);
      nanosleep(&tv, NULL); // pause through most of data just sent
      // to make sure next batch is at new frequency
    } else
      old_ns = tv.tv_nsec;

    // Adjust offset freq if -ve beat detected!
    if (pitch_if<baseLineP) baseLineP = pitch_if;
    if (vol_if<baseLineV) baseLineV = vol_if;
    
    phaseIncr = (1*(pitch_if-baseLineP))/PCM_RATE;
    volAdj = (exp(-(vol_if-baseLineV)/250)*vol - curVol)/TWEET;
    // adjust volume gradually over batch to avoid crackle
    
    for (i=0; i<TWEET; ++i) {
      phase += phaseIncr;
      curVol += volAdj;
      // sine wave
      buffer[i] = 32768*curVol*sin(2*3.14159*phase);
    }

    frames = snd_pcm_writei(handle, buffer, TWEET);
    if (frames < 0)
      frames = snd_pcm_recover(handle, frames, 0);
    if (frames < 0) {
      printf("snd_pcm_writei failed: %s\n", snd_strerror(frames));
      break;
    }
    if (frames > 0 && frames < TWEET)
      printf("Short write (expected %li, wrote %li)\n", TWEET, frames);
  }
}
