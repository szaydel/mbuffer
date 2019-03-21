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

#ifndef LOG_H
#define LOG_H

#include "config.h"

#if !(defined(__sun) || defined(__linux))
#define NEED_IO_INTERLOCK
#endif

#ifdef NEED_IO_INTERLOCK
#include <pthread.h>
extern pthread_mutex_t LogMut;
#endif

typedef enum { silent = 0, fatals, errors, warnings, infos, debugs, iomsgs } verbose_t;

extern verbose_t Verbose;
extern int Log, ErrorOccurred, ErrorsFatal;

#ifdef DEBUG
void logdebug(const char *msg, ...);
#define debugmsg if (Verbose >= debugs) logdebug
#define debugiomsg if (Verbose >= iomsgs) logdebug
#elif __STDC_VERSION__ >= 199901L
#define debugmsg(...)
#define debugiomsg(...)
#else
#define debugmsg
#define debugiomsg
#endif

void setVerbose(const char *arg);
void infomsg(const char *msg, ...);
void statusmsg(const char *msg, ...);
void warningmsg(const char *msg, ...);
void errormsg(const char *msg, ...);
void fatal(const char *msg, ...);
void printmsg(const char *msg, ...);


#endif
