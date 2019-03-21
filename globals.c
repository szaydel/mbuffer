/*
 *  Copyright (C) 2000-2017, Thomas Maier-Komor
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

#include "dest.h"
#include "globals.h"
#include <fcntl.h>
#include <sys/time.h>

dest_t *Dest = 0;

int
	Hashers = 0,		/* number of hashing threads */
	In = -1,
	OptMode = O_EXCL,
	Terminal = 0,		/* do we have a controling terminal? */
	TermQ[2],
	Tmp = -1;

volatile int
	ActSenders = 0,
	NumSenders = -1,	/* number of sender threads */
	SendSize = 0,
	Terminate = 0,		/* abort execution, because of error or signal */
	Watchdog = 0;		/* 0: off, 1: started, 2: raised */

volatile unsigned
	Done = 0,
	EmptyCount = 0,		/* counter incremented when buffer runs empty */
	FullCount = 0,		/* counter incremented when buffer gets full */
	MainOutOK = 1;		/* is the main outputThread still writing or just coordinating senders */

volatile unsigned long long
	Rest = 0,
	Numin = 0,
	Numout = 0,
	InSize = 0;

char *volatile
	SendAt = 0;

size_t
	IDevBSize = 0,
	PrefixLen = 0;

long
	PgSz = 0,
	Finish = -1,		/* this is for graceful termination */
	TickTime = 0;

char
	*Prefix,
	**Buffer;

pthread_mutex_t
	TermMut = PTHREAD_MUTEX_INITIALIZER,	/* prevents statusThread from interfering with request*Volume */
	LowMut = PTHREAD_MUTEX_INITIALIZER,
	HighMut = PTHREAD_MUTEX_INITIALIZER,
	SendMut = PTHREAD_MUTEX_INITIALIZER;

sem_t
	Dev2Buf,
	Buf2Dev;

pthread_cond_t
	PercLow = PTHREAD_COND_INITIALIZER,	/* low watermark */
	PercHigh = PTHREAD_COND_INITIALIZER,	/* high watermark */
	SendCond = PTHREAD_COND_INITIALIZER;

pthread_t
	ReaderThr,
	WatchdogThr;

struct timespec
	Starttime;
