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
#include "dest.h"
#include "hashing.h"
#include "network.h"
#include "settings.h"
#include "globals.h"
#include "log.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>	// for PF_INET6
#include <sys/stat.h>
#include <unistd.h>


typedef enum { off, on, invalid } flag_t;


clockid_t
	ClockSrc;

int	Autoloader = 0,
	Status = 1,
	Memlock = 0,
	TapeAware = 0,
	Memmap = 0,
	Quiet = 0,
	Options = 0,
	OptSync = 0,
	SetOutsize = 0,
	StatusLog = 1;

unsigned int
	NumVolumes = 1,		/* number of input volumes, 0 for interactive prompting */
	AutoloadTime = 0;

long
	AvP = 0,		/* available pages */
	NumP = 0;		/* total number of physical pages */

unsigned long
	Timeout = 0,
	Numblocks = 512,	/* number of buffer blocks */
	Outsize = 10240;

unsigned long long
	Blocksize = 10240,	// fundamental I/O block size
	MaxReadSpeed = 0,
	MaxWriteSpeed = 0,
	Totalmem = 0,
	OutVolsize = 0,
	Pause = 0;

float	StatusInterval = 0.5;	/* status update interval time */

double	StartWrite = 0,		/* high watermark threshold */
	StartRead = 1;		/* low watermark threshold */

const char
	*Infile = 0,
	*OutFile = 0,
	*AutoloadCmd = 0;
char
	*Tmpfile = 0;

extern void *watchdogThread(void *ignored);

static const char *calcval(const char *arg, unsigned long long *res)
{
	char ch;
	double d;
	
	switch (sscanf(arg,"%lf%c",&d,&ch)) {
	default:
		abort();
		break;
	case 2:
		if (d <= 0)
			return "negative value out of range";
		switch (ch) {
		case 'k':
		case 'K':
			d *= 1024.0;
			*res = d;
			return 0;
		case 'm':
		case 'M':
			d *= 1024.0*1024.0;
			*res = d;
			return 0;
		case 'g':
		case 'G':
			d *= 1024.0*1024.0*1024.0;
			*res = d;
			return 0;
		case 't':
		case 'T':
			d *= 1024.0*1024.0*1024.0*1024.0;
			*res = d;
			return 0;
		case '%':
			if ((d >= 90) || (d <= 0))
				return "invalid value for percentage (must be 0..90)";
			*res = d;
			return 0;
		case 'b':
		case 'B':
			if (d < 128)
				return "invalid value for number of bytes";
			*res = d;
			return 0;
		default:
			return "invalid dimension";
		}
	case 1:
		if (d <= 0)
			return "value out of range";
		*res = d;
		return 0;
	case 0:
		break;
	}
	return "unrecognized argument";
}


static int isEmpty(const char *l)
{
	while (*l) {
		if ((*l != ' ') && (*l != '\t'))
			return 0;
		++l;
	}
	return 1;
}


static flag_t parseFlag(const char *valuestr)
{
	if ((strcasecmp(valuestr,"yes") == 0) || (strcasecmp(valuestr,"on") == 0) || (strcmp(valuestr,"1") == 0) || (strcmp(valuestr,"true") == 0))
		return on;
	else if ((strcasecmp(valuestr,"no") == 0) || (strcasecmp(valuestr,"off") == 0) || (strcmp(valuestr,"0") == 0) || (strcmp(valuestr,"false") == 0))
		return off;
	else 
		return invalid;
}


void readConfigFile(const char *cfname)
{
	int df,lineno = 0;
	struct stat st;
	char *cfdata, *line;

	df = open(cfname,O_RDONLY);
	if (df == -1) {
		if (errno == ENOENT)
			infomsg("no config file %s\n",cfname);
		else
			warningmsg("error opening config file %s: %s\n",cfname,strerror(errno));
		return;
	}
	if (-1 == fstat(df,&st)) {
		warningmsg("unable to stat config file %s: %s\n",cfname,strerror(errno));
		close(df);
		return;
	}
	if ((getuid() != st.st_uid) && (st.st_uid != 0)) {
		warningmsg("ignoring config file '%s' from different user\n",cfname);
		close(df);
		return;
	}
	infomsg("reading config file %s\n",cfname);
	cfdata = malloc(st.st_size+1);
	if (cfdata == 0)
		fatal("out of memory\n");
	int n = read(df,cfdata,st.st_size);
	const int save_errno = errno;
	close(df);
	if (n < 0) {
		warningmsg("error reading %s: %s\n",cfname,strerror(save_errno));
		free(cfdata);
		return;
	}
	cfdata[n] = 0;
	line = cfdata;
	while (line && *line) {
		char key[64],valuestr[64];
		int a;
		++lineno;
		char *nl = strchr(line,'\n');
		if (nl) {
			*nl = 0;
			++nl;
		}
		char *pound = strchr(line,'#');
		if (pound)
			*pound = 0;
		if (isEmpty(line)) {
			line = nl;
			continue;
		}
		a = sscanf(line,"%63[A-Za-z]%*[ \t=:]%63[0-9a-zA-Z.]",key,valuestr);
		if (a != 2) {
			warningmsg("config file %s, line %d: error parsing '%s'\n",cfname,lineno,line);
			line = nl;
			continue;
		}
		line = nl;
		debugmsg("parsing key/value pair %s=%s\n",key,valuestr);
		if (strcasecmp(key,"numblocks") == 0) {
			errno = 0;
			long nb = strtol(valuestr,0,0);
			if ((nb <= 0) && (errno != 0)) {
				warningmsg("invalid argument for %s: \"%s\"\n",key,valuestr);
			} else {
				Numblocks = nb;
				debugmsg("Numblocks = %llu\n",Numblocks);
			}
		} else if (strcasecmp(key,"pause") == 0) {
			errno = 0;
			long long p = strtoll(valuestr,0,0);
			if ((p <= 0) || (errno != 0)) {
				warningmsg("invalid argument for %s: \"%s\"\n",key,valuestr);
			} else {
				Pause = p;
				debugmsg("Pause = %lldusec\n",Pause);
			}
		} else if (strcasecmp(key,"autoloadtime") == 0) {
			errno = 0;
			long at = strtol(valuestr,0,0) - 1;
			if ((at <= 0) && (errno != 0)) {
				warningmsg("invalid argument for %s: \"%s\"\n",key,valuestr);
			} else if ((at < 0) || (at > UINT_MAX)) {
				warningmsg("ignoring invalid value for %s: \"%s\"\n",key,valuestr);
			} else {
				AutoloadTime = at;
				debugmsg("Autoloader time = %d\n",AutoloadTime);
			}
		} else if (strcasecmp(key,"startread") == 0) {
			double sr = 0;
			if (1 == sscanf(valuestr,"%lf",&sr))
				sr /= 100;
			if ((sr <= 1) && (sr > 0)) {
				StartRead = sr;
				debugmsg("StartRead = %1.2lf\n",StartRead);
			} else {
				warningmsg("ignoring invalid value '%s' for %s\n",valuestr,key);
			}
		} else if (strcasecmp(key,"startwrite") == 0) {
			double sw = 0;
			if (1 == sscanf(valuestr,"%lf",&sw))
				sw /= 100;
			if ((sw <= 1) && (sw > 0)) {
				StartWrite = sw;
				debugmsg("StartWrite = %1.2lf\n",StartWrite);
			} else {
				warningmsg("ignoring invalid value '%s' for %s\n",valuestr,key);
			}
		} else if (strcasecmp(key,"timeout") == 0) {
			errno = 0;
			long t = strtol(valuestr,0,0);
			if ((t < 0) || (errno != 0)) 
				warningmsg("invalid argument for %s: \"%s\"\n",key,valuestr);
			else {
				Timeout = t;
				debugmsg("Timeout = %lu sec.\n",Timeout);
			}
		} else if (strcasecmp(key,"showstatus") == 0) {
			switch (parseFlag(valuestr)) {
			case on:
				Quiet = 0;
				debugmsg("showstatus = yes\n");
				break;
			case off:
				Quiet = 1;
				debugmsg("showstatus = no\n");
				break;
			default:
				warningmsg("invalid argument for %s: \"%s\"\n",key,valuestr);
			}
		} else if (strcasecmp(key,"tapeaware") == 0) {
			switch (parseFlag(valuestr)) {
			case on:
				TapeAware = 0;
				debugmsg("tapeaware = off\n");
				break;
			case off:
				TapeAware = 1;
				debugmsg("tapeaware = on\n");
				break;
			default:
				warningmsg("invalid argument for %s: \"%s\"\n",key,valuestr);
			}
		} else if (strcasecmp(key,"logstatus") == 0) {
			switch (parseFlag(valuestr)) {
			case on:
				StatusLog = 1;
				debugmsg("logstatus = yes\n");
				break;
			case off:
				StatusLog = 0;
				debugmsg("logstatus = no\n");
				break;
			default:
				warningmsg("invalid argument for %s: \"%s\"\n",key,valuestr);
			}
		} else if (strcasecmp(key,"memlock") == 0) {
			switch (parseFlag(valuestr)) {
			case on:
				Memlock = 1;
				debugmsg("Memlock = %lu\n",Memlock);
				break;
			case off:
				Memlock = 0;
				debugmsg("Memlock = %lu\n",Memlock);
				break;
			default:
				warningmsg("invalid argument for %s: \"%s\"\n",key,valuestr);
			}
		} else if (strcasecmp(key,"printpid") == 0) {
			switch (parseFlag(valuestr)) {
			case on:
				printmsg("PID is %d\n",getpid());
				break;
			case off:
				/* don't do anything */
				break;
			default:
				warningmsg("invalid argument for %s: \"%s\"\n",key,valuestr);
			}
		} else if (strcasecmp(key,"StatusInterval") == 0) {
			float itv;
			if ((1 == sscanf(valuestr,"%f",&itv)) && (itv > 0)) {
				StatusInterval = itv;
				debugmsg("StatusInterval = %f\n",StatusInterval);
			} else {
				warningmsg("invalid argument for %s: \"%s\"\n",key,valuestr);
			}
		} else if (strcasecmp(key,"verbose") == 0) {
			setVerbose(valuestr);
		} else {
			unsigned long long value = 0;
			const char *argerror = calcval(valuestr,&value);
			if (argerror) {
				warningmsg("ignoring invalid key/value pair (%s = %s): %s\n",key,valuestr,argerror);
			} else if (strcasecmp(key,"blocksize") == 0) {
				Blocksize = value;
				debugmsg("Blocksize = %lu\n",Blocksize);
			} else if (strcasecmp(key,"maxwritespeed") == 0) {
				MaxWriteSpeed = value;
				debugmsg("MaxWriteSpeed = %lu\n",MaxWriteSpeed);
			} else if (strcasecmp(key,"maxreadspeed") == 0) {
				MaxReadSpeed = value;
				debugmsg("MaxReadSpeed = %lu\n",MaxReadSpeed);
			} else if (strcasecmp(key,"Totalmem") == 0) {
				if (value >= 100) {
					Totalmem = value;
					debugmsg("Totalmem = %lluk\n",Totalmem>>10);
				} else if (NumP && PgSz) {
					Totalmem = ((unsigned long long) NumP * PgSz * value) / 100 ;
					debugmsg("Totalmem = %lluk\n",Totalmem>>10);
				} else {
					warningmsg("Unable to determine page size or amount of available memory - please specify an absolute amount of memory.\n");
				}
			} else if (strcasecmp(key,"tcpbuffer") == 0) {
				TCPBufSize = value;
				debugmsg("TCPBufSize = %lu\n",TCPBufSize);
			} else {
				warningmsg("unknown parameter: %s\n",key);
			}
		}
	}
	free(cfdata);
}


long maxSemValue()
{
	long mxnrsem = sysconf(_SC_SEM_VALUE_MAX);
	if (-1 == mxnrsem) {
#ifdef SEM_MAX_VALUE
		mxnrsem = SEM_MAX_VALUE;
#else
		mxnrsem = UINT8_MAX;
		warningmsg("unable to determine maximum value of semaphores\n");
#endif
	}
	return mxnrsem;
}


void initBuffer()
{
	int c;
	/* check that we stay within system limits */
	if (Numblocks > maxSemValue())
		fatal("cannot allocate more than %d blocks.\nThis is a system dependent limit, depending on the maximum semaphore value.\nPlease choose a bigger block size.\n",maxSemValue());
	if (Numblocks > 10000)
		warningmsg("high value of number of blocks(%lu): increase block size for better performance\n",Numblocks);
	if ((AvP != 0) && (((AvP * PgSz) / 2) < (Numblocks * Blocksize)))
		warningmsg("allocating more than half of available memory\n");
	if ((Blocksize * (long long)Numblocks) > (long long)SSIZE_MAX)
		fatal("Cannot address so much memory (%lld*%d=%lld>%lld).\n",Blocksize,Numblocks,Blocksize*(long long)Numblocks,(long long)SSIZE_MAX);
	/* create buffer */
	Buffer = (char **) valloc(Numblocks * sizeof(char *));
	if (!Buffer)
		fatal("Could not allocate enough memory (%d requested): %s\n",Numblocks * sizeof(char *),strerror(errno));
	if (Memmap) {
		infomsg("mapping temporary file to memory with %llu blocks with %llu byte (%llu kB total)...\n",(unsigned long long) Numblocks,(unsigned long long) Blocksize,(unsigned long long) ((Numblocks*Blocksize) >> 10));
		if (!Tmpfile) {
			char tmplname[] = "mbuffer-XXXXXX";
			char *tmpdir = getenv("TMPDIR") ? getenv("TMPDIR") : "/var/tmp";
			size_t tl = strlen(tmpdir);
			Tmpfile = malloc(sizeof(tmplname) + tl + 1);
			if (!Tmpfile)
				fatal("out of memory: %s\n",strerror(errno));
			(void) memcpy(Tmpfile,tmpdir,tl);
			Tmpfile[tl] = '/';
			(void) strncpy(Tmpfile+tl+1,tmplname,sizeof(tmplname));
			Tmp = mkstemp(Tmpfile);
			infomsg("tmpfile is %s\n",Tmpfile);
		} else {
			mode_t mode = O_RDWR | O_LARGEFILE;
			if (strncmp(Tmpfile,"/dev/",5))
				mode |= O_CREAT|O_EXCL;
			Tmp = open(Tmpfile,mode,0600);
		}
		if (-1 == Tmp)
			fatal("could not create temporary file (%s): %s\n",Tmpfile,strerror(errno));
		if (strncmp(Tmpfile,"/dev/",5))
			(void) unlink(Tmpfile);
		/* resize the file. Needed - at least under linux, who knows why? */
		if (-1 == lseek(Tmp,Numblocks * Blocksize - sizeof(int),SEEK_SET))
			fatal("could not resize temporary file: %s\n",strerror(errno));
		c = 0;
		if (-1 == write(Tmp,&c,sizeof(c)))
			fatal("could not resize temporary file: %s\n",strerror(errno));
		Buffer[0] = mmap(0,Blocksize*Numblocks,PROT_READ|PROT_WRITE,MAP_SHARED,Tmp,0);
		if (MAP_FAILED == Buffer[0])
			fatal("could not map buffer-file to memory: %s\n",strerror(errno));
		debugmsg("temporary file mapped to address %p\n",Buffer[0]);
	} else {
		infomsg("allocating memory for %d blocks with %llu %s (%llu kB total)...\n"
			,Numblocks
			,Blocksize & 0x3ff ? Blocksize : (Blocksize >> 10)
			,Blocksize & 0x3ff ? "bytes" : "kB"
			,(unsigned long long)((Numblocks*Blocksize) >> 10));
		Buffer[0] = (char *) valloc(Blocksize * Numblocks);
		if (Buffer[0] == 0)
			fatal("Could not allocate enough memory (%lld requested): %s\n",(unsigned long long)Blocksize * Numblocks,strerror(errno));
#ifdef MADV_DONTFORK
		if (-1 == madvise(Buffer[0],Blocksize * Numblocks, MADV_DONTFORK))
			warningmsg("unable to advise memory handling of buffer: %s\n",strerror(errno));
#endif
	}
	for (c = 1; c < Numblocks; c++) {
		Buffer[c] = Buffer[0] + Blocksize * c;
		*Buffer[c] = 0;	/* touch every block before locking */
	}

#ifdef _POSIX_MEMLOCK_RANGE
	if (Memlock) {
		uid_t uid;
#ifndef HAVE_SETEUID
#define seteuid setuid
#endif
		uid = geteuid();
		if (0 != seteuid(0))
			warningmsg("could not change to uid 0 to lock memory (is mbuffer setuid root?)\n");
		else if ((0 != mlock((char *)Buffer,Numblocks * sizeof(char *))) || (0 != mlock(Buffer[0],Blocksize * Numblocks)))
			warningmsg("could not lock buffer in memory: %s\n",strerror(errno));
		else
			infomsg("memory locked successfully\n");
		int err = seteuid(uid);	/* don't give anyone a chance to attack this program, so giveup uid 0 after locking... */
		assert(err == 0);
	}
#endif
}


void searchOptionV(int argc, const char **argv)
{
	int c;
	for (c = 1; c < argc; c++) {
		const char *arg = argv[c];
		if ((arg[0] == '-') && (arg[1] == 'v')) {
			if (arg[2]) {
				setVerbose(arg+2);
			} else if (++c < argc) {
				setVerbose(argv[c]);
			} else {
				fatal("missing argument to option -v\n");
			}
		}
	}
}


static void version(void)
{
	(void) fprintf(stderr,
		"mbuffer version "PACKAGE_VERSION"\n"\
		"Copyright 2001-2019 - T. Maier-Komor\n"\
		"License: GPLv3 - see file LICENSE\n"\
		"This program comes with ABSOLUTELY NO WARRANTY!!!\n"
		"Donations via PayPal to thomas@maier-komor.de are welcome and support this work!\n"
		"\n"
		);
	exit(EXIT_SUCCESS);
}



static void usage(void)
{
	const char *dim = "bkMGTP";
	unsigned long long m = Numblocks * Blocksize;
	while (m >= 10000) {
		m >>= 10;
		++dim;
	}
	(void) fprintf(stderr,
		"usage: mbuffer [Options]\n"
		"Options:\n"
		"-b <num>   : use <num> blocks for buffer (default: %ld)\n"
		"-s <size>  : use blocks of <size> bytes for processing (default: %llu)\n"
#if defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE) && !defined(__CYGWIN__) || defined(__FreeBSD__)
		"-m <size>  : memory <size> of buffer in b,k,M,G,%% (default: 2%% = %llu%c)\n"
#else
		"-m <size>  : memory <size> of buffer in b,k,M,G,%% (default: %llu%c)\n"
#endif
#ifdef _POSIX_MEMLOCK_RANGE
		"-L         : lock buffer in memory (unusable with file based buffers)\n"
#endif
		"-d         : use blocksize of device for output\n"
		"-D <size>  : assumed output device size (default: infinite/auto-detect)\n"
		"-P <num>   : start writing after buffer has been filled more than <num>%%\n"
		"-p <num>   : start reading after buffer has been filled less than <num>%%\n"
		"-i <file>  : use <file> for input\n"
		"-o <file>  : use <file> for output (this option can be passed MULTIPLE times)\n"
		"--append   : append to output file (must be passed before -o)\n"
		"--truncate : truncate next file (must be passed before -o)\n"
		"-I <h>:<p> : use network port <port> as input, allow only host <h> to connect\n"
		"-I <p>     : use network port <port> as input\n"
		"-O <h>:<p> : output data to host <h> and port <p> (MUTLIPLE outputs supported)\n"
		"-n <num>   : <num> volumes for input, '0' to prompt interactively\n"
		"-t         : use memory mapped temporary file (for huge buffer)\n"
		"-T <file>  : as -t but uses <file> as buffer\n"
		"-l <file>  : use <file> for logging messages\n"
		"-u <num>   : pause <num> milliseconds after each write\n"
		"-r <rate>  : limit read rate to <rate> B/s, where <rate> can be given in b,k,M,G\n"
		"-R <rate>  : same as -r for writing; use either one, if your tape is too fast\n"
		"-f         : overwrite existing files\n"
		"-a <time>  : autoloader which needs <time> seconds to reload\n"
		"-A <cmd>   : issue command <cmd> to request new volume\n"
		"-v <level> : set verbose level to <level> (valid values are 0..6)\n"
		"-q         : quiet - do not display the status on stderr\n"
		"-Q         : quiet - do not log the status\n"
		"-c         : write with synchronous data integrity support\n"
		"-e         : stop processing on any kind of error\n"
#if defined HAVE_LIBCRYPTO || defined HAVE_LIBMD5 || defined HAVE_LIBMHASH
		"-H\n"
		"--md5      : generate md5 hash of transfered data\n"
		"--hash <a> : use algorithm <a>, if <a> is 'list' possible algorithms are listed\n"
#endif
		"--pid      : print PID of this instance\n"
		"-W <time>  : set watchdog timeout to <time> seconds\n"
		"-4         : force use of IPv4\n"
		"-6         : force use of IPv6\n"
		"-0         : use IPv4 or IPv6\n"
		"--tcpbuffer: size for TCP buffer\n"
		"--tapeaware: write to end of tape instead of stopping when the drive signals\n"
		"             the media end is approaching (write until 2x ENOSPC errors)\n"
		"-V\n"
		"--version  : print version information\n"
		"Unsupported buffer options: -t -Z -B\n"
		,Numblocks
		,Blocksize
		,m
		,*dim
		);
	exit(EXIT_SUCCESS);
}


static unsigned long long calcint(const char **argv, int c, unsigned long long def)
{
	char ch;
	double d = (double)def;
	
	switch (sscanf(argv[c],"%lf%c",&d,&ch)) {
	default:
		abort();
		break;
	case 2:
		if (d <= 0)
			fatal("invalid argument - must be > 0\n");
		switch (ch) {
		case 'k':
		case 'K':
			d *= 1024.0;
			return (unsigned long long) d;
		case 'm':
		case 'M':
			d *= 1024.0*1024.0;
			return (unsigned long long) d;
		case 'g':
		case 'G':
			d *= 1024.0*1024.0*1024.0;
			return (unsigned long long) d;
		case 't':
		case 'T':
			d *= 1024.0*1024.0*1024.0*1024.0;
			return (unsigned long long) d;
		case '%':
			if ((d >= 90) || (d <= 0))
				fatal("invalid value for percentage (must be 0..90)\n");
			return (unsigned long long) d;
		case 'b':
		case 'B':
			if (d < 128)
				fatal("invalid value for number of bytes\n");
			return (unsigned long long) d;
		default:
			if (argv[c][-2] == '-')
				fatal("unrecognized size charakter \"%c\" for option \"%s\"\n",ch,&argv[c][-2]);
			else
				fatal("unrecognized size charakter \"%c\" for option \"%s\"\n",ch,argv[c-1]);
			return d;
		}
	case 1:
		if (d <= 0)
			fatal("invalid argument - must be > 0\n");
		if (d <= 100) {
			if (argv[c][-2] == '-')
				fatal("invalid low value for option \"%s\" - missing suffix?\n",&argv[c][-2]);
			else
				fatal("invalid low value for option \"%s\" - missing suffix?\n",argv[c-1]);
		}
		return d;
	case 0:
		break;
	}
	errormsg("unrecognized argument \"%s\" for option \"%s\"\n",argv[c],argv[c-1]);
	return d;
}


static int argcheck(const char *opt, const char **argv, int *c, int argc)
{
	if (strncmp(opt,argv[*c],strlen(opt))) 
		return 1;
	if (strlen(argv[*c]) > 2)
		argv[*c] += 2;
	else {
		(*c)++;
		if (*c == argc)
			fatal("missing argument to option %s\n",opt);
	}
	return 0;
}


int parseOption(int c, int argc, const char **argv)
{
	if (!argcheck("-s",argv,&c,argc)) {
		Blocksize = Outsize = calcint(argv,c,Blocksize);
		Options |= OPTION_S;
		debugmsg("Blocksize = %llu\n",Blocksize);
		if (Blocksize < 100)
			fatal("cannot set blocksize as percentage of total physical memory\n");
	} else if (!strcmp("--append",argv[c])) {
		OptMode |= O_APPEND;
		OptMode &= ~O_EXCL;
		debugmsg("append to next file\n");
	} else if (!strcmp("--truncate",argv[c])) {
		OptMode |= O_TRUNC;
		debugmsg("truncate next file\n");
	} else if (!argcheck("-m",argv,&c,argc)) {
		Totalmem = calcint(argv,c,Totalmem);
		Options |= OPTION_M;
		if (Totalmem < 100) {
#if defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE) && !defined(__CYGWIN__) || defined(__FreeBSD__)
			Totalmem = ((unsigned long long) NumP * PgSz * Totalmem) / 100 ;
#else
			fatal("Unable to determine page size or amount of available memory - please specify an absolute amount of memory.\n");
#endif
		}
		debugmsg("Totalmem = %lluk\n",Totalmem>>10);
	} else if (!argcheck("-b",argv,&c,argc)) {
		long nb = strtol(argv[c],0,0);
		if ((nb == 0) && (errno == EINVAL)) {
			errormsg("invalid argument to option -b: \"%s\"\n",argv[c]);
		} else {
			Numblocks = nb;
			Options |= OPTION_B;
		}
		debugmsg("Numblocks = %llu\n",Numblocks);
	} else if (!strcmp("--tcpbuffer",argv[c])) {
		TCPBufSize = calcint(argv,++c,TCPBufSize);
		debugmsg("TCPBufSize = %lu\n",TCPBufSize);
	} else if (!strcmp("--tapeaware",argv[c])) {
		TapeAware = 1;
		debugmsg("sensing early end-of-tape warning\n");
	} else if (!strcmp("-d",argv[c])) {
#ifdef HAVE_STRUCT_STAT_ST_BLKSIZE
		SetOutsize = 1;
		debugmsg("setting output size according to the blocksize of the device\n");
#else
		fatal("cannot determine blocksize of device (unsupported by OS)\n");
#endif
	} else if (!argcheck("-v",argv,&c,argc)) {
		/* option has to be checked again, so that the
		 * command line can override a config file */
		setVerbose(argv[c]);
	} else if (!argcheck("-u",argv,&c,argc)) {
		long long p = strtoll(argv[c],0,0);
		if ((p == 0) && (errno == EINVAL))
			errormsg("invalid argument to option -u: \"%s\"\n",argv[c]);
		else
			Pause = p;
		debugmsg("Pause = %lldusec\n",Pause);
	} else if (!argcheck("-r",argv,&c,argc)) {
		MaxReadSpeed = calcint(argv,c,0);
		debugmsg("MaxReadSpeed = %lld\n",MaxReadSpeed);
	} else if (!argcheck("-R",argv,&c,argc)) {
		MaxWriteSpeed = calcint(argv,c,0);
		debugmsg("MaxWriteSpeed = %lld\n",MaxWriteSpeed);
	} else if (!argcheck("-n",argv,&c,argc)) {
		long nv = strtol(argv[c],0,0);
		if ((nv < 0) || ((nv == 0) && (errno == EINVAL)))
			fatal("invalid argument to option -n: \"%s\"\n",argv[c]);
		else if ((nv < 0) || (nv > UINT_MAX))
			errormsg("argument for number of volumes out of range: %ld\n",nv);
		else
			NumVolumes = nv;
		debugmsg("NumVolumes = %u\n",NumVolumes);
	} else if (!argcheck("-i",argv,&c,argc)) {
		if (Infile)
			fatal("cannot set input file: file already set\n");
		if (In != -1)
			fatal("cannot initialize input - input already set\n");
		if (strcmp(argv[c],"-")) {
			Infile = argv[c];
			debugmsg("Infile = %s\n",Infile);
		} else {
			In = STDIN_FILENO;
			debugmsg("Infile is stdin\n");
		}
	} else if (!argcheck("-o",argv,&c,argc)) {
		dest_t *dest = malloc(sizeof(dest_t));
		if (strcmp(argv[c],"-")) {
			debugmsg("output file: %s\n",argv[c]);
			dest->arg = argv[c];
			dest->name = argv[c];
			dest->fd = -1;
			dest->mode = O_CREAT|O_WRONLY|OptMode|O_LARGEFILE|OptSync;
			// ++NumSenders is done once open() in openDestinationFiles was successful
		} else {
			dest_t *d = Dest;
			while (d) {
				if (0 == strcmp(d->name,"<stdout>"))
					fatal("cannot output multiple times to stdout\n");
				d = d->next;
			}
			debugmsg("output to stdout\n",argv[c]);
			dest->fd = dup(STDOUT_FILENO);
			int err = dup2(STDERR_FILENO,STDOUT_FILENO);
			assert(err != -1);
			dest->arg = "<stdout>";
			dest->name = "<stdout>";
			dest->mode = 0;
			++NumSenders;
		}
		OptMode = O_EXCL;
		dest->port = 0;
		dest->result = 0;
		bzero(&dest->thread,sizeof(dest->thread));
		dest->next = Dest;
		Dest = dest;
		if (OutFile == 0)
			OutFile = argv[c];
#ifdef AF_INET6
	} else if (!strcmp("-0",argv[c])) {
		AddrFam = AF_UNSPEC;
	} else if (!strcmp("-4",argv[c])) {
		AddrFam = AF_INET;
	} else if (!strcmp("-6",argv[c])) {
		AddrFam = AF_INET6;
#endif
	} else if (!argcheck("-I",argv,&c,argc)) {
		initNetworkInput(argv[c]);
	} else if (!argcheck("-O",argv,&c,argc)) {
		dest_t *d = createNetworkOutput(argv[c]);
		d->next = Dest;
		Dest = d;
		if (d->fd != -1)
			++NumSenders;
	} else if (!argcheck("-T",argv,&c,argc)) {
		Tmpfile = strdup(argv[c]);
		if (!Tmpfile)
			fatal("out of memory\n");
		Memmap = 1;
		debugmsg("Tmpfile = %s\n",Tmpfile);
	} else if (!strcmp("-t",argv[c])) {
		Memmap = 1;
		debugmsg("Memmap = 1\n");
	} else if (!argcheck("-l",argv,&c,argc)) {
		Log = open(argv[c],O_WRONLY|O_APPEND|O_TRUNC|O_CREAT|O_LARGEFILE,0666);
		if (-1 == Log) {
			Log = STDERR_FILENO;
			errormsg("error opening log file: %s\n",strerror(errno));
		}
		debugmsg("logFile set to %s\n",argv[c]);
	} else if (!strcmp("-f",argv[c])) {
		OptMode &= ~O_EXCL;
		OptMode |= O_TRUNC;
		debugmsg("overwrite = 1\n");
	} else if (!strcmp("-q",argv[c])) {
		debugmsg("disabling display of status\n");
		Quiet = 1;
	} else if (!strcmp("-Q",argv[c])) {
		debugmsg("disabling logging of status\n");
		StatusLog = 0;
	} else if (!strcmp("-c",argv[c])) {
		debugmsg("enabling full synchronous I/O\n");
		OptSync = O_SYNC;
	} else if (!strcmp("-e",argv[c])) {
		debugmsg("will terminate on any kind of error\n");
		ErrorsFatal = 1;
	} else if (!argcheck("-a",argv,&c,argc)) {
		long at = strtol(argv[c],0,0) - 1;
		if ((at == 0) && (errno == EINVAL)) {
			errormsg("invalid argument to option -a: \"%s\"\n",argv[c]);
		} else if ((at < 0) || (at > UINT_MAX)) {
			warningmsg("ignoring invalid value for autoload time: %ld\n",at);
		} else {
			Autoloader = 1;
			AutoloadTime = at;
		}
		debugmsg("Autoloader time = %d\n",AutoloadTime);
	} else if (!argcheck("-A",argv,&c,argc)) {
		Autoloader = 1;
		AutoloadCmd = argv[c];
		debugmsg("Autoloader command = \"%s\"\n", AutoloadCmd);
	} else if (!argcheck("-P",argv,&c,argc)) {
		if (1 != sscanf(argv[c],"%lf",&StartWrite))
			StartWrite = 0;
		StartWrite /= 100;
		if ((StartWrite > 1) || (StartWrite <= 0))
			fatal("error in argument -P: must be bigger than 0 and less or equal 100\n");
		debugmsg("StartWrite = %1.2lf\n",StartWrite);
	} else if (!argcheck("-p",argv,&c,argc)) {
		if (1 == sscanf(argv[c],"%lf",&StartRead))
			StartRead /= 100;
		else
			StartRead = 1.0;
		if ((StartRead >= 1) || (StartRead < 0))
			fatal("error in argument -p: must be bigger or equal to 0 and less than 100\n");
		debugmsg("StartRead = %1.2lf\n",StartRead);
	} else if (!strcmp("-L",argv[c])) {
#ifdef _POSIX_MEMLOCK_RANGE
		Memlock = 1;
		debugmsg("memory locking enabled\n");
#else
		warning("POSIX memory locking is unsupported on this system.\n");
#endif
	} else if (!argcheck("-W",argv,&c,argc)) {
		Timeout = strtol(argv[c],0,0);
		if (Timeout <= 0)
			fatal("invalid argument to option -W\n");
		if (Timeout < (AutoloadTime*2))
			fatal("timeout must be at least 2x autoload time\n");
		int err = pthread_create(&WatchdogThr,0,&watchdogThread,(void*)0);
		assert(0 == err);
		infomsg("started watchdog with Timeout = %lu sec.\n",Timeout);
		Watchdog = 1;
	} else if (!strcmp("--direct",argv[c])) {
		warningmsg("Option --direct is deprecated. O_DIRECT is used automatically, if possible.\n");
	} else if (!strcmp("--help",argv[c]) || !strcmp("-h",argv[c])) {
		usage();
	} else if (!strcmp("--version",argv[c]) || !strcmp("-V",argv[c])) {
		version();
	} else if (!strcmp("--md5",argv[c]) || !strcmp("-H",argv[c])) {
		if (addHashAlgorithm("MD5")) {
			++Hashers;
			++NumSenders;
		}
	} else if (!strcmp("--hash",argv[c])) {
		++c;
		if (c == argc)
			fatal("missing argument to option --hash\n");
		if (!strcmp(argv[c],"list")) {
			listHashAlgos();
			exit(EXIT_SUCCESS);
		}
		if (addHashAlgorithm(argv[c])) {
			++Hashers;
			++NumSenders;
		}
	} else if (!strcmp("--pid",argv[c])) {
		int pid = getpid();
		printmsg("PID is %d\n",pid);
		int n = snprintf(0,0,"%s (%d): ",argv[0],pid);
		Prefix = realloc(Prefix,n+1);
		if (Prefix == NULL)
			fatal("out of memory allocating the prefix string\n");
		snprintf(Prefix,n+1,"%s (%d): ",argv[0],pid);
		PrefixLen = n;
	} else if (!argcheck("-D",argv,&c,argc)) {
		OutVolsize = calcint(argv,c,0);
		debugmsg("OutVolsize = %llu\n",OutVolsize);
	} else
		fatal("unknown option \"%s\"\n",argv[c]);
	return c;
}
