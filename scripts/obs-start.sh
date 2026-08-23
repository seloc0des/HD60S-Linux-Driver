#!/usr/bin/env bash
# Feed the HD60 S into a v4l2loopback device so OBS sees it as a camera.
#
# Until the capture path works this shows a status frame reporting what the
# receiver actually sees -- which is more useful than a black screen while the
# USB 3 link is still being sorted out.
set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)
DEV=${DEV:-}
SIZE=${SIZE:-1280x720}
FPS=${FPS:-30}
LABEL=${LABEL:-"Elgato HD60 S"}

die() { echo "error: $*" >&2; exit 1; }

# --- v4l2loopback -------------------------------------------------------
if ! modinfo v4l2loopback >/dev/null 2>&1; then
  cat >&2 <<'MSG'
v4l2loopback is not installed. It is what lets OBS see this as a camera.

  sudo apt install v4l2loopback-dkms v4l-utils

Then re-run this script.
MSG
  exit 1
fi

if ! lsmod | grep -q '^v4l2loopback'; then
  echo "loading v4l2loopback..."
  sudo modprobe v4l2loopback devices=1 exclusive_caps=1 card_label="$LABEL" ||
    die "modprobe failed"
  sleep 1
fi

# --- find the loopback node --------------------------------------------
if [ -z "$DEV" ]; then
  for d in /dev/video*; do
    [ -e "$d" ] || continue
    n=$(basename "$d" | tr -d 'video')
    if grep -qs v4l2loopback "/sys/class/video4linux/video$n/name" 2>/dev/null ||
       grep -qs "$LABEL" "/sys/class/video4linux/video$n/name" 2>/dev/null; then
      DEV=$d; break
    fi
  done
fi
[ -n "$DEV" ] || die "could not find the v4l2loopback node; set DEV=/dev/videoN"

echo "loopback device : $DEV"
echo "format          : $SIZE @ ${FPS}fps YUY2"
echo
echo "In OBS: Sources -> + -> Video Capture Device (V4L2) -> $DEV"
echo "        (if OBS was already running, restart it so it rescans)"
echo
echo "Ctrl-C here stops the feed."
echo

exec "$REPO/hd60sd" --source device \
    --script "$REPO/scripts/vendor-bringup.hd60s" \
    --sink "v4l2:$DEV" --size "$SIZE" --fps "$FPS"
