/* SPDX-License-Identifier: GPL-3.0-or-later
 * Common V2 Btrfs probe, using the source-owned CRC32C validator. All
 * device reads are bounded host callbacks, not path or fd operations.
 * Legacy native analysis and recovery remain available during migration.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <infiltratr/filesystem_ro_sessions_v2.h>
/* Strict Common V2 identity is retained even with native LTO and GC. */
__attribute__((used, retain, section(".ifsmeta"))) static const char ifs_btrfs_v2_identity[] =
    "IFS-MODULE:btrfs:common2";

#include "btrfs_reader.h"
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }
/* Restrict superblock recognition to feature bits understood by this
 * single-device metadata probe. Mount applies the narrower namespace policy.
 * Incompat bits 12+ introduce zoned/tree/stripe/quota layouts which this
 * reader cannot safely interpret.  RO-compat allows free-space-tree,
 * its valid marker, and verity, but not unrecognized tree layouts. */
#define IFS_BTRFS_V2_COMPAT_RO_KNOWN UINT64_C(0x7)
#define IFS_BTRFS_V2_INCOMPAT_KNOWN UINT64_C(0xfff)
static int inspect(const IfsFsHostRuntimeV2 *h, uint32_t *variant, IfsFsVolumeInfoV2 *volume)
{
    const uint64_t where = UINT64_C(65536);
    uint8_t *raw;
    int rc = IFS_FS_CORRUPT;
    if (!variant)
        return IFS_FS_INVALID;
    *variant = 0u;
    if (!ifs_fs_host_v2_valid(h))
        return IFS_FS_INVALID;
    if (where > h->volume_bytes || 4096u > h->volume_bytes - where)
        return IFS_FS_NOT_FOUND;
    raw = h->allocate(h->context, 4096u);
    if (!raw)
        return IFS_FS_NO_MEMORY;
    rc = ifs_fs_host_read_bytes_v2(h, where, raw, 4096u);
    if (rc != IFS_FS_OK)
        goto done;
    rc = IFS_FS_CORRUPT;
    if (memcmp(raw + 64u, "_BHRfS_M", 8u)) {
        rc = IFS_FS_NOT_FOUND;
        goto done;
    }
    const uint64_t total = le64(raw + 112u), used = le64(raw + 120u);
    const uint64_t root = le64(raw + 80u), chunk_root = le64(raw + 88u);
    const uint64_t log_root = le64(raw + 96u);
    const uint32_t sectorsize = le32(raw + 144u), nodesize = le32(raw + 148u);
    uint8_t fsid_nonzero = 0u;
    for (unsigned i = 0u; i < 16u; ++i)
        fsid_nonzero |= raw[32u + i];
    if (le16(raw + 196u) != 0u || le64(raw + 136u) > 1u) {
        rc = IFS_FS_UNSUPPORTED;
        goto done;
    }
    if (!btrfs_checksum_valid(raw, 4096u) || le64(raw + 48u) != where || !fsid_nonzero || !total ||
        total > h->volume_bytes || used > total || le64(raw + 136u) != 1u || !le64(raw + 72u) ||
        !root || !chunk_root || sectorsize < 4096u || sectorsize > 65536u ||
        (sectorsize & (sectorsize - 1u)) || nodesize < sectorsize || nodesize > 65536u ||
        (nodesize & (nodesize - 1u)) || (root % sectorsize) || (chunk_root % sectorsize) ||
        (log_root && (log_root % sectorsize)) || (total % sectorsize) || (used % sectorsize) ||
        raw[198u] > 8u || raw[199u] > 8u)
        goto done;
    if ((le64(raw + 180u) & ~IFS_BTRFS_V2_COMPAT_RO_KNOWN) ||
        (le64(raw + 188u) & ~IFS_BTRFS_V2_INCOMPAT_KNOWN)) {
        rc = IFS_FS_UNSUPPORTED;
        goto done;
    }
    *variant = 1u;
    if (volume) {
        /* Recorded logical bytes_used: a single-device filesystem only.
         * This is not a verified unallocated-data-extent scan. */
        *volume = (IfsFsVolumeInfoV2){.structure_bytes = sizeof(*volume),
                                      .variant = 1u,
                                      .max_name_bytes = 255u,
                                      .block_bytes = sectorsize,
                                      .total_bytes = total,
                                      .free_bytes = total - used};
    }
    rc = IFS_FS_OK;
done:
    h->release(h->context, raw);
    return rc;
}
static int probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant)
{
    return inspect(h, variant, NULL);
}

/* Each handle retains its immutable reader until the last in-flight operation
 * completes. A concurrent close returns BUSY rather than freeing live I/O. */
typedef struct {
    uintptr_t token;
    unsigned active;
    BtrfsReader *reader;
    IfsFsVolumeInfoV2 info;
} Mount;
static Mount mounts[IFS_FS_RO_MAX_SESSIONS_V2];
static atomic_flag registry_lock = ATOMIC_FLAG_INIT;
static uintptr_t next_token;
static void lock_mounts(void)
{
    while (atomic_flag_test_and_set_explicit(&registry_lock, memory_order_acquire)) {
    }
}
static void unlock_mounts(void)
{
    atomic_flag_clear_explicit(&registry_lock, memory_order_release);
}
static Mount *find_mount(void *handle)
{
    for (unsigned i = 0; i < IFS_FS_RO_MAX_SESSIONS_V2; ++i)
        if (handle && mounts[i].token == (uintptr_t)handle)
            return &mounts[i];
    return NULL;
}
static int mount_volume(const IfsFsHostRuntimeV2 *h, void **handle, IfsFsVolumeInfoV2 *info)
{
    if (!handle || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *handle = NULL;
    if (!ifs_fs_host_v2_valid(h))
        return IFS_FS_INVALID;
    if (!(h->capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_ACCESS;
    IfsFsVolumeInfoV2 measured;
    uint32_t variant;
    int rc = inspect(h, &variant, &measured);
    if (rc != IFS_FS_OK)
        return rc;
    BtrfsReader *reader = NULL;
    rc = btrfs_reader_open(h, &reader);
    if (rc != IFS_FS_OK)
        return rc;
    lock_mounts();
    for (unsigned i = 0; i < IFS_FS_RO_MAX_SESSIONS_V2 && next_token != UINTPTR_MAX; ++i) {
        if (mounts[i].reader)
            continue;
        /* Keep the monotonically issued token private to this module.
         * A token from another filesystem should not normally identify
         * the same session index in this independent provider. */
        uintptr_t token=++next_token ^ (uintptr_t)(void *)mounts;
        if (!token) {
            if (next_token==UINTPTR_MAX) break;
            token=++next_token ^ (uintptr_t)(void *)mounts;
        }
        mounts[i] = (Mount){token, 0, reader, measured};
        *handle = (void *)mounts[i].token;
        *info = measured;
        break;
    }
    unlock_mounts();
    if (*handle)
        return IFS_FS_OK;
    btrfs_reader_close(reader);
    return IFS_FS_BUSY;
}
static int unmount_volume(void *handle)
{
    lock_mounts();
    Mount *m = find_mount(handle);
    if (!m || m->active) {
        int rc = m ? IFS_FS_BUSY : IFS_FS_INVALID;
        unlock_mounts();
        return rc;
    }
    BtrfsReader *reader = m->reader;
    *m = (Mount){0};
    unlock_mounts();
    btrfs_reader_close(reader);
    return IFS_FS_OK;
}
static int query_volume(void *handle, IfsFsVolumeInfoV2 *info)
{
    if (!info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    lock_mounts();
    Mount *m = find_mount(handle);
    if (m)
        *info = m->info;
    unlock_mounts();
    return m ? IFS_FS_OK : IFS_FS_INVALID;
}
static Mount *acquire(void *handle)
{
    lock_mounts();
    Mount *m = find_mount(handle);
    if (m && m->active != UINT_MAX)
        ++m->active;
    else
        m = NULL;
    unlock_mounts();
    return m;
}
static int finished(Mount *m, int rc)
{
    lock_mounts();
    --m->active;
    unlock_mounts();
    return rc;
}
static int stat_file(void *handle, const char *path, IfsFsEntryV2 *entry)
{
    Mount *m = acquire(handle);
    return m ? finished(m, btrfs_reader_stat(m->reader, path, entry)) : IFS_FS_INVALID;
}
static int list_directory(void *handle, const char *path, uint32_t index, IfsFsEntryV2 *entry)
{
    Mount *m = acquire(handle);
    return m ? finished(m, btrfs_reader_list(m->reader, path, index, entry)) : IFS_FS_INVALID;
}
static int read_file(void *handle, const char *path, uint64_t offset, void *out, uint32_t bytes)
{
    Mount *m = acquire(handle);
    return m ? finished(m, btrfs_reader_read(m->reader, path, offset, out, bytes)) : IFS_FS_INVALID;
}
static const IfsFsVolumeOpsV2 volume_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2), IFS_FS_GROUP_VOLUME, 0u,
               IFS_FS_VOLUME_QUERY},
    .query = query_volume};
static const IfsFsCoreOpsV2 core = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2), IFS_FS_GROUP_CORE, 0u,
               IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT | IFS_FS_CORE_STAT | IFS_FS_CORE_LIST |
                   IFS_FS_CORE_READ},
    .probe = probe,
    .mount = mount_volume,
    .unmount = unmount_volume,
    .stat = stat_file,
    .directory_entry = list_directory,
    .read = read_file};
static const IfsFsGroupHeaderV2 *query(uint32_t group, uint32_t lo, uint32_t hi)
{
    if (lo > IFS_FS_GROUP_ABI_V2 || hi < IFS_FS_GROUP_ABI_V2)
        return NULL;
    if (group == IFS_FS_GROUP_CORE)
        return &core.header;
    if (group == IFS_FS_GROUP_VOLUME)
        return &volume_ops.header;
    return NULL;
}
static const IfsFsModuleApiV2 api = {
    .abi_version = IFS_FS_COMMON_ABI_V2,
    .structure_bytes = sizeof(IfsFsModuleApiV2),
    .format_id = "btrfs",
#ifdef IFS_BTRFS_FIRMWARE
    .runtime_profiles = IFS_FS_PROFILE_FREESTANDING | IFS_FS_PROFILE_FIRMWARE,
#else
    .runtime_profiles = IFS_FS_PROFILE_HOSTED,
#endif
    .group_mask = IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) | IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    .query_group = query};
__attribute__((visibility("default"))) const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &api;
}
