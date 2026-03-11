# Simple SIP ISDN-HDLC-PPP modem

this application allows calling or responding to a SIP "data" call (see RFC4040)
Without going deep into the rabbithole of ISDN, this application implements only synchronous ppp in hdlc-like framing (RFC1662)

It is not intended to develop this to a fully fledged all corner case handling application but exists only because of having a Fritz!Card and Fritz!Box with S0 and having seen
https://www.youtube.com/watch?v=rQfy8T-VOs4
from "The Serial Port" channel

V.110 or x.75 is not implemented.

V.110 might be useful for this to be a GSM dialup server (or calling a ppp server that runs over GSM)

I don't see a need for x.75

# How to use it
In doubt see the source code, but in general something like this on the "server" side (as root because of pppd)
```bash
ppp-sip-isdn --loglevel 1 --id sip:number@pbx.server --reg sip:pbx.server --user number --pass XXXXXXXXXXX --pppd "nodetach debug refuse-eap refuse-chap refuse-mschap require-mschap-v2 local proxyarp ms-dns 1.1.1.1 mp" --bindport 44444 --ppplocalip 10.0.1.1 --pppremoteipstart 10.0.1.2 --linecount 8
```

and on the "client" (channel bind 2 calls, as root because of pppd)
```bash
ppp-sip-isdn --loglevel 1 --id sip:number@pbx.server --reg sip:pbx.server --user number --pass XXXXXXXXXXX --pppd "nodetach debug noauth user myuser password mypassword mp" --bindport 44443 --linecount 2
```

