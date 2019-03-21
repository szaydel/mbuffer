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

#ifndef GLOBALS_H
#define GLOBALS_H

#include <pthread.h>
#include <semaphore.h>

extern dest_t *Dest;

#define OPTION_B 1
#define OPTION_M 2
#define OPTION_S 4

extern int
	Hashers,
	Terminal, 
	TermQ[2],
	Tmp,
	In,
	OptMode;

extern volatile int
	ActSenders,
	NumSenders,	/* number of sender threads */
	SendSize,
	Terminate,	/* abort execution, because of error or signal */
	Watchdog;	/* 0: off, 1: started, 2: raised */

extern volatile unsigned
	Done,
	EmptyCount,	/* counter incremented when buffer runs empty */
	FullCount,	/* counter incremented when buffer gets full */
	MainOutOK;	/* is the main outputThread still writing or just coordinating senders */

extern volatile unsigned long long
	Rest,
	Numin,
	Numout,
	InSize;

extern char *volatile
	SendAt;

extern size_t
	IDevBSize,
	PrefixLen;

extern long
	PgSz,
	Finish,		/* this is for graceful termination */
	TickTime;

extern char
	*Prefix,
	**Buffer;

extern pthread_mutex_t
	TermMut,	/* prevents statusThread from interfering with request*Volume */
	LowMut,
	HighMut,
	SendMut;

extern sem_t
	Dev2Buf,
	Buf2Dev;

extern pthread_cond_t
	PercLow,	/* low watermark */
	PercHigh,	/* high watermark */
	SendCond;

extern pthread_t
	ReaderThr,
	WatchdogThr;

extern struct timespec
	Starttime;

#endif
