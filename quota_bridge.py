"""Read Codex plan limits through the official local app-server protocol.

Only sanitized percentages, window lengths and PC-local reset times are
exposed to the ESP32 over LAN.
No Codex credential or account identifier is sent to the device.
"""

import argparse
import json
import queue
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def request_limits(timeout=20):
    process = subprocess.Popen(
        ["codex", "app-server", "--listen", "stdio://"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        encoding="utf-8",
        bufsize=1,
    )
    lines = queue.Queue()

    def reader():
        for line in process.stdout:
            lines.put(line)

    threading.Thread(target=reader, daemon=True).start()

    def send(message):
        process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        process.stdin.flush()

    def receive(request_id):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                message = json.loads(lines.get(timeout=min(1, deadline - time.monotonic())))
            except queue.Empty:
                if process.poll() is not None:
                    raise RuntimeError("Codex app-server exited early")
                continue
            if message.get("id") == request_id:
                if "error" in message:
                    raise RuntimeError(str(message["error"]))
                return message.get("result", {})
        raise TimeoutError("Codex app-server did not answer")

    try:
        send({"method": "initialize", "id": 1, "params": {"clientInfo": {
            "name": "prague_quota_display", "title": "Prague Quota Display", "version": "1.0.0"
        }}})
        receive(1)
        send({"method": "initialized", "params": {}})
        send({"method": "account/rateLimits/read", "id": 2})
        return receive(2)
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def sanitize_window(window, now=None):
    if not isinstance(window, dict) or window.get("usedPercent") is None:
        return (-1, 0, 0)
    used = float(window["usedPercent"])
    if not 0 <= used <= 100:
        raise ValueError("Unexpected usedPercent")
    remaining = round(100 - used)
    duration = max(0, int(window.get("windowDurationMins") or 0))
    reset = window.get("resetsAt")
    # MMDDHHMM fits an ESP32 signed int and is unambiguous for these short
    # quota windows. The PC supplies its local timezone; no ESP32 RTC needed.
    reset_local = int(time.strftime("%m%d%H%M", time.localtime(int(reset)))) if reset else 0
    return (remaining, duration, reset_local)


def sanitize_result(result, now=None):
    buckets = result.get("rateLimitsByLimitId") or {}
    limits = buckets.get("codex") or result.get("rateLimits")
    if not isinstance(limits, dict) or limits.get("limitId") not in (None, "codex"):
        raise ValueError("No Codex plan limit in account response")
    primary = sanitize_window(limits.get("primary"), now)
    secondary = sanitize_window(limits.get("secondary"), now)
    if primary[0] < 0 and secondary[0] < 0:
        raise ValueError("No Codex usage windows available; check Codex login")
    return primary + secondary


class State:
    values = None
    updated = 0
    lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/quota":
            self.send_error(404)
            return
        with State.lock:
            fresh = State.values is not None and time.monotonic() - State.updated < 600
            body = ",".join(map(str, State.values)).encode("ascii") + b"\n" if fresh else b"unavailable\n"
        self.send_response(200 if fresh else 503)
        self.send_header("Content-Type", "text/plain; charset=ascii")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format, *_args):
        pass


def refresh_forever(interval):
    while True:
        try:
            values = sanitize_result(request_limits())
            with State.lock:
                State.values = values
                State.updated = time.monotonic()
            print("Codex windows refreshed (no account data exposed)", flush=True)
        except Exception as exc:
            print(f"Quota refresh failed: {exc}", flush=True)
        time.sleep(interval)


def discovery_forever(port):
    """Reply only to the expected LAN broadcast; never send account data here."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", port))
        while True:
            payload, address = sock.recvfrom(64)
            if payload == b"PRAGUE_QUOTA_DISCOVER/1":
                sock.sendto(b"PRAGUE_QUOTA_HERE/1", address)


def main():
    parser = argparse.ArgumentParser(description="Codex quota bridge for the ESP32 Prague display")
    parser.add_argument("--once", action="store_true", help="Test local Codex login, then exit")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--discovery-port", type=int, default=8788)
    parser.add_argument("--interval", type=int, default=120, help="Codex refresh seconds")
    args = parser.parse_args()
    if args.once:
        print("remaining%,window minutes,local reset MMDDHHMM (primary then secondary)")
        print(",".join(map(str, sanitize_result(request_limits()))))
        return
    threading.Thread(target=refresh_forever, args=(max(args.interval, 30),), daemon=True).start()
    threading.Thread(target=discovery_forever, args=(args.discovery_port,), daemon=True).start()
    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"Serving sanitized quotas on TCP {args.port}; LAN discovery on UDP {args.discovery_port}.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
