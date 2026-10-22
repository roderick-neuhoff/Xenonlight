"""Minimal read-only TFTP server (RFC 1350 + blksize/tsize/timeout options) for XeLL network boot.

Serves files from one directory only. Write requests are refused.

Usage:
    python scripts/tftp_server.py [--bind 192.168.1.98] [--dir <folder with xenon.elf>]

XeLL asks for xenon.elf (and probes a few other names first). Every request is
logged, so you can see whether the console is reaching the PC at all.
"""
import argparse
import os
import socket
import struct
import threading
import time

OP_RRQ, OP_WRQ, OP_DATA, OP_ACK, OP_ERROR, OP_OACK = 1, 2, 3, 4, 5, 6
ERR_NOT_FOUND, ERR_ACCESS, ERR_ILLEGAL_OP = 1, 2, 4
MAX_RETRIES = 5


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def send_error(sock, addr, code, msg):
    sock.sendto(struct.pack("!HH", OP_ERROR, code) + msg.encode() + b"\0", addr)


def parse_request(packet):
    parts = packet[2:].split(b"\0")
    filename = parts[0].decode(errors="replace")
    mode = parts[1].decode(errors="replace").lower() if len(parts) > 1 else "octet"
    opts = {}
    rest = parts[2:]
    for i in range(0, len(rest) - 1, 2):
        if rest[i]:
            opts[rest[i].decode(errors="replace").lower()] = rest[i + 1].decode(errors="replace")
    return filename, mode, opts


def resolve(root, filename):
    """Maps a requested name into root; returns None for anything outside it."""
    name = filename.replace("\\", "/").lstrip("/")
    path = os.path.realpath(os.path.join(root, name))
    if os.path.commonpath([path, root]) != root or not os.path.isfile(path):
        return None
    return path


def wait_ack(sock, addr, block, timeout):
    """Waits for ACK of `block` from addr. Returns True on ACK, False on timeout."""
    sock.settimeout(timeout)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data, src = sock.recvfrom(1024)
        except socket.timeout:
            return False
        if src != addr or len(data) < 4:
            continue
        op, num = struct.unpack("!HH", data[:4])
        if op == OP_ACK and num == block & 0xFFFF:
            return True
        if op == OP_ERROR:
            raise ConnectionAbortedError(data[4:].split(b"\0")[0].decode(errors="replace"))
    return False


def transfer(bind_ip, addr, path, opts):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind_ip, 0))
    size = os.path.getsize(path)
    blksize, timeout = 512, 2.0
    accepted = {}

    if "blksize" in opts:
        try:
            blksize = max(8, min(int(opts["blksize"]), 65464))
            accepted["blksize"] = str(blksize)
        except ValueError:
            pass
    if "timeout" in opts:
        try:
            timeout = float(max(1, min(int(opts["timeout"]), 255)))
            accepted["timeout"] = str(int(timeout))
        except ValueError:
            pass
    if "tsize" in opts:
        accepted["tsize"] = str(size)

    start = time.time()
    try:
        if accepted:
            oack = struct.pack("!H", OP_OACK) + b"".join(
                k.encode() + b"\0" + v.encode() + b"\0" for k, v in accepted.items())
            for _ in range(MAX_RETRIES):
                sock.sendto(oack, addr)
                if wait_ack(sock, addr, 0, timeout):
                    break
            else:
                log(f"{addr[0]}: no ACK for option negotiation, giving up")
                return

        with open(path, "rb") as f:
            block = 1
            while True:
                chunk = f.read(blksize)
                packet = struct.pack("!HH", OP_DATA, block & 0xFFFF) + chunk
                for _ in range(MAX_RETRIES):
                    sock.sendto(packet, addr)
                    if wait_ack(sock, addr, block, timeout):
                        break
                else:
                    log(f"{addr[0]}: timed out at block {block}, giving up")
                    return
                if len(chunk) < blksize:
                    break
                block += 1
        secs = time.time() - start
        log(f"{addr[0]}: sent {os.path.basename(path)} ({size} bytes, blksize {blksize}) in {secs:.1f}s")
    except ConnectionAbortedError as e:
        log(f"{addr[0]}: client aborted transfer: {e}")
    finally:
        sock.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="192.168.1.98", help="IP address to listen on (default 192.168.1.98)")
    ap.add_argument("--port", type=int, default=69)
    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--dir", default=project_root,
                    help="directory to serve (default: the project folder, where make puts xenon.elf)")
    args = ap.parse_args()

    root = os.path.realpath(args.dir)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((args.bind, args.port))
    except OSError as e:
        raise SystemExit(f"Cannot listen on {args.bind}:{args.port}: {e}\n"
                         f"Is the Ethernet adapter set to {args.bind} and plugged in?")
    log(f"TFTP serving {root} on {args.bind}:{args.port} (Ctrl+C to stop)")
    if not os.path.isfile(os.path.join(root, "xenon.elf")):
        log("warning: xenon.elf is not in that directory")

    while True:
        packet, addr = sock.recvfrom(2048)
        if len(packet) < 4:
            continue
        op = struct.unpack("!H", packet[:2])[0]
        if op == OP_WRQ:
            send_error(sock, addr, ERR_ACCESS, "read-only server")
            continue
        if op != OP_RRQ:
            send_error(sock, addr, ERR_ILLEGAL_OP, "expected RRQ")
            continue
        filename, mode, opts = parse_request(packet)
        path = resolve(root, filename)
        if path is None:
            log(f"{addr[0]}: requested {filename!r} -> not found")
            send_error(sock, addr, ERR_NOT_FOUND, "file not found")
            continue
        log(f"{addr[0]}: requested {filename!r} ({mode}, options {opts or 'none'})")
        threading.Thread(target=transfer, args=(args.bind, addr, path, opts), daemon=True).start()


if __name__ == "__main__":
    main()
