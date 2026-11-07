#pragma once
inline constexpr auto runtimeDeclarations = R"MOST(
#include <cstdint>
#include <cstddef>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

struct region {
    uint32_t address;
    size_t length;
    uint8_t *data;
    int writable;
};
)MOST";
inline constexpr auto runtimeFunctions = R"MOST(
/* Included once, after the generated guest memory regions. */
[[noreturn]] static void guest_fault(void)
{
    static const char message[] = "Most: unsupported guest memory or CPU state\n";
    (void)write(STDERR_FILENO, message, sizeof message - 1);
    _Exit(125);
}

static uint8_t *memory(uint32_t address, uint32_t count, int writing)
{
    uint64_t end = (uint64_t)address + count;
    for (size_t i = 0; import_slots[i]; ++i) {
        if (address < (uint64_t)import_slots[i] + 4 && end > import_slots[i])
            guest_fault();
    }
    for (size_t i = 0; i < sizeof regions / sizeof regions[0]; ++i) {
        const struct region *region = &regions[i];
        if (address >= region->address && end <= (uint64_t)region->address + region->length
            && (!writing || region->writable))
            return region->data + (address - region->address);
    }
    guest_fault();
}

static uint32_t load32(uint32_t address)
{
    uint32_t value;
    memcpy(&value, memory(address, sizeof value, 0), sizeof value);
    return value;
}

static void store32(uint32_t address, uint32_t value)
{
    memcpy(memory(address, sizeof value, 1), &value, sizeof value);
}

static void push32(uint32_t *sp, uint32_t value)
{
    *sp -= 4;
    store32(*sp, value);
}

static uint32_t pop32(uint32_t *sp)
{
    uint32_t value = load32(*sp);
    *sp += 4;
    return value;
}

static uint32_t last_error;

static uint32_t win_GetLastError(uint32_t *sp)
{
    (void)sp;
    return last_error;
}

static uint32_t win_GetStdHandle(uint32_t *sp)
{
    uint32_t id = pop32(sp);
    if (id >= (uint32_t)-12 && id <= (uint32_t)-10)
        return (uint32_t)-id;
    last_error = 87; /* ERROR_INVALID_PARAMETER */
    return UINT32_MAX;
}

static uint32_t win_WriteFile(uint32_t *sp)
{
    uint32_t handle = pop32(sp);
    uint32_t buffer = pop32(sp);
    uint32_t length = pop32(sp);
    uint32_t written = pop32(sp);
    uint32_t overlapped = pop32(sp);
    if (!written || overlapped)
        guest_fault();
    store32(written, 0);
    if (handle != 11 && handle != 12) {
        last_error = 6; /* ERROR_INVALID_HANDLE */
        return 0;
    }
    if (length == 0)
        return 1;
    uint8_t *data = memory(buffer, length, 0);
    ssize_t result;
    do {
        result = write((int)handle - 10, data, length);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        last_error = (errno == EPIPE) ? 109 : 29;
        return 0;
    }
    store32(written, (uint32_t)result);
    return 1;
}
)MOST";
