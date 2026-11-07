#!/bin/sh
set -eu
test "$#" -eq 1
elf=$(realpath "$1")
readelf -h "$elf" | grep -E 'Class:.*ELF64'
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
cp "$elf" "$temporary/chsize"
invoke()
{
    timeout 10 bwrap --unshare-all --clearenv --ro-bind /usr /usr --ro-bind /lib /lib \
        --ro-bind /lib64 /lib64 --bind "$temporary" /test --chdir /test -- /test/chsize "$@"
}
target="$temporary/файл с пробелом.bin"
invoke 'файл с пробелом.bin' 1K
test "$(stat -c %s "$target")" -eq 1024
test "$(tr -d '\000' < "$target" | wc -c)" -eq 0
printf '%s\n' 'PASS create zero-filled file, with UTF-8 name and spaces'
printf abcdef > "$target"
invoke /test/'файл с пробелом.bin' 2K
test "$(stat -c %s "$target")" -eq 2048
test "$(head -c 6 "$target")" = abcdef
test "$(tail -c +7 "$target" | tr -d '\000' | wc -c)" -eq 0
printf '%s\n' 'PASS extend existing file, preserving data and zero-filling new bytes'
invoke 'файл с пробелом.bin' 3
test "$(cat "$target")" = abc
test "$(stat -c %s "$target")" -eq 3
printf '%s\n' 'PASS shrink existing file, preserving prefix'
invoke -s sparse 5G
test "$(stat -c %s "$temporary/sparse")" -eq 5368709120
test "$(stat -c %b "$temporary/sparse")" -lt 1024
printf '%s\n' 'PASS sparse extension beyond 4 GiB'
for size in -1 1KB 9223372036854775807T; do
    if invoke 'файл с пробелом.bin' "$size"; then exit 1; fi
    test "$(cat "$target")" = abc
done
mkfifo "$temporary/pipe"
if invoke pipe 10; then exit 1; fi
test -p "$temporary/pipe"
printf '%s\n' 'PASS reject invalid sizes and special files'
deadline=$(command -v timeout)
PATH=/nonexistent "$deadline" 10 "$elf" "$temporary/direct" 23
test "$(stat -c %s "$temporary/direct")" -eq 23
printf '%s\n' 'PASS direct Linux run with Bubblewrap absent from PATH'
