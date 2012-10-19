Varnish-perf
============

Varnish-perf is a simple HTTP performance testing tool to measure the maximum
performance of target web server or cache server.

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

<pre>
    # make depend
    # make
</pre>

How to use
==========

<pre>
    # ./varnishperf
    [ERROR] No URLs found.
    [INFO] usage: varnishperf [options] urlfile
    [INFO]    -c N                         # Sets number of threads
    [INFO]    -r N                         # Sets rate
    [INFO]    -s file                      # Sets file path containing src IP
</pre>

Each options indicate:

* -c N

  * How many thread will handle the request queue.  This request queue is a
    serialized HTTP request queue built.

    If -c option isn't set, default value is 1.

* -r N

  * Indicates the rate.  For example, if -r 1000, there will be 1000 requests
    per a second.

    If -r option isn't set, Default value is 1.

* -s file

  * Sometimes the stress server could have multiple IP addresses.  If multiple
    src IPs are defined, it'll be selected in round-robin manner.

    If -s option isn't set, OS'll select its source IP of packets
    automatically.

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
  "www.google.com:443", "182.33.44.21" or "14.5.233.9:80"

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

* -bodylen <number>

  If this argument is defined, the random-generated string whose length is
  <number> long would be body of HTTP request.

  Please note that if -body or -bodylen option is used, "Content-Length"
  header will be automatically inserted.

### url command examples

```
url -connect "172.18.14.1:8080" -url "/1b" -hdr "Connection: close"
url -connect "www.google.com:80" -url "/" -req "POST" \
    -body "123"
```

Examples
========

<pre>
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
[root@localhost varnish-perf]# ./varnishperf -s srcips -c 1 -r 30000 ./urls
[INFO] Reading srcips SRCIP file.
[INFO] Total 2 SRCIP are loaded from srcips file.
[INFO] Reading ./urls URL file.
[INFO] Total 1 URLs are loaded from ./urls file.
[STAT]  time    | total    | req   | connect time          | first byte time       | body time             | tx         | tx    | rx         | rx    | errors
[STAT]          |          |       |   min     avg     max |   min     avg     max |   min     avg     max |            |       |            |       |
[STAT] ---------+----------+-------+-----------------------+-----------------------+-----------------------+------------+-------+------------+-------+-------....
[STAT] 00:00:01 |     2192 |  2192 | 0.000 / 0.000 / 0.001 | 0.000 / 0.000 / 0.001 | 0.000 / 0.000 / 0.001 |     356644 |  348K |     709150 |  692K | 0
[STAT] 00:00:02 |    23451 | 21259 | 0.000 / 0.000 / 0.001 | 0.000 / 0.000 / 0.002 | 0.000 / 0.000 / 0.000 |    3465217 |  3,3M |    6909825 |  6,6M | 0
[STAT] 00:00:03 |    46830 | 23378 | 0.000 / 0.000 / 0.001 | 0.000 / 0.000 / 0.002 | 0.000 / 0.000 / 0.000 |    3810777 |  3,6M |    7598175 |  7,3M | 0
[STAT] 00:00:04 |    71542 | 24711 | 0.000 / 0.000 / 0.001 | 0.000 / 0.000 / 0.001 | 0.000 / 0.000 / 0.000 |    4027567 |  3,9M |    8048359 |  7,7M | 0
[STAT] 00:00:05 |    97164 | 25621 | 0.000 / 0.000 / 0.001 | 0.000 / 0.000 / 0.004 | 0.000 / 0.000 / 0.000 |    4176386 |    4M |    8354076 |    8M | 0
[STAT] ---------+----------+-------+-----------------------+-----------------------+-----------------------+------------+-------+------------+-------+-------....
[STAT] Summary:
[STAT]    20490      sessions   # N session active
[STAT]    0          sessions   # N session timed out
[STAT]    4          times      # How many hit the rate limit
[STAT]    106675     reqs       # N requests
[STAT]    106665     reqs       # Successful HTTP request
[STAT]    0          reqs       # Failed HTTP request
[STAT]    34722450   bytes      # Total bytes varnishperf got
[STAT]    17387862   bytes      # Total bytes varnishperf send
[STAT]    13.476183  seconds    # Total time used for connect(2)
[STAT]    31.302394  seconds    # Total time used for waiting the first byte after sending HTTP request
[STAT]    1.489381   seconds    # Total time used for receiving the body
</pre>

License
=======

Simplified BSD (2-clause) license

Author
======

* Weongyo Jeong - weongyo@gmail.com

