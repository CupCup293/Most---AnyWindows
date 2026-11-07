#!/bin/sh
set -eu
test "$#" -eq 1
elf=$(realpath "$1")
readelf -h "$elf" | grep -E 'Class:.*ELF64'
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
cp "$elf" "$temporary/fileinfo"
invoke()
{
    timeout 12 bwrap --unshare-all --clearenv --ro-bind /usr /usr --ro-bind /lib /lib \
        --ro-bind /lib64 /lib64 --bind "$temporary" /test --chdir /test -- /test/fileinfo "$@"
}
target="$temporary/метаданные файла"
printf abcdef > "$target"
ln "$target" "$temporary/hardlink"
chmod 0444 "$target"
touch -a -d '2024-01-02 03:04:05 UTC' "$target"
touch -m -d '2023-05-06 07:08:09 UTC' "$target"
invoke '/test/метаданные файла' hardlink . > "$temporary/output"
grep -F 'Filename: /test/метаданные файла' "$temporary/output"
grep -F 'File size: 6 bytes' "$temporary/output"
grep -F 'File attributes: Read-only' "$temporary/output"
grep -F 'File attributes: Directory' "$temporary/output"
grep -F 'Last access time: 2024-01-02 03:04:05' "$temporary/output"
grep -F 'Last write time: 2023-05-06 07:08:09' "$temporary/output"
if [ "$(stat -c %W "$target")" -gt 0 ]; then
    creation=$(date -u -d "@$(stat -c %W "$target")" '+%F %T')
    grep -F "Creation time: $creation" "$temporary/output"
fi
test "$(grep -Fxc 'Number of file system links: 2' "$temporary/output")" -ge 2
inode=$(stat -c %i "$target")
test "$(grep -Fxc "Number: $((inode & 281474976710655))" "$temporary/output")" -eq 2
printf '%s\n' 'PASS size, attributes, directory, UTC timestamps, hard-link count and identity'
invoke < "$target" > "$temporary/stdin.log"
grep -F 'stdin:' "$temporary/stdin.log"
grep -F 'File size: 6 bytes' "$temporary/stdin.log"
if invoke missing 'метаданные файла' > "$temporary/errors.out" 2> "$temporary/errors.log" ||
   invoke -1 'метаданные файла' 2>> "$temporary/errors.log"; then
    exit 1
fi
grep -F 'FileInfo could not open missing' "$temporary/errors.log"
grep -F 'FileInfo can inspect attributes; changing them is unavailable.' "$temporary/errors.log"
grep -F 'File size: 6 bytes' "$temporary/errors.out"
test "$(stat -c %a "$target")" = 444
test "$(cat "$target")" = abcdef
printf '%s\n' 'PASS redirected stdin, missing file and attribute-change refusal'

deadline=$(command -v timeout)
PATH=/nonexistent "$deadline" 12 "$elf" "$target" > "$temporary/native.log"
grep -Fq 'File size: 6 bytes' "$temporary/native.log"
printf '%s\n' 'PASS direct Linux run with Bubblewrap absent from PATH'
