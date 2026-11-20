# helix-bridge

Run Line 6 **HX Edit** on Linux under Wine and talk to a real Helix over USB.

Tested with HX Edit 3.82 and a Helix LT on Arch Linux. Presets read and write.

## How it works

HX Edit doesn't use libusb for Helix hardware. It talks to Line 6's
`Interposer.dll`, which in turn uses a Windows kernel driver that Wine can't
load. This project swaps both out:

```
HX Edit.exe ──► Interposer.dll (fake, 32-bit) ──TCP 127.0.0.1:47800──► helixd.py ──pyusb──► Helix
             (inside Wine)                                            (native Linux)
```

- `interposer.c` is a drop-in `Interposer.dll`. It exports the same `Interposer*`
  functions HX Edit calls and forwards control and bulk transfers over a local socket.
- `helixd.py` is a small pyusb daemon. It claims the Helix's vendor interface,
  runs the bulk transfers, and streams IN data back to the DLL.

The USB protocol is passed through untouched. HX Edit does all the real work.

## Requirements

- Wine with 32-bit support (WoW64 is fine)
- [zig](https://ziglang.org/) to cross-compile the DLL
- [uv](https://docs.astral.sh/uv/) to run the daemon (it pulls in `pyusb`)
- [just](https://github.com/casey/just)
- `7z` (p7zip) to unpack the installer
- libusb
- The official **HX Edit Windows installer** from line6.com. It isn't included here.

## Setup

1. Give your user access to Line 6 USB devices:

   ```sh
   sudo cp udev/50-line6-helix.rules /etc/udev/rules.d/
   sudo udevadm control --reload && sudo udevadm trigger
   ```

2. Create the Wine prefix and unpack HX Edit into it. The default prefix is
   `~/.local/share/hxedit-wine`. To use another one, set `HXEDIT_WINEPREFIX`
   in your environment or in a `.env` file.

   ```sh
   just setup "/path/to/HX Edit v3.82 Installer.exe"
   ```

3. Plug in the Helix, then run:

   ```sh
   just          # builds + installs the DLL, starts helixd, launches HX Edit
   ```

Other recipes (`just --list`):

| Recipe          | What it does                                  |
|-----------------|-----------------------------------------------|
| `build`         | Build `build/Interposer.dll`                  |
| `install`       | Build and copy the DLL into the prefix        |
| `daemon [-v]`   | Run helixd in the foreground (`-v` logs packets) |
| `hxedit`        | Launch HX Edit only (daemon must be running)  |
| `log`           | Tail the DLL's log                            |
| `lint`          | pylint the daemon, compile the DLL with `-Werror` |

## Configuration

| Variable             | Default | Purpose |
|----------------------|---------|---------|
| `HXEDIT_WINEPREFIX`  | `~/.local/share/hxedit-wine` | Wine prefix |
| `HELIX_BRIDGE_PORT`  | `47800` | TCP port between the DLL and the daemon |
| `HELIX_BRIDGE_PID`   | `0x4241` | USB product ID reported to HX Edit |
| `HELIX_BRIDGE_LOG`   | `C:\ProgramData\Line 6\Interposer\bridge.log` | DLL log path |
| `HELIX_BRIDGE_HEX`   | unset | If set, hex-dump every transfer to the DLL log |

## Caveats

- **HX Edit identifies the device as a "Helix Floor".** Its Interposer code path
  only accepts the Floor, Rack and Control product IDs, so the DLL reports
  `0x4241`. Presets, blocks and the signal chain work. Global settings,
  footswitch and I/O pages that differ between models may be wrong. **Back up
  your presets before editing.**
- **Don't run firmware updates through this.** Use Line 6 Updater on real Windows.
- Only one program can own the USB interface at a time. Stop `helixd` before
  using another editor.
- Only the Helix LT has been tested. Other Helix and HX models may need a
  different `HELIX_BRIDGE_PID` or may not work.
- Bulk OUT transfers are padded to a multiple of 4 bytes, like the real driver
  does. Without the padding the device stalls.

## Credits

- [kempline/helix_usb](https://github.com/kempline/helix_usb) for documenting
  the Helix USB protocol.
- If you'd rather have a native editor, see [Fretwire](https://fretwire.org).

## Disclaimer

Not affiliated with or endorsed by Line 6 or Yamaha Guitar Group. HX Edit and
Helix are trademarks of their respective owners. This repository contains no
Line 6 code or binaries. Use at your own risk.
