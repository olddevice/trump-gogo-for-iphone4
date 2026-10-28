#!/bin/bash
#
# push.sh — one-click install of TrumpGoGo onto a jailbroken iPhone 4 over USB.
#
#   ./push.sh                      # uses the default root password (alpine)
#   ./push.sh --password mypass    # if you changed the root password
#   ./push.sh --probe              # only report device info, install nothing
#   ./push.sh --rebuild            # run build.sh first
#
# Requirements on the Mac: python3 (system one is fine), /usr/bin/expect,
# /usr/bin/ssh — all preinstalled. No Homebrew, no iproxy, no sshpass.
#
# Requirements on the phone: jailbroken iOS 7.x with OpenSSH (Cydia -> OpenSSH).
#
# How it works: usbmuxd is spoken to directly over the USB cable, an SSH
# channel to port 22 is opened, the password is fed with expect, the .deb is
# streamed in over `ssh ... cat > /tmp/TrumpGoGo.deb` (the sftp subsystem is
# unreliable on old OpenSSH) and dpkg runs detached on the device so a dropped
# link can never leave dpkg half-finished.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PORT=2222
RELAY="$ROOT/scripts/usbmux_relay.py"
SSH_EXP="$ROOT/scripts/ssh_password.exp"
UPLOAD_EXP="$ROOT/scripts/upload_password.exp"
DEV_INSTALL="$ROOT/scripts/device_install.sh"
PASSWORD="${IOS_PASSWORD:-alpine}"
DO_PROBE=0
DO_REBUILD=0

log()  { printf '\033[1;32m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!!  %s\033[0m\n' "$*"; }
fail() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --password) PASSWORD="$2"; shift 2 ;;
        --probe)    DO_PROBE=1; shift ;;
        --rebuild)  DO_REBUILD=1; shift ;;
        -h|--help)  sed -n '2,16p' "$0"; exit 0 ;;
        *)          fail "unknown option: $1" ;;
    esac
done

PY="$(command -v python3 || true)"
[ -n "$PY" ] || fail "python3 not found"
[ -x /usr/bin/expect ] || fail "/usr/bin/expect missing (it ships with macOS)"
[ -x "$RELAY" ] || fail "missing $RELAY"
[ -f "$SSH_EXP" ] || fail "missing $SSH_EXP"
[ -f "$UPLOAD_EXP" ] || fail "missing $UPLOAD_EXP"
[ -f "$DEV_INSTALL" ] || fail "missing $DEV_INSTALL"

# ---- 0. package ----------------------------------------------------------
if [ "$DO_REBUILD" -eq 1 ]; then
    log "Rebuilding"
    "$ROOT/build.sh"
fi
if ! ls "$ROOT"/build/TrumpGoGo_*_iphoneos-arm.deb >/dev/null 2>&1; then
    log "No .deb yet — building first"
    "$ROOT/build.sh"
fi
DEB="$(ls -t "$ROOT"/build/TrumpGoGo_*_iphoneos-arm.deb | head -1)"
log "Package: $(basename "$DEB") ($(wc -c <"$DEB" | tr -d ' ') bytes)"

# ---- 1. device on USB? ---------------------------------------------------
log "Looking for the iPhone over USB"
DEV_LINE="$("$PY" -c "
import sys; sys.path.insert(0, '$ROOT/scripts')
import usbmux_relay as r
try:
    d, u = r.find_device(timeout=6)
    print('%s %s' % (d, u))
except Exception:
    print('')
")"
[ -n "$DEV_LINE" ] || fail "No iPhone detected. Plug it in, unlock it, and retry."
set -- $DEV_LINE
DEV_ID="$1"; DEV_UDID="$2"
log "Device id=$DEV_ID udid=$DEV_UDID"

# ---- 2. USB -> SSH tunnel -----------------------------------------------
RELAY_PID=""

relay_start() {
    log "Starting USB->SSH tunnel 127.0.0.1:$PORT -> device:22"
    "$PY" "$RELAY" "$PORT" 22 >/tmp/iphone4-relay.log 2>&1 &
    RELAY_PID=$!
    for _ in $(seq 1 40); do
        lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1 && return 0
        sleep 0.25
    done
    return 1
}

relay_stop() {
    [ -n "$RELAY_PID" ] || return 0
    kill "$RELAY_PID" 2>/dev/null || true
    wait "$RELAY_PID" 2>/dev/null || true
    RELAY_PID=""
}

trap 'relay_stop' EXIT

if lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; then
    log "Tunnel on 127.0.0.1:$PORT already up — reusing (not owned by this run)"
else
    relay_start || fail "could not start the USB tunnel"
fi

# Run one command on the device, echoing its remote stdout.
remote() {
    expect "$SSH_EXP" "$PASSWORD" "$1" 2>/dev/null \
        | grep -vE "^(spawn |Warning: Permanently|root@127\.0\.0\.1's password: )" \
        | grep -vE "password: $" || true
}

# ---- 3. authenticate ----------------------------------------------------
log "Logging in as root"
LOGIN_OK=0
for attempt in 1 2 3; do
    if remote "echo LOGIN-OK" | grep -q LOGIN-OK; then LOGIN_OK=1; break; fi
    # An iPhone 4 needs ~10 s just to answer the SSH banner, and old sshd drops
    # sessions when hammered: pause, then rebuild the tunnel in case it is stale.
    sleep 5
    [ "$attempt" -eq 2 ] && { relay_stop; relay_start || true; }
done
if [ "$LOGIN_OK" -ne 1 ]; then
    fail "SSH login failed. Is OpenSSH installed and is the password right?
  default jailbreak password: alpine
  override it with: ./push.sh --password <your root password>"
fi
log "Login OK"

# ---- 4. probe only? -----------------------------------------------------
if [ "$DO_PROBE" -eq 1 ]; then
    log "Device report (arch, model, dpkg arch, kernel, free /, app present)"
    remote 'uname -m && sysctl -n hw.model && dpkg --print-architecture && uname -r && df -h / && ls -d /Applications/TrumpGoGo.app'
    exit 0
fi

# ---- 5. upload + install ------------------------------------------------
log "Uploading package over the SSH stream"
expect "$UPLOAD_EXP" "$PASSWORD" "$DEB" "/tmp/TrumpGoGo.deb" >/dev/null 2>&1 \
    || fail "upload failed"

# The installer runs detached on the device: a respring (or a dropped USB link,
# or a killed ssh session) must never interrupt dpkg halfway through.
expect "$UPLOAD_EXP" "$PASSWORD" "$ROOT/scripts/device_install.sh" \
    "/tmp/dev_install.sh" >/dev/null 2>&1 || fail "could not stage the installer"

log "Installing on the device (detached, then collecting the result)"
remote 'nohup sh /tmp/dev_install.sh >/dev/null 2>&1 &' >/dev/null 2>&1 || true

# dpkg + uicache + respring take ~20-30 s on an iPhone 4. Poll the log file a
# few times; each attempt is a short, self-terminating ssh session.
RESULT=""
for _ in 1 2 3 4 5 6; do
    sleep 8
    RESULT="$(remote 'cat /tmp/dpkg.out')"
    echo "$RESULT" | grep -q "^DONE" && break
    [ -z "$RESULT" ] && { relay_stop; relay_start || true; }
done

echo "$RESULT" | grep -vE "^DONE$" | sed 's/^/    /'

if ! echo "$RESULT" | grep -q "RC=0"; then
    fail "dpkg reported an error (or timed out) — run ./push.sh --probe and retry."
fi

log "Installed. TrumpGoGo is on the home screen (SpringBoard was respringed)."
echo "    Bundle: com.claw.trumpgogo   |   Remove with: dpkg -r com.claw.trumpgogo"
echo "    Lean the phone to steer, tap the screen to jump."
