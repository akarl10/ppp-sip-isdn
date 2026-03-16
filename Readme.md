# Simple SIP ISDN-HDLC-PPP modem

this application allows calling or responding to a SIP "data" call (see RFC4040)
Without going deep into the rabbithole of ISDN, this application implements only synchronous ppp in hdlc-like framing (RFC1662)

It is not intended to develop this to a fully fledged all corner case handling application but exists only because of having a Fritz!Card and Fritz!Box with S0 and having seen
https://www.youtube.com/watch?v=rQfy8T-VOs4
from "The Serial Port" channel

V.110, V.120 or X.75 is not implemented.

V.110 might be useful for this to be a GSM dialup server (or calling a ppp server that runs over GSM)

I don't see a need for X.75 since as far as I know usually ppp/internet was set up mostly using RFC1662

The reason why this application does not support emulating a AT modem is because this application will only work with ppp and not dealing with AT commands too calling pppd directly was choosen.

To use ipv6 instead of ipv4 for sip and rtp transport, just add --ipv6 to the command line
If you don't want to register to a pbx, just don't set --reg, --user and --pass.
If --bindport is not set the application will use a random port (not 5060) to bind itself to. If you want some consistency just set this value to something you find appropriate
You can use --srtp to enable opportunistic srtp, if you call (--dial) or register (--reg) with sips:, srtp will be mandatory

# How to use it with a PBX
In doubt see the source code, but in general something like this on the "server" side (as root because of pppd)
```bash
ppp-sip-isdn --loglevel 1 --id sip:number@pbx.server --reg sip:pbx.server --user number --pass XXXXXXXXXXX --pppd "nodetach debug refuse-eap refuse-chap refuse-mschap require-mschap-v2 local proxyarp ms-dns 1.1.1.1 mp" --bindport 44444 --ppplocalip 10.0.1.1 --pppremoteipstart 10.0.1.2 --linecount 8
```

and on the "client" (channel bind 2 calls, as root because of pppd)
```bash
ppp-sip-isdn --loglevel 1 --id sip:number@pbx.server --reg sip:pbx.server --user number --pass XXXXXXXXXXX --pppd "nodetach debug noauth user myuser password mypassword mp" --bindport 44443 --linecount 2 --dial sip:dialinnumber@pbx.server
```
# How to use it in a P2P configuration without any security
one one side (server)
```bash
ppp-sip-isdn --loglevel 1 --id sip:serverside@localhost --pppd "nodetach noauth mp" --bindport 5060 --ppplocalip 10.0.1.1 --pppremoteipstart 10.0.1.2 --linecount 2
```
on the other side (client)
```bash
ppp-sip-isdn --loglevel 1 --id sip:clientside@localhost --pppd "nodetach noauth mp" --bindport 5060 --dial sip:serverside@ip-of-server --linecount 2
```
you should get 2*64kbps. Once maybe that felt blazing fast, but except for nostalgia, this speed is useless today.

# How to build

```bash
cd ppp-sip-isdn
git submodule init
git submodule update
make
```
