#!/bin/sh
set -eu
test "$#" -eq 1
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1"
cd -- "$1"
as --32 -o kernel32.o "$here/kernel32.s"
ld -mi386pe --dll --kill-at --out-implib kernel32.a -o kernel32.dll kernel32.o "$here/kernel32.def"
as --32 -o hello.elf.o "$here/hello.s"
objcopy -O pe-i386 hello.elf.o hello.o
g++ -std=c++20 "$here/coff.cpp" -o fix-coff
./fix-coff hello.o
ld -mi386pe --subsystem console -e _start -o hello.exe hello.o kernel32.a
