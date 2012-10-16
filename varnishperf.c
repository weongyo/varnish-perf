/* varnish-perf - multiprocessing http test client
 *
 * Copyright (c) 2012 by Weongyo Jeong <weongyo@gmail.com>.
 * Copyright (c) 1998,1999,2001 by Jef Poskanzer <jef@mail.acme.com>.
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
#include <strings.h>
#include <unistd.h>

#include "vas.h"

#define NEEDLESS_RETURN(foo)	return (foo)

struct sess {
	unsigned		magic;
#define SESS_MAGIC		0x2c2f9c5a
};

struct worker {
	unsigned		magic;
#define WORKER_MAGIC		0x6391adcf
	int		fd;
};
VTAILQ_HEAD(workerhead, worker);

struct wq {
	unsigned		magic;
#define WQ_MAGIC		0x606658fa
	pthread_mutex_t		mtx;
	VTAILQ_HEAD(, sess)	queue;
};

static int	c_arg = 1;

static void *
pef_worker(void *arg)
{
	struct worker *w, ww;

	w = &ww;
	bzero(w, sizeof(*w));
	w->magic = WORKER_MAGIC;
	w->fd = epoll_create(1);
	assert(w->fd >= 0);

	while (1) {
		
	}

	NEEDLESS_RETURN(NULL);
}

static void
PEF_Run(void)
{
	struct wq wq;
	pthread_t tp[c_arg];
	int i;

	wq.magic = WQ_MAGIC;
	AZ(pthread_mutex_init(&wq.mtx, NULL));

	for (i = 0; i < c_arg; i++)
		AZ(pthread_create(&tp[i], NULL, pef_worker, &wq));
	for (i = 0; i < c_arg; i++)
		AZ(pthread_join(tp[i], NULL));
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
