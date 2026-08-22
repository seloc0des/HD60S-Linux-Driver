#!/usr/bin/env bash
# Poll for the HD60 S and dump descriptors the moment it enumerates.
# Catches the SuperSpeed personality without having to babysit the replug.
set -u
VID=${VID:-0fd9}; PID=${PID:-005e}
OUT=${OUT:-docs/enumerations}
mkdir -p "$OUT"
here=$(cd "$(dirname "$0")/.." && pwd)
last=""
echo "watching for $VID:$PID  (ctrl-c to stop)"
while true; do
  node=""
  for d in /sys/bus/usb/devices/*; do
    [ -f "$d/idVendor" ] || continue
    [ "$(cat "$d/idVendor" 2>/dev/null)" = "$VID" ] || continue
    [ "$(cat "$d/idProduct" 2>/dev/null)" = "$PID" ] || continue
    node=$d; break
  done
  if [ -n "$node" ]; then
    speed=$(cat "$node/speed" 2>/dev/null)
    key="$(basename "$node"):$speed"
    if [ "$key" != "$last" ]; then
      ts=$(date +%Y%m%d-%H%M%S)
      echo "[$ts] ENUMERATED $(basename "$node") at ${speed}M"
      # snapshot everything we can reach without root
      f="$OUT/enum-$ts-${speed}M"
      "$here/tools/hd60s-descdump.py" --vid "$VID" --pid "$PID" \
          --json "$f.json" > "$f.txt" 2>&1
      cp "$node/descriptors" "$f.descriptors.bin" 2>/dev/null
      [ -e "$node/bos_descriptors" ] && cp "$node/bos_descriptors" "$f.bos.bin" 2>/dev/null
      echo "        saved $f.{txt,json,descriptors.bin}"
      if [ "$speed" -ge 5000 ] 2>/dev/null; then
        echo "        *** SUPERSPEED LINK -- this is the personality we want ***"
      else
        echo "        (still USB 2.0 fallback: ${speed}M)"
      fi
      last="$key"
    fi
  else
    [ -n "$last" ] && echo "[$(date +%H:%M:%S)] device removed"
    last=""
  fi
  sleep 1
done
