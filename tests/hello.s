.intel_syntax noprefix
.globl _start, message, written
.text
_start:
    push -11
    call DWORD PTR ["__imp__GetStdHandle"]
    push 0
    push OFFSET FLAT:written
    push message_end - message
    push OFFSET FLAT:message
    push eax
    call DWORD PTR ["__imp__WriteFile"]
    test eax, eax
    je failed
    cmp DWORD PTR [written], message_end - message
    jne failed
    push 7
    call DWORD PTR ["__imp__ExitProcess"]
failed:
    push 1
    call DWORD PTR ["__imp__ExitProcess"]

.section .rdata,"a"
message:
    .ascii "Hello from a statically recompiled PE!\n"
message_end:
.data
written:
    .long 0
