# pping
large scale icmp pings

## but why?

Have you ever had any chanllege like this:
```
> Dev: Something went wrong last night, it must be your infra faults.
> Ops: Emmm, according to our monitoring system, it not seems like to be ....
> Dev: Then, it must be an networking error!
> Ops: Maybe but ....
> Dev: You can not prove you are innocent, thus that's all your fault.
```

OK, let's prove that infra/networking is not to blame by continuously pinging EVERY addresses in our network.
But wait, it could be thounds of them, since I can not spawn 65K `ping` process on a server, I need to make a tool.

## Why not ...

There are a lot existing tools like fping or smokeping, would do the same things, but none of them would fullfill my requirement, like scaling, logging, or timestamping.

## Setup

You need `libev-dev` for compiling. 

Adjusting sysctls is no longer required since we are using RAW Sockets now.

## Running

RAW sockets which requires root privilege, however a dockered root would worked fine.

You can run it like `./pping 172.16.{0..255}.{0..255}` for scanning a whole /16 network, add `-o pping.log` for redirect logs to file, log files will rotate after 10000000 lines, which is controlled by `-l`.

### Log format

```
1577249544,8.8.8.8,0.901568,0   ,1.000000,U     ,U          ,55
unix_ts   ,ip     ,rtt     ,loss,interval,status,last_status,ttl
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
RE Read error (syscall, unknow ip, bad reply)
BO Backed off
Q  Queue length
```

## Analysis

Personally i perfer using logstash for collect the logs and elasticsearch for analysis. templates and config example can be found in `logstash` dir.

## Performance

I am running a c5n.large instance on AWS, scanning for few /16 networks, and things worked very well.
