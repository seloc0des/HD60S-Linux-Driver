#!/usr/bin/env bash
# Feed the HD60 S into OBS on Linux: video via v4l2loopback, audio via a
# PipeWire/PulseAudio null sink. Requires a SuperSpeed (USB 3) link.
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
DEV=${DEV:-}
SIZE=${SIZE:-1920x1080}
FPS=${FPS:-60}
ALT=${ALT:-4}                 # bulk (alt 4) is lossless; audio works on 2 and 4
ARATE=${ARATE:-71633}        # measured delivery rate; ear-confirmed vs 48k (runs vary 70.9-71.6k)
LABEL=${LABEL:-"Elgato HD60 S"}
FIFO=/tmp/hd60s-audio.pcm
die() { echo "error: $*" >&2; exit 1; }

# --- video loopback ---
modinfo v4l2loopback >/dev/null 2>&1 || die "install v4l2loopback-dkms v4l-utils"
lsmod | grep -q '^v4l2loopback' || {
  echo "loading v4l2loopback..."
  sudo modprobe v4l2loopback devices=1 exclusive_caps=1 card_label="$LABEL" || die "modprobe failed"
  sleep 1
}
if [ -z "$DEV" ]; then
  for d in /dev/video*; do
    [ -e "$d" ] || continue
    grep -q "$LABEL" "/sys/class/video4linux/$(basename "$d")/name" 2>/dev/null && { DEV=$d; break; }
  done
fi
[ -n "$DEV" ] || die "no v4l2loopback node found"

# --- audio null sink (PipeWire/PulseAudio) ---
if command -v pactl >/dev/null 2>&1; then
  pactl list short modules 2>/dev/null | grep -q 'sink_name=hd60s' || \
    pactl load-module module-null-sink sink_name=hd60s \
      sink_properties=device.description="Elgato_HD60_S_Audio" >/dev/null
  rm -f "$FIFO"; mkfifo "$FIFO"
  # play the daemon's PCM into the null sink; OBS captures "Monitor of Elgato HD60 S Audio"
  ( pacat --playback --device=hd60s --rate="$ARATE" --channels=2 --format=s16le --latency-msec=50 < "$FIFO" ) &
  APID=$!
  AUDIO_ARGS="--audio-out $FIFO"
  echo "audio          : PipeWire sink 'hd60s' @ ${ARATE}Hz  (OBS source: Monitor of Elgato HD60 S Audio)"
else
  AUDIO_ARGS=""; APID=""
  echo "audio          : no PulseAudio/PipeWire (pactl) found -- video only"
fi

echo "video          : $DEV  $SIZE @ ${FPS}fps YUY2 (alt $ALT)"
echo "In OBS: Video Capture Device (V4L2) -> $DEV ; Audio Input Capture -> Monitor of Elgato HD60 S Audio"
echo "Ctrl-C stops the feed."
cleanup() { [ -n "$APID" ] && kill "$APID" 2>/dev/null; rm -f "$FIFO"; }
trap cleanup EXIT

exec "$REPO/hd60sd" --source device \
    --script "$REPO/scripts/stream-cold.hd60s" \
    --post-script "$REPO/scripts/audio-block.hd60s" \
    --alt "$ALT" $AUDIO_ARGS \
    --sink "v4l2:$DEV" --size "$SIZE" --fps "$FPS"
