#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
curl -fLsS 'https://files.pythonhosted.org/packages/15/3e/3a4f45dd4eda6fc8051f76bf3ed50ead6040d827000371211fbaaf057625/capstone-5.0.7-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl' -o "$temporary/capstone.whl"
curl -fLsS 'https://github.com/upx/upx/releases/download/v5.2.1/upx-5.2.1-amd64_linux.tar.xz' -o "$temporary/upx.tar.xz"
(
    cd "$temporary"
    sha256sum -c <<'SUMS'
b809a9654844ce0d35099121a851ddd2ab2689df1ff6687037babcedcaae6391  capstone.whl
402162aad30af47e60dbd767fb2e64ca394ace9727ba1f40283641f1d1b91657  upx.tar.xz
SUMS
)
bsdtar -xOf "$temporary/capstone.whl" capstone/lib/libcapstone.so > "$temporary/libcapstone.so.5"
tar -xOf "$temporary/upx.tar.xz" upx-5.2.1-amd64_linux/upx > "$temporary/upx"
install -m 755 "$temporary/libcapstone.so.5" lib/libcapstone.so.5
install -m 755 "$temporary/upx" tools/upx/upx
