#!/bin/bash
# MiSTer Moonlight: loads the H264 core and opens Moonlight's menu (Sunshine's apps,
# settings, pairing). During the stream Select+Start+LB+RB (or Ctrl+Alt+Shift+Q) goes back
# to the menu; B in the menu exits to the MiSTer's menu.
D=/media/fat/MiSTer_moonlight
BIN=$D/mister_moonlight
[ -x "$BIN" ] || chmod +x "$BIN" 2>/dev/null
[ -f $D/mister_moonlight.conf ] || cp $D/mister_moonlight.conf.example $D/mister_moonlight.conf 2>/dev/null
eval "$(tr -d '\r' < $D/mister_moonlight.conf)"
# HOST is needed only without the menu (MENU=0); with the menu PCs are added from the menu itself
[ "$HOST" = 192.168.1.100 ] && HOST=
if [ "$MENU" = 0 ] && [ -z "$HOST" ]; then
  echo "Set HOST (the address of the PC with Sunshine) in $D/mister_moonlight.conf"
  exit 1
fi
# the core: MiSTer_moonlight/core/H264_*.rbf (or _Other/H264_*.rbf), unless EXTRA has -core
case " $EXTRA " in
  *" -core "*) ;;
  *) if ! ls /media/fat/MiSTer_moonlight/core/H264_*.rbf /media/fat/_Other/H264_*.rbf 2>/dev/null | grep -q .; then
       echo "H264 core not found: copy H264_*.rbf to MiSTer_moonlight/core on the SD"
       exit 1
     fi ;;
esac
if pidof mister_moonlight >/dev/null; then
  echo "MiSTer Moonlight is already running"
  exit 1
fi
# Wi-Fi power saving makes the network pauses worse: it is turned off
for W in /sys/class/net/wlan*; do
  [ -e "$W" ] || continue
  N=$(basename $W)
  iw dev $N set power_save off 2>/dev/null || iwconfig $N power off 2>/dev/null
done
LOG=$D/last_stream.log
# During the stream the log stays in RAM (writing to the SD can block, and wears it); it goes to
# the SD only on exit. With LOG_EVERY=<seconds> in the configuration also during the stream (for
# an unclean exit: the MiSTer switched off halfway), but every copy rewrites the whole file
TMPLOG=/tmp/mister_moonlight.log
LOG_EVERY=${LOG_EVERY:-${LOG_OGNI:-0}}
NETINFO="script start: uptime $(cut -d' ' -f1 /proc/uptime) s; network: $(ip route 2>/dev/null | head -1)"
echo "Loading the H264 core and opening Moonlight${HOST:+ ($HOST)}... log in $LOG"
# Detached from the script: it survives the core's loading. At the end it goes back to the
# MiSTer's menu, but only if the H264 core is still running.
setsid nohup bash -c "
  echo \"[script] $NETINFO\"
  COPIER=
  if [ \"$LOG_EVERY\" -gt 0 ] 2>/dev/null; then ( while sleep $LOG_EVERY; do cp $TMPLOG $LOG; done ) & COPIER=\$!; fi
  if [ \"$MENU\" = 0 ]; then
    $BIN stream $HOST -app \"$APP\" $EXTRA
  else
    $BIN menu $HOST $EXTRA
  fi
  echo \"[script] exit: \$?\"
  [ -n \"\$COPIER\" ] && kill \$COPIER
  cp $TMPLOG $LOG
  [ \"\$(cat /tmp/CORENAME 2>/dev/null)\" = H264 ] && echo 'load_core /media/fat/menu.rbf' > /dev/MiSTer_cmd
" > $TMPLOG 2>&1 < /dev/null &
sleep 1
