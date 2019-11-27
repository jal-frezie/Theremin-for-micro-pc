// minimal theremin system
// copyright simulistics ltd
// install libasound2-dev and pigpio
// compile: gcc -o mts mts.c -lpigpio -lasound -lm
// set permissions for gpio and audio:
// sudo chown root:audio mts;sudo chmod u+s mts

#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/param.h>
#include <alsa/asoundlib.h>
  // from pcm_min.c

#define PCM_RATE 44100
#define TWEET 311 // no common factor with alsa bite of 441

#define TOUCHED		12000 // IF exceeded if antenna is touched
#define TOUCH_P         1 // flags to set if antennae touched
#define TOUCH_V         2

// Player states
#define STANDBY		0
#define PLAY		1
#define SET_PITCH	2
#define SET_SLOPE_P     3
#define SET_VOL		4
#define SET_TONE	5
#define AUTOTUNE        6
#define TUNING          7

// tones
#define	SINE		0
#define	CLASSIC		1
#define	VALVE		2
#define	TRIANGLE	3
#define	SAWTOOTH	4
#define	SQUARE		5

// autotune modes
#define CONTINUOUS      0
#define CHROMATIC       1
#define MAJOR           2
#define FLOYDIAN        3
#define ARPEGGIO        4
#define AEOLIAN         5

void setupSensing();
void getIFs(int*, int*);
/* include to use sensed values from stdin/stdout 
void setupSensing() {};
void getIFs(int* p, int* v) {
  char q;
  printf("0\n?\n");
  fflush(stdout);
  do scanf("%c %d %d\n", &q, p, v); while (q=='?');
}
*/
FILE* say(char *fileName) {
  FILE* stm;
  int16_t throwaway[22];
  char tgt[256];

  sprintf(tgt, "/usr/local/lib/mts/%s", fileName);
  stm = fopen(tgt, "r");
  if (stm) fread(throwaway, 2, 22, stm); // wav header
  return stm;
}

///////// MAIN ROUTINE HERE //////////
int main(int argc, char* argv[]) {
  struct timespec tv;
  int pitch_if, vol_if, baseLineP, baseLineV;
  double phase = 0, phaseIncr, beingEdited,
    curPitch = 0, pitchAdj, tgtPitch,
    curVol = 0, volAdj, tgtVol;
  // settings are integers
  int vol = 50, pitch = 50, pRange = 50, tuning = 440,
    currentTone = SINE, autotune = CONTINUOUS, wrk,
    *current, *next;
  // pthread_t thread_id;

  int16_t buffer[TWEET];
  int touching = 0, touched = 0, state = PLAY, nextState;
  char *curDesc, *nxtDesc, *nxtSpeak;
  FILE* speech = NULL;

  // from pcm_min.c
  int err;
  unsigned int i, old_ns = 0;
  snd_pcm_sframes_t frames;
  snd_pcm_t *handle;

  setupSensing();
  tv.tv_sec = 0;
  tv.tv_nsec = 1e8;
  nanosleep(&tv, NULL); // let IF detection settle
  getIFs(&baseLineP, &baseLineV);
  fprintf(stderr, "IFs: pitch %d, vol %d\n", baseLineP, baseLineV);
  
  if ((err = snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0)
    {
    fprintf(stderr, "Playback open error: %s\n", snd_strerror(err));
    exit(EXIT_FAILURE);
    }
  if ((err = snd_pcm_set_params(handle,
				SND_PCM_FORMAT_S16_LE,
				SND_PCM_ACCESS_RW_INTERLEAVED,
				1, // channels
				PCM_RATE,
				1,
				50000)) < 0) {   /* 0.05sec */
    fprintf(stderr, "Playback open error: %s\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }
  speech = say("play.wav");
  for (;;) {
    clock_gettime(CLOCK_MONOTONIC_RAW, &tv);
    if ((tv.tv_nsec-old_ns)%1000000000 < 2e6) { // if less than 2ms elapsed...
      tv.tv_sec = 0;
      tv.tv_nsec = (1e9/PCM_RATE)*(TWEET*2/3);
      nanosleep(&tv, NULL); // pause through most of data just sent
      // to make sure next batch is at new frequency
    } else
      old_ns = tv.tv_nsec;

    getIFs(&pitch_if, &vol_if);
    // Adjust offset freq if -ve beat detected! (only if both beats slow)
    if (pitch_if<baseLineP && vol_if-baseLineV < 1000) {
      fprintf(stderr, "Reducing P baseline by %d\n", baseLineP-(int)pitch_if);
      baseLineP = (int)pitch_if;
    }
    if (vol_if<baseLineV && pitch_if-baseLineP < 1000) {
      fprintf(stderr, "Reducing V baseline by %d\n", baseLineV-(int)vol_if);
      baseLineV = (int)vol_if;
    }

    touching = (pitch_if>TOUCHED?TOUCH_P:0) | (vol_if>TOUCHED?TOUCH_V:0);
    if (touching)
      touched |= touching;
    else if (touched) {
      if (state == PLAY) {
	if (touched == (TOUCH_P|TOUCH_V)) { // both, not either
	  state = STANDBY;
	  speech = say("standby.wav");
	} else if (touched == TOUCH_P)
	  fprintf(stderr, "Pitch touched, current vol_if %d base %d\n",
		  vol_if, baseLineV);
	else if (touched == TOUCH_V)
	  fprintf(stderr, "Vol touched, current pitch_if %d base %d\n",
		  pitch_if, baseLineP);
      } else if (state == STANDBY) {
	if (touched == (TOUCH_P)) {
	  state = SET_PITCH;
	  curDesc = "pitch range";
	  speech = say("prange.wav");
	  beingEdited = pitch;
	  current = &pitch;
	}
	if (touched == (TOUCH_V)) {
	  state = SET_TONE;
	  curDesc = "tone";
	  speech = say("tone.wav");
	  beingEdited = currentTone;
	  current = &currentTone;
	}
	if (touched == (TOUCH_P|TOUCH_V)) {
	  state = PLAY;
	  baseLineP += 500;
	  baseLineV += 500;
	  speech = say("play.wav");
	} else
	  fprintf(stderr, "Setting %s\n", curDesc);
      } else {
	switch (state) {
	case SET_TONE:
	  next = &vol;
	  nextState = SET_VOL;
	  nxtDesc = "volume range";
	  nxtSpeak = "vrange.wav";
	  break;
	case SET_PITCH:
	  next = &pRange;
	  nextState = SET_SLOPE_P;
	  nxtDesc = "pitch slope";
	  nxtSpeak = "pslope.wav";
	  break;
	case SET_SLOPE_P:
	  next = &autotune;
	  nextState = AUTOTUNE;
	  nxtDesc = "autotune";
	  nxtSpeak = "autotune.wav";
	  break;
	case AUTOTUNE:
	  next = &tuning;
	  nextState = TUNING;
	  nxtDesc = "tuning";
	  nxtSpeak = "tuning.wav";
	  break;
	default: // last state in series
	  next = NULL;
	}
	
	if (touched == (TOUCH_P))
	  fprintf(stderr, "Set %s to %d\n", curDesc, *current);
	else
	  *current = beingEdited;
	if (touched == (TOUCH_P|TOUCH_V) || next == NULL) {
	  state = PLAY;
	  speech = say("play.wav");
	} else {
	  state = nextState;
	  current = next;
	  curDesc = nxtDesc;
	  beingEdited = *current;
	  fprintf(stderr, "Setting %s\n", curDesc);
	  speech = say(nxtSpeak);
	}
      }
      touched = 0;
    }

    switch (state) {
    case STANDBY:
      tgtVol = 0;
      break;
    case PLAY:
      tgtVol = exp(-(vol_if-baseLineV)/250.0)*vol/100.0;
      break;
    case SET_VOL:
      tgtVol = exp(-(vol_if-baseLineV)/250.0); // ignore setting while setting
      break;
    default: // using vol antenna to set attr so fix volume
      tgtVol = 0.3*vol/100.0;      
    }
    
    switch (state) {
    case SET_VOL:
      vol = (int)(100*tgtVol);
      break;
    case SET_TONE:
      // currentTone = (int)(log(vol_if-baseLineV)*2) - 8;
      currentTone = (vol_if-baseLineV)/200;
      break;
    case SET_PITCH:
      pitch = 10 + (vol_if-baseLineV)/20;
      break;
    case SET_SLOPE_P:
      pRange = 10 + (vol_if-baseLineV)/20;
      break;
    case AUTOTUNE:
      autotune = (vol_if-baseLineV)/200;
      if (autotune>AEOLIAN) autotune = AEOLIAN;
      break;
    case TUNING:
      tuning = 300 + (vol_if-baseLineV)/5;
    }

    tgtPitch = pitch*4096*pow((pitch_if-baseLineP)*1.0/tuning,pRange/50.0)/50;
    switch (autotune) {
    case CHROMATIC:
      tgtPitch = exp(log(2)*round(12*log(tgtPitch)/log(2))/12);
      break;
    case MAJOR:
      wrk = round(7*log(tgtPitch)/log(2));
      tgtPitch = exp(log(2)*((12*wrk+2)/7)/12.0);
      break;
    case FLOYDIAN:
      tgtPitch = exp(log(2)*round(4*log(tgtPitch)/log(2))/4);
      break;
    case ARPEGGIO:
      wrk = round(3*log(tgtPitch)/log(2));
      tgtPitch = exp(log(2)*(wrk/3))*(4+wrk%3)/4;
      break;
    case AEOLIAN:
      tgtPitch = 2048*round(tgtPitch/2048);
      break;
    }
    tgtPitch = tuning*tgtPitch/4096;
    
    if (speech) {
      i = fread(buffer, 2, TWEET, speech);
      if (i<TWEET) {
	fclose(speech);
	speech = NULL;
      }
    } else
      i=0;

    if (autotune == CONTINUOUS) // change pitch smoothly through buffer
      pitchAdj = (tgtPitch - curPitch)/TWEET;
    else { // change pitch abruptly
      curPitch = tgtPitch;
      pitchAdj = 0;
    }
    volAdj = (tgtVol - curVol)/TWEET;
    // adjust volume gradually over batch to avoid crackle --
    // if buffer part full of speech, jump to that point
    curPitch += i*pitchAdj;
    curVol += i*volAdj;
    
    for (; i<TWEET; ++i) {
      curPitch += pitchAdj;
      phaseIncr = curPitch/PCM_RATE;
      phase = fmod(phase+phaseIncr,1.0);
      
      curVol += volAdj;
      switch ((int)currentTone) {
      case SINE:      // sine wave (-cos = same phase as next two)
	buffer[i] = 32768*curVol*-cos(2*3.14159*phase);
      	break;
      case CLASSIC:      // classic sound
	buffer[i] = 32768*curVol*(pow(4*phase*(1-phase),2)-1);
      	break;
      case VALVE:      // valve sound
	buffer[i] = 32768*curVol*(pow(4*phase*(1-phase),16)-1);
      	break;
      case TRIANGLE:       // triangle wave
        buffer[i] = 32768*curVol*(phase>0.5?3-4*phase:4*phase-1);
      	break;
      case SAWTOOTH:       // sawtooth wave
	buffer[i] = 32768*curVol*(2*phase-1);
      	break;
      default:       // square wave
	buffer[i] = 32768*curVol*(phase>0.5?1:-1);
      }
    }

    frames = snd_pcm_writei(handle, buffer, TWEET);
    if (frames < 0)
      frames = snd_pcm_recover(handle, frames, 0);
    if (frames < 0) {
      fprintf(stderr, "snd_pcm_writei failed: %s\n", snd_strerror(frames));
      break;
    }
    if (frames > 0 && frames < TWEET)
      fprintf(stderr, "Short write (expected %li, wrote %li)\n", TWEET, frames);
  }
  return 0;
}
