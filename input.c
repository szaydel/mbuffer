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
#include "input.h"
#include "common.h"
#include "log.h"
#include "dest.h"
#include "globals.h"
#include "settings.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>


#ifdef __sun
#define waitInput()
#else
static void waitInput(void)
{
	if (Status != 0) {
		int maxfd = TermQ[0] > In ? TermQ[0] + 1 : In + 1;
		int err;

		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(TermQ[0],&readfds);
		FD_SET(In,&readfds);
		do {
			err = select(maxfd,&readfds,0,0,0);
			debugiomsg("inputThread: select(%d, {%d,%d}, 0, 0, 0) = %d\n", maxfd,In,TermQ[0],err);
			assert((err > 0) || (errno == EBADF || errno == EINTR));
		} while ((err < 0) && (errno == EINTR));
		if (FD_ISSET(TermQ[0],&readfds))
			pthread_exit((void *)-1);
		assert(FD_ISSET(In,&readfds));
	}
}
#endif


int promptInteractive()
{
	static const char prompt[] = "\nContinue with next volume? Press 'y' to continue or 'n' to finish...";
	static const char contmsg[] = "\nyes - continuing with next volume...\n";
	static const char donemsg[] = "\nno - input done, waiting for output to finish...\n";
	int err;

	err = pthread_mutex_lock(&TermMut);
	assert(0 == err);
	if (-1 == write(STDERR_FILENO,prompt,sizeof(prompt))) {
		errormsg("error accessing controlling terminal for manual volume change request: %s\nConsider using autoload option, when running mbuffer without terminal.\n",strerror(errno));
		Terminate = 1;
		pthread_exit((void *) -1);
	}
	for (;;) {
		char c = 0;
		if (-1 == read(STDERR_FILENO,&c,1) && (errno != EINTR)) {
			errormsg("error accessing controlling terminal for manual volume change request: %s\nConsider using autoload option, when running mbuffer without terminal.\n",strerror(errno));
			Terminate = 1;
			pthread_exit((void *) -1);
		}
		debugmsg("prompt input %c\n",c);
		switch (c) {
		case 'n':
		case 'N':
			(void) write(STDERR_FILENO,donemsg,sizeof(donemsg));
			err = pthread_mutex_unlock(&TermMut);
			assert(0 == err);
			return 0;
		case 'y':
		case 'Y':
			(void) write(STDERR_FILENO,contmsg,sizeof(contmsg));
			err = pthread_mutex_unlock(&TermMut);
			assert(0 == err);
			return 1;
		default:;
		}
	}
}


static int requestInputVolume()
{
	static struct timespec volstart = {0,0};
	const char *cmd;
	struct timespec now;
	double diff;
	unsigned min,hr;
	char cmd_buf[15+strlen(Infile)];

	debugmsg("requesting new volume for input\n");
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
		infomsg("time for reading volume: %u:%02u:%02f\n",hr,min,diff);
	} else if (diff > 60) {
		min = (unsigned) (diff / 60);
		diff -= min * 60;
		infomsg("time for reading volume: %02u:%02f\n",min,diff);
	} else
		infomsg("time for reading volume: %02fsec.\n",diff);
	if (-1 == close(In))
		errormsg("error closing input: %s\n",strerror(errno));
	do {
		if ((Autoloader) && (Infile)) {
			int ret;
			if (AutoloadCmd) {
				cmd = AutoloadCmd;
			} else {
				(void) snprintf(cmd_buf, sizeof(cmd_buf), "mt -f %s offline", Infile);
				cmd = cmd_buf;
			}
			infomsg("requesting new input volume with command '%s'\n",cmd);
			ret = system(cmd);
			if (0 < ret) {
				warningmsg("error running \"%s\" to change volume in autoloader: exitcode %d\n",cmd,ret);
				Terminate = 1;
				pthread_exit((void *) 0);
			} else if (0 > ret) {
				errormsg("error starting \"%s\" to change volume in autoloader: %s\n", cmd, strerror(errno));
				Terminate = 1;
				pthread_exit((void *) -1);
			}
			if (AutoloadTime) {
				infomsg("waiting for drive to get ready...\n");
				(void) sleep(AutoloadTime);
			}
		} else if (0 == promptInteractive()) {
			return 0;
		}
		In = open(Infile, O_RDONLY | O_LARGEFILE);
		if ((-1 == In) && (errno == EINVAL))
			In = open(Infile,O_RDONLY);
		if (-1 != In)
			enable_directio(In,Infile);
		else
			errormsg("could not reopen input %s: %s\n",Infile,strerror(errno));
	} while (In == -1);
	(void) clock_gettime(ClockSrc,&volstart);
	diff = volstart.tv_sec - now.tv_sec + (double) (volstart.tv_nsec - now.tv_nsec) * 1E-9;
	infomsg("tape-change took %fsec. - continuing with next volume\n",diff);
	NumVolumes--;
	if (Terminal && ! Autoloader) {
		char msg[] = "\nOK - continuing...\n";
		(void) write(STDERR_FILENO,msg,sizeof(msg));
	}
	return 1;
}


void openInput()
{
	debugmsg("opening input %s\n",Infile);
	int flags = O_RDONLY | O_LARGEFILE;
	In = open(Infile,flags);
	if ((-1 == In) && (errno == EINVAL)) {
		flags &= ~O_LARGEFILE;
		In = open(Infile,flags);
	}
	if (-1 == In)
		fatal("could not open input file: %s\n",strerror(errno));
	enable_directio(In,Infile);
	struct stat st;
	if ((0 == fstat(In,&st)) && ((st.st_mode & S_IFMT) == S_IFREG))
		InSize = st.st_size;
#ifdef __sun
	if (0 == directio(In,DIRECTIO_ON))
		infomsg("direct I/O hinting enabled for input\n");
	else
		infomsg("direct I/O hinting failed for input: %s\n",strerror(errno));
#endif
}


static int devread(unsigned at)
{
	static char *DevBuf = 0;
	static size_t IFill = 0, Off = 0;
	static int hadzero = 0;
	int num = 0;
	do {
		if (IFill) {
			size_t s = IFill;
			if (IFill > (Blocksize-num))
				s = Blocksize-num;
			debugmsg("fillop %d, fill %d, off %d\n",s,IFill,Off);
			memcpy(Buffer[at]+num,DevBuf+Off,s);
			Off += s;
			IFill -= s;
			num += s;
			if (num == Blocksize)
				return num;
		}
		if (hadzero) {
			hadzero = 0;
			return 0;
		}
		ssize_t in = read(In,Buffer[at] + num,Blocksize - num);
		debugmsg("devread %d = %d\n",Blocksize-num,in);
		if (in > 0) {
			num += in;
		} else if (in == 0) {
			if (num)
				hadzero = 1;
			return num;
		} else if (in == -1) {
			if ((errno == EINVAL) && disable_directio(In,Infile))
				continue;
			if (errno != ENOMEM)
				return -1;
			if (DevBuf == 0) {
				// devread is only called when IDevBSize > 0
				assert(IDevBSize > 0);
				DevBuf = malloc(IDevBSize);
				assert(DevBuf);
			}
			assert(IFill == 0);
			ssize_t i2 = read(In,DevBuf,IDevBSize);
			debugmsg("devread2 %d = %d %d/%s\n",IDevBSize,i2,errno,strerror(errno));
			if ((i2 == -1) && (errno == EINVAL) && disable_directio(In,Infile))
				continue;
			if (i2 == -1) {
				assert(errno != ENOMEM);
				return -1;
			}
			if (i2 == 0) {
				if (num)
					hadzero = 1;
				return num;
			}
			assert(IFill == 0);
			IFill = i2;
			Off = 0;
		}
	} while (num != Blocksize);
	return num;
}


int readBlock(unsigned at)
{
	int err;
	size_t num = 0;
	waitInput();
	do {
		ssize_t in;
		if (IDevBSize)
			in = devread(at);
		else
			in = read(In,Buffer[at] + num,Blocksize - num);
		debugiomsg("inputThread: read(In, Buffer[%d] + %llu, %llu) = %d\n", at, num, Blocksize - num, in);
		if (in > 0) {
			num += in;
		} else if (((0 == in) || ((-1 == in) && (errno == EIO))) && (Terminal||Autoloader) && (NumVolumes != 1)) {
			if (0 == requestInputVolume()) {
				Finish = at;
				Rest = num;
				debugmsg("inputThread: last block has %llu bytes\n",num);
				err = pthread_mutex_lock(&HighMut);
				assert(err == 0);
				err = sem_post(&Buf2Dev);
				assert(err == 0);
				err = pthread_cond_signal(&PercHigh);
				assert(err == 0);
				err = pthread_mutex_unlock(&HighMut);
				assert(err == 0);
				infomsg("inputThread: exiting...\n");
				if (Status)
					pthread_exit(0);
				return 0;
			}
		} else if (in <= 0) {
			/* error or end-of-file */
			if ((-1 == in) && (errno == EINVAL) && disable_directio(In,Infile))
				continue;
			if ((-1 == in) && (errno == EINTR))
				continue;
			if ((-1 == in) && (Terminate == 0))
				errormsg("inputThread: error reading at offset 0x%llx: %s\n",Numin*Blocksize,strerror(errno));
			Rest = num;
			Finish = at;
			debugmsg("inputThread: last block has %llu bytes\n",num);
			err = pthread_mutex_lock(&HighMut);
			assert(err == 0);
			err = sem_post(&Buf2Dev);
			assert(err == 0);
			err = pthread_cond_signal(&PercHigh);
			assert(err == 0);
			err = pthread_mutex_unlock(&HighMut);
			assert(err == 0);
			infomsg("inputThread: exiting...\n");
			if (Status)
				pthread_exit((void *)(ptrdiff_t) in);
			return in;
		}
	} while (num < Blocksize);
	return 1;
}


void *inputThread(void *ignored)
{
	int fill = 0;
	unsigned at = 0;
	long long xfer = 0;
	const double startread = StartRead, startwrite = StartWrite;
	struct timespec last;

#ifndef __sun
	if (Status != 0)
		assert(TermQ[0] != -1);
#endif
	(void) clock_gettime(ClockSrc,&last);
	assert(ignored == 0);
	infomsg("inputThread: starting with threadid 0x%lx...\n",(long)pthread_self());
	for (;;) {
		int err;

		if (startread < 1) {
			err = pthread_mutex_lock(&LowMut);
			assert(err == 0);
			err = sem_getvalue(&Buf2Dev,&fill);
			assert(err == 0);
			if (fill == Numblocks - 1) {
				debugmsg("inputThread: buffer full, waiting for it to drain.\n");
				pthread_cleanup_push(releaseLock,&LowMut);
				err = pthread_cond_wait(&PercLow,&LowMut);
				assert(err == 0);
				pthread_cleanup_pop(0);
				++FullCount;
				debugmsg("inputThread: low watermark reached, continuing...\n");
			}
			err = pthread_mutex_unlock(&LowMut);
			assert(err == 0);
		}
		if (Terminate) {	/* for async termination requests */
			debugmsg("inputThread: terminating early upon request...\n");
			if (-1 == close(In))
				errormsg("error closing input: %s\n",strerror(errno));
			if (Status)
				pthread_exit((void *)1);
			return (void *) 1;
		}
		err = sem_wait(&Dev2Buf); /* Wait for one or more buffer blocks to be free */
		assert(err == 0);
		if (0 >= readBlock(at)) {
			debugmsg("inputThread: no more blocks\n");
			return 0;
		}
		if (MaxReadSpeed)
			xfer = enforceSpeedLimit(MaxReadSpeed,xfer,&last);
		err = sem_post(&Buf2Dev);
		assert(err == 0);
		if (startwrite > 0) {
			err = pthread_mutex_lock(&HighMut);
			assert(err == 0);
			err = sem_getvalue(&Buf2Dev,&fill);
			assert(err == 0);
			if (((double) fill / (double) Numblocks) + DBL_EPSILON >= startwrite) {
				err = pthread_cond_signal(&PercHigh);
				assert(err == 0);
			}
			err = pthread_mutex_unlock(&HighMut);
			assert(err == 0);
		}
		if (++at == Numblocks)
			at = 0;
		Numin++;
	}
}


