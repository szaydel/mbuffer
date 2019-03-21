/*
 *  Copyright (C) 2017-2018, Thomas Maier-Komor
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
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXPAND(A) A ## 1
#define ISEMPTY(A) EXPAND(A)

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#if ISEMPTY(LIBC_FSTAT)
#undef LIBC_FSTAT
#define LIBC_FSTAT fstat
#endif


ssize_t (*d_open)(const char *path, int oflag, ...) = 0;
ssize_t (*d_read)(int filedes, void *buf, size_t nbyte) = 0;
int (*d_fxstat)(int ver, int fd, struct stat *st) = 0;
int (*d_fstat)(int fd, struct stat *st) = 0;

static ssize_t Fd = -1;
static size_t BSize = 0;
static const char *IDEV = "";
static int HadZero = 0;


int LIBC_OPEN(const char *path, int oflag, ...)
{
	if (d_open == 0) {
		d_open = (ssize_t (*)(const char *,int,...)) dlsym(RTLD_NEXT,TOSTRING(LIBC_OPEN));
		fprintf(stderr,"idev.so: d_open = %p\n",d_open);
		fflush(stderr);
		const char *idev = getenv("IDEV");
		if (idev)
			IDEV = idev;
	}
	assert(d_open);
	int fd;
	if (oflag & O_CREAT) {
		va_list val;
		va_start(val,oflag);
		int mode = va_arg(val,int);
		va_end(val);
		fd = d_open(path, oflag, mode);
	} else {
		fd = d_open(path, oflag);
	}
	fprintf(stderr,"idev.so: open(%s,0x%x,...) = %d (IDEV='%s')\n",path,oflag,fd,IDEV);
	if (strcmp(path, getenv("IDEV")) == 0) {
		fprintf(stderr,"idev.so: FD = %d\n",fd);
		fflush(stderr);
		Fd = fd;
		HadZero = 0;
	}
	return fd;
}


ssize_t LIBC_READ(int fd, void *buf, size_t s)
{
	if (d_read == 0) {
		d_read = (ssize_t (*)(int,void*,size_t)) dlsym(RTLD_NEXT, "read");
		fprintf(stderr,"idev.so: d_read = %p\n",d_read);
	}
	assert(d_read);
	if (fd != Fd) 
		return d_read(fd,buf,s);
	assert(HadZero == 0);
	if (BSize == 0)
		BSize = strtol(getenv("BSIZE"),0,0);
	if (s < BSize) {
		fprintf(stderr,"idev.so: read(%d,%p,%llu<%llu) = ENOMEM\n",fd,buf,(long long unsigned)s,(long long unsigned)BSize);
		fflush(stderr);
		errno = ENOMEM;
		return -1;
	}
	int n = d_read(fd,buf,s);
	if (n == 0)
		HadZero = 1;
	return n;
}


int __fxstat(int ver, int fd, struct stat *st)
{
	fprintf(stderr,"idev.so: __fxstat(%d,%d,%p)\n",ver,fd,st);
	if (d_fxstat == 0) {
		d_fxstat = (int (*)(int,int,struct stat *)) dlsym(RTLD_NEXT, "__fxstat");
		fprintf(stderr,"idev.so: d_fstat = %p\n",d_fstat);
	}
	assert(d_fxstat);
	int r = d_fxstat(ver,fd,st);
	if (fd == Fd) {
		if (BSize == 0)
			BSize = strtol(getenv("BSIZE"),0,0);
		fprintf(stderr,"idev.so: blksize set to %llu\n",(long long unsigned)BSize);
		fflush(stderr);
		st->st_blksize = BSize;
		st->st_mode &= ~S_IFMT;
		st->st_mode |= S_IFCHR;
	}
	return r;
}


int fstat(int fd, struct stat *st)
{
	fprintf(stderr,"idev.so: fstat(%d,%p)\n",fd,st);
	if (d_fstat == 0) {
		d_fstat = (int (*)(int,struct stat *)) dlsym(RTLD_NEXT, TOSTRING(LIBC_FSTAT));
		fprintf(stderr,"idev.so: d_fstat = %p\n",d_fstat);
	}
	assert(d_fstat);
	int r = d_fstat(fd,st);
	if (fd == Fd) {
		if (BSize == 0)
			BSize = strtol(getenv("BSIZE"),0,0);
		fprintf(stderr,"idev.so: blksize set to %llu\n",(long long unsigned)BSize);
		fflush(stderr);
		st->st_blksize = BSize;
		st->st_mode &= ~S_IFMT;
		st->st_mode |= S_IFCHR;
	}
	return r;
}
