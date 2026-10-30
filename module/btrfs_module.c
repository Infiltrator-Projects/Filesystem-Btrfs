/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_btrfs_module_v1.h"
#include "ifs_btrfs_fixture_v1.h"
int ifs_btrfs_worker_main(int,char **);
int ifs_btrfs_fixture_run(const void *,const char *,int,const void *);
int ifs_bind_services(const IfsDefragServicesV1 *);
void ifs_unbind_services(void);
static int run(int argc,char **argv,const IfsDefragServicesV1 *s){
 if(ifs_bind_services(s)!=0)return 1;
 int result=ifs_btrfs_worker_main(argc,argv);
 ifs_unbind_services();return result;
}
static int fixture(const void *spec,const char *path,int create,
 const IfsDefragServicesV1 *s,const IfsBtrfsFixtureHostV1 *h){
 if(ifs_bind_services(s)!=0)return -1;
 int result=ifs_btrfs_fixture_run(spec,path,create,h);
 ifs_unbind_services();return result;
}
static const IfsBtrfsModuleV1 api={IFS_BTRFS_MODULE_V1,sizeof(IfsBtrfsModuleV1),
 "btrfs",IFS_BTRFS_CAP_RUN|IFS_BTRFS_CAP_FIXTURE,run,fixture};
const IfsBtrfsModuleV1 *infiltrator_filesystem_btrfs_v1(void){return &api;}
