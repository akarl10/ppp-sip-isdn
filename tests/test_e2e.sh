#!/bin/bash
# End-to-end test: two ppp-sip-isdn instances (UDP, localhost) talk to each
# other; the real pppd is replaced by tests/fake_pppd, which exchanges HDLC
# frames through the PTY and verifies what arrives. Run via: make test-e2e
set -u
cd "$(dirname "$0")"
APP=./ppp-sip-isdn-test
WORK=$(mktemp -d)
trap 'kill $SRV_PID 2>/dev/null; rm -rf "$WORK"' EXIT
FAILED=0
fail() { echo "FAIL: $*"; FAILED=1; }

$APP --id sip:server@127.0.0.1 --bindport 5556 \
     --ppplocalip 10.0.1.1 --pppremoteipstart 10.0.1.2 \
     >"$WORK/srv.out" 2>"$WORK/srv.err" &
SRV_PID=$!
sleep 1
kill -0 $SRV_PID 2>/dev/null || { echo "FAIL: server did not start"; cat "$WORK/srv.out" "$WORK/srv.err"; exit 1; }

timeout 30 $APP --id sip:client@127.0.0.1 --bindport 5557 \
     --dial sip:server@127.0.0.1:5556 \
     >"$WORK/cli.out" 2>"$WORK/cli.err"
CLI_RC=$?
[ $CLI_RC -eq 0 ] || fail "client exited with $CLI_RC (124 = timeout)"

# give the server a moment to notice the hangup, then ask it to shut down
sleep 1
kill -TERM $SRV_PID 2>/dev/null
for i in $(seq 1 50); do kill -0 $SRV_PID 2>/dev/null || break; sleep 0.1; done
if kill -0 $SRV_PID 2>/dev/null; then fail "server did not exit on SIGTERM"; kill -KILL $SRV_PID; fi

for side in srv cli; do
    role=server; [ $side = cli ] && role=client
    line=$(grep '^FAKEPPPD' "$WORK/$side.err" | head -1)
    echo "$role: ${line:-<no result>}"
    [ "$line" = "FAKEPPPD role=$role sent=30 recv=30 bad=0" ] || fail "$role did not exchange all frames intact"
done
if [ $FAILED -ne 0 ]; then
    for f in srv.out srv.err cli.out cli.err; do echo "--- $f"; cat "$WORK/$f"; done
    exit 1
fi
echo "PASS"
