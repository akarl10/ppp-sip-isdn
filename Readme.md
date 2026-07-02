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
You can use --srtp to enable opportunistic srtp, if you call (--dial) or register (--reg) with sips: or sip:...;transport=tls, srtp will be mandatory


# Recommendation for non-root invocation

If the `--pppd` option is not used, a helper binary is employed to start
`pppd`. This helper is a small setuid-root program with most parameters
hard-coded.

Using the helper allows `ppp-sip-isdn` itself to run as an unprivileged
user. For dial-in server deployments this is strongly recommended, since
the SIP stack and media processing do not require root privileges.

The helper always uses the PPP options file:

    /etc/ppp/options.isdn

PPP parameters should be configured there. IP addresses are provided by
`ppp-sip-isdn` and therefore must not be specified in that file.

The helper binary must be installed as:

    /usr/local/libexec/ppp-sip-isdn/ppp-helper

(see the Makefile).

A typical installation is:

```bash
install ppp-helper /usr/local/libexec/ppp-sip-isdn/ppp-helper
#Only members of the dip group will then be permitted to start or stop PPP sessions through the helper.
chown root:dip /usr/local/libexec/ppp-sip-isdn/ppp-helper
chmod 4750 /usr/local/libexec/ppp-sip-isdn/ppp-helper
```

The helper validates:

the requested PTY belongs to the invoking user,
local and remote IP addresses are syntactically valid

To authorize termination of a PPPD instance, the helper verifies:

- the specified PID exists,
- the process executable matches the configured PPPD path,
- the process was started with the helper-specific `argv[0]` identifier,
- the PTY used by the PPPD instance still belongs to the invoking user.

No user-supplied PPP options are passed to pppd; the executable path,
configuration file and command-line arguments are fixed by the helper.

# How to use it with a PBX
In doubt see the source code, but in general something like this on the "server" side (as root because of pppd)
```bash
# user must be member of dip, see above
ppp-sip-isdn --loglevel 1 --id sip:number@pbx.server --reg sip:pbx.server --user number --pass XXXXXXXXXXX --bindport 44444 --ppplocalip 10.0.1.1 --pppremoteipstart 10.0.1.2 --linecount 8
```

and on the "client" (channel bind 2 calls, as root because of pppd)
```bash
#as root because of pppd, else put every parameter in /etc/ppp/options.isdn and leave --pppd and run as user, must be member of group dip in that case
ppp-sip-isdn --loglevel 1 --id sip:number@pbx.server --reg sip:pbx.server --user number --pass XXXXXXXXXXX --pppd "noauth user myuser password mypassword mp" --bindport 44443 --linecount 2 --dial sip:dialinnumber@pbx.server
```
# How to use it in a P2P configuration without any security
one one side (server)
```bash
sudo ppp-sip-isdn --loglevel 1 --id sip:serverside@localhost --pppd "noauth mp" --bindport 5060 --ppplocalip 10.0.1.1 --pppremoteipstart 10.0.1.2 --linecount 2
```
on the other side (client)
```bash
sudo ppp-sip-isdn --loglevel 1 --id sip:clientside@localhost --pppd "noauth mp" --bindport 5060 --dial sip:serverside@ip-of-server --linecount 2
```
you should get 2*64kbps. Once maybe that felt blazing fast, but except for nostalgia, this speed is useless today.

# How to build

```bash
cd ppp-sip-isdn
git submodule init
git submodule update
make
```
