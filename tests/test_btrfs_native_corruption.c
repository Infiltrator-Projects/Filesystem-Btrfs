/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Damaged-media safety check for the canonical BTRFS.so native analyser.
 * This uses a disposable regular image; it does not mount, format or alter
 * an external drive. The fake magic is deliberately insufficient to create
 * a valid checksummed Btrfs superblock.
 */
#include "btrfs_native.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "Btrfs corruption regression failed at line %d: %s\\n", \
            __LINE__, #expr); goto failed; } } while (0)

int main(void)
{
    char image[] = "/tmp/ifs-btrfs-image-XXXXXX";
    char stage[] = "/tmp/ifs-btrfs-stage-XXXXXX";
    char error[256] = {0};
    char marker[sizeof("UNCHANGED")] = {0};
    BtrfsAnalysis analysis = {0};
    uint64_t commit_bytes = 0;
    int image_fd = -1;
    int stage_fd = -1;
    int result = 1;

    image_fd = mkstemp(image);
    if (image_fd < 0)
        goto failed;
    stage_fd = mkstemp(stage);
    if (stage_fd < 0)
        goto failed;

    CHECK(ftruncate(image_fd, 256 * 1024) == 0);
    CHECK(!btrfs_probe(image));

    /* A matching signature without a valid CRC must never be mutable. */
    CHECK(pwrite(image_fd, "_BHRfS_M", 8, 65536 + 64) == 8);
    CHECK(btrfs_probe(image));
    CHECK(btrfs_analyse(image, &analysis, error, sizeof(error)) != 0);
    btrfs_analysis_free(&analysis);
    CHECK(btrfs_writer_preflight(image, error, sizeof(error)) != 0);
    CHECK(btrfs_verify_layout(image, false, 0, error, sizeof(error)) != 0);

    CHECK(pwrite(stage_fd, "UNCHANGED", sizeof(marker), 0) ==
          (ssize_t)sizeof(marker));
    CHECK(btrfs_build_stage(image, stage, false, 0, false,
                            &commit_bytes, error, sizeof(error)) != 0);
    CHECK(access(stage, F_OK) == 0);
    CHECK(pread(stage_fd, marker, sizeof(marker), 0) ==
          (ssize_t)sizeof(marker));
    CHECK(memcmp(marker, "UNCHANGED", sizeof(marker)) == 0);

    /* The rejected source must not be modified in place. */
    CHECK(pread(image_fd, marker, 8, 65536 + 64) == 8);
    CHECK(memcmp(marker, "_BHRfS_M", 8) == 0);
    result = 0;

failed:
    btrfs_analysis_free(&analysis);
    if (image_fd >= 0)
        (void)close(image_fd);
    if (stage_fd >= 0)
        (void)close(stage_fd);
    (void)unlink(image);
    (void)unlink(stage);
    return result;
}
