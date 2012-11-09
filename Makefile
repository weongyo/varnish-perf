#
# Copyright (c) 2012 by Weongyo Jeong <weongyo@gmail.com>.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.

SRCS=	\
	humanize_number.c \
	varnishperf.c \
	vas.c \
	vct.c \
	vlck.c \
	vsb.c \
	vcallout.c

OBJS=	$(SRCS:.c=.o)

CFLAGS= \
	-std=gnu99 \
	-Wpointer-arith \
	-Wreturn-type -Wcast-qual -Wwrite-strings -Wswitch \
	-Wcast-align -Wunused-parameter -Wchar-subscripts -Winline \
	-Wformat -Wextra -Wno-missing-field-initializers -Wno-sign-compare \
	-Wall -Werror -Wshadow -Wstrict-prototypes -Wnested-externs \
	-Wmissing-prototypes -Wredundant-decls \
	-O2

LDFLAGS=\
	-lm -lpthread -lrt

all: varnishperf

varnishperf: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

depend:
	@if ! test -f .depend; then \
		touch .depend; \
	fi
	./mkdep -f .depend $(CFLAGS) $(SRCS)

clean:
	rm -f varnishperf $(OBJS) *~

ifeq ($(wildcard .depend), )
$(warning .depend fils is missed.  Runs 'make depend' first.)
else
include .depend
endif
