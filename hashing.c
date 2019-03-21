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

#include "mbconf.h"
#include "hashing.h"

#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

#include "dest.h"
#include "log.h"
#include "globals.h"


#if defined HAVE_LIBMD5 && defined HAVE_MD5_H
#include <md5.h>
#define WITH_LIBMD5

// libmd5 and libssl are xor due to name clashes
#elif defined HAVE_LIBCRYPTO
#include <openssl/md5.h>
#define WITH_SSLMD5
#endif


#define USE_MHASH -3
#define USE_GCRYPT -4
#define USE_LIBMD5 -5
#define USE_SSLMD5 -6
#define USE_RHASH -7

static void *LibMhash = 0, *LibGcrypt = 0, *LibRhash = 0;

typedef struct gcry_md_handle *gcry_md_hd_t;
typedef unsigned int gcry_error_t;
static int (*gcry_md_map_name)(const char *);
static const char *(*gcry_md_algo_name)(int);
static unsigned (*gcry_md_get_algo_dlen)(int);
static gcry_error_t (*gcry_md_open)(gcry_md_hd_t *,int,unsigned);
static unsigned char *(*gcry_md_read)(gcry_md_hd_t,int);
static void (*gcry_md_write)(gcry_md_hd_t, const void *, size_t);

#define MHASH_FAILED ((void*)0)
static void *(*mhash_init)(int);
static void (*mhash_deinit)(void *, void *);
static int (*mhash_count)();
static uint32_t (*mhash_get_block_size)(int);
static const char *(*mhash_get_hash_name_static)(int);
static char (*mhash)(void *,const void *,uint32_t);

typedef void *rhash;
static int (*rhash_count)(void);
static void *(*rhash_init)(unsigned);
static void (*rhash_update)(void *, const void *, size_t);
static void (*rhash_final)(void *, unsigned char *);
static int (*rhash_get_digest_size)(unsigned);
static const char *(*rhash_get_name)(unsigned);


static void initHashLibs()
{
	static int initialized = 0;

	if (initialized)
		return;
	initialized = 1;
	LibMhash = dlopen("libmhash.so",RTLD_NOW);
	if (LibMhash) {
		debugmsg("found libmhash\n");
		mhash_init = (void *(*)(int)) dlsym(LibMhash,"mhash_init");
		mhash_deinit = (void (*)(void *, void *)) dlsym(LibMhash,"mhash_deinit");
		mhash_count = (int (*)()) dlsym(LibMhash,"mhash_count");
		mhash_get_block_size = (uint32_t (*)(int)) dlsym(LibMhash,"mhash_get_block_size");
		mhash_get_hash_name_static = (const char *(*)(int)) dlsym(LibMhash,"mhash_get_hash_name_static");
		mhash = (char (*)(void *,const void*,uint32_t)) dlsym(LibMhash,"mhash");
		if ((mhash_init == 0) || (mhash_deinit == 0) || (mhash_count == 0)
			|| (mhash_get_block_size == 0) || (mhash_get_hash_name_static == 0) || (mhash == 0)) {
			warningmsg("libmhash.so does not contain all required symbols\n");
			dlclose(LibMhash);
			LibMhash = 0;
		}
	}
	LibRhash = dlopen("librhash.so.0",RTLD_NOW);
	if (LibRhash) {
		debugmsg("found librhash\n");
		void (*rhash_library_init)() = (void (*)(void)) dlsym(LibRhash,"rhash_library_init");
		rhash_init = (void *(*)(unsigned)) dlsym(LibRhash,"rhash_init");
		rhash_final = (void (*)(void *, unsigned char*)) dlsym(LibRhash,"rhash_final");
		rhash_count = (int (*)()) dlsym(LibRhash,"rhash_count");
		rhash_get_digest_size = (int (*)(unsigned)) dlsym(LibRhash,"rhash_get_digest_size");
		rhash_get_name = (const char *(*)(unsigned)) dlsym(LibRhash,"rhash_get_name");
		rhash_update = (void (*)(void *,const void*,size_t)) dlsym(LibRhash,"rhash_update");
		if ((rhash_init == 0) || (rhash_final == 0) || (rhash_count == 0)
			|| (rhash_get_digest_size == 0) || (rhash_get_name == 0) || (rhash_update == 0)) {
			warningmsg("librhash.so does not contain all required symbols\n");
			dlclose(LibRhash);
			LibRhash = 0;
		} else {
			rhash_library_init();
		}
	}
	LibGcrypt = dlopen("libgcrypt.so.20",RTLD_NOW);
	if (LibGcrypt) {
		debugmsg("found libmhash\n");
		gcry_md_open = (gcry_error_t (*)(gcry_md_hd_t *,int,unsigned)) dlsym(LibGcrypt,"gcry_md_open");
		gcry_md_read = (unsigned char *(*)(gcry_md_hd_t,int)) dlsym(LibGcrypt,"gcry_md_read");
		gcry_md_write = (void (*)(gcry_md_hd_t,const void *,size_t)) dlsym(LibGcrypt,"gcry_md_write");
		gcry_md_algo_name = (const char *(*)(int)) dlsym(LibGcrypt,"gcry_md_algo_name");
		gcry_md_map_name = (int (*)(const char *)) dlsym(LibGcrypt,"gcry_md_map_name");
		gcry_md_get_algo_dlen = (unsigned (*)(int)) dlsym(LibGcrypt,"gcry_md_get_algo_dlen");
		if ((gcry_md_open == 0) || (gcry_md_read == 0) || (gcry_md_write == 0)
			|| (gcry_md_algo_name == 0) || (gcry_md_map_name == 0) || (gcry_md_get_algo_dlen == 0)) {
			warningmsg("libgcrypt.so.20 does not contain all required symbols\n");
			dlclose(LibGcrypt);
			LibGcrypt = 0;
		}
	}
}


void listHashAlgos()
{
	initHashLibs();
	int n = 0;
	if (LibGcrypt) {
		(void) fprintf(stderr,"valid hash functions of libgcrypt are:\n");
		int algo = 1;
		for (algo = 1; algo < 512; ++algo ) {
			const char *name = gcry_md_algo_name(algo);
			assert(name);
			if (name[0] != '?') {
				assert(algo == gcry_md_map_name(name));
				++n;
				printf("\t%s\n",name);
			}
		}
	}
	if (LibMhash) {
		(void) fprintf(stderr,"valid hash functions of libmhash are:\n");
		int algo = mhash_count();
		while (algo >= 0) {
			const char *algoname = mhash_get_hash_name_static(algo);
			if (algoname) {
				++n;
				(void) fprintf(stderr,"\t%s\n",algoname);
			}
			--algo;
		}
	}
	if (LibRhash) {
		(void) fprintf(stderr,"valid hash functions of librhash are:\n");
		int algo = rhash_count();
		while (algo > 0) {
			const char *algoname = rhash_get_name(algo);
			if (algoname) {
				++n;
				(void) fprintf(stderr,"\t%s\n",algoname);
			}
			--algo;
		}
	}
#ifdef WITH_LIBMD5
	fprintf(stderr,"valid hash functions of libmd5 are:\n\tlibmd5:md5\n");
#endif
#ifdef WITH_SSLMD5
	fprintf(stderr,"valid hash functions of libssl are:\n\topenssl:md5\n");
#endif
	if (n == 0)
		fatal("no hash calculation libraries could be found!\n");
}


static void addDigestDestination(int lib, int algo, const char *algoname)
{
	dest_t *dest = malloc(sizeof(dest_t));
	bzero(dest,sizeof(dest_t));
	dest->name = algoname;
	dest->fd = lib;
	dest->mode = algo;
	if (Dest) {
		dest->next = Dest->next;
		Dest->next = dest;
	} else {
		Dest = dest;
		dest->next = 0;
	}
}


int addHashAlgorithm(const char *name)
{
	initHashLibs();
	if (LibGcrypt) {
		const char *an = name;
		if (0 == memcmp(an,"gcrypt:",7))
			an += 7;
		int algo = gcry_md_map_name(an);
		if (algo != 0) {
			addDigestDestination(USE_GCRYPT,algo,an);
			debugmsg("enabled hash algorithm gcrypt:%s\n",an);
			return 1;
		}
	}
	if (LibMhash) {
		const char *an = name;
		int algo, numalgo = mhash_count();
		if (0 == memcmp(an,"mhash:",6))
			an += 6;
		for (algo = 0; algo <= numalgo; ++algo) {
			const char *algoname = mhash_get_hash_name_static(algo);
			if (algoname && (strcasecmp(algoname,an) == 0)) {
				addDigestDestination(USE_MHASH,algo,an);
				debugmsg("enabled hash algorithm mhash:%s\n",an);
				return 1;
			}
		}
	}
	if (LibRhash) {
		const char *an = name;
		int algo, numalgo = rhash_count();
		if (0 == memcmp(an,"rhash:",6))
			an += 6;
		for (algo = 1; algo <= numalgo; ++algo) {
			const char *algoname = rhash_get_name(algo);
			if (algoname && (strcasecmp(algoname,an) == 0)) {
				addDigestDestination(USE_RHASH,algo,an);
				debugmsg("enabled hash algorithm mhash:%s (%d)\n",an,algo);
				return 1;
			}
		}
	}
#ifdef WITH_LIBMD5
	if ((0 == strcasecmp(name,"md5")) || (0 == strcasecmp(name,"libmd5:md5"))) {
		addDigestDestination(USE_LIBMD5,0,name);
		debugmsg("enabled hash algorithm %s\n",name);
		return 1;
	}
#endif
#ifdef WITH_SSLMD5
	if ((0 == strcasecmp(name,"md5")) || (0 == strcasecmp(name,"openssl:md5"))) {
		addDigestDestination(USE_SSLMD5,0,name);
		debugmsg("enabled hash algorithm %s\n",name);
		return 1;
	}
#endif
	errormsg("invalid or unsupported hash function %s\n",name);
	return 0;
}


void *hashThread(void *arg)
{
	dest_t *dest = (dest_t *) arg;
	gcry_md_hd_t hd;
	void *ctxt = 0;
	int algo = dest->mode;

	switch (dest->fd) {
	case USE_MHASH:
		ctxt = mhash_init(algo);
		assert(ctxt != MHASH_FAILED);
		break;
	case USE_RHASH:
		ctxt = rhash_init(algo);
		assert(ctxt != 0);
		break;
	case USE_GCRYPT:
		gcry_md_open(&hd, dest->mode, 0);
		break;
#ifdef WITH_LIBMD5
	case USE_LIBMD5:
		ctxt = malloc(sizeof(MD5_CTX));
		MD5Init(ctxt);
		break;
#endif
#ifdef WITH_SSLMD5
	case USE_SSLMD5:
		ctxt = malloc(sizeof(MD5_CTX));
		MD5_Init(ctxt);
		break;
#endif
	default:
		abort();
	}

	debugmsg("hashThread(): starting...\n");
	for (;;) {
		int size;

		(void) syncSenders(0,0);
		size = SendSize;
		if (0 == size) {
			size_t ds,al;
			unsigned char hashvalue[128];
			char *msg, *m;
			const char *an;
			int i;
			
			debugmsg("hashThread(): done.\n");
			switch (dest->fd) {
			case USE_GCRYPT:
				ds = gcry_md_get_algo_dlen(dest->mode);
				assert(sizeof(hashvalue) >= ds);
				an = gcry_md_algo_name(dest->mode);
				memcpy(hashvalue,gcry_md_read(hd,dest->mode),ds);
				break;
			case USE_MHASH:
				ds = mhash_get_block_size(algo);
				assert(sizeof(hashvalue) >= ds);
				mhash_deinit(ctxt,hashvalue);
				an =  mhash_get_hash_name_static(algo);
				break;
			case USE_RHASH:
				ds = rhash_get_digest_size(algo);
				assert(sizeof(hashvalue) >= ds);
				rhash_final(ctxt,hashvalue);
				an =  rhash_get_name(algo);
				break;
#ifdef WITH_LIBMD5
			case USE_LIBMD5:
				ds = 16;
				MD5Final(hashvalue,ctxt);
				free(ctxt);
				an = "libmd5:md5";
				break;
#endif
#ifdef WITH_SSLMD5
			case USE_SSLMD5:
				ds = 16;
				MD5_Final(hashvalue,ctxt);
				free(ctxt);
				an = "openssl:md5";
				break;
#endif
			default:
				abort();
			}
			al = strlen(an);
			// 9 = strlen(" hash: ") + \n + \0
			msg = malloc(al+9+(ds<<1));
			assert(msg);
			memcpy(msg,an,al);
			memcpy(msg+al," hash: ",7);
			m = msg + al + 7;
			for (i = 0; i < ds; ++i)
				m += sprintf(m,"%02x",(unsigned int)hashvalue[i]);
			*m++ = '\n';
			*m = 0;
			dest->result = msg;
			pthread_exit((void *) msg);
			return 0;	/* for lint */
		}
		if (Terminate) {
			(void) syncSenders(0,-1);
			infomsg("hashThread(): terminating early upon request...\n");
			pthread_exit((void *) 0);
		}
		debugiomsg("hashThread(): hashing %d@0x%p\n",size,(void*)SendAt);
		switch (dest->fd) {
		case USE_GCRYPT:
			gcry_md_write(hd,SendAt,size);
			break;
		case USE_MHASH:
			mhash(ctxt,SendAt,size);
			break;
		case USE_RHASH:
			rhash_update(ctxt,SendAt,size);
			break;
#ifdef WITH_LIBMD5
		case USE_LIBMD5:
			MD5Update(ctxt,SendAt,size);
			break;
#endif
#ifdef WITH_SSLMD5
		case USE_SSLMD5:
			MD5_Update(ctxt,SendAt,size);
			break;
#endif
		default:
			abort();
		}
	}
	return 0;
}


