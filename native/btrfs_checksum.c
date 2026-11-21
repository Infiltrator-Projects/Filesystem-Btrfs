/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "btrfs_reader.h"
#include <infiltratr/endian.h>

uint32_t btrfs_crc32c(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0U; index < length; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (UINT32_C(0x82f63b78) & mask);
        }
    }
    return ~crc;
}

bool btrfs_checksum_valid(const uint8_t *block, size_t length)
{
    if (length < 32U)
        return false;
    return infiltratr_load_le32(block) == btrfs_crc32c(block + 32U, length - 32U);
}
