#define _GNU_SOURCE
#include <asm/prctl.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include "entry.h"

#define WIN __attribute__((ms_abi))
#define WIN_IMPORT(symbol, dll, name)                                             \
    __attribute__((weak, noreturn)) WIN void symbol(uint64_t a, uint64_t b,        \
            uint64_t c, uint64_t d)                                               \
    {                                                                             \
        fprintf(stderr, "Missing %s!%s caller=%p args=%#lx %#lx %#lx %#lx\n",     \
                dll, name, __builtin_return_address(0), a, b, c, d);               \
        _Exit(125);                                                               \
    }
#define WIN_DATA_IMPORT(symbol, dll, name)
#include "imports.h"
#undef WIN_DATA_IMPORT
#undef WIN_IMPORT

static _Thread_local unsigned char teb[0x2000] __attribute__((aligned(4096)));
static unsigned char parameters[0x400] __attribute__((aligned(16)));
static unsigned char peb[0x1000] __attribute__((aligned(4096)));

static void trap(int sig, siginfo_t *info, void *context)
{
    ucontext_t *cpu = context;
    fprintf(stderr, "Signal %d at %#llx accessing %p\n", sig,
            (unsigned long long)cpu->uc_mcontext.gregs[REG_RIP], info->si_addr);
    _Exit(126);
}

int guest_enter_thread(void)
{
    pthread_attr_t attributes;
    if (!pthread_getattr_np(pthread_self(), &attributes)) {
        void *stack;
        size_t size;
        if (!pthread_attr_getstack(&attributes, &stack, &size)) {
            *(uintptr_t *)(teb + 0x08) = (uintptr_t)stack + size;
            *(uintptr_t *)(teb + 0x10) = (uintptr_t)stack;
        }
        pthread_attr_destroy(&attributes);
    }
    *(uintptr_t *)(teb + 0x30) = (uintptr_t)teb;
    *(uintptr_t *)(teb + 0x40) = getpid();
    *(uintptr_t *)(teb + 0x48) = syscall(SYS_gettid);
    *(uintptr_t *)(teb + 0x60) = (uintptr_t)peb;
    return syscall(SYS_arch_prctl, ARCH_SET_GS, teb) == 0;
}
uint32_t *guest_last_error(void) { return (uint32_t *)(teb + 0x68); }

extern int native_launch;
void configure_arguments(int argc, char **argv);
int launch_if_needed(int argc, char **argv);

#if defined(MOST_CHSIZE) || defined(MOST_FSPLIT) || defined(MOST_REPTXT) || defined(MOST_ELTORITO) || defined(MOST_CUT) || defined(MOST_GETHOST) || defined(MOST_EXELIST)
char **guest_narrow_argv;
#endif

int main(int argc, char **argv)
{
#if defined(MOST_CHSIZE) || defined(MOST_FSPLIT) || defined(MOST_REPTXT) || defined(MOST_ELTORITO) || defined(MOST_CUT) || defined(MOST_GETHOST) || defined(MOST_EXELIST)
    guest_narrow_argv = argv;
#endif
    int launch_status = launch_if_needed(argc, argv);
    if (launch_status >= 0)
        return launch_status;
    struct sigaction handler = {.sa_sigaction = trap, .sa_flags = SA_SIGINFO};
    sigaction(SIGSEGV, &handler, NULL);
    sigaction(SIGILL, &handler, NULL);
    sigaction(SIGTRAP, &handler, NULL);
    *(uintptr_t *)(peb + 0x10) = GUEST_BASE;
    *(uintptr_t *)(peb + 0x20) = (uintptr_t)parameters;
    *(uint32_t *)(parameters + 0x00) = sizeof(parameters);
    *(uint32_t *)(parameters + 0x04) = sizeof(parameters);
    *(uint32_t *)(parameters + 0x08) = 1; // Absolute, normalized process-parameter pointers.
    *(uint32_t *)(peb + 0x118) = 10;
    *(uint16_t *)(peb + 0x120) = 19045;
    *(uint32_t *)(peb + 0x124) = 2;
    if (!guest_enter_thread())
        return 124;
    configure_arguments(native_launch ? 1 : argc, argv);
    return ((WIN int (*)(void))GUEST_ENTRY)();
}
