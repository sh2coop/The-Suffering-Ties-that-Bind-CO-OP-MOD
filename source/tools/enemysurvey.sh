# Every enemy template of every level, lined up six at a time in hidden runs: pictures runs/sv_<level>_<first>_a/b.png,
# what was made in runs/survey.txt. Stops if a game is running that no test started.
cd "$(dirname "$0")/.."
: > runs/survey.txt
for lvl in '02_Streets_North\02a_Tunnels_to_Apt' '02_Streets_North\02b_Apartment' '03_Streets_South\03a_Streets' '03_Streets_South\03b_Streets' '03_Streets_South\03c_Streets' '02_Streets_North\02c_Tunnels_to_Harbor' '01_Harbor\01b_Harbor_Return' '04_Streets_East\04a_Truck_Chase' '05_Prison_Admissions\05_Prison_Admissions' '06_Prison_Cellblock\06_Cellblock' '07_Prison_Industry\07a_Industry' '07_Prison_Industry\07b_Industry' '08_Drowningpool\08_Drowningpool' '09_Slums_Grouphome\09A_Slums_Grouphome' '09_Slums_Grouphome\09B_Slums_Grouphome' '01_Harbor\01a_Harbor_Base' '00_Prison_Riot\00a_Prison_Riot'; do
  short=$(echo "$lvl" | sed 's/.*\//' | cut -c1-3)
  first=0; total=6
  while [ $first -lt $total ] && [ $first -lt 36 ]; do
    out=$(FAR=5 T1=0.5 T2=0.9 timeout 115 bash tools/lineup.sh sv_${short}_${first} "$lvl" $first 6 "S2_TEST_LINEUP_GAP=1.5")
    case "$out" in *"GAME RUNNING"*) echo "stopped: a game is running" >> runs/survey.txt; exit 1;; esac
    echo "$short $first: $out" >> runs/survey.txt
    n=$(echo "$out" | sed -n 's/.*level has \([0-9]*\) such.*/\1/p'); [ -n "$n" ] && total=$n || break
    first=$((first + 6))
  done
done
echo done >> runs/survey.txt
