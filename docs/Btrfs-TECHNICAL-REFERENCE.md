# Btrfs: technical reference

## History and architecture

Btrfs began at Oracle in 2007 under Chris Mason and entered Linux 2.6.29. It uses checksummed copy-on-write B-trees, logical address spaces mapped to physical devices, shared extents, subvolumes and snapshots. This reference is pinned to Linux v6.12's on-disk definitions. A logical tree block address is not directly a device byte offset, and a file's inode identity is scoped to its subvolume tree. Allocation and ownership depend on the extent/backreference trees rather than directory reachability alone. Single-device SINGLE/DUP profiles are distinct from RAID layouts; unsupported feature and device combinations must not be silently approximated.

The design addressed server storage complexity through integrated volume management rather than a conventional journal alone.

## Superblock replicas and fixed fields

Superblocks are 4096 bytes, normally at physical device offsets 64 KiB, 64 MiB and 256 GiB where within the device. Little-endian fields: checksum 0..31, FSID 32..47, self physical bytenr 48, flags 56, magic _BHRfS_M at 64, generation 72, root-tree logical bytenr 80, chunk-root 88, log-root 96, obsolete log transid 104, total bytes 112, used 120, root-directory object ID 128, device count 136. Sector/node/obsolete leaf/stripe size fields at 144/148/152/156, system chunk array bytes 160, chunk-root generation 164, compatible/RO/incompatible 172/180/188, checksum type 196, tree levels 198/199/200. Validate every copy's checksum, identity, geometry, feature masks and root coherence rather than simply choosing the highest generation.

## Bootstrap device and chunk mappings

The packed device item begins at superblock byte 201 and contains device ID at relative 0, total bytes 8, used 16, I/O alignment 24, width 28, sector size 32, type 36, generation 44, start offset 52, group 60, speed/bandwidth 64/65, UUID 66 and FSID 82. Its size is 98 bytes. The system chunk array starts at superblock byte 811 in the pinned layout, and consists of 17-byte disk keys followed by variable-size chunk values. Bound its declared byte count to the array capacity. These system mappings permit reading the chunk tree before the full mapping is known. Cross-check the device table, stripe UUIDs and chunk root; a missing member must not be treated as an all-zero device.

## Chunk items and worked translation

A chunk item value begins with logical length 0, owner 8, stripe length 16, profile/type flags 24, I/O align/width/sector size 32/36/40, stripe count 44 and sub-stripe count 46. Stripe descriptors begin at 48; each is 32 bytes: device ID 0, device byte offset 8, UUID 16. The chunk key's offset is the logical chunk start. For SINGLE, logical p maps to stripe physical offset+(p-chunk_start). For DUP, multiple offsets on the same device hold alternate copies. Example: chunk logical start 1 GiB, physical start 2 GiB, logical byte 1 GiB+8192 maps to 2147491840 on the declared device. RAID0/10 stripe arithmetic and RAID5/6 parity placement require native profile-specific mapping; do not use the SINGLE formula. Split I/O across mapping/stripe boundaries.

## Tree block headers and key layout

Tree blocks have 101-byte headers: checksum 0..31, FSID 32, logical bytenr 48, flags 56, chunk-tree UUID 64, generation 80, owner tree ID 88, item count 96 and level 100. Level zero is a leaf. A packed key has 64-bit object ID at 0, one-byte type at 8 and 64-bit offset at 9 (17 bytes). Compare numerically in object/type/offset order. Internal pointer records are 33 bytes: key, child logical bytenr 17, child generation 25. Leaf item descriptors are 25 bytes: key, data offset 17 and size 21; item offsets are relative to the data area after the 101-byte header. Check nonoverlap of descriptors and payloads, sorted keys, child generation, expected owner and logical location, and metadata checksum. Tree sharing needs feature-aware owner checks.

## Root tree and object identities

The root tree identifies other trees via ROOT_ITEM keys, including filesystem/subvolume trees. Principal IDs include root 1, extent 2, chunk 3, device 4, default filesystem 5 and checksum 7. The root item contains an embedded inode item followed by generation, root-directory ID, tree logical bytenr and other metadata. Root references/backreferences link subvolume names to containing trees. The usual subvolume root directory inode is 256, but resolve actual root metadata and default-subvolume selection. Snapshot roots share underlying metadata and data; separate subvolumes can legitimately reuse inode numbers. A stable handle needs tree/root identity as well as object ID. Never combine roots from unrelated superblock generations to synthesize a namespace.

## Inode items and directories

An inode item has generation 0, transid 8, size 16, allocated-byte accounting 24, block-group hint 32, link count 40, UID/GID 44/48, mode 52, device 56, flags 64 and sequence 72, reserved 80..111, followed by four 12-byte timespecs (seconds then nanoseconds) at 112,124,136,148. Directory items contain target key 0, transid 17, 16-bit data length 25, name length 27, type 29 and variable name/data from 30. DIR_ITEM keys use native filename hash; DIR_INDEX keys provide ordered enumeration. Collisions can pack multiple entries into one item. Match exact bounded names, target key type and root identity. Inode-reference items represent links back to parent directories; subvolume links are root references rather than ordinary inode links.

## File extent item layout

Extent-data keys use the inode object ID, EXTENT_DATA type and logical file byte offset. The value begins generation 0, RAM bytes 8, compression 16, encryption 17, other encoding 18 and extent type 20. Type 0 is inline and its data begins at 21. Regular (1) and preallocated (2) values use disk logical bytenr at 21, disk bytes 29, offset into the extent 37 and logical byte count 45. All are little-endian. In uncompressed regular mapping, file byte position p maps to disk_bytenr+extent_offset+(p-key_offset), then through chunk translation. A zero disk bytenr or absent mapping may represent a hole under admitted rules; preallocated extents read as zero. Compressed extents must be decoded as units, not by direct physical byte arithmetic.

## Checksums, compression and repair

The superblock checksum type selects the native checksum algorithm, including CRC32C, xxhash, SHA-256 or BLAKE2b depending on version. Metadata stores the checksum in its first 32-byte area and checksums the remaining block with native algorithm conventions. The checksum tree holds checksums for logical data sectors, keyed by disk logical byte positions, not file offsets; compressed extents checksum stored representation. NODATASUM data can lack checksums by design. A failed checksum is not permission to accept a different unvalidated copy. For DUP or mirrored profiles, validate alternate copies and their identities before repair. Preserve compression, checksumming and NOCOW semantics; unsupported encodings must refuse reading or mutation rather than exposing raw encoded bytes as file data.

## Extent and backreference ownership

Extent items contain 64-bit reference count at 0, generation 8 and flags 16; additional tree-block and inline reference records depend on the key and extent flags. Data references can encode root ID, inode object ID, logical offset and reference count; shared references can identify parent tree blocks. Skinny metadata uses a separate key layout. Check declared and reconstructed references across all live roots and snapshots before reclamation. Block-group items, free-space tree/cache and device allocation metadata describe different levels of allocation; none is a substitute for the extent tree. A reflink or snapshot can legitimately refer to the same physical extent. A file-level defrag that breaks sharing may increase physical usage and must not silently change snapshot contents.

## Copy-on-write publication and tree log

A transaction writes new payload and changed tree nodes, updates extent ownership and checksums, then publishes consistent new root pointers and generation through the superblock. Previous reachable versions cannot be reused until commit and native reference/pin rules allow. The fsync tree log can contain newer namespace/data metadata than the last full transaction; recovery must apply only the validated log associated with the selected superblock. A read-only forensic scan should preserve the original image and distinguish committed roots from a log-replayed view. A clean-looking root tree does not prove every related allocator tree or device is coherent. Exact delayed-ref processing, tree-log replay, balance and interrupted deletion semantics need dedicated algorithm-level tests.

## Implementation and qualification boundary

Historical revision 20fed85b57f8 admitted single-device SINGLE/DUP mappings, verified uncompressed regular data through the checksum tree including partial-sector reads, and treated sparse/preallocated extents as zero. Hosted FORMAT/QUERY were distinct from the freestanding reader. Active log replay, nested subvolumes, compressed/encrypted extents and other device profiles were outside that documented read path.

Common V2 is the independent provider interface: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate real operation masks and media profiles. Use bounded host I/O, exclusive unmounted ownership and durable flush for mutation; distinguish unsupported, corrupt, I/O and stale-handle results. Test Media and Defragmenter use the same engine. Hosted, freestanding, BOOT, kernel, recovery and package qualification are independent. Current capability and release status belongs to the repository README and exact-source qualification records; historical evidence is not a current status claim.

Historical source or test reference: https://github.com/Infiltrator-Projects/Filesystem-Btrfs/actions/runs/45520083239 .

## Formatting, relocation and acceptance

A native formatter must establish device identity, system and ordinary chunks, root/chunk/extent/device and filesystem trees, allocation metadata, checksums, and coherent superblock copies. Defrag and balance are different operations: rewriting a file's logical extent layout is not equivalent to moving a physical chunk between devices. Require reserve space, copy and verify data, publish coherent pointers/checksums/backrefs and retire old storage under native transaction rules. Sources: https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/btrfs_tree.h and https://github.com/torvalds/linux/tree/v6.12/fs/btrfs . Compare independent btrfs-progs images across node sizes, SINGLE/DUP, multiple devices, inline/prealloc/compressed files, snapshots, reflinks, corrupted checksums, partial transaction publication and tree-log recovery. Exact RAID parity mapping, checksum coverage and feature-specific writer semantics remain unqualified by this document.
