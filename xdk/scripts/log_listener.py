"""Shows the debug log that moonlight.xex broadcasts on the LAN (UDP port 47555).

    python xdk/scripts/log_listener.py [logfile]

Each datagram is one line: the console's uptime in ms, then the message.
Lines are printed and, if a file is given, appended to it.
"""
import datetime
import socket
import sys

PORT = 47555

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", PORT))
out = open(sys.argv[1], "a", encoding="utf-8") if len(sys.argv) > 1 else None
print(f"listening on UDP {PORT}", flush=True)

while True:
    data, (ip, _) = sock.recvfrom(2048)
    line = f"{datetime.datetime.now():%H:%M:%S.%f}"[:-3] + f" {ip} " + data.decode("utf-8", "replace").rstrip()
    print(line, flush=True)
    if out:
        out.write(line + "\n")
        out.flush()
