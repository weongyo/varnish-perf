/*
 * Copyright (c) 2012 by Weongyo Jeong <weongyo@gmail.com>.
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

#include "vqueue.h"

/*--------------------------------------------------------------------*/

VTAILQ_HEAD(callout_tailq, callout);

/* XXX FIXME: has a assumption that CLOCKS_PER_SEC is 100 */
#define	CALLOUT_MSTOTICKS(ms)	((ms) / 10)
#define	CALLOUT_SECTOTICKS(sec)	((sec) * 100)
#define	CALLOUT_ACTIVE		0x0002	/* callout is currently active */
#define	CALLOUT_PENDING		0x0004	/* callout is waiting for timeout */

struct callout {
	unsigned	magic;
#define	CALLOUT_MAGIC	0x2d634820
	union {
		VSLIST_ENTRY(callout) sle;
		VTAILQ_ENTRY(callout) tqe;
	} c_links;
	clock_t	c_time;			/* ticks to the event */
	void	*c_arg;			/* function argument */
	void	(*c_func)(void *);	/* function to call */
	int	c_flags;		/* state of this entry */
	int	c_id;			/* XXX: sp->id.  really need? */
	const char *d_func;		/* func name of caller */
	int	d_line;			/* line num of caller */
};

struct callout_block {
	clock_t		ticks;
	clock_t		softticks;	/* Like ticks, but for COT_clock(). */
	int		ncallout;	/* maximum # of timer events */
	int		callwheelsize;
	int		callwheelbits;
	int		callwheelmask;
	struct callout_tailq *callwheel;
	struct callout *nextsoftcheck;	/* Next callout to be checked. */
};

#define	callout_stop(w, c)	_callout_stop_safe(w, c)
void	COT_init(struct callout_block *);
void	COT_fini(struct callout_block *);
void	COT_clock(struct callout_block *);
void	COT_ticks(struct callout_block *);
void	callout_init(struct callout *, int);
#define	callout_reset(cb, c, to, func, arg) \
	    _callout_reset(cb, c, to, func, arg, __func__, __LINE__)
int	_callout_reset(struct callout_block *, struct callout *, int,
	    void (*)(void *), void *, const char *, int);
int	_callout_stop_safe(struct callout_block *, struct callout *);
