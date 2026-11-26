# Hidden two-copy test of joining with a short code through the meeting point: tools/meetjoin.sh <name> [vars for the joiner]
# The host hosts from its main menu and starts a new game at 62 s; the joiner pastes the host's code at 48 s of its own time.
# Pictures of the joiner's window: runs/<name>_1.png (after the paste), _2 (joining), _3 (in the game)
cd "$(dirname "$0")/.."
T=$(mktemp -d); n=$1
tasklist | grep -qi Suffering2 && { echo "GAME RUNNING - skip"; exit 1; }
before=$(ls -t out/build/release/logs | head -1)
(timeout 140 powershell -NoProfile -ExecutionPolicy Bypass -File tools/run.ps1 -Seconds 115 -Name ${n}_host -FpsCap 60 -Width 1280 -Height 720 -Script scripts/host_wait.txt -Vars "S2_NOTHING=1" -Tail 3 > $T/host.out 2>&1 &)
code=""
for i in $(seq 1 200); do sleep 0.25; L=$(ls -t out/build/release/logs/*.log | head -1); [ "$(basename "$L")" = "$before" ] && continue; code=$(grep -o "this game's code is [A-Z]*-[0-9]*" "$L" 2>/dev/null | tail -1 | grep -o "[A-Z]*-[0-9]*$"); [ -n "$code" ] && break; done
echo "host's code: ${code:-none}"; hostlog=$L
[ -z "$code" ] && code="NONE-0000"
(timeout 135 powershell -NoProfile -ExecutionPolicy Bypass -File tools/join.ps1 -Seconds 100 -Address "" -Script scripts/join_wait.txt -Name ${n}_join -Vars "S2_KEYS=48:v;S2_KEYS_PASTE=my code is $code ok;$2" > $T/join.out 2>&1 &)
sleep 3; jp=$(grep -o "process [0-9]*" $T/join.out | grep -o "[0-9]*")
sleep 49; python tools/winshot.py runs/${n}_1.png 0 $jp > /dev/null
for i in $(seq 1 160); do ls -t out/build/release/logs/*.log | head -2 | xargs grep -qs "says JOINING GAME" && break; sleep 0.25; done
python tools/winshot.py runs/${n}_2.png 0 $jp > /dev/null; sleep 4; python tools/winshot.py runs/${n}_3.png 0 $jp > /dev/null
for i in $(seq 1 90); do tasklist | grep -qi Suffering2 || break; sleep 1; done
for f in $(ls -t out/build/release/logs/*.log | head -2); do echo "== $f"; grep -h "meet:\|screen says\|first picture\|coop: joined\|pasted\|menu: join\|remote player\|expecting\|No answer\|no answer" $f | sed 's/[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}/<ip>/g' | cut -c1-170; done
