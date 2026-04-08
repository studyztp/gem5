#!/usr/bin/env python3
"""
Pack/unpack large result files for git storage.

Large simout.txt files exceed git pre-commit size limits (500KB).
This script splits them into chunks for committing and reassembles
them after checkout.

Usage:
    python3 pack_results.py pack       # split large files into chunks
    python3 pack_results.py unpack     # reassemble chunks into originals
    python3 pack_results.py status     # show which files need packing

Chunk format:
    somefile.txt  ->  somefile.txt.part000, somefile.txt.part001, ...
    A manifest file somefile.txt.manifest is created listing all parts.
    The original file is deleted after packing.
"""

import argparse
import os
import sys

CHUNK_SIZE = 200 * 1024  # 200KB per chunk (well under 250KB/500KB limits)
RESULT_DIRS = ["gem5-result-art", "gem5-result-noart"]


def get_base_dir():
    return os.path.dirname(os.path.abspath(__file__))


def find_large_files(base, threshold=CHUNK_SIZE):
    """Find files larger than threshold in result directories."""
    large = []
    for rdir in RESULT_DIRS:
        dirpath = os.path.join(base, rdir)
        if not os.path.isdir(dirpath):
            continue
        for root, _, files in os.walk(dirpath):
            for fname in files:
                if fname.endswith(".manifest") or fname.endswith(".part"):
                    continue
                # Skip already-chunked part files
                if ".part" in fname:
                    continue
                fpath = os.path.join(root, fname)
                size = os.path.getsize(fpath)
                if size > threshold:
                    large.append((fpath, size))
    return large


def find_manifests(base):
    """Find all manifest files in result directories."""
    manifests = []
    for rdir in RESULT_DIRS:
        dirpath = os.path.join(base, rdir)
        if not os.path.isdir(dirpath):
            continue
        for root, _, files in os.walk(dirpath):
            for fname in files:
                if fname.endswith(".manifest"):
                    manifests.append(os.path.join(root, fname))
    return manifests


def pack_file(filepath):
    """Split a file into chunks and create a manifest."""
    size = os.path.getsize(filepath)
    parts = []
    part_num = 0

    with open(filepath, "rb") as f:
        while True:
            chunk = f.read(CHUNK_SIZE)
            if not chunk:
                break
            part_name = f"{filepath}.part{part_num:03d}"
            with open(part_name, "wb") as pf:
                pf.write(chunk)
            parts.append(os.path.basename(part_name))
            part_num += 1

    # Write manifest
    manifest_path = f"{filepath}.manifest"
    with open(manifest_path, "w") as mf:
        mf.write(f"# Packed by pack_results.py\n")
        mf.write(f"# Original: {os.path.basename(filepath)}\n")
        mf.write(f"# Size: {size}\n")
        mf.write(f"# Parts: {len(parts)}\n")
        for part in parts:
            mf.write(f"{part}\n")

    # Remove original
    os.remove(filepath)

    return len(parts)


def unpack_manifest(manifest_path):
    """Reassemble a file from its manifest and chunks."""
    dirpath = os.path.dirname(manifest_path)
    original_name = None
    parts = []

    with open(manifest_path) as mf:
        for line in mf:
            line = line.strip()
            if line.startswith("# Original: "):
                original_name = line.split("# Original: ", 1)[1]
            elif not line.startswith("#") and line:
                parts.append(line)

    if not original_name or not parts:
        print(f"  WARNING: invalid manifest {manifest_path}", file=sys.stderr)
        return False

    original_path = os.path.join(dirpath, original_name)

    # Check all parts exist
    for part in parts:
        part_path = os.path.join(dirpath, part)
        if not os.path.isfile(part_path):
            print(f"  WARNING: missing part {part_path}", file=sys.stderr)
            return False

    # Reassemble
    with open(original_path, "wb") as out:
        for part in parts:
            part_path = os.path.join(dirpath, part)
            with open(part_path, "rb") as pf:
                out.write(pf.read())

    # Clean up parts and manifest
    for part in parts:
        os.remove(os.path.join(dirpath, part))
    os.remove(manifest_path)

    return True


def cmd_pack():
    base = get_base_dir()
    large_files = find_large_files(base)

    if not large_files:
        print("No large files found. Nothing to pack.")
        return

    print(f"Packing {len(large_files)} large files...")
    total_parts = 0
    for fpath, size in sorted(large_files):
        rel = os.path.relpath(fpath, base)
        n = pack_file(fpath)
        total_parts += n
        print(f"  {rel}: {size / 1024:.0f}KB -> {n} parts")

    print(f"Done. {len(large_files)} files -> {total_parts} parts.")


def cmd_unpack():
    base = get_base_dir()
    manifests = find_manifests(base)

    if not manifests:
        print("No manifest files found. Nothing to unpack.")
        return

    print(f"Unpacking {len(manifests)} files...")
    ok = 0
    for mpath in sorted(manifests):
        rel = os.path.relpath(mpath, base)
        if unpack_manifest(mpath):
            original = rel.replace(".manifest", "")
            size = os.path.getsize(os.path.join(base, original))
            print(f"  {original}: reassembled ({size / 1024:.0f}KB)")
            ok += 1
        else:
            print(f"  {rel}: FAILED")

    print(f"Done. {ok}/{len(manifests)} files unpacked.")


def cmd_status():
    base = get_base_dir()
    large_files = find_large_files(base)
    manifests = find_manifests(base)

    print(f"Large files (>{CHUNK_SIZE / 1024:.0f}KB): {len(large_files)}")
    for fpath, size in sorted(large_files)[:10]:
        print(f"  {os.path.relpath(fpath, base)}: {size / 1024:.0f}KB")
    if len(large_files) > 10:
        print(f"  ... and {len(large_files) - 10} more")

    print(f"\nPacked manifests: {len(manifests)}")
    for mpath in sorted(manifests)[:10]:
        print(f"  {os.path.relpath(mpath, base)}")
    if len(manifests) > 10:
        print(f"  ... and {len(manifests) - 10} more")

    if large_files:
        print(
            f"\nRun 'python3 {os.path.basename(__file__)} pack' to split them."
        )
    if manifests:
        print(
            f"\nRun 'python3 {os.path.basename(__file__)} unpack' to reassemble."
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Pack/unpack large result files for git storage"
    )
    parser.add_argument(
        "command",
        choices=["pack", "unpack", "status"],
        help="pack: split large files; unpack: reassemble; status: show state",
    )
    args = parser.parse_args()

    if args.command == "pack":
        cmd_pack()
    elif args.command == "unpack":
        cmd_unpack()
    elif args.command == "status":
        cmd_status()
