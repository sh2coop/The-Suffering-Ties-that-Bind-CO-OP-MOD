#!/bin/sh
# Pictures of the Display and Co-op page as the window shows it, at one window size.
#   sh tools/menushots.sh 1280 720 NAME      (pictures in runs/NAME: off, join, entry, host)
cd "$(dirname "$0")/.."
W=$1; H=$2; N=$3
mkdir -p runs/$N
(timeout 130 powershell -NoProfile -File tools/run.ps1 -Seconds 68 -Script scripts/online_look.txt -Name $N -Width $W -Height $H \
  -Vars "COOP_FAKE_ADDRESS=203.0.113.7;COOP_FAKE_LOCAL=192.168.1.20" -Tail 1 > runs/$N/run.out 2>&1) &
# the game starts about 3.5 s after this script does
python tools/winshot.py runs/$N/1_off.png 48
python tools/winshot.py runs/$N/2_join.png 3.2
python tools/winshot.py runs/$N/3_entry.png 6.2
python tools/winshot.py runs/$N/4_host.png 9.2
wait
tail -1 runs/$N/run.out
