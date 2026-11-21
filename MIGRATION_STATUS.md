# Btrfs qualification

| Target | Evidence |
| --- | --- |
| Published v1.0.11 | The Debian carrier provides a hosted metadata probe and mount. It has no freestanding namespace payload. |
| Current source | Both profiles share the CRC32C verifier, logical-to-physical chunk mappings and bounded tree reader. Both use the Common V2 descriptor. |
| Real image | `btrfs-carrier-and-firmware-real-media` compares every byte through CORE, tests directory enumeration and distinct session handles, and repeats on 512-byte and 4096-byte logical sectors. |
| Damaged media | The test damages both root-tree replicas on one copy and a different byte in a requested file sector on another. The metadata and data checks must return `IFS_FS_CORRUPT`. Injected host read failure must return `IFS_FS_IO_ERROR`. |
| Image preservation | The test opens images read-only and checks their SHA-256 digests after exercising both profiles. |
| Consumer qualification | Validation and kernel/initramfs reads through the rebuilt Infiltrator BOOT remain pending. New release publication is held. |
