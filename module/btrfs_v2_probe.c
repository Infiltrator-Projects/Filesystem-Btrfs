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
static int probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{
    const uint64_t where=UINT64_C(65536);
    uint32_t sector,span,off;
    uint8_t *raw;
    int rc=IFS_FS_CORRUPT;
    if(!variant || !ifs_fs_host_v2_valid(h)) return IFS_FS_INVALID;
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
    rc=IFS_FS_OK;
done:
    h->release(h->context,raw);
    return rc;
}
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,IFS_FS_CORE_PROBE},
    .probe=probe
};
static const IfsFsGroupHeaderV2 *query(uint32_t group,uint32_t lo,uint32_t hi)
{
    return group==IFS_FS_GROUP_CORE && lo<=IFS_FS_GROUP_ABI_V2 &&
           hi>=IFS_FS_GROUP_ABI_V2 ? &core.header:NULL;
}
static const IfsFsModuleApiV2 api={
    .abi_version=IFS_FS_COMMON_ABI_V2,
    .structure_bytes=sizeof(IfsFsModuleApiV2),
    .format_id="btrfs",
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{ return &api; }
