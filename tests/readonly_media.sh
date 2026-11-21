#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Exercise the installed carrier and the exact ELF bytes extracted from it.
set -euo pipefail
check=$(realpath "${1:?V2 test executable}")
module=$(realpath "${2:?BTRFS.so}")
firmware=$(realpath "${3:?freestanding build}")
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cd "$work"
mkdir -p source/boot source/many
if [[ -n ${BTRFS_TEST_KERNEL:-} ]]; then
    cp -- "$BTRFS_TEST_KERNEL" source/boot/vmlinuz
else
    dd if=/dev/urandom of=source/boot/vmlinuz bs=1M count=2 status=none
fi
if [[ -n ${BTRFS_TEST_INITRAMFS:-} ]]; then
    cp -- "$BTRFS_TEST_INITRAMFS" source/boot/initramfs
else
    dd if=/dev/urandom of=source/boot/initramfs bs=128K count=1 status=none
fi
printf 'inline Btrfs data\n' > source/boot/inline
truncate -s 262144 source/boot/sparse
printf 'written tail' | dd of=source/boot/sparse bs=1 seek=210000 conv=notrunc status=none
# Force an internal FS-tree node and verify bounds on more than one leaf.
for ((i=0;i<512;i++)); do printf '%d\n' "$i" > "source/many/file-$i"; done
truncate -s 192M clean.img
mkfs.btrfs -q -f -s 4096 -n 16384 -m dup -d single --csum crc32c --rootdir source clean.img
btrfs check --readonly clean.img >/dev/null
objcopy --dump-section .infiltrator_boot=embedded.so "$module" carrier-copy.so
cmp embedded.so "$firmware"
if [[ -n $(nm -D --undefined-only embedded.so) ]] || readelf -dW embedded.so | grep -F '(NEEDED)' >/dev/null; then
    echo 'Freestanding Btrfs ELF imports a hosted symbol or library' >&2; exit 1
fi
readelf -rW embedded.so > relocations
if grep 'R_X86_64_' relocations | grep -v 'R_X86_64_RELATIVE' >/dev/null; then exit 1; fi
cp --sparse=always --reflink=auto clean.img bad-tree.img
cp --sparse=always --reflink=auto clean.img bad-data.img
# Locate the current tree by its on-disk FSID and logical address, including
# both DUP replicas. Locate file data independently using whole-sector bytes.
python3 - <<'PY'
import mmap
from pathlib import Path
with open('bad-tree.img','r+b') as f, mmap.mmap(f.fileno(),0) as disk:
    sb = disk[65536:65536+4096]
    needle = sb[32:48] + sb[80:88]
    hits = 0
    i = disk.find(needle)
    while i != -1:
        if i % 4096 == 32:
            disk[i-32] ^= 1
            hits += 1
        i = disk.find(needle,i+len(needle))
    assert hits == 2, hits
with open('bad-data.img','r+b') as f, mmap.mmap(f.fileno(),0) as disk:
    with open('source/boot/vmlinuz','rb') as kernel:
        needle = kernel.read(4096)
    assert len(needle) == 4096
    i = disk.find(needle)
    assert i >= 0 and i % 4096 == 0 and disk.find(needle,i+1) == -1
    disk[i+4079] ^= 1  # A one-byte read at offset zero must catch this.
PY
sha256sum clean.img bad-tree.img bad-data.img > images.sha256
for elf in "$module" "$PWD/embedded.so"; do
    "$check" "$elf" clean.img source 512 clean
    "$check" "$elf" clean.img source 4096 clean
    "$check" "$elf" bad-tree.img source 4096 bad-tree
    "$check" "$elf" bad-data.img source 512 bad-data
done
sha256sum -c images.sha256
