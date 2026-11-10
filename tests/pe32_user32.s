.intel_syntax noprefix
.globl _start
.text
_start:
    call tail_get_last_error
    test eax, eax
    jne failed
    # An ELF with an embedded overlay can read its original PE as itself.
    push 260
    push OFFSET FLAT:self_path
    push 0
    call DWORD PTR ["__imp__GetModuleFileNameW"]
    test eax, eax
    je failed
    push 0
    push 0x80
    push 3
    push 0
    push 1
    push 0x80000000
    push OFFSET FLAT:self_path
    call DWORD PTR ["__imp__CreateFileW"]
    cmp eax, -1
    je failed
    mov DWORD PTR [self_handle], eax
    push 0
    push OFFSET FLAT:self_read
    push 4
    push OFFSET FLAT:self_data
    push eax
    call DWORD PTR ["__imp__ReadFile"]
    test eax, eax
    je failed
    cmp WORD PTR [self_data], 0x5a4d
    jne failed
    push 2
    push 0
    push -4
    push DWORD PTR [self_handle]
    call DWORD PTR ["__imp__SetFilePointer"]
    cmp eax, -1
    je failed
    push 0
    push OFFSET FLAT:self_read
    push 4
    push OFFSET FLAT:self_data
    push DWORD PTR [self_handle]
    call DWORD PTR ["__imp__ReadFile"]
    test eax, eax
    je failed
    cmp DWORD PTR [self_read], 4
    jne failed
    cmp DWORD PTR [self_data], 0x2154534d
    jne failed
    push DWORD PTR [self_handle]
    call DWORD PTR ["__imp__CloseHandle"]
    test eax, eax
    je failed
    # Match the bounded dispatch form found in the installer.
    mov eax, 0x12340006
    cmp ax, 6
    jne failed
    mov ax, 7
    cmp eax, 0x12340007
    jne failed
    imul eax, eax, 28
    jno failed
    xor eax, eax
    mov edx, 0
    sete al
    cmp eax, 1
    jne failed
    mov eax, -25
    cdq
    mov ebx, 4
    idiv ebx
    cmp eax, -6
    jne failed
    cmp edx, -1
    jne failed
    mov eax, 1
    mov edx, 0x80000000
    shld eax, edx, 1
    cmp eax, 3
    jne failed
    mov ecx, 1
    cmp ecx, 1
    mov DWORD PTR [jump_marker], 42
    ja failed
    jmp DWORD PTR [jump_cases+ecx*4]
jump_bad:
    jmp failed
jump_good:
    cmp DWORD PTR [jump_marker], 42
    jne failed
    mov esi, OFFSET FLAT:string_source
    mov edi, OFFSET FLAT:string_target
    mov ecx, 2
    cld
    rep movsd
    cmp DWORD PTR [string_target], 0x11223344
    jne failed
    cmp DWORD PTR [string_target+4], 0x55667788
    jne failed
    test ecx, ecx
    jne failed

    push OFFSET FLAT:wc
    call DWORD PTR ["__imp__RegisterClassW"]
    cmp eax, 0
    jne failed
    mov DWORD PTR [wc+4], OFFSET FLAT:wnd_proc
    push OFFSET FLAT:wc
    call DWORD PTR ["__imp__RegisterClassW"]
    test eax, eax
    je failed
    mov DWORD PTR [atom], eax
    cmp eax, 49152
    jne failed
    push OFFSET FLAT:wc_out
    push DWORD PTR [atom]
    push 4194304
    call DWORD PTR ["__imp__GetClassInfoW"]
    cmp eax, 1
    jne failed
    cmp DWORD PTR [wc_out+4], OFFSET FLAT:wnd_proc
    jne failed
    push 42
    push 4194304
    push 0
    push 0
    push 100
    push 220
    push 20
    push 10
    push 268435456
    push OFFSET FLAT:caption
    push DWORD PTR [atom]
    push 0
    call DWORD PTR ["__imp__CreateWindowExW"]
    test eax, eax
    je failed
    mov DWORD PTR [win], eax
    cmp DWORD PTR [created], 42
    jne failed
    push 16
    push 25
    push 1025
    push DWORD PTR [win]
    call DWORD PTR ["__imp__SendMessageW"]
    cmp eax, 41
    jne failed
    push OFFSET FLAT:result
    push 50
    push 2
    push 2
    push 8
    push 1025
    push DWORD PTR [win]
    call DWORD PTR ["__imp__SendMessageTimeoutW"]
    cmp eax, 1
    jne failed
    cmp DWORD PTR [result], 10
    jne failed
    push OFFSET FLAT:replacement
    push -4
    push DWORD PTR [win]
    call DWORD PTR ["__imp__SetWindowLongW"]
    test eax, eax
    je failed
    mov DWORD PTR [oldproc], eax
    cmp eax, OFFSET FLAT:wnd_proc
    jne failed
    push 16
    push 25
    push 1025
    push DWORD PTR [win]
    call DWORD PTR ["__imp__SendMessageW"]
    cmp eax, 99
    jne failed
    push 13
    push 12
    push 1025
    push DWORD PTR [win]
    push DWORD PTR [oldproc]
    call DWORD PTR ["__imp__CallWindowProcW"]
    cmp eax, 25
    jne failed
    push DWORD PTR [oldproc]
    push -4
    push DWORD PTR [win]
    call DWORD PTR ["__imp__SetWindowLongW"]
    cmp eax, OFFSET FLAT:replacement
    jne failed
    push 42
    push -21
    push DWORD PTR [win]
    call DWORD PTR ["__imp__SetWindowLongW"]
    cmp eax, 0
    jne failed
    push -21
    push DWORD PTR [win]
    call DWORD PTR ["__imp__GetWindowLongW"]
    cmp eax, 42
    jne failed
    push OFFSET FLAT:timerproc
    push 10
    push 17
    push DWORD PTR [win]
    call DWORD PTR ["__imp__SetTimer"]
    cmp eax, 17
    jne failed
    push 70
    call DWORD PTR ["__imp__Sleep"]
    push 0
    push 275
    push 275
    push DWORD PTR [win]
    push OFFSET FLAT:msg
    call DWORD PTR ["__imp__PeekMessageW"]
    cmp eax, 1
    jne failed
    cmp DWORD PTR [msg+4], 275
    jne failed
    cmp DWORD PTR [msg_guard], 0x77553311
    jne failed
    cmp DWORD PTR [msg+8], 17
    jne failed
    push 1
    push 275
    push 275
    push DWORD PTR [win]
    push OFFSET FLAT:msg
    call DWORD PTR ["__imp__PeekMessageW"]
    cmp eax, 1
    jne failed
    call DWORD PTR ["__imp__GetMessagePos"]
    push OFFSET FLAT:msg
    call DWORD PTR ["__imp__DispatchMessageW"]
    cmp DWORD PTR [fired], 17
    jne failed
    push DWORD PTR [win]
    call DWORD PTR ["__imp__DestroyWindow"]
    cmp eax, 1
    jne failed
    cmp DWORD PTR [destroyed], 1
    jne failed
    push 1
    push 0
    push 0
    push 0
    push OFFSET FLAT:msg
    call DWORD PTR ["__imp__PeekMessageW"]
    cmp eax, 1
    jne failed
    cmp DWORD PTR [msg+4], 18
    jne failed
    cmp DWORD PTR [msg+8], 37
    jne failed
    push OFFSET FLAT:msg
    call DWORD PTR ["__imp__DispatchMessageW"]
    cmp eax, 0
    jne failed
    push 1
    push 0
    push 0
    push 0
    push OFFSET FLAT:msg
    call DWORD PTR ["__imp__PeekMessageW"]
    cmp eax, 0
    jne failed
    # The guest cannot reboot the host or end its login session.
    push 0
    push 2
    call DWORD PTR ["__imp__ExitWindowsEx"]
    test eax, eax
    jne failed
    call DWORD PTR ["__imp__GetLastError"]
    cmp eax, 1314
    jne failed
    push 0
    push 0
    call DWORD PTR ["__imp__ExitWindowsEx"]
    test eax, eax
    jne failed
    call DWORD PTR ["__imp__GetLastError"]
    cmp eax, 5
    jne failed
    push 7
    call DWORD PTR ["__imp__ExitProcess"]
failed:
    push 1
    call DWORD PTR ["__imp__ExitProcess"]
.globl wnd_proc, replacement, timerproc
wnd_proc:
mov eax, [esp+8]
cmp eax, 1
je created_msg
cmp eax, 2
je destroy_msg
cmp eax, 0x401
je add_msg
mov ebx, esp
    push DWORD PTR [ebx+16]
    push DWORD PTR [ebx+12]
    push DWORD PTR [ebx+8]
    push DWORD PTR [ebx+4]
    call DWORD PTR ["__imp__DefWindowProcW"]
ret 16
created_msg:
mov eax, [esp+16]
mov eax, [eax]
mov [created], eax
xor eax, eax
ret 16
destroy_msg:
mov DWORD PTR [destroyed], 1
    push 37
    call DWORD PTR ["__imp__PostQuitMessage"]
xor eax, eax
ret 16
add_msg:
mov eax, [esp+12]
mov edx, [esp+16]
call add_pair
ret 16
add_pair:
add eax, edx
ret
replacement:
mov eax, 99
ret 16
timerproc:
mov eax, [esp+12]
mov [fired], eax
xor eax, eax
ret 16
tail_get_last_error:
    jmp DWORD PTR ["__imp__GetLastError"]
    .globl jump_cases, jump_bad, jump_good
jump_cases: .long jump_bad, jump_good
.section .rdata,"a"
.globl classname
classname: .short 77, 111, 115, 116, 84, 101, 115, 116, 87, 105, 110, 100, 111, 119, 0
.globl caption
caption: .short 87, 105, 110, 100, 111, 119, 32, 112, 114, 111, 99, 101, 100, 117, 114, 101, 32, 116, 101, 115, 116, 0
.data
.globl win
win: .space 40
.globl atom
atom: .space 40
.globl created
created: .space 40
.globl destroyed
destroyed: .space 40
.globl oldproc
oldproc: .space 40
.globl result
result: .space 40
.globl fired
fired: .space 40
.globl msg, msg_guard
msg: .space 28
msg_guard: .long 0x77553311
.globl wc_out
wc_out: .space 40
.globl wc
wc: .long 0,0,0,0,0x400000,0,0,0,0,classname

.globl jump_marker, string_source, string_target
jump_marker: .long 0
string_source: .long 0x11223344, 0x55667788
string_target: .long 0, 0

.globl self_path, self_handle, self_read, self_data
self_path: .space 520
self_handle: .long 0
self_read: .long 0
self_data: .long 0
