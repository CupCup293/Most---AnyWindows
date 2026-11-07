#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void fail(const char *what)
{
    perror(what);
    exit(1);
}
static int checked(int result, const char *what)
{
    if (result < 0) fail(what);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    int image = checked(open(argv[1], O_RDONLY | O_CLOEXEC), "open image");
    unsigned char *memory = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) fail("guest memory");
    if (read(image, memory + 0x7c00, 512) != 512 ||
        memory[0x7dfe] != 0x55 || memory[0x7dff] != 0xaa) {
        fputs("Missing boot sector signature\n", stderr);
        return 1;
    }
    close(image);
    int kvm = checked(open("/dev/kvm", O_RDWR | O_CLOEXEC), "open KVM");
    if (ioctl(kvm, KVM_GET_API_VERSION, 0) != KVM_API_VERSION) return 1;
    int vm = checked(ioctl(kvm, KVM_CREATE_VM, 0), "create VM");
    struct kvm_userspace_memory_region region = {0, 0, 0, 1 << 20, (uintptr_t)memory};
    checked(ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region), "map guest memory");
    int cpu = checked(ioctl(vm, KVM_CREATE_VCPU, 0), "create vCPU");
    int run_size = checked(ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0), "vCPU mmap size");
    struct kvm_run *run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, cpu, 0);
    if (run == MAP_FAILED) fail("vCPU mmap");
    struct kvm_sregs segments;
    checked(ioctl(cpu, KVM_GET_SREGS, &segments), "get segments");
    segments.cs.base = 0;
    segments.cs.selector = 0;
    checked(ioctl(cpu, KVM_SET_SREGS, &segments), "set segments");
    struct kvm_regs registers = {.rip = 0x7c00, .rflags = 2};
    checked(ioctl(cpu, KVM_SET_REGS, &registers), "set registers");

    char message[128] = {0};
    size_t length = 0;
    for (unsigned step = 0; step < 128; ++step) {
        checked(ioctl(cpu, KVM_RUN, 0), "run vCPU");
        if (run->exit_reason == KVM_EXIT_HLT) {
            puts(message);
            return strcmp(message, "RUFUS-BOOT-OK") != 0;
        }
        if (run->exit_reason != KVM_EXIT_IO || run->io.direction != KVM_EXIT_IO_OUT ||
            run->io.port != 0xe9 || run->io.size != 1 || run->io.count != 1 ||
            length >= sizeof(message) - 1) {
            fprintf(stderr, "Unexpected KVM exit %u\n", run->exit_reason);
            return 1;
        }
        message[length++] = *((char *)run + run->io.data_offset);
    }
    return 1;
}
