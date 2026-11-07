#include "win64.hpp"
#include "pe.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSet>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>

namespace most {
namespace {
const QStringList sourceFiles{
    u"runtime.c"_s, u"launcher.c"_s, u"startup.c"_s, u"host.inc"_s, u"registry.inc"_s,
    u"resource.inc"_s, u"threads.inc"_s, u"gui.cpp"_s, u"gdi.inc"_s, u"toolbar.inc"_s,
    u"filedialog.inc"_s, u"console.inc"_s, u"reptxt.inc"_s, u"cut.inc"_s, u"winsock.inc"_s, u"exelist.inc"_s
};

QString number(quint64 value)
{
    return u"0x%1"_s.arg(value, 0, 16);
}

void writeFile(const QDir& stage, const QString& name, const QByteArray& data)
{
    QFile file(stage.filePath(name));
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.flush())
        throw Failure(u"Не удалось подготовить файл %1 для перелинковки."_s.arg(name));
}

QByteArray process(const QString& program, const QStringList& args, const QDir& stage)
{
    QProcess child;
    child.setWorkingDirectory(stage.absolutePath());
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(program, args);
    if (!child.waitForStarted())
        throw Failure(u"Не удалось запустить %1 — %2"_s.arg(program, child.errorString()));
    if (!child.waitForFinished(120000)) {
        child.kill();
        child.waitForFinished();
        throw Failure(u"%1 не завершился за 120 секунд."_s.arg(program));
    }
    auto output = child.readAll();
    if (child.exitStatus() != QProcess::NormalExit || child.exitCode())
        throw Failure(u"%1 завершился с ошибкой. %2"_s.arg(program, QString::fromLocal8Bit(output).trimmed()));
    return output;
}

struct GuestSection {
    quint64 address, size;
    QString name, flags;
    QByteArray bytes;
    unsigned protection;
};

QString importSymbol(const Import& item)
{
    const auto& name = item.name;
    static const QRegularExpression identifier(u"^[A-Za-z_][A-Za-z0-9_]*$"_s);
    static const QRegularExpression ordinal(u"^#[0-9]+$"_s);
    if (identifier.match(name).hasMatch())
        return name;
    if (ordinal.match(name).hasMatch()) {
        if (item.dll.compare(u"wsock32.dll", Qt::CaseInsensitive) == 0)
            return u"wsock32_ordinal_"_s + name.mid(1);
        return u"ordinal_"_s + name.mid(1);
    }
    throw Failure(u"Некорректное имя импорта Win64 — %1"_s.arg(name));
}
}

QString win64Profile(const QByteArray& image)
{
    if (image.size() != 5422952 && image.size() != 16368 && image.size() != 35400 && image.size() != 20040 && image.size() != 18920 && image.size() != 23624 && image.size() != 21064 && image.size() != 27112 && image.size() != 19016 && image.size() != 32328)
        return {};
    const auto hash = QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex();
    if (hash == "5cfec8255ad5a11883fafdf54ee064e44d218de6ab80b94375bc5b57b34c5325")
        return u"Rufus 4.15"_s;
    if (hash == "c87c9e8a34fc7270552f8e11239db52bea65c780e94bd4d5449796b23dd52a1a")
        return u"W32Ver x64"_s;
    if (hash == "2f25d4fc1228c9c4510be3d2d6cd7b35679ac472bd64a60ba796c9956edb2ea2")
        return u"RawCopy x64"_s;
    if (hash == "10702c8d7726b60e74194cbfff2b54304f0dfd482343485d8d31b0d7a9826691")
        return u"CHSIZE32 x64"_s;
    if (hash == "7dca26eadc50d561ae029a8a2d8e005b8424c7ddfe2fc5462a731e818a97daf3")
        return u"FSplit x64"_s;
    if (hash == "73faf7a45939719426c3c8045e94d7803e9c28c6b1b2a9cd1d6e0bf89ff4f8ff")
        return u"FileInfo x64"_s;
    if (hash == "e3f20eadf066a757ff5a1c3025b22429e1e7f4a914e8310e8fe0cb2418f88a82")
        return u"Reptxt x64"_s;
    if (hash == "4b4c5d015bdeef7d11f85fee6a496b351abad6cb59d8f23895ef779c45401a68")
        return u"GetElTorito x64"_s;
    if (hash == "89935f1b5c5542792238930154f7600876ddfb77a7fb4804b853de45e9c4dbd7")
        return u"Cut x64"_s;
    if (hash == "ed4494b868914e9a1bcd108c1332fc89fdb0245bdd7279f8a4686bfa408bfe3c")
        return u"GetHost x64"_s;
    if (hash == "3eb739f9aa4b3dda874373241e361bcabb97e9dbfd926587e75bbb3669888ea9")
        return u"EXEList x64"_s;
    return {};
}

void linkWin64(const QByteArray& image, const QString& directory, const QString& result,
               const std::function<void(const QString&)>& log)
{
    const auto profile = win64Profile(image);
    if (profile.isEmpty())
        throw Failure(u"Для этого образа нет профиля перелинковки Win64."_s);
    const bool w32ver = profile == u"W32Ver x64";
    const bool rawcopy = profile == u"RawCopy x64";
    const bool chsize = profile == u"CHSIZE32 x64";
    const bool fsplit = profile == u"FSplit x64";
    const bool fileinfo = profile == u"FileInfo x64";
    const bool reptxt = profile == u"Reptxt x64";
    const bool eltorito = profile == u"GetElTorito x64";
    const bool cut = profile == u"Cut x64";
    const bool gethost = profile == u"GetHost x64";
    const bool exelist = profile == u"EXEList x64";
    const bool console = rawcopy || chsize || fsplit || fileinfo || reptxt || eltorito || cut || gethost || exelist;
    const auto gcc = QStandardPaths::findExecutable(u"gcc"_s);
    const auto pkg = QStandardPaths::findExecutable(u"pkg-config"_s);
    if (gcc.isEmpty() || pkg.isEmpty())
        throw Failure(u"Для сборки Win64 нужны GCC и pkg-config."_s);

    const QDir stage(directory);
    const QDir runtime(QCoreApplication::applicationDirPath() + u"/runtime64"_s);
    QString cDefinitions;
    for (const auto& name : sourceFiles) {
        QFile file(runtime.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            throw Failure(u"В комплекте отсутствует файл совместимости %1."_s.arg(name));
        const auto bytes = file.readAll();
        writeFile(stage, name, bytes);
        if ((name.endsWith(u".c") || name.endsWith(u".inc")) &&
            (name != u"console.inc" || console) && (name != u"reptxt.inc" || reptxt || exelist) && (name != u"cut.inc" || cut) &&
            (name != u"winsock.inc" || gethost) && (name != u"exelist.inc" || exelist))
            cDefinitions += QString::fromUtf8(bytes) + u'\n';
    }

    const Pe pe(image);
    QList<Import> imports;
    pe.readImports(false, imports);
    pe.readImports(true, imports);
    std::sort(imports.begin(), imports.end(), [](const Import& a, const Import& b) { return a.slot < b.slot; });

    QMap<QString, QString> symbols;
    QMap<quint64, QString> slotsByAddress;
    QSet<QString> modules;
    if (w32ver)
        modules.insert(u"ntdll.dll"_s); // W32Ver obtains RtlGetVersion dynamically.
    static const QRegularExpression library(u"^[A-Za-z0-9._-]+$"_s);
    for (const auto& item : imports) {
        if (!library.match(item.dll).hasMatch() || slotsByAddress.contains(item.slot))
            throw Failure(u"Некорректная таблица импорта Win64."_s);
        auto symbol = importSymbol(item);
        if (symbols.contains(symbol) && symbols.value(symbol).compare(item.dll, Qt::CaseInsensitive))
            throw Failure(u"Неоднозначный импорт Win64 — %1"_s.arg(symbol));
        symbols.insert(symbol, item.dll);
        modules.insert(item.dll);
        slotsByAddress.insert(item.slot, symbol);
    }

    QString weakImports, declarations, moduleNames;
    for (const auto& item : imports) {
        const auto symbol = importSymbol(item);
        if (!symbols.contains(symbol))
            continue;
        symbols.remove(symbol);
        const bool data = item.dll.compare(u"msvcrt.dll", Qt::CaseInsensitive) == 0 &&
            ((console && symbol == u"_iob") ||
             ((chsize || fsplit || reptxt || eltorito || cut || gethost || exelist) && (symbol == u"_fmode" || symbol == u"_commode" || symbol == u"__initenv")) ||
             (cut && symbol == u"__argv"));
        const auto macro = data ? u"WIN_DATA_IMPORT"_s : u"WIN_IMPORT"_s;
        weakImports += u"%1(win_%2, \"%3\", \"%4\")\n"_s.arg(
            macro, symbol, item.dll, item.name.startsWith(u'#') ? symbol : item.name);
        const auto signature = QRegularExpression(u"(?m)^WIN [^\\n]*?\\bwin_%1\\("_s.arg(symbol));
        if (data)
            continue;
        if (symbol != u"MessageBoxW" && !cDefinitions.contains(signature))
            declarations += u"extern void win_%1(void);\n"_s.arg(symbol);
    }
    auto orderedModules = modules.values();
    orderedModules.sort();
    for (const auto& name : orderedModules)
        moduleNames += u"\"%1\",\n"_s.arg(name);
    writeFile(stage, u"imports.h"_s, weakImports.toUtf8());
    writeFile(stage, u"exports.h"_s, declarations.toUtf8());
    writeFile(stage, u"modules.h"_s, moduleNames.toUtf8());

    QList<GuestSection> sections{{pe.base, pe.headers, u".guest_meta"_s, u"a"_s, image.left(pe.headers), 2}};
    for (qsizetype i = 0; i < pe.sections.size(); ++i) {
        const auto& section = pe.sections[i];
        if (section.name == ".reloc")
            continue; // The ELF linker preserves preferred addresses for the known image.
        QString flags = u"a"_s;
        if (section.executable()) flags += u'x';
        if (section.writable()) flags += u'w';
        if (quint64(section.raw) + section.rawSize > quint64(image.size()) ||
            quint64(section.rva) + qMax(section.size, section.rawSize) > pe.imageSize)
            throw Failure(u"Секция Win64 выходит за границы образа."_s);
        sections.append({pe.base + section.rva, qMax(section.size, section.rawSize),
                         u".guest_%1"_s.arg(i), flags, image.mid(section.raw, section.rawSize),
                         section.executable() ? 0x20u : section.writable() ? 4u : 2u});
    }

    QString assembly, pages;
    QStringList linkArgs{u"-O2"_s, u"-fPIC"_s, u"-no-pie"_s, u"-Wl,-z,noexecstack"_s};
    if (pe.base == 0x400000)
        linkArgs << u"-Wl,-Ttext-segment=0x600000"_s;
    qsizetype patched = 0;
    for (const auto& section : sections) {
        const auto blob = section.name + u".bin"_s;
        writeFile(stage, blob, section.bytes);
        assembly += u".section %1,\"%2\",@progbits\n"_s.arg(section.name, section.flags);
        quint64 cursor = 0;
        const auto end = section.address + section.bytes.size();
        for (auto slot = slotsByAddress.lowerBound(section.address); slot != slotsByAddress.cend() && slot.key() < end; ++slot) {
            const auto offset = slot.key() - section.address;
            if (offset < cursor || slot.key() + 8 > end)
                throw Failure(u"Слот IAT выходит за границы секции."_s);
            if (offset > cursor)
                assembly += u".incbin \"%1\", %2, %3\n"_s.arg(blob).arg(cursor).arg(offset - cursor);
            assembly += u".quad win_%1\n"_s.arg(slot.value());
            cursor = offset + 8;
            ++patched;
        }
        if (cursor < quint64(section.bytes.size()))
            assembly += u".incbin \"%1\", %2, %3\n"_s.arg(blob).arg(cursor).arg(section.bytes.size() - cursor);
        if (section.size > quint64(section.bytes.size()))
            assembly += u".zero %1\n"_s.arg(section.size - section.bytes.size());
        pages += u"{%1ull, %2u, %3u},\n"_s.arg(number(section.address), number(section.size)).arg(section.protection);
        linkArgs << u"-Wl,--section-start=%1=%2"_s.arg(section.name, number(section.address));
    }
    if (patched != imports.size())
        throw Failure(u"Перелинковка не обработала все слоты IAT."_s);
    writeFile(stage, u"guest.S"_s, assembly.toUtf8());
    writeFile(stage, u"pages.h"_s, pages.toUtf8());
    const auto resource = pe.directories[2];
    writeFile(stage, u"entry.h"_s, u"#define GUEST_ENTRY %1ull\n#define GUEST_BASE %2ull\n"
        u"#define GUEST_SIZE %3u\n#define GUEST_RSRC %4ull\n#define GUEST_RSRC_SIZE %5u\n"_s.arg(
            number(pe.base + pe.entry), number(pe.base), number(pe.imageSize),
            number(pe.base + resource.rva), number(resource.size)).toUtf8());

    if (w32ver)
        linkArgs << u"-DMOST_W32VER"_s;
    if (rawcopy)
        linkArgs << u"-DMOST_RAWCOPY"_s;
    if (chsize)
        linkArgs << u"-DMOST_CHSIZE"_s;
    if (fsplit)
        linkArgs << u"-DMOST_FSPLIT"_s;
    if (fileinfo)
        linkArgs << u"-DMOST_FILEINFO"_s;
    if (reptxt)
        linkArgs << u"-DMOST_REPTXT"_s;
    if (eltorito)
        linkArgs << u"-DMOST_ELTORITO"_s;
    if (cut)
        linkArgs << u"-DMOST_CUT"_s;
    if (gethost)
        linkArgs << u"-DMOST_GETHOST"_s;
    if (exelist)
        linkArgs << u"-DMOST_EXELIST"_s;
    log(u"Перелинковка %1 — %2 привязанных слотов импорта."_s.arg(profile).arg(patched));
    QStringList packages{u"--cflags"_s, u"--libs"_s, u"Qt6Widgets"_s};
    if (profile == u"Rufus 4.15") {
        linkArgs << u"-DMOST_RUFUS"_s;
        packages << u"libsystemd"_s;
    }
    const auto qt = process(pkg, packages, stage);
    linkArgs << u"guest.S"_s << u"runtime.c"_s << u"launcher.c"_s << u"startup.c"_s << u"gui.cpp"_s
        << QProcess::splitCommand(QString::fromLocal8Bit(qt)) << u"-lstdc++"_s << u"-o"_s << result;
    const auto messages = process(gcc, linkArgs, stage);
    if (!messages.trimmed().isEmpty())
        log(QString::fromLocal8Bit(messages).trimmed());
}
}
