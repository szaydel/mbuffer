/*
 *  Copyright (C) 2018, Peter Pentchev
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

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fputs("Usage: have-af {inet,inet6}\n", stderr);
		exit(1);
	}

	const char * const af_name = argv[1];
	int af = AF_UNSPEC;
#if defined(AF_INET)
	if (strcmp(af_name, "inet") == 0) {
		af = AF_INET;
	}
#endif
#if defined(AF_INET6)
	if (strcmp(af_name, "inet6") == 0) {
		af = AF_INET6;
	}
#endif
	if (af == AF_UNSPEC) {
		fprintf(stderr, "Unsupported address family: %s\n", af_name);
		exit(1);
	}

	struct addrinfo hint;
	bzero(&hint,sizeof(hint));
	hint.ai_family = af;
	hint.ai_protocol = IPPROTO_TCP;
	hint.ai_socktype = SOCK_STREAM;
	hint.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;

	struct addrinfo *pinfo;
	const int err = getaddrinfo(NULL, "7001", &hint, &pinfo);
	return err != 0;
}
