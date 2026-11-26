# Hidden measurement of the host's frame pacing alone and with a light joined copy: tools/hostpace.sh <name> [fps cap] [w h] [vars]
cd "$(dirname "$0")/.."
tasklist | grep -qi Suffering2 && { echo "GAME RUNNING - skip"; exit 1; }
n=$1; cap=${2:-0}; w=${3:-3840}; h=${4:-1600}
(timeout 90 powershell -NoProfile -ExecutionPolicy Bypass -File tools/run.ps1 -Seconds 58 -Name ${n}_host -FpsCap $cap -Width $w -Height $h -Level '05_Prison_Admissions\05b_Prison_Kitchen' -Vars "S2_COOP_HOST=1;$5" -Tail 1 > /dev/null 2>&1 &)
sleep 12; sed -i 's/^fps_cap.*/fps_cap = 30/; s/^width.*/width = 640/; s/^height.*/height = 480/' out/build/release/settings.toml
sleep 14; timeout 50 powershell -NoProfile -ExecutionPolicy Bypass -File tools/join.ps1 -Seconds 26 -Address 127.0.0.1 -Script scripts/join_walk.txt -Name ${n}_join -Shot 20 > /dev/null 2>&1
for i in $(seq 1 30); do tasklist | grep -qi Suffering2 || break; sleep 1; done
for f in $(ls -t out/build/release/logs/*.log | head -2); do echo $f; grep "pace:\|joined from\|player left\|first picture\|coop: stream:\|for the stream:" $f | cut -c1-150; done
