set dotenv-load

# Wine prefix holding HX Edit. Override in .env or the environment.
prefix := env("HXEDIT_WINEPREFIX", home_directory() / ".local/share/hxedit-wine")
ipdir := prefix / "drive_c/ProgramData/Line 6/Interposer"
appdir := prefix / "drive_c/Program Files (x86)/Line6"
hxedit := appdir / "HX Edit/HX Edit.exe"
port := env("HELIX_BRIDGE_PORT", "47800")

default: helix

# Build the fake Interposer.dll (32-bit, matches HX Edit)
build:
    mkdir -p build
    zig cc -target x86-windows-gnu -shared -O2 -o build/Interposer.dll interposer.c interposer.def -lws2_32

# Create the Wine prefix and unpack HX Edit from the official Windows installer
setup installer:
    #!/usr/bin/env bash
    set -euo pipefail
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    WINEPREFIX="{{prefix}}" WINEDEBUG=-all wineboot -i >/dev/null 2>&1
    7z x -y -o"$tmp" "{{installer}}" >/dev/null
    mkdir -p "{{appdir}}"
    rm -rf "{{appdir}}/HX Edit"
    cp -r "$tmp/HX Edit" "{{appdir}}/"
    echo "HX Edit installed into {{prefix}}"

# Install the DLL into the Wine prefix
install: build
    mkdir -p "{{ipdir}}"
    cp build/Interposer.dll "{{ipdir}}/Interposer.dll"

# Run the USB daemon in the foreground (-v for packet logging)
daemon *args:
    uv run --script helixd.py --port {{port}} {{args}}

# Run HX Edit only (expects the daemon to be running)
hxedit:
    WINEPREFIX="{{prefix}}" WINEDEBUG=-all wine "{{hxedit}}"

# Start the daemon, run HX Edit, stop the daemon when HX Edit exits
helix: install
    #!/usr/bin/env bash
    set -euo pipefail
    uv run --script helixd.py --port {{port}} &
    daemon=$!
    trap 'kill $daemon 2>/dev/null; wait $daemon 2>/dev/null || true' EXIT
    for _ in $(seq 50); do
        (exec 3<>/dev/tcp/127.0.0.1/{{port}}) 2>/dev/null && break
        kill -0 $daemon 2>/dev/null || { echo "helixd failed to start" >&2; exit 1; }
        sleep 0.2
    done
    WINEPREFIX="{{prefix}}" WINEDEBUG=-all wine "{{hxedit}}"

# Show the DLL's log
log:
    tail -n 50 "{{ipdir}}/bridge.log"

# Screenshot the HX Edit window via GDI (black if the window is GL-rendered)
shot out="/tmp/hxedit.bmp":
    mkdir -p build
    zig cc -target x86-windows-gnu -O2 -o build/winshot.exe tools/winshot.c -lgdi32 -luser32
    WINEPREFIX="{{prefix}}" WINEDEBUG=-all wine build/winshot.exe "HX Edit" "$(winepath -w "{{out}}")"

# Lint the daemon (pylint) and compile the DLL with warnings as errors
lint:
    uvx --with pyusb pylint helixd.py
    mkdir -p build
    zig cc -target x86-windows-gnu -Wall -Wextra -Werror -c -o build/lint.o interposer.c
