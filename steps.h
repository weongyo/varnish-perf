/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

STEP(first,				FIRST)
STEP(start,				START)
STEP(http_start,			HTTP_START)
STEP(http_wait,				HTTP_WAIT)
STEP(http_connect,			HTTP_CONNECT)
STEP(http_txreq_init,			HTTP_TXREQ_INIT)
STEP(http_txreq,			HTTP_TXREQ)
STEP(http_rxresp,			HTTP_RXRESP)
STEP(http_rxresp_hdr,			HTTP_RXRESP_HDR)
STEP(http_rxresp_cl,			HTTP_RXRESP_CL)
STEP(http_rxresp_chunked_init,		HTTP_RXRESP_CHUNKED_INIT)
STEP(http_rxresp_chunked_no,		HTTP_RXRESP_CHUNKED_NO)
STEP(http_rxresp_chunked_body,		HTTP_RXRESP_CHUNKED_BODY)
STEP(http_rxresp_chunked_tail,		HTTP_RXRESP_CHUNKED_TAIL)
STEP(http_rxresp_eof,			HTTP_RXRESP_EOF)
STEP(http_done,				HTTP_DONE)
STEP(http_error,			HTTP_ERROR)
STEP(http_ok,				HTTP_OK)
STEP(timeout,				TIMEOUT)
STEP(done,				DONE)
