#!/bin/bash
# End-to-end test: two ppp-sip-isdn instances (UDP, localhost) talk to each
# other; the real pppd is replaced by tests/fake_pppd, which exchanges HDLC
# frames through the PTY and verifies what arrives. Run via: make test-e2e
#
# usage: test_e2e.sh [lines [extra ppp-sip-isdn options for both sides...]]
#   test_e2e.sh                 single line, CLEARMODE
#   test_e2e.sh 8               8 bundled lines (multilink)
#   test_e2e.sh 1 --pcma        single line, PCMA bearer (easter egg mode)
# environment:
#   FRAMES=n   frames each side sends per line (default 30, max 255)
#   MIN_PCT=n  percentage of frames that must arrive intact (default 100)
#   KILL=stagger  like half, but one line every STAGGER_MS (default 500) instead of all at once
#   KILL=half  once all lines are up and exchanging, SIGTERM the fake pppd of
#              every second line on the client side (like pppd dying); the
#              other lines must carry on undisturbed
set -u
LINES=${1:-1}
[ $# -gt 0 ] && shift
EXTRA=("$@")
FRAMES=${FRAMES:-30}
KILL=${KILL:-none}
STAGGER_MS=${STAGGER_MS:-500}
KILL_T=()
MIN_PCT=${MIN_PCT:-100}
export FAKE_PPPD_FRAMES=$FRAMES
echo "== e2e: $LINES line(s), $FRAMES frames, kill=$KILL, min ${MIN_PCT}% ${EXTRA[*]:-}"
cd "$(dirname "$0")"
APP=${APP:-./ppp-sip-isdn-test}
WORK=$(mktemp -d)
SRV_PID=; CLI_PID=
trap 'kill $SRV_PID $CLI_PID 2>/dev/null; rm -rf "$WORK"' EXIT
FAILED=0
mono_ms() { perl -MTime::HiRes=clock_gettime,CLOCK_MONOTONIC -e 'printf "%d\n", clock_gettime(CLOCK_MONOTONIC)*1000'; }
fail() { echo "FAIL: $*"; FAILED=1; }

$APP --id sip:server@127.0.0.1 --bindport 5556 --linecount $LINES --loglevel 1 "${EXTRA[@]}" \
     --ppplocalip 10.0.1.1 --pppremoteipstart 10.0.1.2 \
     >"$WORK/srv.out" 2>"$WORK/srv.err" &
SRV_PID=$!
sleep 1
kill -0 $SRV_PID 2>/dev/null || { echo "FAIL: server did not start"; cat "$WORK/srv.out" "$WORK/srv.err"; exit 1; }

$APP --id sip:client@127.0.0.1 --bindport 5557 --linecount $LINES --loglevel 1 "${EXTRA[@]}" \
     --dial sip:server@127.0.0.1:5556 \
     >"$WORK/cli.out" 2>"$WORK/cli.err" &
CLI_PID=$!

KILLED=0
if [ "$KILL" = half ] || [ "$KILL" = stagger ]; then
    # wait until every line is up, let them exchange for a while, then kill every second one
    for i in $(seq 1 300); do
        [ "$(grep -c '^FAKEUP' "$WORK/cli.err")" -ge "$LINES" ] && break
        sleep 0.1
    done
    sleep 1
    n=0
    for pid in $(pgrep -P $CLI_PID | sort -n); do
        n=$((n+1))
        if [ $((n % 2)) -eq 0 ]; then
            kill -TERM $pid && KILLED=$((KILLED+1)) && KILL_T+=($(mono_ms))
            [ "$KILL" = stagger ] && sleep $(awk "BEGIN{print $STAGGER_MS/1000}")
        fi
    done
    echo "killed $KILLED of $LINES fake pppd instances on the client side"
fi

# wait for the client to finish (it exits when all its lines are gone)
for i in $(seq 1 600); do kill -0 $CLI_PID 2>/dev/null || break; sleep 0.1; done
if kill -0 $CLI_PID 2>/dev/null; then fail "client did not finish in 60s"; kill -TERM $CLI_PID; fi
wait $CLI_PID; CLI_RC=$?
[ $CLI_RC -eq 0 ] || fail "client exited with $CLI_RC"

# give the server a moment to notice the hangup, then ask it to shut down
sleep 1
kill -TERM $SRV_PID 2>/dev/null
for i in $(seq 1 50); do kill -0 $SRV_PID 2>/dev/null || break; sleep 0.1; done
if kill -0 $SRV_PID 2>/dev/null; then fail "server did not exit on SIGTERM"; kill -KILL $SRV_PID; fi

SURVIVORS=$((LINES - KILLED))
# Per side, look at the lines whose pppd ran its full course (sent everything);
# lines killed on purpose stop short of that and are not judged.
# Pass = enough frames arrived intact (MIN_PCT of what was sent to those lines).
check_side() {
    local side=$1 role=$2 n=0 recv=0 bad=0 line
    while read -r line; do
        local sent=${line##*sent=}; sent=${sent%% *}
        local r=${line##*recv=}; r=${r%% *}
        local b=${line##*bad=}; b=${b%% *}
        if [ "$sent" -eq "$FRAMES" ]; then n=$((n+1)); recv=$((recv+r)); bad=$((bad+b)); fi
    done < <(grep "^FAKEPPPD role=$role" "$WORK/$side.err")
    local pct=0
    [ $n -gt 0 ] && pct=$(( recv * 100 / (n * FRAMES) ))
    echo "$role: $n/$SURVIVORS lines ran to completion, $recv/$((n*FRAMES)) frames intact ($pct%, need $MIN_PCT%), $bad bad"
    [ "$n" -eq "$SURVIVORS" ] || fail "$role: expected $SURVIVORS completed lines, got $n"
    [ "$pct" -ge "$MIN_PCT" ] || fail "$role: only $pct% of frames arrived intact"
    if [ "$MIN_PCT" -eq 100 ]; then [ "$bad" -eq 0 ] || fail "$role: $bad bad frames"; fi
}
check_side cli client
check_side srv server
n=$(grep -c '^FAKEPPPD' "$WORK/srv.err")
[ "$n" -eq "$LINES" ] || fail "server: expected $LINES result lines, got $n"
if [ ${#KILL_T[@]} -gt 0 ]; then
    # frame problems seen by lines that ran to completion, grouped by the kill that precedes them
    echo "kills at (ms rel. to first): $(for k in "${KILL_T[@]}"; do echo $((k-${KILL_T[0]})); done | tr '\n' ' ')"
    echo "problem frames at (ms rel. to first kill): $(cat "$WORK/srv.err" "$WORK/cli.err" | sed -n 's/^EVT t=\([0-9]*\).*/\1/p' | sort -n | while read -r e; do echo $((e-${KILL_T[0]})); done | tr '\n' ' ')"
    echo "EVENTS $(cat "$WORK/srv.err" "$WORK/cli.err" | sed -n 's/^EVT t=\([0-9]*\).*/\1/p' | \
        awk -v kills="${KILL_T[*]}" 'BEGIN{n=split(kills,k," ")} {t=$1; hit=0
            for(i=1;i<=n;i++) if(t>=k[i] && t<k[i]+1000 && (i==n || t<k[i+1])) {c[i]++; hit=1; break}
            if(!hit) other++}
          END{for(i=1;i<=n;i++) printf "kill%d=%d ", i, c[i]+0; printf "other=%d", other+0}')"
fi
if [ $FAILED -ne 0 ]; then
    for f in srv cli; do
        echo "--- $f stderr"; cat "$WORK/$f.err"
        echo "--- $f stream stats"; grep STREAMSTAT "$WORK/$f.out"
    done
    exit 1
fi
echo "PASS"
