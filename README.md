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
