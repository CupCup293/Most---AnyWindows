#!/bin/sh
set -eu
test "$#" -ge 1
test "$#" -le 2
mode=${2:-}
case "$mode" in
    ''|--mbr-target|--gpt-target|--corrupt-gpt-target|--write-gpt-target) ;;
    *) exit 2 ;;
esac
elf=$(realpath "$1")
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
mkfs.fat -C -n RUFUSTEST "$temporary/source.img" 1440 >/dev/null
as --32 "$here/rufus_boot_marker.s" -o "$temporary/boot.o"
objcopy -j .text -O binary "$temporary/boot.o" "$temporary/boot.bin"
test "$(od -An -tu1 -N1 "$temporary/source.img" | tr -d ' ')" -eq 235
offset=$(od -An -tu1 -j1 -N1 "$temporary/source.img")
dd if="$temporary/boot.bin" of="$temporary/source.img" bs=1 seek=$((offset + 2)) conv=notrunc status=none
truncate -s 128M "$temporary/target.img"
control=Write
case "$mode" in
    --mbr-target)
        printf 'label: dos\nstart=2048, size=32768, type=c, bootable\n' |
            sfdisk --quiet "$temporary/target.img" ;;
    --gpt-target|--corrupt-gpt-target|--write-gpt-target)
        printf 'label: gpt\nstart=2048, size=32768, type=U, name=RUFUSTEST\n' |
            sfdisk --quiet "$temporary/target.img"
        if [ "$mode" = --corrupt-gpt-target ]; then
            # Modify the first partition's name without updating the array CRC.
            printf X | dd of="$temporary/target.img" bs=1 seek=1080 conv=notrunc status=none
        fi
        if [ "$mode" != --write-gpt-target ]; then
            sha256sum "$temporary/target.img" > "$temporary/target.sha256"
            control=
        else
            printf 'KEEP' | dd of="$temporary/target.img" bs=1 \
                seek=$((128 * 1024 * 1024 - 34 * 512)) conv=notrunc status=none
        fi ;;
esac
timeout 35 bwrap --unshare-all --uid 0 --gid 0 --clearenv \
    --setenv QT_QPA_PLATFORM offscreen --setenv HOME /scratch --setenv XDG_CACHE_HOME /scratch/cache \
    --setenv RUFUS_PROBE_NONINTERACTIVE 1 --setenv RUFUS_PROBE_STARTUP 1 \
    --setenv RUFUS_PROBE_CAPTURE /capture/window.png --setenv RUFUS_PROBE_CONTROL "$control" \
    --setenv RUFUS_PROBE_PICK sample.img --setenv RUFUS_PROBE_DEVICE 1 \
    --ro-bind-try /etc/fonts /etc/fonts --ro-bind /usr /usr --ro-bind /lib /lib --ro-bind /lib64 /lib64 \
    --ro-bind "$elf" /rufus --dir /capture --bind "$temporary" /capture --tmpfs /scratch \
    --ro-bind "$temporary/source.img" /scratch/sample.img \
    --bind "$temporary/target.img" /scratch/device.img --proc /proc --chdir /scratch -- /rufus \
    > "$temporary/rufus.log" 2>&1 || { cat "$temporary/rufus.log"; exit 1; }
if grep -Eq '^Missing |^Signal ' "$temporary/rufus.log"; then
    cat "$temporary/rufus.log"
    exit 1
fi
require_log()
{
    grep -F "$1" "$temporary/rufus.log" || { cat "$temporary/rufus.log"; exit 1; }
}
require_log 'Main window shown'
case "$mode" in
    --gpt-target)
        require_log 'Partition type: GPT, NB Partitions: 1'
        require_log 'Start Sector: 2048, Attributes:'
        sha256sum --check --status "$temporary/target.sha256"
        echo 'GPT target unchanged'
        exit 0 ;;
    --corrupt-gpt-target)
        require_log 'Unsupported or invalid primary GPT'
        if grep -Fq 'Partition type: GPT' "$temporary/rufus.log"; then exit 1; fi
        sha256sum --check --status "$temporary/target.sha256"
        echo 'Invalid GPT target unchanged'
        exit 0 ;;
    --mbr-target)
        require_log 'Partition type: MBR, NB Partitions: 1'
        require_log 'Start Sector: 2048, Boot: Yes' ;;
    --write-gpt-target)
        require_log 'Partition type: GPT, NB Partitions: 1'
        require_log 'Start Sector: 2048, Attributes:' ;;
esac
require_log 'Rufus file selected'
require_log 'Image is a bootable disk image'
require_log 'Confirmed write'
require_log 'returned to its main dialog'
require_log 'Rufus exit'
size=$(stat -c %s "$temporary/source.img")
cmp -n "$size" "$temporary/source.img" "$temporary/target.img"
printf 'Byte-for-byte match: %s bytes\n' "$size"
if [ "$mode" = --write-gpt-target ]; then
    tail -c $((33 * 512)) "$temporary/target.img" > "$temporary/gpt-tail"
    truncate -s $((33 * 512)) "$temporary/zeros"
    cmp "$temporary/gpt-tail" "$temporary/zeros"
    test "$(tail -c $((34 * 512)) "$temporary/target.img" | head -c 4)" = KEEP
    echo 'Old backup GPT erased'
fi
cc -O2 -Wall -Wextra "$here/rufus_boot_check.c" -o "$temporary/boot-check"
timeout 5 "$temporary/boot-check" "$temporary/target.img"
