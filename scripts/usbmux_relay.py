#!/usr/bin/env python3
"""
usbmux_relay.py - pure-stdlib USB-to-TCP relay for iPhones (drop-in replacement
for libimobiledevice's iproxy, no Homebrew needed).

Talks the usbmuxd wire protocol directly:
  plist message  : header <4I> (total_len, version=1, tag, msgtype=8) + XML plist
  after Connect  : raw bidirectional TCP stream (no framing), confirmed against
                   libusbmuxd/usbmuxd sources (libimobiledevice).

Usage: usbmux_relay.py <local_port> <remote_port> [device_udid]
Example: usbmux_relay.py 2222 22
"""
import plistlib
import select
import socket
import struct
import sys
import threading

USBMUXD_SOCKET = "/var/run/usbmuxd"
MSG_PLIST = 8
PROTO_VERSION = 1  # plist protocol version (libusbmuxd: proto_version = 1)
TAG = 1


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("usbmuxd closed connection")
        buf += chunk
    return buf


def send_plist(sock, message, tag=TAG):
    payload = plistlib.dumps(message)  # XML plist, bytes
    # Header field order is length, version, MESSAGE, tag -- message and tag
    # must not be swapped or usbmuxd silently ignores the request.
    header = struct.pack("<4I", 16 + len(payload), PROTO_VERSION, MSG_PLIST, tag)
    sock.sendall(header + payload)


def recv_plist(sock):
    hdr = recv_exact(sock, 16)
    length, version, msgtype, tag = struct.unpack("<4I", hdr)
    payload = recv_exact(sock, length - 16)
    if msgtype == MSG_PLIST:
        return plistlib.loads(payload)
    raise ValueError("unexpected usbmuxd message type %d" % msgtype)


def find_device(udid=None, timeout=5.0):
    """Connect to usbmuxd, listen for Attached events, return DeviceID.

    Raises ConnectionError on timeout (no device attached).
    """
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(USBMUXD_SOCKET)
    s.settimeout(timeout)          # Attached for already-present devices is
                                   # replayed immediately; timeout means none
    send_plist(s, {"MessageType": "Listen", "ProgName": "claw-relay",
                   "kLibUSBMuxVersion": 2})
    try:
        while True:
            msg = recv_plist(s)
            mtype = msg.get("MessageType")
            if mtype == "Attached":
                props = msg.get("Properties", {})
                dev_udid = props.get("SerialNumber")
                dev_id = props.get("DeviceID", msg.get("DeviceID"))
                if udid is None or dev_udid == udid:
                    return dev_id, dev_udid
    except socket.timeout:
        raise ConnectionError("no device attached (timeout)")
    finally:
        s.close()


def open_device_channel(device_id, remote_port):
    """Open a dedicated usbmuxd connection and start a tunnel to remote_port."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(USBMUXD_SOCKET)
    # PortNumber must be in network byte order (htons), per libusbmuxd
    send_plist(s, {"MessageType": "Connect", "ProgName": "claw-relay",
                   "kLibUSBMuxVersion": 2,
                   "DeviceID": int(device_id),
                   "PortNumber": ((remote_port & 0xFF) << 8) | (remote_port >> 8)})
    result = recv_plist(s)
    if result.get("Number", 1) != 0:
        raise ConnectionError("usbmuxd Connect failed: %r" % result)
    return s


def pump(a, b):
    """Bidirectional raw pump; closes both sockets when one side ends."""
    try:
        socks = [a, b]
        while True:
            r, _, x = select.select(socks, [], socks)
            if x:
                break
            for s in r:
                data = s.recv(65536)
                if not data:
                    raise ConnectionError("closed")
                (b if s is a else a).sendall(data)
    except OSError:
        pass
    finally:
        for s in (a, b):
            try:
                s.close()
            except OSError:
                pass


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: usbmux_relay.py <local_port> <remote_port> [device_udid]")
    local_port = int(sys.argv[1])
    remote_port = int(sys.argv[2])
    udid = sys.argv[3] if len(sys.argv) > 3 else None

    device_id, dev_udid = find_device(udid)
    print("usbmux_relay: device found id=%s udid=%s" % (device_id, dev_udid), flush=True)
    print("usbmux_relay: listening on 127.0.0.1:%d -> device:%d"
          % (local_port, remote_port), flush=True)

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", local_port))
    server.listen(4)

    while True:
        client, addr = server.accept()
        try:
            mux = open_device_channel(device_id, remote_port)
        except Exception as e:
            print("usbmux_relay: connect failed: %s" % e, flush=True)
            client.close()
            continue
        threading.Thread(target=pump, args=(client, mux), daemon=True).start()


if __name__ == "__main__":
    main()
