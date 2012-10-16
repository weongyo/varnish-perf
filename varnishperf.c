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

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "miniobj.h"
#include "vas.h"
#include "vcallout.h"
#include "vqueue.h"

#define NEEDLESS_RETURN(foo)	return (foo)

struct worker;

/*--------------------------------------------------------------------*/

struct params {
	/* Control diagnostic code */
	unsigned		diag_bitmap;
};
static struct params		_params = {
	.diag_bitmap = 0x0
};
static struct params		*params = &_params;

/*--------------------------------------------------------------------*/

enum step {
#define STEP(l, u)	STP_##u,
#include "steps.h"
#undef STEP
};

struct sess {
	unsigned		magic;
#define SESS_MAGIC		0x2c2f9c5a
	struct worker		*wrk;

	enum step		step;
	int			fd;

	socklen_t		sockaddrlen;
	socklen_t		mysockaddrlen;
	struct sockaddr_storage	*sockaddr;
	struct sockaddr_storage	*mysockaddr;
	struct listen_sock	*mylsock;

	struct sessmem		*mem;
	VTAILQ_ENTRY(sess)	poollist;
};

/*--------------------------------------------------------------------*/

struct sessmem {
	unsigned		magic;
#define SESSMEM_MAGIC		0x555859c5

	struct sess		sess;
	VTAILQ_ENTRY(sessmem)	list;
	struct sockaddr_storage	sockaddr[2];
};

static VTAILQ_HEAD(,sessmem)	ses_free_mem[2] = {
    VTAILQ_HEAD_INITIALIZER(ses_free_mem[0]),
    VTAILQ_HEAD_INITIALIZER(ses_free_mem[1]),
};

static unsigned			ses_qp;
static pthread_mutex_t		ses_mem_mtx;

/*--------------------------------------------------------------------*/

struct worker {
	unsigned		magic;
#define WORKER_MAGIC		0x6391adcf
	int			fd;
	struct sess		*sp;

	pthread_cond_t		cond;
	VTAILQ_ENTRY(worker)	list;
};
VTAILQ_HEAD(workerhead, worker);

/*--------------------------------------------------------------------*/

struct wq {
	unsigned		magic;
#define WQ_MAGIC		0x606658fa
	pthread_mutex_t		mtx;
	struct workerhead	idle;
	VTAILQ_HEAD(, sess)	queue;
	unsigned		lqueue;
	uintmax_t		nqueue;
};

static struct wq		wq;

/*--------------------------------------------------------------------*/

/*
 * Sets the concurrent number of threads.  Default is 1 indicating one thread
 * would be invoked.
 */
static int	c_arg = 1;

static void	SES_Delete(struct sess *sp);

/*--------------------------------------------------------------------*/

static inline int
VTCP_Check(int a)
{
	if (a == 0)
		return (1);
	if (errno == ECONNRESET || errno == ENOTCONN)
		return (1);
#if (defined (__SVR4) && defined (__sun)) || defined (__NetBSD__)
	/*
	 * Solaris returns EINVAL if the other end unexepectedly reset the
	 * connection.
	 * This is a bug in Solaris and documented behaviour on NetBSD.
	 */
	if (errno == EINVAL || errno == ETIMEDOUT)
		return (1);
#endif
	return (0);
}

#define VTCP_Assert(a) assert(VTCP_Check(a))

/*--------------------------------------------------------------------
 * Functions for controlling NONBLOCK mode.
 *
 * We use FIONBIO because it is cheaper than fcntl(2), which requires
 * us to do two syscalls, one to get and one to set, the latter of
 * which mucks about a bit before it ends up calling ioctl(FIONBIO),
 * at least on FreeBSD.
 */

static int
VTCP_nonblocking(int sock)
{
	int i, j;

	i = 1;
	j = ioctl(sock, FIONBIO, &i);
	VTCP_Assert(j);
	return (j);
}

/*--------------------------------------------------------------------*/

static void
vca_close_session(struct sess *sp, const char *why)
{
	int i;

	(void)why;

	if (sp->fd >= 0) {
		i = close(sp->fd);
		assert(i == 0 || errno != EBADF);	/* XXX EINVAL seen */
	}
	sp->fd = -1;
}

/*--------------------------------------------------------------------
 * The very first request
 */

static int
cnt_first(struct sess *sp)
{

	sp->step = STP_DONE;
	return (0);
}

/*--------------------------------------------------------------------
 * Emit an error
 */

static int
cnt_error(struct sess *sp)
{

	sp->step = STP_DONE;
	return (0);
}

/*--------------------------------------------------------------------
 * This is the final state, figure out if we should close or recycle
 * the client connection
 */

static int
cnt_done(struct sess *sp)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);

	SES_Delete(sp);
	return (1);
}

/*--------------------------------------------------------------------
 * Central state engine dispatcher.
 *
 * Kick the session around until it has had enough.
 *
 */

static void
cnt_diag(struct sess *sp, const char *state)
{

	fprintf(stdout, "thr %p STP_%s sp %p", (void *)pthread_self(), state,
	    sp);
}

static void
CNT_Session(struct sess *sp)
{
	struct worker *w;
	int done;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	w = sp->wrk;
	CHECK_OBJ_NOTNULL(w, WORKER_MAGIC);

	if (sp->step == STP_FIRST && VTCP_nonblocking(sp->fd)) {
		if (errno == ECONNRESET)
			vca_close_session(sp, "remote closed");
		else
			vca_close_session(sp, "error");
		sp->step = STP_DONE;
	}

	/*
	 * NB: Once done is set, we can no longer touch sp!
	 */
	for (done = 0; !done; ) {
		assert(sp->wrk == w);
		/*
		 * This is a good place to be paranoid about the various
		 * pointers still pointing to the things we expect.
		 */
		CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
		CHECK_OBJ_NOTNULL(sp->wrk, WORKER_MAGIC);

		switch (sp->step) {
#define STEP(l,u) \
		    case STP_##u: \
			if (params->diag_bitmap & 0x01) \
				cnt_diag(sp, #u); \
			done = cnt_##l(sp); \
		        break;
#include "steps.h"
#undef STEP
		default:
			WRONG("State engine misfire");
		}
	}
}

/*--------------------------------------------------------------------
 * Queue a workrequest if possible.
 *
 * Return zero if the request was queued, negative if it wasn't.
 */

static int
WRK_Queue(struct sess *sp)
{
	struct worker *w;
	struct wq *qp;

	qp = &wq;
	AZ(pthread_mutex_lock(&qp->mtx));

	/* If there are idle threads, we tickle the first one into action */
	w = VTAILQ_FIRST(&qp->idle);
	if (w != NULL) {
		VTAILQ_REMOVE(&qp->idle, w, list);
		AZ(pthread_mutex_unlock(&qp->mtx));
		w->sp = sp;
		AZ(pthread_cond_signal(&w->cond));
		return (0);
	}

	VTAILQ_INSERT_TAIL(&qp->queue, sp, poollist);
	qp->nqueue++;
	qp->lqueue++;
	AZ(pthread_mutex_unlock(&qp->mtx));
	return (0);
}

static int
WRK_QueueSession(struct sess *sp)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	AZ(sp->wrk);
	if (WRK_Queue(sp) == 0)
		return (0);
	SES_Delete(sp);
	return (1);
}

static void *
WRK_thread(void *arg)
{
	struct worker *w, ww;
	struct wq *qp;

	CAST_OBJ_NOTNULL(qp, arg, WQ_MAGIC);

	w = &ww;
	bzero(w, sizeof(*w));
	w->magic = WORKER_MAGIC;
	w->fd = epoll_create(1);
	assert(w->fd >= 0);
	AZ(pthread_cond_init(&w->cond, NULL));

	AZ(pthread_mutex_lock(&qp->mtx));
	while (1) {
		CHECK_OBJ_NOTNULL(w, WORKER_MAGIC);
		/* Process queued requests, if any */
		w->sp = VTAILQ_FIRST(&qp->queue);
		if (w->sp != NULL) {
			VTAILQ_REMOVE(&qp->queue, w->sp, poollist);
			qp->lqueue--;
		} else {
			VTAILQ_INSERT_HEAD(&qp->idle, w, list);
			AZ(pthread_cond_wait(&w->cond, &qp->mtx));
		}
		if (w->sp == NULL)
			break;
		AZ(pthread_mutex_unlock(&qp->mtx));

		AZ(w->sp->wrk);
		w->sp->wrk = w;
		CNT_Session(w->sp);
		w->sp = NULL;
		AZ(pthread_mutex_lock(&qp->mtx));
	}

	NEEDLESS_RETURN(NULL);
}

/*--------------------------------------------------------------------*/

static struct timespec
TIM_timespec(double t)
{
	struct timespec tv;

	tv.tv_sec = (time_t)trunc(t);
	tv.tv_nsec = (int)(1e9 * (t - tv.tv_sec));
	return (tv);
}

static void
TIM_sleep(double t)
{
	struct timespec ts;

	ts = TIM_timespec(t);
	(void)nanosleep(&ts, NULL);
}

/*--------------------------------------------------------------------*/

static struct sessmem *
ses_sm_alloc(void)
{
	struct sessmem *sm;

	sm = malloc(sizeof(*sm));
	if (sm == NULL)
		return (NULL);
	sm->magic = SESSMEM_MAGIC;
	return (sm);
}

/*--------------------------------------------------------------------
 * This prepares a session for use, based on its sessmem structure.
 */

static void
ses_setup(struct sessmem *sm)
{
	struct sess *sp;

	CHECK_OBJ_NOTNULL(sm, SESSMEM_MAGIC);
	sp = &sm->sess;
	memset(sp, 0, sizeof *sp);

	/* We assume that the sess has been zeroed by the time we get here */
	AZ(sp->magic);
	sp->magic = SESS_MAGIC;
	sp->mem = sm;
	sp->sockaddr = (void*)(&sm->sockaddr[0]);
	sp->sockaddrlen = sizeof(sm->sockaddr[0]);
	sp->mysockaddr = (void*)(&sm->sockaddr[1]);
	sp->mysockaddrlen = sizeof(sm->sockaddr[1]);
	sp->sockaddr->ss_family = sp->mysockaddr->ss_family = PF_UNSPEC;
}

/*--------------------------------------------------------------------
 * Get a new session, preferably by recycling an already ready one
 */

static struct sess *
SES_New(void)
{
	struct sessmem *sm;
	struct sess *sp;

	assert(ses_qp <= 1);
	sm = VTAILQ_FIRST(&ses_free_mem[ses_qp]);
	if (sm == NULL) {
		/*
		 * If that queue is empty, flip queues holding the lock
		 * and try the new unlocked queue.
		 */
		AZ(pthread_mutex_lock(&ses_mem_mtx));
		ses_qp = 1 - ses_qp;
		AZ(pthread_mutex_unlock(&ses_mem_mtx));
		sm = VTAILQ_FIRST(&ses_free_mem[ses_qp]);
	}
	if (sm != NULL) {
		VTAILQ_REMOVE(&ses_free_mem[ses_qp], sm, list);
		sp = &sm->sess;
		CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	} else {
		sm = ses_sm_alloc();
		if (sm == NULL)
			return (NULL);
		ses_setup(sm);
		sp = &sm->sess;
		CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	}
	return (sp);
}

/*--------------------------------------------------------------------
 * Recycle a session.  If the workspace has changed, deleted it,
 * otherwise wash it, and put it up for adoption.
 */

static void
SES_Delete(struct sess *sp)
{
	struct sessmem *sm;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	sm = sp->mem;
	CHECK_OBJ_NOTNULL(sm, SESSMEM_MAGIC);

	/* Clean and prepare for reuse */
	ses_setup(sm);
	AZ(pthread_mutex_lock(&ses_mem_mtx));
	VTAILQ_INSERT_HEAD(&ses_free_mem[1 - ses_qp], sm, list);
	AZ(pthread_mutex_unlock(&ses_mem_mtx));
}

/*--------------------------------------------------------------------*/

struct sched {
	unsigned		magic;
#define	SCHED_MAGIC		0x5c43a3af
	struct callout		co;
	struct callout_block	cb;
	struct wq		*qp;
};

static void
SCH_tick_1s(void *arg)
{
	struct sched *scp;
	struct sess *sp;

	CAST_OBJ_NOTNULL(scp, arg, SCHED_MAGIC);

	sp = SES_New();
	AN(sp);
	AZ(WRK_QueueSession(sp));

	callout_reset(&scp->cb, &scp->co, CALLOUT_SECTOTICKS(1), SCH_tick_1s,
	    arg);
}

static void *
SCH_thread(void *arg)
{
	struct sched sc, *scp;

	scp = &sc;
	bzero(scp, sizeof(*scp));
	scp->magic = SCHED_MAGIC;
	CAST_OBJ_NOTNULL(scp->qp, arg, WQ_MAGIC);
	COT_init(&scp->cb);
	callout_init(&scp->co, 0);
	callout_reset(&scp->cb, &scp->co, CALLOUT_SECTOTICKS(1), SCH_tick_1s,
	    &sc);
	while (1) {
		COT_ticks(&scp->cb);
		COT_clock(&scp->cb);
		TIM_sleep(0.1);
	}

	NEEDLESS_RETURN(NULL);
}

static void
PEF_Run(void)
{
	pthread_t tp[c_arg], schedtp;
	int i;

	bzero(&wq, sizeof(wq));
	wq.magic = WQ_MAGIC;
	AZ(pthread_mutex_init(&wq.mtx, NULL));
	VTAILQ_INIT(&wq.idle);
	VTAILQ_INIT(&wq.queue);

	for (i = 0; i < c_arg; i++)
		AZ(pthread_create(&tp[i], NULL, WRK_thread, &wq));
	AZ(pthread_create(&schedtp, NULL, SCH_thread, &wq));
	AZ(pthread_join(schedtp, NULL));
	for (i = 0; i < c_arg; i++)
		AZ(pthread_join(tp[i], NULL));
}

static void
usage(void)
{

	fprintf(stderr, "usage: varnishperf [options] urlfile\n");
#define FMT "    %-28s # %s\n"
	fprintf(stderr, FMT, "-c N", "Sets number of threads");
	exit(1);
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
