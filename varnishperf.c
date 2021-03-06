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
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
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

struct parspec;
struct worker;
struct wq;

/*--------------------------------------------------------------------*/

typedef void tweak_t(const struct parspec *, const char *arg);

struct parspec {
	const char	*name;
	tweak_t		*func;
	volatile void	*priv;
	double		min;
	double		max;
	const char	*descr;
	const char	*def;
	const char	*units;
};

static int nparspec;
static struct parspec const ** parspec;
static int margin;

/*--------------------------------------------------------------------*/

struct params {
	/* Timeouts */
	unsigned		connect_timeout;
	unsigned		read_timeout;
	unsigned		write_timeout;

	unsigned		diag_bitmap;

	unsigned		sess_workspace;
	unsigned		linger;
};
static struct params		master;
static struct params		*params;

/*--------------------------------------------------------------------
 * XXX some variables are protected by ses_stat_mtx but others are
 * not.
 */

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
#define	PEFSTAT_STATUS_MAX	1000
	int			n_status[PEFSTAT_STATUS_MAX];
	int			n_statusother;
#define	PERFSTAT_u64(a, b, c, d)	uint64_t a;
#define	PERFSTAT_dbl(a, b, c, d)	double a;
#include "stats.h"
#undef PERFSTAT_dbl
#undef PERFSTAT_u64
};
static struct perfstat		_perfstat;
static struct perfstat		*VSC_C_main = &_perfstat;

/*--------------------------------------------------------------------*/

struct cmds;

#define CMD_ARGS \
    char * const *av, void *priv, const struct cmds *cmd

typedef void cmd_f(CMD_ARGS);

struct cmds {
	const char	*name;
	cmd_f		*cmd;
};

struct vss_addr {
	int			 va_family;
	int			 va_socktype;
	int			 va_protocol;
	socklen_t		 va_addrlen;
	struct sockaddr_storage	 va_addr;
};

struct url {
	unsigned		magic;
#define	URL_MAGIC		0x3178c2cb

	struct vsb		*vsb;
	struct vss_addr		**vaddr;
	int			nvaddr;

	char			addr[VTCP_ADDRBUFSIZE];
	char			port[VTCP_PORTBUFSIZE];

	VTAILQ_ENTRY(url)	list;
};
static VTAILQ_HEAD(, url)	url_list = VTAILQ_HEAD_INITIALIZER(url_list);
static struct url		**urls;
static int			num_urls;

struct srcip {
	char			*ip;
	struct sockaddr_storage sockaddr;
	int			sockaddrlen;
};
static struct srcip		*srcips;
static int			num_srcips;
static int			max_srcips;

/*--------------------------------------------------------------------
 * Pointer aligment magic
 */

#define PALGN		(sizeof(void *) - 1)
#define PAOK(p)		(((uintptr_t)(p) & PALGN) == 0)
#define PRNDDN(p)	((uintptr_t)(p) & ~PALGN)
#define PRNDUP(p)	(((uintptr_t)(p) + PALGN) & ~PALGN)

/*--------------------------------------------------------------------*/

typedef struct {
	char			*b;
	char			*e;
} txt;

/*--------------------------------------------------------------------
 * HTTP Protocol connection structure
 */

struct http_conn {
	unsigned		magic;
#define HTTP_CONN_MAGIC		0x3e19edd1

	int			fd;
	unsigned		maxbytes;
	struct ws		*ws;
	txt			rxbuf;
	txt			pipeline;
};

/*--------------------------------------------------------------------
 * Workspace structure for quick memory allocation.
 */

struct ws {
	unsigned		magic;
#define WS_MAGIC		0x35fac554
	unsigned		overflow;	/* workspace overflowed */
	const char		*id;		/* identity */
	char			*s;		/* (S)tart of buffer */
	char			*f;		/* (F)ree pointer */
	char			*r;		/* (R)eserved length */
	char			*e;		/* (E)nd of buffer */
};

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
	unsigned		flags;
#define	SESS_F_EOF		(1 << 0)
	struct worker		*wrk;

	enum step		prevstep;
	enum step		step;
	int			fd;

	int			calls;

#ifdef VARNISHPERF_DEBUG
#define	STEPHIST_MAX		64
	enum step		stephist[STEPHIST_MAX];
	int			nstephist;
#endif

	struct url		*url;

	socklen_t		mysockaddrlen;
	struct sockaddr_storage	*mysockaddr;

	struct callout		co;

	ssize_t			cl;		/* Content Length */
	ssize_t			roffset;
	ssize_t			woffset;
	struct http_conn	htc;
#define	SESS_NOBUFSIZ		32
	char			*nobuf;		/* For chunked-encoding */
	ssize_t			nooffset;
	ssize_t			no;
	struct ws		ws[1];
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
static VTAILQ_HEAD(, sess)	waiting_list =
    VTAILQ_HEAD_INITIALIZER(waiting_list);
static struct lock		waiting_mtx;

/*--------------------------------------------------------------------*/

struct sessmem {
	unsigned		magic;
#define SESSMEM_MAGIC		0x555859c5

	struct sess		sess;
	unsigned		workspace;
	void			*wsp;
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
	int			queue[2];
	VTAILQ_ENTRY(worker)	list;
};
static VTAILQ_HEAD(, worker)	workers = VTAILQ_HEAD_INITIALIZER(workers);
static struct lock		workers_mtx;

/*--------------------------------------------------------------------*/

/*
 * Limits the number of total connections.  If c_arg is 0, it points unlimited.
 */
static uint32_t	c_arg;
/*
 * Sets the number of requests per a connection.
 */
static int	C_arg = 1;
/*
 * Limits the TCP-established connections.  If this value is 0, it means it's
 * unlimited.  If it's not 0, total number of connections will be limited to
 * this value.
 */
static int	m_arg = 0;
/*
 * Sets rate.  This option will pointing how many requests will be scheduled
 * per a second.
 */
static int	r_arg = 1;
/*
 * Sets the concurrent number of threads.  Default is 1 indicating one thread
 * would be invoked.
 */
static int	t_arg = 1;
/*
 * Shows all statistic fields.  If stat value is zero, default behaviour is
 * that it'd not be shown.
 */
static int	z_flag = 0;
/*
 * Boot-up time from TIM_real().
 */
static double	boottime;
/*
 * Default value is 0 but 1 if SIGINT is delivered.
 */
static int	stop;
static int	verbose;

static void	EVT_Add(struct worker *wrk, int want, int fd, void *arg);
static void	EVT_Del(struct worker *wrk, int fd);
static void	SES_Acct(struct sess *sp);
static void	SES_Delete(struct sess *sp);
static void	SES_Rush(void);
static int	SES_Schedule(struct sess *sp);
static void	SES_Sleep(struct sess *sp);
static void	SES_Wait(struct sess *sp, int want);
static void	SES_errno(int error);
static double	TIM_real(void);

/*--------------------------------------------------------------------*/

static inline void
Tcheck(const txt t)
{

	AN(t.b);
	AN(t.e);
	assert(t.b <= t.e);
}

/*--------------------------------------------------------------------
 * unsigned length of a txt
 */

static inline unsigned
Tlen(const txt t)
{

	Tcheck(t);
	return ((unsigned)(t.e - t.b));
}

/*--------------------------------------------------------------------
 * A normal pointer difference is signed, but we never want a negative
 * value so this little tool will make sure we don't get that.
 */

static inline unsigned
pdiff(const void *b, const void *e)
{

	assert(b <= e);
	return
	    ((unsigned)((const unsigned char *)e - (const unsigned char *)b));
}

/*--------------------------------------------------------------------*/

static void
WS_Assert(const struct ws *ws)
{

	CHECK_OBJ_NOTNULL(ws, WS_MAGIC);
	if (params->diag_bitmap & 0x8)
		fprintf(stdout, "WS(%p = (%s, %p %u %u %u)",
		    ws, ws->id, ws->s, pdiff(ws->s, ws->f),
		    ws->r == NULL ? 0 : pdiff(ws->f, ws->r),
		    pdiff(ws->s, ws->e));
	assert(ws->s != NULL);
	assert(PAOK(ws->s));
	assert(ws->e != NULL);
	assert(PAOK(ws->e));
	assert(ws->s < ws->e);
	assert(ws->f >= ws->s);
	assert(ws->f <= ws->e);
	assert(PAOK(ws->f));
	if (ws->r) {
		assert(ws->r > ws->s);
		assert(ws->r <= ws->e);
		assert(PAOK(ws->r));
	}
}

static char *
WS_Alloc(struct ws *ws, unsigned bytes)
{
	char *r;

	WS_Assert(ws);
	bytes = PRNDUP(bytes);

	assert(ws->r == NULL);
	if (ws->f + bytes > ws->e) {
		ws->overflow++;
		WS_Assert(ws);
		return(NULL);
	}
	r = ws->f;
	ws->f += bytes;
	if (params->diag_bitmap & 0x8)
		fprintf(stdout, "WS_Alloc(%p, %u) = %p", ws, bytes, r);
	WS_Assert(ws);
	return (r);
}

static unsigned
WS_Reserve(struct ws *ws, unsigned bytes)
{
	unsigned b2;

	WS_Assert(ws);
	assert(ws->r == NULL);
	if (bytes == 0)
		b2 = ws->e - ws->f;
	else if (bytes > ws->e - ws->f)
		b2 = ws->e - ws->f;
	else
		b2 = bytes;
	b2 = PRNDDN(b2);
	xxxassert(ws->f + b2 <= ws->e);
	ws->r = ws->f + b2;
	if (params->diag_bitmap & 0x8)
		fprintf(stdout, "WS_Reserve(%p, %u/%u) = %u",
		    ws, b2, bytes, pdiff(ws->f, ws->r));
	WS_Assert(ws);
	return (pdiff(ws->f, ws->r));
}

static void
WS_ReleaseP(struct ws *ws, char *ptr)
{

	WS_Assert(ws);
	if (params->diag_bitmap & 0x8)
		fprintf(stdout, "WS_ReleaseP(%p, %p)", ws, ptr);
	assert(ws->r != NULL);
	assert(ptr >= ws->f);
	assert(ptr <= ws->r);
	ws->f += PRNDUP(ptr - ws->f);
	ws->r = NULL;
	WS_Assert(ws);
}

static void
WS_Init(struct ws *ws, const char *id, void *space, unsigned len)
{

	if (params->diag_bitmap & 0x8)
		fprintf(stdout,
		    "WS_Init(%p, \"%s\", %p, %u)", ws, id, space, len);
	assert(space != NULL);
	memset(ws, 0, sizeof *ws);
	ws->magic = WS_MAGIC;
	ws->s = space;
	assert(PAOK(space));
	ws->e = ws->s + len;
	assert(PAOK(len));
	ws->f = ws->s;
	ws->id = id;
	WS_Assert(ws);
}

/*
 * Reset a WS to start or a given pointer, likely from WS_Snapshot
 */

static void
WS_Reset(struct ws *ws, char *p)
{

	WS_Assert(ws);
	if (params->diag_bitmap & 0x8)
		fprintf(stdout, "WS_Reset(%p, %p)", ws, p);
	assert(ws->r == NULL);
	if (p == NULL)
		ws->f = ws->s;
	else {
		assert(p >= ws->s);
		assert(p < ws->e);
		ws->f = p;
	}
	WS_Assert(ws);
}

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

/*--------------------------------------------------------------------
 * Check if we have a complete HTTP request or response yet
 *
 * Return values:
 *	 0  No, keep trying
 *	>0  Yes, it is this many bytes long.
 */

static int
htc_header_complete(txt *t)
{
	const char *p;

	Tcheck(*t);
	assert(*t->e == '\0');
	/* Skip any leading white space */
	for (p = t->b ; vct_issp(*p); p++)
		continue;
	if (p == t->e) {
		/* All white space */
		t->e = t->b;
		*t->e = '\0';
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
	return (p - t->b);
}

/*--------------------------------------------------------------------*/

static void
HTC_Init(struct http_conn *htc, struct ws *ws, int fd, unsigned maxbytes)
{

	htc->magic = HTTP_CONN_MAGIC;
	htc->ws = ws;
	htc->fd = fd;
	htc->maxbytes = maxbytes;

	(void)WS_Reserve(htc->ws, htc->maxbytes);
	htc->rxbuf.b = ws->f;
	htc->rxbuf.e = ws->f;
	*htc->rxbuf.e = '\0';
	htc->pipeline.b = NULL;
	htc->pipeline.e = NULL;
}

/*--------------------------------------------------------------------
 * Return 1 if we have a complete HTTP procol header
 */

static int
HTC_Complete(struct http_conn *htc)
{
	int i;

	CHECK_OBJ_NOTNULL(htc, HTTP_CONN_MAGIC);
	i = htc_header_complete(&htc->rxbuf);
	assert(i >= 0);
	if (i == 0)
		return (0);
	WS_ReleaseP(htc->ws, htc->rxbuf.e);
	AZ(htc->pipeline.b);
	AZ(htc->pipeline.e);
	if (htc->rxbuf.b + i < htc->rxbuf.e) {
		htc->pipeline.b = htc->rxbuf.b + i;
		htc->pipeline.e = htc->rxbuf.e;
		htc->rxbuf.e = htc->pipeline.b;
	}
	return (1);
}

/*--------------------------------------------------------------------
 * Receive more HTTP protocol bytes
 */

static int
HTC_Rx(struct http_conn *htc)
{
	int i;

	CHECK_OBJ_NOTNULL(htc, HTTP_CONN_MAGIC);
	AN(htc->ws->r);
	i = (htc->ws->r - htc->rxbuf.e) - 1;	/* space for NUL */
	if (i <= 0) {
		WS_ReleaseP(htc->ws, htc->rxbuf.b);
		return (-1);
	}
	i = read(htc->fd, htc->rxbuf.e, i);
	if (i == -1) {
		if (errno != EAGAIN)
			WS_ReleaseP(htc->ws, htc->rxbuf.b);
		return (-2);
	}
	if (i == 0) {
		WS_ReleaseP(htc->ws, htc->rxbuf.b);
		return (-3);
	}
	VSC_C_main->n_rxbytes += i;
	htc->rxbuf.e += i;
	*htc->rxbuf.e = '\0';
	return (HTC_Complete(htc));
}

/*--------------------------------------------------------------------
 * Read up to len bytes, returning pipelined data first.
 */

static ssize_t
HTC_Read(struct http_conn *htc, void *d, size_t len)
{
	size_t l;
	unsigned char *p;
	ssize_t i;

	CHECK_OBJ_NOTNULL(htc, HTTP_CONN_MAGIC);
	l = 0;
	p = d;
	if (htc->pipeline.b) {
		l = Tlen(htc->pipeline);
		if (l > len)
			l = len;
		memcpy(p, htc->pipeline.b, l);
		p += l;
		len -= l;
		htc->pipeline.b += l;
		if (htc->pipeline.b == htc->pipeline.e)
			htc->pipeline.b = htc->pipeline.e = NULL;
		assert(l > 0);
		return (l);
	}
	if (len == 0)
		return (l);
	i = read(htc->fd, p, len);
	if (i < 0)
		return (i);
	VSC_C_main->n_rxbytes += i;
	return (i + l);
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
	case STP_HTTP_RXRESP_CL:
	case STP_HTTP_RXRESP_CHUNKED_NO:
	case STP_HTTP_RXRESP_CHUNKED_BODY:
	case STP_HTTP_RXRESP_CHUNKED_CRLF:
	case STP_HTTP_RXRESP_EOF:
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
 * Handle a request.
 */

static int
cnt_start(struct sess *sp)
{
	static int cnt = 0;

	callout_init(&sp->co, 0);
	sp->url = urls[cnt++ % num_urls];
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

	/* XXX doesn't care for IPv6 at all */
	sp->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sp->fd == -1) {
		SES_errno(errno);
		if (params->diag_bitmap & 0x2)
			fprintf(stdout, "[ERROR] socket(2) error: %d %s\n",
			    errno, strerror(errno));
		sp->step = STP_DONE;
		return (0);
	}
	sp->step = STP_HTTP_WAIT;
	return (0);
}

/*--------------------------------------------------------------------
 * We want to get out of any kind of trouble-hit TCP connections as fast
 * as absolutely possible, so we set them LINGER enabled with zero timeout,
 * so that even if there are outstanding write data on the socket, a close(2)
 * will return immediately.
 */
static const struct linger linger = {
	.l_onoff	=	0,
};

static int
cnt_http_wait(struct sess *sp)
{
	struct srcip *sip;
	int ret, val = 1;
	static int no = 0;

	Lck_Lock(&ses_stat_mtx);
	if (m_arg != 0 && VSC_C_main->n_conn >= m_arg) {
		Lck_Unlock(&ses_stat_mtx);
		SES_Sleep(sp);
		return (1);
	}
	VSC_C_main->n_conntotal++;
	VSC_C_main->n_conn++;
	Lck_Unlock(&ses_stat_mtx);
	ret = VTCP_nonblocking(sp->fd);
	if (ret != 0) {
		fprintf(stdout, "[ERROR] VTCP_nonblocking() error.\n");
		sp->step = STP_HTTP_ERROR;
		return (0);
	}
	if (params->linger)
		AZ(setsockopt(sp->fd, SOL_SOCKET, SO_LINGER, &linger,
			sizeof linger));
	/* Disable Nagle algorithm for pipelining requests.  */
        AZ(setsockopt(sp->fd, SOL_TCP, TCP_NODELAY, &val, sizeof(val)));
	if (num_srcips > 0) {
		/* Try a source IP address in round-robin manner. */
		sip = &srcips[no++ % num_srcips];
		if (bind(sp->fd, (struct sockaddr *)&sip->sockaddr,
		    sip->sockaddrlen)) {
			SES_errno(errno);
			if (params->diag_bitmap & 0x2)
				fprintf(stdout,
				    "[ERROR] bind(2) with %s error: %d %s\n",
				    sip->ip, errno, strerror(errno));
			sp->step = STP_HTTP_ERROR;
			return (0);
		}
	}
	sp->step = STP_HTTP_CONNECT;
	return (0);
}

static int
cnt_http_connect(struct sess *sp)
{
	struct url *url = sp->url;
	struct vss_addr *vaddr;
	int ret;

	if (isnan(sp->t_connstart))
		sp->t_connstart = TIM_real();

	assert(url->nvaddr > 0);
	vaddr = url->vaddr[0];		/* XXX always use the first */

	ret = connect(sp->fd, (struct sockaddr *)&vaddr->va_addr,
	    vaddr->va_addrlen);
	if (ret == -1) {
		if (errno != EINPROGRESS) {
			if (isnan(sp->t_connend))
				sp->t_connend = TIM_real();
			SES_errno(errno);
			if (params->diag_bitmap & 0x2)
				fprintf(stdout,
				    "[ERROR] connect(2) error: %d %s\n", errno,
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
	sp->step = STP_HTTP_TXREQ_INIT;
	return (0);
}

/*--------------------------------------------------------------------
 * Check that there is still something at the far end of a given socket.
 * We poll the fd with instant timeout, if there are any events we can't
 * use it (backends are not allowed to pipeline).
 */

static int
vbe_CheckFd(int fd)
{
	struct pollfd pfd;

	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	return (poll(&pfd, 1, 0) == 0);
}

static int
cnt_http_txreq_init(struct sess *sp)
{

	if (!vbe_CheckFd(sp->fd)) {
		fprintf(stdout,
		    "[ERROR] socket is closed or its buffer isn't empty.\n");
		sp->step = STP_HTTP_ERROR;
		return (0);
	}
	WS_Reset(sp->ws, NULL);
	sp->woffset = 0;
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

	assert(VSB_len(url->vsb) - sp->woffset > 0);
	l = write(sp->fd, VSB_data(url->vsb) + sp->woffset,
	    VSB_len(url->vsb) - sp->woffset);
	if (l <= 0) {
		if (l == -1 && errno == EAGAIN)
			goto wantwrite;
		SES_errno(errno);
		if (params->diag_bitmap & 0x2)
			fprintf(stdout,
			    "write(2) error: %d %s\n", errno, strerror(errno));
		sp->step = STP_HTTP_ERROR;
		return (0);
	}
	sp->woffset += l;
	VSC_C_main->n_txbytes += l;
	if (sp->woffset != VSB_len(url->vsb)) {
wantwrite:
		callout_reset(&sp->wrk->cb, &sp->co,
		    CALLOUT_SECTOTICKS(params->write_timeout), cnt_timeout_tick,
		    sp);
		SES_Wait(sp, SESS_WANT_WRITE);
		return (1);
	}
	VSC_C_main->n_req++;
	sp->calls++;
	sp->step = STP_HTTP_RXRESP;
	return (0);
}

static int
cnt_http_rxresp(struct sess *sp)
{

	HTC_Init(&sp->htc, sp->ws, sp->fd, /* XXX */4096);
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
	struct http_conn *htc = &sp->htc;
	char *p, *q, **hh;
	int n;

	memset(sp->resphdr, 0, sizeof(sp->resphdr));
	hh = sp->resphdr;

	n = 0;
	p = htc->rxbuf.b;

	/* PROTO */
	while (vct_islws(*p))
		p++;
	hh[n++] = p;
	while (!vct_islws(*p))
		p++;
	if (vct_iscrlf(*p)) {
		VSC_C_main->n_tooearlycrlf++;
		fprintf(stdout, "[ERROR] too early CRLF after PROTO\n");
		return (-1);
	}
	*p++ = '\0';

	/* STATUS */
	while (vct_issp(*p))		/* XXX: H space only */
		p++;
	if (vct_iscrlf(*p)) {
		VSC_C_main->n_tooearlycrlf++;
		fprintf(stdout, "[ERROR] too early CRLF after STATUS\n");
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
		VSC_C_main->n_wrongstatus++;
		fprintf(stdout, "[ERROR] wrong status header\n");
		return (-1);
	}

	while (*p != '\0') {
		if (n >= MAXHDR) {
			VSC_C_main->n_toolonghdr++;
			fprintf(stdout, "[ERROR] too long headers\n");
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
	if (p != htc->rxbuf.e)
		return (-1);
	return (0);
}

static int
cnt_http_rxresp_hdr(struct sess *sp)
{
	int l, r;
	char *end, *p;

retry:
	l = HTC_Rx(&sp->htc);
	switch (l) {
	case -1:
		VSC_C_main->n_toolonghdr++;
		if (params->diag_bitmap & 0x2)
			fprintf(stdout, "[ERROR] too big header response\n");
		sp->step = STP_HTTP_ERROR;
		return (0);
	case -2:
		if (errno == EAGAIN) {
			callout_reset(&sp->wrk->cb, &sp->co,
			    CALLOUT_SECTOTICKS(params->read_timeout),
			    cnt_timeout_tick, sp);
			SES_Wait(sp, SESS_WANT_READ);
			return (1);
		}
		if (isnan(sp->t_fbend))
			sp->t_fbend = TIM_real();
		SES_errno(errno);
		if (params->diag_bitmap & 0x2)
			fprintf(stdout, "[ERROR] %s: read(2) error: %d %s\n",
			    __func__, errno, strerror(errno));
		sp->step = STP_HTTP_ERROR;
		return (0);
	case -3:
		if (isnan(sp->t_fbend))
			sp->t_fbend = TIM_real();
		SES_errno(0);
		if (params->diag_bitmap & 0x2)
			fprintf(stdout,
			    "[ERROR] %s: read(2) error: unexpected EOF\n",
			    __func__);
		sp->step = STP_HTTP_ERROR;
		return (0);
	default:
		if (l == 0)
			goto retry;
		assert(l > 0);
	}
	if (isnan(sp->t_fbend))
		sp->t_fbend = TIM_real();
	r = http_probe_splitheader(sp);
	if (r == -1) {
		VSC_C_main->n_wrongres++;
		if (params->diag_bitmap & 0x2)
			fprintf(stdout, "[ERROR] corrupted response header\n");
		sp->step = STP_HTTP_ERROR;
		return (0);
	}
	if (isnan(sp->t_bodystart))
		sp->t_bodystart = TIM_real();
	p = http_find_header(sp->resphdr, "Content-Length");
	if (p != NULL) {
		errno = 0;
		sp->cl = strtoul(p, &end, 0);
		if (errno == ERANGE || end == p || *end)
			assert(0 == 1);
		VSC_C_main->n_resstraight++;
		sp->step = STP_HTTP_RXRESP_CL;
		return (0);
	}
	p = http_find_header(sp->resphdr, "Transfer-Encoding");
	if (p != NULL && !strcmp(p, "chunked")) {
		VSC_C_main->n_reschunked++;
		sp->nobuf = WS_Alloc(sp->ws, SESS_NOBUFSIZ);
		AN(sp->nobuf);
		sp->nooffset = 0;
		sp->step = STP_HTTP_RXRESP_CHUNKED_INIT;
		return (0);
	}
	VSC_C_main->n_reseof++;
	sp->flags |= SESS_F_EOF;
	sp->step = STP_HTTP_RXRESP_EOF;
	return (0);
}

static int
cnt_http_rxresp_cl(struct sess *sp)
{
	char buf[64 * 1024];
	ssize_t l;

	while (sp->roffset < sp->cl) {
		l = HTC_Read(&sp->htc, buf,
		    MIN(sizeof(buf), sp->cl - sp->roffset));
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
			SES_errno(errno);
			if (params->diag_bitmap & 0x2)
				fprintf(stdout,
				    "[ERROR] read(2) error: %d %s\n", errno,
				    strerror(errno));
			sp->step = STP_HTTP_ERROR;
			return (0);
		}
		sp->roffset += l;
		assert(sp->roffset <= sp->cl);
	}
	if (isnan(sp->t_bodyend))
		sp->t_bodyend = TIM_real();
	sp->step = STP_HTTP_OK;
	return (0);
}

static int
cnt_http_rxresp_chunked_init(struct sess *sp)
{

	AN(sp->nobuf);
	sp->nooffset = 0;
	sp->step = STP_HTTP_RXRESP_CHUNKED_NO;
	return (0);
}

static int
cnt_http_rxresp_chunked_no(struct sess *sp)
{
	ssize_t l;
	char *end;

retry:
	l = HTC_Read(&sp->htc, sp->nobuf + sp->nooffset, 1);
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
		SES_errno(errno);
		if (params->diag_bitmap & 0x2)
			fprintf(stdout,
			    "[ERROR] read(2) error: %d %s\n", errno,
			    strerror(errno));
		sp->step = STP_HTTP_ERROR;
		return (0);
	}
	assert(l == 1);
	sp->nooffset += l;
	assert(sp->nooffset < SESS_NOBUFSIZ);
	sp->nobuf[sp->nooffset] = '\0';
	if (sp->nobuf[sp->nooffset - 1] != '\n')
		goto retry;
	errno = 0;
	sp->no = strtoul(sp->nobuf, &end, 16);
	if (errno == ERANGE || end == sp->nobuf)
		assert(0 == 1);
	sp->nooffset = 0;
	sp->step = STP_HTTP_RXRESP_CHUNKED_BODY;
	return (0);
}

static int
cnt_http_rxresp_chunked_body(struct sess *sp)
{
	char buf[64 * 1024];
	ssize_t l;

	while (sp->nooffset < sp->no) {
		l = HTC_Read(&sp->htc, buf,
		    MIN(sizeof(buf), sp->no - sp->nooffset));
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
			SES_errno(errno);
			if (params->diag_bitmap & 0x2)
				fprintf(stdout,
				    "[ERROR] read(2) error: %d %s\n", errno,
				    strerror(errno));
			sp->step = STP_HTTP_ERROR;
			return (0);
		}
		sp->nooffset += l;
		assert(sp->nooffset <= sp->no);
	}
	assert(sp->nooffset == sp->no);
	sp->nooffset = 0;
	sp->step = STP_HTTP_RXRESP_CHUNKED_CRLF;
	return (0);
}

static int
cnt_http_rxresp_chunked_crlf(struct sess *sp)
{
	ssize_t l;

retry:
	l = HTC_Read(&sp->htc, sp->nobuf + sp->nooffset, 1);
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
		SES_errno(errno);
		if (params->diag_bitmap & 0x2)
			fprintf(stdout,
			    "[ERROR] read(2) error: %d %s\n", errno,
			    strerror(errno));
		sp->step = STP_HTTP_ERROR;
		return (0);
	}
	assert(l == 1);
	sp->nooffset += l;
	assert(sp->nooffset < SESS_NOBUFSIZ);
	sp->nobuf[sp->nooffset] = '\0';
	if (sp->nooffset != 2)
		goto retry;
	assert(sp->nooffset <= 2);
	if (sp->no != 0) {
		sp->step = STP_HTTP_RXRESP_CHUNKED_INIT;
		return (0);
	}
	if (isnan(sp->t_bodyend))
		sp->t_bodyend = TIM_real();
	sp->step = STP_HTTP_OK;
	return (0);
}

static int
cnt_http_rxresp_eof(struct sess *sp)
{
	char buf[64 * 1024];
	ssize_t l;

	while (1) {
		l = HTC_Read(&sp->htc, buf, sizeof(buf));
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
			SES_errno(errno);
			if (params->diag_bitmap & 0x2)
				fprintf(stdout,
				    "[ERROR] read(2) error: %d %s\n", errno,
				    strerror(errno));
			sp->step = STP_HTTP_ERROR;
			return (0);
		}
		if (l == 0)
			break;
		sp->roffset += l;
	}
	if (isnan(sp->t_bodyend))
		sp->t_bodyend = TIM_real();
	sp->step = STP_HTTP_OK;
	return (0);
}

static int
cnt_http_ok(struct sess *sp)
{
	long int http_status;
	int v;
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
		if (http_status >= PEFSTAT_STATUS_MAX)
			VSC_C_main->n_statusother++;
		else {
			v = (int)http_status;
			VSC_C_main->n_status[v]++;
			/*
			 * XXX WG: common on weongyo... Use your brain more!
			 */
			switch (v / 100) {
			case 0:
				VSC_C_main->n_status_0xx++;
				break;
			case 1:
				VSC_C_main->n_status_1xx++;
				break;
			case 2:
				VSC_C_main->n_status_2xx++;
				break;
			case 3:
				VSC_C_main->n_status_3xx++;
				break;
			case 4:
				VSC_C_main->n_status_4xx++;
				break;
			case 5:
				VSC_C_main->n_status_5xx++;
				break;
			case 6:
				VSC_C_main->n_status_6xx++;
				break;
			case 7:
				VSC_C_main->n_status_7xx++;
				break;
			case 8:
				VSC_C_main->n_status_8xx++;
				break;
			case 9:
				VSC_C_main->n_status_9xx++;
				break;
			default:
				WRONG("[CRIT] Unexpected value...");
			}
		}
	}
skip:
	if ((sp->flags & SESS_F_EOF) == 0 && sp->calls < C_arg && !stop) {
		sp->step = STP_HTTP_TXREQ_INIT;
		callout_reset(&sp->wrk->cb, &sp->co,
		    CALLOUT_SECTOTICKS(params->write_timeout), cnt_timeout_tick,
		    sp);
		SES_Wait(sp, SESS_WANT_WRITE);
		return (1);
	}
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

	Lck_Lock(&ses_stat_mtx);
	VSC_C_main->n_conn--;
	if (m_arg != 0)
		SES_Rush();
	Lck_Unlock(&ses_stat_mtx);

	assert(sp->fd >= 0);
	i = close(sp->fd);
	assert(i == 0 || errno != EBADF); /* XXX EINVAL seen */
	sp->fd = -1;
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

#ifdef VARNISHPERF_DEBUG
		sp->stephist[sp->nstephist++ % STEPHIST_MAX] = sp->step;
#endif

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
 */

static int
WRK_Queue(struct sess *sp)
{
	struct worker *w;
	ssize_t l;

	AZ(sp->wrk);
	/* LRU */
	Lck_Lock(&workers_mtx);
	w = VTAILQ_FIRST(&workers);
	AN(w);
	VTAILQ_REMOVE(&workers, w, list);
	VTAILQ_INSERT_TAIL(&workers, w, list);
	Lck_Unlock(&workers_mtx);
	/* Queue on the pipe */
	l = write(w->queue[1], &sp, sizeof(sp));
	if (l == -1) {
		if (errno == EAGAIN)
			return (-2);
		return (-1);
	}
	assert(l == sizeof(sp));
	return (0);
}

static void
WRK_Init(struct worker *w)
{
	int i;

	bzero(w, sizeof(*w));
	w->magic = WORKER_MAGIC;
	w->fd = epoll_create(1);
	assert(w->fd >= 0);
	COT_init(&w->cb);

	AZ(pipe(w->queue));
	i = fcntl(w->queue[0], F_GETFL);
	assert(i != -1);
	i |= O_NONBLOCK;
	i = fcntl(w->queue[0], F_SETFL, i);
	assert(i != -1);
	i = fcntl(w->queue[1], F_GETFL);
	assert(i != -1);
	i |= O_NONBLOCK;
	i = fcntl(w->queue[1], F_SETFL, i);
	assert(i != -1);

	EVT_Add(w, SESS_WANT_READ, w->queue[0], w);
}

static void
WRK_Fini(struct worker *w)
{

	AZ(close(w->queue[0]));
	AZ(close(w->queue[1]));
	COT_fini(&w->cb);
	AZ(close(w->fd));
}

static void
wrk_handleQueue(struct worker *w)
{
	ssize_t l;

	l = read(w->queue[0], &w->sp, sizeof(w->sp));
	assert(l == sizeof(w->sp));

	AZ(w->sp->wrk);
	w->sp->wrk = w;
	CNT_Session(w->sp);
	w->sp = NULL;
}

#define	EPOLLEVENT_MAX	(64 * 1024)

static void *
WRK_thread(void *arg)
{
	struct epoll_event *ev, *ep;
	struct sess *sp;
	struct worker *w;
	int i, n;

	CAST_OBJ_NOTNULL(w, arg, WORKER_MAGIC);
	w->owner = pthread_self();

	ev = malloc(sizeof(*ev) * EPOLLEVENT_MAX);
	AN(ev);

	while (!stop) {
		CHECK_OBJ_NOTNULL(w, WORKER_MAGIC);

		COT_ticks(&w->cb);
		COT_clock(&w->cb);

		n = epoll_wait(w->fd, ev, EPOLLEVENT_MAX, 1000);
		for (ep = ev, i = 0; i < n; i++, ep++) {
			if (ep->data.ptr == w) {
				wrk_handleQueue(w);
				continue;
			}
			CAST_OBJ_NOTNULL(sp, ep->data.ptr, SESS_MAGIC);
			assert(w == sp->wrk);
			sp->wrk = NULL;
			callout_stop(&w->cb, &sp->co);
			EVT_Del(w, sp->fd);
			sp->wrk = w;
			CNT_Session(sp);
		}
	}

	if (params->diag_bitmap & 0x4)
		fprintf(stdout, "[INFO] Finishing the worker thread.\n");
	free(ev);
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
	unsigned l;

	l = sizeof(*sm) + params->sess_workspace;
	sm = malloc(l);
	if (sm == NULL)
		return (NULL);
	sm->magic = SESSMEM_MAGIC;
	sm->workspace = params->sess_workspace;
	sm->wsp = (void *)(sm + 1);
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
	WS_Init(sp->ws, "sess workspace", sm->wsp, sm->workspace);
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
	if (WRK_Queue(sp))
		WRONG("failed to schedule the session");
	return (0);
}

static void
SES_Rush(void)
{
	struct sess *sp;

	Lck_Lock(&waiting_mtx);
	sp = VTAILQ_FIRST(&waiting_list);
	if (sp != NULL) {
		VTAILQ_REMOVE(&waiting_list, sp, poollist);
		Lck_Unlock(&waiting_mtx);
		AZ(WRK_Queue(sp));
		return;
	}
	Lck_Unlock(&waiting_mtx);
}

static void
SES_Sleep(struct sess *sp)
{

	sp->wrk = NULL;
	Lck_Lock(&waiting_mtx);
	VTAILQ_INSERT_TAIL(&waiting_list, sp, poollist);
	Lck_Unlock(&waiting_mtx);
}

static void
SES_errno(int error)
{

	switch (error) {
	case 0:
		VSC_C_main->n_eof++;
		break;
	case EADDRINUSE:
		VSC_C_main->n_eaddrinuse++;
		break;
	case EMFILE:
		VSC_C_main->n_emfile++;
		break;
	case ECONNREFUSED:
		VSC_C_main->n_econnrefused++;
		break;
	case ECONNRESET:
		VSC_C_main->n_econnreset++;
		break;
	default:
		fprintf(stderr, "[ERROR] Unexpected error number: %d\n", error);
		break;
	}
}

/*--------------------------------------------------------------------*/

struct sched {
	unsigned		magic;
#define	SCHED_MAGIC		0x5c43a3af
	struct callout		co;
	struct callout_block	cb;
};

static void
SCH_hdr(void)
{

	/* XXX WG: I'm sure you didn't use your brain. */
	fprintf(stdout, "[STAT] "
	    " time    | total    | req    | conn           |"
	    " connect time          |"
	    " first byte time       |"
	    " body time             |"
	    " tx         | tx    | rx         | rx    | errors\n");
	fprintf(stdout, "[STAT] "
	    "         |          |        | active   total |"
	    "   min     avg     max |"
	    "   min     avg     max |"
	    "   min     avg     max |"
	    "            |       |            |       |\n");
	fprintf(stdout, "[STAT] "
	    "---------+----------+--------+----------------+"
	    "-----------------------+"
	    "-----------------------+"
	    "-----------------------+"
	    "------------+-------+------------+-------+-------....\n");
}

static void
SCH_bottom(void)
{

	/* XXX WG: I'm sure you didn't use your brain. */
	fprintf(stdout, "[STAT] "
	    "---------+----------+--------+----------------+"
	    "-----------------------+"
	    "-----------------------+"
	    "-----------------------+"
	    "------------+-------+------------+-------+-------....\n");
}

static void
SCH_stat(void)
{
	static struct perfstat prev = { { 0, }, };
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
	fprintf(stdout, " | %6jd", VSC_C_main->n_req - prev.n_req);
	fprintf(stdout, " | %5jd / %6jd", VSC_C_main->n_conn,
	    VSC_C_main->n_conntotal - prev.n_conntotal);

	if (VSC_C_1s->t_connmin == 1000.0)
		fprintf(stdout, " |    na");
	else
		fprintf(stdout, " | %2.3f", VSC_C_1s->t_connmin);
	if (VSC_C_1s->n_conn == 0)
		fprintf(stdout, " /    na");
	else
		fprintf(stdout, " / %2.3f",
		    VSC_C_1s->t_conntotal / VSC_C_1s->n_conn);
	if (VSC_C_1s->t_connmax == -1.0)
		fprintf(stdout, " /    na");
	else
		fprintf(stdout, " / %2.3f", VSC_C_1s->t_connmax);

	if (VSC_C_1s->t_fbmin == 1000.0)
		fprintf(stdout, " |    na");
	else
		fprintf(stdout, " | %2.3f", VSC_C_1s->t_fbmin);
	if (VSC_C_1s->n_fb == 0)
		fprintf(stdout, " /    na");
	else
		fprintf(stdout, " / %2.3f",
		    VSC_C_1s->t_fbtotal / VSC_C_1s->n_fb);
	if (VSC_C_1s->t_fbmax == -1.0)
		fprintf(stdout, " /    na");
	else
		fprintf(stdout, " / %2.3f", VSC_C_1s->t_fbmax);

	if (VSC_C_1s->t_bodymin == 1000.0)
		fprintf(stdout, " |    na");
	else
		fprintf(stdout, " | %2.3f", VSC_C_1s->t_bodymin);
	if (VSC_C_1s->n_body == 0)
		fprintf(stdout, " /    na");
	else
		fprintf(stdout, " / %2.3f",
		    VSC_C_1s->t_bodytotal / VSC_C_1s->n_body);
	if (VSC_C_1s->t_bodymax == -1.0)
		fprintf(stdout, " /    na");
	else
		fprintf(stdout, " / %2.3f", VSC_C_1s->t_bodymax);

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
	fprintf(stdout, " | %jd / %jd\n", VSC_C_main->n_timeout,
	    VSC_C_main->n_econnreset);

	/* Reset and Prepare */
	prev = *VSC_C_main;
	bzero(VSC_C_1s, sizeof(*VSC_C_1s));
	VSC_C_1s->t_connmin = 1000.;
	VSC_C_1s->t_connmax = -1.0;
	VSC_C_1s->t_fbmin = 1000.;
	VSC_C_1s->t_fbmax = -1.0;
	VSC_C_1s->t_bodymin = 1000.;
	VSC_C_1s->t_bodymax = -1.0;
}

static void
SCH_tick_1s(void *arg)
{
	struct sched *scp;
	struct sess *sp;
	int i, r;

	CAST_OBJ_NOTNULL(scp, arg, SCHED_MAGIC);

	SCH_stat();

	for (i = 0; i < r_arg && !stop; i++) {
		if (VSC_C_main->n_sess >= r_arg) {
			VSC_C_main->n_hitlimit++;
			break;
		}
		if (c_arg != 0) {
			if (n_sess_rel >= c_arg) {
				stop = 1;
				break;
			}
			if (n_sess_grab >= c_arg)
				break;
		}
		sp = SES_New();
		AN(sp);
		while ((r = WRK_Queue(sp)) != 0 && !stop) {
			if (r == -2) {
				/*
				 * XXX pipe is full with our sp pointers so need to
				 * yield until workers eat some of queue.
				 */
				TIM_sleep(0.1);
				continue;
			}
			if (r == -1) {
				fprintf(stdout, "WRK_Queue failed: %d %s\n",
				    errno, strerror(errno));
				SES_Delete(sp);
				goto fail;
			}
		}
	}
fail:
	callout_reset(&scp->cb, &scp->co, CALLOUT_SECTOTICKS(1), SCH_tick_1s,
	    arg);
}

static void *
SCH_thread(void *arg)
{
	struct sched sc, *scp;

	(void)arg;
	scp = &sc;
	bzero(scp, sizeof(*scp));
	scp->magic = SCHED_MAGIC;
	COT_init(&scp->cb);
	callout_init(&scp->co, 0);
	callout_reset(&scp->cb, &scp->co, CALLOUT_SECTOTICKS(0),
	    SCH_tick_1s, &sc);
	while (!stop) {
		COT_ticks(&scp->cb);
		COT_clock(&scp->cb);
		TIM_sleep(0.1);
	}
	if (params->diag_bitmap & 0x4)
		printf("[INFO] Finishing the scheduler thread.\n");
	callout_stop(&scp->cb, &scp->co);
	COT_fini(&scp->cb);
	NEEDLESS_RETURN(NULL);
}

/*--------------------------------------------------------------------*/

static void
PEF_summary(void)
{

	SCH_bottom();

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

	fprintf(stdout, "[STAT] Summary:\n");
#define FMT_u64 "[STAT]    %-20ju %-10s %c # %s\n"
#define FMT_dbl "[STAT]    %-20f %-10s %c # %s\n"
#define	PERFSTAT_u64(a, b, c, d)	do {				\
	if (z_flag == 0 && VSC_C_main->a == 0)				\
		break;							\
	fprintf(stdout, FMT_u64, VSC_C_main->a, d, b, c);		\
} while (0);
#define	PERFSTAT_dbl(a, b, c, d)	do {				\
	if (z_flag == 0 && VSC_C_main->a == 0.)				\
		break;							\
	fprintf(stdout, FMT_dbl, VSC_C_main->a, d, b, c);		\
} while (0);
#include "stats.h"
#undef PERFSTAT
#undef FMT_dbl
#undef FMT_u64
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
	Lck_New(&waiting_mtx, "waiting list lock");
	Lck_New(&ses_mem_mtx, "Session Memory");
	Lck_New(&ses_stat_mtx, "Session Statistics");
}

static void
PEF_Run(void)
{
	struct worker w[t_arg];
	pthread_t tp[t_arg], schedtp;
	int i;

	Lck_New(&workers_mtx, "workers list mtx");

	for (i = 0; i < t_arg; i++) {
		WRK_Init(&w[i]);
		Lck_Lock(&workers_mtx);
		VTAILQ_INSERT_TAIL(&workers, &w[i], list);
		Lck_Unlock(&workers_mtx);
		AZ(pthread_create(&tp[i], NULL, WRK_thread, &w[i]));
		VSC_C_main->n_worker++;
	}
	AZ(pthread_create(&schedtp, NULL, SCH_thread, NULL));
	if (params->diag_bitmap & 0x4)
		fprintf(stdout, "[INFO] Joining the scheduler thread\n");
	AZ(pthread_join(schedtp, NULL));
	for (i = 0; i < t_arg; i++) {
		if (params->diag_bitmap & 0x4)
			fprintf(stdout, "[INFO] Joining the worker thread\n");
		AZ(pthread_join(tp[i], NULL));
		WRK_Fini(&w[i]);
	}
	PEF_summary();
}

/*--------------------------------------------------------------------*/

static const struct parspec *
mcf_findpar(const char *name)
{
	int i;

	for (i = 0; i < nparspec; i++)
		if (!strcmp(parspec[i]->name, name))
			return (parspec[i]);
	return (NULL);
}

/*--------------------------------------------------------------------
 * Add a group of parameters to the global set and sort by name.
 */

static int
parspec_cmp(const void *a, const void *b)
{
	struct parspec * const * pa = a;
	struct parspec * const * pb = b;
	return (strcmp((*pa)->name, (*pb)->name));
}

static void
MCF_AddParams(const struct parspec *ps)
{
	const struct parspec *pp;
	int n;

	n = 0;
	for (pp = ps; pp->name != NULL; pp++) {
		if (mcf_findpar(pp->name) != NULL)
			fprintf(stderr, "Duplicate param: %s\n", pp->name);
		if (strlen(pp->name) + 1 > margin)
			margin = strlen(pp->name) + 1;
		n++;
	}
	parspec = realloc(parspec, (1L + nparspec + n) * sizeof *parspec);
	XXXAN(parspec);
	for (pp = ps; pp->name != NULL; pp++)
		parspec[nparspec++] = pp;
	parspec[nparspec] = NULL;
	qsort (parspec, nparspec, sizeof parspec[0], parspec_cmp);
}

/*--------------------------------------------------------------------
 * Set defaults for all parameters
 */

static void
MCF_SetDefaults(void)
{
	const struct parspec *pp;
	int i;

	for (i = 0; i < nparspec; i++) {
		pp = parspec[i];
		fprintf(stdout,
		    "[INFO] Set Default for %s = %s\n", pp->name, pp->def);
		pp->func(pp, pp->def);
	}
}

/*--------------------------------------------------------------------*/

static void
MCF_ParamSet(const char *param, const char *val)
{
	const struct parspec *pp;

	pp = mcf_findpar(param);
	if (pp != NULL) {
		pp->func(pp, val);
		return;
	}
	fprintf(stderr, "[ERROR] Unknown parameter \"%s\".", param);
	exit(1);
}

/*--------------------------------------------------------------------*/

static void
tweak_diag_bitmap(const struct parspec *par, const char *arg)
{
	unsigned u;

	(void)par;
	if (arg != NULL) {
		u = strtoul(arg, NULL, 0);
		master.diag_bitmap = u;
	} else {
		fprintf(stderr, "0x%x", master.diag_bitmap);
	}
}

/*--------------------------------------------------------------------*/

static void
tweak_generic_timeout(volatile unsigned *dst, const char *arg)
{
	unsigned u;

	if (arg != NULL) {
		u = strtoul(arg, NULL, 0);
		if (u == 0) {
			fprintf(stdout,
			    "[ERROR] Timeout must be greater than zero\n");
			exit(2);
		}
		*dst = u;
	} else
		fprintf(stdout, "%u", *dst);
}

/*--------------------------------------------------------------------*/

static void
tweak_timeout(const struct parspec *par, const char *arg)
{
	volatile unsigned *dest;

	dest = par->priv;
	tweak_generic_timeout(dest, arg);
}

/*--------------------------------------------------------------------*/

static void
tweak_generic_bool(volatile unsigned *dest, const char *arg)
{
	if (arg != NULL) {
		if (!strcasecmp(arg, "off"))
			*dest = 0;
		else if (!strcasecmp(arg, "disable"))
			*dest = 0;
		else if (!strcasecmp(arg, "no"))
			*dest = 0;
		else if (!strcasecmp(arg, "false"))
			*dest = 0;
		else if (!strcasecmp(arg, "on"))
			*dest = 1;
		else if (!strcasecmp(arg, "enable"))
			*dest = 1;
		else if (!strcasecmp(arg, "yes"))
			*dest = 1;
		else if (!strcasecmp(arg, "true"))
			*dest = 1;
		else {
			fprintf(stdout, "[ERROR] use \"on\" or \"off\"\n");
			exit(2);
		}
	} else
		fprintf(stdout, "%s", *dest ? "on" : "off");
}

/*--------------------------------------------------------------------*/

static void
tweak_bool(const struct parspec *par, const char *arg)
{
	volatile unsigned *dest;

	dest = par->priv;
	tweak_generic_bool(dest, arg);
}

/*--------------------------------------------------------------------*/

static void
tweak_generic_uint(volatile unsigned *dest, const char *arg,
    unsigned min, unsigned max)
{
	unsigned u;

	if (arg != NULL) {
		if (!strcasecmp(arg, "unlimited"))
			u = UINT_MAX;
		else
			u = strtoul(arg, NULL, 0);
		if (u < min) {
			fprintf(stdout, "[ERROR] Must be at least %u\n", min);
			exit(2);
		}
		if (u > max) {
			fprintf(stdout, "[ERROR] Must be no more than %u\n", max);
			exit(2);
		}
		*dest = u;
	} else if (*dest == UINT_MAX) {
		fprintf(stdout, "unlimited");
	} else {
		fprintf(stdout, "%u", *dest);
	}
}

/*--------------------------------------------------------------------*/

static void
tweak_uint(const struct parspec *par, const char *arg)
{
	volatile unsigned *dest;

	dest = par->priv;
	tweak_generic_uint(dest, arg, (uint)par->min, (uint)par->max);
}

/*--------------------------------------------------------------------*/

static const struct parspec input_parspec[] = {
	{ "connect_timeout", tweak_timeout,
		&master.connect_timeout, 0, UINT_MAX,
		"Default connection timeout for backend connections. "
		"We only try to connect to the backend for this many "
		"seconds before giving up. ",
		"3", "seconds" },
	{ "diag_bitmap", tweak_diag_bitmap, 0, 0, 0,
		"Bitmap controlling diagnostics code:\n"
		"  0x00000001 - CNT_Session states.\n"
		"  0x00000002 - socket error messages.\n"
		"  0x00000004 - thread mgt.\n"
		"  0x00000008 - workspace.\n"
		"Use 0x notation and do the bitor in your head :-)\n",
		"0", "bitmap" },
	{ "linger", tweak_bool, &master.linger, 0, 0,
		"Sets the linger.",
		"off", "bool" },
	{ "read_timeout", tweak_timeout,
		&master.read_timeout, 1, UINT_MAX,
		"Default timeout for receiving bytes from target. "
		"We only wait for this many seconds for bytes "
		"before giving up.",
		"6", "seconds" },
	{ "sess_workspace", tweak_uint, &master.sess_workspace, 1024, UINT_MAX,
		"Bytes of HTTP protocol workspace allocated for sessions. "
		"This space must be big enough for the entire HTTP protocol "
		"header.\n"
		"Minimum is 1024 bytes.",
		"4096", "bytes" },
	{ "write_timeout", tweak_timeout, &master.write_timeout, 0, 0,
		"Send timeout for client connections. "
		"If the HTTP response hasn't been transmitted in this many\n"
		"seconds the session is closed. \n",
		"6", "seconds" },
	{ NULL, NULL, NULL }
};

static void
MCF_ParamInit(void)
{

	MCF_AddParams(input_parspec);
	MCF_SetDefaults();
	params = &master;
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

	fprintf(stdout, "[INFO] Reading \"%s\" SRCIP file.\n", file);

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
			(void)fprintf(stdout,
			    "[ERROR] cannot convert source IP address %s\n",
			    srcips[num_srcips].ip);
			exit(1);
		}
		srcips[num_srcips].sockaddrlen = sizeof(*sin4);
		++num_srcips;
	}

	fprintf(stdout, "[INFO] Total %d SRCIP are loaded from \"%s\" file.\n",
	    num_srcips, file);
}

/**********************************************************************
 * Read a file into memory
 */

#define	MAX_FILESIZE	(1024 * 1024)

static char *
read_file(const char *fn)
{
	char *buf;
	ssize_t sz = MAX_FILESIZE;
	ssize_t s;
	int fd;

	fd = open(fn, O_RDONLY);
	if (fd < 0)
		return (NULL);
	buf = malloc(sz);
	assert(buf != NULL);
	s = read(fd, buf, sz - 1);
	if (s <= 0) {
		free(buf);
		return (NULL);
	}
	AZ(close(fd));
	assert(s < sz);		/* XXX: increase MAX_FILESIZE */
	buf[s] = '\0';
	buf = realloc(buf, s + 1);
	assert(buf != NULL);
	return (buf);
}

static int
VAV_BackSlash(const char *s, char *res)
{
	int r;
	char c;
	unsigned u;

	assert(*s == '\\');
	r = c = 0;
	switch(s[1]) {
	case 'n':
		c = '\n';
		r = 2;
		break;
	case 'r':
		c = '\r';
		r = 2;
		break;
	case 't':
		c = '\t';
		r = 2;
		break;
	case '"':
		c = '"';
		r = 2;
		break;
	case '\\':
		c = '\\';
		r = 2;
		break;
	case '0': case '1': case '2': case '3':
	case '4': case '5': case '6': case '7':
		for (r = 1; r < 4; r++) {
			if (!vct_isdigit(s[r]))
				break;
			if (s[r] - '0' > 7)
				break;
			c <<= 3;	/*lint !e701 signed left shift */
			c |= s[r] - '0';
		}
		break;
	case 'x':
		if (1 == sscanf(s + 1, "x%02x", &u)) {
			assert(!(u & ~0xff));
			c = u;	/*lint !e734 loss of precision */
			r = 4;
		}
		break;
	default:
		break;
	}
	if (res != NULL)
		*res = c;
	return (r);
}

/**********************************************************************
 * Macro facility
 */

/* Safe printf into a fixed-size buffer */
#define bprintf(buf, fmt, ...)						\
	do {								\
		assert(snprintf(buf, sizeof buf, fmt, __VA_ARGS__)	\
		    < sizeof buf);					\
	} while (0)

/* Safe printf into a fixed-size buffer */
#define vbprintf(buf, fmt, ap)						\
	do {								\
		assert(vsnprintf(buf, sizeof buf, fmt, ap)		\
		    < sizeof buf);					\
	} while (0)


struct macro {
	VTAILQ_ENTRY(macro)	list;
	char			*name;
	char			*val;
};

static VTAILQ_HEAD(,macro) macro_list = VTAILQ_HEAD_INITIALIZER(macro_list);

static struct lock		macro_mtx;

static void
init_macro(void)
{

	Lck_New(&macro_mtx, "macro lock");
}

static char *
macro_get(const char *b, const char *e)
{
	struct macro *m;
	int l;
	char *retval = NULL;

	l = e - b;

	if (l == 4 && !memcmp(b, "date", l)) {
		double t = TIM_real();
		retval = malloc(64);
		AN(retval);
		TIM_format(t, retval);
		return (retval);
	}

	Lck_Lock(&macro_mtx);
	VTAILQ_FOREACH(m, &macro_list, list)
		if (!memcmp(b, m->name, l) && m->name[l] == '\0')
			break;
	if (m != NULL)
		retval = strdup(m->val);
	Lck_Unlock(&macro_mtx);
	return (retval);
}

static struct vsb *
macro_expand(const char *text)
{
	struct vsb *vsb;
	const char *p, *q;
	char *m;

	vsb = VSB_new_auto();
	AN(vsb);
	while (*text != '\0') {
		p = strstr(text, "${");
		if (p == NULL) {
			VSB_cat(vsb, text);
			break;
		}
		VSB_bcat(vsb, text, p - text);
		q = strchr(p, '}');
		if (q == NULL) {
			VSB_cat(vsb, text);
			break;
		}
		assert(p[0] == '$');
		assert(p[1] == '{');
		assert(q[0] == '}');
		p += 2;
		m = macro_get(p, q);
		if (m == NULL) {
			VSB_delete(vsb);
			fprintf(stdout, "[ERROR] Macro ${%s} not found\n", p);
			return (NULL);
		}
		VSB_printf(vsb, "%s", m);
		text = q + 1;
	}
	AZ(VSB_finish(vsb));
	return (vsb);
}

/**********************************************************************
 * Execute a file
 */

#define	MAX_TOKENS		200

static void
parse_string(char *buf, const struct cmds *cmd, void *priv)
{
	char *token_s[MAX_TOKENS], *token_e[MAX_TOKENS];
	struct vsb *token_exp[MAX_TOKENS];
	char *p, *q, *f;
	int nest_brace;
	int tn;
	const struct cmds *cp;

	assert(buf != NULL);
	for (p = buf; *p != '\0'; p++) {
		/* Start of line */
		if (vct_issp(*p))
			continue;
		if (*p == '#') {
			for (; *p != '\0' && *p != '\n'; p++)
				;
			if (*p == '\0')
				break;
			continue;
		}

		/* First content on line, collect tokens */
		tn = 0;
		f = p;
		while (*p != '\0') {
			assert(tn < MAX_TOKENS);
			if (*p == '\n') { /* End on NL */
				break;
			}
			if (vct_issp(*p)) { /* Inter-token whitespace */
				p++;
				continue;
			}
			if (*p == '\\' && p[1] == '\n') { /* line-cont */
				p += 2;
				continue;
			}
			if (*p == '"') { /* quotes */
				token_s[tn] = ++p;
				q = p;
				for (; *p != '\0'; p++) {
					if (*p == '"')
						break;
					if (*p == '\\') {
						p += VAV_BackSlash(p, q) - 1;
						q++;
					} else {
						if (*p == '\n')
							fprintf(stdout,
				"[ERROR] Unterminated quoted string"
				" in line: %*.*s",
				(int)(p - f), (int)(p - f), f);
						assert(*p != '\n');
						*q++ = *p;
					}
				}
				token_e[tn++] = q;
				p++;
			} else if (*p == '{') { /* Braces */
				nest_brace = 0;
				token_s[tn] = p + 1;
				for (; *p != '\0'; p++) {
					if (*p == '{')
						nest_brace++;
					else if (*p == '}') {
						if (--nest_brace == 0)
							break;
					}
				}
				assert(*p == '}');
				token_e[tn++] = p++;
			} else { /* other tokens */
				token_s[tn] = p;
				for (; *p != '\0' && !vct_issp(*p); p++)
					;
				token_e[tn++] = p;
			}
		}
		assert(tn < MAX_TOKENS);
		if (tn == 0)
			continue;
		token_s[tn] = NULL;
		for (tn = 0; token_s[tn] != NULL; tn++) {
			token_exp[tn] = NULL;
			AN(token_e[tn]);	/*lint !e771 */
			*token_e[tn] = '\0';	/*lint !e771 */
			if (NULL == strstr(token_s[tn], "${"))
				continue;
			token_exp[tn] = macro_expand(token_s[tn]);
			token_s[tn] = VSB_data(token_exp[tn]);
			token_e[tn] = strchr(token_s[tn], '\0');
		}

		for (cp = cmd; cp->name != NULL; cp++)
			if (!strcmp(token_s[0], cp->name))
				break;
		if (cp->name == NULL) {
			fprintf(stdout, "[ERROR] Unknown command: \"%s\"\n",
			    token_s[0]);
			exit(2);
		}
		if (verbose > 0)
			fprintf(stdout, "[DEBUG] %s", token_s[0]);
		assert(cp->cmd != NULL);
		cp->cmd(token_s, priv, cmd);
	}
}

/**********************************************************************
 * Generate a synthetic body
 */

static char *
synth_body(const char *len, int rnd)
{
	int i, j, k, l;
	char *b;


	AN(len);
	i = strtoul(len, NULL, 0);
	assert(i > 0);
	b = malloc(i + 1L);
	AN(b);
	l = k = '!';
	for (j = 0; j < i; j++) {
		if ((j % 64) == 63) {
			b[j] = '\n';
			k++;
			if (k == '~')
				k = '!';
			l = k;
		} else if (rnd) {
			b[j] = (random() % 95) + ' ';
		} else {
			b[j] = (char)l;
			if (++l == '~')
				l = '!';
		}
	}
	b[i - 1] = '\n';
	b[i] = '\0';
	return (b);
}

/*
 * Take a string provided by the user and break it up into address and
 * port parts.  Examples of acceptable input include:
 *
 * "localhost" - "localhost:80"
 * "127.0.0.1" - "127.0.0.1:80"
 * "0.0.0.0" - "0.0.0.0:80"
 * "[::1]" - "[::1]:80"
 * "[::]" - "[::]:80"
 *
 * See also RFC5952
 */

static int
VSS_parse(const char *str, char **addr, char **port)
{
	const char *p;

	*addr = *port = NULL;

	if (str[0] == '[') {
		/* IPv6 address of the form [::1]:80 */
		if ((p = strchr(str, ']')) == NULL ||
		    p == str + 1 ||
		    (p[1] != '\0' && p[1] != ':'))
			return (-1);
		*addr = strdup(str + 1);
		XXXAN(*addr);
		(*addr)[p - (str + 1)] = '\0';
		if (p[1] == ':') {
			*port = strdup(p + 2);
			XXXAN(*port);
		}
	} else {
		/* IPv4 address of the form 127.0.0.1:80, or non-numeric */
		p = strchr(str, ' ');
		if (p == NULL)
			p = strchr(str, ':');
		if (p == NULL) {
			*addr = strdup(str);
			XXXAN(*addr);
		} else {
			if (p > str) {
				*addr = strdup(str);
				XXXAN(*addr);
				(*addr)[p - str] = '\0';
			}
			*port = strdup(p + 1);
			XXXAN(*port);
		}
	}
	return (0);
}

/*
 * For a given host and port, return a list of struct vss_addr, which
 * contains all the information necessary to open and bind a socket.  One
 * vss_addr is returned for each distinct address returned by
 * getaddrinfo().
 *
 * The value pointed to by the tap parameter receives a pointer to an
 * array of pointers to struct vss_addr.  The caller is responsible for
 * freeing each individual struct vss_addr as well as the array.
 *
 * The return value is the number of addresses resoved, or zero.
 *
 * If the addr argument contains a port specification, that takes
 * precedence over the port argument.
 *
 * XXX: We need a function to free the allocated addresses.
 */
static int
VSS_resolve(const char *addr, const char *port, struct vss_addr ***vap)
{
	struct addrinfo hints, *res0, *res;
	struct vss_addr **va;
	int i, ret;
	long int ptst;
	char *adp, *hop;

	*vap = NULL;
	memset(&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	ret = VSS_parse(addr, &hop, &adp);
	if (ret)
		return (0);

	if (adp == NULL)
		ret = getaddrinfo(addr, port, &hints, &res0);
	else {
		ptst = strtol(adp,NULL,10);
		if (ptst < 0 || ptst > 65535)
			return(0);
		ret = getaddrinfo(hop, adp, &hints, &res0);
	}

	free(hop);
	free(adp);

	if (ret != 0)
		return (0);

	XXXAN(res0);
	for (res = res0, i = 0; res != NULL; res = res->ai_next, ++i)
		/* nothing */ ;
	if (i == 0) {
		freeaddrinfo(res0);
		return (0);
	}
	va = calloc(i, sizeof *va);
	XXXAN(va);
	*vap = va;
	for (res = res0, i = 0; res != NULL; res = res->ai_next, ++i) {
		va[i] = calloc(1, sizeof(**va));
		XXXAN(va[i]);
		va[i]->va_family = res->ai_family;
		va[i]->va_socktype = res->ai_socktype;
		va[i]->va_protocol = res->ai_protocol;
		va[i]->va_addrlen = res->ai_addrlen;
		xxxassert(va[i]->va_addrlen <= sizeof va[i]->va_addr);
		memcpy(&va[i]->va_addr, res->ai_addr, va[i]->va_addrlen);
	}
	freeaddrinfo(res0);
	return (i);
}

/* XXX: we may want to vary this */
static const char * const nl = "\r\n";

static void
cmd_url(CMD_ARGS)
{
	struct url *u;
	struct vss_addr *vaddr;
	const char *host = "127.0.0.1:80";
	const char *req = "GET";
	const char *url = "/";
	const char *proto = "HTTP/1.1";
	const char *body = NULL;

	(void)cmd;
	(void)priv;
	assert(!strcmp(av[0], "url"));
	av++;

	ALLOC_OBJ(u, URL_MAGIC);
	AN(u);
	u->vsb = VSB_new_auto();
	AN(u->vsb);
	VTAILQ_INSERT_TAIL(&url_list, u, list);
	num_urls++;

	for (; *av != NULL; av++) {
		if (!strcmp(*av, "-url")) {
			url = av[1];
			av++;
		} else if (!strcmp(*av, "-connect")) {
			host = av[1];
			av++;
		} else if (!strcmp(*av, "-proto")) {
			proto = av[1];
			av++;
		} else if (!strcmp(*av, "-req")) {
			req = av[1];
			av++;
		} else
			break;
	}
	u->nvaddr = VSS_resolve(host, NULL, &u->vaddr);
	if (u->nvaddr == 0) {
		fprintf(stdout, "[ERROR] failed to resolve %s\n", host);
		exit(1);
	}
	vaddr = u->vaddr[0];
	AN(vaddr);
	VTCP_name(&vaddr->va_addr, vaddr->va_addrlen, u->addr, sizeof(u->addr),
	    u->port, sizeof(u->port));

	VSB_printf(u->vsb, "%s %s %s%s", req, url, proto, nl);
	for (; *av != NULL; av++) {
		if (!strcmp(*av, "-hdr")) {
			VSB_printf(u->vsb, "%s%s", av[1], nl);
			av++;
		} else
			break;
	}
	for (; *av != NULL; av++) {
		if (!strcmp(*av, "-body")) {
			AZ(body);
			body = av[1];
			av++;
		} else if (!strcmp(*av, "-bodylen")) {
			AZ(body);
			body = synth_body(av[1], 0);
			av++;
		} else
			break;
	}
	if (*av != NULL) {
		fprintf(stdout, "[ERROR] Unknown http txreq spec: %s\n", *av);
		exit(2);
	}
	if (body != NULL)
		VSB_printf(u->vsb, "Content-Length: %ju%s",
		    (uintmax_t)strlen(body), nl);
	VSB_cat(u->vsb, nl);
	if (body != NULL) {
		VSB_cat(u->vsb, body);
		VSB_cat(u->vsb, nl);
	}
	VSB_finish(u->vsb);
}

static const struct cmds url_cmds[] = {
	{ "url",		cmd_url },
	{ NULL,			NULL }
};

static void
URL_readfile(const char *file)
{
	char *p;

	fprintf(stdout, "[INFO] Reading %s URL file.\n", file);
	p = read_file(file);
	if (p == NULL) {
		fprintf(stdout, "[ERROR] Cannot stat file \"%s\": %s\n",
		    file, strerror(errno));
		exit(2);
	}

	parse_string(p, url_cmds, NULL);

	fprintf(stdout, "[INFO] Total %d URLs are loaded from %s file.\n",
	    num_urls, file);
}

static void
URL_postjob(void)
{
	struct url *u;	
	int i = 0;

	urls = (struct url **)malloc(sizeof(struct url *) * num_urls);
	AN(urls);
	VTAILQ_FOREACH(u, &url_list, list)
		urls[i++] = u;
}

static void
usage(void)
{

	fprintf(stdout, "[INFO] usage: varnishperf [options] urlfile\n");
#define FMT "[INFO]    %-28s # %s\n"
	fprintf(stdout, FMT, "-c N", "Limits total TCP connections");
	fprintf(stdout, FMT, "-C N", "Sets request number per a conn");
	fprintf(stdout, FMT, "-m N", "Limits concurrent TCP connections");
	fprintf(stderr, FMT, "-p param=value", "set parameter");
	fprintf(stdout, FMT, "-r N", "Sets rate");
	fprintf(stdout, FMT, "-s file", "Sets file path containing src IP");
	fprintf(stdout, FMT, "-t N", "Sets number of threads");
	fprintf(stdout, FMT, "-z", "Shows all statistic fields");
	exit(1);
}

int
main(int argc, char *argv[])
{
	int ch;
	char *end, *p;
	const char *s_arg = NULL;

	MCF_ParamInit();

	while ((ch = getopt(argc, argv, "c:C:m:p:r:s:t:z")) != -1) {
		switch (ch) {
		case 'c':
			errno = 0;
			c_arg = strtoul(optarg, &end, 10);
			if (errno == ERANGE || end == optarg || *end) {
				fprintf(stdout,
				    "[ERROR] illegal number for -c\n");
				exit(1);
			}
			break;
		case 'C':
			errno = 0;
			C_arg = strtoul(optarg, &end, 10);
			if (errno == ERANGE || end == optarg || *end) {
				fprintf(stdout,
				    "[ERROR] illegal number for -c\n");
				exit(1);
			}
			if (C_arg <= 0) {
				fprintf(stdout,
				    "[ERROR] wrong number for -C: %u\n", C_arg);
				exit(1);
			}
			break;
		case 'm':
			errno = 0;
			m_arg = strtoul(optarg, &end, 10);
			if (errno == ERANGE || end == optarg || *end) {
				fprintf(stdout,
				    "[ERROR] illegal number for -m\n");
				exit(1);
			}
			break;
		case 'p':
			p = strchr(optarg, '=');
			if (p == NULL)
				usage();
			AN(p);
			*p++ = '\0';
			MCF_ParamSet(optarg, p);
			break;
		case 'r':
			errno = 0;
			r_arg = strtoul(optarg, &end, 10);
			if (errno == ERANGE || end == optarg || *end) {
				fprintf(stdout,
				    "[ERROR] illegal number for -r\n");
				exit(1);
			}
			break;
		case 's':
			s_arg = optarg;
			break;
		case 't':
			errno = 0;
			t_arg = strtoul(optarg, &end, 10);
			if (errno == ERANGE || end == optarg || *end) {
				fprintf(stdout,
				    "[ERROR] illegal number for -t\n");
				exit(1);
			}
			break;
		case 'z':
			z_flag = 1 - z_flag;
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
	init_macro();

	if (s_arg != NULL)
		SIP_readfile(s_arg);
	for (;argc > 0; argc--, argv++)
		URL_readfile(*argv);
	URL_postjob();
	if (num_urls == 0) {
		fprintf(stdout, "[ERROR] No URLs found.\n");
		usage();
	}
	PEF_Init();
	PEF_Run();
	return (0);
}
