/*
 *  Copyright (C) 2000-2019, Thomas Maier-Komor
 *
 *  This is the source code of mbuffer.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mbconf.h"
#include "common.h"
#include "log.h"
#include "dest.h"
#include "globals.h"
#include "settings.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __sun
#if defined(__SunOS_5_8) || defined(__SunOS_5_9)
int SemWait(sema_t *s)
{
	int err;
	do {
		err = sema_wait(s);
	} while (err == EINTR);
	return err;
}
#endif
#endif


static inline long long timediff(struct timespec *restrict t1, struct timespec *restrict t2)
{
	long long tdiff;
	tdiff = (t1->tv_sec - t2->tv_sec) * 1000000;
	tdiff += (t1->tv_nsec - t2->tv_nsec) / 1000;
	if (tdiff < 0) {
		// Tdiff < 0 if no MONOTONIC clock and time adjust happend.
		// Update time to current time to get a sane restarting
		// point. Timing will transiently be incorrect.
		tdiff = 0;
		t2->tv_sec = t1->tv_sec;
		t2->tv_nsec = t1->tv_nsec;
	}
	return tdiff;
}


/* Thread-safe replacement for usleep. Argument must be a whole
 * number of microseconds to sleep.
 */
int mt_usleep(unsigned long long sleep_usecs)
{
	struct timespec tv;
	tv.tv_sec = sleep_usecs / 1000000;
	tv.tv_nsec = (sleep_usecs % 1000000) * 1000;

	do {
		/* Sleep for the time specified in tv. If interrupted by a
		 * signal, place the remaining time left to sleep back into tv.
		 */
		if (0 == nanosleep(&tv, &tv)) 
			return 0;
	} while (errno == EINTR);
	return -1;
}


long long enforceSpeedLimit(unsigned long long limit, long long num, struct timespec *last)
{
	struct timespec now;
	long long tdiff;
	double dt;
	long self = (long) pthread_self();
	
	num += Blocksize;
	if (num < 0) {
		debugmsg("enforceSpeedLimit(%lld,%lld): thread %ld\n",limit,num,self);
		return num;
	}
	(void) clock_gettime(ClockSrc,&now);
	tdiff = timediff(&now,last);
	dt = (double)tdiff * 1E-6;
	if (((double)num/dt) > (double)limit) {
		double req = (double)num/limit - dt;
		long long w = (long long) (req * 1E6);
		if (w >= TickTime) {
			long long slept, ret;
			(void) mt_usleep(w);
			(void) clock_gettime(ClockSrc,last);
			slept = timediff(last,&now);
			ret = -(long long)((double)limit * (double)(slept-w) * 1E-6);
			debugmsg("thread %ld: slept for %lld usec (planned for %lld), ret = %lld\n",self,slept,w,ret);
			return ret;
		} else {
			debugmsg("thread %ld: request for sleeping %lld usec delayed\n",self,w);
			/* 
			 * Sleeping now would cause too much of a slowdown. So
			 * we defer this sleep until the sleeping time is
			 * longer than the tick time. Like this we can stay as
			 * close to the speed limit as possible.
			 */
			return num;
		}
	}
	debugmsg("thread %ld: %lld/%g (%g) <= %g\n",self,num,dt,num/dt,(double)limit);
	return num;
}


void releaseLock(void *l)
{
	int err = pthread_mutex_unlock((pthread_mutex_t *)l);
	assert(err == 0);
}


void enable_directio(int fd, const char *fn)
{
#ifdef O_DIRECT
	if (0 == fcntl(fd,F_SETFL,fcntl(fd,F_GETFL) | O_DIRECT))
		infomsg("enabled O_DIRECT on %s\n",fn);
	else
		infomsg("could not enable O_DIRECT on %s\n",fn);
#endif
#ifdef __sun
	if (-1 == directio(fd,DIRECTIO_ON))
		infomsg("direct I/O hinting failed for output %s: %s\n",fn,strerror(errno));
#endif
}


int disable_directio(int fd, const char *fn)
{
#ifdef O_DIRECT
	int fl = fcntl(fd,F_GETFL);
	if ((fl & O_DIRECT) == 0) {
		warningmsg("EINVAL without O_DIRECT on %s\n",fn);
		return 0;
	}
	if (0 != fcntl(fd,F_SETFL,fl & ~O_DIRECT)) {
		warningmsg("disabling O_DIRECT on %s failed with %s\n",fn,strerror(errno));
		return 0;
	}
	infomsg("disabled O_DIRECT on %s\n",fn);
	return 1;
#else
	return 0;
#endif
}
