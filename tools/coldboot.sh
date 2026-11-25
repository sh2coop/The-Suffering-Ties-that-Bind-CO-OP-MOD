#!/bin/bash
# One start of the game as a player gets it: nothing cached (tools/coldcache.py) and the window in front
# (tools/front.py). After SECONDS it prints the scheduler's view of the threads (tools/threads.py), says how many
# seconds had frames, and stops the game. Every step has its own time limit, so nothing here can wait for good.
#   bash tools/coldboot.sh NAME [SECONDS=10] [extra run.ps1 arguments]
proj="$(cd "$(dirname "$0")/.." && pwd)"
cd "$proj" || exit 2
name="${1:-coldboot}"; secs="${2:-10}"; shift 2 2>/dev/null
rel=out/build/release
old=$(cat $rel/test.pid 2>/dev/null || echo 0)
timeout 20 python tools/coldcache.py
# FRONT=1 also brings the window to the front (it takes the keyboard focus for the length of the run)
: > "runs/front_$name.txt"
if [ "$FRONT" = "1" ]; then (timeout 20 python tools/front.py --wait 10 --after "$old" > "runs/front_$name.txt" 2>&1 &); fi
timeout $((secs + 40)) powershell -NoProfile -ExecutionPolicy Bypass -File tools/run.ps1 -Seconds "$secs" -Keep -FpsCap 60 -VSync -Tail 0 -Name "$name" "$@" 2>&1 | grep -v "^captures\|^saved to" | cut -c1-200
cat "runs/front_$name.txt"
timeout 20 python tools/threads.py --seconds 1 > "runs/$name/threads.txt" 2>&1 || echo "threads.py did not finish in 20 s"
log="runs/$name/log.txt"
# no frame so far: leave the game up until its own freeze report is in the log (it comes 30 s after the start),
# at most 30 s more, then keep that log
if [ "$(grep -c 'perf: [0-9.]* fps' "$log")" = "0" ]; then
    live=$(ls -t $rel/logs/suffering2*.log | head -1)
    for i in $(seq 1 30); do grep -q "freeze report: end" "$live" && break; sleep 1; done
    cp "$live" "$log"
fi
timeout 30 powershell -NoProfile -ExecutionPolicy Bypass -File tools/stop.ps1
echo "seconds with frames: $(grep -c 'perf: [0-9.]* fps' "$log")"
echo "last line: $(tail -1 "$log" | cut -c1-150)"
[ "$(grep -c 'perf: [0-9.]* fps' "$log")" = "0" ] && exit 1
exit 0
