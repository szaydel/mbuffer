/*
 *  Copyright (C) 2000-2018, Thomas Maier-Komor
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

#ifndef SETTINGS_H
#define SETTINGS_H

#include <time.h>
#include <sys/types.h>

extern int32_t TCPBufSize;

extern clockid_t
	ClockSrc;

extern int
	Autoloader,	/* use autoloader for tape change */
	AddrFam,	/* address family - in network.c */
	Direct,
	Memlock,	/* protoect buffer in memory against swapping */
	TapeAware,
	Memmap,
	Options,
	OptSync,
	Quiet,		/* quiet mode */
	SetOutsize,
	Status,
	StatusLog;

extern unsigned int
	NumVolumes,	/* number of volumes to expect while reading */
	AutoloadTime;	/* time to wait after an autoload command */

extern long
	AvP,
	NumP;

extern unsigned long
	Numblocks,		/* number of buffer blocks */
	Timeout,
	Outsize;

extern unsigned long long
	Blocksize,		/* fundamental I/O block size */
	MaxReadSpeed,
	MaxWriteSpeed,
	Totalmem,
	Pause,
	OutVolsize;

extern const char
	*Infile,
	*OutFile,
	*AutoloadCmd;

extern char
	*Tmpfile;

extern float
	StatusInterval;		/* status update interval time */

extern double
	StartWrite,		/* high watermark threshold */
	StartRead;		/* low watermark threshold */

void readConfigFile(const char *cfname);
void initBuffer();
void searchOptionV(int argc, const char **argv);
int parseOption(int c, int argc, const char **argv);
long maxSemValue();

#endif
