/*-
 * Copyright (c) 2012 by Weongyo Jeong <weongyo@gmail.com>.
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2009 Varnish Software AS
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

#include <sys/param.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "humanize_number.h"
#include "miniobj.h"
#include "vas.h"
#include "vcallout.h"
#include "vct.h"
#include "vlck.h"
#include "vqueue.h"
#include "vsb.h"

#define VTCP_ADDRBUFSIZE	64
#define VTCP_PORTBUFSIZE	16
#define TIM_FORMAT_SIZE		30
#define NEEDLESS_RETURN(foo)	return (foo)

struct worker;

/*--------------------------------------------------------------------*/

struct params {
	/* Timeouts */
	unsigned		connect_timeout;
	unsigned		read_timeout;
	unsigned		write_timeout;
	/* Control diagnostic code */
	unsigned		diag_bitmap;
};
static struct params		_params = {
	.connect_timeout	= 3,
	.read_timeout		= 6,
	.write_timeout		= 6,
	.diag_bitmap		= 0x0
};
static struct params		*params = &_params;

/*--------------------------------------------------------------------*/

struct perfstat_1s {
	uint32_t		n_conn;
	double			t_conntotal;
	double			t_connmin;
	double			t_connmax;
	uint32_t		n_fb;
	double			t_fbtotal;
	double			t_fbmin;
	double			t_fbmax;
	uint32_t		n_body;
	double			t_bodytotal;
	double			t_bodymin;
	double			t_bodymax;
};
static struct perfstat_1s	_perfstat_1s;
static struct perfstat_1s	*VSC_C_1s = &_perfstat_1s;

struct perfstat {
#define	PERFSTAT_u64(a, b, c, d)	uint64_t a;
#define	PERFSTAT_dbl(a, b, c, d)	double a;
#include "stats.h"
#undef PERFSTAT_dbl
#undef PERFSTAT_u64
};
static struct perfstat		_perfstat;
static struct perfstat		*VSC_C_main = &_perfstat;

/*--------------------------------------------------------------------*/

struct url {
	unsigned		magic;
#define	URL_MAGIC		0x3178c2cb
	char			*url_str;
	char			*hostname;
	char			*path;
	unsigned short		portnum;
	struct sockaddr_storage sockaddr;
	int			sockaddrlen;

	/* formatted ascii target address */
	char			addr[VTCP_ADDRBUFSIZE];
	char			port[VTCP_PORTBUFSIZE];
};
static struct url		*urls;
static int			num_urls;
static int			max_urls;

struct srcip {
	char			*ip;
	struct sockaddr_storage sockaddr;
	int			sockaddrlen;
};
static struct srcip		*srcips;
static int			num_srcips;
static int			max_srcips;

/*--------------------------------------------------------------------*/

enum step {
#define STEP(l, u)	STP_##u,
#include "steps.h"
#undef STEP
};

#define	SESS_WANT_READ		1
#define	SESS_WANT_WRITE		2

struct sess {
	unsigned		magic;
#define SESS_MAGIC		0x2c2f9c5a
	struct worker		*wrk;

	enum step		prevstep;
	enum step		step;
	int			fd;

#define	STEPHIST_MAX		64
	enum step		stephist[STEPHIST_MAX];
	int			nstephist;

	struct url		*url;

	socklen_t		mysockaddrlen;
	struct sockaddr_storage	*mysockaddr;

	struct callout		co;

	struct vsb		*vsb;
	ssize_t			roffset;
	ssize_t			woffset;
#define	MAXHDRSIZ		(4 * 1024)
	char			resp[MAXHDRSIZ];
#define	MAXHDR			64
	char			*resphdr[MAXHDR];

	double			t_start;
	double			t_done;
	double			t_connstart;
	double			t_connend;
	double			t_fbstart;
	double			t_fbend;
	double			t_bodystart;
	double			t_bodyend;

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
static struct lock		ses_mem_mtx;

static struct lock		ses_stat_mtx;
static volatile uint64_t	n_sess_grab = 0;
static uint64_t			n_sess_rel = 0;

/*--------------------------------------------------------------------*/

struct worker {
	unsigned		magic;
#define WORKER_MAGIC		0x6391adcf
	int			fd;
	struct sess		*sp;
	struct callout_block	cb;
	int			nwant;
	pthread_t		owner;

	pthread_cond_t		cond;
	VTAILQ_ENTRY(worker)	list;
};
VTAILQ_HEAD(workerhead, worker);

/*--------------------------------------------------------------------*/

#define	WQ_LOCK(wq)		Lck_Lock(&(wq)->mtx)
#define	WQ_UNLOCK(wq)		Lck_Unlock(&(wq)->mtx)

struct wq {
	unsigned		magic;
#define WQ_MAGIC		0x606658fa
	struct lock		mtx;
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
/*
 * Sets rate.  This option will pointing how many requests will be scheduled
 * per a second.
 */
static int	r_arg = 1;
/*
 * Boot-up time from TIM_real().
 */
static double	boottime;
/*
 * Default value is 0 but 1 if SIGINT is delivered.
 */
static int	stop;

static void	EVT_Add(struct worker *wrk, int want, int fd, void *arg);
static void	EVT_Del(struct worker *wrk, int fd);
static void	SES_Acct(struct sess *sp);
static void	SES_Delete(struct sess *sp);
static int	SES_Schedule(struct sess *sp);
static void	SES_Wait(struct sess *sp, int want);
static double	TIM_real(void);

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
VTCP_name(const struct sockaddr_storage *addr, unsigned l,
    char *abuf, unsigned alen, char *pbuf, unsigned plen)
{
	int i;

	i = getnameinfo((const void *)addr, l, abuf, alen, pbuf, plen,
	   NI_NUMERICHOST | NI_NUMERICSERV);
	if (i) {
		/*
		 * XXX this printf is shitty, but we may not have space
		 * for the gai_strerror in the bufffer :-(
		 */
		printf("getnameinfo = %d %s\n", i, gai_strerror(i));
		(void)snprintf(abuf, alen, "Conversion");
		(void)snprintf(pbuf, plen, "Failed");
		return;
	}
	/* XXX dirty hack for v4-to-v6 mapped addresses */
	if (strncmp(abuf, "::ffff:", 7) == 0) {
		for (i = 0; abuf[i + 7]; ++i)
			abuf[i] = abuf[i + 7];
		abuf[i] = '\0';
	}
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

/*--------------------------------------------------------------------*/

static void
cnt_timeout_tick(void *arg)
{
	struct sess *sp = arg;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);

	EVT_Del(sp->wrk, sp->fd);
	sp->prevstep = sp->step;
	sp->step = STP_TIMEOUT;
	SES_Schedule(sp);
}

static int
cnt_timeout(struct sess *sp)
{

	VSC_C_main->n_timeout++;

	switch (sp->prevstep) {
	case STP_HTTP_CONNECT:
		if (isnan(sp->t_connend))
			sp->t_connend = TIM_real();
		sp->step = STP_HTTP_ERROR;
		break;
	case STP_HTTP_TXREQ:
	case STP_HTTP_RXRESP_HDR:
		if (isnan(sp->t_fbend))
			sp->t_fbend = TIM_real();
		sp->step = STP_HTTP_ERROR;
		break;
	case STP_HTTP_RXRESP_BODY:
		if (isnan(sp->t_bodyend))
			sp->t_bodyend = TIM_real();
		sp->step = STP_HTTP_ERROR;
		break;
	default:
		WRONG("Unhandled timeout step");
		break;
	}
	return (0);
}

/*--------------------------------------------------------------------
 * The very first request
 */

static int
cnt_first(struct sess *sp)
{

	sp->step = STP_START;
	return (0);
}

/*--------------------------------------------------------------------
 * START
 * Handle a request, wherever it came from recv/restart.
 */

static int
cnt_start(struct sess *sp)
{
	static int cnt = 0;

	VSC_C_main->n_req++;

	callout_init(&sp->co, 0);
	sp->url = &urls[cnt++ % num_urls];
	sp->t_start = TIM_real();
	sp->t_connstart = NAN;
	sp->t_connend = NAN;
	sp->t_fbstart = NAN;
	sp->t_fbend = NAN;
	sp->t_bodystart = NAN;
	sp->t_bodyend = NAN;
	sp->t_done = NAN;

	sp->step = STP_HTTP_START;
	return (0);
}

static int
cnt_http_start(struct sess *sp)
{
	struct srcip *sip;
	int ret;
	static int no = 0;

	/* XXX doesn't care for IPv6 at all */
	sp->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sp->fd == -1) {
		fprintf(stderr, "socket(2) error: %d %s\n", errno,
		    strerror(errno));
		sp->step = STP_DONE;
		return (0);
	}
	ret = VTCP_nonblocking(sp->fd);
	if (ret != 0) {
		fprintf(stderr, "VTCP_nonblocking() error.\n");
		sp->step = STP_HTTP_ERROR;
		return (0);
	}
	if (num_srcips > 0) {
		/* Try a source IP address in round-robin manner. */
		sip = &srcips[no++ % num_srcips];
		if (bind(sp->fd, (struct sockaddr *)&sip->sockaddr,
		    sip->sockaddrlen)) {
			fprintf(stderr, "bind(2) with %s error: %d %s\n",
			    sip->ip, errno, strerror(errno));
			sp->step = STP_HTTP_ERROR;
			return (0);
		}
	}
	sp->vsb = VSB_new_auto();
	AN(sp->vsb);
	sp->step = STP_HTTP_CONNECT;
	return (0);
}

static int
cnt_http_connect(struct sess *sp)
{
	struct url *url = sp->url;
	int ret;

	if (isnan(sp->t_connstart))
		sp->t_connstart = TIM_real();

	ret = connect(sp->fd, (struct sockaddr *)&url->sockaddr,
	    url->sockaddrlen);
	if (ret == -1) {
		if (errno != EINPROGRESS) {
			if (isnan(sp->t_connend))
				sp->t_connend = TIM_real();
			fprintf(stderr, "connect(2) error: %d %s\n", errno,
			    strerror(errno));
			sp->step = STP_HTTP_ERROR;
			return (0);
		}
		callout_reset(&sp->wrk->cb, &sp->co,
		    CALLOUT_SECTOTICKS(params->connect_timeout),
		    cnt_timeout_tick, sp);
		SES_Wait(sp, SESS_WANT_WRITE);
		return (1);
	}
	if (isnan(sp->t_connend))
		sp->t_connend = TIM_real();
	sp->step = STP_HTTP_BUILDREQ;
	return (0);
}

static const char * const nl = "\r\n";

static int
cnt_http_buildreq(struct sess *sp)
{
	const char *req = "GET";
	const char *proto = "HTTP/1.1";
	const char *url = "/1b";
	const char *hostname = "localhost";

	VSB_clear(sp->vsb);
	VSB_printf(sp->vsb, "%s %s %s%s", req, url, proto, nl);
	VSB_printf(sp->vsb, "Accept-Encoding: gzip, deflate%s", nl);
	VSB_printf(sp->vsb, "Host: %s%s", hostname, nl);
	VSB_printf(sp->vsb, "Connection: close%s", nl);
	VSB_printf(sp->vsb, "User-Agent: Mozilla/4.0"
	    " (compatible; MSIE 6.0; Windows NT 5.1; AryakaMon)%s", nl);
	VSB_cat(sp->vsb, nl);
	AZ(VSB_finish(sp->vsb));

	sp->step = STP_HTTP_TXREQ;
	return (0);
}

static int
cnt_http_txreq(struct sess *sp)
{
	struct url *url = sp->url;
	ssize_t l;

	if (isnan(sp->t_fbstart))
		sp->t_fbstart = TIM_real();

	assert(VSB_len(sp->vsb) - sp->woffset > 0);
	l = write(sp->fd, VSB_data(sp->vsb) + sp->woffset,
	    VSB_len(sp->vsb) - sp->woffset);
	if (l <= 0) {
		if (l == -1 && errno == EAGAIN)
			goto wantwrite;
		fprintf(stderr,
		    "write(2) error to %s:%s: %d %s\n",
		    url->addr, url->port, errno, strerror(errno));
		sp->step = STP_HTTP_ERROR;
		return (0);
	}
	sp->woffset += l;
	VSC_C_main->n_txbytes += l;
	if (sp->woffset != VSB_len(sp->vsb)) {
wantwrite:
		callout_reset(&sp->wrk->cb, &sp->co,
		    CALLOUT_SECTOTICKS(params->write_timeout), cnt_timeout_tick,
		    sp);
		SES_Wait(sp, SESS_WANT_WRITE);
		return (1);
	}
	sp->step = STP_HTTP_RXRESP;
	return (0);
}

static int
cnt_http_rxresp(struct sess *sp)
{

	sp->roffset = 0;
	sp->step = STP_HTTP_RXRESP_HDR;
	return (0);
}

/**********************************************************************
 * find header
 */

static char *
http_find_header(char * const *hh, const char *hdr)
{
	int n, l;
	char *r;

	l = strlen(hdr);

	for (n = 3; hh[n] != NULL; n++) {
		if (strncasecmp(hdr, hh[n], l) || hh[n][l] != ':')
			continue;
		for (r = hh[n] + l + 1; vct_issp(*r); r++)
			continue;
		return (r);
	}
	return (NULL);
}

static int
http_probe_splitheader(struct sess *sp)
{
	char *p, *q, **hh;
	int n;

	memset(sp->resphdr, 0, sizeof(sp->resphdr));
	hh = sp->resphdr;

	n = 0;
	p = sp->resp;

	/* PROTO */
	while (vct_islws(*p))
		p++;
	hh[n++] = p;
	while (!vct_islws(*p))
		p++;
	if (vct_iscrlf(*p)) {
		fprintf(stderr, "too early CRLF after PROTO\n");
		return (-1);
	}
	*p++ = '\0';

	/* STATUS */
	while (vct_issp(*p))		/* XXX: H space only */
		p++;
	if (vct_iscrlf(*p)) {
		fprintf(stderr, "too early CRLF after STATUS\n");
		return (-1);
	}
	hh[n++] = p;
	while (!vct_islws(*p))
		p++;
	if (vct_iscrlf(*p)) {
		hh[n++] = NULL;
		q = p;
		p += vct_skipcrlf(p);
		*q = '\0';
	} else {
		*p++ = '\0';
		/* MSG */
		while (vct_issp(*p))		/* XXX: H space only */
			p++;
		hh[n++] = p;
		while (!vct_iscrlf(*p))
			p++;
		q = p;
		p += vct_skipcrlf(p);
		*q = '\0';
	}
	if (n != 3) {
		fprintf(stderr, "wrong status header\n");
		return (-1);
	}

	while (*p != '\0') {
		if (n >= MAXHDR) {
			fprintf(stderr, "too long headers\n");
			return (-1);
		}
		if (vct_iscrlf(*p))
			break;
		hh[n++] = p++;
		while (*p != '\0' && !vct_iscrlf(*p))
			p++;
		q = p;
		p += vct_skipcrlf(p);
		*q = '\0';
	}
	p += vct_skipcrlf(p);
	if (*p != '\0')
		return (-1);
	return (0);
}

/*--------------------------------------------------------------------
 * Check if we have a complete HTTP request or response yet
 *
 * Return values:
 *	 0  No, keep trying
 *	>0  Yes, it is this many bytes long.
 */

static int
htc_header_complete(char *b, char *e)
{
	const char *p;

	assert(*e == '\0');
	/* Skip any leading white space */
	for (p = b ; vct_issp(*p); p++)
		continue;
	if (p == e) {
		/* All white space */
		e = b;
		*e = '\0';
		return (0);
	}
	while (1) {
		p = strchr(p, '\n');
		if (p == NULL)
			return (0);
		p++;
		if (*p == '\r')
			p++;
		if (*p == '\n')
			break;
	}
	p++;
	return (p - b);
}

static int
cnt_http_rxresp_hdr(struct sess *sp)
{
	ssize_t l;
	int i, r;

retry:
	l = read(sp->fd, sp->resp + sp->roffset,
	    sizeof(sp->resp) - sp->roffset);
	if (l <= 0) {
		if (l == -1 && errno == EAGAIN) {
			callout_reset(&sp->wrk->cb, &sp->co,
			    CALLOUT_SECTOTICKS(params->read_timeout),
			    cnt_timeout_tick, sp);
			SES_Wait(sp, SESS_WANT_READ);
			return (1);
		}
		if (isnan(sp->t_fbend))
			sp->t_fbend = TIM_real();
		if (l == 0) {
			fprintf(stderr,
			    "[ERROR] %s: read(2) error: unexpected EOF"
			    " (offset %zd)\n", __func__, sp->roffset);
			sp->step = STP_HTTP_ERROR;
			return (0);
		}
		fprintf(stderr, "[ERROR] %s: read(2) error: %d %s\n", __func__,
		    errno, strerror(errno));
		sp->step = STP_HTTP_ERROR;
		return (0);
	}
	if (isnan(sp->t_fbend))
		sp->t_fbend = TIM_real();
	VSC_C_main->n_rxbytes += l;
	sp->roffset += l;
	sp->resp[sp->roffset] = '\0';
	if (sp->roffset >= sizeof(sp->resp)) {
		fprintf(stderr, "too big header response\n");
		sp->step = STP_HTTP_ERROR;
		return (0);
	}
	i = htc_header_complete(sp->resp, sp->resp + sp->roffset);
	if (i == 0)
		goto retry;
	sp->resp[i] = '\0';
	r = http_probe_splitheader(sp);
	if (r == -1) {
		fprintf(stderr, "corrupted response header\n");
		sp->step = STP_HTTP_ERROR;
		return (0);
	}
	if (isnan(sp->t_bodystart))
		sp->t_bodystart = TIM_real();
	/* Handles sp->resp buffer remained. */
	l = sp->roffset;
	sp->roffset = 0;
	if (i < l)
		sp->roffset += l - i;
	sp->step = STP_HTTP_RXRESP_BODY;
	return (0);
}

/*--------------------------------------------------------------------
 * Reads HTTP body until the state machine got a EOF from the sender.
 * So at this moment, no parsing headers and body at all.  This
 * implementation is right now because it always attaches
 * "Connection: close" header.
 */
static int
cnt_http_rxresp_body(struct sess *sp)
{
	char buf[64 * 1024], *p;
	ssize_t l;

	while ((l = read(sp->fd, buf, sizeof(buf))) > 0)
		sp->roffset += l;
	if (l == -1) {
		if (l == -1 && errno == EAGAIN) {
			callout_reset(&sp->wrk->cb, &sp->co,
			    CALLOUT_SECTOTICKS(params->read_timeout),
			    cnt_timeout_tick, sp);
			SES_Wait(sp, SESS_WANT_READ);
			return (1);
		}
		if (isnan(sp->t_bodyend))
			sp->t_bodyend = TIM_real();
		fprintf(stderr, "read(2) error: %d %s\n", errno,
		    strerror(errno));
		sp->step = STP_HTTP_ERROR;
		return (0);
	}
	if (isnan(sp->t_bodyend))
		sp->t_bodyend = TIM_real();
	/*
	 * Got a EOF from the sender.  Checks the body length if
	 * Content-Length header exists.
	 */
	p = http_find_header(sp->resphdr, "Content-Length");
	if (p != NULL) {
		l = (ssize_t)strtoul(p, NULL, 0);
		if (l != sp->roffset) {
			fprintf(stderr,
			    "Content-Length isn't matched:"
			    " %jd / %jd\n", l, sp->roffset);
			sp->step = STP_HTTP_ERROR;
			return (0);
		}
	}
	sp->step = STP_HTTP_OK;
	return (0);
}

static int
cnt_http_ok(struct sess *sp)
{
	long int http_status;
	char *endptr = NULL;

	VSC_C_main->n_httpok++;

	if (sp->resphdr[1] != NULL) {
		errno = 0;
		http_status = strtol(sp->resphdr[1], &endptr, 10);
		if ((errno == ERANGE &&
		     (http_status == LONG_MAX ||
		      http_status == LONG_MIN)) ||
		    (errno != 0 && http_status == 0) ||
		    (sp->resphdr[1] == endptr))
			goto skip;
	}
skip:
	sp->step = STP_HTTP_DONE;
	return (0);
}

static int
cnt_http_error(struct sess *sp)
{

	VSC_C_main->n_httperror++;
	sp->step = STP_HTTP_DONE;
	return (0);
}

static int
cnt_http_done(struct sess *sp)
{
	int i;

	assert(sp->fd >= 0);
	i = close(sp->fd);
	assert(i == 0 || errno != EBADF); /* XXX EINVAL seen */
	sp->fd = -1;
	if (sp->vsb != NULL) {
		VSB_delete(sp->vsb);
		sp->vsb = NULL;
	}
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

	sp->t_done = TIM_real();
	SES_Acct(sp);

	assert(sp->fd == -1);
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

	fprintf(stdout, "thr %p STP_%s sp %p\n", (void *)pthread_self(), state,
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

		sp->stephist[sp->nstephist++ % STEPHIST_MAX] = sp->step;

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

static void
WRK_QueueInsert(struct wq *qp, struct sess *sp, int athead)
{

	if (athead)
		VTAILQ_INSERT_HEAD(&qp->queue, sp, poollist);
	else
		VTAILQ_INSERT_TAIL(&qp->queue, sp, poollist);
	qp->nqueue++;
	qp->lqueue++;
}

/*--------------------------------------------------------------------
 * Queue a workrequest if possible.
 *
 * Return zero if the request was queued, negative if it wasn't.
 */

static int
WRK_Queue(struct sess *sp, int athead)
{
	struct worker *w;
	struct wq *qp;

	qp = &wq;
	WQ_LOCK(qp);
	/* If there are idle threads, we tickle the first one into action */
	w = VTAILQ_FIRST(&qp->idle);
	if (w != NULL) {
		VTAILQ_REMOVE(&qp->idle, w, list);
		WQ_UNLOCK(qp);
		w->sp = sp;
		AZ(pthread_cond_signal(&w->cond));
		return (0);
	}
	WRK_QueueInsert(qp, sp, athead);
	WQ_UNLOCK(qp);
	return (0);
}

static int
WRK_QueueSession(struct sess *sp)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	AZ(sp->wrk);
	if (WRK_Queue(sp, 0) == 0)
		return (0);
	SES_Delete(sp);
	return (1);
}

#define	EPOLLEVENT_MAX	(64 * 1024)

static void *
WRK_thread(void *arg)
{
	struct epoll_event *ev, *ep;
	struct sess *sp;
	struct worker *w, ww;
	struct wq *qp;
	int i, n;

	CAST_OBJ_NOTNULL(qp, arg, WQ_MAGIC);

	ev = malloc(sizeof(*ev) * EPOLLEVENT_MAX);
	AN(ev);

	w = &ww;
	bzero(w, sizeof(*w));
	w->magic = WORKER_MAGIC;
	w->fd = epoll_create(1);
	assert(w->fd >= 0);
	COT_init(&w->cb);
	AZ(pthread_cond_init(&w->cond, NULL));
	w->owner = pthread_self();

	while (1) {
		CHECK_OBJ_NOTNULL(w, WORKER_MAGIC);

		COT_ticks(&w->cb);
		COT_clock(&w->cb);

		WQ_LOCK(qp);
		if (w->nwant > 0) {
			WQ_UNLOCK(qp);
			/*
			 * XXX WRONG EVENT-MODEL.  With current approach, CPU
			 * burning could happen if
			 *
			 *  - There are some latency between the perf server and
			 *    the target server.
			 *  - The queue length is very long.
			 */
			n = epoll_wait(w->fd, ev, EPOLLEVENT_MAX, 0);
			for (ep = ev, i = 0; i < n; i++, ep++) {
				CAST_OBJ_NOTNULL(sp, ep->data.ptr, SESS_MAGIC);
				assert(w == sp->wrk);
				sp->wrk = NULL;
				callout_stop(&w->cb, &sp->co);
				EVT_Del(w, sp->fd);
				sp->wrk = w;
				CNT_Session(sp);
			}
			WQ_LOCK(qp);
			if (VTAILQ_EMPTY(&qp->queue)) {
				WQ_UNLOCK(qp);
				continue;
			}
		}
		AZ(w->sp);
		if ((w->sp = VTAILQ_FIRST(&qp->queue)) != NULL) {
			/* Process queued requests, if any */
			VTAILQ_REMOVE(&qp->queue, w->sp, poollist);
			qp->lqueue--;
		} else {
			assert(w->nwant == 0);
			VTAILQ_INSERT_HEAD(&qp->idle, w, list);
			Lck_CondWait(&w->cond, &qp->mtx);
		}
		if (w->sp == NULL) {
			WQ_UNLOCK(qp);
			break;
		}
		WQ_UNLOCK(qp);

		AZ(w->sp->wrk);
		w->sp->wrk = w;
		CNT_Session(w->sp);
		w->sp = NULL;
	}

	assert(0 == 1);
	NEEDLESS_RETURN(NULL);
}

/*--------------------------------------------------------------------*/

static double
TIM_real(void)
{
	struct timespec ts;

	assert(clock_gettime(CLOCK_REALTIME, &ts) == 0);
	return (ts.tv_sec + 1e-9 * ts.tv_nsec);
}

static void
TIM_format(double t, char *p)
{
	struct tm tm;
	time_t tt;

	tt = (time_t) t;
	(void)gmtime_r(&tt, &tm);
	AN(strftime(p, TIM_FORMAT_SIZE, "%H:%M:%S", &tm));
}

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

static void
EVT_Add(struct worker *wrk, int want, int fd, void *arg)
{
	struct epoll_event ev;

	assert(pthread_equal(wrk->owner, pthread_self()));

	AN(arg);
	ev.data.ptr = arg;
	ev.events = EPOLLERR;
	switch (want) {
	case SESS_WANT_READ:
		ev.events |= EPOLLIN | EPOLLPRI;
		break;
	case SESS_WANT_WRITE:
		ev.events |= EPOLLOUT;
		break;
	default:
		WRONG("Unknown event type");
		break;
	}
	AZ(epoll_ctl(wrk->fd, EPOLL_CTL_ADD, fd, &ev));
	assert(wrk->nwant >= 0);
	wrk->nwant++;
}

static void
EVT_Del(struct worker *wrk, int fd)
{
	struct epoll_event ev = { 0 , { 0 } };

	assert(pthread_equal(wrk->owner, pthread_self()));

	assert(fd >= 0);
	AZ(epoll_ctl(wrk->fd, EPOLL_CTL_DEL, fd, &ev));
	assert(wrk->nwant > 0);
	wrk->nwant--;
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
	sp->mysockaddr = (void*)(&sm->sockaddr[1]);
	sp->mysockaddrlen = sizeof(sm->sockaddr[1]);
	sp->mysockaddr->ss_family = PF_UNSPEC;
}

static void
SES_Acct(struct sess *sp)
{
	double diff;

	if (!isnan(sp->t_connstart) &&
	    !isnan(sp->t_connend)) {
		diff = sp->t_connend - sp->t_connstart;
		VSC_C_1s->n_conn++;
		VSC_C_1s->t_conntotal += diff;
		VSC_C_1s->t_connmin = MIN(VSC_C_1s->t_connmin, diff);
		VSC_C_1s->t_connmax = MAX(VSC_C_1s->t_connmax, diff);
		VSC_C_main->t_conntotal += diff;
	}
	if (!isnan(sp->t_fbstart) &&
	    !isnan(sp->t_fbend)) {
		diff = sp->t_fbend - sp->t_fbstart;
		VSC_C_1s->n_fb++;
		VSC_C_1s->t_fbtotal += diff;
		VSC_C_1s->t_fbmin = MIN(VSC_C_1s->t_fbmin, diff);
		VSC_C_1s->t_fbmax = MAX(VSC_C_1s->t_fbmax, diff);
		VSC_C_main->t_fbtotal += diff;
	}
	if (!isnan(sp->t_bodystart) &&
	    !isnan(sp->t_bodyend)) {
		diff = sp->t_bodyend - sp->t_bodystart;
		VSC_C_1s->n_body++;
		VSC_C_1s->t_bodytotal += diff;
		VSC_C_1s->t_bodymin = MIN(VSC_C_1s->t_bodymin, diff);
		VSC_C_1s->t_bodymax = MAX(VSC_C_1s->t_bodymax, diff);
		VSC_C_main->t_bodytotal += diff;
	}
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
		Lck_Lock(&ses_mem_mtx);
		ses_qp = 1 - ses_qp;
		Lck_Unlock(&ses_mem_mtx);
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
	/* no lock needed */
	n_sess_grab++;
	Lck_Lock(&ses_stat_mtx);
	VSC_C_main->n_sess = n_sess_grab - n_sess_rel;
	Lck_Unlock(&ses_stat_mtx);

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
	Lck_Lock(&ses_mem_mtx);
	VTAILQ_INSERT_HEAD(&ses_free_mem[1 - ses_qp], sm, list);
	Lck_Unlock(&ses_mem_mtx);

	/* Update statistics */
	Lck_Lock(&ses_stat_mtx);
	n_sess_rel++;
	VSC_C_main->n_sess = n_sess_grab - n_sess_rel;
	Lck_Unlock(&ses_stat_mtx);
}

static void
SES_Wait(struct sess *sp, int want)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->wrk, WORKER_MAGIC);
	assert(sp->fd >= 0);
	EVT_Add(sp->wrk, want, sp->fd, sp);
}

/*--------------------------------------------------------------------
 * Schedule a session back on a work-thread from its pool
 */

static int
SES_Schedule(struct sess *sp)
{

	sp->wrk = NULL;
	if (WRK_Queue(sp, 1))
		WRONG("failed to schedule the session");
	return (0);
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
SCH_hdr(void)
{

	/* XXX WG: I'm sure you didn't use your brain. */
	fprintf(stdout, "[STAT] "
	    " time    | total    | req   |"
	    " connect time          |"
	    " first byte time       |"
	    " body time             |"
	    " tx         | tx    | rx         | rx    | errors\n");
	fprintf(stdout, "[STAT] "
	    "         |          |       |"
	    "   min     avg     max |"
	    "   min     avg     max |"
	    "   min     avg     max |"
	    "            |       |            |       |\n");
	fprintf(stdout, "[STAT] "
	    "---------+----------+-------+"
	    "-----------------------+"
	    "-----------------------+"
	    "-----------------------+"
	    "------------+-------+------------+-------+-------....\n");
}

static void
SCH_bottom(void)
{

	fprintf(stdout, "[STAT] "
	    "---------+----------+-------+"
	    "-----------------------+"
	    "-----------------------+"
	    "-----------------------+"
	    "------------+-------+------------+-------+-------....\n");
}

static void
SCH_stat(void)
{
	static struct perfstat prev = { 0, };
	static int first = 1;
	double now = TIM_real();
	char buf[TIM_FORMAT_SIZE], sbuf[5];

	if (first == 1) {
		SCH_hdr();
		first = 0;
	}

	TIM_format(now - boottime, buf);
	fprintf(stdout, "[STAT] %s", buf);
	fprintf(stdout, " | %8jd", VSC_C_main->n_req);
	fprintf(stdout, " | %5jd", VSC_C_main->n_req - prev.n_req);
	fprintf(stdout, " | %2.3f / %2.3f / %2.3f", VSC_C_1s->t_connmin,
	    VSC_C_1s->t_conntotal / VSC_C_1s->n_conn, VSC_C_1s->t_connmax);
	fprintf(stdout, " | %2.3f / %2.3f / %2.3f", VSC_C_1s->t_fbmin,
	    VSC_C_1s->t_fbtotal / VSC_C_1s->n_fb, VSC_C_1s->t_fbmax);
	fprintf(stdout, " | %2.3f / %2.3f / %2.3f", VSC_C_1s->t_bodymin,
	    VSC_C_1s->t_bodytotal / VSC_C_1s->n_body, VSC_C_1s->t_bodymax);
	fprintf(stdout, " | %10ju", VSC_C_main->n_txbytes - prev.n_txbytes);
	humanize_number(sbuf, sizeof(sbuf),
	    (int64_t)(VSC_C_main->n_txbytes - prev.n_txbytes), "",
	    HN_AUTOSCALE, HN_NOSPACE | HN_DECIMAL);
 	fprintf(stdout, " | %5s", sbuf);
	fprintf(stdout, " | %10ju", VSC_C_main->n_rxbytes - prev.n_rxbytes);
	humanize_number(sbuf, sizeof(sbuf),
	    (int64_t)(VSC_C_main->n_rxbytes - prev.n_rxbytes), "",
	    HN_AUTOSCALE, HN_NOSPACE | HN_DECIMAL);
 	fprintf(stdout, " | %5s", sbuf);
	fprintf(stdout, " | %jd\n", VSC_C_main->n_timeout);

	prev = *VSC_C_main;
	bzero(VSC_C_1s, sizeof(*VSC_C_1s));
	VSC_C_1s->t_connmin = 1000.;
	VSC_C_1s->t_connmax = 0.;
	VSC_C_1s->t_fbmin = 1000.;
	VSC_C_1s->t_fbmax = 0.;
	VSC_C_1s->t_bodymin = 1000.;
	VSC_C_1s->t_bodymax = 0.;
}

static void
SCH_summary(void)
{

	SCH_bottom();

	fprintf(stdout, "[STAT] Summary:\n");
#define FMT_u64 "[STAT]    %-10ju %-10s # %s\n"
#define FMT_dbl "[STAT]    %-10f %-10s # %s\n"
#define	PERFSTAT_u64(a, b, c, d)	\
	fprintf(stdout, FMT_u64, VSC_C_main->a, d, c);
#define	PERFSTAT_dbl(a, b, c, d)	\
	fprintf(stdout, FMT_dbl, VSC_C_main->a, d, c);
#include "stats.h"
#undef PERFSTAT
#undef FMT_dbl
#undef FMT_u64

/*
Total: connections 34184 requests 34166 replies 33894 test-duration 1.388 s

Connection rate: 24626.9 conn/s (0.0 ms/conn, <=300 concurrent connections)
Connection time [ms]: min 0.7 avg 11.6 max 99.9 median 2.5 stddev 19.0
Connection time [ms]: connect 0.5
Connection length [replies/conn]: 1.000

Request rate: 24613.9 req/s (0.0 ms/req)
Request size [B]: 66.0

Reply rate [replies/s]: min 0.0 avg 0.0 max 0.0 stddev 0.0 (0 samples)
Reply time [ms]: response 11.1 transfer 0.0
Reply size [B]: header 328.0 content 1.0 footer 0.0 (total 329.0)
Reply status: 1xx=0 2xx=33894 3xx=0 4xx=0 5xx=0
        200=33894

CPU time [s]: user 0.23 system 1.16 (user 16.6% system 83.3% total 99.9%)
Net I/O: 9455.5 KB/s (77.5*10^6 bps)

Errors: total 0 client-timo 0 socket-timo 0 connrefused 0 connreset 0
Errors: fd-unavail 0 addrunavail 0 ftab-full 0 drop 0 incompleted 0
Errors: closednoresp 0 sslerror 0 sslerror_syscall 0 other 0
*/
}

static void
SCH_tick_1s(void *arg)
{
	struct sched *scp;
	struct sess *sp;
	int i;

	CAST_OBJ_NOTNULL(scp, arg, SCHED_MAGIC);

	for (i = 0; i < r_arg; i++) {
		if (VSC_C_main->n_sess >= r_arg) {
			VSC_C_main->n_hitlimit++;
			break;
		}
		sp = SES_New();
		AN(sp);
		AZ(WRK_QueueSession(sp));
	}

	SCH_stat();

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
	callout_reset(&scp->cb, &scp->co, CALLOUT_SECTOTICKS(1),
	    SCH_tick_1s, &sc);
	while (!stop) {
		COT_ticks(&scp->cb);
		COT_clock(&scp->cb);
		TIM_sleep(0.1);
	}
	SCH_summary();
	NEEDLESS_RETURN(NULL);
}

static void
PEF_sigint(int no)
{

	(void)no;
	stop = 1;
}

static void
PEF_Init(void)
{

	boottime = TIM_real();
	Lck_New(&ses_mem_mtx, "Session Memory");
	Lck_New(&ses_stat_mtx, "Session Statistics");
}

static void
PEF_Run(void)
{
	pthread_t tp[c_arg], schedtp;
	int i;

	bzero(&wq, sizeof(wq));
	wq.magic = WQ_MAGIC;
	Lck_New(&wq.mtx, "WorkQueue lock");
	VTAILQ_INIT(&wq.idle);
	VTAILQ_INIT(&wq.queue);

	for (i = 0; i < c_arg; i++)
		AZ(pthread_create(&tp[i], NULL, WRK_thread, &wq));
	AZ(pthread_create(&schedtp, NULL, SCH_thread, &wq));
	AZ(pthread_join(schedtp, NULL));
}

/*--------------------------------------------------------------------*/

static void
SIP_readfile(const char* file)
{
	struct sockaddr_in *sin4;
	FILE* fp;
	char line[5000];

	fp = fopen(file, "r");
	if (fp == NULL) {
		perror(file);
		exit(1);
	}

	fprintf(stdout, "[INFO] Reading %s SRCIP file.\n", file);

	max_srcips = 100;
	srcips = (struct srcip *)malloc(max_srcips * sizeof(struct srcip));
	num_srcips = 0;
	while (fgets(line, sizeof(line), fp) != (char*) 0) {
		/* Nuke trailing newline. */
		if (line[strlen(line) - 1] == '\n')
			line[strlen(line) - 1] = '\0';
		if (strlen(line) <= 0)
			continue;
		/* Check for room in srcips. */
		if (num_srcips >= max_srcips) {
			max_srcips *= 2;
			srcips = (struct srcip *)realloc((void *)srcips,
			    max_srcips * sizeof(struct srcip));
		}
		/* Add to table. */
		srcips[num_srcips].ip = strdup(line);
		AN(srcips[num_srcips].ip);
		(void)memset((void *)&srcips[num_srcips].sockaddr, 0,
		    sizeof(srcips[num_srcips].sockaddr));
		sin4 = (struct sockaddr_in *)&srcips[num_srcips].sockaddr;
		if (!inet_aton(srcips[num_srcips].ip, &sin4->sin_addr)) {
			(void)fprintf(stderr,
			    "[ERROR] cannot convert source IP address %s\n",
			    srcips[num_srcips].ip);
			exit(1);
		}
		srcips[num_srcips].sockaddrlen = sizeof(*sin4);
		++num_srcips;
	}

	fprintf(stdout, "[INFO] Total %d SRCIP are loaded from %s file.\n",
	    num_srcips, file);
}

static void
URL_resolv(int n)
{
	struct hostent *he;
	struct sockaddr_in *sin4;
	struct url *url;

	url = urls + n;
	bzero(&url->sockaddr, sizeof(url->sockaddr));
	url->sockaddrlen = sizeof(*sin4);
	/* XXX IPv6 */
	he = gethostbyname(url->hostname);
	if (he == NULL) {
		(void)fprintf(stderr, "unknown host - %s\n", url->hostname);
		exit(1);
	}
	sin4 = (struct sockaddr_in *)&url->sockaddr;
	sin4->sin_family = he->h_addrtype;
	(void)memmove(&sin4->sin_addr, he->h_addr, he->h_length);
	sin4->sin_port = htons(url->portnum);

	VTCP_name(&url->sockaddr, url->sockaddrlen,
	    url->addr, sizeof url->addr, url->port, sizeof url->port);
}

static void
URL_readfile(const char *file)
{
	FILE *fp;
	const char *http = "http://";
	char line[5000], hostname[5000];
	int http_len = strlen(http);
	int proto_len, host_len;
	char *cp;

	fp = fopen(file, "r");
	if (fp == NULL) {
		perror(file);
		exit(1);
	}

	fprintf(stdout, "[INFO] Reading %s URL file.\n", file);

	max_urls = 100;
	urls = (struct url *)malloc(max_urls * sizeof(struct url));
	num_urls = 0;
	while (fgets(line, sizeof(line), fp) != (char*) 0) {
		if (line[strlen(line) - 1] == '\n')
			line[strlen(line) - 1] = '\0';
		if (num_urls >= max_urls) {
			max_urls *= 2;
			urls = (struct url *)realloc((void *)urls,
			    max_urls * sizeof(struct url));
		}
		if (strlen(line) <= 0)
			continue;
		urls[num_urls].magic = URL_MAGIC;
		urls[num_urls].url_str = strdup(line);
		AN(urls[num_urls].url_str);
		if (strncmp(http, line, http_len) == 0) {
			proto_len = http_len;
		} else {
			(void)fprintf(stderr, "unknown protocol - %s\n",
			    line);
			exit(1);
		}
		for (cp = line + proto_len;
		     *cp != '\0' && *cp != ':' && *cp != '/'; ++cp)
			;
		host_len = cp - line;
		host_len -= proto_len;
		strncpy(hostname, line + proto_len, host_len);
		hostname[host_len] = '\0';
		urls[num_urls].hostname = strdup(hostname);
		AN(urls[num_urls].hostname);
		if (*cp == ':') {
			urls[num_urls].portnum = (unsigned short)atoi(++cp);
			while (*cp != '\0' && *cp != '/')
				++cp;
		} else
			urls[num_urls].portnum = 80;
		if (*cp == '\0') 
			urls[num_urls].path = strdup("/");
		else
			urls[num_urls].path = strdup(cp);
		AN(urls[num_urls].path);
		URL_resolv(num_urls);
		++num_urls;
	}

	fprintf(stdout, "[INFO] Total %d URLs are loaded from %s file.\n",
	    num_urls, file);
}

static void
usage(void)
{

	fprintf(stderr, "usage: varnishperf [options] urlfile\n");
#define FMT "    %-28s # %s\n"
	fprintf(stderr, FMT, "-c N", "Sets number of threads");
	fprintf(stderr, FMT, "-r N", "Sets rate.");
	exit(1);
}

int
main(int argc, char *argv[])
{
	int ch;
	char *end;
	const char *s_arg = NULL;

	while ((ch = getopt(argc, argv, "c:r:s:")) != -1) {
		switch (ch) {
		case 'c':
			errno = 0;
			c_arg = strtoul(optarg, &end, 10);
			if (errno == ERANGE || end == optarg || *end) {
				fprintf(stderr, "illegal number for -c\n");
				exit(1);
			}
			break;
		case 'r':
			errno = 0;
			r_arg = strtoul(optarg, &end, 10);
			if (errno == ERANGE || end == optarg || *end) {
				fprintf(stderr, "illegal number for -r\n");
				exit(1);
			}
			break;
		case 's':
			s_arg = optarg;
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	(void)signal(SIGINT, PEF_sigint);
	(void)signal(SIGPIPE, SIG_IGN);

	LCK_Init();

	if (s_arg != NULL)
		SIP_readfile(s_arg);
	for (;argc > 0; argc--, argv++)
		URL_readfile(*argv);
	if (num_urls == 0) {
		fprintf(stderr, "No URLs found.\n");
		exit(1);
	}
	PEF_Init();
	PEF_Run();
	return (0);
}
