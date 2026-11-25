# Hidden two-copy test of the joiner's waiting screen: tools/joinwait.sh <name> [width height]
# The host waits in its main menu and starts a new game at 62 s; the joiner pastes the code at 48 s.
# Pictures of the joiner's window: runs/<name>_1.png (connected, waiting), _2 (joining), _3 (in the game)
cd "$(dirname "$0")/.."
S=$(mktemp -d); n=$1; w=${2:-1280}; h=${3:-720}
tasklist | grep -qi Suffering2 && { echo "GAME RUNNING - skip"; exit 1; }
before=$(ls out/build/release/logs | sort | tail -1)
(timeout 140 powershell -NoProfile -ExecutionPolicy Bypass -File tools/run.ps1 -Seconds 112 -Name ${n}_host -FpsCap 60 -Width $w -Height $h -Script scripts/host_wait.txt -Vars "S2_COOP_HOST=1" -Tail 3 > $S/host.out 2>&1 &)
sleep 4
(timeout 135 powershell -NoProfile -ExecutionPolicy Bypass -File tools/join.ps1 -Seconds 104 -Address "" -Script scripts/join_wait.txt -Name ${n}_join -Vars "S2_KEYS=48:v;S2_KEYS_PASTE=127.0.0.1" > $S/join.out 2>&1 &)
sleep 3; jp=$(grep -o "process [0-9]*" $S/join.out | grep -o "[0-9]*")
sleep 48; python tools/winshot.py runs/${n}_1.png 0 $jp
for i in $(seq 1 180); do grep -qs "says JOINING GAME" out/build/release/logs/*.log -l --include="*" 2>/dev/null && ls -t out/build/release/logs/*.log | head -2 | xargs grep -qs "says JOINING GAME" && break; sleep 0.25; done
python tools/winshot.py runs/${n}_2.png 0 $jp; sleep 4; python tools/winshot.py runs/${n}_3.png 0 $jp
for i in $(seq 1 90); do tasklist | grep -qi Suffering2 || break; sleep 1; done
ls -t out/build/release/logs/*.log | head -2 | xargs grep -h "screen says\|first picture\|coop: joined\|pasted" | cut -c1-150
