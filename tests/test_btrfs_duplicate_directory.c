/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise the exact native directory decoder linked into both BTRFS.so
 * profiles. The separate real-image test continues to exercise this reader
 * through the exported Common V2 provider on immutable media.
 */
#include "../native/btrfs_reader.c"
#include <stdio.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr,"Btrfs duplicate directory regression line %u: %s\n", \
            (unsigned)__LINE__,#test); return 1; \
} } while (0)

static void store16(uint8_t *p, uint16_t value)
{
    p[0]=(uint8_t)value;
    p[1]=(uint8_t)(value>>8);
}
static void store64(uint8_t *p, uint64_t value)
{
    for(unsigned i=0;i<8u;++i)
        p[i]=(uint8_t)(value>>(i*8u));
}
static void dir_record(uint8_t record[34], const char name[4])
{
    memset(record,0,34);
    store64(record,512u);
    record[8]=INODE_ITEM;
    store16(record+25,0u);
    store16(record+27,4u);
    record[29]=1u; /* regular-file directory type */
    memcpy(record+30,name,4u);
}
int main(void)
{
    uint8_t first[34],different[34],paired[68];
    Key indexed={256u,DIR_INDEX,0u};
    dir_record(first,"boot");
    dir_record(different,"init");
    DirectorySearch exact={.match="boot",.length=4u};
    CHECK(dir_item(NULL,indexed,first,sizeof(first),&exact)==IFS_FS_OK);
    CHECK(exact.found==1u && exact.location.id==512u);
    CHECK(!memcmp(exact.name,"boot",4u));
    CHECK(dir_item(NULL,indexed,different,sizeof(different),&exact)==IFS_FS_OK);
    CHECK(exact.found==1u && !memcmp(exact.name,"boot",4u));
    /* Two separately keyed index items with the same filename must not
     * silently resolve to whichever inode happened to appear first. */
    CHECK(dir_item(NULL,indexed,first,sizeof(first),&exact)==IFS_FS_CORRUPT);

    DirectorySearch indexed_listing={.skip=0u};
    CHECK(dir_item(NULL,indexed,first,sizeof(first),&indexed_listing)==STOP);
    CHECK(indexed_listing.found==1u);
    DirectorySearch missing={.match="none",.length=4u};
    CHECK(dir_item(NULL,indexed,first,sizeof(first),&missing)==IFS_FS_OK);
    CHECK(missing.found==0u);

    /* Hash-bucket records may contain multiple entries; enforce exact
     * parent/name uniqueness there too without rejecting a hash collision
     * between distinct names. */
    memcpy(paired,first,sizeof(first));
    memcpy(paired+sizeof(first),different,sizeof(different));
    DirectorySearch collision={.match="boot",.length=4u};
    Key bucket={256u,DIR_ITEM,0u};
    CHECK(dir_item(NULL,bucket,paired,sizeof(paired),&collision)==IFS_FS_OK);
    CHECK(collision.found==1u);
    memcpy(paired+sizeof(first),first,sizeof(first));
    DirectorySearch repeated={.match="boot",.length=4u};
    CHECK(dir_item(NULL,bucket,paired,sizeof(paired),&repeated)==IFS_FS_CORRUPT);
    return 0;
}
