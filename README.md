# pping
large scale icmp pings

## TL;DR

```
docker run --rm bearice/pping pping 1.1.1.{0..255}

Usage: ./pping -v [-o log_name] [-l log_length] [-s slow_start] [-t loss_thr] [@target_file] targets...
```
## but why?

Have you ever had any challenge like this:
```
> Dev: Something went wrong last night, it must be your infra faults.
> Ops: Emmm, according to our monitoring system, it not seems like to be ....
> Dev: Then, it must be an networking error!
> Ops: Maybe but ....
> Dev: You can not prove you are innocent, thus that's all your fault.
```

OK, let's prove that infra/networking is not to blame by continuously pinging EVERY addresses in our network.
But wait, it could be thousands of them, since I can not spawn 65K `ping` process on a server, I need to make a tool.

## Why not ...

There are a lot existing tools like `fping` or `smokeping`, would do the same things, but none of them would fullfil my requirement, like scaling, logging, or timestamps.

## Setup

You need `libev-dev` for compiling. 

Adjusting sysctls is no longer required since we are using RAW Sockets now.

## Running

RAW sockets which requires root privilege, however a dockered root would worked fine.

You can run it like `./pping 172.16.{0..255}.{0..255}` for scanning a whole /16 network, add `-o pping.log` for redirect logs to file, log files will rotate after 10000000 lines, which is controlled by `-l`.

### Target list

Use `@file` to load a list of ip addresses, each line contains one ip address and nothing else, unrecognizable lines will be ignored.

### Slow start

To avoid a burst session creation at start to overload your firewall, an initial delay could be added to each target, delay is calculated as `i * delay_factor / 1000.0` seconds, so `-s 1` which is the default value limits 100ms delay for the 100th target.

### Loss threshold and back off

Loss threshold controls how many loss before consider a target as down. When a target is down, probe delay will increase liner rate calculated based on losses. The max delay is 60s+rand(10s).

### Log format

```
1577249544000,8.8.8.8,0.901568,0   ,1.000000,U     ,U          ,55
unix_ts_ms   ,ip     ,rtt     ,loss,interval,status,last_status,ttl
```


### Verbose status

```
TX=0 RX=0 TO=0 EQ=0 WT=0 WE=0 RE=0 (0 0 0) BO=0 Q=0

TX number of packets sent
RX number of packets recv
TO timeout events 
EQ Empty queue event
WT None-empty queue
WE Write error
RE Read error (syscall, unknown ip, bad reply)
BO Backed off
Q  Queue length
```

### Reload target list

```
#!/bin/bash

./pping 1.1.1.1 @a &
pid=$!
sleep 3
echo 8.8.8.8 > a
kill -USR1 $pid
sleep 3
rm a
kill -USR1 $pid
sleep 3
wait $pid

```

## Analysis

Personally i prefer using logstash for collect the logs and elasticsearch for analysis. templates and config example can be found in `logstash` dir.

## Performance

I am running a c5n.large instance on AWS, scanning for few /16 networks, and things worked very well.
