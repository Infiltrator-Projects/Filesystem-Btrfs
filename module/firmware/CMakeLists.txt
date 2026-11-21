# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(BtrfsFirmware LANGUAGES C)
add_library(btrfs-boot MODULE ../btrfs_v2_probe.c ../../native/btrfs_checksum.c
    ../../native/btrfs_reader.c runtime.c)
set_target_properties(btrfs-boot PROPERTIES PREFIX "" OUTPUT_NAME BTRFS C_VISIBILITY_PRESET hidden)
target_compile_features(btrfs-boot PRIVATE c_std_11)
target_include_directories(btrfs-boot PRIVATE "${COMMON_INCLUDE_DIR}" ../../native)
target_compile_definitions(btrfs-boot PRIVATE IFS_BTRFS_FIRMWARE=1)
target_compile_options(btrfs-boot PRIVATE -Wall -Wextra -Wpedantic -Werror
    -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone
    -fno-tree-loop-distribute-patterns -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(btrfs-boot PRIVATE -nostdlib -Wl,-Bsymbolic,-z,defs,-z,noexecstack,-z,relro,-z,now)
