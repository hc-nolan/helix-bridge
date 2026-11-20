# /// script
# requires-python = ">=3.11"
# dependencies = ["pyusb>=1.2"]
# ///
"""USB side of the HX Edit bridge: owns the Helix via libusb, serves Interposer.dll over TCP.

Protocol (little endian):
  request : u32 len, u8 op, payload[len-1]
  response: u32 len, i32 status, payload[len-4]
  ops: 1 ENUM -> u16 vid, u16 pid | 2 CTRL setup(8)+data -> IN data
       3 BULK_WRITE data | 9 SUBSCRIBE: connection becomes a stream of u32 len + bulk-IN data
"""

import argparse
import logging
import socket
import struct
import threading

import usb.core
import usb.util

VID = 0x0E41
EP_OUT, EP_IN, IFACE = 0x01, 0x81, 0
OP_ENUM, OP_CTRL, OP_BULK_WRITE, OP_SUBSCRIBE = 1, 2, 3, 9

log = logging.getLogger("helixd")


class Helix:
    """Owns the USB device; fans bulk-IN data out to subscribers."""

    def __init__(self):
        self.lock = threading.Lock()
        self.dev = None
        self.subscribers: list[socket.socket] = []
        self.sub_lock = threading.Lock()

    def open(self):
        """Find and claim the Helix control interface, start the IN reader."""
        dev = usb.core.find(idVendor=VID)
        if dev is None:
            raise RuntimeError("no Line 6 device found")
        if dev.is_kernel_driver_active(IFACE):
            dev.detach_kernel_driver(IFACE)
        usb.util.claim_interface(dev, IFACE)
        for ep in (EP_OUT, EP_IN):
            dev.clear_halt(ep)
        self.dev = dev
        log.info("opened %04x:%04x", dev.idVendor, dev.idProduct)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            try:
                data = bytes(self.dev.read(EP_IN, 16384, timeout=1000))
            except usb.core.USBTimeoutError:
                continue
            except usb.core.USBError as e:
                log.error("bulk read failed: %s", e)
                return
            if not data:
                continue
            log.debug("IN  %d %s", len(data), data[:32].hex(" "))
            frame = struct.pack("<I", len(data)) + data
            with self.sub_lock:
                for s in list(self.subscribers):
                    try:
                        s.sendall(frame)
                    except OSError:
                        self.subscribers.remove(s)

    def handle(self, op: int, pl: bytes) -> tuple[int, bytes]:
        """Run one request op; returns (status, response payload)."""
        if op == OP_ENUM:
            return 0, struct.pack("<HH", self.dev.idVendor, self.dev.idProduct)
        if op == OP_CTRL:
            bmrt, breq, wval, widx, wlen = struct.unpack_from("<BBHHH", pl)
            with self.lock:
                if bmrt & 0x80:
                    data = bytes(self.dev.ctrl_transfer(bmrt, breq, wval, widx, wlen,
                                                        timeout=2000))
                else:
                    self.dev.ctrl_transfer(bmrt, breq, wval, widx, pl[8:], timeout=2000)
                    data = b""
            log.debug("CTRL %02x %02x %04x %04x %d -> %s",
                      bmrt, breq, wval, widx, wlen, data.hex(" "))
            return 0, data
        if op == OP_BULK_WRITE:
            log.debug("OUT %d %s", len(pl), pl[:32].hex(" "))
            # The Line 6 driver pads bulk OUT transfers to 4-byte multiples;
            # the device waits for the padding before answering.
            pl += b"\0" * (-len(pl) % 4)
            with self.lock:
                self.dev.write(EP_OUT, pl, timeout=2000)
            return 0, b""
        return -1, b""


def recv_exact(s: socket.socket, n: int) -> bytes:
    """Read exactly n bytes or raise ConnectionError."""
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError
        buf += chunk
    return buf


def serve_client(h: Helix, s: socket.socket):
    """Serve request/response ops for one DLL connection."""
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        while True:
            (n,) = struct.unpack("<I", recv_exact(s, 4))
            msg = recv_exact(s, n)
            op, pl = msg[0], msg[1:]
            if op == OP_SUBSCRIBE:
                log.info("subscriber connected")
                with h.sub_lock:
                    h.subscribers.append(s)
                return  # socket now owned by reader thread
            try:
                st, out = h.handle(op, pl)
            except usb.core.USBError as e:
                log.warning("op %d failed: %s", op, e)
                st, out = -(e.errno or 5), b""
            s.sendall(struct.pack("<Ii", 4 + len(out), st) + out)
    except (ConnectionError, OSError):
        log.info("client disconnected")
        s.close()


def main():
    """Entry point."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=47800)
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    h = Helix()
    h.open()
    srv = socket.create_server(("127.0.0.1", a.port))
    log.info("listening on 127.0.0.1:%d", a.port)
    while True:
        c, _ = srv.accept()
        threading.Thread(target=serve_client, args=(h, c), daemon=True).start()


if __name__ == "__main__":
    main()
