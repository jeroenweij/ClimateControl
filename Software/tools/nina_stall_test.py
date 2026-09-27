#!/usr/bin/env python3
"""Bare-module NINA-W152 UART->TCP stall rig.

Runs with NO MainController firmware in the loop: flash NinaEnable on the
MainController board (it only releases NINA reset and asserts its UART_CTS),
wire a USB-TTL adapter to the NINA's own TXD/RXD/GND (header H1), and run this
on a PC on the same LAN.

The script plays both ends:
  * TCP server on this PC (the module dials in, like the real ccserver);
  * UART side over the adapter (brings the module up with AT commands, puts it
    in data mode, then echoes every request back as a reply).

The server sends a numbered request every --period ms. A request that shows
up on the UART proves peer->UART still works; its reply arriving back at the
server proves UART->peer works. A stall is: requests keep reaching the UART
(and get answered there) but no reply reaches the server for --stall-ms. That
is the MainController-Server-Link-Spec.md §11 item 2 signature.

Wi-Fi credentials come from Modules/MainController/Secrets.h (--secrets), or
the environment (never on the command line):
  NINA_SSID=... NINA_PSK=... ./nina_stall_test.py --host 192.168.2.29
"""

import argparse
import os
import re
import socket
import sys
import threading
import time

import serial

REQ_PREFIX = b"Q"
REP_PREFIX = b"R"
# Request padded to roughly an OTA Write frame; reply roughly an Ack frame.
REQ_PAD = b"x" * 40
REP_PAD = b"y" * 8


DEFAULT_SECRETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Modules", "MainController", "Secrets.h")


def read_secrets(path):
    """WifiSsid / WifiPassword from the MainController's Secrets.h (never printed)."""
    try:
        text = open(path).read()
    except OSError:
        return None, None
    values = {}
    for name in ("WifiSsid", "WifiPassword"):
        m = re.search(name + r'\s*=\s*"((?:[^"\\]|\\.)*)"', text)
        values[name] = m.group(1).encode().decode("unicode_escape") if m else None
    return values["WifiSsid"], values["WifiPassword"]


def log(msg):
    print(time.strftime("%H:%M:%S") + " " + msg, flush=True)


class Nina:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.05, rtscts=False)
        # Bytes and whole lines read past the line a wait was looking for:
        # a URC (+UUDPC, +UUNU) often arrives in the same read as the OK
        # before it, and must not be lost.
        self.buf = b""
        self.backlog = []

    def write(self, data):
        self.ser.write(data)

    def read_lines_until(self, want, timeout):
        """Collect lines until one starts with any of `want`; return (hit, lines)."""
        deadline = time.monotonic() + timeout
        lines = []
        while True:
            while self.backlog:
                text = self.backlog.pop(0)
                lines.append(text)
                if text.startswith(want):
                    return text, lines
            if time.monotonic() >= deadline:
                return None, lines
            self.buf += self.ser.read(256)
            while b"\r\n" in self.buf:
                line, self.buf = self.buf.split(b"\r\n", 1)
                text = line.decode(errors="replace").strip()
                if text:
                    self.backlog.append(text)

    def discard(self):
        self.ser.reset_input_buffer()
        self.buf = b""
        self.backlog = []

    def at(self, cmd, timeout=3.0, quiet=False):
        self.write(cmd.encode() + b"\r")
        hit, lines = self.read_lines_until(("OK", "ERROR"), timeout)
        if not quiet:
            shown = cmd.split(",\"")[0] + ",<secret>" if cmd.startswith(("AT+UWSC=0,2,", "AT+UWSC=0,8,")) else cmd
            log(f"  {shown} -> {hit}  {[l for l in lines if l not in ('OK', cmd)]}")
        return hit == "OK", lines

    def escape_to_command_mode(self):
        # Data-mode escape: >= 1 s silence, "+++", >= 1 s silence (use 2 s).
        time.sleep(2.2)
        self.write(b"+++")
        time.sleep(2.2)
        self.discard()

    def bring_up(self, ssid, psk, host, port, set_bt):
        log("Bringing the module up")
        for attempt in range(3):
            ok, _ = self.at("AT", quiet=True)
            if ok:
                break
            self.escape_to_command_mode()
        else:
            raise RuntimeError("module does not answer AT")
        self.at("ATE0")
        ok, lines = self.at("AT+UBTMODE?")
        bt = next((l.split(":", 1)[1] for l in lines if l.startswith("+UBTMODE:")), "?")
        log(f"Bluetooth mode = {bt} (0 = off)")
        if set_bt is not None and bt.strip() != str(set_bt):
            log(f"Setting AT+UBTMODE={set_bt}, storing, restarting")
            self.at(f"AT+UBTMODE={set_bt}")
            self.at("AT&W")
            self.at("AT+CPWROFF")
            self.read_lines_until(("+STARTUP",), 10)
            time.sleep(1)
            self.at("AT")
            self.at("ATE0")
            ok, lines = self.at("AT+UBTMODE?")
            bt = next((l.split(":", 1)[1] for l in lines if l.startswith("+UBTMODE:")), "?")
            log(f"Bluetooth mode now = {bt}")
        # Restart for a clean slate every run: a station or peer left active
        # by the previous run makes the station config commands fail. The
        # stored settings (UBTMODE) survive the restart.
        self.at("AT+CPWROFF")
        if not self.read_lines_until(("+STARTUP",), 10)[0]:
            raise RuntimeError("module did not restart (+STARTUP)")
        time.sleep(1)
        self.at("AT")
        self.at("ATE0")
        self.at(f'AT+UWSC=0,2,"{ssid}"')
        self.at("AT+UWSC=0,5,2")
        self.at(f'AT+UWSC=0,8,"{psk}"')
        self.at("AT+UWSCA=0,3", timeout=10)
        hit, _ = self.read_lines_until(("+UUNU",), 30)
        if not hit:
            raise RuntimeError("network did not come up (+UUNU)")
        # Network URCs come in bursts (up, down, up again): like NinaLink,
        # connect only after 3 s without any network event.
        log("Network up, waiting for 3 s without network events")
        settle_deadline = time.monotonic() + 30
        while True:
            hit, lines = self.read_lines_until(("+UUN", "+UUW"), 3)
            if not hit:
                break
            log(f"  network event {hit}")
            if time.monotonic() > settle_deadline:
                raise RuntimeError("network never settled")
        self.at(f'AT+UDCP="tcp://{host}:{port}/"', timeout=10)
        hit, lines = self.read_lines_until(("+UUDPC",), 15)
        if not hit:
            raise RuntimeError(f"peer did not connect (+UUDPC), saw {lines}")
        self.at("ATO")
        time.sleep(0.5)
        self.discard()
        log("Data mode")
        return bt.strip()


class Run:
    def __init__(self, period_ms, stall_ms):
        self.period = period_ms / 1000.0
        self.stall = stall_ms / 1000.0
        self.lock = threading.Lock()
        self.sent = 0
        self.at_uart = 0  # requests seen on the UART (peer -> UART works)
        self.replied = 0  # replies received by the server (UART -> peer works)
        self.last_uart = time.monotonic()
        self.last_reply = time.monotonic()
        self.stalls = []  # (start, end or None)
        self.stop = threading.Event()


def uart_echo(nina, run):
    buf = b""
    while not run.stop.is_set():
        buf += nina.ser.read(512)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if line.startswith(REQ_PREFIX):
                seq = line[1:7]
                nina.write(REP_PREFIX + seq + REP_PAD + b"\n")
                with run.lock:
                    run.at_uart += 1
                    run.last_uart = time.monotonic()


def server_loop(conn, run, duration):
    conn.settimeout(0.01)
    buf = b""
    start = time.monotonic()
    next_send = start
    stalled_since = None
    while time.monotonic() - start < duration:
        now = time.monotonic()
        if now >= next_send:
            conn.sendall(REQ_PREFIX + b"%06d" % (run.sent % 1000000) + REQ_PAD + b"\n")
            run.sent += 1
            next_send += run.period
        try:
            data = conn.recv(4096)
            if not data:
                log("Peer closed the socket")
                break
            buf += data
        except socket.timeout:
            pass
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if line.startswith(REP_PREFIX):
                with run.lock:
                    run.replied += 1
                    run.last_reply = time.monotonic()
        with run.lock:
            silent = now - run.last_reply
            uart_live = now - run.last_uart < 1.0
        if stalled_since is None and silent > run.stall and uart_live:
            stalled_since = run.last_reply
            run.stalls.append([stalled_since, None])
            log(f"STALL: no reply for {silent:.1f} s while requests still reach the UART")
        elif stalled_since is not None and silent < run.stall:
            run.stalls[-1][1] = now
            log(f"  recovered after {now - stalled_since:.1f} s")
            stalled_since = None
    run.stop.set()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="this PC's LAN address, as the module should dial it")
    ap.add_argument("--port", type=int, default=9100)
    ap.add_argument("--serial", default="/dev/ttyUSB0")
    ap.add_argument("--periods", default="130,160,200,250", help="request periods in ms, one run each")
    ap.add_argument("--duration", type=float, default=300, help="seconds per run")
    ap.add_argument("--stall-ms", type=float, default=2000)
    ap.add_argument("--set-bt", type=int, choices=(0, 3), help="force AT+UBTMODE (stored) before testing")
    ap.add_argument("--secrets", default=DEFAULT_SECRETS, help="MainController Secrets.h to take the Wi-Fi credentials from")
    args = ap.parse_args()

    ssid = os.environ.get("NINA_SSID")
    psk = os.environ.get("NINA_PSK")
    if not ssid or not psk:
        ssid, psk = read_secrets(args.secrets)
    if not ssid or not psk:
        sys.exit("no Wi-Fi credentials: set NINA_SSID/NINA_PSK or fill in " + args.secrets)

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", args.port))
    listener.listen(1)

    nina = Nina(args.serial)
    results = []
    set_bt = args.set_bt
    for period in [int(p) for p in args.periods.split(",")]:
        log(f"=== Run: period {period} ms, {args.duration:.0f} s ===")
        nina.escape_to_command_mode()
        accepted = {}

        def accept():
            listener.settimeout(240)  # covers 3 bring-up attempts
            try:
                accepted["conn"], accepted["addr"] = listener.accept()
            except socket.timeout:
                pass

        t = threading.Thread(target=accept)
        t.start()
        for attempt in range(3):
            try:
                bt = nina.bring_up(ssid, psk, args.host, args.port, set_bt)
                break
            except RuntimeError as e:
                log(f"Bring-up failed: {e}; retrying")
        else:
            sys.exit("bring-up failed 3 times")
        set_bt = None  # stored now, don't redo per run
        t.join()
        if "conn" not in accepted:
            sys.exit("module never connected to this PC -- check --host and the firewall")
        conn = accepted["conn"]
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        log(f"Module connected from {accepted['addr'][0]}")

        run = Run(period, args.stall_ms)
        echo = threading.Thread(target=uart_echo, args=(nina, run), daemon=True)
        echo.start()
        server_loop(conn, run, args.duration)
        echo.join(1)
        conn.close()
        stalled = sum((e or time.monotonic()) - s for s, e in run.stalls)
        results.append((period, bt, run.sent, run.at_uart, run.replied, len(run.stalls), stalled))
        log(f"Run done: sent {run.sent}, at UART {run.at_uart}, replies {run.replied}, "
            f"stalls {len(run.stalls)} ({stalled:.1f} s stalled)")

    print()
    print(f"{'period':>7} {'BT':>3} {'sent':>6} {'atUART':>7} {'replies':>8} {'stalls':>7} {'stalled s':>10}")
    for r in results:
        print(f"{r[0]:>6}ms {r[1]:>3} {r[2]:>6} {r[3]:>7} {r[4]:>8} {r[5]:>7} {r[6]:>10.1f}")


if __name__ == "__main__":
    main()
