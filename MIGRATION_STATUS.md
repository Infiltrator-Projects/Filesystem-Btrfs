# Btrfs migration
Native first-party Btrfs analysis, durable Defrag/Growth/Recover, and the
Test Media formatter and exact payload checker are now compiled in BTRFS.so.
Generic host services/corpus callbacks are provided by Defragmenter through
size- and version-validated interfaces. No external mkfs tool is required.

This driver must pass standalone build/test AND a Defragmenter integration
regression before deleting original embedded source; source import alone
does not establish integration.
