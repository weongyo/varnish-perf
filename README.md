<pre>
Varnish-perf
============

Varnish-perf is a simple HTTP performance (stress) testing tool to
measure the maximum performance of target web server or cache
server.

Architecture
============

* The most of code are based on varnish-cache
  (https://www.varnish-cache.org/).  That is the reason why this is
  including 'varnish' in its name.

* But all socket operations are non-blocking.  One thread is enough to
  handle several thousand concurrent connections.

* For edge case testing, sometimes one thread isn't enough to hit the
  top.  So multiple threads are supported.  Even more one centralized
  statistics.

* Little bit flexible syntax to define each URL entries.

How to compile
==============

1. Download thr Varnish-perf from official site (Github).
2. Untar the file and compile as belows:

    # make depend
    # make

How to use
==========

    # ./varnishperf
    [ERROR] No URLs found.
    [INFO] usage: varnishperf [options] urlfile
    [INFO]    -c N                         # Sets number of threads
    [INFO]    -m N                         # Limits concurrent TCP connections
    [INFO]    -r N                         # Sets rate
    [INFO]    -s file                      # Sets file path containing src IP

Each options indicate:

* -c N

  How many thread will handle the request queue.  This request queue is a
  serialized HTTP request queue built.

  If -c option isn't set, default value is 1.

* -m N

  Sets the maximum number of TCP connections which connected to the backend.

  Default value is 0 indicating unlimited.

* -p param=value

  Sets the parameters used to control varnishperf's behaviours.  Following
  parameters are supported.

  * connect_timeout=N

    Default connection timeout for backend connections.
    We only try to connect to the backend for this many
    seconds before giving up. 

    Default value is 3 seconds.

  * diag_bitmap=N

    Bitmap controlling diagnostics code:

    * 0x00000001 - CNT_Session states.
    * 0x00000002 - socket error messages.

    Default value is 0.

  * read_timeout=N

    Default timeout for receiving bytes from target.
    We only wait for this many seconds for bytes
    before giving up.

    Default value is 6 seconds.

  * write_timeout=N

    Send timeout for client connections.
    If the HTTP response hasn't been transmitted in this many
    seconds the session is closed.

    Default value is 6 seconds.

* -r N

  Indicates the rate.  For example, if -r 1000, there will be 1000 requests
  per a second.

  If -r option isn't set, Default value is 1.

* -s file

  Sometimes the stress server could have multiple IP addresses.  If multiple
  src IPs are defined, it'll be selected in round-robin manner.

  If -s option isn't set, OS'll select its source IP of packets
  automatically.

* -z

  Shows all statistic fields.  If stat value is zero, default behaviour is
  that it'd not be shown.

URL file syntax
===============

Please note that if multiple URLs are defined, it'll be ran in round-robin
manner.

At this moment only one command is supported;

* url

  Used to define a URL entry for testing.

url command
-----------

As arguments, four essential arguments are supported.  This argument should be
first always before extend arguments.

* -connect "string"

  Where this connection would be to.  For example, "string" could be
  "www.google.com:443", "182.33.44.21" or "14.5.233.9:80".

  Please note that if you want to set "Host" header of HTTP request,
  you should use -hdr argument explicitly.

  If not defined, default value is "127.0.0.1:80".

* -proto "string"

  Default value is "HTTP/1.1".

* -req "string"

  Default value is "GET".

* -url "string"

  Default value if "/".

Extend arguments are as follows:

* -hdr "string"

  Sets extra HTTP header.  Please note that don't need to put \r\n at
  end of string.  It'll be automatically inserted.

* -body "string"

  If this argument is defined, "string" would be body of HTTP request.

  Please note that if -body or -bodylen option is used, "Content-Length"
  header will be automatically inserted.

* -bodylen number

  If this argument is defined, the random-generated string whose length is
  number long would be body of HTTP request.

  Please note that if -body or -bodylen option is used, "Content-Length"
  header will be automatically inserted.

### url command examples

```
url -connect "172.18.14.1:8080" -url "/1b" -hdr "Connection: close"
url -connect "www.google.com:80" -url "/" -req "POST" \
    -body "ABCD"
url -connect "www.google.com:80" -url "/" -req "POST" -proto "HTTP/1.0" \
    -hdr "Host: www.google.com" \
    -hdr "User-Agent: varnishperf (trunk)" \
    -bodylen 5
```

Examples
========

[root@localhost varnish-perf]# /sbin/ifconfig
eth0      Link encap:Ethernet  HWaddr 00:15:17:D2:EB:0A  
          inet addr:172.18.14.2  Bcast:172.18.255.255  Mask:255.255.0.0
          inet6 addr: fe80::215:17ff:fed2:eb0a/64 Scope:Link
          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
          RX packets:1121852984 errors:0 dropped:135732 overruns:0 frame:0
          TX packets:1130311524 errors:0 dropped:0 overruns:0 carrier:0
          collisions:0 txqueuelen:1000 
          RX bytes:146224470591 (136.1 GiB)  TX bytes:117561365260 (109.4 GiB)
          Memory:fafe0000-fb000000 

eth0:0    Link encap:Ethernet  HWaddr 00:15:17:D2:EB:0A  
          inet addr:172.18.14.3  Bcast:172.18.255.255  Mask:255.255.0.0
          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
          Memory:fafe0000-fb000000 
[root@localhost varnish-perf]# cat urls 
url -connect "172.18.14.1:8080" -url "/1b" -hdr "Connection: close"
[root@localhost varnish-perf]# cat srcips 
172.18.14.2
172.18.14.3
[root@localhost varnish-perf]# ./varnishperf -s srcips -c 1 -m 300 -r 30000 ./urls
[INFO] Reading "srcips" SRCIP file.
[INFO] Total 2 SRCIP are loaded from "srcips" file.
[INFO] Reading ./urls URL file.
[INFO] Total 1 URLs are loaded from ./urls file.
[STAT]  time    | total    | req   | conn  | connect time          | first byte time       | body time             | tx         | tx    | rx         | rx    | errors
[STAT]          |          |       |       |   min     avg     max |   min     avg     max |   min     avg     max |            |       |            |       |
[STAT] ---------+----------+-------+-------+-----------------------+-----------------------+-----------------------+------------+-------+------------+-------+-------....
[STAT] 00:00:01 |     1479 |  1479 |    12 | 0.000 / 0.000 / 0.001 | 0.000 / 0.000 / 0.003 | 0.000 / 0.000 / 0.001 |      57408 |   56K |     476764 |  465K | 0
[STAT] 00:00:02 |    22893 | 21414 |     9 | 0.000 / 0.000 / 0.001 | 0.000 / 0.000 / 0.003 | 0.000 / 0.000 / 0.001 |     835263 |  815K |    6960525 |  6,7M | 0
[STAT] 00:00:03 |    44647 | 21753 |     6 | 0.000 / 0.000 / 0.001 | 0.000 / 0.000 / 0.002 | 0.000 / 0.000 / 0.000 |     848367 |  828K |    7070050 |  6,8M | 0
[STAT] 00:00:04 |    67307 | 22659 |     6 | 0.000 / 0.000 / 0.001 | 0.000 / 0.000 / 0.005 | 0.000 / 0.000 / 0.003 |     883740 |  863K |    7364500 |  7,0M | 0
[STAT] 00:00:05 |    96227 | 28917 |    23 | 0.000 / 0.000 / 0.001 | 0.000 / 0.001 / 0.203 | 0.000 / 0.000 / 0.005 |    1127100 |  1,1M |    9392825 |    9M | 0
[STAT] 00:00:06 |   126460 | 30232 |    46 | 0.000 / 0.000 / 0.001 | 0.000 / 0.001 / 0.201 | 0.000 / 0.000 / 0.000 |    1178190 |  1,1M |    9818575 |  9,4M | 0
[STAT] 00:00:07 |   156436 | 29972 |    85 | 0.000 / 0.000 / 0.001 | 0.000 / 0.001 / 0.203 | 0.000 / 0.000 / 0.002 |    1167504 |  1,1M |    9729200 |  9,3M | 0
[STAT] 00:00:08 |   186504 | 30065 |    71 | 0.000 / 0.002 / 3.000 | 0.000 / 0.001 / 0.201 | 0.000 / 0.000 / 0.001 |    1172340 |  1,1M |    9769825 |  9,3M | 19
[STAT] 00:00:09 |   216516 | 30011 |    84 | 0.000 / 0.002 / 3.000 | 0.000 / 0.001 / 0.202 | 0.000 / 0.000 / 0.001 |    1169298 |  1,1M |    9743500 |  9,3M | 40
[STAT] 00:00:10 |   246505 | 29988 |   111 | 0.000 / 0.004 / 3.000 | 0.000 / 0.002 / 0.204 | 0.000 / 0.000 / 0.000 |    1166919 |  1,1M |    9724325 |  9,3M | 79
[STAT] 00:00:11 |   276545 | 30040 |   111 | 0.000 / 0.001 / 2.997 | 0.000 / 0.001 / 0.201 | 0.000 / 0.000 / 0.000 |    1171248 |  1,1M |    9784436 |  9,3M | 85
[STAT] 00:00:12 |   305990 | 29441 |    97 | 0.000 / 0.003 / 2.998 | 0.000 / 0.001 / 0.202 | 0.000 / 0.000 / 0.001 |    1147692 |  1,1M |    9592876 |  9,2M | 115
[STAT] 00:00:13 |   335966 | 29971 |   144 | 0.000 / 0.007 / 3.000 | 0.000 / 0.001 / 0.202 | 0.000 / 0.000 / 0.001 |    1165749 |  1,1M |    9739250 |  9,3M | 167
[STAT] 00:00:14 |   366168 | 30201 |   164 | 0.000 / 0.001 / 3.000 | 0.000 / 0.002 / 0.204 | 0.000 / 0.000 / 0.000 |    1176201 |  1,1M |    9837376 |  9,4M | 174
[STAT] ---------+----------+-------+-------+-----------------------+-----------------------+-----------------------+------------+-------+------------+-------+-------....
[STAT] Summary:
[STAT]    21471                sessions   g # N session active
[STAT]    174                  sessions   c # N session timed out
[STAT]    181                  conns      g # N connection active
[STAT]    6                    times      c # How many hit the rate limit
[STAT]    374533               reqs       c # N requests
[STAT]    374178               reqs       c # Successful HTTP request
[STAT]    174                  reqs       c # Failed HTTP request
[STAT]    374533               conns      c # Total TCP connected
[STAT]    121730682            bytes      c # Total bytes varnishperf got
[STAT]    14593956             bytes      c # Total bytes varnishperf send
[STAT]    606.957768           seconds    c # Total time used for connect(2)
[STAT]    367.215323           seconds    c # Total time used for waiting the first byte after sending HTTP request
[STAT]    5.044619             seconds    c # Total time used for receiving the body

Supported OS
============

* linux

License
=======

Simplified BSD (2-clause) license

Author
======

* Weongyo Jeong - weongyo@gmail.com
</pre>
