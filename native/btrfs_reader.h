/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_BTRFS_READER_H
#define IFS_BTRFS_READER_H
#include <infiltratr/filesystem_module_v2.h>
#include <stdbool.h>

typedef struct BtrfsReader BtrfsReader;
uint32_t btrfs_crc32c(const uint8_t *data, size_t bytes);
bool btrfs_checksum_valid(const uint8_t *block, size_t length);
int btrfs_reader_open(const IfsFsHostRuntimeV2 *host, BtrfsReader **out);
void btrfs_reader_close(BtrfsReader *reader);
int btrfs_reader_stat(BtrfsReader *reader, const char *path, IfsFsEntryV2 *out);
int btrfs_reader_list(BtrfsReader *reader, const char *path, uint32_t index, IfsFsEntryV2 *out);
int btrfs_reader_read(BtrfsReader *reader, const char *path, uint64_t offset, void *out,
                      uint32_t bytes);
#endif
