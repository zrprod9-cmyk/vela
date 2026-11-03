#!/bin/sh
# RE4 GPU fault hunt on the console: one command before the user plays, one after.
# Runs as the console's normal user, no sudo; everything in /tmp/vela_faultrun, removed by "clean".
#
#   scp tools/fault_run.sh <diag dll> <user>@<console>:/tmp/
#   sh /tmp/fault_run.sh start nodll   RE4 without our DLL (WINEDLLOVERRIDES n,b falls back to Wine's dxgi)
#   sh /tmp/fault_run.sh start diag    the diagnostic DLL (/tmp/vela_fsr2_dxgi_faultdiag.dll) with
#                                      VELA_FAULT_CYCLE=75 RECON_VACHECK=1: phases A..E, 75 s each, in one session
#   ... the user plays RE4 in a level (diag: >= 7 min, through all five phases), then quits the game ...
#   sh /tmp/fault_run.sh stop          plugin DLL and launch options back (md5 / exact text), logs collected
#   sh /tmp/fault_run.sh clean         after the logs were copied off: /tmp/vela_faultrun removed
set -u
D="$HOME/.local/share/Steam/steamapps/common/RESIDENT EVIL 4  BIOHAZARD RE4"
W=/tmp/vela_faultrun
APP=2050650
STEAM="python3 /tmp/steam.py"
mode=${2:-}

lo_get() { $STEAM getlo $APP | python3 -c "import ast,sys; sys.stdout.write(ast.literal_eval(sys.stdin.read().strip()))"; }

case "${1:-}" in
start)
    pgrep -x re4.exe >/dev/null && { echo "RE4 is running: quit it first"; exit 1; }
    [ "$mode" = nodll ] || [ "$mode" = diag ] || { echo "start nodll|diag"; exit 2; }
    [ -e "$W/started" ] && { echo "a run is already set up: stop it first"; exit 1; }
    mkdir -p "$W"
    cp "$D/dxgi.dll" "$W/dxgi.dll.plugin"
    md5sum "$D/dxgi.dll" | cut -d' ' -f1 > "$W/dxgi.md5"
    lo_get > "$W/launch.orig"
    [ -s "$W/launch.orig" ] || { echo "cannot read the launch options"; exit 1; }
    [ -f "$D/sgsr3intercept.log" ] && mv "$D/sgsr3intercept.log" "$W/sgsr3intercept.before.log"
    if [ "$mode" = nodll ]; then
        rm -f "$D/dxgi.dll"
    else
        [ -f /tmp/vela_fsr2_dxgi_faultdiag.dll ] || { echo "no /tmp/vela_fsr2_dxgi_faultdiag.dll"; exit 1; }
        cp /tmp/vela_fsr2_dxgi_faultdiag.dll "$D/dxgi.dll.tmp" && mv "$D/dxgi.dll.tmp" "$D/dxgi.dll"
        printf 'VELA_FAULT_CYCLE=75 RECON_VACHECK=1 %s' "$(cat "$W/launch.orig")" > "$W/launch.diag"
        $STEAM setlo $APP "$W/launch.diag"
    fi
    # kernel messages with wall-clock time (the DLL logs local time at each phase change)
    ( journalctl -k -f -o short-iso-precise 2>/dev/null || dmesg -w -T 2>/dev/null ) \
        | grep --line-buffered -i "gpu fault\|fault.*iova" > "$W/faults_$mode.log" &
    echo $! > "$W/logger.pid"
    echo "$mode" > "$W/started"
    date '+%F %T' > "$W/start_time"
    echo "ready: $mode. The user can start RE4 now. Faults go to $W/faults_$mode.log"
    ;;
stop)
    [ -e "$W/started" ] || { echo "nothing set up"; exit 1; }
    pgrep -x re4.exe >/dev/null && { echo "RE4 is still running: quit it first"; exit 1; }
    mode=$(cat "$W/started")
    pkill -P "$(cat "$W/logger.pid")" 2>/dev/null; kill "$(cat "$W/logger.pid")" 2>/dev/null
    date '+%F %T' > "$W/stop_time"
    [ -f "$D/sgsr3intercept.log" ] && mv "$D/sgsr3intercept.log" "$W/sgsr3intercept_$mode.log"
    cp "$W/dxgi.dll.plugin" "$D/dxgi.dll.tmp" && mv "$D/dxgi.dll.tmp" "$D/dxgi.dll"
    [ -f "$W/sgsr3intercept.before.log" ] && cp "$W/sgsr3intercept.before.log" "$D/sgsr3intercept.log"
    $STEAM setlo $APP "$W/launch.orig"
    now=$(lo_get)
    [ "$now" = "$(cat "$W/launch.orig")" ] && echo "launch options restored" || echo "WARNING: launch options differ"
    [ "$(md5sum "$D/dxgi.dll" | cut -d' ' -f1)" = "$(cat "$W/dxgi.md5")" ] && echo "plugin dxgi.dll restored" \
        || echo "WARNING: dxgi.dll md5 differs"
    rm -f "$W/started" "$W/logger.pid"
    echo "faults ($mode): $(wc -l < "$W/faults_$mode.log") lines from $(cat "$W/start_time") to $(cat "$W/stop_time")"
    ls -la "$W"
    ;;
clean)
    [ -e "$W/started" ] && { echo "a run is still set up: stop it first"; exit 1; }
    rm -rf "$W"
    echo "removed $W"
    ;;
*)
    echo "usage: $0 start nodll|diag | stop | clean"; exit 2 ;;
esac
