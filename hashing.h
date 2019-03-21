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

#ifndef HASHING_H
#define HASHING_H

#if defined HAVE_GCRYPT_H && defined HAVE_LIBGCRYPT
#define HAVE_MD5
#elif defined HAVE_MHASH_H && defined HAVE_LIBMHASH
#define HAVE_MD5
#elif defined HAVE_LIBMD5 && defined HAVE_MD5_H
#define HAVE_MD5
#elif defined HAVE_LIBCRYPTO
#define HAVE_MD5
#endif

int addHashAlgorithm(const char *name);
void listHashAlgos();
void *hashThread(void *arg);

#endif
