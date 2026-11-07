#!/bin/sh
set -eu
test "$#" -eq 1
elf=$(realpath "$1")
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
cd "$temporary"
# The sandbox binds only the output ELF and the test files.
cp "$elf" ./rawcopy
invoke()
{
    timeout 12 bwrap --unshare-all --clearenv --ro-bind /usr /usr --ro-bind /lib /lib \
        --ro-bind /lib64 /lib64 --bind "$temporary" /test --proc /proc --chdir /test -- /test/rawcopy "$@"
}
printf 'abcdef0123456789\n' > 'вход file'
invoke '/test/вход file' '/test/full copy'
cmp 'вход file' 'full copy'
printf '%s\n' 'PASS full copy, absolute UTF-8 paths'
invoke 3 7 'вход file' range
printf 'def0123' > expected
cmp expected range
printf '%s\n' 'PASS source range'
printf 'XXXXXXXXXXXX' > offset
invoke -o:4 6 'вход file' offset
printf 'XXXXabcdefXX' > expected
cmp expected offset
printf '%s\n' 'PASS target offset with surrounding data preserved'
cat 'вход file' | invoke > stream
cmp 'вход file' stream
printf '%s\n' 'PASS stdin/stdout pipe'
invoke -m:2K 'вход file' buffered
cmp 'вход file' buffered
printf '%s\n' 'PASS user-selected buffer size'
printf 'XXXXXXXXXXXXXXXXX' > differential
invoke -D 'вход file' differential
cmp 'вход file' differential
printf '%s\n' 'PASS differential copy, equal-size target'
truncate -s 1048576 zero
invoke -s zero sparse
cmp zero sparse
test "$(stat -c %b sparse)" -lt 2048
printf '%s\n' 'PASS sparse copy, empty target'
ln 'вход file' alias
printf 'short' > too-short
if invoke 'вход file' alias || invoke -D 'вход file' too-short || invoke missing rejected ||
   invoke -o:4 -D 'вход file' differential; then
    exit 1
fi
printf 'abcdef0123456789\n' | cmp - 'вход file'
printf 'short' | cmp - too-short
test ! -e rejected
printf '%s\n' 'PASS invalid input, alias and short differential target rejected without modifying data'
