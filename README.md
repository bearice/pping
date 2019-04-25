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


## Setup

You need `libev-dev` for compiling. 
It's recommened to put `pping.sysctl.conf` to your `/etc/sysctl.d`, or change these values yourself. please read kernel manuals for each item in case of questions.

## Running

If you have `net.ipv4.ping_group_range` set, no root privilage needed. 

You can run it like `./pping 172.16.{0..255}.{0..255}` for scanning a whole /16 network, add `-o pping.log` for redirect logging to file, log files will rotate after 10000000 lines, which is controlled by `-l`.

## Analysis

Personally i perfer using logstash for collect the logs and elasticsearch for analysis. templates and config example can be found in `logstash` dir.
