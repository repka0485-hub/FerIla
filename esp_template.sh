#!/system/bin/sh
# single-file esp: libesp.so and injector are embedded at the end of this file.
# root. run ONCE, it stays in the background and injects every time the game starts:
#   su -c "sh /data/local/tmp/esp.sh"         start watcher (until reboot)
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
  echo "watcher started, just open the game"
  exit 0 ;;
esac
