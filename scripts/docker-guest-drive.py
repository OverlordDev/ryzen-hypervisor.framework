#!/usr/bin/env python3
# Drives the serial console of the Alpine guest started by docker-hvf-test.sh (QEMU serial on tcp:4555).
#
#   docker-guest-drive.py login              wait for the login prompt and log in as root
#   docker-guest-drive.py run SECONDS CMD... run commands one by one, print their output
import re
import socket
import sys
import time

PORT = 4555
sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
sock.settimeout(1)
buf = b""
log = open("/tmp/rhv-docker-serial.log", "ab")


def pump(wait):
    global buf
    end = time.time() + wait
    while time.time() < end:
        try:
            data = sock.recv(65536)
        except socket.timeout:
            continue
        if not data:
            break
        log.write(data)
        log.flush()
        buf += data


def expect(pattern, timeout):
    global buf
    end = time.time() + timeout
    rx = re.compile(pattern.encode())
    while time.time() < end:
        m = rx.search(buf)
        if m:
            out, buf = buf[:m.end()], buf[m.end():]
            return out.decode(errors="replace")
        pump(0.5)
    raise TimeoutError("timeout waiting for %r; tail: %r" % (pattern, buf[-300:]))


def run(cmd, timeout):
    sock.sendall((cmd + "; echo __RC_$?__\n").encode())
    return expect(r"__RC_(\d+)__\r?\n", timeout)


if sys.argv[1] == "login":
    sock.sendall(b"\n")
    expect(r"login: ", 600)
    sock.sendall(b"root\n")
    expect(r"# ", 30)
    print("logged in")
else:
    limit = int(sys.argv[2])
    for command in sys.argv[3:]:
        print(">>>", command)
        print(run(command, limit))
