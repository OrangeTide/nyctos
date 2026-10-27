# Changelog

Notable changes per minor version. Patch bumps between minors move the VERSION
file alone and are not listed here; see the git log for their detail. Each
milestone's full retrospective lives in docs/ROADMAP.md.

## 0.27.0

M26, "A Write the Disk Keeps Across a Boot," is the milestone of this release.
The user-space virtio-mmio store gained a write path: it answers FS_OP_WRITE by
a read-modify-write of the block's sector, so a write smaller than a sector
leaves its neighbours alone, and a marker written in one boot is read back by
the next boot over the same disk image.

The versioning and release process was also established in this release: the
VERSION file as the source of truth, this changelog, annotated tags on minor
bumps, and a local CI run across all four configs after tagging and before any
push for as long as ocelot is the host.

## 0.26.0

Baseline. The version number is anchored here at the project's built history.
The scheme did not exist before this point; the milestones up to it are
recorded in docs/ROADMAP.md.
