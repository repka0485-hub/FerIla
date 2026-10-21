#!/system/bin/sh
# single-file esp: libesp.so, injector and the launcher apk are embedded at the end of this file.
# root. copy to phone, then:
#   su -c "sh /data/local/tmp/esp.sh"         installs + opens the launcher menu, press the button there
#   su -c "sh /data/local/tmp/esp.sh off"     remove wrap preload
PKG=com.ScarletWork.CounterFlame; D=/data/local/tmp
LIB=$D/libesp.so; INJ=$D/injector; APK=$D/esp_launcher.apk; ME=$D/esp.sh
[ "$(id -u)" = 0 ] || { echo "need root: su -c \"sh $0\""; exit 1; }
ex() { grep "^$1:" "$ME" | cut -c5- | base64 -d > "$2"; }

case "$1" in
off) setprop "wrap.$PKG" ""; echo removed; exit 0 ;;

inject) # started by the launcher: waits for the game, then injects
  [ -f "$LIB" ] && [ -f "$INJ" ] || { echo "FAILED: libesp/injector missing, run esp.sh without args first"; exit 1; }
  chcon u:object_r:apk_data_file:s0 "$LIB" 2>/dev/null
  setprop "wrap.$PKG" "LD_PRELOAD=$LIB"   # applies on the next game start, works on some roms
  echo "waiting for $PKG..."
  i=0; PID=""
  while [ $i -lt 600 ]; do
    PID=$(pidof "$PKG")
    [ -n "$PID" ] && grep -q libil2cpp.so /proc/$PID/maps 2>/dev/null && break
    PID=""; sleep 1; i=$((i+1))
  done
  [ -n "$PID" ] || { echo "game not started in time"; exit 1; }
  sleep 3
  if grep -q libesp.so /proc/$PID/maps; then echo "loaded via wrap, pid $PID"
  else
    "$INJ" "$PID" "$LIB"; sleep 1
    grep -q libesp.so /proc/$PID/maps && echo "loaded, menu is in the game" || echo "FAILED, check: logcat -d | grep -i -E 'ESP|avc'"
  fi
  exit 0 ;;

*) # default: unpack, install launcher, open it
  [ "$0" = "$ME" ] || cp "$0" "$ME" || { echo "copy failed"; exit 1; }
  ex LIB "$LIB" && ex INJ "$INJ" && ex APK "$APK" || { echo "extract failed"; exit 1; }
  chmod 755 "$LIB" "$INJ" "$ME"; chmod 644 "$APK"
  chcon u:object_r:apk_data_file:s0 "$LIB" 2>/dev/null
  pm install -r "$APK" >/dev/null 2>&1 || pm install -r -d "$APK" || { echo "launcher install failed"; exit 1; }
  am start -n com.esp.launcher/.LauncherMain >/dev/null
  echo "menu opened, press the button there"
  exit 0 ;;
esac
