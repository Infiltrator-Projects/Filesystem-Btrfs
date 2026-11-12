# Btrfs migration
Native first-party Btrfs analysis, durable Defrag/Growth/Recover, and the
Test Media formatter and exact payload checker are now compiled in BTRFS.so.
Generic host services/corpus callbacks are provided by Defragmenter through
size- and version-validated interfaces. No external mkfs tool is required.

This driver must pass standalone build/test AND a Defragmenter integration
regression before deleting original embedded source; source import alone
does not establish integration.

## Strict Common V2 Btrfs read-only volume view (1.0.9)
CORE.PROBE, CORE.MOUNT/UNMOUNT and VOLUME.QUERY now expose source-owned,
CRC32C-verified single-device Btrfs superblock geometry over host-bounded
Common V2 read-only I/O. The reported available space is **recorded logical
superblock total minus bytes_used**, not a free-extents/quotas/profile-aware
physical-space scan. The existing native multidevice, snapshot, defrag and
recovery logic is not represented by this single-device V2 view, and V2
DEFRAG MAP/PLAN/EXECUTE/VERIFY/RECOVER remain unadvertised. Wrong checksum,
impossible occupancy, stale session and write-only access are refused.
