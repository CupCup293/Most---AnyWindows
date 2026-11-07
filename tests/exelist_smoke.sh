#!/bin/sh
set -eu
test "$#" -eq 3
elf=$(realpath "$1")
x64=$(realpath "$2")
x86=$(realpath "$3")
readelf -h "$elf" | grep -E 'Class:.*ELF64'
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
cp "$elf" "$temporary/exelist"
cp "$x64" "$temporary/первый файл.exe"
cp "$x86" "$temporary/second.exe"
cp "$x64" "$temporary/.hidden.exe"
invoke()
{
    timeout 12 bwrap --unshare-all --clearenv --ro-bind /usr /usr --ro-bind /lib /lib \
        --ro-bind /lib64 /lib64 --ro-bind "$temporary" /test --chdir /test -- /test/exelist "$@"
}
status=0
invoke '/test/первый файл.exe' second.exe > "$temporary/output" || status=$?
# EXEList returns the number of processed inputs, not a Boolean status.
test "$status" -eq 2
grep -Fx 'Machine: x86-64' "$temporary/output"
grep -Fx 'Machine: x86' "$temporary/output"
grep -Fx 'Number of sections: 4' "$temporary/output"
grep -Fx 'Number of sections: 6' "$temporary/output"
grep -Fx 'Entry point: 0x14F0' "$temporary/output"
grep -Fx 'Entry point: 0x1000' "$temporary/output"
test "$(grep -Fxc 'PE format (Windows NT executable) image.' "$temporary/output")" -eq 2
printf '%s\n' 'PASS known PE32 and PE32+ headers, absolute UTF-8 path and multiple operands'

status=0
invoke '*.exe' > "$temporary/output" || status=$?
test "$status" -eq 2
test "$(grep -c '^Module name:' "$temporary/output")" -eq 2
if grep -Fq '.hidden.exe' "$temporary/output"; then exit 1; fi
status=0
invoke -a '*.exe' > "$temporary/output" || status=$?
test "$status" -eq 3
grep -F '.hidden.exe' "$temporary/output"
printf '%s\n' 'PASS wildcard and hidden-file inclusion'

mkdir -p "$temporary/nested/deeper" "$temporary/leaf" "$temporary/.hidden-dir"
cp "$x64" "$temporary/nested/child.exe"
cp "$x86" "$temporary/nested/deeper/grandchild.exe"
cp "$x64" "$temporary/leaf/leaf.exe"
cp "$x86" "$temporary/.hidden-dir/hidden-child.exe"
ln -s .. "$temporary/nested/loop"
status=0
invoke -r '*.exe' > "$temporary/output" || status=$?
test "$status" -eq 5
grep -Fx 'Module name: Z:\test\nested\child.exe' "$temporary/output"
grep -Fx 'Module name: Z:\test\nested\deeper\grandchild.exe' "$temporary/output"
grep -Fx 'Module name: Z:\test\leaf\leaf.exe' "$temporary/output"
if grep -Fq 'hidden-child.exe' "$temporary/output"; then exit 1; fi
status=0
invoke -as '*.exe' > "$temporary/output" || status=$?
test "$status" -eq 7
grep -F 'hidden-child.exe' "$temporary/output"
printf '%s\n' 'PASS both recursive flags, leaf folders, hidden folders and directory symlink exclusion'

head -c 200 "$x64" > "$temporary/truncated.exe"
status=0
invoke truncated.exe > "$temporary/output" || status=$?
test "$status" -eq 1
if grep -Eq '^Machine:|^Entry point:' "$temporary/output"; then exit 1; fi
grep -F 'truncated.exe' "$temporary/output"
printf '%s\n' 'PASS truncated header rejected'

deadline=$(command -v timeout)
status=0
PATH=/nonexistent "$deadline" 12 "$elf" "$temporary/second.exe" > "$temporary/output" || status=$?
test "$status" -eq 1
grep -Fx 'Machine: x86' "$temporary/output"
printf '%s\n' 'PASS direct Linux run with Bubblewrap absent from PATH'
