/* varnish-perf - multiprocessing http test client
 *
 * Copyright © 2012 by Weongyo Jeong <weongyo@gmail.com>.
 * Copyright © 1998,1999,2001 by Jef Poskanzer <jef@mail.acme.com>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "vas.h"

#define NEEDLESS_RETURN(foo)	return (foo)

static int	c_arg = 1;

static void *
pef_worker(void *arg)
{

	

	NEEDLESS_RETURN(NULL);
}

static void
PEF_Run(void)
{
	pthread_t tp;
	int i;

	for (i = 0; i < c_arg; i++)
		AZ(pthread_create(&tp, NULL, pef_worker, NULL));
}

static void
usage(void)
{
	fprintf(stderr, "usage: varnishperf [options] urlfile\n");
#define FMT "    %-28s # %s\n"
	fprintf(stderr, FMT, "-c N", "Sets number of threads");

}

int
main(int argc, char *argv[])
{
	int ch;
	char *end;

	while ((ch = getopt(argc, argv, "c:")) != -1) {
		switch (ch) {
		case 'c':
			errno = 0;
			c_arg = strtoul(optarg, &end, 10);
			if (errno == ERANGE || end == optarg || *end) {
				fprintf(stderr, "illegal number for -c\n");
				exit(1);
			}
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	(void)signal(SIGPIPE, SIG_IGN);

	PEF_Run();
}
