// CamillaDSP ALSA "IO" Plugin (Maybe O plugin as it is output only)
//
// Based on bluealsa-pcm.c from the project bluez-alsa
// which is Copyright (c) 2016-2020 Arkadiusz Bokowy
//
// This project is licensed under the terms of the MIT license.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <limits.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "strrep.h"

#define DEBUG 1
#define error(fmt, ...) \
  do { if(DEBUG > 0){fprintf(stderr,"CDSP Plugin ERROR: ");\
    fprintf(stderr,((fmt)), ##__VA_ARGS__);} } while (0)
#define warn(fmt, ...) \
  do { if(DEBUG > 1){fprintf(stderr,"CDSP Plugin WARN: ");\
    fprintf(stderr,((fmt)), ##__VA_ARGS__);} } while (0)
#define info(fmt, ...) \
  do { if(DEBUG > 2){fprintf(stderr,"CDSP Plugin INFO: ");\
    fprintf(stderr,((fmt)), ##__VA_ARGS__);} } while (0)
#define debug(fmt, ...) \
  do { if(DEBUG > 3){fprintf(stderr,"CDSP Plugin DEBUG: ");\
    fprintf(stderr,((fmt)), ##__VA_ARGS__);} } while (0)
#define excessive(fmt, ...) \
  do { if(DEBUG > 4){fprintf(stderr,((fmt)), ##__VA_ARGS__);} } while (0)

#define CDSP_PAUSE_STATE_RUNNING 0
#define CDSP_PAUSE_STATE_PAUSED  (1 << 0)
#define CDSP_PAUSE_STATE_PENDING (1 << 1)

// Cleanup callback casting wrapper for the brevity's sake.
#define PTHREAD_CLEANUP(f) ((void (*)(void *))(void (*)(void))(f))

// Thread routing callback casting wrapper.
#define PTHREAD_ROUTINE(f) ((void *(*)(void *))(f))

//
// Get system monotonic time-stamp.
//
// Why try for RAW?  We're trying to simulate an accurate clock.  Let
// ntp correct the rate.
//
// @param ts Address to the timespec structure where the time-stamp will
// be stored.
// @return On success this function returns 0. Otherwise, -1 is returned
// and errno is set to indicate the error.
//#ifdef CLOCK_MONOTONIC_RAW
//# define gettimestamp(ts) clock_gettime(CLOCK_MONOTONIC_RAW, ts)
//#else
# define gettimestamp(ts) clock_gettime(CLOCK_MONOTONIC, ts)
//#endif

// Calculate time difference for two time points.
// 
//  @param ts1 Address to the timespec structure providing t1 time point.
//  @param ts2 Address to the timespec structure providing t2 time point.
//  @param ts Address to the timespec structure where the absolute time
//    difference will be stored.
// @return This function returns an integer less than, equal to, or greater
//    than zero, if t2 time point is found to be, respectively, less than,
//    equal to, or greater than the t1 time point.*/
int difftimespec( const struct timespec *ts1, const struct timespec *ts2,
    struct timespec *ts) {
  const struct timespec _ts1 = *ts1;
  const struct timespec _ts2 = *ts2;

  if (_ts1.tv_sec == _ts2.tv_sec) {
    ts->tv_sec = 0;
    ts->tv_nsec = labs(_ts2.tv_nsec - _ts1.tv_nsec);
    return _ts2.tv_nsec > _ts1.tv_nsec ? 1 : -ts->tv_nsec;
  }

  if (_ts1.tv_sec < _ts2.tv_sec) {
    if (_ts1.tv_nsec <= _ts2.tv_nsec) {
      ts->tv_sec = _ts2.tv_sec - _ts1.tv_sec;
      ts->tv_nsec = _ts2.tv_nsec - _ts1.tv_nsec;
    }
    else {
      ts->tv_sec = _ts2.tv_sec - 1 - _ts1.tv_sec;
      ts->tv_nsec = _ts2.tv_nsec + 1000000000 - _ts1.tv_nsec;
    }
    return 1;
  }

  if (_ts1.tv_nsec >= _ts2.tv_nsec) {
    ts->tv_sec = _ts1.tv_sec - _ts2.tv_sec;
    ts->tv_nsec = _ts1.tv_nsec - _ts2.tv_nsec;
  }
  else {
    ts->tv_sec = _ts1.tv_sec - 1 - _ts2.tv_sec;
    ts->tv_nsec = _ts1.tv_nsec + 1000000000 - _ts2.tv_nsec;
  }
  return -1;
}

typedef struct {
  snd_pcm_ioplug_t io;

  // IO thread and application thread sync
  pthread_mutex_t mutex;

  // Pipe to talk to CamillaDSP
  int cdsp_pcm_fd;

  // event file descriptor
  int event_fd;

  // virtual hardware - ring buffer
  char *io_hw_buffer;

  // The IO thread is responsible for maintaining the hardware pointer
  // (pcm->io_hw_ptr), the application is responsible for the application
  // pointer (io->appl_ptr). These are both volatile as they are both
  // written in one thread and read in the other.
  volatile snd_pcm_uframes_t io_hw_ptr;
  // A signed value for the status return to the IO plugin pointer call
  volatile int io_status;
  snd_pcm_uframes_t io_hw_boundary;
  // Permit the application to modify the frequency of poll() events.
  volatile snd_pcm_uframes_t io_avail_min;
  pthread_t io_thread;
  bool io_started;

  // ALSA operates on frames, we on bytes
  size_t frame_size;

  struct timespec delay_ts;
  snd_pcm_uframes_t delay_hw_ptr;
  unsigned int delay_pcm_nread;
  // In the capture mode, delay_running indicates that frames are being
  // transfered to the FIFO by the server. In playback mode it indicates
  // that the IO thread is transferring frames to the FIFO.
  bool delay_running;

  // delay accumulated just before pausing
  snd_pcm_sframes_t delay_paused;
  // maximum delay in FIFO
  snd_pcm_sframes_t delay_fifo_size;

  // synchronize threads to begin/end pause
  pthread_cond_t pause_cond;
  unsigned int pause_state;

  // Process id of forked CamillaDSP
  pid_t cpid;
  // Path to CamillaDSP executable
  char *cpath;
  // Location of CamillaDSP input YAML template
  char *config_in;
  // Alternatively to providing config_in provide a program
  // that will generate config_out
  char *config_cmd;
  // And yet another alternative use CamillaDSP's new internal
  // subsitution
  long config_cdsp;
  // Gain and mute startup file
  char *vol_file;
  // Arguments to execv
  // cargs[0] = "camilladsp" => Process name
  // cargs[1] = config_out => Location of CamillaDSP output YAML configuration
  // cargs[2+] = Additional arguments passed through .asoundrc
  // If config_cdsp cargs will also be used to hold hw_params
  // If vol_file is set cargs will also be used to hold mute and gain
  // Make the array a bit bigger to allow them
  size_t n_cargs;
  char *cargs[120];
  // Search / Replace string tokens - let people use whatever format
  // they want.
  char *format_token;
  char *rate_token;
  char *channels_token;
  char *ext_samp_token;
  // Extra samples parameter to pass to CamillaDSP if the config_in template
  // is used instead of config_cmd
  // ext_samp_44100 and ext_samp_4800 allow rate matched expansion of the
  // extra samples.  They will be multiplied by {rate}/44100 or rate/{48000}
  // if {rate} is an integer multiple of 44100 or 48000 respectively
  long ext_samp;
  long ext_samp_44100;
  long ext_samp_48000;

  // Suppress a spurious warning on the first call to revents for the
  // event triggered during prepare.  Some programs need that event to
  // start so it's not actually an overcall.
  bool first_revent;
  // A command to run at init - turn on audio system for example
  char *start_cmd;
  // A command to run just before the CamillaDSP process is ended
  // A good time to update the gain and mute values for example
  char *camilla_exit_cmd;
} cdsp_t;

#if SND_LIB_VERSION < 0x010106
//
// Get the available frames.
//
// This function is available in alsa-lib since version 1.1.6. For older
// alsa-lib versions we need to provide our own implementation.
static snd_pcm_uframes_t snd_pcm_ioplug_hw_avail(
    const snd_pcm_ioplug_t * const io, const snd_pcm_uframes_t hw_ptr, 
    const snd_pcm_uframes_t appl_ptr) {
  cdsp_t *pcm = io->private_data;
  snd_pcm_sframes_t diff;
  if (io->stream == SND_PCM_STREAM_PLAYBACK)
    diff = appl_ptr - hw_ptr;
  else
    diff = io->buffer_size - hw_ptr + appl_ptr;
  if (diff < 0)
    diff += pcm->io_hw_boundary;
  return diff <= io->buffer_size ? (snd_pcm_uframes_t) diff : 0;
}
#endif

// Helper function for closing PCM transport.
static int close_transport(cdsp_t *pcm) {
  int rv = 0;
  pthread_mutex_lock(&pcm->mutex);
  if (pcm->cdsp_pcm_fd != -1) {
    rv |= close(pcm->cdsp_pcm_fd);
    pcm->cdsp_pcm_fd = -1;
  }
  pthread_mutex_unlock(&pcm->mutex);
  return rv;
}

// Helper function for IO thread termination.
static void io_thread_cleanup(cdsp_t *pcm) {
  debug("IO thread cleanup\n");
  pcm->io_started = false;
}

// Helper function for IO thread delay calculation.
static void io_thread_update_delay(cdsp_t *pcm, snd_pcm_uframes_t hw_ptr) {
  struct timespec now;
  unsigned int nread = 0;

  gettimestamp(&now);
  // Get the number of bytes still in the pipe to cdsp
  ioctl(pcm->cdsp_pcm_fd, FIONREAD, &nread);

  pthread_mutex_lock(&pcm->mutex);

  // stash current time and levels
  pcm->delay_ts = now;
  pcm->delay_pcm_nread = nread;
  if (pcm->io_status < 0) {
    pcm->delay_hw_ptr = 0;
    if (pcm->io.stream == SND_PCM_STREAM_PLAYBACK)
      pcm->delay_running = false;
  }
  else {
    pcm->delay_hw_ptr = hw_ptr;
    if (pcm->io.stream == SND_PCM_STREAM_PLAYBACK)
      pcm->delay_running = true;
  }

  pthread_mutex_unlock(&pcm->mutex);

}

// IO thread, which facilitates ring buffer.
static void *io_thread(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  pthread_cleanup_push(PTHREAD_CLEANUP(io_thread_cleanup), pcm);
  int xrun = 0;

  sigset_t sigset;
  sigemptyset(&sigset);

  // Block signal, which will be used for pause/resume actions.
  sigaddset(&sigset, SIGIO);
  // Block SIGPIPE, so we could receive EPIPE while writing to the pipe
  // whose reading end has been closed. This will allow clean playback
  // termination.
  sigaddset(&sigset, SIGPIPE);

  if ((errno = pthread_sigmask(SIG_BLOCK, &sigset, NULL)) != 0) {
    SNDERR("Thread signal mask error: %s", strerror(errno));
    goto fail;
  }

  // We update pcm->io_hw_ptr (i.e. the value seen by ioplug) only when
  // a period has been completed. We use a temporary copy during the
  // transfer procedure.
  snd_pcm_uframes_t io_hw_ptr = pcm->io_hw_ptr;

  debug("Starting IO loop: %d\n", pcm->cdsp_pcm_fd);
  for (;;) {
    if (pcm->pause_state & CDSP_PAUSE_STATE_PENDING ||
        pcm->io_status < 0) {
      debug("Pausing IO thread\n");

      pthread_mutex_lock(&pcm->mutex);
      pcm->pause_state = CDSP_PAUSE_STATE_PAUSED;
      pthread_cond_signal(&pcm->pause_cond);
      pthread_mutex_unlock(&pcm->mutex);

      int tmp;
      sigwait(&sigset, &tmp);

      pthread_mutex_lock(&pcm->mutex);
      pcm->pause_state = CDSP_PAUSE_STATE_RUNNING;
      pthread_mutex_unlock(&pcm->mutex);

      debug("IO thread resumed\n");

      if (pcm->io_status < 0)
        continue;
      if (pcm->cdsp_pcm_fd == -1) {
        error("FAILING BECAUSE PIPE GONE\n");
        goto fail;
      }
    }

    // There are 2 reasons why the number of available frames may be
    // zero: XRUN or drained final samples; we set the io_status to
    // -1 to indicate we have no work to do.
    snd_pcm_uframes_t avail;
    if ((avail = snd_pcm_ioplug_hw_avail(io, io_hw_ptr, io->appl_ptr)) == 0) {
      if(io->state == SND_PCM_STATE_DRAINING) {
        // Draining is complete.  Signal that to the ioplug code so it will
        // drop the pcm.
        pcm->io_status = -1;
        io_thread_update_delay(pcm, 0);
        eventfd_write(pcm->event_fd, 1);
        continue;
      } else {
        warn("IO Thread out of data.\n");
        // Running and no data is available.  The internal alsa buffer is
        // empty.  This isn't a problem until a period has passed though
        // at which point we have an underrun condition.
        // Sleep in 1/4 period intervals to wait for data to catch up
        // Add 1 extra sample to the period to allow for clock differences
        // and rounding errors
        uint64_t quarter_period_ns = 
          (1000000000 / 4) * (io->period_size+1) / io->rate;
        struct timespec ts;
        ts.tv_sec = quarter_period_ns / 1000000000;
        ts.tv_nsec = quarter_period_ns - 1000000000 * ts.tv_sec;
        debug("Sleep Time: %ld %ld %lu %d\n", ts.tv_sec, ts.tv_nsec,io->period_size, io->rate);
        nanosleep(&ts, NULL);
        xrun++;
        if(xrun > 4) {
          // We've gone longer than a period with no data.
          // The player isn't providing data fast enough.
          error("XRUN OCCURRED!\n");
          // Signal XRUN to the ioplug code
          pcm->io_status = -1;
          io_thread_update_delay(pcm, 0);
          eventfd_write(pcm->event_fd, 1);
        }
        continue;
      }
    }
    // Data available - reset the xrun counter
    xrun = 0;
    pcm->io_status = 0;

    // current offset of the head pointer in the IO buffer
    snd_pcm_uframes_t offset = io_hw_ptr % io->buffer_size;

    // Transfer at most 1 period of frames each iteration
    snd_pcm_uframes_t frames = io->period_size;
    // ... but do not try to transfer more frames than are available in
    // the ring buffer!
    if (frames > avail) {
      frames = avail;
    }

    // Sometimes alsa chooses a buffer size that isn't an integer multiple
    // of the period size.  In that case don't read past the end of the 
    // buffer.
    if (io->buffer_size - offset < frames) {
      frames = io->buffer_size - offset;
    }

    // IO operation size in bytes
    size_t len = frames * pcm->frame_size;
    char *head = pcm->io_hw_buffer + offset * pcm->frame_size;

    // Increment the HW pointer (with boundary wrap)
    io_hw_ptr += frames;
    if (io_hw_ptr >= pcm->io_hw_boundary)
      io_hw_ptr -= pcm->io_hw_boundary;

    ssize_t ret = 0;

    struct timespec tstart,tstop,twrite;
    gettimestamp(&tstart);

    // Perform atomic write - see the explanation above.
    do {
      if ((ret = write(pcm->cdsp_pcm_fd, head, len)) == -1) {
        if (errno == EINTR)
          continue;
        if (errno != EPIPE)
          SNDERR("PCM FIFO write error: %s", strerror(errno));
        goto fail;
      }
      head += ret;
      len -= ret;
    } while (len != 0);
    io_thread_update_delay(pcm, io_hw_ptr);
    // Things tend to run a little smoother if writes take at least
    // some time.  So slow down when the pipe was empty enough that the
    // write was basically instant.
    gettimestamp(&tstop);
    difftimespec(&tstart, &tstop, &twrite);
    double sampletime = (double)frames/(double)io->rate;
    double writetime = (double)twrite.tv_sec + (double)twrite.tv_nsec/1e9;
    double excess = sampletime - writetime;
    if(excess > 0) {
      excess *= 0.5;
      tstop.tv_sec = (time_t)excess;
      excess -= tstop.tv_sec;
      tstop.tv_nsec = (long)(excess*1e9);
      nanosleep(&tstop, NULL);
    }
    excessive("Frames = %lu = %lf secs, Write Time = %lf\n", frames, sampletime, writetime);

    // Make the new HW pointer value visible to the ioplug.
    pcm->io_hw_ptr = io_hw_ptr;

    // Wake application thread if enough space/frames are available
    // to write avail_min frames.  Note that just as we can't read
    // past the end of the hardware buffer the app can't write past
    // it so the metric is distance from the end of the buffer.
    offset = io_hw_ptr % io->buffer_size;
    if(io->buffer_size - offset >= pcm->io_avail_min)
      eventfd_write(pcm->event_fd, 1);
  }

fail:
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
  pthread_cleanup_pop(1);
  close_transport(pcm);
  eventfd_write(pcm->event_fd, 0xDEAD0000);
  pthread_cond_signal(&pcm->pause_cond);
  return NULL;
}

static int start_camilla(cdsp_t *pcm) {
  char sformat[20];
  switch (pcm->io.format) {
    case SND_PCM_FORMAT_S16_LE:
      snprintf(sformat, sizeof(sformat), "S16LE");
      break;
    case SND_PCM_FORMAT_S24_LE:
      snprintf(sformat, sizeof(sformat), "S24LE");
      break;
    case SND_PCM_FORMAT_S24_3LE:
      snprintf(sformat, sizeof(sformat), "S24LE3");
      break;
    case SND_PCM_FORMAT_S32_LE:
      snprintf(sformat, sizeof(sformat), "S32LE");
      break;
    case SND_PCM_FORMAT_FLOAT_LE:
      snprintf(sformat, sizeof(sformat), "FLOAT32LE");
      break;
    case SND_PCM_FORMAT_FLOAT64_LE:
      snprintf(sformat, sizeof(sformat), "FLOAT64LE");
      break;
    default:
      // Shouldn't get here
      SNDERR("Unsupported Format: %s\n", snd_pcm_format_name(pcm->io.format));
      return -EINVAL;
  }

  char schannels[10]; // The number of channels should fit in 9 digits
  snprintf(schannels, sizeof(schannels), "%u", pcm->io.channels);

  char srate[10]; // The rate should fit in 9 digits too
  snprintf(srate, sizeof(srate), "%u", pcm->io.rate);

  char sextrasamples[20];  // Some use really long audio chains
  long extrasamples = -1;
  // We multiply the ext_samp by the ratio of sample rate to
  // one of the two common audio rates if the sample rate is an
  // integer multiple
  if((pcm->ext_samp_44100 >= 0) && ((pcm->io.rate % 44100) == 0)) {
    extrasamples = pcm->ext_samp_44100*(pcm->io.rate/44100);
  } else if((pcm->ext_samp_48000 >= 0) && ((pcm->io.rate % 48000) == 0)) {
    extrasamples = pcm->ext_samp_48000*(pcm->io.rate/48000);
  } else if(pcm->ext_samp >= 0) {
    extrasamples = pcm->ext_samp;
  }
  if(extrasamples >= 0) {
    snprintf(sextrasamples, sizeof(sextrasamples), "%ld", extrasamples);
  }

  // Create the pipe to send data to camilla
  int fd[2];
  if(pipe(fd)) {
    return -ENODEV;
  }

  // Fork to launch camilla
  pcm->cpid = fork();
  if(pcm->cpid < 0) {
    return -ENODEV;
  }

  if(pcm->cpid == 0) {
    // Child process
    close(fd[1]);
    dup2(fd[0], STDIN_FILENO);
    close(fd[0]);

    debug("cpath: %s\n", pcm->cpath);
    debug("config_in: %s\n", pcm->config_in);
    debug("config_out: %s\n", pcm->cargs[1]);
    debug("config_cmd: %s\n", pcm->config_cmd);
    debug("config_cdsp: %ld\n", pcm->config_cdsp);
    debug("vol_file: %s\n", pcm->vol_file);
    debug("cargs:");
#if DEBUG > 3
    for(size_t ca = 2; ca < pcm->n_cargs; ca++) {
      fprintf(stderr," %s", pcm->cargs[ca]);
    }
    fprintf(stderr,"\n");
#endif
    
    double gain = 0;
    // Use mute < 0 as the flag for gain and mute being set.
    int mute = -1;
    size_t extra_cargs = 0;
    if(pcm->vol_file) {
      FILE *volfile = fopen(pcm->vol_file, "r");
      if(!volfile) {
        SNDERR("Error reading input volume file %s\n", pcm->vol_file);
        return -EINVAL;
      }
      if(fscanf(volfile, "%lf %d", &gain, &mute) != 2) {
        SNDERR("Error reading input volume file %s\n", pcm->vol_file);
        return -EINVAL;
      }
      debug("Read Volume File:  Gain = %lf, Mute = %d\n", gain, mute);
      fclose(volfile);
    }
    if(pcm->config_in) {
      debug("format_token: %s\n", pcm->format_token);
      debug("rate_token: %s\n", pcm->rate_token);
      debug("channels_token: %s\n", pcm->channels_token);
      debug("ext_samp_token: %s\n", pcm->ext_samp_token);
      FILE *cfgin = fopen(pcm->config_in, "r");
      if(!cfgin) {
        SNDERR("Error reading input config file %s\n", pcm->config_in);
        return -EINVAL;
      }
      FILE *cfgout = fopen(pcm->cargs[1], "w");
      if(!cfgout) {
        SNDERR("Error writing output config file %s\n", pcm->cargs[1]);
        return -EINVAL;
      }
      char buf[1000];
      char *obuf;
      while(fgets(buf, sizeof(buf), cfgin)) {
        obuf = strrep(buf, pcm->format_token, sformat);
        obuf = strrep(obuf, pcm->rate_token, srate);
        obuf = strrep(obuf, pcm->channels_token, schannels);
        if(extrasamples >= 0) {
          obuf = strrep(obuf, pcm->ext_samp_token, sextrasamples);
        }
        fprintf(cfgout,"%s",obuf);
      }
      fclose(cfgin);
      fclose(cfgout);
    } else if(pcm->config_cmd) {
      char command[1000];
      // Call the config_cmd with the hw params to do whatever
      // camilla configuration is desired
      // Command will be called with arguments "format rate channels"
      snprintf(command, 1000, "%s %s %d %d\n", pcm->config_cmd, 
          sformat, pcm->io.rate, pcm->io.channels);
      debug("Calling config_cmd %s\n", command);
      int err = system(command);
      if(err != 0) {
        SNDERR("Error executing config_cmd %s\n", pcm->config_cmd);
        if(err > 0) return -err;
        return err;
      }
    } else {
      // Pass the hw_params as arguments directly to CamillaDSP
      char farg[] = "-f";
      pcm->cargs[pcm->n_cargs] = farg;
      pcm->cargs[pcm->n_cargs+1] = sformat;
      extra_cargs += 2;

      char rarg[] = "-r";
      pcm->cargs[pcm->n_cargs+2] = rarg;
      pcm->cargs[pcm->n_cargs+3] = srate;
      extra_cargs += 2;

      char narg[] = "-n";
      pcm->cargs[pcm->n_cargs+4] = narg;
      pcm->cargs[pcm->n_cargs+5] = schannels;
      extra_cargs += 2;

      char earg[] = "-e";
      if(extrasamples >= 0) {
        pcm->cargs[pcm->n_cargs+6] = earg;
        pcm->cargs[pcm->n_cargs+7] = sextrasamples;
        extra_cargs += 2;
      } else {
        pcm->cargs[pcm->n_cargs+6] = 0;
        pcm->cargs[pcm->n_cargs+7] = 0;
        // Don't advance extra_cargs pointer as we might add
        // gain and mute
      }
    }
    if(mute >= 0) {
      char sgain[30]; // Big enough for argument and value
      snprintf(sgain, sizeof(sgain), "-g%lf", gain);
      pcm->cargs[pcm->n_cargs+extra_cargs] = sgain;
      if(mute > 0) {
        char marg[] = "-m";
        pcm->cargs[pcm->n_cargs+extra_cargs+1] = marg;
        pcm->cargs[pcm->n_cargs+extra_cargs+2] = 0;
      } else {
        pcm->cargs[pcm->n_cargs+extra_cargs+1] = 0;
      }
    }

    execv(pcm->cpath, pcm->cargs);

    // Shouldn't get here
    SNDERR("Failed to execute CamillaDSP");
    return -ENODEV;
  } else {
    // Parent process
    close(fd[0]);
    pcm->cdsp_pcm_fd = fd[1];
  }
  return 0;
}

static int cdsp_start(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  debug("Starting\n");

  // If the IO thread is already started, skip thread creation. Otherwise,
  // we might end up with a bunch of IO threads reading or writing to the
  // same FIFO simultaneously. Instead, just send resume signal. */
  if (pcm->io_started) {
    pthread_kill(pcm->io_thread, SIGIO);
    return 0;
  }

  // Initialize delay calculation - capture reception begins immediately,
  // playback transmission begins only when first period has been written
  // by the application.
  pcm->delay_running = io->stream == SND_PCM_STREAM_CAPTURE ? true : false;
  gettimestamp(&pcm->delay_ts);

  // start the IO thread
  pcm->io_started = true;
  if ((errno = pthread_create(&pcm->io_thread, NULL,
          PTHREAD_ROUTINE(io_thread), io)) != 0) {
    error("Couldn't create IO thread: %s\n", strerror(errno));
    pcm->io_started = false;
    return -errno;
  }

  pthread_setname_np(pcm->io_thread, "pcm-io");
  return 0;
}

static int cdsp_stop(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  debug("Stopping\n");

  if (pcm->io_started) {
    pcm->io_started = false;
    pthread_cancel(pcm->io_thread);
  }
  pthread_join(pcm->io_thread, NULL);

  pcm->delay_running = false;
  pcm->delay_pcm_nread = 0;

  // Bug in ioplug - if pcm->io_status == -1 then it reports state
  // SND_PCM_STATE_XRUN instead of SND_PCM_STATE_SETUP after PCM
  // was stopped.
  // However -1 should be set if it is due to draining
  if(io->state != SND_PCM_STATE_DRAINING) {
    pcm->io_status = 0;
  }

  // Applications that call poll() after snd_pcm_drain() will be blocked
  // forever unless we generate a poll() event here.
  eventfd_write(pcm->event_fd, 1);

  return 0;
}

static snd_pcm_sframes_t cdsp_pointer(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  
  // Any error returned here is translated to -EPIPE, SND_PCM_STATE_XRUN,
  // by ioplug; and that prevents snd_pcm_readi() and snd_pcm_writei()
  // from returning -ENODEV to the application on device disconnection.
  // Instead, when the device is disconnected, we update the PCM state
  // directly here but we do not return an error code. This ensures that
  // ioplug does not undo that state change. Both snd_pcm_readi() and
  // snd_pcm_writei() return -ENODEV when the PCM state is
  // SND_PCM_STATE_DISCONNECTED after their internal call to
  // snd_pcm_avail_update(), which will be the case when we set it here.
  if (pcm->cdsp_pcm_fd == -1)
    snd_pcm_ioplug_set_state(io, SND_PCM_STATE_DISCONNECTED);
#ifndef SND_PCM_IOPLUG_FLAG_BOUNDARY_WA
  if (pcm->io_status >= 0)
    return pcm->io_hw_ptr % io->buffer_size;
#endif
  if (pcm->io_status >= 0)
    return pcm->io_hw_ptr;
  return pcm->io_status;
}

static void free_cdsp(cdsp_t **pcm) {
  if ((*pcm)->event_fd != -1)
    close((*pcm)->event_fd);
  if((*pcm)->cpath)
    free((void *)(*pcm)->cpath);
  if((*pcm)->config_in)
    free((void *)(*pcm)->config_in);
  for(size_t f = 0; f < (*pcm)->n_cargs; f++) {
    if((*pcm)->cargs[f] != 0) {
      free((void *)(*pcm)->cargs[f]);
    }
  }
  if((*pcm)->config_cmd)
    free((void *)(*pcm)->config_cmd);
  if((*pcm)->vol_file)
    free((void *)(*pcm)->vol_file);
  if((*pcm)->format_token)
    free((void *)(*pcm)->format_token);
  if((*pcm)->rate_token)
    free((void *)(*pcm)->rate_token);
  if((*pcm)->channels_token)
    free((void *)(*pcm)->channels_token);
  if((*pcm)->ext_samp_token)
    free((void *)(*pcm)->ext_samp_token);
  if((*pcm)->start_cmd)
    free((void *)(*pcm)->start_cmd);
  if((*pcm)->camilla_exit_cmd)
    free((void *)(*pcm)->camilla_exit_cmd);
  pthread_mutex_destroy(&(*pcm)->mutex);
  pthread_cond_destroy(&(*pcm)->pause_cond);
  free((void *)*pcm);
}

static int cdsp_close(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  debug("Closing\n");
  free_cdsp(&pcm);
  return 0;
}

static int cdsp_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params __attribute__((unused))) {
  cdsp_t *pcm = io->private_data;
  info("Initializing hw_params: %s %d %d\n",
      snd_pcm_format_name(io->format), io->rate, io->channels);

  pcm->frame_size = (snd_pcm_format_physical_width(io->format)*io->channels)/8;

  // Start CamillaDSP in a forked process
  start_camilla(pcm);

  // By default, the size of the pipe buffer is set to a too large value for
  // our purpose. On modern Linux system it is 65536 bytes. Large buffer in
  // the playback mode might contribute to an unnecessary audio delay. Since
  // it is possible to modify the size of this buffer we will set is to some
  // low value, but big enough to prevent audio tearing. Note, that the size
  // will be rounded up to the page size (typically 4096 bytes).
  pcm->delay_fifo_size = 
    fcntl(pcm->cdsp_pcm_fd, F_SETPIPE_SZ, 2048) / pcm->frame_size;

  info("FIFO buffer size: %ld frames\n", pcm->delay_fifo_size);

  /* ALSA default for avail min is one period. */
  pcm->io_avail_min = io->period_size;

  info("Selected HW buffer: %ld periods x %ld bytes %c= %ld bytes\n",
      io->buffer_size / io->period_size, pcm->frame_size * io->period_size,
      io->period_size * (io->buffer_size / io->period_size) == io->buffer_size ? '=' : '<',
      io->buffer_size * pcm->frame_size);

  return 0;
}

static int cdsp_hw_free(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  debug("Freeing HW\n");
  int err = 0;
  if(pcm->camilla_exit_cmd) {
    debug("Calling camilla_exit_cmd: %s\n", pcm->camilla_exit_cmd);
    // Call the camilla_exit_cmd 
    err = system(pcm->camilla_exit_cmd);
    if(err != 0) {
      SNDERR("Error executing camilla_exit_cmd %s\n", pcm->camilla_exit_cmd);
    }
  } 
  debug("Stopping Camilla\n");
  if (close_transport(pcm) == -1)
    return -errno;
  if(pcm->cpid != -1) {
    // Wait on CamillaDSP to finish.  It needs to free the ALSA
    // device before another copy is started.
    waitpid(pcm->cpid, NULL, 0);
    pcm->cpid = -1;
  }
  if(err > 0) return -err;
  return err;
}

// A check that get_boundary still works
static snd_pcm_uframes_t calc_boundary_size(snd_pcm_ioplug_t *io) {
  snd_pcm_uframes_t boundary;
  boundary = io->buffer_size;
  while (boundary * 2 <= LONG_MAX - io->buffer_size)
    boundary *= 2;
  return boundary;
}

static int cdsp_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params) {
  cdsp_t *pcm = io->private_data;
  debug("Initializing SW\n");

  snd_pcm_sw_params_get_boundary(params, &pcm->io_hw_boundary);
  assert(pcm->io_hw_boundary == calc_boundary_size(io));

  // We would get avail_min here but alsa has hidden it from the plugin
  // So we'll just have to ignore the player's request and stick to
  // period_size

  return 0;
}

static int cdsp_prepare(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;

  // if PCM FIFO is not opened, report it right away
  if (pcm->cdsp_pcm_fd == -1)
    return -ENODEV;

  // initialize ring buffer and status
  pcm->io_hw_ptr = 0;
  pcm->io_status = 0;

  // The ioplug allocates and configures its channel area buffer when the
  // HW parameters are fixed, but after calling cdsp_hw_params(). So,
  // this is the earliest opportunity for us to safely cache the ring
  // buffer start address.
  const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(io);
  pcm->io_hw_buffer = (char *)areas->addr + areas->first / 8;

  // Indicate that our PCM is ready for IO, even though is is not 100%
  // true - the IO thread may not be running yet. Applications using
  // snd_pcm_sw_params_set_start_threshold() require the PCM to be usable
  // as soon as it has been prepared.
  pcm->first_revent = true;
  eventfd_write(pcm->event_fd, 1);

  debug("Prepared\n");
  return 0;
}

static int cdsp_drain(snd_pcm_ioplug_t *io) {
  debug("Draining\n");
  cdsp_t *pcm = io->private_data;
  // Wait for the playback thread to empty the ring buffer
  while(pcm->io_status >= 0)
    usleep(10);
  // We cannot recover from an error here. By returning zero we ensure that
  // ioplug stops the pcm. Returning an error code would be interpreted by
  // ioplug as an incomplete drain and would it leave the pcm running.
  return 0;
}

// Calculate overall PCM delay.
//
// Exact calculation of the PCM delay is very hard, if not impossible. For
// the sake of simplicity we will make few assumptions and approximations.
// In general, the delay of this plugin is proportional to the number of 
// bytes queued in the FIFO buffer.  Of course CamillaDSP may add consdirable
// additional delay which is not accounted for in this estimation.
static snd_pcm_sframes_t cdsp_calculate_delay(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;

  snd_pcm_sframes_t delay = 0;

  struct timespec now;

  pthread_mutex_lock(&pcm->mutex);

  gettimestamp(&now);
  struct timespec diff;
  difftimespec(&now, &pcm->delay_ts, &diff);

  // the maximum number of frames that can have been
  // produced/consumed by the server since pcm->delay_ts
  unsigned int tframes =
    //(diff.tv_sec * 1000 + diff.tv_nsec / 1000000) * io->rate / 1000;
    (unsigned int)(((double)diff.tv_sec + ((double)diff.tv_nsec)/1e9) * io->rate);

  // the number of frames that were in the FIFO at pcm->delay_ts
  snd_pcm_uframes_t fifo_delay = pcm->delay_pcm_nread / pcm->frame_size;

  delay = fifo_delay;

  // The buffer_delay is the number of frames that were in the buffer at
  // pcm->delay_ts, adjusted the number written by the application since
  // then.
  snd_pcm_sframes_t buffer_delay = 0;
  if (io->state != SND_PCM_STATE_XRUN)
    buffer_delay = snd_pcm_ioplug_hw_avail(io, pcm->delay_hw_ptr, io->appl_ptr);
  delay += buffer_delay;

  if (pcm->delay_running) {
    // Adjust the total delay by the number of frames consumed.
    if ((delay -= tframes) < 0) delay = 0;
  }

  pthread_mutex_unlock(&pcm->mutex);

  return delay;
}

static int cdsp_pause(snd_pcm_ioplug_t *io, int enable) {
  cdsp_t *pcm = io->private_data;

  if (enable == 1) {
    // Synchronize the IO thread with an application thread to ensure that
    // the server will not be paused while we are processing a transfer.
    pthread_mutex_lock(&pcm->mutex);
    pcm->pause_state |= CDSP_PAUSE_STATE_PENDING;
    while (!(pcm->pause_state & CDSP_PAUSE_STATE_PAUSED) 
        && pcm->cdsp_pcm_fd != -1) {
      pthread_cond_wait(&pcm->pause_cond, &pcm->mutex);
    }
    pthread_mutex_unlock(&pcm->mutex);
  }

  if (enable == 0) {
    pcm->first_revent = true;
    pthread_kill(pcm->io_thread, SIGIO);
  } else {
    // store current delay value
    pcm->delay_paused = cdsp_calculate_delay(io);
  }

  // Even though PCM transport is paused, our IO thread is still running. If
  // the implementer relies on the PCM file descriptor readiness, we have to
  // bump our internal event trigger. Otherwise, client might stuck forever
  // in the poll/select system call.
  eventfd_write(pcm->event_fd, 1);

  return 0;
}

static void cdsp_dump(snd_pcm_ioplug_t *io, snd_output_t *out) {
  cdsp_t *pcm = io->private_data;
  snd_output_printf(out, "CamillaDSP Plugin\n");
  snd_output_printf(out, "c_path: %s\n", pcm->cpath);
  snd_output_printf(out, "config_out: %s\n", pcm->cargs[1]);
  if(pcm->config_in)
    snd_output_printf(out, "config_in: %s\n", pcm->config_in);
  if(pcm->config_cmd)
    snd_output_printf(out, "config_cmd: %s\n", pcm->config_cmd);
  snd_output_printf(out, "vol_file: %s\n", pcm->vol_file);
  // alsa-lib commits the PCM setup only if cdsp_hw_params() returned
  // success, so we only dump the ALSA PCM parameters if CamillaDSP was
  // started.
  if (pcm->cpid >= 0) {
    snd_output_printf(out, "Its setup is:\n");
    snd_pcm_dump_setup(io->pcm, out);
  }
}

static int cdsp_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp) {
  cdsp_t *pcm = io->private_data;

  if (pcm->cdsp_pcm_fd == -1)
    return -ENODEV;

  int ret = 0;
  *delayp = 0;

  switch (io->state) {
    case SND_PCM_STATE_PREPARED:
    case SND_PCM_STATE_RUNNING:
      *delayp = cdsp_calculate_delay(io);
      break;
    case SND_PCM_STATE_PAUSED:
      *delayp = pcm->delay_paused;
      break;
    case SND_PCM_STATE_XRUN:
      *delayp = cdsp_calculate_delay(io);
      ret = -EPIPE;
      break;
    case SND_PCM_STATE_SUSPENDED:
      ret = -ESTRPIPE;
      break;
    case SND_PCM_STATE_DISCONNECTED:
      ret = -ENODEV;
      break;
    default:
      break;
  }

  return ret;
}

static int cdsp_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd,
    unsigned int nfds, unsigned short *revents) {
  cdsp_t *pcm = io->private_data;

  *revents = 0;
  int ret = 0;

  if (pcm->cdsp_pcm_fd == -1)
    goto fail;

  // We only advertise a single file descriptor so the 
  // player really should be giving us that descriptor
  // and just that descriptor.  
  assert(nfds == 1);
  assert(pfd[0].fd == pcm->event_fd);

  if (pfd[0].revents & POLLIN) {

    eventfd_t event;
    eventfd_read(pcm->event_fd, &event);

    if (event & 0xDEAD0000)
      goto fail;

    // This call synchronizes the ring buffer pointers and updates the
    // ioplug state.
    snd_pcm_sframes_t avail = snd_pcm_avail(io->pcm);

    // ALSA expects that the event will match stream direction, e.g.
    // playback will not start if the event is for reading.
    *revents = io->stream == SND_PCM_STREAM_CAPTURE ? POLLIN : POLLOUT;

    // We hold the event fd ready, unless insufficient frames are
    // available in the ring buffer.
    bool ready = true;

    switch (io->state) {
      case SND_PCM_STATE_SETUP:
        ready = false;
        *revents = 0;
        break;
      case SND_PCM_STATE_PREPARED:
        // capture poll should block forever
        if (io->stream == SND_PCM_STREAM_CAPTURE) {
          ready = false;
          *revents = 0;
        }
        if ((snd_pcm_uframes_t)avail < pcm->io_avail_min) {
          ready = false;
          *revents = 0;
        }
        break;
      case SND_PCM_STATE_RUNNING:
        if ((snd_pcm_uframes_t)avail < pcm->io_avail_min) {
          if(pcm->first_revent) {
            pcm->first_revent = false;
          } else {
            warn("Revents overcall %lu < %lu\n", avail, pcm->io_avail_min);
          }
          *revents = 0;
        }
        ready = false;
        break;
      case SND_PCM_STATE_XRUN:
      case SND_PCM_STATE_PAUSED:
      case SND_PCM_STATE_SUSPENDED:
        *revents |= POLLERR;
        break;
      case SND_PCM_STATE_DISCONNECTED:
        *revents = POLLERR;
        ret = -ENODEV;
        break;
      case SND_PCM_STATE_OPEN:
        *revents = POLLERR;
        ret = -EBADF;
        break;
      default:
        break;
    };

    if (ready)
      eventfd_write(pcm->event_fd, 1);

  }

  return ret;

fail:
  *revents = POLLERR | POLLHUP;
  return -ENODEV;
}

static const snd_pcm_ioplug_callback_t cdsp_callback = {
  .start = cdsp_start,
  .stop = cdsp_stop,
  .pointer = cdsp_pointer,
  .close = cdsp_close,
  .hw_params = cdsp_hw_params,
  .hw_free = cdsp_hw_free,
  .sw_params = cdsp_sw_params,
  .prepare = cdsp_prepare,
  .drain = cdsp_drain,
  .pause = cdsp_pause,
  .poll_revents = cdsp_poll_revents,
  .dump = cdsp_dump,
  .delay = cdsp_delay,
};

// THIS ASSUMES SRC IS NULL TERMINATED!
static int alloc_copy_string(char **dst, const char *src) {
  size_t len = strlen(src)+1;
  *dst = (char *)malloc(len);
  if(!(*dst)) {
    SNDERR("Out of memory");
    return -ENOMEM;
  }
  strncpy(*dst, src, len);
  return 0;
}

SND_PCM_PLUGIN_DEFINE_FUNC(cdsp) {
  debug("Plugin creation\n");

  cdsp_t *pcm;
  int err = 0;
  snd_config_iterator_t i, next;
  const char *temp = NULL;
  pcm = calloc(1, sizeof(*pcm));
  if(pcm == NULL) {
    return -ENOMEM;
  }

  // HW Parameters to accept
  long min_channels = 0;
  long max_channels = 0;
  long channels = 0;
  // If they list more than 100 rates too bad.
  unsigned int n_rates = 0;
  pcm->n_cargs = 2; // First two are proc name and config
  unsigned int rate_list[100];
  long min_rate = 0;
  long max_rate = 0;
  if((err = alloc_copy_string(&pcm->cargs[0], "camilladsp")) < 0) goto _err;
  pcm->ext_samp = -1;
  pcm->ext_samp_44100 = -1;
  pcm->ext_samp_48000 = -1;

  snd_config_for_each(i, next, conf) {
    snd_config_t *n = snd_config_iterator_entry(i);
    const char *id;
    if(snd_config_get_id(n, &id) < 0)
      continue;
    if(strcmp(id, "comment") == 0 || strcmp(id, "type") == 0)
      continue;
    if(strcmp(id, "cpath") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->cpath, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "config_in") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->config_in, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "config_out") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->cargs[1], temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "config_cmd") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->config_cmd, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "config_cdsp") == 0) {
      if((err = snd_config_get_integer(n, &pcm->config_cdsp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "vol_file") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->vol_file, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "format_token") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->format_token, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "samplerate_token") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->rate_token, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "channels_token") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->channels_token, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "extrasamples_token") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->ext_samp_token, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "cargs") == 0) {
      snd_config_iterator_t ci, cnext;
      snd_config_for_each(ci, cnext, n) {
        if(pcm->n_cargs >= 100) {
          SNDERR("Too many args specified.  Max 100.");
          err = -1;
          goto _err;
        }
        snd_config_t *cn = snd_config_iterator_entry(ci);
        if((err = snd_config_get_string(cn, &temp)) < 0) goto _err;
        if((err = alloc_copy_string(&pcm->cargs[pcm->n_cargs], temp)) < 0)
          goto _err;
        pcm->n_cargs++;
      }
      continue;
    }
    if(strcmp(id, "rates") == 0) {
      snd_config_iterator_t ri, rnext;
      snd_config_for_each(ri, rnext, n) {
        if(n_rates >= 100) {
          SNDERR("Too many rates specified.  Max 100.");
          err = -1;
          goto _err;
        }
        snd_config_t *rn = snd_config_iterator_entry(ri);
        long rate;
        if((err = snd_config_get_integer(rn, &rate)) < 0) goto _err;
        rate_list[n_rates++] = (unsigned)rate;
      }
      continue;
    }
    if(strcmp(id, "min_rate") == 0) {
      if((err = snd_config_get_integer(n, &min_rate)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "max_rate") == 0) {
      if((err = snd_config_get_integer(n, &max_rate)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "min_channels") == 0) {
      if((err = snd_config_get_integer(n, &min_channels)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "max_channels") == 0) {
      if((err = snd_config_get_integer(n, &max_channels)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "channels") == 0) {
      if((err = snd_config_get_integer(n, &channels)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "extra_samples") == 0) {
      if((err = snd_config_get_integer(n, &pcm->ext_samp)) < 0) goto _err;
      if(pcm->ext_samp < 0) {
        SNDERR("extra_samples must be >= 0");
        err = -EINVAL;
        goto _err;
      }
      continue;
    }
    if(strcmp(id, "extra_samples_44100") == 0) {
      if((err = snd_config_get_integer(n, &pcm->ext_samp_44100)) < 0) 
        goto _err;
      if(pcm->ext_samp_44100 < 0) {
        SNDERR("extra_samples_44100 must be >= 0");
        err = -EINVAL;
        goto _err;
      }
      continue;
    }
    if(strcmp(id, "extra_samples_48000") == 0) {
      if((err = snd_config_get_integer(n, &pcm->ext_samp_48000)) < 0) 
        goto _err;
      if(pcm->ext_samp_48000 < 0) {
        SNDERR("extra_samples_48000 must be >= 0");
        err = -EINVAL;
        goto _err;
      }
      continue;
    }
    if(strcmp(id, "start_cmd") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->start_cmd, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "camilla_exit_cmd") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->camilla_exit_cmd, temp)) < 0) goto _err;
      continue;
    }
    err = -EINVAL;
    goto _err;
  }
  
  // Validate user input
  if(!pcm->cpath) {
    SNDERR("Must supply cpath parameter with path to CamillaDSP.");
    err = -EINVAL;
    goto _err;
  }
  if(!pcm->config_in && !pcm->config_cmd && !pcm->config_cdsp) {
    SNDERR("Must supply config_in, config_cmd, or config_cdsp parameter.");
    err = -EINVAL;
    goto _err;
  }
  if(pcm->config_in) {
    if(pcm->config_cmd || pcm->config_cdsp) {
      SNDERR("Only config_in, config_cmd, or config_cdsp can be set.");
      err = -EINVAL;
      goto _err;
    }
  } else if(pcm->config_cmd) {
    if(pcm->config_cdsp) {
      SNDERR("Only config_in, config_cmd, or config_cdsp can be set.");
      err = -EINVAL;
      goto _err;
    }
  }

  if(channels == 0) {
    if(min_channels <= 0 || max_channels <= 0) {
      SNDERR("Must supply valid channel information.");
      err = -EINVAL;
      goto _err;
    }
    if(min_channels > max_channels) {
      SNDERR("Max channels must be >= min channels.");
      err = -EINVAL;
      goto _err;
    }
  } else {
    if(min_channels != 0 || max_channels != 0) {
      SNDERR("Cannot set channels and min/max channels.");
      err = -EINVAL;
      goto _err;
    }
    if(channels < 0) {
      SNDERR("Must supply valid channel information.");
      err = -EINVAL;
      goto _err;
    }
    min_channels = max_channels = channels;
  }

  if(n_rates == 0) {
    if(min_rate <= 0 || max_rate <= 0) {
      SNDERR("Must supply valid sample rate information.");
      err = -EINVAL;
      goto _err;
    }
    if(min_rate > max_rate) {
      SNDERR("Max sample rate must be >= min sample rate.");
      err = -EINVAL;
      goto _err;
    }
  } else {
    if(min_rate != 0 || max_rate != 0) {
      SNDERR("Cannot set rates and min/max rates.");
      err = -EINVAL;
      goto _err;
    }
    // Now find the max rate in the list for setting the period size
    // below
    max_rate = rate_list[0];
    for(unsigned int ii = 0; ii < n_rates; ii++) {
      if(rate_list[ii] > max_rate) max_rate = rate_list[ii];
    }
  }
  if(pcm->config_in) {
    if(!pcm->format_token) {
      if((err = alloc_copy_string(&pcm->format_token, "$format$")) < 0) 
        goto _err;
    }
    if(!pcm->rate_token) {
      if((err = alloc_copy_string(&pcm->rate_token, "$samplerate$")) < 0) 
        goto _err;
    }
    if(!pcm->channels_token) {
      if((err = alloc_copy_string(&pcm->channels_token, "$channels$")) < 0) 
        goto _err;
    }
    if(!pcm->ext_samp_token) {
      if((err = alloc_copy_string(&pcm->ext_samp_token, "$extrasamples$")) < 0) 
        goto _err;
    }
  }
    
  // Done parsing / validating user input


  // Establish the event_fd used to signal ALSA
  if ((pcm->event_fd = eventfd(0, EFD_CLOEXEC)) == -1) {
    err = -errno;
    goto _err;
  }

  pcm->io.version = SND_PCM_IOPLUG_VERSION;
  pcm->io.name = "CamillaDSP Plugin";
  pcm->io.callback = &cdsp_callback;
  pcm->io.private_data = pcm;
  pcm->io.poll_events = POLLIN;
  pcm->io.poll_fd = pcm->event_fd;
  pcm->cpid = -1;
  pcm->cdsp_pcm_fd = -1;
  pthread_mutex_init(&pcm->mutex, NULL);
  pthread_cond_init(&pcm->pause_cond, NULL);
  pcm->pause_state = CDSP_PAUSE_STATE_RUNNING;
  pcm->io.flags = SND_PCM_IOPLUG_FLAG_LISTED;
#ifdef SND_PCM_IOPLUG_FLAG_BOUNDARY_WA
  pcm->io.flags |= SND_PCM_IOPLUG_FLAG_BOUNDARY_WA;
#endif
  pcm->io.mmap_rw = 1;

#if SND_LIB_VERSION >= 0x010102 && SND_LIB_VERSION <= 0x010103
  /* ALSA library thread-safe API functionality does not play well with ALSA
   * IO-plug plug-ins. It causes deadlocks which often make our PCM plug-in
   * unusable. As a workaround we are going to disable this functionality. */
  if (setenv("LIBASOUND_THREAD_SAFE", "0", 0) == -1)
    SNDERR("Couldn't disable ALSA thread-safe API: %s", strerror(errno));
#endif
  err = snd_pcm_ioplug_create(&pcm->io, name, stream, mode);
  if(err < 0) goto _err;
  
  // Configure "hw" constraints
  unsigned int format_list[] = {
    SND_PCM_FORMAT_S16_LE,
    SND_PCM_FORMAT_S24_LE,
    SND_PCM_FORMAT_S24_3LE,
    SND_PCM_FORMAT_S32_LE,
    SND_PCM_FORMAT_FLOAT_LE,
    SND_PCM_FORMAT_FLOAT64_LE
  };
  if((err = snd_pcm_ioplug_set_param_list(&pcm->io, 
      SND_PCM_IOPLUG_HW_FORMAT, 6, format_list)) < 0) goto _err;

  if((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, 
      SND_PCM_IOPLUG_HW_CHANNELS, min_channels, max_channels)) < 0) goto _err;

  if(n_rates > 0) {
    if((err = snd_pcm_ioplug_set_param_list(&pcm->io, 
        SND_PCM_IOPLUG_HW_RATE, n_rates, rate_list)) < 0) goto _err;
  } else {
    if((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, 
        SND_PCM_IOPLUG_HW_RATE, min_rate, max_rate)) < 0) goto _err;
  }

  if ((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, 
          SND_PCM_IOPLUG_HW_PERIODS, 2, 1024)) < 0)
    goto _err;

  // In order to prevent audio tearing and minimize CPU utilization, we're
  // going to setup period size constraint. The limit is derived from the
  // maximum sampling rate, max channels, and maximum integer format size
  // (32 bits) so the minium period "time" size will be about 10ms. The upper
  // limit will not be constrained.
  unsigned int min_p = max_rate / 100 * max_channels * 4 / 8;

  if ((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, 
          SND_PCM_IOPLUG_HW_PERIOD_BYTES, min_p, 1024 * 1024)) < 0) goto _err;

  unsigned int max_buffer = 2*1024*1024;
  if(max_buffer < 2*min_p) max_buffer = 2*min_p;
  if((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, 
          SND_PCM_IOPLUG_HW_BUFFER_BYTES, 2*min_p, max_buffer)) < 0) goto _err;

  *pcmp = pcm->io.pcm;

  if(pcm->start_cmd) {
    debug("Calling start_cmd: %s\n", pcm->start_cmd);
    // Call the start_cmd 
    int err = system(pcm->start_cmd);
    if(err != 0) {
      SNDERR("Error executing start_cmd %s\n", pcm->start_cmd);
      if(err > 0) return -err;
      return err;
    }
  } 

  return 0;

_err:
  free_cdsp(&pcm);
  return err;
}
SND_PCM_PLUGIN_SYMBOL(cdsp)

