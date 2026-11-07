#include <QApplication>
#include <QGuiApplication>
#include <QCloseEvent>
#include <QScreen>
#include <QTimer>
#include <QStyle>
#include <QFontMetrics>
#include <QWidget>
#include <QtMath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#define WINAPI extern "C" __attribute__((ms_abi))

static QApplication& application()
{
    static int argc = 1;
#ifdef MOST_W32VER
    static char name[] = "w32ver-linux";
#else
    static char name[] = "rufus-linux";
#endif
    static char* argv[] = {name, nullptr};
    static QApplication* app = [] {
        auto* instance = new QApplication(argc, argv);
        instance->setQuitOnLastWindowClosed(false);
        return instance;
    }();
    return *app;
}

static QScreen* screen()
{
    application();
    auto* primary = QGuiApplication::primaryScreen();
    if (!primary)
        std::abort();
    return primary;
}

WINAPI void* win_GetDC(void* window)
{
    (void)window;
    return screen();
}

WINAPI int win_ReleaseDC(void* window, void* dc)
{
    (void)window;
    return dc == screen();
}

WINAPI int win_GetDeviceCaps(void* dc, int index)
{
    if (dc != screen())
        return 0;
    const auto* display = static_cast<QScreen*>(dc);
    switch (index) {
    case 4: return qRound(display->physicalSize().width());
    case 6: return qRound(display->physicalSize().height());
    case 8: return display->geometry().width();
    case 10: return display->geometry().height();
    case 12: return display->depth();
    case 14: return 1;
    case 88: return qRound(display->logicalDotsPerInchX());
    case 90: return qRound(display->logicalDotsPerInchY());
    default:
        std::fprintf(stderr, "GetDeviceCaps index=%d\n", index);
        return 0;
    }
}

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFontMetrics>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QTextEdit>
#include <cstring>
#include <stdexcept>

using Proc = intptr_t (__attribute__((ms_abi)) *)(void*, uint32_t, uintptr_t, intptr_t);

WINAPI int win_SetProcessDefaultLayout(uint32_t layout)
{
    application().setLayoutDirection(layout & 1 ? Qt::RightToLeft : Qt::LeftToRight);
    return 1;
}

class GuestWindow : public QDialog {
public:
    explicit GuestWindow(QWidget* parent, Proc proc) : QDialog(parent), proc_(proc) {}
protected:
    void closeEvent(QCloseEvent* event) override
    {
        if (proc_ && proc_(this, 0x10, 0, 0))
            event->ignore();
        else
            QDialog::closeEvent(event);
    }
private:
    Proc proc_;
};

class DialogReader {
public:
    explicit DialogReader(const void* start) : bytes_(static_cast<const unsigned char*>(start)) {}
    uint16_t word() { return take<uint16_t>(); }
    uint32_t dword() { return take<uint32_t>(); }
    int16_t coord() { return take<int16_t>(); }
    bool extended() const { return bytes_[2] == 0xff && bytes_[3] == 0xff; }
    void align() { position_ = (position_ + 3) & ~size_t(3); }
    void skip(size_t size)
    {
        if (position_ + size > 65536)
            throw std::runtime_error("dialog template exceeds 64 KiB");
        position_ += size;
    }
    QString text()
    {
        const auto first = word();
        if (!first)
            return {};
        if (first == 0xffff)
            return QStringLiteral("#%1").arg(word());
        QString result{QChar(first)};
        for (uint16_t ch; (ch = word()); )
            result += QChar(ch);
        return result;
    }
private:
    template<typename T> T take()
    {
        auto at = position_;
        skip(sizeof(T));
        T value;
        std::memcpy(&value, bytes_ + at, sizeof(T));
        return value;
    }
    const unsigned char* bytes_;
    size_t position_ = 0;
};

struct GuestDialog {
    QDialog* window;
    Proc proc;
    QHash<uint32_t, QWidget*> controls;
    int unitX;
    int unitY;
};

static QHash<QWidget*, GuestDialog*> dialogs;
static QHash<QWidget*, QHash<int, intptr_t>> window_data;
static QHash<QWidget*, QString> window_classes;
static QHash<QWidget*, void*> window_fonts;

static QWidget* control(QDialog* dialog, const QString& kind, const QString& title, uint32_t style)
{
    const QString type = kind.toLower();
    if (type == QStringLiteral("#128") || type == QStringLiteral("button")) {
        switch (style & 15) {
        case 2: case 3: case 4: case 5: case 6: case 9:
            return new QCheckBox(title, dialog);
        default:
            return new QPushButton(title, dialog);
        }
    }
    if (type == QStringLiteral("#129") || type == QStringLiteral("edit")) {
        if (style & 4) {
            auto* edit = new QTextEdit(dialog);
            edit->setPlainText(title);
            edit->setReadOnly(style & 0x800);
            return edit;
        }
        auto* edit = new QLineEdit(title, dialog);
        edit->setReadOnly(style & 0x800);
        return edit;
    }
    if (type == QStringLiteral("#130") || type == QStringLiteral("static"))
        return new QLabel(title.startsWith('#') ? QString() : title, dialog);
    if (type == QStringLiteral("#131") || type == QStringLiteral("listbox"))
        return new QListWidget(dialog);
    if (type == QStringLiteral("#132") || type == QStringLiteral("scrollbar"))
        return new QScrollBar(dialog);
    if (type == QStringLiteral("#133") || type == QStringLiteral("combobox"))
        return new QComboBox(dialog);
    if (type == QStringLiteral("msctls_trackbar32"))
        return new QSlider(Qt::Horizontal, dialog);
    if (type == QStringLiteral("msctls_progress32"))
        return new QProgressBar(dialog);
    if (type.startsWith(QStringLiteral("richedit")))
        return new QTextEdit(dialog);
    if (type == QStringLiteral("syslink"))
        return new QLabel(title, dialog);
    std::fprintf(stderr, "Dialog control class %s is unsupported\n", qPrintable(kind));
    return new QWidget(dialog);
}

static GuestDialog* make_dialog(const void* memory, void* parent, Proc procedure, intptr_t arg)
{
    application();
    try {
        DialogReader reader(memory);
        const bool extended = reader.extended();
        uint32_t style, extended_style;
        if (extended) {
            reader.skip(8);
            extended_style = reader.dword();
            style = reader.dword();
        } else {
            style = reader.dword();
            extended_style = reader.dword();
        }
        const auto count = reader.word();
        if (count > 256)
            throw std::runtime_error("too many controls");
        reader.skip(4);
        const auto width = reader.coord(), height = reader.coord();
        reader.text();
        reader.text();
        const auto title = reader.text();
        auto* parent_widget = dialogs.contains(static_cast<QWidget*>(parent))
            ? static_cast<QWidget*>(parent) : nullptr;
        auto* window = new GuestWindow(parent_widget, procedure);
        window->setWindowTitle(title);
        window_classes[window] = QStringLiteral("#32770");
        window_data[window][-16] = style;
        window_data[window][-20] = extended_style;
        if (style & 0x40) {
            const int size = reader.word();
            int weight = 400;
            bool italic = false;
            if (extended) {
                weight = reader.word();
                italic = reader.word() & 0xff;
            }
            auto family = reader.text();
            if (family == QStringLiteral("MS Shell Dlg"))
                family = window->font().family();
            window->setFont(QFont(family, size, qBound(1, weight, 1000), italic));
        }
        const QFontMetrics metrics(window->font());
        const int unitX = metrics.averageCharWidth(), unitY = metrics.height();
        window->resize(width * unitX / 4, height * unitY / 8);
        auto* dialog = new GuestDialog{window, procedure, {}, unitX, unitY};
        for (unsigned i = 0; i < count; ++i) {
            reader.align();
            uint32_t item_style, item_extended_style;
            if (extended) {
                reader.skip(4);
                item_extended_style = reader.dword();
                item_style = reader.dword();
            } else {
                item_style = reader.dword();
                item_extended_style = reader.dword();
            }
            const int x = reader.coord(), y = reader.coord(), w = reader.coord(), h = reader.coord();
            const auto id = extended ? reader.dword() : reader.word();
            const auto class_name = reader.text();
            const auto text = reader.text();
            const auto extra = reader.word();
            if (extra)
                reader.skip(extended ? extra : extra - 2u);
            auto* widget = control(window, class_name, text, item_style);
            dialog->controls.insert(id, widget);
            static const QHash<QString, QString> builtin_classes{
                {QStringLiteral("#128"), QStringLiteral("Button")},
                {QStringLiteral("#129"), QStringLiteral("Edit")},
                {QStringLiteral("#130"), QStringLiteral("Static")},
                {QStringLiteral("#131"), QStringLiteral("ListBox")},
                {QStringLiteral("#132"), QStringLiteral("ScrollBar")},
                {QStringLiteral("#133"), QStringLiteral("ComboBox")}
            };
            window_classes[widget] = builtin_classes.value(class_name, class_name);
            window_data[widget][-16] = item_style;
            window_data[widget][-20] = item_extended_style;
            window_data[widget][-12] = id;
            const int height = qobject_cast<QComboBox*>(widget)
                ? qMin(h * unitY / 8, widget->sizeHint().height()) : h * unitY / 8;
            widget->setGeometry(x * unitX / 4, y * unitY / 8, w * unitX / 4, height);
            widget->setEnabled(!(item_style & 0x08000000));
            widget->setVisible(item_style & 0x10000000);
            if (auto* button = qobject_cast<QAbstractButton*>(widget))
                QObject::connect(button, &QAbstractButton::clicked, window, [dialog, id, widget] {
                    if (dialog->proc)
                        dialog->proc(dialog->window, 0x111, id & 0xffff, reinterpret_cast<intptr_t>(widget));
                });
            if (auto* combo = qobject_cast<QComboBox*>(widget))
                QObject::connect(combo, &QComboBox::activated, window, [dialog, id, widget](int) {
                    if (dialog->proc)
                        dialog->proc(dialog->window, 0x111, (id & 0xffff) | (1u << 16),
                                     reinterpret_cast<intptr_t>(widget));
                });
        }
        dialogs.insert(window, dialog);
        std::fprintf(stderr, "Rufus Win32 dialog: %s (%u controls)\n", qPrintable(title), count);
        if (procedure)
            procedure(window, 0x110, 0, arg);
        return dialog;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Cannot create guest dialog: %s\n", error.what());
        return nullptr;
    }
}

WINAPI void* win_CreateDialogIndirectParamW(void* instance, const void* memory, void* parent,
                                            Proc proc, intptr_t arg)
{
    (void)instance;
    if (auto* dialog = make_dialog(memory, parent, proc, arg))
        return dialog->window;
    return nullptr;
}
static bool test_writing()
{
    return qEnvironmentVariable("RUFUS_PROBE_CONTROL") == QStringLiteral("Write") &&
           qEnvironmentVariable("RUFUS_PROBE_DEVICE") == QStringLiteral("1");
}
extern "C" int probe_is_virtual_disk();
static bool test_disk_warning(const QString& text)
{
    return test_writing() && probe_is_virtual_disk() &&
        text.contains(QStringLiteral("NO_LABEL (Disk 2)"), Qt::CaseInsensitive) &&
        text.contains(QStringLiteral("destroy"), Qt::CaseInsensitive);
}
static QPushButton* named_button(QWidget* window, const QString& title)
{
    for (auto* button : window->findChildren<QPushButton*>())
        if (button->text().remove('&').compare(title, Qt::CaseInsensitive) == 0)
            return button;
    return nullptr;
}
WINAPI intptr_t win_DialogBoxIndirectParamW(void* instance, const void* memory, void* parent,
                                            Proc proc, intptr_t arg)
{
    auto* window = static_cast<QDialog*>(win_CreateDialogIndirectParamW(instance, memory, parent, proc, arg));
    if (!window)
        return -1;
    if (test_writing()) {
        QStringList labels;
        for (auto* label : window->findChildren<QLabel*>())
            labels << label->text();
        const auto text = labels.join(' ');
        std::fprintf(stderr, "Rufus modal [%s] %s\n", qPrintable(window->windowTitle()), qPrintable(text));
        if (test_disk_warning(text)) {
            QTimer::singleShot(0, window, [window] {
                auto* button = named_button(window, QStringLiteral("OK"));
                if (!button)
                    button = named_button(window, QStringLiteral("Yes"));
                if (button) {
                    std::fprintf(stderr, "Confirmed write to isolated file-backed disk\n");
                    button->click();
                }
            });
        }
    }
    return window->exec();
}
WINAPI int win_EndDialog(void* window, intptr_t result)
{
    auto* dialog = dialogs.value(static_cast<QWidget*>(window), nullptr);
    if (!dialog)
        return 0;
    dialog->window->done(result);
    return 1;
}

static QString utf16(const char16_t* text)
{
    return text ? QString::fromUtf16(text) : QString();
}

WINAPI int win_MessageBoxExW(void* owner, const char16_t* text, const char16_t* title,
                             uint32_t type, uint16_t language)
{
    (void)language;
    application();
    auto* parent = dialogs.contains(static_cast<QWidget*>(owner)) ? static_cast<QWidget*>(owner) : nullptr;
    const auto message = utf16(text);
    std::fprintf(stderr, "%s message [%s]: %s\n", qPrintable(QApplication::applicationName()),
                 qPrintable(utf16(title)), qPrintable(message));
    const auto kind = type & 0xf;
    if (test_disk_warning(message)) {
        std::fprintf(stderr, "Confirmed write to isolated file-backed disk\n");
        return kind == 4 ? 6 : 1;
    }
    if (qEnvironmentVariableIsSet("RUFUS_PROBE_STARTUP") && kind == 4 &&
        utf16(title) == QStringLiteral("Warning: Unofficial version"))
        return 6; // The test explicitly proceeds with this unsigned relinked build.
    if (qEnvironmentVariableIsSet("RUFUS_PROBE_STARTUP") &&
        kind == 4 && message.contains(QStringLiteral("check for application updates"), Qt::CaseInsensitive))
        return 7; // Test chooses No, without requesting network access.
    QMessageBox box(parent);
    box.setWindowTitle(utf16(title));
    box.setText(message);
    if ((type & 0xf0) == 0x10) box.setIcon(QMessageBox::Critical);
    if ((type & 0xf0) == 0x30) box.setIcon(QMessageBox::Warning);
    if ((type & 0xf0) == 0x40) box.setIcon(QMessageBox::Information);
    switch (kind) {
    case 1: box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel); break;
    case 3: box.setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel); break;
    case 4: box.setStandardButtons(QMessageBox::Yes | QMessageBox::No); break;
    case 5: box.setStandardButtons(QMessageBox::Retry | QMessageBox::Cancel); break;
    default: box.setStandardButtons(QMessageBox::Ok); break;
    }
    if (qEnvironmentVariableIsSet("RUFUS_PROBE_NONINTERACTIVE"))
        std::_Exit(127);
#ifdef MOST_W32VER
    if (QGuiApplication::platformName() == QStringLiteral("offscreen") &&
        qEnvironmentVariableIsSet("MOST_W32VER_SMOKE"))
        QTimer::singleShot(0, &box, [&box] { box.done(QMessageBox::Ok); });
#endif
    switch (box.exec()) {
    case QMessageBox::Ok: return 1;
    case QMessageBox::Cancel: return 2;
    case QMessageBox::Retry: return 4;
    case QMessageBox::Yes: return 6;
    case QMessageBox::No: return 7;
    default: return 0;
    }
}
WINAPI int win_MessageBoxA(void* owner, const char* text, const char* title, uint32_t type)
{
    auto message = QString::fromLocal8Bit(text);
    auto caption = QString::fromLocal8Bit(title);
    return win_MessageBoxExW(owner, reinterpret_cast<const char16_t*>(message.utf16()),
                            reinterpret_cast<const char16_t*>(caption.utf16()), type, 0);
}

#include <QFontDatabase>
#include <QRegularExpression>

struct LogFontA {
    int32_t height, width, escapement, orientation, weight;
    uint8_t italic, underline, strike, charset, output, clip, quality, pitch;
    char family[32];
};
using FontCallback = int (__attribute__((ms_abi)) *)(const void*, const void*, uint32_t, intptr_t);

WINAPI int win_EnumFontFamiliesExA(void* dc, const LogFontA* filter, FontCallback callback,
                                  intptr_t arg, uint32_t flags)
{
    (void)flags;
    if (dc != screen() || !filter || !callback)
        return 0;
    const auto requested = QString::fromLocal8Bit(filter->family,
        strnlen(filter->family, sizeof(filter->family)));
    auto families = QFontDatabase::families();
    if (!requested.isEmpty()) {
        auto match = families.filter(QRegularExpression(
            QStringLiteral("^%1$").arg(QRegularExpression::escape(requested)),
            QRegularExpression::CaseInsensitiveOption));
        families = match;
    }
    int result = 1;
    for (const auto& family : families) {
        struct {
            LogFontA font;
            char fullname[64], style[32], script[32];
        } info{};
        const auto name = family.toLocal8Bit();
        std::snprintf(info.font.family, sizeof(info.font.family), "%s", name.constData());
        std::snprintf(info.fullname, sizeof(info.fullname), "%s", name.constData());
        std::snprintf(info.style, sizeof(info.style), "%s", "Regular");
        info.font.weight = 400;
        alignas(8) unsigned char metrics[256]{};
        result = callback(&info, metrics, 4, arg);
        if (!result)
            break;
    }
    return result;
}

WINAPI int win_SystemParametersInfoW(uint32_t action, uint32_t count, void* data, uint32_t flags)
{
    (void)flags;
    if (!data)
        return 0;
    auto* words = static_cast<uint32_t*>(data);
    if ((action == 0x42 || action == 0x32 || action == 0x3a || action == 0x3c) &&
        count >= 8 && words[0] == count) {
        // Qt does not enable the legacy Windows accessibility key filters.
        std::memset(words + 1, 0, count - 4);
        return 1;
    }
    if (action == 0x29 && (count == 500 || count == 504) && words[0] == count) {
        std::memset(words + 1, 0, count - 4);
        auto& app = application();
        words[1] = app.style()->pixelMetric(QStyle::PM_DefaultFrameWidth);
        words[2] = words[3] = app.style()->pixelMetric(QStyle::PM_ScrollBarExtent);
        words[4] = words[5] = app.style()->pixelMetric(QStyle::PM_TitleBarHeight);
        auto font = app.font();
        const auto family = font.family();
        const auto length = qMin(qsizetype(31), family.size());
        const int pixels = font.pixelSize() > 0 ? font.pixelSize() : QFontMetrics(font).height();
        auto* raw = static_cast<unsigned char*>(data);
        for (int offset : {24, 124, 224, 316, 408}) {
            auto* metrics = reinterpret_cast<int32_t*>(raw + offset);
            metrics[0] = -pixels;
            metrics[4] = font.weight();
            std::memcpy(raw + offset + 28, family.utf16(), length * 2);
        }
        return 1;
    }
    if (action == 0x30) {
        auto rect = screen()->availableGeometry();
        auto* coords = static_cast<int32_t*>(data);
        coords[0] = rect.left(); coords[1] = rect.top();
        coords[2] = rect.x() + rect.width(); coords[3] = rect.y() + rect.height();
        return 1;
    }
    std::fprintf(stderr, "SystemParametersInfo action=%#x count=%u\n", action, count);
    return 0;
}

static QWidget* as_widget(void* handle)
{
    auto* widget = static_cast<QWidget*>(handle);
    return window_data.contains(widget) ? widget : nullptr;
}
WINAPI void* win_GetDlgItem(void* handle, int id)
{
    auto* dialog = dialogs.value(static_cast<QWidget*>(handle), nullptr);
    return dialog ? dialog->controls.value(id, nullptr) : nullptr;
}
WINAPI int win_IsWindow(void* handle) { return as_widget(handle) != nullptr; }
WINAPI int win_IsWindowEnabled(void* handle)
{
    auto* widget = as_widget(handle);
    return widget && widget->isEnabled();
}
WINAPI int win_IsWindowVisible(void* handle)
{
    auto* widget = as_widget(handle);
    return widget && widget->isVisible();
}
WINAPI int win_EnableWindow(void* handle, int enable)
{
    auto* widget = as_widget(handle);
    if (!widget)
        return 0;
    const bool was_disabled = !widget->isEnabled();
    widget->setEnabled(enable);
    return was_disabled;
}
WINAPI int win_ShowWindow(void* handle, int command)
{
    auto* widget = as_widget(handle);
    if (!widget)
        return 0;
    const bool was_visible = widget->isVisible();
    if (!command) widget->hide();
    else if (command == 3) widget->showMaximized();
    else if (command == 6) widget->showMinimized();
    else widget->show();
    if (!was_visible && widget->isVisible() && widget->windowTitle().startsWith(QStringLiteral("Rufus")))
        std::fprintf(stderr, "Rufus main window visible: %s\n", qPrintable(widget->windowTitle()));
    return was_visible;
}
WINAPI int win_UpdateWindow(void* handle)
{
    auto* widget = as_widget(handle);
    if (!widget)
        return 0;
    widget->repaint();
    return 1;
}

static QString widget_text(QWidget* widget)
{
    if (auto* text = qobject_cast<QLabel*>(widget)) return text->text();
    if (auto* text = qobject_cast<QAbstractButton*>(widget)) return text->text();
    if (auto* text = qobject_cast<QLineEdit*>(widget)) return text->text();
    if (auto* text = qobject_cast<QTextEdit*>(widget)) return text->toPlainText();
    if (auto* text = qobject_cast<QComboBox*>(widget)) return text->currentText();
    return widget ? widget->windowTitle() : QString();
}
static int set_widget_text(QWidget* widget, const QString& value)
{
    if (!widget) return 0;
    if (auto* text = qobject_cast<QLabel*>(widget)) text->setText(value);
    else if (auto* text = qobject_cast<QAbstractButton*>(widget)) text->setText(value);
    else if (auto* text = qobject_cast<QLineEdit*>(widget)) text->setText(value);
    else if (auto* text = qobject_cast<QTextEdit*>(widget)) text->setPlainText(value);
    else widget->setWindowTitle(value);
    return 1;
}
WINAPI int win_SetWindowTextW(void* handle, const char16_t* value)
{
    return set_widget_text(as_widget(handle), utf16(value));
}
WINAPI int win_SetWindowTextA(void* handle, const char* value)
{
    return set_widget_text(as_widget(handle), value ? QString::fromLocal8Bit(value) : QString());
}
WINAPI int win_SetDlgItemTextW(void* handle, int id, const char16_t* value)
{
    return win_SetWindowTextW(win_GetDlgItem(handle, id), value);
}
WINAPI int win_SetDlgItemTextA(void* handle, int id, const char* value)
{
    return win_SetWindowTextA(win_GetDlgItem(handle, id), value);
}
static int copy_text(const QString& text, char16_t* output, int capacity)
{
    if (!output || capacity <= 0)
        return 0;
    const int length = qMin(qsizetype(capacity - 1), text.size());
    std::memcpy(output, text.utf16(), length * sizeof(*output));
    output[length] = 0;
    return length;
}
WINAPI int win_GetWindowTextW(void* handle, char16_t* output, int capacity)
{
    return copy_text(widget_text(as_widget(handle)), output, capacity);
}
WINAPI int win_GetWindowTextA(void* handle, char* output, int capacity)
{
    if (!output || capacity <= 0)
        return 0;
    const auto text = widget_text(as_widget(handle)).toLocal8Bit();
    int length = qMin(qsizetype(capacity - 1), text.size());
    std::memcpy(output, text.constData(), length);
    output[length] = 0;
    return length;
}
WINAPI int win_GetWindowTextLengthW(void* handle) { return widget_text(as_widget(handle)).size(); }
WINAPI int win_GetDlgItemTextW(void* handle, int id, char16_t* output, int capacity)
{
    return win_GetWindowTextW(win_GetDlgItem(handle, id), output, capacity);
}

struct WinRect { int32_t left, top, right, bottom; };
WINAPI int win_GetWindowRect(void* handle, WinRect* output)
{
    auto* widget = as_widget(handle);
    if (!widget || !output) return 0;
    const auto point = widget->mapToGlobal(QPoint(0, 0));
    *output = {point.x(), point.y(), point.x() + widget->width(), point.y() + widget->height()};
    return 1;
}
WINAPI int win_GetClientRect(void* handle, WinRect* output)
{
    auto* widget = as_widget(handle);
    if (!widget || !output) return 0;
    *output = {0, 0, widget->width(), widget->height()};
    return 1;
}
WINAPI int win_MapDialogRect(void* handle, WinRect* rect)
{
    auto* dialog = dialogs.value(static_cast<QWidget*>(handle), nullptr);
    if (!dialog || !rect) return 0;
    rect->left = qRound(rect->left * dialog->unitX / 4.0);
    rect->right = qRound(rect->right * dialog->unitX / 4.0);
    rect->top = qRound(rect->top * dialog->unitY / 8.0);
    rect->bottom = qRound(rect->bottom * dialog->unitY / 8.0);
    return 1;
}
WINAPI int win_MoveWindow(void* handle, int x, int y, int w, int h, int repaint)
{
    auto* widget = as_widget(handle);
    if (!widget) return 0;
    widget->setGeometry(x, y, w, h);
    if (repaint) widget->update();
    return 1;
}
WINAPI int win_SetWindowPos(void* handle, void* after, int x, int y, int w, int h, uint32_t flags)
{
    (void)after;
    auto* widget = as_widget(handle);
    if (!widget) return 0;
    if (!(flags & 2)) widget->move(x, y);
    if (!(flags & 1)) widget->resize(w, h);
    if (flags & 0x40) widget->show();
    if (flags & 0x80) widget->hide();
    return 1;
}

#include <QTimer>
#include <QAbstractItemView>
#include <QEventLoop>
#include <QThread>
#include <atomic>
#include <QQueue>
#include <QTextCursor>

#include "gdi.inc"
#include "toolbar.inc"

static intptr_t window_message(void* handle, uint32_t msg, uintptr_t w, intptr_t l, bool wide)
{
    auto* widget = as_widget(handle);
    if (!widget)
        return 0;
    auto string = [&] {
        return wide ? utf16(reinterpret_cast<const char16_t*>(l))
                    : QString::fromLocal8Bit(reinterpret_cast<const char*>(l));
    };
    switch (msg) {
    case 0x0030:
        if (auto* font = get_font(reinterpret_cast<void*>(w))) {
            window_fonts.insert(widget, reinterpret_cast<void*>(w));
            widget->setFont(*font);
        }
        if (l) widget->update();
        return 0;
    case 0x0031:
        return reinterpret_cast<intptr_t>(window_fonts.value(widget, win_GetStockObject(17)));
    case 0x0170: case 0x0172:
        if (auto* label = qobject_cast<QLabel*>(widget)) {
            auto* icon = reinterpret_cast<void*>(msg == 0x170 ? w : uintptr_t(l));
            auto previous = label->property("winIcon").toULongLong();
            label->setProperty("winIcon", qulonglong(reinterpret_cast<uintptr_t>(icon)));
            if (guest_icons.contains(icon))
                label->setPixmap(guest_icons.value(icon).pixmap(label->size()));
            else
                label->clear();
            return previous;
        }
        return 0;
    case 0x000c: return set_widget_text(widget, string());
    case 0x000d: return wide ? win_GetWindowTextW(handle, reinterpret_cast<char16_t*>(l), w)
                            : win_GetWindowTextA(handle, reinterpret_cast<char*>(l), w);
    case 0x000e: return widget_text(widget).size();
    case 0x000b: widget->setUpdatesEnabled(w); return 0;
    case 0x00c5:
        if (auto* text = qobject_cast<QLineEdit*>(widget)) text->setMaxLength(w);
        return 0; // QTextEdit has no native 32-bit limit; the requested limit fits Qt's index type.
    case 0x00b1:
        if (auto* text = qobject_cast<QTextEdit*>(widget)) {
            QTextCursor cursor(text->document());
            const int end = text->document()->characterCount() - 1;
            auto start = static_cast<intptr_t>(w);
            cursor.setPosition(start < 0 ? end : qMin(intptr_t(end), start));
            cursor.setPosition(l < 0 ? end : qMin(intptr_t(end), l), QTextCursor::KeepAnchor);
            text->setTextCursor(cursor);
        }
        return 0;
    case 0x00c2:
        if (auto* text = qobject_cast<QTextEdit*>(widget))
            text->insertPlainText(string());
        return 0;
    case 0x00ba:
        if (auto* text = qobject_cast<QTextEdit*>(widget)) return text->document()->blockCount();
        return 1;
    case 0x00b6:
        if (auto* text = qobject_cast<QTextEdit*>(widget)) {
            auto* bar = text->verticalScrollBar();
            bar->setValue(bar->value() + qBound(intptr_t(-100000), l, intptr_t(100000)) * bar->singleStep());
        }
        return 1;
    case 0x00b7:
        if (auto* text = qobject_cast<QTextEdit*>(widget)) text->ensureCursorVisible();
        return 0;
    case 0x00cf:
        if (auto* text = qobject_cast<QTextEdit*>(widget)) text->setReadOnly(w);
        if (auto* text = qobject_cast<QLineEdit*>(widget)) text->setReadOnly(w);
        return 1;
    case 0x00f0:
        if (auto* button = qobject_cast<QCheckBox*>(widget)) return button->checkState() == Qt::Checked ? 1 : 0;
        return 0;
    case 0x00f1:
        if (auto* button = qobject_cast<QCheckBox*>(widget)) button->setChecked(w == 1);
        return 0;
    case 0x00f5:
        if (auto* button = qobject_cast<QAbstractButton*>(widget)) button->click();
        return 0;
    }
    if (auto* toolbar = qobject_cast<QToolBar*>(widget)) {
        intptr_t value;
        if (toolbar_message(toolbar, msg, w, l, value))
            return value;
    }
    if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        const int index = w;
        switch (msg) {
        case 0x0143: combo->addItem(string()); return combo->count() - 1;
        case 0x014a:
            if (index == -1 || index >= combo->count()) { combo->addItem(string()); return combo->count() - 1; }
            if (index >= 0) { combo->insertItem(index, string()); return index; }
            return -1;
        case 0x0144:
            if (index < 0 || index >= combo->count()) return -1;
            combo->removeItem(index); return combo->count();
        case 0x0146: return combo->count();
        case 0x0147: return combo->currentIndex();
        case 0x014b: combo->clear(); return 0;
        case 0x014e:
            combo->setCurrentIndex(index < 0 || index >= combo->count() ? -1 : index);
            return combo->currentIndex();
        case 0x015f: return combo->view()->minimumWidth();
        case 0x0160: combo->view()->setMinimumWidth(w); return w;
        case 0x0157: return combo->view()->isVisible();
        case 0x0150: return index >= 0 && index < combo->count() ? combo->itemData(index).toLongLong() : -1;
        case 0x0151:
            if (index < 0 || index >= combo->count()) return -1;
            combo->setItemData(index, QVariant::fromValue(qlonglong(l))); return 0;
        case 0x0149: return index >= 0 && index < combo->count() ? combo->itemText(index).size() : -1;
        case 0x0148:
            if (index < 0 || index >= combo->count()) return -1;
            if (wide) {
                auto text = combo->itemText(index);
                return copy_text(text, reinterpret_cast<char16_t*>(l), text.size() + 1);
            }
            auto text = combo->itemText(index).toLocal8Bit();
            if (l) std::memcpy(reinterpret_cast<char*>(l), text.constData(), text.size() + 1);
            return text.size();
        }
    }
    if (auto* slider = qobject_cast<QSlider*>(widget)) {
        switch (msg) {
        case 0x400: return slider->value();
        case 0x405: slider->setValue(l); return 0;
        case 0x406: slider->setRange(int16_t(l), int16_t(l >> 16)); return 0;
        case 0x407: slider->setMinimum(l); return 0;
        case 0x408: slider->setMaximum(l); return 0;
        case 0x414: slider->setPageStep(l); return 0;
        case 0x417: slider->setSingleStep(l); return 0;
        }
    }
    if (auto* progress = qobject_cast<QProgressBar*>(widget)) {
        switch (msg) {
        case 0x401: progress->setRange(uint16_t(l), uint16_t(l >> 16)); return 0;
        case 0x402: { const int old = progress->value(); progress->setValue(w); return old; }
        case 0x406: progress->setRange(w, l); return 0;
        case 0x40a:
            if (w && progress->maximum() != 0) {
                progress->setProperty("winOldMaximum", progress->maximum());
                progress->setRange(0, 0);
            } else if (!w && progress->maximum() == 0) {
                progress->setRange(0, qMax(100, progress->property("winOldMaximum").toInt()));
            }
            return 1;
        }
    }
    if (auto* dialog = dialogs.value(widget, nullptr); dialog && dialog->proc)
        return dialog->proc(handle, msg, w, l);
    std::fprintf(stderr, "Unmapped message %#x for %s\n", msg, widget->metaObject()->className());
    return 0;
}

WINAPI intptr_t win_DefWindowProcW(void* window, uint32_t message, uintptr_t w, intptr_t l)
{
    if (message == 0x10) {
        if (auto* widget = as_widget(window))
            widget->hide();
        return 0;
    }
    return window_message(window, message, w, l, true);
}
WINAPI intptr_t win_GetWindowLongPtrW(void* handle, int index)
{
    auto* widget = as_widget(handle);
    if (!widget)
        return 0;
    auto& properties = window_data[widget];
    if (index == -4)
        return properties.value(index, reinterpret_cast<intptr_t>(win_DefWindowProcW));
    return properties.value(index);
}
WINAPI intptr_t win_SetWindowLongPtrW(void* handle, int index, intptr_t value)
{
    auto* widget = as_widget(handle);
    if (!widget)
        return 0;
    const auto previous = win_GetWindowLongPtrW(handle, index);
    window_data[widget][index] = value;
    if (index == -16)
        widget->setEnabled(!(value & 0x08000000));
    return previous;
}
WINAPI intptr_t win_CallWindowProcW(Proc procedure, void* window, uint32_t message, uintptr_t w, intptr_t l)
{
    return procedure ? procedure(window, message, w, l) : 0;
}
WINAPI int win_GetDlgCtrlID(void* window) { return win_GetWindowLongPtrW(window, -12); }
WINAPI void* win_GetParent(void* handle)
{
    auto* widget = as_widget(handle);
    return widget ? widget->parentWidget() : nullptr;
}

WINAPI intptr_t win_SendMessageW(void* window, uint32_t message, uintptr_t w, intptr_t l)
{
    if (QThread::currentThread() != application().thread()) {
        intptr_t result = 0;
        QMetaObject::invokeMethod(&application(), [&] {
            result = win_SendMessageW(window, message, w, l);
        }, Qt::BlockingQueuedConnection);
        return result;
    }
    auto* widget = as_widget(window);
    if (widget) {
        auto address = window_data[widget].value(-4);
        if (address && address != reinterpret_cast<intptr_t>(win_DefWindowProcW))
            return reinterpret_cast<Proc>(address)(window, message, w, l);
    }
    return window_message(window, message, w, l, true);
}
WINAPI intptr_t win_SendMessageA(void* window, uint32_t message, uintptr_t w, intptr_t l)
{
    if (QThread::currentThread() != application().thread()) {
        intptr_t result = 0;
        QMetaObject::invokeMethod(&application(), [&] {
            result = win_SendMessageA(window, message, w, l);
        }, Qt::BlockingQueuedConnection);
        return result;
    }
    return window_message(window, message, w, l, false);
}
WINAPI intptr_t win_SendDlgItemMessageA(void* dialog, int id, uint32_t message, uintptr_t w, intptr_t l)
{
    return win_SendMessageA(win_GetDlgItem(dialog, id), message, w, l);
}

struct WinMessage {
    void* window;
    uint32_t message;
    uintptr_t w;
    intptr_t l;
    uint32_t time;
    int32_t x, y;
};
static QQueue<WinMessage> messages;
static QEventLoop* waiting;

static void queue_message(WinMessage message)
{
    QTimer::singleShot(0, &application(), [message] {
        messages.enqueue(message);
        if (waiting)
            waiting->quit();
    });
}
WINAPI int win_PostMessageW(void* window, uint32_t message, uintptr_t w, intptr_t l)
{
    queue_message({window, message, w, l, 0, 0, 0});
    return 1;
}
WINAPI void win_PostQuitMessage(int status) { queue_message({nullptr, 0x12, uintptr_t(status), 0, 0, 0, 0}); }

WINAPI int win_GetMessageW(WinMessage* output, void* filter, uint32_t first, uint32_t last)
{
    (void)filter; (void)first; (void)last;
    if (!output)
        return -1;
    static bool captured = false;
    if (!captured && qEnvironmentVariableIsSet("RUFUS_PROBE_CAPTURE")) {
        captured = true;
        QTimer::singleShot(150, &application(), [] {
            for (auto* dialog : dialogs) {
                if (dialog->window->isVisible() && dialog->window->windowTitle().contains(QStringLiteral("Rufus"))) {
                    const auto path = qEnvironmentVariable("RUFUS_PROBE_CAPTURE");
                    const bool saved = dialog->window->grab().save(path);
                    std::fprintf(stderr, "Main window shown: %s (%dx%d), screenshot=%d\n",
                        qPrintable(dialog->window->windowTitle()), dialog->window->width(), dialog->window->height(), saved);
                    const auto test_control = qEnvironmentVariable("RUFUS_PROBE_CONTROL");
                    if (saved && !test_control.isEmpty()) {
                        const auto first = test_control == QStringLiteral("Write")
                            ? QStringLiteral("Select") : test_control;
                        auto* button = named_button(dialog->window, first);
                        if (!button)
                            std::_Exit(130);
                        std::fprintf(stderr, "Invoking Rufus %s control\n", qPrintable(first));
                        button->click();
                        if (test_control == QStringLiteral("Write")) {
                            auto* timer = new QTimer(dialog->window);
                            auto* window = dialog->window;
                            QObject::connect(timer, &QTimer::timeout, window,
                                             [timer, window, started = false, disabled = false]() mutable {
                                auto* start = named_button(window, QStringLiteral("Start"));
                                if (!start)
                                    return;
                                if (!started && start->isEnabled()) {
                                    started = true;
                                    std::fprintf(stderr, "Invoking Rufus Start control\n");
                                    start->click();
                                    disabled = !start->isEnabled();
                                } else if (started && !start->isEnabled()) {
                                    disabled = true;
                                } else if (started && disabled && start->isEnabled()) {
                                    timer->stop();
                                    std::fprintf(stderr, "Rufus write workflow returned to its main dialog\n");
                                    if (auto* close = named_button(window, QStringLiteral("Close")))
                                        close->click();
                                }
                            });
                            timer->start(50);
                        }
                        return;
                    }
                    std::_Exit(saved ? 0 : 128);
                }
            }
            std::fprintf(stderr, "No visible Rufus main window\n");
            std::_Exit(129);
        });
    }
    while (messages.isEmpty()) {
        QEventLoop loop;
        waiting = &loop;
        loop.exec();
        waiting = nullptr;
    }
    *output = messages.dequeue();
    return output->message == 0x12 ? 0 : 1;
}
WINAPI intptr_t win_DispatchMessageW(const WinMessage* message)
{
    return message ? win_SendMessageW(message->window, message->message, message->w, message->l) : 0;
}
WINAPI int win_TranslateMessage(const WinMessage* message)
{
    (void)message;
    return 0; // Qt handles input-method and character composition.
}
WINAPI int win_IsDialogMessageW(void* dialog, const WinMessage* message)
{
    (void)dialog; (void)message;
    return 0; // Qt already handles dialog navigation for its widgets.
}
WINAPI int win_CheckDlgButton(void* window, int id, unsigned check)
{
    auto* widget = as_widget(win_GetDlgItem(window, id));
    if (!qobject_cast<QCheckBox*>(widget))
        return 0;
    win_SendMessageW(widget, 0x00f1, check, 0);
    return 1;
}
WINAPI unsigned win_IsDlgButtonChecked(void* window, int id)
{
    return win_SendMessageW(win_GetDlgItem(window, id), 0x00f0, 0, 0);
}

WINAPI uint32_t win_CharUpperBuffW(char16_t* text, uint32_t length)
{
    if (!text)
        return 0;
    for (uint32_t i = 0; i < length; ++i)
        text[i] = QChar(text[i]).toUpper().unicode();
    return length;
}
static char* ascii_case(char* text, bool upper)
{
    auto convert = [upper](unsigned char ch) {
        if (upper && ch >= 'a' && ch <= 'z') return int(ch - 'a' + 'A');
        if (!upper && ch >= 'A' && ch <= 'Z') return int(ch - 'A' + 'a');
        return int(ch);
    };
    if (reinterpret_cast<uintptr_t>(text) <= 0xffff)
        return reinterpret_cast<char*>(uintptr_t(convert(reinterpret_cast<uintptr_t>(text))));
    for (char* p = text; *p; ++p)
        *p = convert(static_cast<unsigned char>(*p));
    return text;
}
WINAPI char* win_CharUpperA(char* text) { return ascii_case(text, true); }
WINAPI char* win_CharLowerA(char* text) { return ascii_case(text, false); }

struct WinPoint { int32_t x, y; };
WINAPI int win_MapWindowPoints(void* source, void* target, WinPoint* points, unsigned count)
{
    auto* from = as_widget(source);
    auto* to = as_widget(target);
    if (!points || (source && !from) || (target && !to))
        return 0;
    QPoint delta;
    if (from)
        delta += from->mapToGlobal(QPoint());
    if (to)
        delta -= to->mapToGlobal(QPoint());
    for (unsigned i = 0; i < count; ++i) {
        points[i].x += delta.x();
        points[i].y += delta.y();
    }
    return uint16_t(delta.x()) | (uint32_t(uint16_t(delta.y())) << 16);
}
WINAPI int win_ClientToScreen(void* source, WinPoint* point)
{
    if (!as_widget(source) || !point) return 0;
    win_MapWindowPoints(source, nullptr, point, 1);
    return 1;
}
WINAPI int win_ScreenToClient(void* target, WinPoint* point)
{
    if (!as_widget(target) || !point) return 0;
    win_MapWindowPoints(nullptr, target, point, 1);
    return 1;
}
WINAPI int win_CopyRect(WinRect* destination, const WinRect* source)
{
    if (!destination || !source) return 0;
    *destination = *source;
    return 1;
}
WINAPI int win_OffsetRect(WinRect* rect, int x, int y)
{
    if (!rect) return 0;
    rect->left += x; rect->right += x; rect->top += y; rect->bottom += y;
    return 1;
}
WINAPI int win_InflateRect(WinRect* rect, int x, int y)
{
    if (!rect) return 0;
    rect->left -= x; rect->right += x; rect->top -= y; rect->bottom += y;
    return 1;
}
WINAPI int win_IntersectRect(WinRect* output, const WinRect* first, const WinRect* second)
{
    if (!output || !first || !second) return 0;
    WinRect overlap{qMax(first->left, second->left), qMax(first->top, second->top),
                    qMin(first->right, second->right), qMin(first->bottom, second->bottom)};
    const bool nonempty = overlap.left < overlap.right && overlap.top < overlap.bottom;
    *output = nonempty ? overlap : WinRect{};
    return nonempty;
}

WINAPI uint32_t win_GetSysColor(int index)
{
    const auto palette = application().palette();
    QPalette::ColorRole role;
    switch (index) {
    case 5: role = QPalette::Base; break;
    case 8: role = QPalette::Text; break;
    case 13: role = QPalette::Highlight; break;
    case 14: role = QPalette::HighlightedText; break;
    case 15: role = QPalette::Button; break;
    case 16: role = QPalette::Dark; break;
    case 18: role = QPalette::ButtonText; break;
    case 20: role = QPalette::Light; break;
    default: role = QPalette::Window; break;
    }
    auto color = palette.color(role);
    return color.red() | (color.green() << 8) | (color.blue() << 16);
}

WINAPI void* win_FindWindowA(const char* class_name, const char* title)
{
    const QString type = class_name && reinterpret_cast<uintptr_t>(class_name) > 0xffff
        ? QString::fromLocal8Bit(class_name) : QString();
    const QString caption = title ? QString::fromLocal8Bit(title) : QString();
    for (auto* dialog : dialogs) {
        if (class_name && type.compare(window_classes.value(dialog->window), Qt::CaseInsensitive))
            continue;
        if (title && caption.compare(dialog->window->windowTitle(), Qt::CaseInsensitive))
            continue;
        return dialog->window;
    }
    return nullptr;
}
WINAPI int win_GetClassNameW(void* handle, char16_t* output, int capacity)
{
    auto* widget = as_widget(handle);
    return widget ? copy_text(window_classes.value(widget), output, capacity) : 0;
}

WINAPI void* win_GetWindow(void* handle, unsigned command)
{
    auto* widget = as_widget(handle);
    if (!widget)
        return nullptr;
    auto* parent = widget->parentWidget();
    if (command == 4)
        return parent;
    auto children = (command == 5 ? widget : parent) ?
        (command == 5 ? widget : parent)->findChildren<QWidget*>(Qt::FindDirectChildrenOnly) : QList<QWidget*>();
    children.removeIf([](QWidget* child) { return !as_widget(child); });
    if (children.isEmpty())
        return nullptr;
    if (command == 0 || command == 5)
        return children.first();
    if (command == 1)
        return children.last();
    auto index = children.indexOf(widget);
    if (command == 2 && index >= 0 && index + 1 < children.size())
        return children[index + 1];
    if (command == 3 && index > 0)
        return children[index - 1];
    return nullptr;
}

WINAPI int win_DestroyWindow(void* handle)
{
    auto* widget = as_widget(handle);
    if (!widget)
        return 0;
    auto clear = [](QWidget* item) {
        window_classes.remove(item);
        window_fonts.remove(item);
        window_data.remove(item);
    };
    for (auto* child : widget->findChildren<QWidget*>())
        clear(child);
    if (auto* dialog = dialogs.take(widget)) {
        if (dialog->proc)
            dialog->proc(handle, 2, 0, 0);
        delete dialog;
    } else {
        for (auto* parent : dialogs)
            parent->controls.remove(win_GetDlgCtrlID(handle));
    }
    clear(widget);
    widget->hide();
    widget->deleteLater();
    return 1;
}

struct WinWindowInfo {
    uint32_t size;
    WinRect window, client;
    uint32_t style, extended_style, status, border_x, border_y;
    uint16_t atom, version;
};
static_assert(sizeof(WinWindowInfo) == 60);

WINAPI int win_GetWindowInfo(void* handle, WinWindowInfo* output)
{
    auto* widget = as_widget(handle);
    if (!widget || !output || output->size != sizeof(*output))
        return 0;
    WinRect rect{};
    win_GetWindowRect(handle, &rect);
    uint32_t border = widget->style()->pixelMetric(QStyle::PM_DefaultFrameWidth);
    *output = {sizeof(*output), rect, rect, uint32_t(win_GetWindowLongPtrW(handle, -16)),
        uint32_t(win_GetWindowLongPtrW(handle, -20)), uint32_t(widget->isActiveWindow()),
        border, border, 0, 0x600};
    return 1;
}
WINAPI void* win_GetDesktopWindow(void)
{
    static QWidget* desktop = [] {
        auto* widget = new QWidget;
        widget->setGeometry(screen()->geometry());
        window_classes[widget] = QStringLiteral("#32769");
        window_data[widget][-16] = 0;
        return widget;
    }();
    return desktop;
}
WINAPI int win_InvalidateRect(void* handle, const WinRect* region, int erase)
{
    (void)erase;
    auto* widget = as_widget(handle);
    if (!widget)
        return 0;
    if (region)
        widget->update(region->left, region->top, region->right - region->left, region->bottom - region->top);
    else
        widget->update();
    return 1;
}
WINAPI int win_RedrawWindow(void* handle, const WinRect* region, void* clip, unsigned flags)
{
    (void)clip;
    if (!win_InvalidateRect(handle, region, flags & 4))
        return 0;
    if (flags & 0x100)
        return win_UpdateWindow(handle);
    return 1;
}

WINAPI int win_AdjustWindowRectEx(WinRect* rect, uint32_t style, int menu, uint32_t extended)
{
    (void)extended;
    if (!rect)
        return 0;
    const bool caption = (style & 0x00c00000) == 0x00c00000;
    const bool bordered = style & (0x00800000 | 0x00040000);
    const int border = bordered ? win_GetSystemMetrics(5) : 0;
    win_InflateRect(rect, border, border);
    if (caption)
        rect->top -= win_GetSystemMetrics(4);
    if (menu)
        rect->top -= QFontMetrics(application().font()).height() + 6;
    return 1;
}

static QHash<QWidget*, QHash<QString, void*>> named_properties;
WINAPI int win_SetPropW(void* handle, const char16_t* name, void* value)
{
    auto* widget = as_widget(handle);
    if (!widget || !name)
        return 0;
    auto key = reinterpret_cast<uintptr_t>(name) <= 0xffff
        ? QStringLiteral("#%1").arg(reinterpret_cast<uintptr_t>(name)) : utf16(name);
    named_properties[widget].insert(key, value);
    return 1;
}

WINAPI int win_ChangeWindowMessageFilterEx(void* window, unsigned message, unsigned action, uint32_t* info)
{
    auto* widget = as_widget(window);
    if (!widget || action > 2 || (info && info[0] != 8))
        return 0;
    auto state = widget->property("winMessageFilter").toMap();
    auto key = QString::number(message);
    if (info)
        info[1] = state.value(key).toUInt() == action ? (action == 1 ? 1 : action == 2 ? 2 : 0) : 0;
    if (action)
        state.insert(key, action);
    else
        state.remove(key);
    widget->setProperty("winMessageFilter", state);
    return 1;
}

using WinEventProc = void (__attribute__((ms_abi)) *)(void*, uint32_t, void*, int, int, uint32_t, uint32_t);
struct EventHook {
    QMetaObject::Connection connection;
};
static QSet<EventHook*> event_hooks;
WINAPI void* win_SetWinEventHook(unsigned minimum, unsigned maximum, void* module, WinEventProc procedure,
                                  uint32_t process, uint32_t thread, unsigned flags)
{
    (void)module;
    (void)process;
    (void)thread;
    if (!procedure || minimum > maximum || (flags & 4))
        return nullptr;
    auto* hook = new EventHook;
    if (!(flags & 2) && minimum <= 3 && maximum >= 3) {
        hook->connection = QObject::connect(&application(), &QApplication::focusChanged,
            &application(), [hook, procedure](QWidget*, QWidget* widget) {
                auto* window = widget ? as_widget(widget->window()) : nullptr;
                if (window)
                    procedure(hook, 3, window, 0, 0, 0, 0);
            });
    }
    event_hooks.insert(hook);
    return hook;
}
WINAPI int win_UnhookWinEvent(EventHook* hook)
{
    if (!event_hooks.remove(hook))
        return 0;
    QObject::disconnect(hook->connection);
    delete hook;
    return 1;
}

WINAPI int16_t win_GetKeyState(int key)
{
    auto modifiers = QGuiApplication::keyboardModifiers();
    switch (key) {
    case 0x10: case 0xa0: case 0xa1: return modifiers & Qt::ShiftModifier ? INT16_MIN : 0;
    case 0x11: case 0xa2: case 0xa3: return modifiers & Qt::ControlModifier ? INT16_MIN : 0;
    case 0x12: case 0xa4: case 0xa5: return modifiers & Qt::AltModifier ? INT16_MIN : 0;
    case 1: return QGuiApplication::mouseButtons() & Qt::LeftButton ? INT16_MIN : 0;
    case 2: return QGuiApplication::mouseButtons() & Qt::RightButton ? INT16_MIN : 0;
    default: return 0;
    }
}

#include "filedialog.inc"

using TimerProc = void (__attribute__((ms_abi)) *)(void*, uint32_t, uintptr_t, uint32_t);
static QHash<QWidget*, QHash<uintptr_t, QTimer*>> timers;
static std::atomic_uintptr_t timer_id{0x10000};

static void start_timer(QWidget* widget, uintptr_t id, unsigned ms, TimerProc callback)
{
    auto* target = widget ? static_cast<QObject*>(widget) : static_cast<QObject*>(&application());
    auto& window_timers = timers[widget];
    if (auto* old = window_timers.take(id))
        old->deleteLater();
    auto* timer = new QTimer(target);
    timer->setInterval(qBound(10u, ms, unsigned(INT_MAX)));
    QObject::connect(timer, &QTimer::timeout, target, [widget, id, callback] {
        if (callback)
            callback(widget, 0x113, id, 0);
        else
            queue_message({widget, 0x113, id, 0, 0, 0, 0});
    });
    window_timers.insert(id, timer);
    timer->start();
}
WINAPI uintptr_t win_SetTimer(void* handle, uintptr_t id, unsigned ms, TimerProc callback)
{
    auto* widget = as_widget(handle);
    if (handle && !widget)
        return 0;
    if (!id || !handle)
        id = timer_id.fetch_add(1);
    if (QThread::currentThread() == application().thread())
        start_timer(widget, id, ms, callback);
    else
        QMetaObject::invokeMethod(&application(), [=] {
            if (!widget || as_widget(widget))
                start_timer(widget, id, ms, callback);
        }, Qt::QueuedConnection);
    return id;
}
WINAPI int win_KillTimer(void* handle, uintptr_t id)
{
    auto* widget = as_widget(handle);
    if (handle && !widget)
        return 0;
    auto stop = [=] {
        if (auto* timer = timers[widget].take(id)) {
            timer->stop();
            timer->deleteLater();
        }
    };
    if (QThread::currentThread() == application().thread())
        stop();
    else
        QMetaObject::invokeMethod(&application(), stop, Qt::QueuedConnection);
    return 1;
}

#include <QBrush>
static QHash<int, QBrush*> system_brushes;
WINAPI void* win_GetSysColorBrush(int index)
{
    if (auto* brush = system_brushes.value(index))
        return brush;
    uint32_t color = win_GetSysColor(index);
    auto* brush = new QBrush(QColor(color & 255, (color >> 8) & 255, (color >> 16) & 255));
    system_brushes.insert(index, brush);
    return brush;
}

static int measure_text(void* dc, const QString& text, WinRect* rect, unsigned format)
{
    if (!rect)
        return 0;
    const QFontMetrics metrics(selected_font(dc));
    int flags = 0;
    if (format & 0x10) flags |= Qt::TextWordWrap;
    if (format & 0x20) flags |= Qt::TextSingleLine;
    if (format & 0x800) flags |= Qt::TextHideMnemonic;
    else flags |= Qt::TextShowMnemonic;
    int width = qMax(1, rect->right - rect->left);
    if (!(format & 0x10) || (format & 0x20)) width = 32767;
    const auto size = metrics.boundingRect(QRect(0, 0, width, 32767), flags, text).size();
    if (format & 0x400) {
        rect->right = rect->left + size.width();
        rect->bottom = rect->top + size.height();
    }
    return size.height();
}
WINAPI int win_DrawTextW(void* dc, const char16_t* text, int count, WinRect* rect, unsigned format)
{
    if (!text)
        return 0;
    return measure_text(dc, count < 0 ? utf16(text) : QString::fromUtf16(text, count), rect, format);
}
WINAPI int win_DrawTextExW(void* dc, char16_t* text, int count, WinRect* rect, unsigned format, void* parameters)
{
    (void)parameters;
    return win_DrawTextW(dc, text, count, rect, format);
}
WINAPI int win_DrawTextExA(void* dc, char* text, int count, WinRect* rect, unsigned format, void* parameters)
{
    (void)parameters;
    if (!text)
        return 0;
    return measure_text(dc, QString::fromLocal8Bit(text, count), rect, format);
}

WINAPI int win_MessageBoxW(void* owner, const char16_t* text, const char16_t* title, uint32_t type)
{
    return win_MessageBoxExW(owner, text, title, type, 0);
}

WINAPI int win_MessageBeep(unsigned type)
{
    (void)type;
    if (!qEnvironmentVariableIsSet("RUFUS_PROBE_NONINTERACTIVE"))
        QMetaObject::invokeMethod(&application(), [] { QApplication::beep(); }, Qt::QueuedConnection);
    return 1;
}

struct FlashInfo {
    uint32_t size;
    void* window;
    uint32_t flags, count, timeout;
};
WINAPI int win_FlashWindowEx(const FlashInfo* request)
{
    if (!request || request->size != sizeof(*request))
        return 0;
    auto* widget = as_widget(request->window);
    if (!widget)
        return 0;
    if (request->flags)
        QApplication::alert(widget, request->timeout);
    return widget->isActiveWindow();
}
