/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_btrfs_module_v1.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"fail: %s\n",#x);return 1;} } while(0)
int main(void){
 const IfsBtrfsModuleV1 *api=infiltrator_filesystem_btrfs_v1();
 CHECK(api && api->abi_version==1u && api->structure_bytes>=sizeof(*api));
 CHECK(api->format_id && strcmp(api->format_id,"btrfs")==0);
 CHECK(api->capabilities==(IFS_BTRFS_CAP_RUN|IFS_BTRFS_CAP_FIXTURE));
 CHECK(api->run && api->fixture);
 char *argv[]={"defragmenter-btrfs-worker","--version",NULL};
 CHECK(api->run(2,argv,NULL)!=0);
 return 0;
}
