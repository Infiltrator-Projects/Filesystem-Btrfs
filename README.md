# Filesystem-Btrfs

`/System/Filesystems/BTRFS.so` exposes `infiltrator_filesystem_api_v2`. Its hosted library and embedded freestanding ELF compile the same read-only Btrfs namespace implementation. The source supports CORE probe, mount, unmount, stat, directory enumeration and file reads. VOLUME query reports the superblock's recorded capacity and allocation.

The reader resolves logical addresses through single-device SINGLE and DUP chunk mappings. It checks tree identity, generation, keys, lengths and CRC32C before using a metadata node. Uncompressed file data is read against the checksum tree, including partial-sector reads. Inline data is protected by its leaf checksum. Sparse and preallocated extents return zero-filled bytes.

The supported namespace uses regular files and directories. Active log replay, nested subvolume traversal, compressed or encrypted extents, unchecked regular data and other device profiles are outside this reader's supported scope. The historical analyser, writer and worker remain available to regression tests.

`ctest` builds disposable media using `mkfs.btrfs` and exercises the carrier and its extracted ELF through Common V2. `BTRFS_TEST_KERNEL` and `BTRFS_TEST_INITRAMFS` select existing payloads for byte comparison. The latest published v1.0.11 Debian module predates this reader. A new BOOT-qualified release requires testing the rebuilt BOOT loader in OVMF.
