#!/bin/sh
set -eu
test "$#" -eq 1
elf=$(realpath "$1")
readelf -h "$elf" | grep -E 'Class:.*ELF64'
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
cp "$elf" "$temporary/eltorito"
cd "$temporary"
invoke()
{
    timeout 12 bwrap --unshare-all --clearenv --ro-bind /usr /usr --ro-bind /lib /lib \
        --ro-bind /lib64 /lib64 --ro-bind "$temporary" /test --chdir /test -- /test/eltorito "$@"
}
make_iso()
{
    bsdtar -c --format iso9660 --options "iso9660:boot=boot.img,iso9660:boot-type=no-emulation,iso9660:boot-load-size=$1" -f "$2" boot.img
}
truncate -s 4096 boot.img
printf 'First boot sector' | dd of=boot.img conv=notrunc status=none
printf 'Last boot sector' | dd of=boot.img bs=1 seek=3584 conv=notrunc status=none
make_iso 8 'загрузочный образ.iso'
make_iso 4 partial.iso
invoke '/test/загрузочный образ.iso' > result.img
cmp boot.img result.img
invoke partial.iso > first-sectors.img
test "$(stat -c %s first-sectors.img)" -eq 2048
cmp -n 2048 boot.img first-sectors.img
printf '%s\n' 'PASS exact catalog-sized extraction, Unicode path and alternate sector count'

catalog=$(od -An -tu4 -j $((17 * 2048 + 71)) -N4 partial.iso)
entry=$((catalog * 2048 + 40))
image=$(od -An -tu4 -j "$entry" -N4 partial.iso)
cp partial.iso truncated.iso
truncate -s "$((image * 2048 + 511))" truncated.iso
if invoke truncated.iso > bad.img 2> errors.log; then
    exit 1
fi
test ! -s bad.img
cp partial.iso large-offset.iso
# Synthetic fixture: relocate the image above 4 GiB and update its catalog LBA.
printf '\003\000\040\000' | dd of=large-offset.iso bs=1 seek="$entry" conv=notrunc status=none
dd if=boot.img of=large-offset.iso bs=2048 count=1 seek=2097155 conv=notrunc status=none
invoke large-offset.iso > large-offset.img
cmp first-sectors.img large-offset.img
printf '%s\n' 'PASS high file offset and refusal to emit a truncated boot image'

bsdtar -c --format iso9660 -f nonboot.iso boot.img
invoke nonboot.iso > no-image 2> nonboot.log
test ! -s no-image
grep -F 'does not contain an El Torito default boot image' nonboot.log
if invoke partial.iso > /dev/full 2> write-error.log; then
    exit 1
fi
printf '%s\n' 'PASS nonbootable ISO and output write failure'

deadline=$(command -v timeout)
PATH=/nonexistent "$deadline" 12 "$elf" "$temporary/загрузочный образ.iso" > direct.img
cmp boot.img direct.img
printf '%s\n' 'PASS direct Linux run with Bubblewrap absent from PATH'
