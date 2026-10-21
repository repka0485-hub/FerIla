#!/system/bin/sh
# single-file esp: libesp.so and injector are embedded at the end of this file.
# root. run ONCE: starts the game and injects, then keeps watching and injects on every later game start:
#   su -c "sh /data/local/tmp/esp.sh"         start watcher (until reboot)
#   su -c "sh /data/local/tmp/esp.sh nogles"  same, but start the game normally (no forced GLES)
#   su -c "sh /data/local/tmp/esp.sh boot"    same + autostart after every reboot (magisk service.d)
#   su -c "sh /data/local/tmp/esp.sh off"     stop watcher, remove autostart
#   log: cat /data/local/tmp/esp.log
PKG=com.ScarletWork.CounterFlame; D=/data/local/tmp
LIB=$D/libesp.so; INJ=$D/injector; ME=$D/esp.sh; PIDF=$D/esp.pid; BOOT=/data/adb/service.d/esp_watch.sh
[ "$(id -u)" = 0 ] || { echo "need root: su -c \"sh $0\""; exit 1; }
ex() { grep "^$1:" "$ME" | cut -c5- | base64 -d > "$2"; }
stop() { [ -f "$PIDF" ] && kill "$(cat $PIDF)" 2>/dev/null; rm -f "$PIDF"; }

case "$1" in
off) stop; rm -f "$BOOT"; setprop "wrap.$PKG" ""; echo "stopped"; exit 0 ;;

watch) # the background loop
  echo $$ > "$PIDF"; LAST=""
  echo "$(date +%T) watching $PKG"
  while true; do
    PID=$(pidof "$PKG"); PID=${PID%% *}
    if [ -n "$PID" ] && [ "$PID" != "$LAST" ] && grep -q libil2cpp.so /proc/$PID/maps 2>/dev/null; then
      sleep 3
      grep -q libesp.so /proc/$PID/maps 2>/dev/null || "$INJ" "$PID" "$LIB"
      if grep -q libesp.so /proc/$PID/maps 2>/dev/null; then echo "$(date +%T) injected, pid $PID"
      else echo "$(date +%T) FAILED pid $PID, check: logcat -d | grep -i -E 'ESP|avc'"; fi
      ( sleep 20; { echo "--- diag pid $PID"
          echo "maps: vulkan=$(grep -c libvulkan /proc/$PID/maps) egl=$(grep -c libEGL /proc/$PID/maps) gles3=$(grep -c libGLESv3 /proc/$PID/maps) il2cpp=$(grep -c libil2cpp /proc/$PID/maps) esp=$(grep -c libesp /proc/$PID/maps)"
          logcat -d -s ESP | tail -30
          logcat -d | grep -i -E "avc.*(libesp|CounterFlame|injector)" | tail -5
          echo "--- end diag"; } >> "$D/esp.log" 2>&1 ) &
      LAST=$PID
    fi
    sleep 1
  done ;;

*) # default / boot: unpack and start the watcher
  [ "$0" = "$ME" ] || cp "$0" "$ME" || { echo "copy failed"; exit 1; }
  ex LIB "$LIB" && ex INJ "$INJ" || { echo "extract failed"; exit 1; }
  chmod 755 "$LIB" "$INJ" "$ME"; chcon u:object_r:apk_data_file:s0 "$LIB" 2>/dev/null
  setprop "wrap.$PKG" ""                 # old preload method off, ptrace only
  stop
  nohup setsid sh "$ME" watch >"$D/esp.log" 2>&1 </dev/null &
  if [ "$1" = "boot" ]; then
    mkdir -p /data/adb/service.d && printf '#!/system/bin/sh\nsh %s watch >%s 2>&1 </dev/null &\n' "$ME" "$D/esp.log" > "$BOOT" && chmod 755 "$BOOT" && echo "autostart on"
  fi
  # restart the game with OpenGL ES forced (the menu draws through GLES, a Vulkan game would show nothing)
  ACT=$(cmd package resolve-activity --brief -c android.intent.category.LAUNCHER "$PKG" 2>/dev/null | tail -1)
  am force-stop "$PKG"; sleep 1
  if [ "$1" = "nogles" ] || [ -z "$ACT" ]; then monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
  else am start -n "$ACT" -e unity "-force-gles" >/dev/null 2>&1; fi
  echo "watcher started, game launching, injects by itself (log: cat $D/esp.log)"
  exit 0 ;;
esac
