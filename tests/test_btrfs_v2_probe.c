/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
static uint8_t disk[1048576];
static int reader(void *ctx,uint64_t at,void *out,uint32_t n) {
    (void)ctx;
    if(n!=512u || at>sizeof(disk) || n>sizeof(disk)-at) return -1;
    memcpy(out,disk+(size_t)at,n);return 0;
}
static void *alloc(void *c,size_t n){(void)c;return malloc(n);}
static void release(void *c,void *p){(void)c;free(p);}
static void le32(uint8_t *p,uint32_t v){for(int i=0;i<4;++i)p[i]=(uint8_t)(v>>(i*8));}
static void le64(uint8_t *p,uint64_t v){for(int i=0;i<8;++i)p[i]=(uint8_t)(v>>(i*8));}
static uint32_t crc32c(const uint8_t *p,size_t n) {
    uint32_t crc=UINT32_MAX;
    for(size_t i=0;i<n;++i) {
        crc^=p[i];
        for(int bit=0;bit<8;++bit)
            crc=(crc>>1)^(UINT32_C(0x82f63b78)&(uint32_t)(0u-(crc&1u)));
    }
    return ~crc;
}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"Btrfs V2: %s\n",#x);return 1;}}while(0)
int main(void) {
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"btrfs"));
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    CHECK(core && core->probe && core->mount && core->unmount &&
          vol && vol->query && !core->write);
    CHECK(core->header.capabilities==(IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT));
    CHECK(vol->header.capabilities==IFS_FS_VOLUME_QUERY);
    IfsFsHostRuntimeV2 h={0};
    h.abi_version=IFS_FS_HOST_ABI_V2;h.structure_bytes=sizeof(h);
    h.capabilities=IFS_FS_HOST_READ_ONLY;h.volume_bytes=sizeof(disk);
    h.logical_sector_bytes=512;h.physical_sector_bytes=512;
    h.maximum_transfer_bytes=512;h.context=disk;
    h.read_at=reader;h.allocate=alloc;h.release=release;
    uint32_t variant=0;
    CHECK(core->probe(&h,&variant)==IFS_FS_NOT_FOUND);
    uint8_t *s=disk+65536u;
    memcpy(s+64u,"_BHRfS_M",8u);
    for(unsigned i=0u;i<16u;++i)s[32u+i]=(uint8_t)(i+1u);
    le64(s+48u,65536u);le64(s+80u,4096u);
    le64(s+88u,8192u);le64(s+112u,sizeof(disk));
    le64(s+120u,32768u);le64(s+136u,1u);
    le32(s+144u,4096u);le32(s+148u,4096u);
    le32(s,crc32c(s+32u,4096u-32u));
    CHECK(core->probe(&h,&variant)==IFS_FS_OK && variant==1u);
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    void *mounted=NULL,*old=NULL;
    CHECK(core->mount(&h,&mounted,&info)==IFS_FS_OK && mounted);
    CHECK(info.variant==1u && info.block_bytes==4096u &&
          info.total_bytes==sizeof(disk) &&
          info.free_bytes==sizeof(disk)-32768u);
    CHECK(core->mount(&h,&old,&info)==IFS_FS_OK && old && old!=mounted);
    info.structure_bytes=sizeof(info);
    CHECK(vol->query(mounted,&info)==IFS_FS_OK);
    CHECK(core->unmount(mounted)==IFS_FS_OK);
    CHECK(core->unmount(mounted)==IFS_FS_INVALID);
    CHECK(vol->query(mounted,&info)==IFS_FS_INVALID);
    info.structure_bytes=sizeof(info);
    CHECK(vol->query(old,&info)==IFS_FS_OK);
    CHECK(core->unmount(old)==IFS_FS_OK);
    CHECK(vol->query(old,&info)==IFS_FS_INVALID);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&h,&mounted,&info)==IFS_FS_OK && mounted!=old);
    CHECK(vol->query(old,&info)==IFS_FS_INVALID);
    CHECK(core->unmount(mounted)==IFS_FS_OK);
    le64(s+120u,sizeof(disk)+1u);
    le32(s,crc32c(s+32u,4096u-32u));
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&h,&mounted,&info)==IFS_FS_CORRUPT && !mounted);
    le64(s+120u,32768u);
    le32(s,crc32c(s+32u,4096u-32u));
    s[200u]^=1u;variant=0;
    CHECK(core->probe(&h,&variant)==IFS_FS_CORRUPT && variant==0);
    s[200u]^=1u;
    /* Checksum-valid fabricated identity cannot describe a real Btrfs
     * volume even when all other geometry is internally consistent. */
    uint8_t saved_fsid[16];
    memcpy(saved_fsid,s+32u,sizeof(saved_fsid));
    memset(s+32u,0u,sizeof(saved_fsid));
    le32(s,crc32c(s+32u,4096u-32u));
    variant=0u;
    CHECK(core->probe(&h,&variant)==IFS_FS_CORRUPT && variant==0u);
    memcpy(s+32u,saved_fsid,sizeof(saved_fsid));
    le64(s+80u,4097u);
    le32(s,crc32c(s+32u,4096u-32u));
    CHECK(core->probe(&h,&variant)==IFS_FS_CORRUPT && variant==0u);
    le64(s+80u,4096u);
    le64(s+88u,8193u);
    le32(s,crc32c(s+32u,4096u-32u));
    CHECK(core->probe(&h,&variant)==IFS_FS_CORRUPT && variant==0u);
    le64(s+88u,8192u);
    /* Checksum-valid metadata must still fail for impossible geometry. */
    le32(s+144u,131072u);le32(s+148u,131072u);
    le32(s,crc32c(s+32u,4096u-32u));
    variant=0;
    CHECK(core->probe(&h,&variant)==IFS_FS_CORRUPT && variant==0u);
    le32(s+144u,4096u);le32(s+148u,4096u);
    /* A future incompatible tree format must not be mistaken for
     * a supported Common V2 filesystem. */
    le64(s+188u,UINT64_C(1)<<12u);
    le32(s,crc32c(s+32u,4096u-32u));
    CHECK(core->probe(&h,&variant)==IFS_FS_UNSUPPORTED && variant==0u);
    le64(s+188u,UINT64_C(1)<<61u);
    le32(s,crc32c(s+32u,4096u-32u));
    CHECK(core->probe(&h,&variant)==IFS_FS_UNSUPPORTED && variant==0u);
    le64(s+188u,0u);
    le64(s+180u,UINT64_C(1)<<60u);
    le32(s,crc32c(s+32u,4096u-32u));
    CHECK(core->probe(&h,&variant)==IFS_FS_UNSUPPORTED && variant==0u);
    le64(s+180u,0u);
    le32(s,crc32c(s+32u,4096u-32u));
    CHECK(core->probe(&h,&variant)==IFS_FS_OK && variant==1u);
    h.maximum_transfer_bytes=0;
    CHECK(core->probe(&h,&variant)==IFS_FS_INVALID);
    return 0;
}
