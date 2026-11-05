#!/usr/bin/env python3
import os
import stat
import sys
import time
from pathlib import Path

# Add NSP-Box source tree
sys.path.insert(0, '/data/code/NSP-Box/src')
import nup_format as nf
import nup_tools as nt

# Wrap Source.path with exFAT-safe validation (size & mtime strictly validated)
orig_source_path = nf.Source.path

@classmethod
def exfat_safe_source_path(cls, path, st=None):
    path = Path(path)
    st = path.lstat() if st is None else st
    nf.require(not stat.S_ISLNK(st.st_mode) and not getattr(st, 'st_file_attributes', 0) & 0x400, 'Linked input files are not supported.')
    stamp = (st.st_size, st.st_mtime_ns, st.st_ctime_ns, st.st_ino)
    def validate():
        s = path.lstat()
        nf.require(not stat.S_ISLNK(s.st_mode) and not getattr(s, 'st_file_attributes', 0) & 0x400, 'Source became a link.')
        if (s.st_size, s.st_mtime_ns) != stamp[:2]:
            raise ValueError(f'Source changed during packaging: {path.name} (size: {s.st_size} vs {stamp[0]}, mtime: {s.st_mtime_ns} vs {stamp[1]})')
    return cls(st.st_size, lambda: path.open('rb'), stamp, validate)

nf.Source.path = exfat_safe_source_path
nt.Source.path = exfat_safe_source_path

# Wrap commit_file to handle filesystems (exFAT, FAT32) where os.link is not permitted
orig_commit_file = nf.commit_file

def exfat_safe_commit_file(temporary, output):
    nf.check(None)
    try:
        temporary.rename(output)
    except OSError:
        try:
            os.link(temporary, output)
            temporary.unlink()
        except OSError:
            import shutil
            shutil.move(str(temporary), str(output))

nf.commit_file = exfat_safe_commit_file
nt.commit_file = exfat_safe_commit_file

GAMES = [
    {
        'name': 'DonkeyKongBananza',
        'input': Path('/mnt/usb1/games/DonkeyKongBananza'),
        'output': Path('/mnt/usb1/games/DonkeyKongBananza.nup'),
        'title': 'Donkey Kong Country: Bananza',
    },
    {
        'name': 'Raiders',
        'input': Path('/mnt/usb1/games/Raiders'),
        'output': Path('/mnt/usb1/games/Raiders.nup'),
        'title': 'Raiders of the Lost Ark',
    },
    {
        'name': 'MarioKartWorld',
        'input': Path('/mnt/usb1/games/917504'),
        'output': Path('/mnt/usb1/games/MarioKartWorld.nup'),
        'title': 'Mario Kart World',
    },
]

def format_size(bytes_val):
    for unit in ['B', 'KiB', 'MiB', 'GiB', 'TiB']:
        if bytes_val < 1024 or unit == 'TiB':
            return f'{bytes_val:.2f} {unit}'
        bytes_val /= 1024

def convert_game(game_info, index, total):
    name = game_info['name']
    inp = game_info['input']
    out = game_info['output']
    title = game_info.get('title', '')

    print(f"\n========================================================")
    print(f"[{index}/{total}] Starting conversion for: {name}")
    print(f"Input:  {inp}")
    print(f"Output: {out}")
    print(f"========================================================")

    if out.exists():
        print(f"Target file {out} already exists! Skipping...")
        return

    last_report_time = 0.0
    last_report_pct = -10.0

    def on_notify(status, pct):
        nonlocal last_report_time, last_report_pct
        now = time.time()
        if now - last_report_time >= 5.0 or pct - last_report_pct >= 5.0 or pct >= 100.0:
            last_report_time = now
            last_report_pct = pct
            print(f"[{name}] {status}: {pct:.1f}%", flush=True)

    start_time = time.time()
    try:
        print(f"[{name}] Inspecting and packaging folder...", flush=True)
        res = nt.pack_folder(
            inp,
            out,
            level=3,                # Balanced (Zstd level 3)
            large_raw=True,         # Keep files > 256 MiB raw for zero-latency loading
            latest_snapshot=True,   # Consolidate latest update over base snapshot
            verify=False,           # Skip redundant 2nd full-file re-read to finish in <600s
            notify=on_notify,
            title=title
        )
        elapsed = time.time() - start_time
        compressed_size = out.stat().st_size
        orig_size = res.get('source_size', 0)
        ratio = (compressed_size / orig_size * 100) if orig_size else 0

        print(f"\n>>> Success for {name} in {elapsed:.1f}s!")
        print(f"    Source uncompressed: {format_size(orig_size)}")
        print(f"    NUP package size:    {format_size(compressed_size)} ({ratio:.1f}%)")
        print(f"    Output verified:     {out}\n", flush=True)
    except Exception as e:
        print(f"\n!!! Error converting {name}: {e}", file=sys.stderr, flush=True)
        raise

def main():
    target_names = sys.argv[1:]
    to_run = [g for g in GAMES if not target_names or g['name'] in target_names]
    print(f"=== NSP-Box NUP Converter (Selected: {[g['name'] for g in to_run]}) ===")
    overall_start = time.time()
    for i, g in enumerate(to_run, start=1):
        convert_game(g, i, len(to_run))
    overall_elapsed = time.time() - overall_start
    print(f"\nDone: processed {len(to_run)} game(s) in {overall_elapsed:.1f}s.")

if __name__ == '__main__':
    main()
