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

PERFSTAT_u64(n_sess,		'g', "N session active", "sessions")
PERFSTAT_u64(n_timeout,		'c', "N session timed out", "sessions")
PERFSTAT_u64(n_hitlimit,	'c', "How many hit the rate limit", "times")
PERFSTAT_u64(n_req,		'c', "N requests", "reqs")
PERFSTAT_u64(n_httpok,		'c', "Successful HTTP request", "reqs")
PERFSTAT_u64(n_httperror,	'c', "Failed HTTP request", "reqs")
PERFSTAT_u64(n_rxbytes,		'c', "Total bytes varnishperf got", "bytes")
PERFSTAT_u64(n_txbytes,		'c', "Total bytes varnishperf send", "bytes")
PERFSTAT_dbl(t_conntotal,	'c', "Total time used for connect(2)",
				       "seconds")
PERFSTAT_dbl(t_fbtotal,		'c', "Total time used for waiting"
				     " the first byte after sending HTTP"
				     " request",
				     "seconds")
PERFSTAT_dbl(t_bodytotal,	'c', "Total time used for receiving the body",
				     "seconds")
