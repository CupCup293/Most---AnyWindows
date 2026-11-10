#!/bin/sh
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
most=$(realpath "${1:-$here/../most}")
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
cd "$scratch"

make_import_lib() {
    dll=$1
    shift
    {
        printf 'LIBRARY %s.dll\nEXPORTS\n' "$dll"
        printf '%s\n' "$@"
    } > "$dll.def"
    {
        printf '.text\n'
        for symbol in "$@"; do
            printf '.globl "_%s"\n"_%s": ret\n' "$symbol" "$symbol"
        done
    } > "$dll.s"
    as --32 -o "$dll.o" "$dll.s"
    ld -mi386pe --dll --kill-at --out-implib "$dll.a" -o "$dll.dll" "$dll.o" "$dll.def"
}

make_import_lib user32 RegisterClassW@4 GetClassInfoW@12 CreateWindowExW@48 SendMessageW@16 SendMessageTimeoutW@28 SetWindowLongW@12 CallWindowProcW@20 GetWindowLongW@8 SetTimer@16 PeekMessageW@20 GetMessagePos@0 DispatchMessageW@4 DestroyWindow@4 DefWindowProcW@16 PostQuitMessage@4 ExitWindowsEx@8
make_import_lib kernel32 Sleep@4 ExitProcess@4 GetLastError@0 \
    GetModuleFileNameW@12 CreateFileW@28 ReadFile@20 SetFilePointer@16 CloseHandle@4

as --32 -o app.elf.o "$here/pe32_user32.s"
objcopy -O pe-i386 app.elf.o app.o
g++ -std=c++20 "$here/coff.cpp" -o fix-coff
./fix-coff app.o
ld -mi386pe --subsystem windows -e _start -o app.exe app.o user32.a kernel32.a
printf "MST!" >> app.exe

"$most" build app.exe -o app-linux
mkdir run
mv app-linux run/
cd run
status=0
QT_QPA_PLATFORM=offscreen timeout 10 ./app-linux || status=$?
test "$status" = 7
