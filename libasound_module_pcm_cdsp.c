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

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "rt.h"
#include "strrep.h"

#define CDSP_PAUSE_STATE_RUNNING 0
#define CDSP_PAUSE_STATE_PAUSED  (1 << 0)
#define CDSP_PAUSE_STATE_PENDING (1 << 1)

// Cleanup callback casting wrapper for the brevity's sake.
#define PTHREAD_CLEANUP(f) ((void (*)(void *))(void (*)(void))(f))

// Thread routing callback casting wrapper.
#define PTHREAD_ROUTINE(f) ((void *(*)(void *))(f))


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
  volatile snd_pcm_sframes_t io_hw_ptr;
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
  // Arguments to execv
  // cargs[0] = "camilladsp" => Process name
  // cargs[1] = config_out => Location of CamillaDSP output YAML configuration
  // cargs[2+] = Additional arguments passed through .asoundrc
  char *cargs[100];
  // Extra samples parameter to pass to CamillaDSP if the config_in template
  // is used instead of config_cmd
  // ext_samp_44100 and ext_samp_4800 allow rate matched expansion of the
  // extra samples.  They will be multiplied by {rate}/44100 or rate/{48000}
  // if {rate} is an integer multiple of 44100 or 48000 respectively
  long ext_samp;
  long ext_samp_44100;
  long ext_samp_48000;
} cdsp_t;

//#define DEBUG 1
#if DEBUG
void _debug(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  fprintf(stderr, format, ap);
  va_end(ap);
}
#endif
#if DEBUG
void _debug(const char *format, ...) __attribute__ ((format(printf, 1, 2)));
# define debug(M, ...) _debug("%s:%d: " M, __FILE__, __LINE__, ## __VA_ARGS__)
#else
# define debug(M, ...) do {} while (0)
#endif

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
  debug("IO thread cleanup");
  pcm->io_started = false;
}

// Helper function for IO thread delay calculation.
static void io_thread_update_delay(cdsp_t *pcm, snd_pcm_sframes_t hw_ptr) {
  struct timespec now;
  unsigned int nread = 0;

  gettimestamp(&now);
  ioctl(pcm->cdsp_pcm_fd, FIONREAD, &nread);

  pthread_mutex_lock(&pcm->mutex);

  // stash current time and levels
  pcm->delay_ts = now;
  pcm->delay_pcm_nread = nread;
  if (hw_ptr == -1) {
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

  struct asrsync asrs;
  asrsync_init(&asrs, io->rate);

  // We update pcm->io_hw_ptr (i.e. the value seen by ioplug) only when
  // a period has been completed. We use a temporary copy during the
  // transfer procedure.
  snd_pcm_uframes_t io_hw_ptr = pcm->io_hw_ptr;

  // The number of frames to complete the current period.
  snd_pcm_uframes_t balance = io->period_size;

  debug("Starting IO loop: %d", pcm->cdsp_pcm_fd);
  for (;;) {
    if (pcm->pause_state & CDSP_PAUSE_STATE_PENDING ||
        pcm->io_hw_ptr == -1) {
      debug("Pausing IO thread: %ld", pcm->io_hw_ptr);

      pthread_mutex_lock(&pcm->mutex);
      pcm->pause_state = CDSP_PAUSE_STATE_PAUSED;
      pthread_cond_signal(&pcm->pause_cond);
      pthread_mutex_unlock(&pcm->mutex);

      int tmp;
      sigwait(&sigset, &tmp);

      pthread_mutex_lock(&pcm->mutex);
      pcm->pause_state = CDSP_PAUSE_STATE_RUNNING;
      pthread_mutex_unlock(&pcm->mutex);

      debug("IO thread resumed");

      if (pcm->io_hw_ptr == -1)
        continue;
      if (pcm->cdsp_pcm_fd == -1) {
        debug("FAILING BECAUSE PIPE GONE\n");
        goto fail;
      }

      asrsync_init(&asrs, io->rate);
      io_hw_ptr = io->hw_ptr;
    }

    if (io->state == SND_PCM_STATE_DISCONNECTED) {
      fprintf(stderr,"DISCONNECTED?\n");
      goto fail;
    }

    // There are 2 reasons why the number of available frames may be
    // zero: XRUN or drained final samples; we set the HW pointer to
    // -1 to indicate we have no work to do.
    snd_pcm_uframes_t avail;
    if ((avail = snd_pcm_ioplug_hw_avail(io, io_hw_ptr, io->appl_ptr)) == 0) {
      io_thread_update_delay(pcm, 0);
      io_hw_ptr = -1;
      goto sync;
    }

    // the number of frames to be transferred in this iteration
    snd_pcm_uframes_t frames = balance;
    // current offset of the head pointer in the IO buffer
    snd_pcm_uframes_t offset = io_hw_ptr % io->buffer_size;

    // Do not try to transfer more frames than are available in the ring
    // buffer!
    if (frames > avail)
      frames = avail;

    // If the leftover in the buffer is less than a whole period sizes,
    // adjust the number of frames which should be transfered. It has
    // turned out, that the buffer might contain fractional number of
    // periods - it could be an ALSA bug, though, it has to be handled.
    if (io->buffer_size - offset < frames)
      frames = io->buffer_size - offset;

    // IO operation size in bytes
    size_t len = frames * pcm->frame_size;
    char *head = pcm->io_hw_buffer + offset * pcm->frame_size;

    // Increment the HW pointer (with boundary wrap) ready for the next
    // iteration.
    io_hw_ptr += frames;
    if (io_hw_ptr >= pcm->io_hw_boundary)
      io_hw_ptr -= pcm->io_hw_boundary;

    ssize_t ret = 0;

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

    // synchronize playback time
    asrsync_sync(&asrs, frames);

    // repeat until period is completed
    balance -= frames;
    if (balance > 0)
      continue;

sync:
    // Make the new HW pointer value visible to the ioplug.
    pcm->io_hw_ptr = io_hw_ptr;

    // Generate poll() event so application is made aware of
    // the HW pointer change.
    eventfd_write(pcm->event_fd, 1);

    // Start the next period.
    balance = io->period_size;
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
  char cformat[20];
  switch (pcm->io.format) {
    case SND_PCM_FORMAT_S16_LE:
      snprintf(cformat, sizeof(cformat), "S16LE");
      break;
    case SND_PCM_FORMAT_S24_LE:
      snprintf(cformat, sizeof(cformat), "S24LE");
      break;
    case SND_PCM_FORMAT_S24_3LE:
      snprintf(cformat, sizeof(cformat), "S24LE3");
      break;
    case SND_PCM_FORMAT_S32_LE:
      snprintf(cformat, sizeof(cformat), "S32LE");
      break;
    case SND_PCM_FORMAT_FLOAT_LE:
      snprintf(cformat, sizeof(cformat), "FLOAT32LE");
      break;
    case SND_PCM_FORMAT_FLOAT64_LE:
      snprintf(cformat, sizeof(cformat), "FLOAT64LE");
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

  char sextrasamples[20] = "0";  // Some use really long audio chains
  // We multiply the ext_samp by the ratio of sample rate to
  // one of the two common audio rates if the sample rate is an
  // integer multiple
  if((pcm->ext_samp_44100 > 0) && ((pcm->io.rate % 44100) == 0)) {
    snprintf(sextrasamples, sizeof(sextrasamples), "%lu", 
        pcm->ext_samp_44100*(pcm->io.rate/44100));
  } else if((pcm->ext_samp_48000 > 0) && ((pcm->io.rate % 48000) == 0)) {
    snprintf(sextrasamples, sizeof(sextrasamples), "%lu", 
        pcm->ext_samp_48000*(pcm->io.rate/48000));
  } else if(pcm->ext_samp > 0) {
    snprintf(sextrasamples, sizeof(sextrasamples), "%lu", 
        pcm->ext_samp);
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

    debug("cpath %s\n", pcm->cpath);
    debug("config_in %s\n", pcm->config_in);
    debug("config_out %s\n", pcm->cargs[1]);
    debug("config_cmd %s\n", pcm->config_cmd);
    debug("cargs:");
#ifdef DEBUG
    int ca = 2;
    while(pcm->cargs[ca]) {
      debug(" %s", pcm->cargs[ca]);
      ca++;
    }
    debug("\n");
#endif
    
    if(!pcm->config_cmd) {
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
        obuf = strrep(buf, "{format}", cformat);
        obuf = strrep(obuf, "{channels}", schannels);
        obuf = strrep(obuf, "{samplerate}", srate);
        obuf = strrep(obuf, "{extrasamples}", sextrasamples);
        fprintf(cfgout,"%s",obuf);
      }
      fclose(cfgin);
      fclose(cfgout);
    } else {
      char command[1000];
      // Call the config_cmd with the hw params to do whatever
      // camilla configuration is desired
      // Command will be called with arguments "format rate channels"
      snprintf(command, 1000, "%s %s %d %d\n", pcm->config_cmd, 
          cformat, pcm->io.rate, pcm->io.channels);
      debug("CALLING CONFIG COMMAND %s\n", command);
      int err = system(command);
      if(err != 0) {
        SNDERR("Error executing config command %s\n", pcm->config_cmd);
        if(err > 0) return -err;
        return err;
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
  debug("Starting");

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
    debug("Couldn't create IO thread: %s", strerror(errno));
    pcm->io_started = false;
    return -errno;
  }

  pthread_setname_np(pcm->io_thread, "pcm-io");
  return 0;
}

static int cdsp_stop(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  debug("Stopping");

  if (pcm->io_started) {
    pcm->io_started = false;
    pthread_cancel(pcm->io_thread);
    pthread_join(pcm->io_thread, NULL);
  }

  pcm->delay_running = false;
  pcm->delay_pcm_nread = 0;

  // Bug in ioplug - if pcm->io_hw_ptr == -1 then it reports state
  // SND_PCM_STATE_XRUN instead of SND_PCM_STATE_SETUP after PCM
  // was stopped.
  pcm->io_hw_ptr = 0;

  // Applications that call poll() after snd_pcm_drain() will be blocked
  // forever unless we generate a poll() event here.
  eventfd_write(pcm->event_fd, 1);

  return 0;
}

static snd_pcm_sframes_t cdsp_pointer(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  if (pcm->cdsp_pcm_fd == -1)
    return -ENODEV;
#ifndef SND_PCM_IOPLUG_FLAG_BOUNDARY_WA
  if (pcm->io_hw_ptr != -1)
    return pcm->io_hw_ptr % io->buffer_size;
#endif
  return pcm->io_hw_ptr;
}

static int cdsp_close(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  debug("Closing");
  close(pcm->event_fd);
  if(pcm->cpath)
    free((void *)pcm->cpath);
  if(pcm->config_in)
    free((void *)pcm->config_in);
  int f = 0;
  while(pcm->cargs[f] != 0) {
    free((void *)pcm->cargs[f++]);
  }
  if(pcm->config_cmd)
    free((void *)pcm->config_cmd);
  pthread_mutex_destroy(&pcm->mutex);
  pthread_cond_destroy(&pcm->pause_cond);
  free(pcm);
  return 0;
}

static int cdsp_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
  cdsp_t *pcm = io->private_data;
  debug("Initializing HW");

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

  debug("FIFO buffer size: %zd frames", pcm->delay_fifo_size);

  /* ALSA default for avail min is one period. */
  pcm->io_avail_min = io->period_size;

  debug("Selected HW buffer: %zd periods x %zd bytes %c= %zd bytes",
      io->buffer_size / io->period_size, pcm->frame_size * io->period_size,
      io->period_size * (io->buffer_size / io->period_size) == io->buffer_size ? '=' : '<',
      io->buffer_size * pcm->frame_size);

  return 0;
}

static int cdsp_hw_free(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  debug("Freeing HW");
  debug("Stopping Camilla");
  if (close_transport(pcm) == -1)
    return -errno;
  if(pcm->cpid != -1) {
    // Wait on CamillaDSP to finish.  It needs to free the ALSA
    // device before another copy is started.
    waitpid(pcm->cpid, NULL, 0);
    pcm->cpid = -1;
  }
  return 0;
}

static int cdsp_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params) {
  cdsp_t *pcm = io->private_data;
  debug("Initializing SW");

  snd_pcm_sw_params_get_boundary(params, &pcm->io_hw_boundary);

  snd_pcm_uframes_t avail_min;
  snd_pcm_sw_params_get_avail_min(params, &avail_min);
  if (avail_min != pcm->io_avail_min) {
    debug("Changing SW avail min: %zu -> %zu", pcm->io_avail_min, avail_min);
    pcm->io_avail_min = avail_min;
  }

  return 0;
}

static int cdsp_prepare(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;

  // if PCM FIFO is not opened, report it right away
  if (pcm->cdsp_pcm_fd == -1)
    return -ENODEV;

  // initialize ring buffer
  pcm->io_hw_ptr = 0;

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
  eventfd_write(pcm->event_fd, 1);

  debug("Prepared");
  return 0;
}

static int cdsp_drain(snd_pcm_ioplug_t *io) {
  debug("Draining...");
  cdsp_t *pcm = io->private_data;
  // Wait for the playback thread to empty the ring buffer
  while(pcm->io_hw_ptr != -1)
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
  gettimestamp(&now);

  pthread_mutex_lock(&pcm->mutex);

  struct timespec diff;
  difftimespec(&now, &pcm->delay_ts, &diff);

  // the maximum number of frames that can have been
  // produced/consumed by the server since pcm->delay_ts
  unsigned int tframes =
    (diff.tv_sec * 1000 + diff.tv_nsec / 1000000) * io->rate / 1000;

  // the number of frames that were in the FIFO at pcm->delay_ts
  snd_pcm_uframes_t fifo_delay = pcm->delay_pcm_nread / pcm->frame_size;

  delay = fifo_delay;

  // The buffer_delay is the number of frames that were in the buffer at
  // pcm->delay_ts, adjusted the number written by the application since
  // then.
  snd_pcm_sframes_t buffer_delay = 0;
  if (io->state != SND_PCM_STATE_XRUN)
    buffer_delay = snd_pcm_ioplug_hw_avail(io, pcm->delay_hw_ptr, io->appl_ptr);

  // If the PCM is running, then some frames from the buffer may have been
  // consumed.
  if (pcm->delay_running)
    delay += buffer_delay;

  // Adjust the total delay by the number of frames consumed.
  if ((delay -= tframes) < 0)
    delay = 0;

  // If the PCM is not running, then the frames in the buffer will not have
  // been consumed since pcm->delay_ts.
  if (!pcm->delay_running)
    delay += buffer_delay;

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

  if (enable == 0)
    pthread_kill(pcm->io_thread, SIGIO);
  else
    // store current delay value
    pcm->delay_paused = cdsp_calculate_delay(io);

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

static int cdsp_poll_descriptors_count(snd_pcm_ioplug_t *io) {
  return 2;
}

static int cdsp_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd,
    unsigned int nfds) {
  cdsp_t *pcm = io->private_data;

  pfd[1].fd = pcm->cdsp_pcm_fd;
  pfd[1].events = POLLOUT;

  // PCM plug-in relies on our internal event file descriptor.
  pfd[0].fd = pcm->event_fd;
  pfd[0].events = POLLIN;

  return 2;
}

static int cdsp_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd,
    unsigned int nfds, unsigned short *revents) {
  cdsp_t *pcm = io->private_data;

  *revents = 0;
  int ret = 0;

  if (pcm->cdsp_pcm_fd == -1)
    goto fail;

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
        break;
      case SND_PCM_STATE_RUNNING:
        if ((snd_pcm_uframes_t)avail < pcm->io_avail_min) {
          ready = false;
          *revents = 0;
        }
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
  .dump = cdsp_dump,
  .delay = cdsp_delay,
  .poll_descriptors_count = cdsp_poll_descriptors_count,
  .poll_descriptors = cdsp_poll_descriptors,
  .poll_revents = cdsp_poll_revents,
};

SND_PCM_PLUGIN_DEFINE_FUNC(cdsp) {
  debug("Plugin creation\n");

  cdsp_t *pcm;
  int err = 0;
  snd_config_iterator_t i, next;
  const char *temp = NULL;
  pcm = calloc(1, sizeof(*pcm));
  if(pcm == NULL) {
    err = -ENOMEM;
    goto _err;
  }

  // HW Parameters to accept
  long min_channels = 0;
  long max_channels = 0;
  long channels = 0;
  // If they list more than 100 rates too bad.
  unsigned int n_rates = 0;
  unsigned int n_cargs = 2; // First two are proc name and config
  unsigned int rate_list[100];
  long min_rate = 0;
  long max_rate = 0;
  pcm->cargs[0] = (char *)malloc(strlen("camilladsp")+1);
  if(!pcm->cargs[0]) {
    SNDERR("Out of memory");
    err = -ENOMEM;
    goto _err;
  }
  strncpy(pcm->cargs[0], "camilladsp", strlen("camilladsp")+1);

  snd_config_for_each(i, next, conf) {
    snd_config_t *n = snd_config_iterator_entry(i);
    const char *id;
    if(snd_config_get_id(n, &id) < 0)
      continue;
    if(strcmp(id, "comment") == 0 || strcmp(id, "type") == 0)
      continue;
    if(strcmp(id, "cpath") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      pcm->cpath = (char *)malloc(strlen(temp)+1);
      if(!pcm->cpath) {
        SNDERR("Out of memory");
        err = -ENOMEM;
        goto _err;
      }
      strncpy(pcm->cpath, temp, strlen(temp)+1);
      continue;
    }
    if(strcmp(id, "config_in") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      pcm->config_in = (char *)malloc(strlen(temp)+1);
      if(!pcm->config_in) {
        SNDERR("Out of memory");
        err = -ENOMEM;
        goto _err;
      }
      strncpy(pcm->config_in, temp, strlen(temp)+1);
      continue;
    }
    if(strcmp(id, "config_out") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      pcm->cargs[1] = (char *)malloc(strlen(temp)+1);
      if(!pcm->cargs[1]) {
        SNDERR("Out of memory");
        err = -ENOMEM;
        goto _err;
      }
      strncpy(pcm->cargs[1], temp, strlen(temp)+1);
      continue;
    }
    if(strcmp(id, "config_cmd") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      pcm->config_cmd = (char *)malloc(strlen(temp)+1);
      if(!pcm->config_cmd) {
        SNDERR("Out of memory");
        err = -ENOMEM;
        goto _err;
      }
      strncpy(pcm->config_cmd, temp, strlen(temp)+1);
      continue;
    }
    if(strcmp(id, "cargs") == 0) {
      snd_config_iterator_t ci, cnext;
      snd_config_for_each(ci, cnext, n) {
        if(n_cargs >= 100) {
          SNDERR("Too many args specified.  Max 100.");
          err = -1;
          goto _err;
        }
        snd_config_t *cn = snd_config_iterator_entry(ci);
        if((err = snd_config_get_string(cn, &temp)) < 0) goto _err;
        pcm->cargs[n_cargs] = (char *)malloc(strlen(temp)+1);
        if(!pcm->cargs[n_cargs]) {
          SNDERR("Out of memory");
          err = -ENOMEM;
          goto _err;
        }
        strncpy(pcm->cargs[n_cargs++], temp, strlen(temp)+1);
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
      if((err = snd_config_get_integer(n, &pcm->ext_samp)) < 0) 
              goto _err;
      continue;
    }
    if(strcmp(id, "extra_samples_44100") == 0) {
      if((err = snd_config_get_integer(n, &pcm->ext_samp_44100)) < 0) 
              goto _err;
      continue;
    }
    if(strcmp(id, "extra_samples_48000") == 0) {
      if((err = snd_config_get_integer(n, &pcm->ext_samp_48000)) < 0) 
              goto _err;
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
  if(!pcm->config_in && !pcm->config_cmd) {
    SNDERR("Must supply config_in file parameter or config_cmd parameter.");
    err = -EINVAL;
    goto _err;
  }
  if(pcm->config_in && pcm->config_cmd) {
    SNDERR("Only config_in or config_cmd can be set, not both.");
    err = -EINVAL;
    goto _err;
  }
  if(!pcm->cargs[1]) {
    SNDERR("Must supply config_out file parameter.");
    err = -EINVAL;
    goto _err;
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
  // Done parsing / validating user input

  pcm->io.version = SND_PCM_IOPLUG_VERSION;
  pcm->io.name = "CamillaDSP Plugin";
  pcm->io.mmap_rw = 0;
  pcm->io.callback = &cdsp_callback;
  pcm->io.private_data = pcm;
  pcm->cpid = -1;
  pcm->event_fd = -1;
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

  if ((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, SND_PCM_IOPLUG_HW_PERIODS,
          2, 1024)) < 0)
    goto _err;

  // In order to prevent audio tearing and minimize CPU utilization, we're
  // going to setup period size constraint. The limit is derived from the
  // maximum sampling rate, max channels, and maximum integer format size
  // (32 bits) so the minium period "time" size will be about 10ms. The upper
  // limit will not be constrained.
  unsigned int min_p = max_rate / 100 * max_channels * 4 / 8;

  if ((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, 
          SND_PCM_IOPLUG_HW_PERIOD_BYTES, min_p, 1024 * 16)) < 0) goto _err;

//  if((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, 
//          SND_PCM_IOPLUG_HW_BUFFER_BYTES, 32*1024, 128*1024)) < 0) goto _err;

  *pcmp = pcm->io.pcm;

  if ((pcm->event_fd = eventfd(0, EFD_CLOEXEC)) == -1) {
    err = -errno;
    goto _err;
  }

  return 0;

_err:
  if (pcm->event_fd != -1)
    close(pcm->event_fd);
  if(pcm->cpath)
    free((void *)pcm->cpath);
  if(pcm->config_in)
    free((void *)pcm->config_in);
  int f = 0;
  while(pcm->cargs[f] != 0) {
    free((void *)pcm->cargs[f++]);
  }
  if(pcm->config_cmd)
    free((void *)pcm->config_cmd);
  if(pcm)
    free((void *)pcm);
  return err;
}
SND_PCM_PLUGIN_SYMBOL(cdsp)

