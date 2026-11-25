#!/bin/bash
# Cold starts one after another until one draws no frame (the start-up hang); that run's folder then holds the
# scheduler's view of its threads (threads.txt) and its log. Each start has its own time limit (tools/coldboot.sh).
#   bash tools/coldhunt.sh [TRIES=12] [SECONDS=10] [PAUSE=0 seconds between starts]
proj="$(cd "$(dirname "$0")/.." && pwd)"
cd "$proj" || exit 2
tries="${1:-12}"; secs="${2:-10}"; pause="${3:-0}"
for i in $(seq 1 "$tries"); do
    [ "$pause" != "0" ] && sleep "$pause"
    out=$(timeout $((secs + 150)) bash tools/coldboot.sh "coldhunt_$i" "$secs" 2>&1); code=$?
    frames=$(echo "$out" | grep "seconds with frames" | sed 's/.*: //')
    echo "start $i: $frames seconds with frames (exit $code)"
    if [ "$code" != "0" ]; then echo "$out" | tail -4; echo "NO FRAMES in start $i: runs/coldhunt_$i"; exit 1; fi
done
echo "all $tries starts drew frames"
