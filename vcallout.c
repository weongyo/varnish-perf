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

#include <sys/times.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vas.h"
#include "vcallout.h"

static int callout_debug = 0;
static int avg_depth;
static int avg_gcalls;
static int avg_mtxcalls;
static int avg_mpcalls;

void
callout_init(struct callout *c, int id)
{

	bzero(c, sizeof *c);
	c->magic = CALLOUT_MAGIC;
	c->c_id = id;
}

int
_callout_reset(struct callout_block *cb, struct callout *c, int to_ticks,
    void (*ftn)(void *), void *arg, const char *d_func, int d_line)
{
	int cancelled = 0;

	if (c->c_flags & CALLOUT_PENDING) {
		if (cb->nextsoftcheck == c)
			cb->nextsoftcheck = VTAILQ_NEXT(c, c_links.tqe);
		VTAILQ_REMOVE(&cb->callwheel[c->c_time & cb->callwheelmask], c,
		    c_links.tqe);
		cancelled = 1;
	}

	if (to_ticks <= 0)
		to_ticks = 1;

	c->magic = CALLOUT_MAGIC;
	c->c_arg = arg;
	c->c_flags |= (CALLOUT_ACTIVE | CALLOUT_PENDING);
	c->c_func = ftn;
	c->c_time = cb->ticks + to_ticks;
	c->d_func = d_func;
	c->d_line = d_line;
	VTAILQ_INSERT_TAIL(&cb->callwheel[c->c_time & cb->callwheelmask],
	    c, c_links.tqe);
	if (callout_debug)
		fprintf(stdout,
		    "%sscheduled %p func %p arg %p in %d",
		    cancelled ? "re" : "", c, c->c_func, c->c_arg, to_ticks);

	return (cancelled);
}

void
COT_clock(struct callout_block *cb)
{
	struct callout *c;
	struct callout_tailq *bucket;
	clock_t curticks;
	int steps;      /* #steps since we last allowed interrupts */
	int depth;
	int mpcalls;
	int mtxcalls;
	int gcalls;

#ifndef MAX_SOFTCLOCK_STEPS
#define	MAX_SOFTCLOCK_STEPS	100 /* Maximum allowed value of steps. */
#endif /* MAX_SOFTCLOCK_STEPS */

	mpcalls = 0;
	mtxcalls = 0;
	gcalls = 0;
	depth = 0;
	steps = 0;

	while (cb->softticks != cb->ticks) {
		cb->softticks++;
		/*
		 * softticks may be modified by hard clock, so cache
		 * it while we work on a given bucket.
		 */
		curticks = cb->softticks;
		bucket = &cb->callwheel[curticks & cb->callwheelmask];
		c = VTAILQ_FIRST(bucket);
		while (c) {
			depth++;
			if (c->c_time != curticks) {
				c = VTAILQ_NEXT(c, c_links.tqe);
				++steps;
				if (steps >= MAX_SOFTCLOCK_STEPS) {
					cb->nextsoftcheck = c;
					c = cb->nextsoftcheck;
					steps = 0;
				}
			} else {
				void (*c_func)(void *);
				void *c_arg;

				cb->nextsoftcheck = VTAILQ_NEXT(c, c_links.tqe);
				VTAILQ_REMOVE(bucket, c, c_links.tqe);
				c_func = c->c_func;
				c_arg = c->c_arg;
				c->c_flags = (c->c_flags & ~CALLOUT_PENDING);
				mpcalls++;
				if (callout_debug)
					fprintf(stdout,
					    "callout mpsafe %p func %p "
					    "arg %p", c, c_func, c_arg);
				c_func(c_arg);
				if (callout_debug)
					fprintf(stdout,
					    "callout %p finished", c);
				steps = 0;
				c = cb->nextsoftcheck;
			}
		}
	}
	avg_depth += (depth * 1000 - avg_depth) >> 8;
	avg_mpcalls += (mpcalls * 1000 - avg_mpcalls) >> 8;
	avg_mtxcalls += (mtxcalls * 1000 - avg_mtxcalls) >> 8;
	avg_gcalls += (gcalls * 1000 - avg_gcalls) >> 8;
	cb->nextsoftcheck = NULL;
}

int
_callout_stop_safe(struct callout_block *cb, struct callout *c)
{

	/*
	 * If the callout isn't pending, it's not on the queue, so
	 * don't attempt to remove it from the queue.  We can try to
	 * stop it by other means however.
	 */
	if (!(c->c_flags & CALLOUT_PENDING)) {
		c->c_flags &= ~CALLOUT_ACTIVE;
		if (callout_debug)
			fprintf(stdout,
			    "failed to stop %p func %p arg %p",
			    c, c->c_func, c->c_arg);
		return (0);
	}

	c->c_flags &= ~(CALLOUT_ACTIVE | CALLOUT_PENDING);

	if (cb->nextsoftcheck == c)
		cb->nextsoftcheck = VTAILQ_NEXT(c, c_links.tqe);
	VTAILQ_REMOVE(&cb->callwheel[c->c_time & cb->callwheelmask], c,
	    c_links.tqe);

	if (callout_debug)
		fprintf(stderr, "cancelled %p func %p arg %p",
		    c, c->c_func, c->c_arg);
	return (1);
}

static void
cot_callwhell_alloc(struct callout_block *cb)
{
	/*
	 * Calculate callout wheel size
	 */
	for (cb->callwheelsize = 1, cb->callwheelbits = 0;
	     cb->callwheelsize < cb->ncallout;
	     cb->callwheelsize <<= 1, ++cb->callwheelbits)
		;
	cb->callwheelmask = cb->callwheelsize - 1;

	cb->callwheel = calloc(cb->callwheelsize, sizeof(struct callout_tailq));
	AN(cb->callwheel);
}

void
COT_ticks(struct callout_block *cb)
{
	struct tms tms;

	/*
	 * XXX times(2) is a system call so little bit expensive to
	 * call it frequently.  And moreover it's called at every
	 * workers.
	 */
	cb->ticks = times(&tms);
}

void
COT_init(struct callout_block *cb)
{
	struct tms tms;
	int i;

	bzero(cb, sizeof(struct callout_block));

	cb->ncallout = 16;		/* FIXME: with proper value */
	cot_callwhell_alloc(cb);
	cb->ticks = cb->softticks = times(&tms);

	for (i = 0; i < cb->callwheelsize; i++) {
		VTAILQ_INIT(&cb->callwheel[i]);
	}
}

void
COT_fini(struct callout_block *cb)
{

	free(cb->callwheel);
}
