#!/bin/sh
set -eu
test "$#" -eq 1
elf=$(realpath "$1")
readelf -h "$elf" | grep -E 'Class:.*ELF64'
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
cp "$elf" "$temporary/cut"
invoke()
{
    timeout 12 bwrap --unshare-all --clearenv --setenv LC_ALL C.UTF-8 \
        --ro-bind /usr /usr --ro-bind /lib /lib --ro-bind /lib64 /lib64 \
        --ro-bind "$temporary" /test --chdir /test -- /test/cut "$@"
}
printf 'abcdef\n123456\n' | invoke -b 2-4,6 > "$temporary/output"
printf 'bcdf\n2346\n' | cmp - "$temporary/output"
printf '%s\n' 'PASS byte ranges and stdin'

printf 'abcdef\n' | invoke -c 2-4 - > "$temporary/output"
printf 'bcd\n' | cmp - "$temporary/output"
printf '%s\n' 'PASS character selection on ASCII'

printf 'aЖ🙂z\n' | invoke -b 2-5 -n > "$temporary/output"
printf 'Ж\n' | cmp - "$temporary/output"
printf 'aЖ🙂z\n' | invoke -b 2-7 -n > "$temporary/output"
printf 'Ж🙂\n' | cmp - "$temporary/output"
printf '%s\n' 'PASS UTF-8 byte selection with multibyte boundaries'

printf 'abc;def;ghi\r\nplain\r\n' | invoke -d ';' -f 2 -s > "$temporary/output"
printf 'def\n' | cmp - "$temporary/output"
printf 'abc;def;ghi\nplain\n' | invoke -d ';' -f 1,3 > "$temporary/output"
printf 'abc;ghi\nplain\n' | cmp - "$temporary/output"
printf '%s\n' 'PASS custom delimiter, selected fields, suppression and CRLF'

mkdir "$temporary/файлы"
printf 'abcdef\n' > "$temporary/файлы/part 1"
printf '123456\n' > "$temporary/файлы/part 2"
invoke -b 2-4 '/test/файлы/part 1' 'файлы/part *' > "$temporary/output"
printf 'bcd\nbcd\n234\n' | cmp - "$temporary/output"
printf '%s\n' 'PASS multiple files, absolute UTF-8 path and wildcard'

if invoke -b 0 > "$temporary/output" 2> "$temporary/error" ||
   invoke -b 1 missing > "$temporary/output" 2> "$temporary/error"; then
    exit 1
fi
grep -F 'missing' "$temporary/error"
printf '%s\n' 'PASS invalid list and missing-file errors'

deadline=$(command -v timeout)
printf 'abcd\n' | PATH=/nonexistent "$deadline" 12 "$elf" -b 2 > "$temporary/output"
printf 'b\n' | cmp - "$temporary/output"
printf '%s\n' 'PASS direct Linux run with Bubblewrap absent from PATH'
