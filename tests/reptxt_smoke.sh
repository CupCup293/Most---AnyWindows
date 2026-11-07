#!/bin/sh
set -eu
test "$#" -eq 1
elf=$(realpath "$1")
readelf -h "$elf" | grep -E 'Class:.*ELF64'
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
cp "$elf" "$temporary/reptxt"
invoke()
{
    timeout 12 bwrap --unshare-all --clearenv --ro-bind /usr /usr --ro-bind /lib /lib \
        --ro-bind /lib64 /lib64 --bind "$temporary" /test --chdir /test -- /test/reptxt "$@" > "$temporary/run.log"
}
printf 'prefix\000cat CAT cat\377' > "$temporary/файл data"
invoke '/test/файл data' cat dog
printf 'prefix\000dog CAT dog\377' | cmp - "$temporary/файл data"
printf '%s\n' 'PASS exact replacement in binary data, absolute UTF-8 path'
invoke /I 'файл data' cat fox
printf 'prefix\000dog fox dog\377' | cmp - "$temporary/файл data"
printf '%s\n' 'PASS ASCII case-insensitive matching'
invoke 'файл data' abc def
printf 'prefix\000dog fox dog\377' | cmp - "$temporary/файл data"
printf '%s\n' 'PASS no match preserves content'

mkdir "$temporary/sub"
printf cat > "$temporary/sub/one.txt"
printf cat > "$temporary/sub/two.txt"
printf cat > "$temporary/sub/.hidden.txt"
printf cat > "$temporary/sub/other.bin"
invoke '/test/sub/*.txt' cat dog
test "$(cat "$temporary/sub/one.txt")" = dog
test "$(cat "$temporary/sub/two.txt")" = dog
test "$(cat "$temporary/sub/.hidden.txt")" = cat
test "$(cat "$temporary/sub/other.bin")" = cat
invoke /A 'sub/*.txt' cat fox
test "$(cat "$temporary/sub/.hidden.txt")" = fox
test "$(cat "$temporary/sub/other.bin")" = cat
printf '%s\n' 'PASS multi-file glob and hidden-file option'

if invoke 'файл data' '' '' || invoke 'файл data' fox longer || invoke missing cat dog; then
    exit 1
fi
printf 'prefix\000dog fox dog\377' | cmp - "$temporary/файл data"
printf '%s\n' 'PASS invalid arguments and absent files leave data unchanged'

printf foo > "$temporary/prompt"
printf 'prompt\nfoo\nbar\n' | invoke
test "$(cat "$temporary/prompt")" = bar
printf '%s\n' 'PASS interactive input'

deadline=$(command -v timeout)
PATH=/nonexistent "$deadline" 12 "$elf" "$temporary/файл data" dog cat > "$temporary/native.log"
printf 'prefix\000cat fox cat\377' | cmp - "$temporary/файл data"
printf '%s\n' 'PASS direct Linux run with Bubblewrap absent from PATH'
