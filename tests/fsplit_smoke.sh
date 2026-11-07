#!/bin/sh
set -eu
test "$#" -eq 1
elf=$(realpath "$1")
readelf -h "$elf" | grep -E 'Class:.*ELF64'
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
cp "$elf" "$temporary/fsplit"
invoke()
{
    timeout 12 bwrap --unshare-all --clearenv --ro-bind /usr /usr --ro-bind /lib /lib \
        --ro-bind /lib64 /lib64 --bind "$temporary" /test --chdir /test -- /test/fsplit "$@"
}
printf '0123456789abcdefghijX' > "$temporary/вход file"
invoke '/test/вход file' 'часть file' 8 > "$temporary/split.log"
test "$(stat -c %s "$temporary/часть file.1")" -eq 8
test "$(stat -c %s "$temporary/часть file.2")" -eq 8
test "$(stat -c %s "$temporary/часть file.3")" -eq 5
test ! -e "$temporary/часть file.4"
cat "$temporary/часть file.1" "$temporary/часть file.2" "$temporary/часть file.3" |
    cmp - "$temporary/вход file"
printf '%s\n' 'PASS explicit prefix and size, short final part, UTF-8, byte-identical reassembly'

truncate -s 1457665 "$temporary/default.bin"
printf X | dd of="$temporary/default.bin" bs=1 seek=1457664 conv=notrunc status=none
invoke default.bin > "$temporary/default.log"
test "$(stat -c %s "$temporary/default.bin.1")" -eq 1457664
test "$(cat "$temporary/default.bin.2")" = X
cat "$temporary/default.bin.1" "$temporary/default.bin.2" | cmp - "$temporary/default.bin"
printf '%s\n' 'PASS default prefix and size'

printf keep > "$temporary/existing.1"
if invoke 'вход file' existing 8 || invoke 'вход file' invalid 7x; then
    exit 1
fi
test "$(cat "$temporary/existing.1")" = keep
test ! -e "$temporary/invalid.1"
printf '%s\n' 'PASS rejects existing output and invalid size'

deadline=$(command -v timeout)
PATH=/nonexistent "$deadline" 12 "$elf" "$temporary/вход file" "$temporary/native" 32 > "$temporary/native.log"
cmp "$temporary/вход file" "$temporary/native.1"
printf '%s\n' 'PASS direct Linux run with Bubblewrap absent from PATH'
