# Filesystem-Btrfs

## 1. Purpose and format scope

Btrfs, with native copy-on-write tree structures. This repository owns the format-specific implementation of `BTRFS.so` for InfiltratorOS.

## 2. On-disk architecture and invariants

Superblocks, chunk mappings and B-trees describe logical addresses and allocation; the reader validates tree identity, generation, key ordering, lengths and CRC32C, rejecting impossible root and log tree levels. Single-device SINGLE and DUP mappings are supported by the documented reader. Inline data is protected by leaf checksums; regular uncompressed data is verified through the checksum tree, including partial-sector reads. Sparse and preallocated extents read as zeroes.

## 3. Common V2 implementation and capabilities

Hosted and freestanding namespace reader exposes probe, mount, stat, enumeration and read. The hosted VOLUME descriptor also declares FORMAT and QUERY; check the actual masks and qualification for each profile. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

## 4. Integrity, mutation, allocation and recovery

Active log replay, nested subvolume traversal, compressed or encrypted extents, unchecked regular data and other device profiles are outside the documented read path. Native formatting does not imply arbitrary writable subvolume or defragmentation support. For destructive operations, require the applicable exclusive writable host, durability contract and format-specific validation. Never silently substitute a kernel or FUSE driver or treat a rejected or unavailable operation as success. Preserve original disk-format semantics; relocation and recovery are separate from ordinary file I/O.

## 5. Build, installation and release

The target installation is `/System/Filesystems/BTRFS.so` and `/System/Resources/Documentation/Filesystem-Btrfs.pdf`. The project release policy is a portable amd64 DEB and a CPU-native source-first RUN, with GitHub's automatic source archive; do not treat this policy statement as verification of the current installer payload. The release workflow is the authoritative executable build and package specification. Historical adapters and test artifacts are not additional production filesystem authorities.

## 6. Qualification and limitations

[Recorded qualification workflow](https://github.com/Infiltrator-Projects/Filesystem-Btrfs/actions/runs/45520083239) · [Current workflows](https://github.com/Infiltrator-Projects/Filesystem-Btrfs/actions?query=branch%3Amain). A successful workflow establishes the tested scope only, not complete original-format parity, physical-media crash safety, or production BOOT/kernel integration. Use disposable images for destructive qualification.

## 7. Technical references and provenance

- [Provider implementation](module/btrfs_v2_probe.c)
- [Native design or source provenance](native/filesystems/btrfs/DESIGN.md)
- [Migration and source history](MIGRATION_STATUS.md)
- [Original-format reference source](docs/Btrfs.pdfstreams) and [PDF](docs/Btrfs.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

The original-format reference and historical migration notes describe different evidence from the current production provider. When descriptions disagree, the current exported descriptor, implementation, executable tests and exact-revision release evidence take precedence.

The [engineering reference](docs/Btrfs-TECHNICAL-REFERENCE.md) adds field-level original-format guidance. The earlier manual is retained as a historical appendix in the PDF.
