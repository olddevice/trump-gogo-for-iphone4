#!/bin/bash
#
# bench.sh — measure the renderer on the phone itself.
#
# Uploads the armv7 frame-cost probe plus the baked assets into /tmp/tgb and
# runs it there.  No app install, no SpringBoard, no window server: this is
# purely "how many milliseconds does one frame take on an A4".
#
#   ./bench.sh [--password xxx] [--frames N] [--size WxH]
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PORT=2222
PY="$(command -v python3 || true)"
SSH_EXP="$ROOT/scripts/ssh_password.exp"
UP_EXP="$ROOT/scripts/upload_password.exp"
PASSWORD="${IOS_PASSWORD:-alpine}"
FRAMES=120
SIZES=("480x720" "320x480")
REMOTE=/tmp/tgb

log()  { printf '\033[1;32m==> %s\033[0m\n' "$*"; }
fail() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --password) PASSWORD="$2"; shift 2 ;;
        --frames)   FRAMES="$2"; shift 2 ;;
        --size)     SIZES=("$2"); shift 2 ;;
        -h|--help)  sed -n '2,12p' "$0"; exit 0 ;;
        *)          fail "unknown option: $1" ;;
    esac
done

[ -x "$ROOT/build/bench_dev" ] || fail "build/bench_dev missing — run ./build.sh"
[ -f "$ROOT/assets/atlas.bin" ] || fail "assets/atlas.bin missing — run tools/gen_assets.py"

RELAY_PID=""
cleanup() { [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null || true; }
trap cleanup EXIT

if ! lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; then
    log "Starting USB->SSH tunnel"
    "$PY" "$ROOT/scripts/usbmux_relay.py" "$PORT" 22 >/tmp/iphone4-relay.log 2>&1 &
    RELAY_PID=$!
    for _ in $(seq 1 40); do
        lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1 && break
        sleep 0.25
    done
fi

remote() {
    expect "$SSH_EXP" "$PASSWORD" "$1" 2>/dev/null \
        | grep -vE "^(spawn |Warning: Permanently|root@127\.0\.0\.1's password: )" \
        | grep -vE "password: $" || true
}

log "Uploading probe and assets"
remote "mkdir -p $REMOTE"
expect "$UP_EXP" "$PASSWORD" "$ROOT/build/bench_dev"   "$REMOTE/bench_dev" >/dev/null 2>&1 || fail "upload bench_dev"
expect "$UP_EXP" "$PASSWORD" "$ROOT/assets/atlas.bin"  "$REMOTE/atlas.bin" >/dev/null 2>&1 || fail "upload atlas"
expect "$UP_EXP" "$PASSWORD" "$ROOT/assets/font.bin"   "$REMOTE/font.bin"  >/dev/null 2>&1 || fail "upload font"

remote "chmod 755 $REMOTE/bench_dev; ls -l $REMOTE"

# The probe must run detached: an A4 can take well over a minute for a
# hundred frames, and this sshd drops the session at the 60 s mark.
for size in "${SIZES[@]}"; do
    W="${size%x*}"; H="${size#*x}"
    log "Rendering $FRAMES frames at $size"
    remote "cd $REMOTE && rm -f run.out done.flag && \
            nohup sh -c './bench_dev atlas.bin font.bin $FRAMES $W $H > run.out 2>&1; \
                         touch done.flag' >/dev/null 2>&1 &" >/dev/null 2>&1 || true
    for _ in $(seq 1 90); do
        sleep 2
        remote "test -f $REMOTE/done.flag && echo READY" | grep -q READY && break
    done
    remote "cat $REMOTE/run.out" | sed 's/^/    /'
done

log "Done"
