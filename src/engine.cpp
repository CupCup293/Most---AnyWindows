#include "pe.hpp"
#include "win64.hpp"
#include "runtime.hpp"
#include <capstone/capstone.h>
#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <algorithm>
#include <filesystem>
#include <map>

namespace most {
const QString dataNote = u"Слой Win32 встроен в ELF. Нужна системная glibc. "
                         u"Наличие внешних данных по PE определить нельзя. "_s
                       + u"Файловый API Windows не поддерживается."_s;

namespace {
const QSet<QString> supported{u"GetStdHandle"_s, u"GetLastError"_s, u"WriteFile"_s, u"ExitProcess"_s};

bool supportedImport(const Import& item)
{
    return !item.delayed && item.dll.compare(u"kernel32.dll", Qt::CaseInsensitive) == 0
        && supported.contains(item.name);
}
}

QString Report::log() const
{
    QStringList lines{ready() ? u"Готово к сборке"_s : u"Сборка недоступна"_s,
                      u"Архитектура — %1"_s.arg(architecture)};
    lines << notes << problems;
    if (ready() && !hasWin64Profile())
        lines << u"Переведено инструкций — %1"_s.arg(instructions) << dataNote;
    if (imports.isEmpty()) {
        lines << u"В прочитанных таблицах импорты не обнаружены."_s;
    } else {
        lines << u""_s << u"Импорты — %1"_s.arg(imports.size());
        for (const auto& item : imports) {
            QString entry = u"  %1!%2"_s.arg(item.dll, item.name);
            if (item.delayed)
                entry += u" (отложенный)"_s;
            if (!hasWin64Profile() && !supportedImport(item))
                entry += u" — не поддерживается"_s;
            lines << entry;
        }
    }
    return lines.join(u'\n');
}

namespace {
QString hex(quint32 value) { return QStringLiteral("%1").arg(value, 8, 16, QChar(u'0')); }
QString constant(quint32 value) { return u"0x"_s + hex(value) + u"u"_s; }
QString label(quint32 value) { return u"pc_"_s + hex(value); }

struct Decoder {
    csh handle{};
    cs_insn* ins{};
    Decoder()
    {
        if (cs_open(CS_ARCH_X86, CS_MODE_32, &handle) != CS_ERR_OK)
            throw Failure(u"Не удалось открыть декодер x86."_s);
        if (cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK || !(ins = cs_malloc(handle))) {
            cs_close(&handle);
            throw Failure(u"Не удалось инициализировать декодер x86."_s);
        }
    }
    ~Decoder()
    {
        cs_free(ins, 1);
        cs_close(&handle);
    }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool decode(const QByteArray& code, quint32 pc) const
    {
        auto* bytes = reinterpret_cast<const uint8_t*>(code.constData());
        size_t size = code.size();
        uint64_t address = pc;
        return cs_disasm_iter(handle, &bytes, &size, &address, ins);
    }

    QString reg(x86_reg value) const
    {
        static const QSet<unsigned> allowed{X86_REG_EAX, X86_REG_EBX, X86_REG_ECX, X86_REG_EDX,
                                            X86_REG_ESP, X86_REG_EBP, X86_REG_ESI, X86_REG_EDI};
        if (!allowed.contains(value))
            throw Failure(u"ожидался 32-битный регистр общего назначения"_s);
        return QString::fromLatin1(cs_reg_name(handle, value));
    }

    QString address(const x86_op_mem& mem) const
    {
        if (mem.segment)
            throw Failure(u"сегментная адресация"_s);
        QString expr = constant(mem.disp);
        if (mem.base)
            expr += u" + "_s + reg(mem.base);
        if (mem.index)
            expr += u" + %1 * %2u"_s.arg(reg(mem.index)).arg(mem.scale);
        return u'(' + expr + u')';
    }

    QString operand(const cs_x86_op& op, const QString& assignment = {}) const
    {
        if (op.size != 4)
            throw Failure(u"поддерживаются только 32-битные операнды"_s);
        switch (op.type) {
        case X86_OP_REG:
            return assignment.isEmpty() ? reg(op.reg) : u"%1 = %2;"_s.arg(reg(op.reg), assignment);
        case X86_OP_MEM:
            return assignment.isEmpty() ? u"load32(%1)"_s.arg(address(op.mem))
                                        : u"store32(%1, %2);"_s.arg(address(op.mem), assignment);
        case X86_OP_IMM:
            if (assignment.isEmpty())
                return constant(op.imm);
            break;
        default:
            break;
        }
        throw Failure(u"неподдерживаемый операнд"_s);
    }

    QString lift(const QMap<quint32, QString>& imports, QList<quint32>& targets) const
    {
        const auto& x = ins->detail->x86;
        if (x.addr_size != 4 || std::any_of(std::begin(x.prefix), std::end(x.prefix),
                                           [](auto prefix) { return prefix != 0; }))
            throw Failure(u"префикс или нестандартная адресация"_s);
        const auto* a = x.operands;
        const auto count = x.op_count;
        targets = {quint32(ins->address + ins->size)};
        const auto id = ins->id;
        if (id == X86_INS_NOP && !count)
            return {};
        if (id == X86_INS_MOV && count == 2)
            return operand(a[0], operand(a[1]));
        if (id == X86_INS_LEA && count == 2 && a[1].type == X86_OP_MEM)
            return operand(a[0], address(a[1].mem));
        if (id == X86_INS_PUSH && count == 1)
            return u"push32(&esp, %1);"_s.arg(operand(a[0]));
        if (id == X86_INS_POP && count == 1 && a[0].type == X86_OP_REG)
            return u"tmp = pop32(&esp); "_s + operand(a[0], u"tmp"_s);
        static const QMap<unsigned, QChar> operators{{X86_INS_ADD, u'+'}, {X86_INS_SUB, u'-'},
            {X86_INS_CMP, u'-'}, {X86_INS_TEST, u'&'}, {X86_INS_XOR, u'^'}};
        if (operators.contains(id) && count == 2) {
            const auto assign = (id == X86_INS_CMP || id == X86_INS_TEST) ? QString()
                : operand(a[0], u"tmp"_s);
            return u"tmp = %1 %2 %3; %4 zero = (tmp == 0);"_s
                .arg(operand(a[0]), QString(operators[id]), operand(a[1]), assign);
        }
        if ((id == X86_INS_JMP || id == X86_INS_JE || id == X86_INS_JNE)
            && count == 1 && a[0].type == X86_OP_IMM) {
            const auto destination = quint32(a[0].imm);
            if (id == X86_INS_JMP) {
                targets = {destination};
                return {};
            }
            targets << destination;
            return u"if (zero < 0) guest_fault(); if (%1) goto %2;"_s
                .arg(id == X86_INS_JE ? u"zero"_s : u"!zero"_s, label(destination));
        }
        if (id == X86_INS_CALL && count == 1 && a[0].type == X86_OP_MEM) {
            const auto& mem = a[0].mem;
            const auto symbol = imports.value(quint32(mem.disp));
            if (mem.base || mem.index || mem.segment || symbol.isEmpty())
                throw Failure(u"косвенный вызов вне известного слота IAT"_s);
            if (symbol == u"ExitProcess") {
                targets.clear();
                return u"return int(load32(esp) & 255u);"_s;
            }
            return u"eax = win_%1(&esp); zero = -1;"_s.arg(symbol);
        }
        throw Failure(u"инструкция или форма операндов не поддерживается"_s);
    }
};

void translate(const Pe& pe, Report& report)
{
    pe.validateImage();
    QMap<quint32, QString> imports;
    for (const auto& item : report.imports) {
        const bool mapped = std::any_of(pe.sections.cbegin(), pe.sections.cend(), [&](const Section& s) {
            const auto start = pe.base + s.rva;
            return !s.executable() && item.slot >= start && item.slot + 4 <= start + s.size;
        });
        if (!mapped || imports.contains(item.slot))
            throw Failure(u"Некорректное расположение слота IAT."_s);
        imports.insert(item.slot, item.name);
    }

    const auto entry = quint32(pe.base + pe.entry);
    QList<quint32> pending{entry};
    QMap<quint32, QString> translated;
    QSet<quint32> occupied;
    Decoder decoder;
    while (!pending.isEmpty()) {
        const auto pc = pending.takeLast();
        if (translated.contains(pc))
            continue;
        if (translated.size() >= 20'000)
            throw Failure(u"Превышен предел в 20 000 достижимых инструкций."_s);
        if (!decoder.decode(pe.codeAt(pc), pc))
            throw Failure(u"0x%1 — нет декодируемой инструкции в секции кода."_s.arg(hex(pc)));
        for (quint32 pos = pc; pos < pc + decoder.ins->size; ++pos) {
            if (occupied.contains(pos))
                throw Failure(u"0x%1 — перекрывающиеся инструкции."_s.arg(hex(pc)));
            occupied.insert(pos);
        }
        QList<quint32> targets;
        QString code;
        try {
            code = decoder.lift(imports, targets);
        } catch (const Failure& error) {
            throw Failure(u"0x%1 — %2 %3: %4."_s.arg(hex(pc), QString::fromLatin1(decoder.ins->mnemonic),
                QString::fromLatin1(decoder.ins->op_str), QString::fromUtf8(error.what())));
        }
        if (!targets.isEmpty())
            code += u" goto "_s + label(targets.front()) + u';';
        translated.insert(pc, code);
        pending.append(targets);
    }

    QString source;
    QTextStream out(&source);
    out << runtimeDeclarations;
    QStringList regions;
    for (const auto& section : pe.sections) {
        if (!section.size || section.executable())
            continue;
        const QString name = u"data_%1"_s.arg(regions.size());
        auto data = pe.bytes.mid(section.raw, std::min(section.rawSize, section.size));
        while (!data.isEmpty() && data.back() == '\0')
            data.chop(1);
        out << "static uint8_t " << name << '[' << section.size << "] = {\n";
        for (qsizetype i = 0; i < data.size(); ++i) {
            out << unsigned(quint8(data[i])) << ',';
            if (i % 32 == 31)
                out << '\n';
        }
        out << "};\n";
        regions << u"{%1, sizeof %2, %2, %3},"_s.arg(constant(pe.base + section.rva), name)
                      .arg(int(section.writable()));
    }
    out << "static uint8_t stack[1048576];\nstatic const region regions[] = {\n"
        << regions.join(u'\n') << "\n{0x7ff00000u, sizeof stack, stack, 1}};\n"
        << "static const uint32_t import_slots[] = {";
    for (const auto slot : imports.keys())
        out << constant(slot) << ',';
    out << "0};\n" << runtimeFunctions << "\nint main() {\n"
        << "uint32_t eax=0, ebx=0, ecx=0, edx=0, esi=0, edi=0, ebp=0, tmp=0, esp=0x7ffffff0u;\n"
        << "int zero=-1;\ngoto " << label(entry) << ";\n";
    for (auto it = translated.cbegin(); it != translated.cend(); ++it)
        out << label(it.key()) << ": " << it.value() << '\n';
    out << "}\n";
    out.flush();
    report.instructions = translated.size();
    report.source = std::move(source);
}
}

namespace {
constexpr qint64 maxPeSize = 16 * 1024 * 1024;

QString architecture(const Pe& pe)
{
    const QMap<quint16, QString> architectures{{0x14c, u"x86"_s}, {0x8664, u"x86-64"_s},
                            {0xaa64, u"ARM64"_s}, {0x1c4, u"ARM"_s}};
    return architectures.value(pe.machine, u"машина 0x%1"_s.arg(pe.machine, 0, 16))
        + (pe.magic == 0x20b ? u" / PE32+"_s : u" / PE32"_s);
}

bool packed(const Pe& pe)
{
    return std::any_of(pe.sections.cbegin(), pe.sections.cend(),
                       [](const Section& section) { return section.name.startsWith("UPX"); });
}

QByteArray unpack(const QByteArray& bytes)
{
    const auto upx = QCoreApplication::applicationDirPath() + u"/tools/upx/upx"_s;
    const auto bwrap = QStandardPaths::findExecutable(u"bwrap"_s);
    const auto prlimit = QStandardPaths::findExecutable(u"prlimit"_s);
    if (!QFileInfo(upx).isExecutable() || bwrap.isEmpty() || prlimit.isEmpty())
        throw Failure(u"Для автоматической распаковки нужны UPX из комплекта, bwrap и prlimit."_s);
    QTemporaryDir work;
    if (!work.isValid())
        throw Failure(u"Не удалось создать временную папку для UPX."_s);
    QFile input(work.filePath(u"input.exe"_s));
    if (!input.open(QIODevice::WriteOnly) || input.write(bytes) != bytes.size() || !input.flush())
        throw Failure(u"Не удалось записать временную копию для UPX."_s);
    input.close();
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(prlimit, {u"--fsize=%1"_s.arg(maxPeSize), u"--as=536870912"_s, u"--"_s, bwrap,
        u"--unshare-all"_s, u"--die-with-parent"_s, u"--clearenv"_s,
        u"--ro-bind"_s, upx, u"/upx"_s, u"--bind"_s, work.path(), u"/job"_s,
        u"--ro-bind"_s, input.fileName(), u"/job/input.exe"_s,
        u"--"_s, u"/upx"_s, u"-d"_s, u"-q"_s, u"-o"_s, u"/job/unpacked.exe"_s, u"/job/input.exe"_s});
    if (!process.waitForStarted())
        throw Failure(u"Не удалось запустить UPX — %1"_s.arg(process.errorString()));
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished();
        throw Failure(u"UPX не завершил распаковку за 15 секунд."_s);
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode())
        throw Failure(u"Распаковка UPX не удалась (лимит образа 16 МиБ). %1"_s
                          .arg(QString::fromLocal8Bit(process.readAll()).trimmed().right(600)));
    QFile output(work.filePath(u"unpacked.exe"_s));
    if (!output.open(QIODevice::ReadOnly) || output.size() > maxPeSize)
        throw Failure(u"Не удалось прочитать результат UPX или превышен предел 16 МиБ."_s);
    return output.read(maxPeSize + 1);
}
}

QString defaultOutput(const QString& input)
{
    const QFileInfo file(input);
    return QDir(file.absolutePath()).filePath(file.completeBaseName() + u"-linux"_s);
}

Report analyze(const QString& path)
{
    Report report;
    try {
        QFile file(path);
        if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
            throw Failure(u"Не удалось открыть локальный файл %1."_s.arg(path));
        constexpr qint64 limit = maxPeSize;
        auto bytes = file.read(limit + 1);
        if (bytes.size() > limit)
            throw Failure(u"Размер PE превышает 16 МиБ."_s);
        Pe pe(std::move(bytes));
        report.architecture = architecture(pe);
        if (packed(pe)) {
            report.notes << u"Обнаружена упаковка UPX."_s;
            pe = Pe(unpack(pe.bytes));
            if (packed(pe))
                throw Failure(u"После распаковки остались секции UPX."_s);
            report.notes << u"UPX распакован автоматически. Анализируется распакованный PE."_s;
            report.architecture = architecture(pe);
        }
        if (const auto profile = win64Profile(pe.bytes); !profile.isEmpty()) {
            pe.readImports(false, report.imports);
            pe.readImports(true, report.imports);
            report.win64Image = pe.bytes;
            report.profile = profile;
            report.notes << u"Доступна экспериментальная перелинковка Win64 — %1."_s.arg(profile)
                         << (pe.subsystem == 3
                            ? u"Консольный запуск в Linux. Доступные операции зависят от профиля."_s
                            : u"Запуск в песочнице Wayland. Доступен отдельный режим --native."_s);
            return report;
        }
        if (pe.machine == 0x8664 && pe.magic == 0x20b)
            report.problems << u"Для этого PE32+ нет профиля перелинковки Win64."_s;
        else if (pe.machine != 0x14c || pe.magic != 0x10b)
            report.problems << u"Общий режим сборки поддерживает только x86 PE32."_s;
        if (!(pe.characteristics & 2) || (pe.characteristics & 0x2000) || pe.subsystem != 3)
            report.problems << u"Требуется консольный Windows EXE (подсистема CUI)."_s;

        for (bool delayed : {false, true}) {
            try {
                pe.readImports(delayed, report.imports);
            } catch (const Failure& error) {
                report.problems << (delayed ? u"Список отложенных импортов неполон. "_s
                                            : u"Список статических импортов неполон. "_s)
                                       + QString::fromUtf8(error.what());
            }
        }
        const char* names[] = {"EXPORT", "IMPORT", "RESOURCE", "EXCEPTION", "SECURITY", "BASERELOC",
            "DEBUG", "ARCHITECTURE", "GLOBALPTR", "TLS", "LOAD_CONFIG", "BOUND_IMPORT", "IAT",
            "DELAY_IMPORT", "CLR", "RESERVED"};
        for (unsigned i = 0; i < pe.directories.size(); ++i)
            if (pe.directories[i] && i != 1 && i != 5 && i != 6 && i != 12)
                report.problems << u"Каталог %1 не поддерживается."_s.arg(QString::fromLatin1(names[i]));
        const auto unsupported = std::count_if(report.imports.cbegin(), report.imports.cend(),
                                               [](const Import& item) { return !supportedImport(item); });
        if (unsupported) {
            const auto example = std::find_if(report.imports.cbegin(), report.imports.cend(),
                                              [](const Import& item) { return !supportedImport(item); });
            report.problems << u"Неподдерживаемых импортов — %1, например %2!%3."_s
                                   .arg(unsupported).arg(example->dll, example->name);
        }
        if (std::any_of(report.imports.cbegin(), report.imports.cend(), [](const Import& item) {
                return item.name == u"GetProcAddress" || item.name == u"LdrGetProcedureAddress";
            }))
            report.problems << u"Полный список динамических зависимостей неизвестен."_s;
        if (report.problems.isEmpty())
            translate(pe, report);
    } catch (const std::exception& error) {
        report.problems << QString::fromUtf8(error.what());
    }
    return report;
}

void build(const Report& report, const QString& output,
           const std::function<void(const QString&)>& log)
{
    if (!report.ready())
        throw Failure(u"PE не прошёл анализ."_s);
    const QFileInfo destination(output);
    QTemporaryDir staging(destination.absolutePath() + u"/.most-XXXXXX"_s);
    if (!staging.isValid())
        throw Failure(u"Не удалось создать временный каталог в папке назначения."_s);
    const auto elf = staging.filePath(u"result"_s);
    if (report.hasWin64Profile()) {
        linkWin64(report.win64Image, staging.path(), elf, log);
    } else {
        const auto compiler = QStandardPaths::findExecutable(u"g++"_s);
        if (compiler.isEmpty())
            throw Failure(u"Для создания ELF нужен системный g++ в PATH."_s);
        QFile source(staging.filePath(u"lifted.cpp"_s));
        const auto bytes = report.source.toUtf8();
        if (!source.open(QIODevice::WriteOnly) || source.write(bytes) != bytes.size() || !source.flush())
            throw Failure(u"Не удалось записать переведённый C++."_s);
        source.close();
    
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        log(u"g++ — компиляция переведённого кода и слоя Win32."_s);
        process.start(compiler, {u"-m64"_s, u"-std=c++20"_s, u"-O1"_s, u"-static-libstdc++"_s, u"-static-libgcc"_s, source.fileName(), u"-o"_s, elf});
        if (!process.waitForStarted())
            throw Failure(process.errorString());
        if (!process.waitForFinished(120000)) {
            process.kill();
            process.waitForFinished();
            throw Failure(u"Компилятор не завершил сборку за 120 секунд."_s);
        }
        if (auto messages = process.readAll(); !messages.isEmpty())
            log(QString::fromLocal8Bit(messages).trimmed());
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode())
            throw Failure(u"Компилятор завершился с ошибкой."_s);
    }
    QFile binary(elf);
    if (!binary.open(QIODevice::ReadOnly))
        throw Failure(u"Не удалось прочитать результат компиляции."_s);
    const auto header = binary.read(20);
    if (!header.startsWith(QByteArray::fromHex("7f454c460201")) ||
        header.mid(18, 2) != QByteArray::fromHex("3e00"))
        throw Failure(u"Сборка не создала ELF64 x86-64."_s);
    std::error_code error;
    std::filesystem::create_hard_link(QFile::encodeName(elf).constData(),
                                    QFile::encodeName(destination.absoluteFilePath()).constData(), error);
    if (error)
        throw Failure(u"Не удалось сохранить ELF в %1 — %2."_s
            .arg(destination.absoluteFilePath(), QString::fromStdString(error.message())));
    log(u"Собрано — %1\nЗапуск не проверен."_s.arg(destination.absoluteFilePath()));
}
}
