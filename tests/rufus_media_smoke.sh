#!/bin/sh
set -eu
test "$#" -ge 1
test "$#" -le 2
mode=${2:-}
case "$mode" in
    ''|--format|--gzip|--mbr-target|--gpt-target|--corrupt-gpt-target|--write-gpt-target) ;;
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
picker=sample.img
input=$temporary/source.img
if [ "$mode" = --gzip ]; then
    picker=sample.img.gz
    input=$temporary/source.img.gz
    gzip -n -c "$temporary/source.img" > "$input"
fi
case "$mode" in
    --format)
        control=Format
        # Rufus clears the first 8 MiB of both disk and partition,
        # and the last MiB of the disk. Guard the bytes beside those ranges.
        printf 'KEEP-HEAD' | dd of="$temporary/target.img" bs=1 seek=$((9 * 1024 * 1024)) \
            conv=notrunc status=none
        printf 'KEEP-TAIL' | dd of="$temporary/target.img" bs=1 seek=$((127 * 1024 * 1024 - 9)) \
            conv=notrunc status=none ;;
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
    --setenv RUFUS_PROBE_PICK "$picker" --setenv RUFUS_PROBE_DEVICE 1 \
    --ro-bind-try /etc/fonts /etc/fonts --ro-bind /usr /usr --ro-bind /lib /lib --ro-bind /lib64 /lib64 \
    --ro-bind "$elf" /rufus --dir /capture --bind "$temporary" /capture --tmpfs /scratch \
    --ro-bind "$input" "/scratch/$picker" \
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
if [ "$mode" = --format ]; then
    require_log 'Confirmed write'
    require_log 'Format completed.'
    if grep -Fq 'Rufus modal [Error]' "$temporary/rufus.log"; then
        cat "$temporary/rufus.log"
        exit 1
    fi
    require_log 'returned to its main dialog'
    require_log 'Rufus exit'
    if ! sfdisk --verify "$temporary/target.img" ||
       ! partition=$(partx --raw --noheadings --output START,SECTORS,SCHEME,TYPE \
           "$temporary/target.img") ||
       ! read -r start sectors scheme type <<EOF
$partition
EOF
    then
        cat "$temporary/rufus.log"
        exit 1
    fi
    if [ "$partition" != '2048 260064 dos 0xc' ] ||
       ! dd if="$temporary/target.img" of="$temporary/partition.img" bs=512 \
           skip="$start" count="$sectors" status=none ||
       ! fsck.fat -vn "$temporary/partition.img" > "$temporary/fsck.log" ||
       ! grep -Fq '32 bit entries' "$temporary/fsck.log" ||
       [ "$(fatlabel "$temporary/partition.img")" != 'MOST FAT32' ]; then
        cat "$temporary/rufus.log"
        cat "$temporary/fsck.log" 2>/dev/null || :
        exit 1
    fi
    cat "$temporary/fsck.log"
    test "$(stat -c %s "$temporary/target.img")" -eq $((128 * 1024 * 1024))
    printf 'KEEP-HEAD' > "$temporary/expected"
    dd if="$temporary/target.img" of="$temporary/actual" bs=1 \
        skip=$((9 * 1024 * 1024)) count=9 status=none
    cmp "$temporary/expected" "$temporary/actual"
    printf 'KEEP-TAIL' > "$temporary/expected"
    dd if="$temporary/target.img" of="$temporary/actual" bs=1 \
        skip=$((127 * 1024 * 1024 - 9)) count=9 status=none
    cmp "$temporary/expected" "$temporary/actual"
    # The unallocated space on both sides of the partition is cleared.
    dd if="$temporary/target.img" of="$temporary/gap" bs=512 skip=1 count=$((start - 1)) status=none
    tail -c $((32 * 512)) "$temporary/target.img" >> "$temporary/gap"
    truncate -s "$(((start - 1 + 32) * 512))" "$temporary/zeros"
    cmp "$temporary/gap" "$temporary/zeros"
    echo 'FAT32 label MOST FAT32, unallocated space and guard bytes verified'

    # Supply the generated headers needed to compile the shared runtime.
    cat > "$temporary/entry.h" <<'EOF'
#define GUEST_BASE 0x140000000ull
#define GUEST_SIZE 4096u
#define GUEST_RSRC 0ull
#define GUEST_RSRC_SIZE 0u
EOF
    printf ' {GUEST_BASE, 4096, 4},\n' > "$temporary/pages.h"
    touch "$temporary/modules.h" "$temporary/exports.h" "$temporary/imports.h"
    cc -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections -I"$temporary" \
        "$here/rufus_volume_check.c" -o "$temporary/volume-check" -pthread
    truncate -s 16M "$temporary/volume.img"
    bwrap --unshare-all --clearenv --setenv RUFUS_PROBE_DEVICE 1 \
        --ro-bind /usr /usr --ro-bind /lib /lib --ro-bind /lib64 /lib64 \
        --tmpfs /scratch --bind "$temporary/volume.img" /scratch/device.img \
        --ro-bind "$temporary/volume-check" /check -- /check
    exit 0
fi
require_log 'Rufus file selected'
if [ "$mode" = --gzip ]; then
    require_log 'Image is a compressed bootable disk image'
    require_log 'Writing compressed image'
else
    require_log 'Image is a bootable disk image'
fi
require_log 'Confirmed write'
require_log 'returned to its main dialog'
require_log 'Rufus exit'
size=$(stat -c %s "$temporary/source.img")
cmp -n "$size" "$temporary/source.img" "$temporary/target.img" || { cat "$temporary/rufus.log"; exit 1; }
printf 'Byte-for-byte match: %s bytes\n' "$size"
if [ "$mode" = --write-gpt-target ]; then
    tail -c $((33 * 512)) "$temporary/target.img" > "$temporary/gpt-tail"
    truncate -s $((33 * 512)) "$temporary/zeros"
    cmp "$temporary/gpt-tail" "$temporary/zeros"
    test "$(tail -c $((34 * 512)) "$temporary/target.img" | head -c 4)" = KEEP
    echo 'Old backup GPT erased'
fi
cc -O2 -Wall -Wextra "$here/rufus_boot_check.c" -o "$temporary/boot-check"
timeout 5 "$temporary/boot-check" "$temporary/target.img" || { cat "$temporary/rufus.log"; exit 1; }
