# One line-up run, hidden: tools/lineup.sh <name> <level> <first> <how many> [extra vars]
# Two pictures of the game window right after the enemies are made: runs/<name>_a.png, runs/<name>_b.png
cd "$(dirname "$0")/.."
for i in $(seq 1 60); do tasklist | grep -qi Suffering2 || break; sleep 0.5; done
if tasklist | grep -qi Suffering2; then echo "GAME RUNNING - skip"; exit 1; fi
n=$1; lvl=$2; first=$3; many=$4; extra=$5
before=$(ls out/build/release/logs/suffering2*.log 2>/dev/null | sort | tail -1)
(timeout 100 powershell -NoProfile -ExecutionPolicy Bypass -File tools/run.ps1 -Seconds 24 -Name $n -FpsCap 60 -Width ${W:-1920} -Height ${H:-1080} -Level "$lvl" -Vars "S2_TEST_LINEUP=360,$first,$many,${FAR:-5};S2_TEST_GOD=1;S2_TEST_LINEUP_CREATURES=1;$extra" -Tail 3 > /dev/null 2>&1 &)
# the line-up is made when this run's log says so; the pictures 0.4 and 1.2 s later
L=""
for i in $(seq 1 120); do sleep 0.25; L=$(ls -t out/build/release/logs/suffering2*.log 2>/dev/null | head -1); if [ "$L" != "$before" ] && grep -q "lineup: the level" "$L" 2>/dev/null; then break; fi; done
sleep ${T1:-0.3}; python tools/winshot.py runs/${n}_a.png 0 > /dev/null; sleep ${T2:-0.5}; python tools/winshot.py runs/${n}_b.png 0 > /dev/null
grep "lineup:" "$L" | sed 's/.*lineup: //' | tr '\n' '|'; echo
for i in $(seq 1 60); do tasklist | grep -qi Suffering2 || break; sleep 0.5; done
