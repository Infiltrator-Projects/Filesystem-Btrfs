# BTRFS Common V2 migration

## Hosted status

- V2 probe: CRC32C-checked superblock, basic geometry and single-device checks
- V2 read-only mount/unmount: **metadata-only session**, not a namespace
- V2 volume query: reports recorded superblock allocation, not a full verified free-space tree
- V2 stat, directory enumeration, path traversal and file contents: **missing**
- Freestanding ELF and real BOOT production validator acceptance: **missing**
- OVMF kernel and initramfs reads: **not tested**
- Supported incompatible feature flags and full tree/checksum validation:
  **still require qualification**

## Production boundary

BTRFS.so now exports only the Common V2 API. The original format scanner,
worker and fixture callback bindings stay in tree for a **test-only** library,
never the installed ELF. The Linux VFS implementation is retained as
historical source, not bundled as a kernel fallback. Build version comes
from the repository's actual VERSION file rather than a stale hardcoded
1.0.0 string. Package workflows qualify only two payload files: the real
V2 shared object and its PDF; no .ko is published. No firmware release
is published from these hosted-only checks.
