#!/usr/bin/env python3
"""Read-only identity probe of a candidate Yeelight host.

1) plaintext JSON-RPC get_prop on TCP/55443 (what fw65 speaks)
2) TLS ClientHello on the same port -> dump what the peer answers
3) SSDP unicast M-SEARCH (ST: wifi_bulb) to the host
Nothing is written to the device.
"""
import json
import socket
import ssl
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.173"
PORT = 55443


def plaintext_get_prop():
    req = json.dumps({"id": 1, "method": "get_prop",
                      "params": ["fw_ver", "model", "lan_ctrl", "name", "power"]}) + "\r\n"
    print("== 1) plaintext JSON-RPC ==")
    print("--> " + req.strip())
    try:
        s = socket.create_connection((HOST, PORT), timeout=6)
        s.settimeout(6)
        s.sendall(req.encode())
        chunks = []
        t0 = time.time()
        while time.time() - t0 < 6:
            try:
                b = s.recv(4096)
            except socket.timeout:
                break
            if not b:
                break
            chunks.append(b)
        data = b"".join(chunks)
        print(f"<-- {len(data)} bytes: {data!r}")
        if data:
            print("    hex: " + data.hex(" "))
        s.close()
    except Exception as e:  # noqa: BLE001
        print(f"<-- EXCEPTION {type(e).__name__}: {e}")


def tls_probe():
    print("\n== 2) TLS ClientHello on the same port ==")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        raw = socket.create_connection((HOST, PORT), timeout=8)
        raw.settimeout(8)
        tls = ctx.wrap_socket(raw)
        print(f"<-- TLS handshake OK: {tls.version()} cipher={tls.cipher()}")
        der = tls.getpeercert(binary_form=True)
        if der:
            print(f"    peer cert DER {len(der)} bytes sha256={__import__('hashlib').sha256(der).hexdigest()}")
        try:
            tls.sendall(b'{"id":1,"method":"get_prop","params":["fw_ver","model"]}\r\n')
            a = tls.recv(4096)
            print(f"<-- after TLS send: {a!r}")
        except Exception as e:  # noqa: BLE001
            print(f"<-- TLS send/recv EXCEPTION {type(e).__name__}: {e}")
        tls.close()
    except ssl.SSLError as e:
        print(f"<-- SSLError: {e}")
    except Exception as e:  # noqa: BLE001
        print(f"<-- EXCEPTION {type(e).__name__}: {e}")


def ssdp_probe(host, st="wifi_bulb", port=1982, timeout=4):
    msg = ("M-SEARCH * HTTP/1.1\r\n"
           "HOST: 239.255.255.250:1982\r\n"
           'MAN: "ssdp:discover"\r\n'
           f"ST: {st}\r\n"
           "MX: 2\r\n\r\n").encode()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(msg, (host, port))
    out = []
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            data, addr = s.recvfrom(4096)
            out.append((addr, data.decode(errors="replace")))
        except socket.timeout:
            break
    s.close()
    return out


print(f"== 3) SSDP unicast M-SEARCH to {HOST}:1982 (ST=wifi_bulb) ==")
res = ssdp_probe(HOST)
if not res:
    print("<-- no answer (timeout)")
for addr, txt in res:
    print(f"<-- from {addr}:")
    for line in txt.splitlines():
        if line.strip():
            print("    " + line)

plaintext_get_prop()
tls_probe()
