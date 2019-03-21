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

#ifndef COMMON_H
#define COMMON_H

#include <sys/time.h>

#ifdef __sun
#include <synch.h>
#define sem_t sema_t
#define sem_init(a,b,c) sema_init(a,c,USYNC_THREAD,0)
#define sem_post sema_post
#define sem_getvalue(a,b) ((*(b) = (a)->count), 0)
#if defined(__SunOS_5_8) || defined(__SunOS_5_9)
#define sem_wait SemWait
#else
#define sem_wait sema_wait
#endif
#endif

int mt_usleep(unsigned long long sleep_usecs);
long long enforceSpeedLimit(unsigned long long limit, long long num, struct timespec *last);
void releaseLock(void *l);
void enable_directio(int fd, const char *fn);
int disable_directio(int fd, const char *fn);

#endif
