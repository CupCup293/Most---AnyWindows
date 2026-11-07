#!/bin/sh
set -eu
test "$#" -eq 1
elf=$(realpath "$1")
readelf -h "$elf" | grep -E 'Class:.*ELF64'
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
cp "$elf" "$temporary/gethost"
cat > "$temporary/hosts" <<'HOSTS'
127.0.0.1 localhost
192.0.2.42 fixture-host example-alias
192.0.2.43 fixture-local
HOSTS
printf 'hosts: files\n' > "$temporary/nsswitch.conf"
invoke()
{
    timeout 10 bwrap --unshare-all --hostname fixture-local --clearenv \
        --ro-bind /usr /usr --ro-bind /lib /lib --ro-bind /lib64 /lib64 \
        --ro-bind "$temporary/gethost" /gethost \
        --ro-bind "$temporary/hosts" /etc/hosts \
        --ro-bind "$temporary/nsswitch.conf" /etc/nsswitch.conf -- /gethost "$@"
}
invoke example-alias > "$temporary/output"
grep -Eq '^Host:[[:space:]]+fixture-host$' "$temporary/output"
grep -Eq '^Alias:[[:space:]]+example-alias$' "$temporary/output"
grep -F '192.0.2.42' "$temporary/output"
printf '%s\n' 'PASS forward lookup by alias, canonical name and IPv4'

invoke 192.0.2.42 > "$temporary/output"
grep -Eq '^Host:[[:space:]]+fixture-host$' "$temporary/output"
grep -F '192.0.2.42' "$temporary/output"
printf '%s\n' 'PASS reverse IPv4 lookup'

invoke > "$temporary/output"
grep -Eq '^Host:[[:space:]]+fixture-local$' "$temporary/output"
grep -F '192.0.2.43' "$temporary/output"
printf '%s\n' 'PASS implicit lookup of the local hostname'

if invoke absent.invalid > "$temporary/output" 2> "$temporary/error"; then
    exit 1
fi
grep -F 'Host name lookup failed.' "$temporary/error"
printf '%s\n' 'PASS missing host fails in an isolated resolver without DNS'

deadline=$(command -v timeout)
PATH=/nonexistent "$deadline" 10 "$elf" localhost > "$temporary/output"
grep -F '127.0.0.1' "$temporary/output"
printf '%s\n' 'PASS direct Linux run with Bubblewrap absent from PATH'
