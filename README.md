# Filesystem-Btrfs
Independent filesystem implementation for InfiltratorOS, using the shared Common ABI.

## Installer files

- `/System/Filesystems/BTRFS.so`
- `/System/Resources/Documentation/Filesystem-Btrfs.pdf`
- `/System/Kernel/Filesystems/btrfs.ko`

The published Debian package installs these three regular files. A manual native RUN compiles the shared object and Linux adapter locally, generates the PDF, and replaces all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.

## Common V2 migration (1.0.8)

BTRFS.so now supports canonical Common V2 CORE identification through
bounded host I/O and the same CRC32C verifier used by the native engine.
The existing V1 path-based analyser and controlled writer remain until
V2 allocation-tree traversal, namespace and mutation operations are
fully qualified. Probe-only Common V2 must not be represented as
completed mount, file I/O, defragmentation or repair.
