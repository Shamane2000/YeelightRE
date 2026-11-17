#!/usr/bin/env python3
"""Probe an Yeelight LAN-control port (TCP 55443) for its RPC method surface.

Read-only by design: only GET_METHODS are sent, plus at most one idempotent
set_ps write (cfg_lan_ctrl=1) which is skipped unless the device already
reports lan_ctrl=1.

Usage: python3 tools/lan_method_probe.py <ip> [<ip> ...]
"""
import json, socket, sys

READ_METHODS = [
    ("get_prop", ["fw_ver", "model", "lan_ctrl", "power"]),
    ("get_prop", ["lan_ctrl"]),
    ("get_ps", ["cfg_lan_ctrl"]),
    ("get_aging_status", []),
    ("miio_test", []),
    ("__probe_no_such_method__", []),
]
WRITE_PROBE = ("set_ps", ["cfg_lan_ctrl", "1"])


class Bulb:
    def __init__(self, ip, port=55443, timeout=5):
        self.sock = socket.create_connection((ip, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""

    def call(self, mid, method, params):
        req = json.dumps({"id": mid, "method": method, "params": params}) + "\r\n"
        self.sock.sendall(req.encode())
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return json.loads(line.decode())

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def probe(ip):
    print("=" * 70)
    print("DEVICE", ip)
    b = Bulb(ip)
    out = {}
    try:
        for mid, (m, p) in enumerate(READ_METHODS, start=1):
            try:
                r = b.call(mid, m, p)
            except Exception as e:
                r = {"transport_error": repr(e)}
            out[(m, tuple(p))] = r
            print("  %-26s %s" % (m + str(p), json.dumps(r)))
        lc = out.get(("get_prop", ("lan_ctrl",)), {}).get("result")
        if lc == ["1"]:
            try:
                r = b.call(90, WRITE_PROBE[0], WRITE_PROBE[1])
            except Exception as e:
                r = {"transport_error": repr(e)}
            print("  %-26s %s" % ("set_ps[cfg_lan_ctrl,1]", json.dumps(r)))
            try:
                print("  %-26s %s" % ("readback get_prop", json.dumps(b.call(91, "get_prop", ["lan_ctrl"]))))
            except Exception as e:
                print("  readback error", repr(e))
        else:
            print("  SKIP set_ps probe: lan_ctrl =", lc)
    finally:
        b.close()
    return out


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    results = {}
    for ip in sys.argv[1:]:
        try:
            results[ip] = probe(ip)
        except Exception as e:
            print("DEVICE", ip, "unreachable:", repr(e))
