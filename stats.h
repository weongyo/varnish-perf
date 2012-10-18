/*-
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

PERFSTAT(n_sess,	uint64_t, 'g', "N session active", "sessions")
PERFSTAT(n_timeout,	uint64_t, 'c', "N session timed out", "sessions")
PERFSTAT(n_hitlimit,	uint64_t, 'c', "How many hit the rate limit", "times")
PERFSTAT(n_req,		uint64_t, 'c', "N requests", "reqs")
PERFSTAT(n_httpok,	uint64_t, 'c', "Successful HTTP request", "reqs")
PERFSTAT(n_httperror,	uint64_t, 'c', "Failed HTTP request", "reqs")
PERFSTAT(n_rxbytes,	uint64_t, 'c', "Total bytes varnishperf sent", "bytes")
PERFSTAT(n_txbytes,	uint64_t, 'c', "Total bytes varnishperf got", "bytes")
PERFSTAT(t_conntotal,	double,   'c', "Total time used for connect(2)",
				       "seconds")
PERFSTAT(t_fbtotal,	double,   'c', "Total time used for waiting"
				       " the first byte after sending HTTP"
				       " request",
				       "seconds")
PERFSTAT(t_bodytotal,	double,   'c', "Total time used for receiving the body",
				       "reqs")
