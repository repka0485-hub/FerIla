#!/system/bin/sh
# single-file esp: libesp.so + injector are embedded at the end of this file.
# root. copy to phone, then:
#   su -c "sh /data/local/tmp/esp.sh"        run it, then open the game yourself, it injects on its own
#   su -c "sh /data/local/tmp/esp.sh off"    remove wrap preload
PKG=com.ScarletWork.CounterFlame; LIB=/data/local/tmp/libesp.so; INJ=/data/local/tmp/injector
[ "$1" = "off" ] && { setprop "wrap.$PKG" ""; echo removed; exit 0; }
[ "$(id -u)" = 0 ] || { echo "need root: su -c \"sh $0\""; exit 1; }
grep '^LIB:' "$0" | cut -c5- | base64 -d > "$LIB" && grep '^INJ:' "$0" | cut -c5- | base64 -d > "$INJ" || { echo "extract failed"; exit 1; }
chmod 755 "$LIB" "$INJ"; chcon u:object_r:apk_data_file:s0 "$LIB" 2>/dev/null
setprop "wrap.$PKG" "LD_PRELOAD=$LIB"   # applies on the next game start, works on some roms
echo "waiting for $PKG, open the game now..."
i=0; PID=""
while [ $i -lt 600 ]; do
  PID=$(pidof "$PKG")
  [ -n "$PID" ] && grep -q libil2cpp.so /proc/$PID/maps 2>/dev/null && break
  PID=""; sleep 1; i=$((i+1))
done
[ -n "$PID" ] || { echo "game was not started in time"; exit 1; }
sleep 3
if grep -q libesp.so /proc/$PID/maps; then echo "loaded via wrap, pid $PID"
else
  echo "ptrace inject into $PID"; "$INJ" "$PID" "$LIB"; sleep 1
  grep -q libesp.so /proc/$PID/maps && echo "loaded" || echo "FAILED, check: logcat -d | grep -i -E 'ESP|avc'"
fi
echo "menu shows in the game ~10s after load. logs: logcat -d -s ESP"
exit 0
