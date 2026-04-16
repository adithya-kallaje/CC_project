# Mini-UnionFS

A userspace Union File System built on libfuse3. Merges a read-only
`lower_dir` and a read-write `upper_dir` into a single mount point,
with Copy-on-Write and whiteout-based deletions — the same paradigm
Docker's image/container layers use.

## Build

Requires `fuse3-devel` (Fedora) or `libfuse3-dev` (Debian/Ubuntu).

```
make
```

## Run

```
./mini_unionfs <lower_dir> <upper_dir> <mount_point> [-f] [-d]
```

`-f` keeps the process in the foreground; `-d` adds FUSE debug output.

Unmount with:

```
fusermount3 -u <mount_point>
```

## Test

```
chmod +x test_unionfs.sh
./test_unionfs.sh
```

Runs the three tests from the project spec (visibility, CoW, whiteout).

## Supported operations

`getattr`, `readdir`, `open`, `create`, `read`, `write`, `release`,
`unlink`, `mkdir`, `rmdir`.
