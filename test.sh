#!/usr/bin/env bash
# Demonstration + automated verification of the virtual file system.
set -uo pipefail
cd "$(dirname "$0")" || exit 1
VFS="$(pwd)/vfs"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/vfs-demo.XXXXXX")" || exit 1
IMG="$WORK/demo.img"
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
STEP=0
LAST_OUTPUT=''
LAST_RC=0

section() { printf '\n\n========== %s ==========\n' "$1"; }
# Run a command silently; checks use its captured status and output.
run() {
    LAST_OUTPUT="$("$@" 2>&1)"
    LAST_RC=$?
}
# Print actual output for a few representative demo commands only.
show_output() {
    printf '  $'
    printf ' %q' "$@"
    printf '\n'
    run "$@"
    if [[ -n "$LAST_OUTPUT" ]]; then
        printf '%s\n' "$LAST_OUTPUT" | head -n 14 | sed 's/^/    /'
    fi
}
pass() { printf '  [PASS] %s\n' "$1"; PASS=$((PASS + 1)); }
fail() { printf '  [FAIL] %s\n' "$1"; FAIL=$((FAIL + 1)); }
expect_ok() {
    local label="$1"
    if [[ "$LAST_RC" -eq 0 ]]; then pass "$label"; else fail "$label"; fi
}
expect_error() {
    local label="$1"
    if [[ "$LAST_RC" -ne 0 ]]; then pass "$label"; else fail "$label (unexpected success)"; fi
}
contains() {
    local needle="$1" label="$2"
    if [[ "$LAST_RC" -eq 0 && "$LAST_OUTPUT" == *"$needle"* ]]; then pass "$label"; else fail "$label"; fi
}
vfs() { run "$VFS" "$IMG" "$@"; }

if [[ ! -x "$VFS" ]]; then
    printf 'Executable not found. Run "make" first.\n' >&2
    exit 1
fi

section '1. FORMAT VIRTUAL DISK'
run "$VFS" mkfs "$IMG" 64 1024
expect_ok '64 MiB disk image created successfully'

section '2. DIRECTORY AND FILE CREATION'
vfs mkdir /docs
expect_ok 'Created /docs directory'
vfs mkdir /docs/projects
expect_ok 'Created nested /docs/projects directory'
vfs touch /docs/projects/notes.txt
expect_ok 'Created an empty file'
vfs write /docs/projects/notes.txt 'Hello from the virtual file system'
expect_ok 'Wrote text to the virtual file'
show_output "$VFS" "$IMG" cat /docs/projects/notes.txt
contains 'Hello from the virtual file system' 'Read back correct file content'
show_output "$VFS" "$IMG" ls -l /docs/projects
contains 'notes.txt' 'Directory listing contains notes.txt'
vfs tree /
contains 'projects/' 'Directory tree shows nested folder'

section '3. FILE METADATA AND PERMISSIONS'
vfs stat /docs/projects/notes.txt
contains 'Inode:' 'Stat displays inode metadata'
vfs chmod 600 /docs/projects/notes.txt
expect_ok 'Changed mode to 0600'
vfs stat /docs/projects/notes.txt
contains '0600' 'Updated permission mode is visible'
run "$VFS" -u 1001 -g 1001 "$IMG" cat /docs/projects/notes.txt
expect_error 'Other user cannot read a mode-0600 file'
vfs cat /docs/projects/notes.txt
contains 'Hello from the virtual file system' 'Owner can still read the file'

section '4. LINKS, RENAMING AND ERROR HANDLING'
vfs ln /docs/projects/notes.txt /docs/projects/hardlink.txt
expect_ok 'Created a hard link'
vfs cat /docs/projects/hardlink.txt
contains 'Hello from the virtual file system' 'Hard link refers to the same content'
vfs mv /docs/projects/hardlink.txt /docs/projects/renamed.txt
expect_ok 'Renamed hard link'
vfs ls /docs/projects
contains 'renamed.txt' 'Renamed file appears in directory'
vfs mkdir /docs
expect_error 'Duplicate mkdir is rejected'
vfs rmdir /docs/projects
expect_error 'Non-empty directory cannot be removed'

section '5. HOST FILE IMPORT AND EXPORT'
printf 'A sample host file transferred into VFS.\n' > "$WORK/input.txt"
run "$VFS" "$IMG" put "$WORK/input.txt" /docs/projects/imported.txt
expect_ok 'Imported host file into VFS'
vfs cat /docs/projects/imported.txt
contains 'A sample host file transferred into VFS.' 'Imported content is readable'
run "$VFS" "$IMG" get /docs/projects/imported.txt "$WORK/output.txt"
expect_ok 'Exported virtual file to host filesystem'
run cmp "$WORK/input.txt" "$WORK/output.txt"
expect_ok 'Host round-trip files are byte-for-byte identical'

section '6. LARGE-FILE AND INDIRECT-BLOCK TEST'
# 100 KiB exceeds the original 10-direct-block / 40 KiB limit.
run dd if=/dev/zero of="$WORK/large.bin" bs=1024 count=100 status=none
expect_ok 'Generated 100 KiB input file'
run "$VFS" "$IMG" put "$WORK/large.bin" /docs/projects/large.bin
expect_ok 'Stored a 100 KiB file using indirect allocation'
vfs stat /docs/projects/large.bin
contains '102400 bytes' 'Stat reports expected large-file size'
run "$VFS" "$IMG" get /docs/projects/large.bin "$WORK/large-out.bin"
expect_ok 'Exported the 100 KiB file'
run cmp "$WORK/large.bin" "$WORK/large-out.bin"
expect_ok 'Large-file round trip preserved every byte'

section '7. DISK UTILIZATION AND PERFORMANCE'
show_output "$VFS" "$IMG" df
contains 'total blocks' 'Displayed filesystem capacity and allocation'
vfs stats
contains 'Block I/O' 'Displayed storage statistics and I/O metrics'
vfs fsck
contains 'fsck:' 'Executed file system consistency check'

section '8. DELETE FILES AND DIRECTORIES'
vfs rm /docs/projects/renamed.txt
expect_ok 'Removed renamed hard link'
vfs cat /docs/projects/notes.txt
contains 'Hello from the virtual file system' 'Original file survives link deletion'
vfs rm /docs/projects/notes.txt
expect_ok 'Removed original file'
vfs rm /docs/projects/imported.txt
expect_ok 'Removed imported file'
vfs rm /docs/projects/large.bin
expect_ok 'Removed large file and released its blocks'
vfs rmdir /docs/projects
expect_ok 'Removed empty projects directory'
vfs rmdir /docs
expect_ok 'Removed empty docs directory'
vfs tree /
expect_ok 'Displayed final directory tree'
show_output "$VFS" "$IMG" fsck
if [[ "$LAST_RC" -eq 0 && "$LAST_OUTPUT" == *'clean'* ]]; then
    pass 'Final fsck reports a clean file system'
else
    fail 'Final fsck reports a clean file system'
fi

printf '\n========== FINAL TEST SUMMARY ==========\n'
printf 'PASSED: %d | FAILED: %d\n' "$PASS" "$FAIL"
if (( FAIL > 0 )); then exit 1; fi
printf 'All demonstration checks passed.\n'
