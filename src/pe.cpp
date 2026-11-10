#include "pe.hpp"
#include <algorithm>

namespace most {
quint64 Pe::number(quint64 offset, int size) const
{
    if (offset + size > quint64(bytes.size()))
        throw Failure(u"PE обрывается внутри заголовка или таблицы."_s);
    quint64 result = 0;
    for (int i = 0; i < size; ++i)
        result |= quint64(quint8(bytes[qsizetype(offset) + i])) << (i * 8);
    return result;
}

Pe::Pe(QByteArray input) : bytes(std::move(input))
{
    if (!bytes.startsWith("MZ"))
        throw Failure(u"Отсутствует заголовок MZ."_s);
    const auto pe = number(0x3c, 4);
    if (number(pe, 4) != 0x4550)
        throw Failure(u"Отсутствует сигнатура PE."_s);
    machine = number(pe + 4, 2);
    const auto count = number(pe + 6, 2);
    const auto optionalSize = number(pe + 20, 2);
    characteristics = number(pe + 22, 2);
    const auto opt = pe + 24;
    magic = number(opt, 2);
    if (magic != 0x10b && magic != 0x20b)
        throw Failure(u"Неизвестный формат дополнительного заголовка PE."_s);
    const bool wide = magic == 0x20b;
    const auto fixed = wide ? 112u : 96u;
    if (optionalSize < fixed || opt + optionalSize + count * 40 > quint64(bytes.size()) || count > 96)
        throw Failure(u"Некорректный размер заголовков PE."_s);
    entry = number(opt + 16, 4);
    base = wide ? number(opt + 24, 8) : number(opt + 28, 4);
    imageSize = number(opt + 56, 4);
    headers = number(opt + 60, 4);
    subsystem = number(opt + 68, 2);
    const auto numDirs = number(opt + fixed - 4, 4);
    if (numDirs > (optionalSize - fixed) / 8 || numDirs > directories.size())
        throw Failure(u"Некорректное число каталогов PE."_s);
    for (quint64 i = 0; i < numDirs; ++i) {
        const auto at = opt + fixed + 8 * i;
        directories[i] = {quint32(number(at, 4)), quint32(number(at + 4, 4))};
    }
    for (quint64 i = 0; i < count; ++i) {
        const auto at = opt + optionalSize + 40 * i;
        auto sectionName = bytes.mid(at, 8);
        if (const auto nul = sectionName.indexOf('\0'); nul >= 0)
            sectionName.truncate(nul);
        const auto size = quint32(number(at + 8, 4));
        const auto rawSize = quint32(number(at + 16, 4));
        sections.append({sectionName, quint32(number(at + 12, 4)), size ? size : rawSize,
                         quint32(number(at + 20, 4)), rawSize, quint32(number(at + 36, 4))});
    }
}

std::optional<quint64> Pe::fileOffset(quint64 rva, quint64 size) const
{
    if (rva < headers && rva + size <= headers && rva + size <= quint64(bytes.size()))
        return rva;
    for (const auto& section : sections) {
        const auto extent = std::min(section.size, section.rawSize);
        if (rva >= section.rva && rva - section.rva + size <= extent) {
            const auto offset = section.raw + (rva - section.rva);
            if (offset + size <= quint64(bytes.size()))
                return offset;
        }
    }
    return std::nullopt;
}

quint64 Pe::mapped(quint64 rva, quint64 size) const
{
    if (auto at = fileOffset(rva, size))
        return *at;
    throw Failure(u"Адрес таблицы импортов выходит за пределы данных PE."_s);
}

QString Pe::name(quint64 rva) const
{
    QByteArray text;
    for (int i = 0; i < 512; ++i) {
        const char ch = bytes[qsizetype(mapped(rva + i, 1))];
        if (ch == '\0' && !text.isEmpty())
            return QString::fromLatin1(text);
        if (ch < 32 || ch > 126)
            break;
        text.append(ch);
    }
    throw Failure(u"Недопустимое имя в таблице импортов."_s);
}

void Pe::readImports(bool delayed, QList<Import>& result) const
{
    const auto directory = directories[delayed ? 13 : 1];
    if (!directory)
        return;
    const int stride = delayed ? 32 : 20;
    const int width = magic == 0x20b ? 8 : 4;
    const quint64 ordinalFlag = quint64(1) << (width * 8 - 1);
    for (quint64 pos = 0; pos + stride <= directory.size && pos / stride < 1024; pos += stride) {
        const auto at = mapped(quint64(directory.rva) + pos, stride);
        if (std::all_of(bytes.cbegin() + at, bytes.cbegin() + at + stride,
                        [](char ch) { return ch == 0; }))
            return;
        if (delayed && number(at, 4) != 1)
            throw Failure(u"Отложенные импорты используют неподдерживаемый формат адресов."_s);
        const auto dll = name(number(at + (delayed ? 4 : 12), 4));
        const auto iat = number(at + (delayed ? 12 : 16), 4);
        auto lookup = number(at + (delayed ? 16 : 0), 4);
        if (!iat)
            throw Failure(u"В таблице импортов отсутствует IAT."_s);
        if (!lookup)
            lookup = iat;
        for (quint64 index = 0; ; ++index) {
            if (result.size() >= 8192)
                throw Failure(u"Превышен предел в 8192 импорта."_s);
            const auto item = number(mapped(lookup + index * width, width), width);
            if (!item)
                break;
            if (!(item & ordinalFlag) && item > 0xffffffff)
                throw Failure(u"Некорректный адрес имени импорта."_s);
            const auto symbol = item & ordinalFlag
                ? u"#%1"_s.arg(item & 0xffff) : name(item + 2);
            result.append({dll, symbol, base + iat + index * width, delayed});
        }
    }
    throw Failure(u"Таблица импортов не завершена нулевым дескриптором."_s);
}

unsigned Pe::countResources() const
{
    const auto directory = directories[2];
    const auto root = fileOffset(directory.rva, directory.size);
    if (!root || !directory.rva || quint64(directory.rva) + directory.size > imageSize)
        throw Failure(u"RESOURCE — каталог выходит за пределы образа."_s);
    const auto at = [&](quint64 offset, quint64 size) {
        if (offset + size > directory.size)
            throw Failure(u"RESOURCE — запись выходит за пределы каталога."_s);
        return *root + offset;
    };

    constexpr quint64 subdirectory = 0x80000000;
    unsigned visited = 0;
    const auto countLeaves = [&](auto&& walk, quint64 offset, unsigned level) -> unsigned {
        const auto table = at(offset, 16);
        const auto entries = number(table + 12, 2) + number(table + 14, 2);
        if (entries > 8192 - visited)
            throw Failure(u"RESOURCE — превышен предел в 8192 записи."_s);
        visited += entries;
        const auto first = at(offset + 16, entries * 8);
        unsigned leaves = 0;
        for (quint64 index = 0; index < entries; ++index) {
            const auto entry = first + index * 8;
            const auto name = number(entry, 4);
            if (name & subdirectory) {
                const auto offset = name & ~subdirectory;
                const auto chars = number(at(offset, 2), 2);
                at(offset + 2, chars * 2);
            }
            const auto target = number(entry + 4, 4);
            if (level < 2 && (target & subdirectory)) {
                leaves += walk(walk, target & ~subdirectory, level + 1);
            } else if (level == 2 && !(target & subdirectory)) {
                const auto data = at(target, 16);
                const auto rva = number(data, 4), size = number(data + 4, 4);
                if (size && (!fileOffset(rva, size) || rva + size > imageSize))
                    throw Failure(u"RESOURCE — данные ресурса выходят за пределы образа."_s);
                ++leaves;
            } else {
                throw Failure(u"RESOURCE — некорректный уровень дерева ресурсов."_s);
            }
        }
        return leaves;
    };
    return countLeaves(countLeaves, 0, 0);
}

unsigned Pe::countCertificates() const
{
    const auto directory = directories[4];
    // Unlike other data directories, the certificate table uses a file offset.
    const quint64 start = directory.rva, end = start + directory.size;
    if (!start || start < headers || start % 8 || !directory.size || end > quint64(bytes.size()) ||
        std::any_of(sections.cbegin(), sections.cend(), [&](const Section& section) {
            return section.rawSize && section.raw < end && start < quint64(section.raw) + section.rawSize;
        }))
        throw Failure(u"SECURITY — некорректное расположение таблицы сертификатов."_s);
    unsigned count = 0;
    for (quint64 pos = start; pos < end; ++count) {
        if (end - pos < 8)
            throw Failure(u"SECURITY — заголовок сертификата обрезан."_s);
        const auto length = number(pos, 4);
        const auto aligned = (length + 7) & ~quint64(7);
        if (length < 8 || aligned > end - pos)
            throw Failure(u"SECURITY — некорректная длина записи сертификата."_s);
        pos += aligned;
    }
    return count;
}

void Pe::validateImage() const
{
    if (base < 0x10000 || imageSize == 0
        || base + imageSize > 0x7ff00000 || headers > quint64(bytes.size()) || entry >= imageSize)
        throw Failure(u"Некорректный диапазон образа PE или точки входа."_s);
    for (qsizetype i = 0; i < sections.size(); ++i) {
        const auto& s = sections[i];
        if (!s.size)
            continue;
        const quint64 end = quint64(s.rva) + s.size;
        if (s.rva < headers || end > imageSize ||
            (s.rawSize && (s.raw < headers || quint64(s.raw) + s.rawSize > quint64(bytes.size()))))
            throw Failure(u"Секция PE выходит за пределы образа или файла."_s);
        for (qsizetype j = 0; j < i; ++j) {
            const auto& previous = sections[j];
            if (previous.size && previous.rva < end && s.rva < quint64(previous.rva) + previous.size)
                throw Failure(u"Секции PE перекрываются."_s);
        }
        if (s.executable() && s.writable())
            throw Failure(u"Секция кода допускает запись."_s);
    }
}

std::optional<quint32> Pe::dataWordAt(quint32 address) const
{
    if (address < base)
        return std::nullopt;
    const auto offset = fileOffset(quint64(address) - base, 4);
    if (!offset)
        return std::nullopt;
    return quint32(number(*offset, 4));
}

QByteArray Pe::codeAt(quint32 address) const
{
    for (const auto& section : sections) {
        const auto start = base + section.rva;
        const auto count = std::min(section.size, section.rawSize);
        if (section.executable() && address >= start && address - start < count)
            return bytes.mid(section.raw + address - start, std::min<quint64>(15, start + count - address));
    }
    return {};
}
}
