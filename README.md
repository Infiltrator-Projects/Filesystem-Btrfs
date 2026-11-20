# Filesystem-Btrfs

The production filesystem provider is installed as
`/System/Filesystems/BTRFS.so`, with its manual at
`/System/Resources/Documentation/Filesystem-Btrfs.pdf`.

## Actual Common V2 support

The installed ELF exposes the Common V2 entry point only. It presently
supports CRC32C-checked on-disk superblock probing, metadata-only read-only
mount sessions and volume information through a partition-relative host.

**This version is NOT BOOT-compatible.** It does not implement Btrfs
chunk/root/FS-tree traversal, Common V2 pathname lookup, directory entries
or file reads. Its mount operation is a metadata session, not a working
read-only filesystem namespace. The BOOT validator must not accept this
hosted-only module as a boot provider.

The original native Btrfs analyser, controlled mutation engine, worker,
fixtures and Linux-specific source remain in the repository. Legacy worker
and fixture callbacks are confined to **non-installed regression tests**.
No Linux `.ko` is built or shipped as a fallback. Source-embedded `.run`
and Debian artifacts undergo hosted qualification, but publication is held
until genuine BOOT and OVMF acceptance requirements are met.
