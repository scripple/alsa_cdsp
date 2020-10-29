/*
 * Based on BlueALSA - rt.h / rt.c from the bluez-alsa project
 * which is Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef RT_H_
#define RT_H_

#include <stdint.h>
#include <sys/time.h>
#include <time.h>

/**
 * Structure used for time synchronization.
 *
 * With the size of the frame counter being 32 bits, it is possible to track
 * up to ~24 hours, with the sampling rate of 48 kHz. If it is insufficient,
 * one can switch to 64 bits, which would suffice for 12 million years. */
struct asrsync {

	/* used sampling rate */
	unsigned int rate;
	/* reference time point */
	struct timespec ts0;

	/* time-stamp from the previous sync */
	struct timespec ts;
	/* transfered frames since ts0 */
	uint64_t frames;

	/* time spent outside of the sync function */
	struct timespec ts_busy;
	/* If the asrsync_sync() returns a positive value, then this variable
	 * contains an amount of time used for synchronization. Otherwise, it
	 * contains an overdue time - synchronization was not possible due to
	 * too much time spent outside of the sync function. */
	struct timespec ts_idle;

};

/**
 * Start (initialize) time synchronization.
 *
 * @param asrs Pointer to the time synchronization structure.
 * @param sr Synchronization sampling rate. */
#define asrsync_init(asrs, sr) do { \
		(asrs)->rate = sr; \
		gettimestamp(&(asrs)->ts0); \
		(asrs)->ts = (asrs)->ts0; \
		(asrs)->frames = 0; \
	} while (0)

int asrsync_sync(struct asrsync *asrs, uint64_t frames);

/**
 * Get the number of microseconds spent outside of the sync function. */
#define asrsync_get_busy_usec(asrs) \
	((asrs)->ts_busy.tv_nsec / 1000)

/**
 * Get system monotonic time-stamp.
 *
 * @param ts Address to the timespec structure where the time-stamp will
 *   be stored.
 * @return On success this function returns 0. Otherwise, -1 is returned
 *   and errno is set to indicate the error. */
#ifdef CLOCK_MONOTONIC_RAW
# define gettimestamp(ts) clock_gettime(CLOCK_MONOTONIC_RAW, ts)
#else
# define gettimestamp(ts) clock_gettime(CLOCK_MONOTONIC, ts)
#endif

int difftimespec(
		const struct timespec *ts1,
		const struct timespec *ts2,
		struct timespec *ts);

/**
 * Synchronize time with the sampling rate.
 *
 * Notes:
 * 1. Time synchronization relies on the frame counter being linear.
 * 2. In order to prevent frame counter overflow (for more information see
 *   the asrsync structure definition), this counter should be initialized
 *   (zeroed) upon every transfer stop.
 *
 * @param asrs Pointer to the time synchronization structure.
 * @param frames Number of frames since the last call to this function.
 * @return This function returns a positive value or zero respectively for
 *   the case, when the synchronization was required or when blocking was
 *   not necessary. If an error has occurred, -1 is returned and errno is
 *   set to indicate the error. */
int asrsync_sync(struct asrsync *asrs, uint64_t frames) {

	const unsigned int rate = asrs->rate;
	struct timespec ts_rate;
	struct timespec ts;
	int rv = 0;

	asrs->frames += frames;
	frames = asrs->frames;

	ts_rate.tv_sec = frames / rate;
	ts_rate.tv_nsec = 1000000000 / rate * (frames % rate);

	gettimestamp(&ts);
	/* calculate delay since the last sync */
	difftimespec(&asrs->ts, &ts, &asrs->ts_busy);

	/* maintain constant rate */
	difftimespec(&asrs->ts0, &ts, &ts);
	if (difftimespec(&ts, &ts_rate, &asrs->ts_idle) > 0) {
		nanosleep(&asrs->ts_idle, NULL);
		rv = 1;
	}

	gettimestamp(&asrs->ts);
	return rv;
}

/**
 * Calculate time difference for two time points.
 *
 * @param ts1 Address to the timespec structure providing t1 time point.
 * @param ts2 Address to the timespec structure providing t2 time point.
 * @param ts Address to the timespec structure where the absolute time
 *   difference will be stored.
 * @return This function returns an integer less than, equal to, or greater
 *   than zero, if t2 time point is found to be, respectively, less than,
 *   equal to, or greater than the t1 time point.*/
int difftimespec(
		const struct timespec *ts1,
		const struct timespec *ts2,
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
#endif

