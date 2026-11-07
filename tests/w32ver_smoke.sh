#!/bin/sh
set -eu
test "$#" -eq 1
elf=$(realpath "$1")
readelf -h "$elf" | grep -E 'Class:.*ELF64'
output=$(timeout 12 bwrap --unshare-all --uid 0 --gid 0 --clearenv \
    --setenv QT_QPA_PLATFORM offscreen --setenv MOST_W32VER_SMOKE 1 --setenv HOME /scratch \
    --ro-bind-try /etc/fonts /etc/fonts --ro-bind /usr /usr --ro-bind /lib /lib --ro-bind /lib64 /lib64 \
    --ro-bind "$elf" /w32ver --tmpfs /scratch --proc /proc --chdir /scratch -- /w32ver 2>&1) ||
    { printf '%s\n' "$output"; exit 1; }
printf '%s\n' "$output"
printf '%s\n' "$output" | grep -Fq 'message [Win32 version]: Windows 10 version 10.0'
printf '%s\n' "$output" | grep -Fq 'Build 19045'
if printf '%s\n' "$output" | grep -Eq '^Missing |^Signal '; then
    exit 1
fi
printf '%s\n' 'Dialog content verified; exit status 0.'

if [ "$(id -u)" -ne 0 ]; then
    deadline=$(command -v timeout)
    output=$(QT_QPA_PLATFORM=offscreen MOST_W32VER_SMOKE=1 PATH=/nonexistent \
        "$deadline" 12 "$elf" --native 2>&1) || { printf '%s\n' "$output"; exit 1; }
    printf '%s\n' "$output" | grep -Fq 'message [Win32 version]: Windows 10 version 10.0'
    printf '%s\n' "$output" | grep -Fq 'Build 19045'
    printf '%s\n' 'Native dialog verified with Bubblewrap absent from PATH.'
fi
