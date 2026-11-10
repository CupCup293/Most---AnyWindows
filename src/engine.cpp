#include "pe.hpp"
#include "win64.hpp"
#include "runtime.hpp"
#include "runtime_registry.hpp"
#include "runtime_find.hpp"
#include "runtime_encoding.hpp"
#include "runtime_process.hpp"
#include "runtime_child.hpp"
#include "runtime_modules.hpp"
#include "runtime_security.hpp"
#include "runtime_profile.hpp"
#include "runtime_gui.hpp"
#include "runtime_messages.hpp"
#include "runtime_dialogs.hpp"
#include "runtime_user32_extra.hpp"
#include "runtime_paint.hpp"
#include "runtime_gdi.hpp"
#include "runtime_shell.hpp"
#include "runtime_com.hpp"
#include "runtime_threads.hpp"
#include "runtime_alu.hpp"
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
#include <QtEndian>
#include <QTextStream>
#include <algorithm>
#include <filesystem>
#include <map>

namespace most {
const QString dataNote = u"Слой Win32 встроен в ELF. Нужна системная glibc. "
                         u"Наличие внешних данных по PE определить нельзя. "_s
                       + u"Файловый API Windows ограничен локальными путями и синхронным I/O."_s;

namespace {
const QSet<QString> kernel32Imports{u"GetStdHandle"_s, u"GetLastError"_s, u"WriteFile"_s, u"ExitProcess"_s,
    u"GetCurrentProcess"_s, u"Sleep"_s, u"GetTickCount"_s, u"CompareFileTime"_s, u"MulDiv"_s,
    u"lstrlenA"_s, u"lstrlenW"_s, u"SetCurrentDirectoryW"_s, u"GetFileAttributesW"_s, u"GetFullPathNameW"_s,
    u"CreateFileW"_s, u"CloseHandle"_s, u"GetFileSize"_s, u"SetFilePointer"_s, u"ReadFile"_s,
    u"MoveFileW"_s, u"MoveFileExW"_s, u"DeleteFileW"_s, u"CreateDirectoryW"_s, u"RemoveDirectoryW"_s,
    u"SetFileAttributesW"_s, u"CopyFileW"_s,
    u"GetModuleFileNameW"_s, u"GetVersion"_s, u"SetErrorMode"_s, u"SetEnvironmentVariableW"_s,
    u"GetTempPathW"_s, u"GetDiskFreeSpaceW"_s, u"lstrcpyA"_s, u"lstrcpyW"_s, u"lstrcatW"_s, u"lstrcpynW"_s,
    u"FindFirstFileW"_s, u"FindNextFileW"_s, u"FindClose"_s, u"SetFileTime"_s,
    u"MultiByteToWideChar"_s, u"WideCharToMultiByte"_s, u"ExpandEnvironmentStringsW"_s,
    u"GlobalAlloc"_s, u"GlobalFree"_s, u"GlobalLock"_s, u"GlobalUnlock"_s,
    u"GetModuleHandleA"_s, u"GetModuleHandleW"_s, u"GetCommandLineW"_s, u"GetTempFileNameW"_s,
    u"GetShortPathNameW"_s, u"SearchPathW"_s, u"lstrcmpW"_s, u"lstrcmpiW"_s, u"lstrcmpiA"_s,
    u"GetWindowsDirectoryW"_s, u"GetSystemDirectoryW"_s, u"GetPrivateProfileStringW"_s,
    u"WritePrivateProfileStringW"_s, u"WaitForSingleObject"_s, u"CreateProcessW"_s, u"GetExitCodeProcess"_s,
    u"LoadLibraryExW"_s, u"GetProcAddress"_s, u"FreeLibrary"_s, u"CreateThread"_s};
const QSet<QString> advapi32Imports{u"RegOpenKeyExW"_s, u"RegCreateKeyExW"_s, u"RegCloseKey"_s,
    u"RegSetValueExW"_s, u"RegQueryValueExW"_s, u"RegEnumValueW"_s, u"RegEnumKeyW"_s,
    u"RegDeleteValueW"_s, u"RegDeleteKeyW"_s,
    u"OpenProcessToken"_s, u"LookupPrivilegeValueW"_s, u"AdjustTokenPrivileges"_s, u"SetFileSecurityW"_s};
const QSet<QString> ole32Imports{u"OleInitialize"_s, u"OleUninitialize"_s, u"CoTaskMemFree"_s};
const QSet<QString> widgetOleImports{u"CoCreateInstance"_s};
const QSet<QString> user32Imports{u"CharNextW"_s, u"CharPrevW"_s, u"CharNextA"_s};

const QSet<QString> widgetImports{
    u"CreateWindowExW"_s, u"DestroyWindow"_s, u"IsWindow"_s, u"IsWindowEnabled"_s,
    u"IsWindowVisible"_s, u"ShowWindow"_s, u"EnableWindow"_s, u"GetWindowRect"_s,
    u"GetClientRect"_s, u"SetWindowPos"_s, u"ScreenToClient"_s, u"GetDlgItem"_s,
    u"FindWindowExW"_s, u"SetWindowTextW"_s, u"SetDlgItemTextW"_s, u"GetDlgItemTextW"_s,
    u"CheckDlgButton"_s, u"InvalidateRect"_s, u"SetForegroundWindow"_s,
    u"GetSysColor"_s, u"GetSystemMetrics"_s, u"CreatePopupMenu"_s, u"AppendMenuW"_s,
    u"EnableMenuItem"_s, u"GetSystemMenu"_s, u"OpenClipboard"_s, u"EmptyClipboard"_s,
    u"SetClipboardData"_s, u"CloseClipboard"_s, u"GetWindowLongW"_s,
    u"RegisterClassW"_s, u"GetClassInfoW"_s, u"SetWindowLongW"_s, u"CallWindowProcW"_s,
    u"DefWindowProcW"_s, u"SendMessageW"_s, u"SendMessageTimeoutW"_s, u"PeekMessageW"_s,
    u"DispatchMessageW"_s, u"GetMessagePos"_s, u"PostQuitMessage"_s, u"SetTimer"_s,
    u"CreateDialogParamW"_s, u"DialogBoxParamW"_s, u"EndDialog"_s,
    u"SetClassLongW"_s, u"SetCursor"_s, u"LoadCursorW"_s, u"SystemParametersInfoW"_s,
    u"wsprintfA"_s, u"wsprintfW"_s, u"TrackPopupMenu"_s, u"MessageBoxIndirectW"_s,
    u"GetDC"_s, u"ReleaseDC"_s, u"BeginPaint"_s, u"EndPaint"_s, u"FillRect"_s,
    u"DrawTextW"_s, u"LoadBitmapW"_s, u"LoadImageW"_s, u"ExitWindowsEx"_s
};

const QSet<QString> gdi32Imports{u"SelectObject"_s, u"SetBkMode"_s, u"CreateFontIndirectW"_s,
    u"SetTextColor"_s, u"DeleteObject"_s, u"GetDeviceCaps"_s, u"CreateBrushIndirect"_s, u"SetBkColor"_s};
const QSet<QString> shell32Imports{u"SHGetSpecialFolderLocation"_s, u"SHGetPathFromIDListW"_s,
    u"SHBrowseForFolderW"_s, u"SHGetFileInfoW"_s, u"ShellExecuteW"_s, u"SHFileOperationW"_s};
const QSet<QString> comctl32Imports{u"ImageList_AddMasked"_s, u"#17"_s, u"ImageList_Destroy"_s,
    u"ImageList_Create"_s};

QString importSymbol(const Import& item)
{
    if (item.dll.compare(u"comctl32.dll", Qt::CaseInsensitive) == 0 && item.name == u"#17")
        return u"InitCommonControls"_s;
    return item.name;
}

bool widgetImport(const Import& item)
{
    return !item.delayed &&
        ((item.dll.compare(u"user32.dll", Qt::CaseInsensitive) == 0 && widgetImports.contains(item.name)) ||
         (item.dll.compare(u"gdi32.dll", Qt::CaseInsensitive) == 0 && gdi32Imports.contains(item.name)) ||
         (item.dll.compare(u"comctl32.dll", Qt::CaseInsensitive) == 0 && comctl32Imports.contains(item.name)) ||
         (item.dll.compare(u"shell32.dll", Qt::CaseInsensitive) == 0 && shell32Imports.contains(item.name)) ||
         (item.dll.compare(u"ole32.dll", Qt::CaseInsensitive) == 0 && widgetOleImports.contains(item.name)));
}

bool supportedImport(const Import& item)
{
    return widgetImport(item) || (!item.delayed &&
        ((item.dll.compare(u"kernel32.dll", Qt::CaseInsensitive) == 0 && kernel32Imports.contains(item.name)) ||
         (item.dll.compare(u"user32.dll", Qt::CaseInsensitive) == 0 && user32Imports.contains(item.name)) ||
         (item.dll.compare(u"advapi32.dll", Qt::CaseInsensitive) == 0 && advapi32Imports.contains(item.name)) ||
         (item.dll.compare(u"ole32.dll", Qt::CaseInsensitive) == 0 && ole32Imports.contains(item.name))));
}
}

QString buildVersionLine()
{
    return u"Версия сборки «Моста» — %1"_s.arg(QStringLiteral(MOST_BUILD_VERSION));
}

QString Report::log() const
{
    QStringList lines{buildVersionLine(), ready() ? u"Готово к сборке"_s : u"Сборка недоступна"_s,
                      u"Архитектура — %1"_s.arg(architecture)};
    lines << notes << problems;
    if (ready() && !hasWin64Profile())
        lines << u"Переведено инструкций — %1"_s.arg(instructions) << dataNote;
    if (ready() && needsWidgets)
        lines << u"Интерфейс ELF использует системную Qt 6."_s;
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
            else if (!hasWin64Profile() && widgetImport(item) && item.name == u"ExitWindowsEx")
                entry += u" — ограничено: операция отклоняется для гостевого процесса"_s;
            lines << entry;
        }
    }
    return lines.join(u'\n');
}

namespace {
QString hex(quint32 value) { return QStringLiteral("%1").arg(value, 8, 16, QChar(u'0')); }
QString constant(quint32 value) { return u"0x"_s + hex(value) + u"u"_s; }
QString label(quint32 value) { return u"pc_"_s + hex(value); }

struct JumpBound {
    unsigned index;
    quint32 maximum;
};

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
        if (op.size != 1 && op.size != 2 && op.size != 4)
            throw Failure(u"ожидался целочисленный операнд шириной 8, 16 или 32 бита"_s);
        switch (op.type) {
        case X86_OP_REG: {
            if (op.size == 4)
                return assignment.isEmpty() ? reg(op.reg) : u"%1 = %2;"_s.arg(reg(op.reg), assignment);
            static const QMap<unsigned, unsigned> parents{
                {X86_REG_AL, X86_REG_EAX}, {X86_REG_AH, X86_REG_EAX}, {X86_REG_AX, X86_REG_EAX},
                {X86_REG_BL, X86_REG_EBX}, {X86_REG_BH, X86_REG_EBX}, {X86_REG_BX, X86_REG_EBX},
                {X86_REG_CL, X86_REG_ECX}, {X86_REG_CH, X86_REG_ECX}, {X86_REG_CX, X86_REG_ECX},
                {X86_REG_DL, X86_REG_EDX}, {X86_REG_DH, X86_REG_EDX}, {X86_REG_DX, X86_REG_EDX},
                {X86_REG_SI, X86_REG_ESI}, {X86_REG_DI, X86_REG_EDI},
                {X86_REG_BP, X86_REG_EBP}, {X86_REG_SP, X86_REG_ESP}
            };
            if (!parents.contains(op.reg))
                throw Failure(u"неподдерживаемый частичный регистр"_s);
            const auto name = reg(x86_reg(parents.value(op.reg)));
            const bool high = op.reg == X86_REG_AH || op.reg == X86_REG_BH
                || op.reg == X86_REG_CH || op.reg == X86_REG_DH;
            const int shift = high ? 8 : 0;
            const quint32 mask = op.size == 1 ? 0xff : 0xffff;
            if (assignment.isEmpty())
                return u"((%1 >> %2) & %3)"_s.arg(name).arg(shift).arg(constant(mask));
            return u"%1 = (%1 & %2) | ((%3 & %4) << %5);"_s
                .arg(name, constant(~(mask << shift)), assignment, constant(mask)).arg(shift);
        }
        case X86_OP_MEM:
            return assignment.isEmpty()
                ? u"load%1(%2)"_s.arg(op.size * 8).arg(address(op.mem))
                : u"store%1(%2, %3);"_s.arg(op.size * 8).arg(address(op.mem), assignment);
        case X86_OP_IMM:
            if (assignment.isEmpty())
                return constant(op.imm);
            break;
        default:
            break;
        }
        throw Failure(u"неподдерживаемый операнд"_s);
    }

    QString lift(const QMap<quint32, QString>& imports, QList<quint32>& targets, QSet<quint32>& continuations, bool indirect, const Pe& pe,
                 const std::optional<JumpBound>& bound) const
    {
        const auto& x = ins->detail->x86;
        const auto opcode = x.opcode[0];
        if (x.addr_size == 4 && !x.prefix[1] && !x.prefix[3] &&
            (x.prefix[0] == 0 || x.prefix[0] == 0xf3) &&
            (opcode == 0xa4 || opcode == 0xa5 || opcode == 0xaa || opcode == 0xab)) {
            const int width = (opcode & 1) == 0 ? 1 : x.prefix[2] == 0x66 ? 2 : 4;
            targets = {quint32(ins->address + ins->size)};
            return u"guest_string_write(edi, esi, ecx, eax, %1, %2, %3, backwards);"_s
                .arg(width).arg(opcode <= 0xa5 ? 1 : 0).arg(x.prefix[0] == 0xf3 ? 1 : 0);
        }
        if (x.addr_size != 4 || std::any_of(std::begin(x.prefix), std::end(x.prefix),
                                           [](auto prefix) { return prefix != 0 && prefix != 0x66; }))
            throw Failure(u"префикс или нестандартная адресация"_s);
        const auto* a = x.operands;
        const auto count = x.op_count;
        targets = {quint32(ins->address + ins->size)};
        const auto id = ins->id;
        if (id == X86_INS_NOP && !count)
            return {};
        if ((id == X86_INS_CLD || id == X86_INS_STD) && !count)
            return id == X86_INS_CLD ? u"backwards = false;"_s : u"backwards = true;"_s;
        if (id == X86_INS_MOV && count == 2)
            return operand(a[0], operand(a[1]));
        if ((id == X86_INS_MOVSX || id == X86_INS_MOVZX) && count == 2) {
            const auto source = operand(a[1]);
            return operand(a[0], id == X86_INS_MOVZX ? source
                : u"uint32_t(int%1_t(%2))"_s.arg(a[1].size * 8).arg(source));
        }
        if (id == X86_INS_LEAVE && !count && !x.prefix[2])
            return u"esp = ebp; ebp = pop32(&esp);"_s;
        if (id == X86_INS_CDQ && !count)
            return u"edx = (eax & 0x80000000u) ? ~0u : 0u;"_s;
        if (id == X86_INS_LEA && count == 2 && a[1].type == X86_OP_MEM)
            return operand(a[0], address(a[1].mem));
        if (id == X86_INS_PUSH && count == 1 && a[0].size == 4)
            return u"push32(&esp, %1);"_s.arg(operand(a[0]));
        if (id == X86_INS_POP && count == 1 && a[0].type == X86_OP_REG && a[0].size == 4)
            return u"tmp = pop32(&esp); "_s + operand(a[0], u"tmp"_s);
        static const QMap<unsigned, QString> operations{
            {X86_INS_ADD, u"Add"_s}, {X86_INS_SUB, u"Sub"_s}, {X86_INS_CMP, u"Sub"_s},
            {X86_INS_TEST, u"And"_s}, {X86_INS_XOR, u"Xor"_s}, {X86_INS_OR, u"Or"_s},
            {X86_INS_AND, u"And"_s}, {X86_INS_INC, u"Inc"_s}, {X86_INS_DEC, u"Dec"_s},
            {X86_INS_NEG, u"Neg"_s}, {X86_INS_SHL, u"Shl"_s}, {X86_INS_SHR, u"Shr"_s},
            {X86_INS_SAR, u"Sar"_s}, {X86_INS_ADC, u"Adc"_s}, {X86_INS_SBB, u"Sbb"_s}
        };
        if (operations.contains(id) && (count == 1 || count == 2)) {
            const auto assign = (id == X86_INS_CMP || id == X86_INS_TEST) ? QString()
                : operand(a[0], u"tmp"_s);
            return u"tmp = guest_alu(flags, Alu::%1, %2, %3, %4); %5"_s
                .arg(operations.value(id)).arg(a[0].size * 8)
                .arg(operand(a[0]), count == 2 ? operand(a[1]) : u"1u"_s, assign);
        }
        if (opcode == 0x0f && (x.opcode[1] & 0xf0) == 0x90 && count == 1)
            return operand(a[0], u"guest_condition(flags, %1)"_s.arg(x.opcode[1] & 15));
        if ((id == X86_INS_IDIV || id == X86_INS_DIV) && count == 1)
            return u"guest_divide(flags, %1, eax, edx, %2, %3);"_s
                .arg(a[0].size * 8).arg(operand(a[0])).arg(id == X86_INS_IDIV ? 1 : 0);
        if ((id == X86_INS_SHLD || id == X86_INS_SHRD) && count == 3)
            return u"tmp = guest_double_shift(flags, %1, %2, %3, %4, %5); %6"_s
                .arg(a[0].size * 8).arg(operand(a[0]), operand(a[1]), operand(a[2]))
                .arg(id == X86_INS_SHLD ? 1 : 0).arg(operand(a[0], u"tmp"_s));
        if (id == X86_INS_IMUL && (count == 2 || count == 3)) {
            const auto width = a[0].size * 8;
            return u"tmp = guest_imul(flags, %1, %2, %3); %4"_s.arg(width)
                .arg(operand(a[count - 2]), operand(a[count - 1]), operand(a[0], u"tmp"_s));
        }
        if (id == X86_INS_NOT && count == 1)
            return operand(a[0], u"~"_s + operand(a[0]));
        static const QMap<unsigned, int> conditions{{X86_INS_JO, 0}, {X86_INS_JNO, 1},
            {X86_INS_JB, 2}, {X86_INS_JAE, 3}, {X86_INS_JE, 4}, {X86_INS_JNE, 5},
            {X86_INS_JBE, 6}, {X86_INS_JA, 7}, {X86_INS_JS, 8}, {X86_INS_JNS, 9},
            {X86_INS_JP, 10}, {X86_INS_JNP, 11}, {X86_INS_JL, 12}, {X86_INS_JGE, 13},
            {X86_INS_JLE, 14}, {X86_INS_JG, 15}};
        if ((id == X86_INS_JMP || conditions.contains(id)) && count == 1 && a[0].type == X86_OP_IMM) {
            const auto destination = quint32(a[0].imm);
            if (id == X86_INS_JMP) {
                targets = {destination};
                return {};
            }
            targets << destination;
            return u"if (guest_condition(flags, %1)) goto %2;"_s
                .arg(conditions.value(id)).arg(label(destination));
        }
        if (id == X86_INS_JMP && count == 1 && a[0].type == X86_OP_MEM && a[0].size == 4
            && !a[0].mem.base && !a[0].mem.index && !a[0].mem.segment) {
            const auto symbol = imports.value(quint32(a[0].mem.disp));
            if (!symbol.isEmpty()) {
                targets.clear();
                return u"pc = pop32(&esp); trace_guest_call(\"%1\"); eax = win_%1(&esp); flags = {}; "
                       u"if (!pc) return eax; goto resume;"_s.arg(symbol);
            }
        }
        if (id == X86_INS_JMP && count == 1 && a[0].type == X86_OP_MEM && a[0].size == 4
            && bound && !a[0].mem.base && !a[0].mem.segment && a[0].mem.scale == 4
            && a[0].mem.index == bound->index) {
            const auto table = quint32(a[0].mem.disp);
            const auto last = quint64(table) + 4u * bound->maximum;
            if (last > UINT32_MAX)
                throw Failure(u"таблица переходов выходит за пределы образа"_s);
            targets.clear();
            QString output = u"switch (%1) { "_s.arg(reg(a[0].mem.index));
            for (quint32 i = 0; i <= bound->maximum; ++i) {
                const auto bytes = pe.codeAt(table + 4 * i);
                if (bytes.size() < 4)
                    throw Failure(u"обрезана таблица переходов"_s);
                const auto destination = qFromLittleEndian<quint32>(bytes.constData());
                if (pe.codeAt(destination).isEmpty())
                    throw Failure(u"таблица переходов указывает за пределы кода"_s);
                targets << destination;
                output += u"case %1: goto %2; "_s.arg(i).arg(label(destination));
            }
            return output + u"default: guest_fault(); }"_s;
        }
        if (id == X86_INS_CALL && !x.prefix[2] && count == 1 && a[0].type == X86_OP_IMM) {
            const auto continuation = targets.front();
            continuations.insert(continuation);
            targets.prepend(quint32(a[0].imm));
            return u"push32(&esp, %1);"_s.arg(constant(continuation));
        }
        if (id == X86_INS_RET && !x.prefix[2] && (!count || (count == 1 && a[0].type == X86_OP_IMM))) {
            targets.clear();
            return u"pc = pop32(&esp); esp += %1; if (!pc) return eax; goto resume;"_s
                .arg(constant(count ? quint16(a[0].imm) : 0));
        }
        if (id == X86_INS_CALL && !x.prefix[2] && count == 1) {
            if (a[0].type == X86_OP_MEM) {
                const auto& mem = a[0].mem;
                if (!mem.base && !mem.index && !mem.segment) {
                    const auto symbol = imports.value(quint32(mem.disp));
                    if (symbol == u"ExitProcess") {
                        targets.clear();
                        return u"win_ExitProcess(&esp);"_s;
                    }
                    if (!symbol.isEmpty())
                        return u"trace_guest_call(\"%1\"); eax = win_%1(&esp); flags = {};"_s.arg(symbol);
                }
            }
            if (indirect)
                return u"eax = call_indirect(%1, &esp); flags = {};"_s.arg(operand(a[0]));
            throw Failure(u"косвенный вызов вне известного слота IAT"_s);
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
        imports.insert(item.slot, importSymbol(item));
    }

    const auto entry = quint32(pe.base + pe.entry);
    QList<quint32> pending{entry};
    QSet<quint32> continuations, callbacks;
    const bool threads = std::any_of(report.imports.cbegin(), report.imports.cend(), [](const Import& item) {
        return item.name == u"CreateThread";
    });
    const bool indirect = std::any_of(report.imports.cbegin(), report.imports.cend(), [](const Import& item) {
        return item.name == u"GetProcAddress" || item.name == u"CoCreateInstance";
    });
    const bool usesCallbacks = threads || std::any_of(report.imports.cbegin(), report.imports.cend(), [](const auto& item) {
        return widgetImport(item) && (item.name == u"RegisterClassW" || item.name == u"SetWindowLongW"
            || item.name == u"CallWindowProcW" || item.name == u"SetTimer"
            || item.name == u"CreateDialogParamW" || item.name == u"DialogBoxParamW"
            || item.name == u"SetClassLongW" || item.name == u"SHBrowseForFolderW");
    });
    QMap<quint32, QString> translated;
    QMap<quint32, JumpBound> comparisons, jumpTables;
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
            const auto instruction = decoder.ins->id;
            const auto& detail = decoder.ins->detail->x86;
            const auto next = pc + decoder.ins->size;
            if (instruction == X86_INS_CMP && detail.op_count == 2
                && detail.operands[0].type == X86_OP_REG && detail.operands[0].size == 4
                && detail.operands[1].type == X86_OP_IMM && quint64(detail.operands[1].imm) < 1024) {
                comparisons.insert(next, {detail.operands[0].reg, quint32(detail.operands[1].imm)});
            } else if (const auto compare = comparisons.constFind(pc); compare != comparisons.cend()) {
                if (instruction == X86_INS_JA)
                    jumpTables.insert(next, *compare);
                else if (instruction == X86_INS_MOV && detail.operands[0].type == X86_OP_MEM)
                    comparisons.insert(next, *compare);
            }
            const auto table = jumpTables.constFind(pc);
            const std::optional<JumpBound> bound = table == jumpTables.cend() ? std::nullopt
                : std::optional<JumpBound>(*table);
            code = decoder.lift(imports, targets, continuations, indirect, pe, bound);
            if (usesCallbacks && (decoder.ins->id == X86_INS_MOV || decoder.ins->id == X86_INS_PUSH)) {
                const auto& x = decoder.ins->detail->x86;
                const auto& source = x.operands[x.op_count - 1];
                const auto addCallback = [&](quint32 address) {
                    if (!callbacks.contains(address)) {
                        callbacks.insert(address);
                        pending.append(address);
                    }
                };
                if (source.type == X86_OP_IMM && !pe.codeAt(quint32(source.imm)).isEmpty()) {
                    addCallback(quint32(source.imm));
                } else if (decoder.ins->id == X86_INS_PUSH && source.type == X86_OP_MEM
                    && !source.mem.base && !source.mem.segment && source.mem.index && source.mem.scale == 4) {
                    const auto start = quint32(source.mem.disp);
                    QList<quint32> candidates;
                    for (quint32 index = 0; index < 64 && quint64(start) + 4 * index <= UINT32_MAX; ++index) {
                        const auto address = pe.dataWordAt(start + 4 * index);
                        if (!address || pe.codeAt(*address).isEmpty())
                            break;
                        candidates.append(*address);
                    }
                    if (candidates.size() >= 2)
                        for (const auto address : candidates)
                            addCallback(address);
                }
            }
        } catch (const Failure& error) {
            throw Failure(u"0x%1 — %2 %3: %4."_s.arg(hex(pc), QString::fromLatin1(decoder.ins->mnemonic),
                QString::fromLatin1(decoder.ins->op_str), QString::fromUtf8(error.what())));
        }
        if (!targets.isEmpty() && !(decoder.ins->id == X86_INS_JMP
                && decoder.ins->detail->x86.operands[0].type == X86_OP_MEM))
            code += u" goto "_s + label(targets.front()) + u';';
        translated.insert(pc, code);
        pending.append(targets);
    }

    report.needsWidgets = std::any_of(report.imports.cbegin(), report.imports.cend(), widgetImport);
    const auto hasImport = [&](QStringView name) {
        return std::any_of(report.imports.cbegin(), report.imports.cend(),
            [&](const Import& item) { return item.name == name; });
    };
    bool embedOriginal = false;
    if (hasImport(u"GetModuleFileNameW") && hasImport(u"CreateFileW")) {
        quint64 sectionEnd = pe.headers;
        for (const auto& section : pe.sections)
            if (section.rawSize)
                sectionEnd = std::max(sectionEnd, quint64(section.raw) + section.rawSize);
        const auto payloadEnd = pe.directories[4] ? pe.directories[4].rva : quint64(pe.bytes.size());
        embedOriginal = payloadEnd > sectionEnd;
    }
    QString source;
    QTextStream out(&source);
    if (report.needsWidgets)
        out << "#define MOST_WIDGETS 1\n" << widgetHeaders;
    if (threads)
        out << "#define MOST_GUEST_THREADS 1\n";
    out << runtimeDeclarations << "static constexpr uint32_t guest_image_base = "
        << constant(quint32(pe.base)) << ";\n";
    if (report.needsWidgets)
        out << "static constexpr uint32_t guest_resource_rva = " << constant(pe.directories[2].rva)
            << ", guest_resource_size = " << constant(pe.directories[2].size) << ";\n";
    if (embedOriginal) {
        out << "#define MOST_EMBEDDED_PE 1\nextern \"C\" const uint8_t guest_original_pe[];\n"
               "static constexpr size_t guest_original_pe_size = " << pe.bytes.size() << ";\n";
        report.peAssembly += u".section .rodata\n.globl guest_original_pe\n"
                             "guest_original_pe: .incbin \"image.pe\"\n"_s;
    }
    QStringList regions;
    for (const auto& section : pe.sections) {
        if (!section.size || section.executable())
            continue;
        const QString name = u"data_%1"_s.arg(regions.size());
        auto initialized = std::min(section.rawSize, section.size);
        while (initialized && pe.bytes.constData()[quint64(section.raw) + initialized - 1] == '\0')
            --initialized;
        if (!initialized) {
            out << "static uint8_t " << name << '[' << section.size << "];\n";
        } else {
            out << "extern \"C\" uint8_t " << name << '[' << section.size << "];\n";
            report.peAssembly += u".data\n.balign 16\n.globl %1\n%1: .incbin \"image.pe\", %2, %3\n"_s
                                    .arg(name).arg(section.raw).arg(initialized);
            if (initialized < section.size)
                report.peAssembly += u".zero %1\n"_s.arg(section.size - initialized);
        }
        regions << u"{%1, sizeof %2, %2, %3},"_s.arg(constant(pe.base + section.rva), name)
                      .arg(int(section.writable()));
    }
    out << "static uint8_t stack[1048576];\nstatic const region regions[] = {\n"
        << regions.join(u'\n') << "\n{0x7ff00000u, sizeof stack, stack, 1}};\n"
        << "static const uint32_t import_slots[] = {";
    for (const auto slot : imports.keys())
        out << constant(slot) << ',';
    out << "0};\n";
    if (indirect) {
        out << "static uint32_t read_import_address(uint32_t);\n";
        out << "#define MOST_DYNAMIC_IMPORTS 1\n";
    }
    out << runtimeFunctions << runtimeAlu << runtimeRegistry << runtimeFind << runtimeEncoding
        << runtimeProcess << runtimeSecurity << runtimeProfile << runtimeChild;
    out << "\nstruct GuestExit { int status; };\n"
           "[[noreturn]] static uint32_t win_ExitProcess(uint32_t* sp) { int status = int(pop32(sp) & 255u); "
           "if (trace_guest()) fprintf(stderr, \"Most PE32: 0x%08x ExitProcess(%d)\\n\", guest_pc, status); "
           "throw GuestExit{status}; }\n";
    out << runtimeModuleDeclarations;
    {
        out << "static uint32_t run_guest_code(uint32_t, uint32_t&);\n"
               "static bool is_guest_callback(uint32_t address) { switch (address) {\n";
        for (auto it = translated.cbegin(); it != translated.cend(); ++it) {
            if (callbacks.contains(it.key()))
                out << "case " << constant(it.key()) << ": return true;\n";
        }
        out << "default: return false; } }\n" << runtimeThreads;
    }
    if (report.needsWidgets) {
        out << runtimeGui << runtimeMessages << runtimeDialogs << runtimeUser32Extra
            << runtimePaint << runtimeGdi << runtimeShell << runtimeCom;
    }
    const auto exportModule = [&](const QString& dll, const QSet<QString>& exports) {
        auto names = exports.values();
        names.sort();
        for (const auto& name : names)
            out << "{" << '"' << dll << "\", \"" << name << "\", win_"
                << importSymbol(Import{dll, name, 0, false}) << "},\n";
    };
    out << "static const BuiltinExport builtin_exports[] = {\n";
    exportModule(u"kernel32.dll"_s, kernel32Imports);
    exportModule(u"advapi32.dll"_s, advapi32Imports);
    exportModule(u"ole32.dll"_s, ole32Imports);
    exportModule(u"user32.dll"_s, user32Imports);
    if (report.needsWidgets) {
        exportModule(u"user32.dll"_s, widgetImports);
        exportModule(u"gdi32.dll"_s, gdi32Imports);
        exportModule(u"comctl32.dll"_s, comctl32Imports);
        exportModule(u"shell32.dll"_s, shell32Imports);
        exportModule(u"ole32.dll"_s, widgetOleImports);
        out << "{\"comctl32.dll\", \"InitCommonControls\", win_InitCommonControls},\n";
    }
    out << "};\n" << runtimeModules;
    if (indirect) {
        out << "static uint32_t read_import_address(uint32_t address) { switch (address) {\n";
        for (const auto& item : report.imports)
            out << "case " << constant(item.slot) << ": return builtin_address(\""
                << item.dll.toLower() << "\", \"" << item.name << "\");\n";
        out << "default: return 0; } }\n";
    }
    out << "\nstatic uint32_t run_guest_code(uint32_t pc, uint32_t& esp) {\n"
        << "uint32_t eax=0, ebx=0, ecx=0, edx=0, esi=0, edi=0, ebp=0, tmp=0;\n"
        << "GuestFlags flags; bool backwards = false;\nresume: switch (pc) {\n";
    continuations.unite(callbacks);
    continuations.insert(entry);
    for (auto it = translated.cbegin(); it != translated.cend(); ++it)
        if (continuations.contains(it.key()))
            out << "case " << constant(it.key()) << ": goto " << label(it.key()) << ";\n";
    out << "default: guest_fault(); }\n";
    for (auto it = translated.cbegin(); it != translated.cend(); ++it)
        out << label(it.key()) << ": guest_pc = " << constant(it.key()) << "; "
            << (threads ? "guest_checkpoint(); " : "") << it.value() << '\n';
    out << "}\nint main(int argc, char** argv) { host_argc = argc; host_argv = argv; ";
    QSet<QString> importedModules;
    for (const auto& item : report.imports) {
        const auto dll = item.dll.toLower();
        if (!importedModules.contains(dll)) {
            importedModules.insert(dll);
            out << "pin_builtin_module(\"" << dll << "\"); ";
        }
    }
    if (threads)
        out << "std::lock_guard main_execution(guest_execution_mutex); ";
    out <<
           "uint32_t esp = 0x7ffffff0u; push32(&esp, 0); int result; "
           "try { result = int(run_guest_code(" << constant(entry) << ", esp) & 255u); } "
           "catch (const GuestExit& exit) { result = exit.status; }\n";
    if (report.needsWidgets)
        out << "finish_widgets();\n";
    out << "finish_files();\n";
    if (threads)
        out << "if (active_guest_threads) { fflush(nullptr); _Exit(result); }\n";
    out << "return result; }\n";
    out.flush();
    if (!report.peAssembly.isEmpty()) {
        report.peAssembly += u".section .note.GNU-stack,\"\",@progbits\n"_s;
        report.peData = pe.bytes;
    }
    report.instructions = translated.size();
    report.source = std::move(source);
}
}

namespace {
QByteArray readPe(QFile& file)
{
    const auto expected = file.size();
    auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || bytes.size() != expected)
        throw Failure(u"Не удалось полностью прочитать %1."_s.arg(file.fileName()));
    return bytes;
}

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
    if (!QFileInfo(upx).isExecutable() || bwrap.isEmpty())
        throw Failure(u"Для автоматической распаковки нужны UPX из комплекта и bwrap."_s);
    QTemporaryDir work;
    if (!work.isValid())
        throw Failure(u"Не удалось создать временную папку для UPX."_s);
    QFile input(work.filePath(u"input.exe"_s));
    if (!input.open(QIODevice::WriteOnly) || input.write(bytes) != bytes.size() || !input.flush())
        throw Failure(u"Не удалось записать временную копию для UPX."_s);
    input.close();
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(bwrap, {
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
        throw Failure(u"Распаковка UPX не удалась. %1"_s
                          .arg(QString::fromLocal8Bit(process.readAll()).trimmed().right(600)));
    QFile output(work.filePath(u"unpacked.exe"_s));
    if (!output.open(QIODevice::ReadOnly))
        throw Failure(u"Не удалось открыть результат UPX."_s);
    return readPe(output);
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
        Pe pe(readPe(file));
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
        if (!(pe.characteristics & 2) || (pe.characteristics & 0x2000) || (pe.subsystem != 3 && pe.subsystem != 2))
            report.problems << u"Требуется Windows EXE с подсистемой CUI или GUI."_s;

        for (bool delayed : {false, true}) {
            try {
                pe.readImports(delayed, report.imports);
            } catch (const Failure& error) {
                report.problems << (delayed ? u"Список отложенных импортов неполон. "_s
                                            : u"Список статических импортов неполон. "_s)
                                       + QString::fromUtf8(error.what());
            }
        }
        if (pe.directories[2]) {
            try {
                report.notes << u"RESOURCE — записей ресурсов: %1. Вызовы Win32 для ресурсов зависят от профиля."_s
                                    .arg(pe.countResources());
            } catch (const Failure& error) {
                report.problems << QString::fromUtf8(error.what());
            }
        }
        if (pe.directories[4]) {
            try {
                report.notes << u"SECURITY — записей сертификатов: %1. Подлинность подписи не проверяется."_s
                                    .arg(pe.countCertificates());
            } catch (const Failure& error) {
                report.problems << QString::fromUtf8(error.what());
            }
        }
        const char* names[] = {"EXPORT", "IMPORT", "RESOURCE", "EXCEPTION", "SECURITY", "BASERELOC",
            "DEBUG", "ARCHITECTURE", "GLOBALPTR", "TLS", "LOAD_CONFIG", "BOUND_IMPORT", "IAT",
            "DELAY_IMPORT", "CLR", "RESERVED"};
        for (unsigned i = 0; i < pe.directories.size(); ++i)
            if (pe.directories[i] && i != 1 && i != 2 && i != 4 && i != 5 && i != 6 && i != 12)
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
            report.notes << u"Полный список динамических зависимостей неизвестен. Доступны встроенные экспорты."_s;
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
        QStringList arguments{u"-m64"_s, u"-std=c++20"_s, u"-O1"_s, source.fileName(), u"-o"_s, elf};
        if (!report.peAssembly.isEmpty()) {
            const auto writeStaged = [&](const QString& name, const QByteArray& data) {
                QFile file(staging.filePath(name));
                if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.flush())
                    throw Failure(u"Не удалось встроить данные PE."_s);
            };
            writeStaged(u"image.pe"_s, report.peData);
            writeStaged(u"image.S"_s, report.peAssembly.toUtf8());
            process.setWorkingDirectory(staging.path());
            arguments << staging.filePath(u"image.S"_s);
        }
        if (report.needsWidgets) {
            QProcess config;
            config.start(u"pkg-config"_s, {u"--cflags"_s, u"--libs"_s, u"Qt6Widgets"_s});
            if (!config.waitForStarted() || !config.waitForFinished(5000)
                || config.exitStatus() != QProcess::NormalExit || config.exitCode())
                throw Failure(u"Для сборки PE32 с окнами нужны pkg-config и Qt6Widgets."_s);
            arguments << u"-fPIC"_s
                      << QProcess::splitCommand(QString::fromLocal8Bit(config.readAllStandardOutput()).simplified());
        } else {
            arguments << u"-static-libstdc++"_s << u"-static-libgcc"_s;
        }
        process.start(compiler, arguments);
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
