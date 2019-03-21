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

#ifndef MBCONF_H
#define MBCONF_H

#include "config.h"

#define _GNU_SOURCE 1	/* needed for O_DIRECT */
#define _LARGEFILE64_SOURCE

#ifndef lint
#undef restrict
#undef inline
#endif

/* derived configuration defines */
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <unistd.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#ifndef O_DIRECT
#define O_DIRECT 0
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef S_SPLINT_S
#ifndef _POSIX_SEMAPHORES
#error posix sempahores are required
#endif
#endif

#ifdef __CYGWIN__
#include <malloc.h>
#undef assert
#define assert(x) ((x) || (*(char *) 0 = 1))
#endif

#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif


#endif
