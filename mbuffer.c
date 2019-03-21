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

#ifdef S_SPLINT_S
#ifdef __CYGWIN__
typedef int caddr_t;
#include <sys/_types.h>
#include <cygwin/types.h>
#include <cygwin/in.h>
#endif
#endif

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>

#ifdef __FreeBSD__
#include <sys/vmmeter.h>
#endif

#ifdef HAVE_SENDFILE
#ifdef HAVE_SENDFILE_H
#include <sys/sendfile.h>
#endif
#endif

#ifndef EBADRQC
#define EBADRQC EINVAL
#endif


#include "common.h"
#include "dest.h"
#include "globals.h"
#include "hashing.h"
#include "input.h"
#include "log.h"
#include "network.h"
#include "settings.h"

/* if this sendfile implementation does not support sending from buffers,
   disable sendfile support */
#ifndef SFV_FD_SELF
#ifdef __GNUC__
#warning sendfile is unable to send from buffers
#endif
#undef HAVE_SENDFILE
#endif



static int kb2str(char *s, double v)
{
	const char *dim = "kMGT", *f;

	while (v > 10000.0) {
		v /= 1024.0;
		++dim;
		if (*dim == 0) {
			v *= 1024.0*1024.0*1024.0*1024.0;
			break;
		}
	}
	if (v < 0)
		f = " ??? ";
	else if (v < 100)
		f = "%4.1f %ci";
	else if (v < 10000) {
		v = rint(v);
		f = "%4.0f %ci";
	} else
		f = "%5.lg ";
	return sprintf(s,f,v,*dim);
}


static void summary(unsigned long long numb, int numthreads)
{
	int h,m;
	double secs,av;
	char buf[256], *msg = buf;
	struct timespec now;
	
	(void) clock_gettime(ClockSrc,&now);
	if ((Terminate == 1) && (numthreads == 0))
		numthreads = 1;
	numb >>= 10;
	secs = now.tv_sec - Starttime.tv_sec + (double) now.tv_nsec * 1E-9 - (double) Starttime.tv_nsec * 1E-9;
	if (secs > 0) {
		av = (double)(numb)/secs*numthreads;
		h = (int) secs/3600;
		m = (int) (secs - h * 3600)/60;
		secs -= m * 60 + h * 3600;
	} else if (secs == 0) {
		// System does not have CLOCK_MONOTONIC &
		// time delta is too small.
		h = 0;
		m = 0;
		av = 0;
		secs = 1.0/(double)sysconf(_SC_CLK_TCK);
	} else {
		// System does not have CLOCK_MONOTONIC &
		// time adjust happened.
		h = 0;
		m = 0;
		av = 0;
	}
	if (numthreads > 1)
		msg += sprintf(msg,"summary: %dx ",numthreads);
	else
		msg += sprintf(msg,"summary: ");
	msg += kb2str(msg,numb);
	msg += sprintf(msg,"Byte in ");
	if (h > 0)
		msg += sprintf(msg,"%dh %02dmin %04.1fsec - average of ",h,m,secs);
	else if (m > 0)
		msg += sprintf(msg,"%2dmin %04.1fsec - average of ",m,secs);
	else
		msg += sprintf(msg,"%4.1fsec - average of ",secs);
	msg += kb2str(msg,av);
	msg += sprintf(msg,"B/s");
	if (EmptyCount != 0)
		msg += sprintf(msg,", %dx empty",EmptyCount);
	if (FullCount != 0)
		msg += sprintf(msg,", %dx full",FullCount);
	*msg++ = '\n';
	*msg = '\0';
	if ((Log != STDERR_FILENO) && (StatusLog != 0))
		(void) write(Log,buf,msg-buf);
	if ((Status != 0) && (Quiet == 0))
		(void) write(STDERR_FILENO,buf,msg-buf);
}


static void cancelAll(void)
{
	dest_t *d = Dest;
	do {
		(void) pthread_cancel(d->thread);
		if (d->result == 0)
			d->result = "canceled";
		d = d->next;
	} while (d);
	if (Status)
		(void) pthread_cancel(ReaderThr);
}


static RETSIGTYPE sigHandler(int signr)
{
	switch (signr) {
	case SIGHUP:
	case SIGINT:
		ErrorOccurred = 1;
		Terminate = 1;
		(void) close(In);
		if (TermQ[1] != -1)
			(void) write(TermQ[1],"0",1);
		if (StartWrite > 0)
			(void) pthread_cond_signal(&PercHigh);
		if (StartRead < 1)
			(void) pthread_cond_signal(&PercLow);
		break;
	default:
		(void) raise(SIGABRT);
	}
}


void *watchdogThread(void *ignored)
{
	unsigned long ni = Numin, no = Numout;
	unsigned long long timeout = Timeout * 1000000LL;
	for (;;) {
		mt_usleep(timeout);
		if (Watchdog > 1) {
			errormsg("watchdog timeout: SIGINT had no effect; sending SIGKILL\n");
			kill(getpid(),SIGKILL);
		}
		if ((ni == Numin) && (Finish == -1)) {
			errormsg("watchdog timeout: input stalled; sending SIGINT\n");
			Watchdog = 2;
			kill(getpid(),SIGINT);
		}
		if (no == Numout) {
			errormsg("watchdog timeout: output stalled; sending SIGINT\n");
			Watchdog = 2;
			kill(getpid(),SIGINT);
		}
		ni = Numin;
		no = Numout;
	}
#ifdef __GNUC__
	return 0;	// suppresses a gcc warning
#endif
}


static void statusThread(void) 
{
	struct timespec last, now;
	double in = 0, out = 0, total, diff, fill;
	unsigned long long lin = 0, lout = 0;
	int unwritten = 1;	/* assumption: initially there is at least one unwritten block */
 	fd_set readfds;
 	struct timeval timeout;
	int maxfd = 0;
	long tsec,tusec;
  
	last = Starttime;
	tsec = (long)StatusInterval;
	tusec = (long)((StatusInterval-tsec)*1E6);
	debugmsg("timeout init: %f => %ld : %ld\n",StatusInterval,tsec,tusec);
	if (TermQ[0] != -1)
		maxfd = TermQ[0]+1;
 	while ((Numin == 0) && (Terminate == 0) && (Finish == -1)) {
 		timeout.tv_sec = 0;
 		timeout.tv_usec = 200000;
		FD_ZERO(&readfds);
		if (TermQ[0] != -1)
			FD_SET(TermQ[0],&readfds);
 		switch (select(maxfd,&readfds,0,0,&timeout)) {
 		case 0: continue;
 		case 1: break;
 		case -1:
 			if (errno == EINTR)
 				break;
 		default: abort();
 		}
 	}
	while (!Done) {
		int err,numsender;
		ssize_t nw = 0;
		char buf[256], *b = buf;

 		timeout.tv_sec = tsec;
 		timeout.tv_usec = tusec;
		FD_ZERO(&readfds);
		if (TermQ[0] != -1)
			FD_SET(TermQ[0],&readfds);
 		err = select(maxfd,&readfds,0,0,&timeout);
 		switch (err) {
 		case 0: break;
 		case 1:
			if (Quiet == 0)
				(void) write(STDERR_FILENO,"\n",1);
			return;
 		case -1:
 			if (errno == EINTR)
 				break;
 		default: abort();
 		}
		(void) clock_gettime(ClockSrc,&now);
		diff = now.tv_sec - last.tv_sec + (double) (now.tv_nsec - last.tv_nsec) * 1E-9;
		err = pthread_mutex_lock(&TermMut);
		assert(0 == err);
		err = sem_getvalue(&Buf2Dev,&unwritten);
		assert(0 == err);
		fill = (double)unwritten / (double)Numblocks * 100.0;
		in = (double)(((Numin - lin) * Blocksize) >> 10);
		in /= diff;
		out = (double)(((Numout - lout) * Blocksize) >> 10);
		out /= diff;
		lin = Numin;
		lout = Numout;
		last = now;
		total = (double)((Numout * Blocksize) >> 10);
		fill = (fill < 0.0) ? 0.0 : fill;
		b += sprintf(b,"\rin @ ");
		b += kb2str(b,in);
		numsender = NumSenders + MainOutOK - Hashers;
		b += sprintf(b,"B/s, out @ ");
		b += kb2str(b, out * numsender);
		if (numsender != 1)
			b += sprintf(b,"B/s, %d x ",numsender);
		else
			b += sprintf(b,"B/s, ");
		b += kb2str(b,total);
		b += sprintf(b,"B total, buffer %3.0f%% full",fill);
		if (InSize != 0) {
			double done = (double)Numout*Blocksize/(double)InSize*100;
			b += sprintf(b,", %3.0f%% done",done);
		}
		if (Quiet == 0) {
#ifdef NEED_IO_INTERLOCK
			if (Log == STDERR_FILENO) {
				int e;
				e = pthread_mutex_lock(&LogMut);
				assert(e == 0);
				nw = write(STDERR_FILENO,buf,strlen(buf));
				e = pthread_mutex_unlock(&LogMut);
				assert(e == 0);
			} else
#endif
				nw = write(STDERR_FILENO,buf,strlen(buf));
		}
		if ((StatusLog != 0) && (Log != STDERR_FILENO))
			statusmsg("%s\n",buf+1);
		err = pthread_mutex_unlock(&TermMut);
		assert(0 == err);
		if (nw == -1)	/* stop trying to print status messages after a write error */
			break;
	}
}


int syncSenders(char *b, int s)
{
	static volatile int size = 0, skipped = 0;
	static char *volatile buf = 0;
	int err;

	err = pthread_mutex_lock(&SendMut);
	assert(err == 0);
	if (b) {
		buf = b;
		size = s;
	}
	if (s < 0)
		--NumSenders;
	if (--ActSenders) {
		debugiomsg("syncSenders(%p,%d): ActSenders = %d\n",b,s,ActSenders);
		pthread_cleanup_push(releaseLock,&SendMut);
		err = pthread_cond_wait(&SendCond,&SendMut);
		assert(err == 0);
		pthread_cleanup_pop(1);
		debugiomsg("syncSenders(): continue\n");
		return 0;
	} else {
		ActSenders = NumSenders + 1;
		assert((buf != 0) || Terminate);
		SendAt = buf;
		SendSize = size;
		buf = 0;
		if (skipped) {
			// after the first time, always give a buffer free after sync
			err = sem_post(&Dev2Buf);
			assert(err == 0);
		} else {
			// the first time no buffer has been given free
			skipped = 1;
		}
		err = pthread_mutex_unlock(&SendMut);
		assert(err == 0);
		debugiomsg("syncSenders(): send %d@%p, BROADCAST\n",SendSize,SendAt);
		err = pthread_cond_broadcast(&SendCond);
		assert(err == 0);
		return 1;
	}
}



static inline void terminateSender(int fd, dest_t *d, intptr_t ret)
{
	debugmsg("terminating operation on %s\n",d->arg);
	if (-1 != fd) {
		int err;
		infomsg("syncing %s...\n",d->arg);
		do 
			err = fsync(fd);
		while ((err != 0) && (errno == EINTR));
		if (err != 0) {
			if ((errno == EINVAL) || (errno == EBADRQC)) {
				infomsg("syncing unsupported on %s: omitted.\n",d->arg);
			} else {
				warningmsg("unable to sync %s: %s\n",d->arg,strerror(errno));
			}
		}
		if (-1 == close(fd))
			errormsg("error closing file %s: %s\n",d->arg,strerror(errno));
	}
	if (ret != 0) {
		ret = syncSenders(0,-1);
		debugmsg("terminateSender(%s): sendSender(0,-1) = %d\n",d->arg,ret);
	}
	pthread_exit((void *) ret);
}



static void *senderThread(void *arg)
{
	unsigned long long outsize = Blocksize;
	dest_t *dest = (dest_t *)arg;
	int out = dest->fd;
#ifdef HAVE_SENDFILE
	int sendout = 1;
#endif
#ifdef HAVE_STRUCT_STAT_ST_BLKSIZE
	struct stat st;

	debugmsg("sender(%s): checking output device...\n",dest->arg);
	if (-1 == fstat(out,&st))
		warningmsg("could not stat output %s: %s\n",dest->arg,strerror(errno));
	else if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
		infomsg("blocksize is %d bytes on output device\n",st.st_blksize);
		if ((Blocksize < st.st_blksize) || (Blocksize % st.st_blksize != 0)) {
			warningmsg("Blocksize should be a multiple of the blocksize of the output device!\n"
				"This can cause problems with some device/OS combinations...\n"
				"Blocksize on output device %s is %d (transfer block size is %lld)\n", dest->arg, st.st_blksize, Blocksize);
			if (SetOutsize) {
				errormsg("unable to set output blocksize\n");
				dest->result = strerror(errno);
				terminateSender(out,dest,1);
			}
		} else {
			if (SetOutsize) {
				infomsg("setting output blocksize to %d\n",st.st_blksize);
				outsize = st.st_blksize;
			}
		}
	} else
		infomsg("no device on output stream %s\n",dest->arg);
#endif
	debugmsg("sender(%s): starting...\n",dest->arg);
	for (;;) {
		int size, num = 0;
		(void) syncSenders(0,0);
		size = SendSize;
		if (0 == size) {
			debugmsg("senderThread(\"%s\"): done.\n",dest->arg);
			terminateSender(out,dest,0);
			return 0;	/* for lint */
		}
		if (Terminate) {
			infomsg("senderThread(\"%s\"): terminating early upon request...\n",dest->arg);
			dest->result = "canceled";
			terminateSender(out,dest,1);
		}
		do {
			unsigned long long rest = size - num;
			int ret;
			assert(size >= num);
#ifdef HAVE_SENDFILE
			if (sendout) {
				off_t baddr = (off_t) (SendAt+num);
				unsigned long long n = SetOutsize ? (rest > Outsize ? (rest/Outsize)*Outsize : rest) : rest;
				ret = sendfile(out,SFV_FD_SELF,&baddr,n);
				debugiomsg("sender(%s): sendfile(%d, SFV_FD_SELF, &%p, %llu) = %d\n", dest->arg, dest->fd, (void*)baddr, n, ret);
				if ((ret == -1) && ((errno == EINVAL) || (errno == EOPNOTSUPP))) {
					sendout = 0;
					debugmsg("sender(%s): sendfile unsupported - falling back to write\n", dest->arg);
					continue;
				}
			} else
#endif
			{
				char *baddr = SendAt+num;
				ret = write(out,baddr,rest > outsize ? outsize :rest);
				debugiomsg("sender(%s): writing %llu@0x%p: ret = %d\n",dest->arg,rest,(void*)baddr,ret);
			}
			if (-1 == ret) {
				if (errno == EINTR)
					continue;
				if ((errno == EINVAL) && disable_directio(out,dest->arg))
					continue;
				errormsg("error writing to %s: %s\n",dest->arg,strerror(errno));
				dest->result = strerror(errno);
				terminateSender(out,dest,1);
			}
			num += ret;
		} while (num != size);
	}
}



static int requestOutputVolume(int out, const char *outfile)
{
	static struct timespec volstart = {0,0};
	struct timespec now;
	double diff;
	unsigned min,hr;

	if (!outfile) {
		errormsg("End of volume, but not end of input:\n"
			"Output file must be given (option -o) for multi volume support!\n");
		return -1;
	}
	infomsg("end of volume - last block on volume: %lld\n",Numout);
	(void) clock_gettime(ClockSrc,&now);
	if (volstart.tv_sec) 
		diff = now.tv_sec - volstart.tv_sec + (double) (now.tv_nsec - volstart.tv_nsec) * 1E-9;
	else
		diff = now.tv_sec - Starttime.tv_sec + (double) (now.tv_nsec - Starttime.tv_nsec) * 1E-9;
	if (diff > 3600) {
		hr = (unsigned) (diff / 3600);
		diff -= hr * 3600;
		min = (unsigned) (diff / 60);
		diff -= min * 60;
		infomsg("time for writing volume: %u:%02u:%02f\n",hr,min,diff);
	} else if (diff > 60) {
		min = (unsigned) (diff / 60);
		diff -= min * 60;
		infomsg("time for writing volume: %02u:%02f\n",min,diff);
	} else
		infomsg("time for writing volume: %02fsec.\n",diff);
	if (-1 == close(out))
		errormsg("error closing output %s: %s\n",outfile,strerror(errno));
	do {
		mode_t mode;
		if (Autoloader) {
			const char default_cmd[] = "mt -f %s offline";
			char cmd_buf[sizeof(default_cmd)+strlen(outfile)];
			const char *cmd = AutoloadCmd;
			int err;

			if (cmd == 0) {
				(void) snprintf(cmd_buf, sizeof(cmd_buf), default_cmd, Infile);
				cmd = cmd_buf;
			}
			infomsg("requesting new output volume with command '%s'\n",cmd);
			err = system(cmd);
			if (0 < err) {
				errormsg("error running \"%s\" to change volume in autoloader - exitcode %d\n", cmd, err);
				Autoloader = 0;
				return -1;
			} else if (0 > err) {
				errormsg("error starting \"%s\" to change volume in autoloader: %s\n", cmd, strerror(errno));
				Autoloader = 0;
				return -1;
			}
			if (AutoloadTime) {
				infomsg("waiting for drive to get ready...\n");
				(void) sleep(AutoloadTime);
			}
		} else {
			int err;
			char c = 0, msg[] = "\nvolume full - insert new media and press return when ready...\n";
			if (Terminal == 0) {
				errormsg("End of volume, but not end of input.\n"
					"Specify an autoload command, if you are working without terminal.\n");
				return -1;
			}
			err = pthread_mutex_lock(&TermMut);
			assert(0 == err);
			if (-1 == write(STDERR_FILENO,msg,sizeof(msg))) {
				errormsg("error accessing controlling terminal for manual volume change request: %s\nConsider using autoload option, when running mbuffer without terminal.\n",strerror(errno));
				return -1;
			}
			do {
				if (-1 == read(STDERR_FILENO,&c,1) && (errno != EINTR)) {
					errormsg("error accessing controlling terminal for manual volume change request: %s\nConsider using autoload option, when running mbuffer without terminal.\n",strerror(errno));
					return -1;
				}
			} while (c != '\n');
			err = pthread_mutex_unlock(&TermMut);
			assert(0 == err);
		}
		mode = O_WRONLY|O_TRUNC|OptSync|O_LARGEFILE;
		if (strncmp(outfile,"/dev/",5))
			mode |= O_CREAT;
		out = open(outfile,mode,0666);
		if (-1 == out)
			errormsg("error reopening output file: %s\n",strerror(errno));
		enable_directio(out,outfile);
	} while (-1 == out);
	(void) clock_gettime(ClockSrc,&volstart);
	diff = volstart.tv_sec - now.tv_sec + (double) (volstart.tv_nsec - now.tv_nsec) * 1E-9;
	infomsg("tape-change took %fsec. - continuing with next volume\n",diff);
	if (Terminal && ! Autoloader) {
		char msg[] = "\nOK - continuing...\n";
		(void) write(STDERR_FILENO,msg,sizeof(msg));
	}
	return out;
}



static void terminateOutputThread(dest_t *d, int status)
{
	int err;

	infomsg("outputThread: syncing %s...\n",d->arg);
	do 
		err = fsync(d->fd);
	while ((err != 0) && (errno == EINTR));
	if (err != 0) {
		if ((errno == EINVAL) || (errno == EBADRQC)) {
			infomsg("syncing unsupported on %s: omitted.\n",d->arg);
		} else {
			warningmsg("unable to sync %s: %s\n",d->arg,strerror(errno));
		}
	}
	infomsg("outputThread: finished - exiting...\n");
	if (-1 == close(d->fd))
		errormsg("error closing %s: %s\n",d->arg,strerror(errno));
	if (TermQ[1] != -1) {
		err = write(TermQ[1],"0",1);
		if (err == -1)
			errormsg("error writing to termination queue: %s\n",strerror(errno));
	}
	if (status) {
		(void) sem_post(&Dev2Buf);
		(void) pthread_cond_broadcast(&SendCond);
	}
	Done = 1;
	pthread_exit((void *)(ptrdiff_t) status);
}



static void *outputThread(void *arg)
{
	dest_t *dest = (dest_t *) arg;
	unsigned at = 0;
	int fill = 0, haderror = 0, out, multipleSenders;
#ifdef HAVE_SENDFILE
	int sendout = 1;
#endif
	int countENOSPC = 0, tapeEWEOM = 0; /* Early Warning End Of Media */
	unsigned long long blocksize = Blocksize;
	long long xfer = 0;
	struct timespec last;

	assert(NumSenders >= 0);
	if (dest->next) {
		int ret;
		dest_t *d = dest->next;
		debugmsg("NumSenders = %d\n",NumSenders);
		ActSenders = NumSenders + 1;
		ret = pthread_mutex_init(&SendMut,0);
		assert(ret == 0);
		ret = pthread_cond_init(&SendCond,0);
		assert(ret == 0);
		do {
			if (d->arg == 0) {
				debugmsg("creating hash thread with algorithm %s\n",d->name);
				ret = pthread_create(&d->thread,0,hashThread,d);
				assert(ret == 0);
			} else if (d->fd != -1) {
				debugmsg("creating sender for %s\n",d->arg);
				ret = pthread_create(&d->thread,0,senderThread,d);
				assert(ret == 0);
			} else {
				debugmsg("outputThread: ignoring destination %s\n",d->arg);
				d->name = 0;
			}
			d = d->next;
		} while (d);
	}
	multipleSenders = (NumSenders > 0);
	dest->result = 0;
	out = dest->fd;
	if ((StartWrite > 0) && (Finish == -1)) {
		int err;
		debugmsg("outputThread: delaying start until buffer reaches high watermark\n");
		err = pthread_mutex_lock(&HighMut);
		assert(err == 0);
		pthread_cleanup_push(releaseLock,&HighMut);
		err = pthread_cond_wait(&PercHigh,&HighMut);
		assert(err == 0);
		pthread_cleanup_pop(0);
		err = pthread_mutex_unlock(&HighMut);
		assert(err == 0);
		debugmsg("outputThread: high watermark reached, starting...\n");
	} else
		infomsg("outputThread: starting output on %s...\n",dest->arg);
	/* initialize last to 0, because we don't want to wait initially */
	(void) clock_gettime(ClockSrc,&last);
	for (;;) {
		unsigned long long rest = blocksize;
		int err;

		if ((StartWrite > 0) && (fill <= 0)) {
			assert(fill == 0);
			err = pthread_mutex_lock(&HighMut);
			assert(err == 0);
			err = sem_getvalue(&Buf2Dev,&fill);
			assert(err == 0);
			if (fill == 0) {
				debugmsg("outputThread: buffer empty, waiting for it to fill\n");
				pthread_cleanup_push(releaseLock,&HighMut);
				err = pthread_cond_wait(&PercHigh,&HighMut);
				assert(err == 0);
				pthread_cleanup_pop(0);
				++EmptyCount;
				debugmsg("outputThread: high watermark reached, continuing...\n");
				(void) clock_gettime(ClockSrc,&last);
			}
			err = pthread_mutex_unlock(&HighMut);
			assert(err == 0);
		} else
			--fill;
		err = sem_wait(&Buf2Dev);
		assert(err == 0);
		if (Terminate) {
			infomsg("outputThread: terminating upon termination request...\n");
			dest->result = "canceled";
			terminateOutputThread(dest,1);
		}
		if (Finish == at) {
			err = sem_getvalue(&Buf2Dev,&fill);
			assert(err == 0);
			if ((fill == 0) && (0 == Rest)) {
				if (multipleSenders)
					(void) syncSenders((char*)0xdeadbeef,0);
				infomsg("outputThread: finished - exiting...\n");
				terminateOutputThread(dest,haderror);
			} else {
				blocksize = rest = Rest;
				debugmsg("outputThread: last block has %llu bytes\n",(unsigned long long)Rest);
			}
		}
		if (multipleSenders)
			(void) syncSenders(Buffer[at],blocksize);
		/* switch output volume if -D <size> has been reached */
		if ( (OutVolsize != 0) && (Numout > 0) && (Numout % (OutVolsize/Blocksize)) == 0 ) {
			/* Sleep to let status thread "catch up" so that the displayed total is a multiple of OutVolsize */
			(void) mt_usleep(500000);
			out = requestOutputVolume(out,dest->name);
			if (out == -1) {
				haderror = 1;
				dest->result = strerror(errno);
			}
		}
		do {
			/* use Outsize which could be the blocksize of the device (option -d) */
			unsigned long long n = rest > Outsize ? Outsize : rest;
			int num;
			if (haderror) {
				if (NumSenders == 0)
					Terminate = 1;
				num = (int)rest;
			} else
#ifdef HAVE_SENDFILE
			if (sendout) {
				off_t baddr = (off_t) (Buffer[at] + blocksize - rest);
				num = sendfile(out,SFV_FD_SELF,&baddr,n);
				debugiomsg("outputThread: sendfile(%d, SFV_FD_SELF, &(Buffer[%d] + %llu), %llu) = %d\n", out, at, blocksize - rest, n, num);
				if ((num == -1) && ((errno == EOPNOTSUPP) || (errno == EINVAL))) {
					infomsg("sendfile not supported - falling back to write...\n");
					sendout = 0;
					continue;
				}
			} else
#endif
			{
				num = write(out,Buffer[at] + blocksize - rest, n);
				debugiomsg("outputThread: writing %lld@0x%p: ret = %d\n", n, Buffer[at] + blocksize - rest, num);
			}
			if (TapeAware) {
				if ((num == 0) || ((num < 0) && (errno == ENOSPC))) {
					countENOSPC++;
					/* Got one ENOSPC, try again to see if we get another */
					if (countENOSPC <= 1) continue;
					/* Otherwise we got more than one ENOSPC in a row, it's really the
					   end of the tape. */
				}
				if (countENOSPC > 0) {
					/* We got an ENOSPC followed by something that wasn't another ENOSPC
					   (probably no error at all), so this means we have received the
					   Early Warning End Of Media notification. */
					if (!tapeEWEOM) {
						infomsg("end of media approaching\n");
						/* We receive the EWEOM signal continuously but we only want to
						   display the message once. */
						tapeEWEOM = 1;
					}
				}
				/* Reset the error count - we either got no error this time or we had
				   two so the error logic below will handle a tape change. */
				countENOSPC = 0;
			}
			if (Terminal||Autoloader) {
				if (((-1 == num) && ((errno == ENOMEM) || (errno == ENOSPC)))
					|| (0 == num)) {
					/* request a new volume */
					out = requestOutputVolume(out,dest->name);
					if (out == -1)
						haderror = 1;
					tapeEWEOM = 0; /* No longer at end of tape */
					continue;
				}
			}
			if (-1 == num) {
				if ((errno == EINVAL) && disable_directio(out,dest->arg))
					continue;
				if (errno == EINTR)
					continue;
				dest->result = strerror(errno);
				errormsg("outputThread: error writing to %s at offset 0x%llx: %s\n",dest->arg,(long long)Blocksize*Numout+blocksize-rest,strerror(errno));
				MainOutOK = 0;
				if (NumSenders == 0) {
					debugmsg("outputThread: terminating...\n");
					Terminate = 1;
					err = sem_post(&Dev2Buf);
					assert(err == 0);
					terminateOutputThread(dest,1);
				}
				debugmsg("outputThread: %d senders remaining - continuing...\n",NumSenders);
				haderror = 1;
			}
			rest -= num;
		} while (rest > 0);
		if (multipleSenders == 0) {
			err = sem_post(&Dev2Buf);
			assert(err == 0);
		}
		if (MaxWriteSpeed)
			xfer = enforceSpeedLimit(MaxWriteSpeed,xfer,&last);
		if (Pause)
			(void) mt_usleep(Pause);
		if (Finish == at) {
			err = sem_getvalue(&Buf2Dev,&fill);
			assert(err == 0);
			if (fill == 0) {
				if (multipleSenders)
					(void) syncSenders((char*)0xdeadbeef,0);
				terminateOutputThread(dest,0);
				return 0;	/* make lint happy */
			}
		}
		if (Numblocks == ++at)
			at = 0;
		if (StartRead < 1) {
			err = pthread_mutex_lock(&LowMut);
			assert(err == 0);
			err = sem_getvalue(&Buf2Dev,&fill);
			assert(err == 0);
			if (((double)fill / (double)Numblocks) < StartRead) {
				err = pthread_cond_signal(&PercLow);
				assert(err == 0);
			}
			err = pthread_mutex_unlock(&LowMut);
			assert(err == 0);
		}
		Numout++;
	}
}



static void openDestinationFiles(dest_t *d)
{
	unsigned errs = ErrorOccurred;
	while (d) {
		if ((d->fd == -1) && (d->name != 0)) {
			if (0 == strncmp(d->arg,"/dev/",5))
				d->mode &= ~O_EXCL;
			d->fd = open(d->arg,d->mode,0666);
			if ((-1 == d->fd) && (errno == EINVAL) && (d->mode & O_LARGEFILE)) {
				warningmsg("open of %s failed with EINVAL, retrying without O_LARGEFILE\n",d->arg);
				d->mode &= ~O_LARGEFILE;
				d->fd = open(d->arg,d->mode,0666);
				if (d->fd == -1)
					d->mode |= O_LARGEFILE;
				else
					warningmsg("opened of %s without O_LARGEFILE\n",d->arg);
			}
			if (-1 == d->fd) {
				d->result = strerror(errno);
				errormsg("unable to open output %s: %s\n",d->arg,strerror(errno));
			} else {
				++NumSenders;
				debugmsg("successfully opened destination file %s with fd %d\n",d->arg,d->fd);
				enable_directio(d->fd,d->arg);
			}
		}
		if (-1 == d->fd)
			d->name = 0;	/* tag destination as unstartable */
#ifdef __sun
		else if (d->arg) {
			if (0 == directio(d->fd,DIRECTIO_ON))
				infomsg("direct I/O hinting enabled for output to %s\n",d->arg);
			else
				infomsg("direct I/O hinting failed for output to %s: %s\n",d->arg,strerror(errno));
		}
#endif
		d = d->next;
	}
	if (ErrorOccurred != errs)
		warningmsg("unable to open all outputs\n");
}



static int joinSenders()
{
	dest_t *d = Dest;
	int numthreads = 0;

	if (d == 0) 
		return 0;
	infomsg("waiting for senders...\n");
	if (Terminate)
		cancelAll();
	do {
		if (d->name) {
			int ret;
			void *status;
			if (d->arg) {
				debugmsg("joining sender for %s\n",d->arg);
			} else {
				debugmsg("joining hasher for %s\n",d->name);
			}
			ret = pthread_join(d->thread,&status);
			if (ret != 0)
				errormsg("error joining %s: %s\n",d->arg,d->name,strerror(errno));
			if (status == 0)
				++numthreads;
		}
		d = d->next;
	} while (d);
	return numthreads;
}


static void reportSenders()
{
	dest_t *d = Dest;
	while (d) {
		dest_t *n = d->next;
		if (d->result) {
			/* some result to output */
			if (d->arg) {
				/* destination had a problem */
				warningmsg("error during output to %s: %s\n",d->arg,d->result);
			} else {
				/* some generic result - e.g. MD5 hash */
				(void) write(STDERR_FILENO,d->result,strlen(d->result));
				if (Log != STDERR_FILENO)
					(void) write(Log,d->result,strlen(d->result));
			}
		}
		free(d);
		d = n;
	}
}


static void initDefaults()
{
	/* gather system parameters */
	TickTime = 1000000 / sysconf(_SC_CLK_TCK);

	/* get page size */
#ifdef _SC_PAGESIZE
	PgSz = sysconf(_SC_PAGESIZE);
	if (PgSz < 0) {
		warningmsg("unable to determine system pagesize: %s\n",strerror(errno));
		PgSz = 0;
	}
#endif

	/* get physical memory size */
#if defined(_SC_PHYS_PAGES)
	NumP = sysconf(_SC_PHYS_PAGES);
	if (NumP < 0) {
		warningmsg("unable to determine number of total memory pages: %s\n",strerror(errno));
		NumP = 0;
	} else {
		debugmsg("Physical memory (in pages) : %li\n",NumP);
	}
#endif

	/* get number of available free pages */
	AvP = 0;
#if defined(__linux)
	int pm = open("/proc/meminfo",O_RDONLY);
	if (pm != -1) {
		char tmp[4096];
		int n = read(pm,tmp,sizeof(tmp));
		if (n > 0) {
			char *at = strstr(tmp,"MemAvailable:");
			if (at) {
				AvP = strtol(at+13,0,0);
				AvP <<= 10;
				AvP /= PgSz;
				debugmsg("available memory: %lu pages\n",AvP);
			}
		}
		close(pm);
	}
	if (AvP == 0)
		warningmsg("unable to determine amount of available memory\n");
#elif defined(_SC_AVPHYS_PAGES)
	AvP = sysconf(_SC_AVPHYS_PAGES);
	if (AvP < 0) {
		warningmsg("unable to determine number of available pages: %s\n",strerror(errno));
		AvP = 0;
	}
#elif defined(__FreeBSD__)
	struct vmtotal vmt;
	size_t vmt_size = sizeof(vmt);
	if ((sysctlbyname("vm.vmtotal", &vmt, &vmt_size, NULL, 0) < 0) || (vmt_size != sizeof(vmt))) {
		warningmsg("unable to determine number of available pages: %s\n",strerror(errno));
	} else {
		AvP = vmt.t_free;
	}
#else
	warningmsg("no mechanism to determine number of available pages\n",strerror(errno));
#endif
	if (AvP && PgSz) {
		debugmsg("available memory: %llukB / %li pages\n",((long long unsigned)AvP*(long long unsigned)PgSz)>>10,AvP);
	} else if (AvP) {
		debugmsg("available memory: %li pages\n",AvP);
	}

	if (NumP && PgSz) {
		debugmsg("virtual memory: %llukB / %li pages\n",((long long unsigned)NumP*(long long unsigned)PgSz)>>10,NumP);
		Blocksize = PgSz;
		debugmsg("Blocksize set to physical page size of %ld bytes\n",PgSz);
		Numblocks = NumP/50;
		long mxsemv = maxSemValue();
		while ((Numblocks > mxsemv) || (Numblocks > 200)) {
			Numblocks >>= 1;
			Blocksize <<= 1;
		}
		debugmsg("default Numblocks = %lu, default Blocksize = %llu\n",Numblocks,Blocksize);
	}
	Outsize = Blocksize;

#if defined(_POSIX_MONOTONIC_CLOCK) && (_POSIX_MONOTONIC_CLOCK >= 0) && defined(CLOCK_MONOTONIC)
	if (sysconf(_SC_MONOTONIC_CLOCK) > 0)
		ClockSrc = CLOCK_MONOTONIC;
#endif

	const char *home = getenv("HOME");
	readConfigFile("/etc/mbuffer.rc");
	readConfigFile(PREFIX "/etc/mbuffer.rc");
	if (home == 0) {
		warningmsg("HOME environment variable not set - unable to find defaults file\n");
		return;
	}
	size_t l = strlen(home);
	char dfname[PATH_MAX+1];
	if (l + 13 > sizeof(dfname)) {
		warningmsg("path to defaults file breaks PATH_MAX\n");
		return;
	}
	memcpy(dfname,home,l);
	if (dfname[l-1] != '/')
		dfname[l++] = '/';
	memcpy(dfname+l,".mbuffer.rc",12);
	readConfigFile(dfname);
}


static void checkBlocksizes(dest_t *dest)
{
#ifdef HAVE_STRUCT_STAT_ST_BLKSIZE
	struct stat st;

	debugmsg("checking output device...\n");
	if (-1 == fstat(dest->fd,&st))
		errormsg("could not stat output: %s\n",strerror(errno));
	else if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
		if (Blocksize % st.st_blksize != 0) {
			warningmsg("Block size is not a multiple of native output size.\n");
			infomsg("output device's native block-size is %d bytes\n",st.st_blksize);
			infomsg("transfer block size is %lld\n", Blocksize);
			if (SetOutsize)
				fatal("unable to set output blocksize\n");
		} else {
			infomsg("output device's native block-size is %d bytes\n",st.st_blksize);
			if (SetOutsize) {
				infomsg("setting output blocksize to %d\n",st.st_blksize);
				Outsize = st.st_blksize;
			}
		}
	} else
		infomsg("no device on output stream\n");
	debugmsg("checking input device...\n");
	if (-1 == fstat(In,&st))
		warningmsg("could not stat input: %s\n",strerror(errno));
	else if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
		IDevBSize = st.st_blksize;
		if ((st.st_blksize != 0) && (Blocksize % st.st_blksize != 0)) {
			warningmsg("Block size is not a multiple of native input size.\n");
			infomsg("input device's native block-size is %d bytes\n",st.st_blksize);
			infomsg("transfer block size is %lld\n", Blocksize);
		} else {
			infomsg("input device's native block-size is %d bytes\n",st.st_blksize);
		}
	} else
		infomsg("no device on input stream\n");
#else
	warningmsg("Could not stat output device (unsupported by system)!\n"
		   "This can result in incorrect written data when\n"
		   "using multiple volumes. Continue at your own risk!\n");
#endif
}


static int outputIsSet()
{
	dest_t *d = Dest;
	while (d) {
		if (d->fd != -2) {
			debugmsg("outputIsSet: %d\n",d->fd);
			return 1;
		}
		d = d->next;
	}
	debugmsg("no output is set\n");
	return 0;
}


int main(int argc, const char **argv)
{
	int c, fl, err;
	sigset_t signalSet;
	char *argv0 = strdup(argv[0]), *progname, null;
	struct sigaction sig;
	dest_t *dest = 0;

	/* setup logging prefix */
	progname = basename(argv0);
	PrefixLen = strlen(progname);
	Prefix = malloc(PrefixLen + 2);
	(void) strncpy(Prefix,progname,PrefixLen);
	Prefix[PrefixLen++] = ':';
	Prefix[PrefixLen++] = ' ';

	/* set verbose level before parsing defaults and options */
	searchOptionV(argc,argv);

	/* setup parameters */
	initDefaults();
	debugmsg("default buffer set to %d blocks of %lld bytes\n",Numblocks,Blocksize);
	for (c = 1; c < argc; c++)
		c = parseOption(c,argc,argv);

	/* consistency check for options */
	if (AutoloadTime && Timeout && Timeout <= AutoloadTime)
		fatal("autoload time must be smaller than watchdog timeout\n");
	if (Options == (OPTION_B|OPTION_M|OPTION_S)) {
		/* options -m -b -s set */
		if (Numblocks * Blocksize != Totalmem)
			fatal("inconsistent options: blocksize * number of blocks != totalsize!\n");
	} else if ((Options == (OPTION_S|OPTION_M)) || (Options == OPTION_M)) {
		if (Totalmem < (Blocksize*5))
			fatal("total memory must be large enough for 5 blocks\n");
		Numblocks = Totalmem / Blocksize;
		infomsg("Numblocks = %llu, Blocksize = %llu, Totalmem = %llu\n",(unsigned long long)Numblocks,(unsigned long long)Blocksize,(unsigned long long)Totalmem);
	} else if (Options == (OPTION_B|OPTION_M)) {
		if (Blocksize == 0)
			fatal("blocksize must be greater than 0\n");
		if (Totalmem <= Blocksize)
			fatal("total memory must be larger than block size\n");
		Blocksize = Totalmem / Numblocks;
		infomsg("blocksize = %llu\n",(unsigned long long)Blocksize);
	}
	if ((StartRead < 1) && (StartWrite > 0))
		fatal("setting both low watermark and high watermark doesn't make any sense...\n");
	if ((NumSenders-Hashers > 0) && (Autoloader || OutVolsize))
		fatal("multi-volume support is unsupported with multiple outputs\n");
	if (Autoloader) {
		if ((!OutFile) && (!Infile))
			fatal("Setting autoloader time or command without using a device doesn't make any sense!\n");
		if (OutFile && Infile) {
			fatal("Which one is your autoloader? Input or output? Replace input or output with a pipe.\n");
		}
	}

	/* multi volume input consistency checking */
	if ((NumVolumes != 1) && (!Infile))
		fatal("multi volume support for input needs an explicit given input device (option -i)\n");

	/* SPW: Volsize consistency checking */
	if (OutVolsize && !OutFile)
		fatal("Setting OutVolsize without an output device doesn't make sense!\n");
	if ((OutVolsize != 0) && (OutVolsize < Blocksize))
		/* code assumes we can write at least one block */
		fatal("If non-zero, OutVolsize must be at least as large as the buffer blocksize (%llu)!\n",Blocksize);
	/* SPW END */

	if (Numblocks < 5)
		fatal("Minimum block count is 5.\n");

	initBuffer();

	debugmsg("creating semaphores...\n");
	if (0 != sem_init(&Buf2Dev,0,0))
		fatal("Error creating semaphore Buf2Dev: %s\n",strerror(errno));
	if (0 != sem_init(&Dev2Buf,0,Numblocks))
		fatal("Error creating semaphore Dev2Buf: %s\n",strerror(errno));

	if (Infile)
		openInput();
	if (In == -1) {
		debugmsg("input is stdin\n");
		In = STDIN_FILENO;
	}
	if (!outputIsSet()) {
		debugmsg("no output set - adding stdout as destination\n");
		dest_t *d = malloc(sizeof(dest_t));
		d->fd = dup(STDOUT_FILENO);
		err = dup2(STDERR_FILENO,STDOUT_FILENO);
		assert(err != -1);
		d->name = "<stdout>";
		d->arg = "<stdout>";
		d->port = 0;
		d->result = 0;
		bzero(&d->thread,sizeof(d->thread));
		d->next = Dest;
		Dest = d;
		++NumSenders;
	}
	openDestinationFiles(Dest);
	if (NumSenders == -1)
		fatal("no output left - nothing to do\n");

	sig.sa_handler = SIG_IGN;
	sigemptyset(&sig.sa_mask);
	sig.sa_flags = 0;
	err = sigaction(SIGTTIN,&sig,0);
	assert(err == 0);
	fl = fcntl(STDERR_FILENO,F_GETFL);
	err = fcntl(STDERR_FILENO,F_SETFL,fl | O_NONBLOCK);
	assert(err == 0);
	if ((read(STDERR_FILENO,&c,1) != -1) || (errno == EAGAIN)) {
		Terminal = 1;
	} else {
		int tty = open("/dev/tty",O_RDWR);
		if (-1 == tty) {
			Terminal = 0;
			if ((Autoloader == 0) && (OutFile))
				warningmsg("No controlling terminal and no autoloader command specified.\n");
		} else {
			Terminal = 1;
			err = dup2(tty,STDERR_FILENO);
			assert(err != -1);
		}
	}
	debugmsg(Terminal ? "found controlling terminal\n" : "no access to controlling terminal available\n");
	err = fcntl(STDERR_FILENO,F_SETFL,fl);
	assert(err == 0);
	if ((Terminal == 1) && (NumVolumes != 1)) {
		struct termios tset;
		if (-1 == tcgetattr(STDERR_FILENO,&tset)) {
			warningmsg("unable to get terminal attributes: %s\n",strerror(errno));
		} else {
			tset.c_lflag &= (~ICANON) & (~ECHO);
			tset.c_cc[VTIME] = 0;
			tset.c_cc[VMIN] = 1;
			if (-1 == tcsetattr(STDERR_FILENO,TCSANOW,&tset))
				warningmsg("unable to set terminal attributes: %s\n",strerror(errno));
		}
	}

	debugmsg("registering signals...\n");
	sig.sa_handler = sigHandler;
	err = sigemptyset(&sig.sa_mask);
	assert(err == 0);
	err = sigaddset(&sig.sa_mask,SIGINT);
	assert(err == 0);
	sig.sa_flags = SA_RESTART;
	if (0 != sigaction(SIGINT,&sig,0))
		warningmsg("error registering new SIGINT handler: %s\n",strerror(errno));
	err = sigemptyset(&sig.sa_mask);
	assert(err == 0);
	err = sigaddset(&sig.sa_mask,SIGHUP);
	assert(err == 0);
	if (0 != sigaction(SIGHUP,&sig,0))
		warningmsg("error registering new SIGHUP handler: %s\n",strerror(errno));

	debugmsg("starting threads...\n");
	(void) clock_gettime(ClockSrc,&Starttime);
	err = sigfillset(&signalSet);
	assert(0 == err);
	(void) pthread_sigmask(SIG_BLOCK, &signalSet, NULL);

	/* select destination for output thread */
	dest = Dest;
	while (dest && dest->fd < 0) {
		/* -1: file failed to open or network failed to conenct
		 * -2: hash target
		 */
		debugmsg("skipping destination %s\n",dest->name);
		dest->name = 0;
		dest = dest->next;
	}

	if (dest)
		checkBlocksizes(dest);

	if (((Verbose < 3) || (StatusLog == 0)) && (Quiet != 0))
		Status = 0;
	if (Status) {
		if (-1 == pipe(TermQ))
			fatal("could not create termination pipe: %s\n",strerror(errno));
	} else {
		TermQ[0] = -1;
		TermQ[1] = -1;
	}
	infomsg("%u senders, %u hashers\n",NumSenders,Hashers);
	if ((Watchdog == 0) && (Timeout != 0)) {
		err = pthread_create(&WatchdogThr,0,&watchdogThread,(void*)0);
		assert(0 == err);
		infomsg("started watchdog with Timeout = %lu sec.\n",Timeout);
	}
	if (dest == 0) {
		/* no real output, only hashing functions */
		fatal("no output to send data to\n");
	}
	err = pthread_create(&dest->thread,0,&outputThread,dest);
	assert(0 == err);
	if (Status) {
		err = pthread_create(&ReaderThr,0,&inputThread,0);
		assert(0 == err);
		(void) pthread_sigmask(SIG_UNBLOCK, &signalSet, NULL);
		statusThread();
		err = pthread_join(ReaderThr,0);
		if (err != 0)
			errormsg("error joining reader: %s\n",strerror(errno));
	} else {
		(void) pthread_sigmask(SIG_UNBLOCK, &signalSet, NULL);
		(void) inputThread(0);
		debugmsg("waiting for output to finish...\n");
		if (TermQ[0] != -1) {
			err = read(TermQ[0],&null,1);
			assert(err == 1);
		}
	}
	int numthreads = joinSenders();
	if (Memmap) {
		int ret = munmap(Buffer[0],Blocksize*Numblocks);
		assert(ret == 0);
	}
	if (Tmp != -1)
		(void) close(Tmp);
	reportSenders();
	if (Status || Log != STDERR_FILENO)
		summary(Numout * Blocksize + Rest, numthreads);
	exit(ErrorOccurred ? EXIT_FAILURE : EXIT_SUCCESS);
}

/* vim:tw=0
 */
