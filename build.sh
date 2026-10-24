#!/usr/bin/env bash
# Builds Mumoria. Run it on Linux or in WSL, from anywhere.
#
#   ./build.sh            the Linux programs and the Windows client
#   ./build.sh linux      the server, the client and single-player umoria
#                         (CMake, into build/umoria/)
#   ./build.sh windows    the client for Windows, mumoria-client.exe
#                         (zig cc, into build/windows/)
#   ./build.sh dist       both, then packages in dist/:
#                           mumoria-linux-x86_64.tar.gz  (server, client, sounds, data)
#                           mumoria-client-windows-x86_64.zip  (the .exe and sounds)
#   ./build.sh get-zig    downloads Zig into ~/local/zig (no root needed)
#
# The Windows client is cross-compiled with Zig: $ZIG, else zig on the PATH,
# else ~/local/zig/zig. The server is Linux only (Windows: run it in WSL).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}" # BUILD_DIR, DIST_DIR: elsewhere than in the source
DIST="${DIST_DIR:-$ROOT/dist}"
ZIG_VERSION="0.17.0"

say() { printf '\033[1m== %s\033[0m\n' "$*"; }
fail() { printf 'build.sh: %s\n' "$*" >&2; exit 1; }

find_zig() {
    if [[ -n "${ZIG:-}" ]]; then echo "$ZIG"; return; fi
    if command -v zig >/dev/null 2>&1; then command -v zig; return; fi
    if [[ -x "$HOME/local/zig/zig" ]]; then echo "$HOME/local/zig/zig"; return; fi
    return 1
}

get_zig() {
    if find_zig >/dev/null; then
        say "zig is already here: $(find_zig) ($("$(find_zig)" version))"
        return
    fi
    say "downloading Zig $ZIG_VERSION into ~/local/zig"
    local arch tarball sum
    arch="$(uname -m)"
    tarball="zig-$arch-linux-$ZIG_VERSION.tar.xz"
    mkdir -p "$HOME/local"
    curl -fL -o "/tmp/$tarball" "https://ziglang.org/download/$ZIG_VERSION/$tarball"
    sum="$(curl -fsSL https://ziglang.org/download/index.json |
        python3 -c "import json,sys; print(json.load(sys.stdin)['$ZIG_VERSION']['$arch-linux']['shasum'])")"
    echo "$sum  /tmp/$tarball" | sha256sum -c - >/dev/null || fail "the Zig download's checksum is wrong"
    rm -rf "$HOME/local/zig" "$HOME/local/zig-$arch-linux-$ZIG_VERSION"
    tar -xf "/tmp/$tarball" -C "$HOME/local"
    mv "$HOME/local/zig-$arch-linux-$ZIG_VERSION" "$HOME/local/zig"
    rm -f "/tmp/$tarball"
    say "zig $("$HOME/local/zig/zig" version) is in ~/local/zig"
}

build_linux() {
    say "Linux: CMake into $BUILD"
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$BUILD" -j"$(nproc)"
    say "Linux: done"
    ls -1 "$BUILD/umoria" | grep -E '^(mumoria-server|mumoria-client|umoria)$' | sed "s#^#  $BUILD/umoria/#"
}

build_windows() {
    local zig
    zig="$(find_zig)" || fail "no zig: run ./build.sh get-zig (or set ZIG=/path/to/zig)"
    local out="$BUILD/windows"
    say "Windows: the client, with $zig ($("$zig" version))"
    mkdir -p "$out/sounds"
    "$zig" cc -target x86_64-windows-gnu -std=c11 -O2 -g -Wall -Wextra \
        "$ROOT/client/mumoria_client.c" "$ROOT/client/client_win.c" \
        -lws2_32 -lwinmm -o "$out/mumoria-client.exe"
    cp "$ROOT"/client/sounds/*.wav "$out/sounds/"
    say "Windows: done"
    echo "  $out/mumoria-client.exe (and sounds/; the .pdb maps a crash's offset to the source)"
}

package() {
    say "packaging into $DIST"
    rm -rf "$DIST" && mkdir -p "$DIST"

    local lin="$DIST/mumoria-linux-x86_64"
    mkdir -p "$lin/sounds" "$lin/data" "$lin/scripts"
    cp "$BUILD/umoria/mumoria-server" "$BUILD/umoria/mumoria-client" "$lin/"
    [[ -x "$BUILD/umoria/umoria" ]] && cp "$BUILD/umoria/umoria" "$lin/"
    cp "$BUILD"/umoria/data/* "$lin/data/"
    cp "$BUILD/umoria/scores.dat" "$lin/" 2>/dev/null || true
    cp -r "$ROOT/scripts/." "$lin/scripts/"
    cp "$ROOT"/client/sounds/*.wav "$lin/sounds/"
    cp "$ROOT/mumoria.conf.example" "$ROOT/README.md" "$ROOT/LICENSE" "$lin/"
    cp "$ROOT/client/sounds/CREDITS.md" "$lin/sounds/"
    tar -czf "$DIST/mumoria-linux-x86_64.tar.gz" -C "$DIST" "mumoria-linux-x86_64"

    local win="$DIST/mumoria-client-windows-x86_64"
    mkdir -p "$win/sounds"
    cp "$BUILD/windows/mumoria-client.exe" "$win/"
    cp "$ROOT"/client/sounds/*.wav "$ROOT/client/sounds/CREDITS.md" "$win/sounds/"
    cp "$ROOT/LICENSE" "$win/LICENSE.txt"
    cat >"$win/README.txt" <<'EOF'
Mumoria client for Windows

  mumoria-client.exe [-q] [host] [port]

Double-click it to play on this computer's server (localhost, port 4000), or
run it from a terminal with the server's address:

  mumoria-client.exe yourserver.example.com 4000

Use Windows Terminal (or Windows 10 1809 and later) so the colours show.
-q plays without sounds. Ctrl-] quits.

The sounds are in sounds\; replace any .wav to change it (see
sounds\CREDITS.md). If something goes wrong, mumoria-client.log beside the
program says what.
EOF
    sed -i 's/$/\r/' "$win/README.txt"
    (cd "$DIST" && python3 -c "
import os, sys, zipfile
root = 'mumoria-client-windows-x86_64'
with zipfile.ZipFile(root + '.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for folder, _, files in os.walk(root):
        for name in files:
            z.write(os.path.join(folder, name))
")
    say "packages:"
    ls -lh "$DIST"/*.tar.gz "$DIST"/*.zip | awk '{print "  " $5 "  " $NF}'
}

case "${1:-all}" in
    linux) build_linux ;;
    windows) build_windows ;;
    all) build_linux; build_windows ;;
    dist) build_linux; build_windows; package ;;
    get-zig) get_zig ;;
    -h | --help | help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//' ;;
    *) fail "unknown: $1 (linux, windows, all, dist, get-zig)" ;;
esac
