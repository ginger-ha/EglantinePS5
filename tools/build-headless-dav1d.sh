#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# dav1d (tools/deps.json) for the PS5: FFmpeg's AV1 decoder (libdav1d). Switch 2 games play AV1
# movies (nx2_video_decoder), which FFmpeg has no software decoder of its own for. Static, with
# its x86-64 assembly; installed into the FFmpeg prefix, where FFmpeg's configure and the app's
# link (headless/CMakeLists.txt) find it.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
scratch=$(cat "$root/.local/headless-cache")
[[ "$(cat "$scratch/owner")" == "$root" ]]
export PS5_PAYLOAD_SDK="$scratch/sdk"
source="$root/.deps/dav1d-1.5.1"
test -f "$source/meson.build"
build="$scratch/dav1d-native"
prefix="$scratch/ffmpeg-native/install"
mkdir -p "$build"
cat >"$build/ps5-cross.ini" <<EOF
[binaries]
c = ['bash', '$root/tools/ffmpeg-ps5-cc.sh']
ar = 'llvm-ar-18'
strip = 'llvm-strip-18'
nasm = 'nasm'

[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
cd "$build"  # meson probes the build machine's compiler in the current folder
rm -rf "$build/meson"
meson setup "$build/meson" "$source" --cross-file "$build/ps5-cross.ini" \
    --prefix="$prefix" --libdir=lib --buildtype=release --default-library=static \
    -Denable_tools=false -Denable_examples=false -Denable_tests=false -Denable_docs=false \
    -Dlogging=false
ninja -C "$build/meson" -j"${EDEN_BUILD_JOBS:-$(nproc)}"
ninja -C "$build/meson" install
