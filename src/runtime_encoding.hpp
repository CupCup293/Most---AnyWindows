#pragma once
inline constexpr auto runtimeEncoding = R"MOST(
static const char* guest_code_page(uint32_t page)
{
    switch (page) {
    case 0: case 1252: return "CP1252";
    case 1: case 437: return "CP437";
    case 65001: return "UTF-8";
    default: return nullptr;
    }
}

static bool transcode_guest(const char* from, const char* to, const uint8_t* input, size_t length,
                            const char* replacement, size_t replacement_size, size_t input_unit,
                            bool strict, std::string& converted, bool& replaced)
{
    const auto converter = iconv_open(to, from);
    if (converter == (iconv_t)-1) {
        last_error = 50;
        return false;
    }
    char* cursor = reinterpret_cast<char*>(const_cast<uint8_t*>(input));
    while (length) {
        char chunk[4096];
        char* output = chunk;
        size_t remaining = sizeof chunk;
        const auto result = iconv(converter, &cursor, &length, &output, &remaining);
        converted.append(chunk, output - chunk);
        if (result != size_t(-1) || errno == E2BIG)
            continue;
        if (strict || (errno != EILSEQ && errno != EINVAL)) {
            iconv_close(converter);
            last_error = 1113;
            return false;
        }
        size_t skipped = std::min(input_unit, length);
        if (input_unit == 2 && length >= 4) {
            uint16_t first, second;
            memcpy(&first, cursor, 2);
            memcpy(&second, cursor + 2, 2);
            if (first >= 0xd800 && first <= 0xdbff && second >= 0xdc00 && second <= 0xdfff)
                skipped = 4;
        }
        converted.append(replacement, replacement_size);
        replaced = true;
        cursor += skipped;
        length -= skipped;
    }
    iconv_close(converter);
    return true;
}

static uint32_t win_MultiByteToWideChar(uint32_t *sp)
{
    const uint32_t page = pop32(sp), flags = pop32(sp), source = pop32(sp);
    const int32_t length = int32_t(pop32(sp));
    const uint32_t output = pop32(sp);
    const int32_t capacity = int32_t(pop32(sp));
    const char* encoding = guest_code_page(page);
    if (!encoding || !source || length == 0 || length < -1 || capacity < 0 || (capacity && !output)) {
        last_error = 87;
        return 0;
    }
    if (flags & ~8u) {
        last_error = 1004;
        return 0;
    }
    uint32_t count = length;
    if (length == -1) {
        count = 1;
        while (*memory(source + count - 1, 1, 0))
            ++count;
    }
    std::string converted;
    bool replaced = false;
    if (!transcode_guest(encoding, "UTF-16LE", memory(source, count, 0), count,
                         "\xfd\xff", 2, 1, flags & 8, converted, replaced))
        return 0;
    if (capacity && size_t(capacity) < converted.size() / 2) {
        last_error = 122;
        return 0;
    }
    if (capacity)
        memcpy(memory(output, converted.size(), 1), converted.data(), converted.size());
    return converted.size() / 2;
}

static uint32_t win_WideCharToMultiByte(uint32_t *sp)
{
    const uint32_t page = pop32(sp), flags = pop32(sp), source = pop32(sp);
    const int32_t length = int32_t(pop32(sp));
    const uint32_t output = pop32(sp);
    const int32_t capacity = int32_t(pop32(sp));
    const uint32_t fallback = pop32(sp), used_default = pop32(sp);
    const char* encoding = guest_code_page(page);
    if (!encoding || !source || length == 0 || length < -1 || length > INT32_MAX / 2 ||
        capacity < 0 || (capacity && !output) || (page == 65001 && (fallback || used_default))) {
        last_error = 87;
        return 0;
    }
    if (flags & ~(page == 65001 ? 0x80u : 0x400u)) {
        last_error = 1004;
        return 0;
    }
    uint32_t units = length;
    if (length == -1) {
        units = 1;
        while (load16(source + (units - 1) * 2))
            ++units;
    }
    const char default_char = fallback ? char(*memory(fallback, 1, 0)) : '?';
    const char* replacement = page == 65001 ? "\xef\xbf\xbd" : &default_char;
    std::string converted;
    bool replaced = false;
    const auto count = units * 2;
    if (!transcode_guest("UTF-16LE", encoding, memory(source, count, 0), count,
                         replacement, page == 65001 ? 3 : 1, 2, flags & 0x80, converted, replaced))
        return 0;
    if (capacity && size_t(capacity) < converted.size()) {
        last_error = 122;
        return 0;
    }
    if (capacity)
        memcpy(memory(output, converted.size(), 1), converted.data(), converted.size());
    if (used_default)
        store32(used_default, replaced);
    return converted.size();
}

static uint32_t compare_guest_strings(uint32_t *sp, bool wide, bool ignore_case)
{
    const uint32_t first = pop32(sp), second = pop32(sp);
    if (!first || !second)
        return uint32_t((first != 0) - (second != 0));
    static const locale_t locale = [] {
        auto value = newlocale(LC_CTYPE_MASK | LC_COLLATE_MASK, "en_US.UTF-8", nullptr);
        return value ? value : newlocale(LC_CTYPE_MASK | LC_COLLATE_MASK, "C", nullptr);
    }();
    const auto convert = [&](uint32_t address, std::wstring& result) {
        uint32_t count = 0;
        if (wide) {
            while (load16(address + count))
                count += 2;
        } else {
            while (*memory(address + count, 1, 0))
                ++count;
        }
        std::string utf32;
        bool replaced = false;
        if (!transcode_guest(wide ? "UTF-16LE" : "CP1252", "UTF-32LE", memory(address, count, 0),
                             count, "\xfd\xff\0\0", 4, wide ? 2 : 1, false, utf32, replaced))
            return false;
        static_assert(sizeof(wchar_t) == 4);
        result.resize(utf32.size() / sizeof(wchar_t));
        memcpy(result.data(), utf32.data(), utf32.size());
        if (ignore_case && locale)
            for (auto& unit : result)
                unit = towlower_l(unit, locale);
        return true;
    };
    std::wstring a, b;
    if (!convert(first, a) || !convert(second, b))
        return 0;
    const int order = locale ? wcscoll_l(a.c_str(), b.c_str(), locale) : a.compare(b);
    return uint32_t((order > 0) - (order < 0));
}

static uint32_t win_lstrcmpW(uint32_t *sp) { return compare_guest_strings(sp, true, false); }
static uint32_t win_lstrcmpiW(uint32_t *sp) { return compare_guest_strings(sp, true, true); }
static uint32_t win_lstrcmpiA(uint32_t *sp) { return compare_guest_strings(sp, false, true); }

static uint32_t win_CharNextA(uint32_t *sp)
{
    const uint32_t address = pop32(sp);
    return *memory(address, 1, 0) ? address + 1 : address;
}

static uint32_t win_ExpandEnvironmentStringsW(uint32_t *sp)
{
    const uint32_t source = pop32(sp), destination = pop32(sp), capacity = pop32(sp);
    std::u16string input, result;
    if (!read_guest_wide(source, input))
        return 0;
    for (size_t i = 0; i < input.size();) {
        const auto close = input[i] == u'%' ? input.find(u'%', i + 1) : input.npos;
        if (close == input.npos) {
            result += input[i++];
            continue;
        }
        const auto found = guest_environment.find(fold_ascii(input.substr(i + 1, close - i - 1)));
        if (found == guest_environment.end())
            result.append(input, i, close - i + 1);
        else
            result += found->second;
        i = close + 1;
        if (result.size() > 32767) {
            last_error = 206;
            return 0;
        }
    }
    if (capacity > result.size()) {
        if (!destination) {
            last_error = 87;
            return 0;
        }
        store_wide(destination, result.data(), result.size());
    }
    return result.size() + 1;
}
)MOST";
