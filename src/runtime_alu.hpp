#pragma once
inline constexpr auto runtimeAlu = R"MOST(
static void guest_string_write(uint32_t& target, uint32_t& source, uint32_t& counter,
                               uint32_t accumulator, unsigned width, bool move, bool repeat, bool backwards)
{
    const uint32_t step = backwards ? 0u - width : width;
    for (uint32_t remaining = repeat ? counter : 1; remaining; --remaining) {
        uint32_t value = accumulator;
        if (move) {
            memcpy(&value, memory(source, width, 0), width);
            source += step;
        }
        memcpy(memory(target, width, 1), &value, width);
        target += step;
        if (repeat)
            --counter;
    }
}

enum class Alu { Add, Sub, And, Xor, Or, Inc, Dec, Neg, Shl, Shr, Sar, Adc, Sbb };
struct GuestFlags {
    unsigned value = 0, known = 0;
};
static constexpr unsigned carry_flag = 1, parity_flag = 4, zero_flag = 64,
    sign_flag = 128, overflow_flag = 2048;

static bool flag(const GuestFlags& flags, unsigned bit)
{
    if (!(flags.known & bit))
        guest_fault();
    return flags.value & bit;
}

static bool guest_condition(const GuestFlags& flags, unsigned condition)
{
    bool result;
    switch (condition >> 1) {
    case 0: result = flag(flags, overflow_flag); break;
    case 1: result = flag(flags, carry_flag); break;
    case 2: result = flag(flags, zero_flag); break;
    case 3: result = flag(flags, carry_flag) || flag(flags, zero_flag); break;
    case 4: result = flag(flags, sign_flag); break;
    case 5: result = flag(flags, parity_flag); break;
    case 6: result = flag(flags, sign_flag) != flag(flags, overflow_flag); break;
    default: result = flag(flags, zero_flag) || flag(flags, sign_flag) != flag(flags, overflow_flag); break;
    }
    return result != bool(condition & 1);
}

static int64_t guest_signed(uint32_t value, unsigned width)
{
    const uint64_t limit = uint64_t(1) << width;
    value &= uint32_t(limit - 1);
    return value & (limit >> 1) ? int64_t(value) - int64_t(limit) : value;
}

static void guest_divide(GuestFlags& flags, unsigned width, uint32_t& eax, uint32_t& edx,
                         uint32_t divisor, bool signed_division)
{
    const uint64_t limit = uint64_t(1) << width, mask = limit - 1;
    divisor &= mask;
    if (!divisor)
        guest_fault();
    const uint64_t numerator = width == 8 ? eax & 0xffffu
        : ((uint64_t(edx) & mask) << width) | (eax & mask);
    uint64_t quotient, remainder;
    if (signed_division) {
        const auto sign = uint64_t(1) << (2 * width - 1);
        const __int128 full = numerator & sign
            ? __int128(numerator) - (__int128(1) << (2 * width)) : numerator;
        const auto den = guest_signed(divisor, width);
        const auto result = full / den;
        if (result < -__int128(limit / 2) || result >= limit / 2)
            guest_fault();
        quotient = uint64_t(result) & mask;
        remainder = uint64_t(full % den) & mask;
    } else {
        quotient = numerator / divisor;
        remainder = numerator % divisor;
        if (quotient > mask)
            guest_fault();
    }
    if (width == 8) {
        eax = (eax & 0xffff0000u) | quotient | (remainder << 8);
    } else {
        eax = (eax & ~uint32_t(mask)) | quotient;
        edx = (edx & ~uint32_t(mask)) | remainder;
    }
    flags = {};
}

static uint32_t guest_double_shift(GuestFlags& flags, unsigned width, uint32_t left,
                                   uint32_t right, uint32_t count, bool forward)
{
    count &= 31;
    if (!count)
        return left;
    if (count > width)
        guest_fault();
    const uint64_t mask = (uint64_t(1) << width) - 1;
    const uint32_t sign = uint32_t(1) << (width - 1);
    left &= mask;
    right &= mask;
    const uint32_t result = forward
        ? ((uint64_t(left) << count) | (uint64_t(right) >> (width - count))) & mask
        : ((uint64_t(right) << (width - count)) | (uint64_t(left) >> count)) & mask;
    const bool carry = forward ? (left >> (width - count)) & 1 : (left >> (count - 1)) & 1;
    const bool overflow = (left ^ result) & sign;
    flags.known = carry_flag | parity_flag | zero_flag | sign_flag | (count == 1 ? overflow_flag : 0);
    flags.value = (carry ? carry_flag : 0) | (overflow ? overflow_flag : 0)
        | (!result ? zero_flag : 0) | ((result & sign) ? sign_flag : 0)
        | (__builtin_parity(result & 255) ? 0 : parity_flag);
    return result;
}

static uint32_t guest_imul(GuestFlags& flags, unsigned width, uint32_t left, uint32_t right)
{
    const auto product = guest_signed(left, width) * guest_signed(right, width);
    const auto result = uint32_t(product);
    const bool overflow = product != guest_signed(result, width);
    flags = {overflow ? carry_flag | overflow_flag : 0, carry_flag | overflow_flag};
    return result;
}

static uint32_t guest_alu(GuestFlags& flags, Alu op, unsigned width, uint32_t left, uint32_t right)
{
    const uint32_t mask = uint32_t((uint64_t(1) << width) - 1), sign = uint32_t(1) << (width - 1);
    left &= mask;
    right &= mask;
    uint32_t result = 0;
    bool carry = false, overflow = false, carry_known = true, overflow_known = true;
    switch (op) {
    case Alu::Add: case Alu::Inc: case Alu::Adc: {
        const bool input = op == Alu::Adc && flag(flags, carry_flag);
        const uint64_t sum = uint64_t(left) + right + input;
        result = uint32_t(sum) & mask;
        carry = sum > mask;
        overflow = (~(left ^ right) & (left ^ result) & sign) != 0;
        break;
    }
    case Alu::Sub: case Alu::Dec: case Alu::Neg: case Alu::Sbb: {
        if (op == Alu::Neg) {
            right = left;
            left = 0;
        }
        const bool input = op == Alu::Sbb && flag(flags, carry_flag);
        result = (left - right - input) & mask;
        carry = uint64_t(left) < uint64_t(right) + input;
        overflow = ((left ^ right) & (left ^ result) & sign) != 0;
        break;
    }
    case Alu::And: result = left & right; break;
    case Alu::Xor: result = left ^ right; break;
    case Alu::Or: result = left | right; break;
    case Alu::Shl: case Alu::Shr: case Alu::Sar: {
        const auto count = right & 31;
        if (!count)
            return left;
        carry_known = count < width || op == Alu::Sar;
        overflow_known = count == 1;
        if (op == Alu::Shl) {
            result = (left << count) & mask;
            carry = count <= width && ((left >> (width - count)) & 1);
            overflow = bool(result & sign) != carry;
        } else {
            result = left >> count;
            if (op == Alu::Sar && (left & sign)) {
                const auto prefix = count >= width ? mask : mask ^ (mask >> count);
                result |= prefix;
            }
            carry = count <= width ? ((left >> (count - 1)) & 1) : bool(left & sign);
            overflow = op == Alu::Shr && (left & sign);
        }
        break;
    }
    }
    if (op == Alu::Inc || op == Alu::Dec) {
        carry = flags.value & carry_flag;
        carry_known = flags.known & carry_flag;
    }
    flags.known = parity_flag | zero_flag | sign_flag | (carry_known ? carry_flag : 0)
        | (overflow_known ? overflow_flag : 0);
    flags.value = (carry ? carry_flag : 0) | (overflow ? overflow_flag : 0)
        | (!result ? zero_flag : 0) | ((result & sign) ? sign_flag : 0)
        | (__builtin_parity(result & 255) ? 0 : parity_flag);
    return result;
}
)MOST";
