/* SPDX-License-Identifier: GPL-3.0-or-later
 * Common V2 Btrfs probe, using the source-owned CRC32C validator. All
 * device reads are bounded host callbacks, not path or fd operations.
 * Legacy native analysis and recovery remain available during migration.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "btrfs_native.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
static uint16_t le16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8)); }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|
         ((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint64_t le64(const uint8_t *p)
{ return (uint64_t)le32(p)|((uint64_t)le32(p+4)<<32); }
static int inspect(const IfsFsHostRuntimeV2 *h,uint32_t *variant,
                   IfsFsVolumeInfoV2 *volume)
{
    const uint64_t where=UINT64_C(65536);
    uint32_t sector,span,off;
    uint8_t *raw;
    int rc=IFS_FS_CORRUPT;
    if(!variant)return IFS_FS_INVALID;
    *variant=0u;
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    sector=h->logical_sector_bytes;
    if(h->maximum_transfer_bytes<sector) return IFS_FS_INVALID;
    span=sector>4096u?sector:4096u;
    if(where>h->volume_bytes || span>h->volume_bytes-where ||
       where%sector) return IFS_FS_NOT_FOUND;
    raw=h->allocate(h->context,span);
    if(!raw) return IFS_FS_NO_MEMORY;
    for(off=0;off<span;off+=sector)
        if(h->read_at(h->context,where+off,raw+off,sector)){
            rc=IFS_FS_IO_ERROR;goto done;
        }
    if(memcmp(raw+64u,"_BHRfS_M",8u)){
        rc=IFS_FS_NOT_FOUND;goto done;
    }
    const uint64_t total=le64(raw+112u),used=le64(raw+120u);
    const uint32_t sectorsize=le32(raw+144u),nodesize=le32(raw+148u);
    if(!btrfs_checksum_valid(raw,4096u) ||
       le16(raw+196u)!=0u || le64(raw+48u)!=where ||
       !total || total>h->volume_bytes || used>total ||
       le64(raw+136u)!=1u || !le64(raw+80u) ||
       !le64(raw+88u) || sectorsize<4096u ||
       (sectorsize&(sectorsize-1u)) ||
       nodesize<sectorsize || nodesize>65536u ||
       (nodesize&(nodesize-1u)) ||
       raw[198u]>8u || raw[199u]>8u)
        goto done;
    *variant=1u;
    if(volume) {
        /* Recorded logical bytes_used: a single-device filesystem only.
         * This is not a verified unallocated-data-extent scan. */
        *volume=(IfsFsVolumeInfoV2){
            .structure_bytes=sizeof(*volume),.variant=1u,
            .max_name_bytes=255u,.block_bytes=sectorsize,
            .total_bytes=total,.free_bytes=total-used
        };
    }
    rc=IFS_FS_OK;
done:
    h->release(h->context,raw);
    return rc;
}
static int probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{ return inspect(h,variant,NULL); }

/* Read-only Common V2 session: the canonical Btrfs scanner validated
 * feature flags and geometry; this reports the superblock's RECORDED available
 * block count, not an independently scanned block bitmap. The host alone
 * owns media I/O and writable access is always rejected. */
static struct {
    uintptr_t generation,active;
    IfsFsVolumeInfoV2 info;
} session;
static int mounted(void *handle)
{ return session.active && (uintptr_t)handle==session.active; }
static int mount_volume(const IfsFsHostRuntimeV2 *h, void **volume,
                        IfsFsVolumeInfoV2 *info)
{
    IfsFsVolumeInfoV2 measured={0};
    uint32_t variant=0u;
    int rc;
    if(!volume || !info || info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    *volume=NULL;
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(!(h->capabilities & IFS_FS_HOST_READ_ONLY))return IFS_FS_ACCESS;
    if(session.active || session.generation==UINTPTR_MAX)return IFS_FS_BUSY;
    rc=inspect(h,&variant,&measured);
    if(rc!=IFS_FS_OK)return rc;
    session.info=measured;
    session.active=++session.generation;
    *info=measured;
    *volume=(void *)session.active;
    return IFS_FS_OK;
}
static int unmount_volume(void *volume)
{
    if(!mounted(volume))return IFS_FS_INVALID;
    session.active=0u;
    session.info=(IfsFsVolumeInfoV2){0};
    return IFS_FS_OK;
}
static int query_volume(void *volume,IfsFsVolumeInfoV2 *info)
{
    if(!mounted(volume) || !info ||
       info->structure_bytes<sizeof(*info))return IFS_FS_INVALID;
    *info=session.info;
    return IFS_FS_OK;
}
static const IfsFsVolumeOpsV2 volume_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
             IFS_FS_GROUP_VOLUME,0u,IFS_FS_VOLUME_QUERY},
    .query=query_volume
};
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT},
    .probe=probe,
    .mount=mount_volume,
    .unmount=unmount_volume
};
static const IfsFsGroupHeaderV2 *query(uint32_t group,uint32_t lo,uint32_t hi)
{
    if(lo>IFS_FS_GROUP_ABI_V2 || hi<IFS_FS_GROUP_ABI_V2)return NULL;
    if(group==IFS_FS_GROUP_CORE)return &core.header;
    if(group==IFS_FS_GROUP_VOLUME)return &volume_ops.header;
    return NULL;
}
static const IfsFsModuleApiV2 api={
    .abi_version=IFS_FS_COMMON_ABI_V2,
    .structure_bytes=sizeof(IfsFsModuleApiV2),
    .format_id="btrfs",
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{ return &api; }
