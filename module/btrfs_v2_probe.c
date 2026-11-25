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
        /* On-media B-tree levels are 0..7, including the log tree. */
        raw[198u] >= 8u || raw[199u] >= 8u || raw[200u] >= 8u)
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
static int mount_volume_impl(const IfsFsHostRuntimeV2 *h, void **handle, IfsFsVolumeInfoV2 *info)
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
/* Pin the mounted reader for metadata I/O just as for file reads. */
static int get_metadata(void *handle,const char *path,IfsFsMetadataV2 *out)
{
    Mount *m=acquire(handle);
    return m ? finished(m,btrfs_reader_metadata(m->reader,path,out)) : IFS_FS_INVALID;
}
static const IfsFsMetadataOpsV2 metadata_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(metadata_ops),IFS_FS_GROUP_METADATA,0u,
             IFS_FS_META_GET_MODE},
    .get=get_metadata
};
#ifndef IFS_BTRFS_FIRMWARE
/* Single-device mixed-block-group native formatter. All addresses are
 * partition-relative, with independent physical and logical extent records.
 * Disk layout follows Btrfs's packed tree structures; the existing CRC32C
 * implementation is shared with the production reader. No external mkfs.
 * Initial profile: 4 KiB sectors, 16 KiB nodes, CRC32C, skinny metadata.
 * No RAID, snapshots, quotas, free-space tree or log replay. */
enum { FMT_NODE=16384u,FMT_SECTOR=4096u,FMT_SYSTEM=1048576u,
       FMT_SYSTEM_LEN=4194304u,FMT_MIXED=8388608u,FMT_MIXED_LEN=33554432u };
#define FMT_RELOC (UINT64_MAX-8u)
typedef struct {
    uint8_t raw[FMT_NODE];
    uint32_t count,cursor;
} FormatLeaf;
static atomic_flag format_gate=ATOMIC_FLAG_INIT;
static void f16(uint8_t *p,uint32_t x)
{p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static void f32(uint8_t *p,uint32_t x)
{f16(p,x);f16(p+2,x>>16);}
static void f64(uint8_t *p,uint64_t x)
{f32(p,(uint32_t)x);f32(p+4,(uint32_t)(x>>32));}
static void fkey(uint8_t *p,uint64_t object,uint8_t type,uint64_t offset)
{f64(p,object);p[8]=type;f64(p+9,offset);}
static int fwrite_at(const IfsFsHostRuntimeV2 *h,uint64_t at,const void *p,uint32_t n)
{
    if(h->cancelled && h->cancelled(h->context))return IFS_FS_BUSY;
    return ifs_fs_host_write_bytes_v2(h,at,p,n);
}
static void leaf_begin(FormatLeaf *leaf,const uint8_t *ids,uint64_t address,uint64_t owner)
{
    memset(leaf,0,sizeof(*leaf));leaf->cursor=FMT_NODE;
    memcpy(leaf->raw+32,ids,16u);f64(leaf->raw+48,address);
    f64(leaf->raw+56,(UINT64_C(1)<<56)|1u);
    memcpy(leaf->raw+64,ids+32,16u);
    f64(leaf->raw+80,1u);f64(leaf->raw+88,owner);
}
/* The seven small trees have a fixed, explicitly sorted key sequence.
 * Insertions are bounded and checked rather than relying on fixture sizes. */
static uint8_t *leaf_item(FormatLeaf *leaf,uint64_t object,uint8_t type,
                          uint64_t offset,uint32_t bytes)
{
    if(bytes>leaf->cursor || leaf->cursor-bytes<101u+25u*(leaf->count+1u))
        return NULL;
    if(leaf->count) {
        const uint8_t *prev=leaf->raw+101u+25u*(leaf->count-1u);
        const uint64_t po=le64(prev),pf=le64(prev+9);
        if(object<po || (object==po && (type<prev[8] ||
           (type==prev[8] && offset<=pf))))return NULL;
    }
    leaf->cursor-=bytes;
    uint8_t *slot=leaf->raw+101u+25u*leaf->count++;
    fkey(slot,object,type,offset);f32(slot+17,leaf->cursor-101u);
    f32(slot+21,bytes);
    return leaf->raw+leaf->cursor;
}
static int leaf_commit(const IfsFsHostRuntimeV2 *h,FormatLeaf *leaf)
{
    f32(leaf->raw+96,leaf->count);
    f32(leaf->raw,btrfs_crc32c(leaf->raw+32,FMT_NODE-32u));
    return fwrite_at(h,le64(leaf->raw+48),leaf->raw,FMT_NODE);
}
static void chunk_record(uint8_t *p,const uint8_t *ids,uint64_t at,uint64_t length,uint64_t flags)
{
    f64(p,length);f64(p+8,2u);f64(p+16,65536u);f64(p+24,flags);
    f32(p+32,FMT_SECTOR);f32(p+36,FMT_SECTOR);f32(p+40,FMT_SECTOR);
    f16(p+44,1u);f16(p+46,1u);f64(p+48,1u);f64(p+56,at);
    memcpy(p+64,ids+16,16u);
}
static void inode_record(uint8_t *p,uint64_t size,uint64_t now)
{
    f64(p,1u);f64(p+8,1u);f64(p+16,size);f64(p+24,FMT_NODE);
    f32(p+40,1u);f32(p+52,0040755u);f64(p+72,1u);
    for(unsigned i=0;i<4u;++i)f64(p+112u+i*12u,now);
}
static uint32_t hash_name(const char *name)
{
    uint32_t crc=UINT32_MAX-1u;
    for(;*name;++name) {
        crc^=(uint8_t)*name;
        for(unsigned i=0;i<8u;++i)crc=(crc>>1)^
            (UINT32_C(0x82f63b78)&(0u-(crc&1u)));
    }
    return crc;
}
static int format_impl(const IfsFsHostRuntimeV2 *h,const char *label)
{
    /* UUIDs: filesystem, device, chunk tree. Unique for every format. */
    uint8_t ids[48],sb[4096]={0},dev[98]={0},system[80]={0},mixed[80]={0};
    int rc=ifs_fs_host_random_bytes_v2(h,ids,sizeof(ids));
    if(rc!=IFS_FS_OK)return rc;
    for(unsigned i=0;i<3u;++i) {
        ids[i*16u+6]=(uint8_t)((ids[i*16u+6]&15u)|64u);
        ids[i*16u+8]=(uint8_t)((ids[i*16u+8]&63u)|128u);
    }
    uint64_t now=h->realtime_ns?h->realtime_ns(h->context)/1000000000ull:0u;
    const uint64_t total=h->volume_bytes;
    const uint64_t owners[]={3u,1u,2u,4u,5u,7u,FMT_RELOC};
    const uint64_t addr[]={FMT_SYSTEM,FMT_MIXED,FMT_MIXED+FMT_NODE,
        FMT_MIXED+2u*FMT_NODE,FMT_MIXED+3u*FMT_NODE,
        FMT_MIXED+4u*FMT_NODE,FMT_MIXED+5u*FMT_NODE};
    FormatLeaf *leaf=h->allocate(h->context,sizeof(*leaf));
    if(!leaf)return IFS_FS_NO_MEMORY;
    uint8_t *v;
#define ITEM(o,t,f,n) do {v=leaf_item(leaf,(o),(t),(f),(n)); \
    if(!v){rc=IFS_FS_CORRUPT;goto done;}}while(0)
#define COMMIT() do {rc=leaf_commit(h,leaf);if(rc!=IFS_FS_OK)goto done;}while(0)
    /* Clear both mirrors before changing the referenced trees. */
    rc=fwrite_at(h,65536u,sb,sizeof(sb));if(rc!=IFS_FS_OK)goto done;
    rc=fwrite_at(h,64ull*1024u*1024u,sb,sizeof(sb));if(rc!=IFS_FS_OK)goto done;
    if(h->flush(h->context)!=0){rc=IFS_FS_IO_ERROR;goto done;}
    chunk_record(system,ids,FMT_SYSTEM,FMT_SYSTEM_LEN,2u);
    chunk_record(mixed,ids,FMT_MIXED,FMT_MIXED_LEN,5u);
    f64(dev,1u);f64(dev+8,total);f64(dev+16,FMT_SYSTEM_LEN+FMT_MIXED_LEN);
    f32(dev+24,FMT_SECTOR);f32(dev+28,FMT_SECTOR);f32(dev+32,FMT_SECTOR);
    memcpy(dev+66,ids+16,16u);memcpy(dev+82,ids,16u);
    leaf_begin(leaf,ids,addr[0],3u);
    ITEM(1u,216u,1u,sizeof(dev));memcpy(v,dev,sizeof(dev));
    ITEM(256u,228u,FMT_SYSTEM,sizeof(system));memcpy(v,system,sizeof(system));
    ITEM(256u,228u,FMT_MIXED,sizeof(mixed));memcpy(v,mixed,sizeof(mixed));COMMIT();

    /* Root tree: extent/dev/fs/csum/relocation root items, and default
     * subvolume link in the root-tree directory (object six). */
    leaf_begin(leaf,ids,addr[1],1u);
    for(unsigned i=2u;i<7u;++i) {
        if(i==4u) {
            ITEM(5u,12u,6u,17u);f16(v+8,7u);memcpy(v+10,"default",7u);
        }
        if(i==5u) {
            ITEM(6u,1u,0u,160u);inode_record(v,14u,now);
            ITEM(6u,12u,6u,12u);f16(v+8,2u);memcpy(v+10,"..",2u);
            ITEM(6u,84u,hash_name("default"),37u);
            fkey(v,5u,132u,UINT64_MAX);f64(v+17,1u);f16(v+27,7u);
            v[29]=2u;memcpy(v+30,"default",7u);
        }
        ITEM(owners[i],132u,0u,439u);
        f64(v+160,1u);f64(v+168,(i==4u||i==6u)?256u:0u);
        f64(v+176,addr[i]);f64(v+192,FMT_NODE);f32(v+216,1u);
        f64(v+239,1u);
        if(i==4u||i==6u)inode_record(v,0u,now);
    }
    COMMIT();
    /* Allocation accounting includes every tree and both block groups.
     * Inline root backrefs allow independent root-item consistency checking. */
    leaf_begin(leaf,ids,addr[2],2u);
    for(unsigned i=0;i<7u;++i) {
        ITEM(addr[i],169u,0u,33u);f64(v,1u);f64(v+8,1u);f64(v+16,2u);
        /* Inline root backreference is required by root-item checking. */
        v[24]=176u;f64(v+25,owners[i]);
        if(i==0u||i==1u) {
            ITEM(addr[i],192u,i?FMT_MIXED_LEN:FMT_SYSTEM_LEN,24u);
            f64(v,i?6u*FMT_NODE:FMT_NODE);f64(v+8,256u);f64(v+16,i?5u:2u);
        }
    }
    COMMIT();
    leaf_begin(leaf,ids,addr[3],4u);
    for(unsigned i=0;i<2u;++i) {
        const uint64_t at=i?FMT_MIXED:FMT_SYSTEM;
        ITEM(1u,204u,at,48u);f64(v,3u);f64(v+8,256u);
        f64(v+16,at);f64(v+24,i?FMT_MIXED_LEN:FMT_SYSTEM_LEN);
        memcpy(v+32,ids+32,16u);
    }
    COMMIT();
    for(unsigned i=4u;i<7u;++i) {
        leaf_begin(leaf,ids,addr[i],owners[i]);
        if(i!=5u) {
            ITEM(256u,1u,0u,160u);inode_record(v,0u,now);
            ITEM(256u,12u,256u,12u);f16(v+8,2u);memcpy(v+10,"..",2u);
        }
        COMMIT();
    }
    if(h->flush(h->context)!=0){rc=IFS_FS_IO_ERROR;goto done;}
    memcpy(sb+32,ids,16u);f64(sb+56,1u);memcpy(sb+64,"_BHRfS_M",8u);
    f64(sb+72,1u);f64(sb+80,addr[1]);f64(sb+88,addr[0]);
    f64(sb+112,total);f64(sb+120,7u*FMT_NODE);f64(sb+128,6u);f64(sb+136,1u);
    f32(sb+144,FMT_SECTOR);f32(sb+148,FMT_NODE);f32(sb+152,FMT_NODE);
    f32(sb+156,FMT_SECTOR);f32(sb+160,97u);f64(sb+164,1u);
    f64(sb+188,0x106u); /* mixed groups, default subvol, skinny metadata */
    memcpy(sb+201,dev,sizeof(dev));memcpy(sb+299,label,strlen(label));
    f64(sb+555,UINT64_MAX);
    fkey(sb+811,256u,228u,FMT_SYSTEM);memcpy(sb+828,system,80u);
    f64(sb+48,64ull*1024u*1024u);f32(sb,btrfs_crc32c(sb+32,sizeof(sb)-32u));
    rc=fwrite_at(h,64ull*1024u*1024u,sb,sizeof(sb));if(rc!=IFS_FS_OK)goto done;
    if(h->flush(h->context)!=0){rc=IFS_FS_IO_ERROR;goto done;}
    f64(sb+48,65536u);f32(sb,btrfs_crc32c(sb+32,sizeof(sb)-32u));
    rc=fwrite_at(h,65536u,sb,sizeof(sb));if(rc!=IFS_FS_OK)goto done;
    if(h->flush(h->context)!=0) {
        memset(sb,0,sizeof(sb));
        (void)ifs_fs_host_write_bytes_v2(h,65536u,sb,sizeof(sb));
        (void)ifs_fs_host_write_bytes_v2(h,64ull*1024u*1024u,sb,sizeof(sb));
        (void)h->flush(h->context);rc=IFS_FS_IO_ERROR;goto done;
    }
    BtrfsReader *reader=NULL;
    rc=btrfs_reader_open(h,&reader);
    if(rc==IFS_FS_OK)btrfs_reader_close(reader);
done:
    h->release(h->context,leaf);
    return rc;
#undef ITEM
#undef COMMIT
}
static int format_volume(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                          const char *label,uint32_t flags)
{
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    if(variant!=1u || flags || h->logical_sector_bytes>FMT_SECTOR ||
       h->volume_bytes%FMT_NODE || h->volume_bytes<128ull*1024u*1024u ||
       h->volume_bytes>16ull*1024u*1024u*1024u)return IFS_FS_UNSUPPORTED;
    if(!label)label="";
    if(strlen(label)>255u || strchr(label,'/'))return IFS_FS_INVALID;
    if(atomic_flag_test_and_set_explicit(&format_gate,memory_order_acquire))return IFS_FS_BUSY;
    int rc=IFS_FS_OK;
    lock_mounts();
    for(unsigned i=0;i<IFS_FS_RO_MAX_SESSIONS_V2;++i)
        if(mounts[i].reader){rc=IFS_FS_BUSY;break;}
    unlock_mounts();
    if(rc==IFS_FS_OK)rc=format_impl(h,label);
    atomic_flag_clear_explicit(&format_gate,memory_order_release);
    return rc;
}
#endif
static int mount_volume(const IfsFsHostRuntimeV2 *h,void **handle,IfsFsVolumeInfoV2 *info)
{
#ifndef IFS_BTRFS_FIRMWARE
    if(atomic_flag_test_and_set_explicit(&format_gate,memory_order_acquire)) {
        if(handle)*handle=NULL;
        return IFS_FS_BUSY;
    }
#endif
    int rc=mount_volume_impl(h,handle,info);
#ifndef IFS_BTRFS_FIRMWARE
    atomic_flag_clear_explicit(&format_gate,memory_order_release);
#endif
    return rc;
}

/* Hold the session pin while returning the verified superblock label. */
static int volume_label(void *handle,char *out,uint32_t capacity,uint32_t *bytes)
{
    if(!bytes)return IFS_FS_INVALID;
    *bytes=0u;
    Mount *m=acquire(handle);
    return m ? finished(m,btrfs_reader_label(m->reader,out,capacity,bytes)) : IFS_FS_INVALID;
}
static const IfsFsVolumeOpsV2 volume_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2), IFS_FS_GROUP_VOLUME, 0u,
               IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_LABEL_GET
#ifndef IFS_BTRFS_FIRMWARE
               | IFS_FS_VOLUME_FORMAT
#endif
    },
    .query = query_volume, .label=volume_label
#ifndef IFS_BTRFS_FIRMWARE
    ,.format = format_volume
#endif
};
#ifndef IFS_BTRFS_FIRMWARE
static int map_file(void *handle,const char *path,uint64_t first,
                    IfsFsExtentV2 *out,uint32_t capacity,uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0u;
    Mount *m=acquire(handle);
    return m?finished(m,btrfs_reader_map(m->reader,path,first,out,capacity,written)):
             IFS_FS_INVALID;
}
static const IfsFsDefragOpsV2 defrag_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(defrag_ops),IFS_FS_GROUP_DEFRAG,0u,
             IFS_FS_DEFRAG_MAP},
    .map=map_file
};
#endif
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
    if (group == IFS_FS_GROUP_METADATA) return &metadata_ops.header;
#ifndef IFS_BTRFS_FIRMWARE
    if(group==IFS_FS_GROUP_DEFRAG)return &defrag_ops.header;
#endif
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
    .group_mask = IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) | IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME) |
                  IFS_FS_GROUP_BIT(IFS_FS_GROUP_METADATA)
#ifndef IFS_BTRFS_FIRMWARE
                  |IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)
#endif
                  ,
    .query_group = query};
__attribute__((visibility("default"))) const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &api;
}
