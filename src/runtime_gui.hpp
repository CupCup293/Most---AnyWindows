#pragma once
inline constexpr auto widgetHeaders = R"MOST(
#include <QApplication>
#include <QCheckBox>
#include <QGroupBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTreeWidget>
#include <QMenu>
#include <QMessageBox>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSaveFile>
#include <QPainter>
#include <QResizeEvent>
#include <QPointer>
#include <QPushButton>
#include <QProgressBar>
#include <QScreen>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>
#include <QCursor>
#include <QDialog>
#include <QEventLoop>
#include <QFontMetrics>
#include <span>
#include <optional>
#include <array>
#include <deque>
#include <utility>
)MOST";

inline constexpr auto runtimeGui = R"MOST(
struct GuestWidget {
    QPointer<QWidget> widget;
    QString class_name;
    uint32_t parent, id, instance, style, ex_style;
    uint32_t system_menu = 0;
    uint32_t procedure = 0, user_data = 0;
    bool destroying = false, dialog = false, owns_widget = false;
    QRect dirty;
    bool erase = false;
    QImage surface;
    uint32_t bitmap = 0, icon_small = 0, icon_large = 0, font = 0;
};

static std::map<uint32_t, GuestWidget> guest_widgets;
static std::map<uint32_t, std::unique_ptr<QMenu>> guest_menus;
static uint32_t next_widget = 0x71000000u;
static uint32_t next_menu = 0x72000000u;

struct GuestClass {
    std::array<uint32_t, 10> info;
    uint32_t atom;
};
static std::map<QString, GuestClass> guest_classes;
static bool common_controls_initialized;
static constexpr uint32_t default_window_proc = 0xffff0100u;
static std::optional<Qt::CursorShape> stock_cursor(uint32_t);
static uint32_t dispatch_window(uint32_t*, uint32_t, uint32_t, uint32_t, uint32_t);
static uint32_t destroy_widget(uint32_t*, uint32_t);
static void queue_window_message(uint32_t, uint32_t, uint32_t, uint32_t);
static void invalidate_guest_surface(uint32_t, QRect, bool);
static void paint_guest_surface(uint32_t, QWidget*);
static void release_guest_dc(uint32_t);
static uint32_t widget_font_message(GuestWidget&, uint32_t, uint32_t, uint32_t);
static uint32_t widget_image_message(GuestWidget&, uint32_t, uint32_t, uint32_t);

class GuestTopWindow : public QWidget {
public:
    explicit GuestTopWindow(QWidget* parent) : QWidget(parent) {}
    uint32_t guest_handle = 0;
protected:
    void paintEvent(QPaintEvent* event) override
    {
        QWidget::paintEvent(event);
        paint_guest_surface(guest_handle, this);
    }
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        invalidate_guest_surface(guest_handle, rect(), true);
    }
    void closeEvent(QCloseEvent* event) override
    {
        event->ignore();
        queue_window_message(guest_handle, 0x10, 0, 0);
    }
};

static std::unique_ptr<QApplication> widget_application;

static QApplication& widget_app()
{
    if (!guest_main_thread)
        guest_fault();
    static int argc = 1;
    static char name[] = "most-guest";
    static char* argv[] = {name, nullptr};
    if (!widget_application) {
        widget_application = std::make_unique<QApplication>(argc, argv);
        widget_application->setQuitOnLastWindowClosed(false);
    }
    return *widget_application;
}

static void finish_widgets()
{
    guest_menus.clear();
    for (const auto& [handle, entry] : guest_widgets)
        if (entry.owns_widget || !entry.parent)
            delete entry.widget.data();
    guest_widgets.clear();
    widget_application.reset();
}

static uint32_t gui_error(uint32_t error)
{
    last_error = error;
    return 0;
}

static GuestWidget* widget_entry(uint32_t handle)
{
    if (!guest_main_thread)
        guest_fault();
    const auto it = guest_widgets.find(handle);
    if (it == guest_widgets.end() || !it->second.widget) {
        gui_error(1400);
        return nullptr;
    }
    return &it->second;
}

static QWidget* widget_at(uint32_t handle)
{
    auto* entry = widget_entry(handle);
    return entry ? entry->widget.data() : nullptr;
}

static bool widget_string(uint32_t address, QString& text)
{
    std::u16string value;
    if (!address) {
        text.clear();
        return true;
    }
    if (address <= 0xffffu || !read_guest_wide(address, value))
        return false;
    text = QString::fromStdU16String(value);
    return true;
}

static void widget_set_text(QWidget* widget, const QString& text)
{
    if (auto* button = qobject_cast<QAbstractButton*>(widget))
        button->setText(text);
    else if (auto* label = qobject_cast<QLabel*>(widget))
        label->setText(text);
    else if (auto* group = qobject_cast<QGroupBox*>(widget))
        group->setTitle(text);
    else if (auto* edit = qobject_cast<QLineEdit*>(widget))
        edit->setText(text);
    else if (auto* multi = qobject_cast<QPlainTextEdit*>(widget))
        multi->setPlainText(text);
    else
        widget->setWindowTitle(text);
}

static QString widget_text(QWidget* widget)
{
    if (auto* button = qobject_cast<QAbstractButton*>(widget))
        return button->text();
    if (auto* label = qobject_cast<QLabel*>(widget))
        return label->text();
    if (auto* group = qobject_cast<QGroupBox*>(widget))
        return group->title();
    if (auto* edit = qobject_cast<QLineEdit*>(widget))
        return edit->text();
    if (auto* multi = qobject_cast<QPlainTextEdit*>(widget))
        return multi->toPlainText();
    return widget->windowTitle();
}

static QWidget* create_guest_button(QWidget* parent, uint32_t style)
{
    const auto type = style & 15;
    if (type == 2 || type == 3 || type == 5 || type == 6) {
        auto* check = new QCheckBox(parent);
        check->setTristate(type == 5 || type == 6);
        return check;
    }
    if (type <= 1) {
        auto* button = new QPushButton(parent);
        button->setDefault(type == 1);
        return button;
    }
    if (type == 7) {
        auto* group = new QGroupBox(parent);
        group->setAttribute(Qt::WA_TransparentForMouseEvents);
        group->lower();
        return group;
    }
    return nullptr;
}

static QWidget* create_guest_edit(QWidget* parent, uint32_t style)
{
    if (style & 4) {
        auto* edit = new QPlainTextEdit(parent);
        edit->setReadOnly(style & 0x800);
        edit->setLineWrapMode(style & 0x80 ? QPlainTextEdit::NoWrap : QPlainTextEdit::WidgetWidth);
        return edit;
    }
    auto* edit = new QLineEdit(parent);
    edit->setReadOnly(style & 0x800);
    if (style & 0x20)
        edit->setEchoMode(QLineEdit::Password);
    return edit;
}

static void wire_widget_notifications(QWidget* widget, uint32_t parent, uint32_t id, uint32_t handle)
{
    if (!parent)
        return;
    const auto command = id & 0xffffu;
    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        QObject::connect(button, &QAbstractButton::clicked, button, [=] {
            queue_window_message(parent, 0x111, command, handle);
        });
    } else {
        const auto changed = [=] {
            queue_window_message(parent, 0x111, command | (0x300u << 16), handle);
        };
        if (auto* edit = qobject_cast<QLineEdit*>(widget))
            QObject::connect(edit, &QLineEdit::textChanged, edit, changed);
        else if (auto* multi = qobject_cast<QPlainTextEdit*>(widget))
            QObject::connect(multi, &QPlainTextEdit::textChanged, multi, changed);
    }
}

static uint32_t win_CreateWindowExW(uint32_t* sp)
{
    const auto ex_style = pop32(sp), class_address = pop32(sp), title_address = pop32(sp);
    const auto style = pop32(sp);
    const int x = pop32(sp), y = pop32(sp), width = pop32(sp), height = pop32(sp);
    const auto parent_handle = pop32(sp), menu_or_id = pop32(sp), instance = pop32(sp);
    const auto parameter = pop32(sp);
    const bool child = style & 0x40000000u;
    const auto* owner = parent_handle ? widget_entry(parent_handle) : nullptr;
    if (owner && owner->destroying)
        return gui_error(1400);
    QWidget* parent = owner ? owner->widget.data() : nullptr;
    if ((parent_handle || child) && !parent)
        return gui_error(1400);
    if ((!child && (parent || menu_or_id)) || ex_style)
        return gui_error(50);
    QString class_name, title;
    if (class_address && class_address <= 0xffffu) {
        for (const auto& [name, entry] : guest_classes)
            if (entry.atom == class_address) {
                class_name = name;
                break;
            }
    } else if (!widget_string(class_address, class_name)) {
        return gui_error(1407);
    }
    if (!widget_string(title_address, title))
        return gui_error(87);
    class_name = class_name.toUpper();
    const auto registered = guest_classes.find(class_name);
    const bool custom = registered != guest_classes.end();
    const bool progress = common_controls_initialized && class_name == "MSCTLS_PROGRESS32";
    if (!custom && !progress && class_name != "STATIC" && class_name != "BUTTON" && class_name != "EDIT")
        return gui_error(1407);
    if (parameter && !custom)
        return gui_error(50);
    if (guest_widgets.size() >= 4096 || next_widget == 0x72000000u)
        return gui_error(8);
    widget_app();
    QWidget* widget;
    if (custom) {
        widget = new GuestTopWindow(parent);
    } else if (progress) {
        auto* bar = new QProgressBar(parent);
        bar->setTextVisible(false);
        bar->setValue(0);
        widget = bar;
    } else if (class_name == "BUTTON") {
        widget = create_guest_button(parent, style);
        if (!widget)
            return gui_error(50);
    } else if (class_name == "EDIT") {
        widget = create_guest_edit(parent, style);
    } else {
        auto* label = new QLabel(parent);
        label->setTextFormat(Qt::PlainText);
        widget = label;
    }
    widget_set_text(widget, title);
    if (!child)
        widget->setWindowTitle(title);
    if (x != INT32_MIN && y != INT32_MIN)
        widget->move(x, y);
    if (width != INT32_MIN && height != INT32_MIN)
        widget->resize(std::max(0, width), std::max(0, height));
    widget->setEnabled(!(style & 0x08000000u));
    const auto handle = next_widget++;
    auto& entry = guest_widgets.emplace(handle, GuestWidget{widget, class_name, parent_handle,
                                        child ? menu_or_id : 0, instance, style, ex_style}).first->second;
    if (custom) {
        static_cast<GuestTopWindow*>(widget)->guest_handle = handle;
        entry.procedure = registered->second.info[1];
        if (const auto cursor = stock_cursor(registered->second.info[6]))
            widget->setCursor(*cursor);
        const uint32_t fields[] = {parameter, instance, menu_or_id, parent_handle,
            uint32_t(height), uint32_t(width), uint32_t(y), uint32_t(x), style,
            title_address, class_address, ex_style};
        const uint32_t old_sp = *sp;
        *sp -= sizeof fields;
        const auto create = *sp;
        memcpy(memory(create, sizeof fields, 1), fields, sizeof fields);
        const bool accepted = dispatch_window(sp, handle, 0x81, 0, create)
            && guest_widgets.contains(handle)
            && dispatch_window(sp, handle, 1, 0, create) != ~0u;
        *sp = old_sp;
        if (!accepted || !guest_widgets.contains(handle)) {
            if (guest_widgets.contains(handle))
                destroy_widget(sp, handle);
            return 0;
        }
    } else {
        wire_widget_notifications(widget, parent_handle, menu_or_id, handle);
    }
    if (style & 0x10000000u)
        widget->show();
    return handle;
}

static uint32_t win_DestroyWindow(uint32_t* sp)
{
    const auto handle = pop32(sp);
    return destroy_widget(sp, handle);
}

static uint32_t win_IsWindow(uint32_t* sp)
{
    const auto it = guest_widgets.find(pop32(sp));
    return it != guest_widgets.end() && it->second.widget;
}

static uint32_t win_IsWindowEnabled(uint32_t* sp)
{
    auto* widget = widget_at(pop32(sp));
    return widget && widget->isEnabledTo(widget->parentWidget());
}

static uint32_t win_IsWindowVisible(uint32_t* sp)
{
    auto* widget = widget_at(pop32(sp));
    return widget && widget->isVisible();
}

static uint32_t win_ShowWindow(uint32_t* sp)
{
    auto* widget = widget_at(pop32(sp));
    const auto command = pop32(sp);
    if (!widget)
        return 0;
    const bool previous = !widget->isHidden();
    switch (command) {
    case 0: widget->hide(); break;
    case 2: case 6: case 7: case 11: widget->showMinimized(); break;
    case 3: widget->showMaximized(); break;
    case 1: case 9: widget->showNormal(); break;
    case 4: case 5: case 8: case 10: widget->show(); break;
    default: return gui_error(87);
    }
    widget_app().processEvents();
    return previous;
}

static uint32_t win_EnableWindow(uint32_t* sp)
{
    auto* widget = widget_at(pop32(sp));
    const auto enable = pop32(sp);
    if (!widget)
        return 0;
    const bool was_disabled = !widget->isEnabledTo(widget->parentWidget());
    widget->setEnabled(enable != 0);
    return was_disabled;
}

static uint32_t write_rect(uint32_t destination, const QRect& rect)
{
    if (!destination)
        return gui_error(87);
    const int64_t right = int64_t(rect.x()) + rect.width();
    const int64_t bottom = int64_t(rect.y()) + rect.height();
    if (right > INT32_MAX || bottom > INT32_MAX)
        return gui_error(87);
    const int fields[] = {rect.x(), rect.y(), int(right), int(bottom)};
    memcpy(memory(destination, sizeof fields, 1), fields, sizeof fields);
    return 1;
}

static uint32_t win_GetWindowRect(uint32_t* sp)
{
    auto* widget = widget_at(pop32(sp));
    const auto destination = pop32(sp);
    if (!widget)
        return 0;
    const QRect rect = widget->isWindow() ? widget->frameGeometry()
        : QRect(widget->mapToGlobal(QPoint()), widget->size());
    return write_rect(destination, rect);
}

static uint32_t win_GetClientRect(uint32_t* sp)
{
    auto* widget = widget_at(pop32(sp));
    const auto destination = pop32(sp);
    return widget ? write_rect(destination, widget->rect()) : 0;
}

static uint32_t win_SetWindowPos(uint32_t* sp)
{
    auto* widget = widget_at(pop32(sp));
    const auto after = pop32(sp);
    const int x = pop32(sp), y = pop32(sp), width = pop32(sp), height = pop32(sp);
    const auto flags = pop32(sp);
    if (!widget)
        return 0;
    if ((flags & ~0xdfu) || (!(flags & 4) && after != 0 && after != 1))
        return gui_error(50);
    if (!(flags & 1) && (width < 0 || height < 0))
        return gui_error(87);
    if (!(flags & 2))
        widget->move(x, y);
    if (!(flags & 1))
        widget->resize(width, height);
    if (!(flags & 4)) {
        if (after == 1)
            widget->lower();
        else
            widget->raise();
    }
    if (flags & 0x80)
        widget->hide();
    else if (flags & 0x40)
        widget->show();
    widget_app().processEvents();
    return 1;
}

static uint32_t win_ScreenToClient(uint32_t* sp)
{
    auto* widget = widget_at(pop32(sp));
    const auto address = pop32(sp);
    if (!widget || !address)
        return gui_error(widget ? 87 : 1400);
    int fields[2];
    auto* data = memory(address, sizeof fields, 1);
    memcpy(fields, data, sizeof fields);
    const auto point = widget->mapFromGlobal(QPoint(fields[0], fields[1]));
    fields[0] = point.x();
    fields[1] = point.y();
    memcpy(data, fields, sizeof fields);
    return 1;
}

static uint32_t widget_child(uint32_t parent, uint32_t id)
{
    if (!widget_at(parent))
        return 0;
    for (const auto& [handle, entry] : guest_widgets)
        if (entry.widget && entry.parent == parent && entry.id == id)
            return handle;
    return gui_error(1421);
}

static uint32_t win_GetDlgItem(uint32_t* sp)
{
    const auto parent = pop32(sp), id = pop32(sp);
    return widget_child(parent, id);
}

static uint32_t win_FindWindowExW(uint32_t* sp)
{
    const auto parent = pop32(sp), after = pop32(sp);
    const auto class_address = pop32(sp), title_address = pop32(sp);
    if (parent && !widget_at(parent))
        return 0;
    if (after) {
        const auto* entry = widget_entry(after);
        if (!entry || entry->parent != parent)
            return gui_error(1400);
    }
    QString class_name, title;
    if (!widget_string(class_address, class_name) || !widget_string(title_address, title))
        return gui_error(87);
    for (auto it = guest_widgets.upper_bound(after); it != guest_widgets.end(); ++it) {
        const auto& entry = it->second;
        if (entry.widget && entry.parent == parent &&
            (!class_address || entry.class_name.compare(class_name, Qt::CaseInsensitive) == 0) &&
            (!title_address || widget_text(entry.widget).compare(title, Qt::CaseInsensitive) == 0))
            return it->first;
    }
    return 0;
}

static uint32_t set_widget_text(uint32_t handle, uint32_t address)
{
    if (!handle)
        return 0;
    auto* widget = widget_at(handle);
    if (!widget)
        return 0;
    QString text;
    if (!widget_string(address, text))
        return gui_error(87);
    widget_set_text(widget, text);
    return 1;
}

static uint32_t win_SetWindowTextW(uint32_t* sp)
{
    const auto handle = pop32(sp), address = pop32(sp);
    return set_widget_text(handle, address);
}

static uint32_t win_SetDlgItemTextW(uint32_t* sp)
{
    const auto parent = pop32(sp), id = pop32(sp), address = pop32(sp);
    return set_widget_text(widget_child(parent, id), address);
}

static uint32_t win_GetDlgItemTextW(uint32_t* sp)
{
    const auto parent = pop32(sp), id = pop32(sp), address = pop32(sp);
    const int capacity = pop32(sp);
    if (!address || capacity <= 0)
        return gui_error(87);
    const auto child = widget_child(parent, id);
    if (!child)
        return 0;
    const auto text = widget_text(widget_at(child));
    const auto count = std::min<qsizetype>(text.size(), capacity - 1);
    auto* data = memory(address, (count + 1) * 2, 1);
    memcpy(data, text.utf16(), count * 2);
    memset(data + count * 2, 0, 2);
    return count;
}

static uint32_t win_CheckDlgButton(uint32_t* sp)
{
    const auto parent = pop32(sp), id = pop32(sp), state = pop32(sp);
    const auto child = widget_child(parent, id);
    if (!child)
        return 0;
    auto* check = qobject_cast<QCheckBox*>(widget_at(child));
    if (!check || state > 2 || (state == 2 && !check->isTristate()))
        return gui_error(87);
    check->setCheckState(state == 0 ? Qt::Unchecked : state == 1 ? Qt::Checked : Qt::PartiallyChecked);
    return 1;
}

static uint32_t win_InvalidateRect(uint32_t* sp)
{
    const auto handle = pop32(sp), address = pop32(sp);
    const auto erase = pop32(sp);
    auto* widget = widget_at(handle);
    if (!widget)
        return 0;
    if (address) {
        int rect[4];
        memcpy(rect, memory(address, sizeof rect, 0), sizeof rect);
        const int64_t width = int64_t(rect[2]) - rect[0], height = int64_t(rect[3]) - rect[1];
        if (width > INT32_MAX || height > INT32_MAX)
            return gui_error(87);
        if (width > 0 && height > 0)
            invalidate_guest_surface(handle, QRect(rect[0], rect[1], width, height), erase != 0);
    } else {
        invalidate_guest_surface(handle, widget->rect(), erase != 0);
    }
    return 1;
}

static uint32_t win_SetForegroundWindow(uint32_t* sp)
{
    auto* widget = widget_at(pop32(sp));
    if (!widget || !widget->isVisible())
        return 0;
    widget->activateWindow();
    widget_app().processEvents();
    return widget->isActiveWindow();
}

static QColor guest_system_color(uint32_t index)
{
    static const std::map<unsigned, QPalette::ColorRole> roles{
        {0, QPalette::Button}, {4, QPalette::Window}, {5, QPalette::Base},
        {7, QPalette::WindowText}, {8, QPalette::Text}, {13, QPalette::Highlight},
        {14, QPalette::HighlightedText}, {15, QPalette::Button}, {16, QPalette::Dark},
        {18, QPalette::ButtonText}, {20, QPalette::Light}
    };
    const auto role = roles.find(index);
    if (role == roles.end() && index != 17)
        return {};
    const auto palette = widget_app().palette();
    return index == 17 ? palette.color(QPalette::Disabled, QPalette::Text)
                       : palette.color(role->second);
}

static uint32_t win_GetSysColor(uint32_t* sp)
{
    const auto color = guest_system_color(pop32(sp));
    if (!color.isValid())
        return gui_error(87);
    return color.red() | (color.green() << 8) | (color.blue() << 16);
}

static uint32_t win_GetSystemMetrics(uint32_t* sp)
{
    const auto index = pop32(sp);
    auto& app = widget_app();
    const auto* screen = app.primaryScreen();
    if (!screen)
        return 0;
    switch (index) {
    case 0: return screen->size().width();
    case 1: return screen->size().height();
    case 2: case 3: return app.style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    case 4: return app.style()->pixelMetric(QStyle::PM_TitleBarHeight);
    case 32: case 33: return app.style()->pixelMetric(QStyle::PM_DefaultFrameWidth);
    case 36: case 37: return app.styleHints()->mouseDoubleClickDistance();
    default: return gui_error(87);
    }
}

static uint32_t win_GetWindowLongW(uint32_t* sp)
{
    const auto* entry = widget_entry(pop32(sp));
    const int index = pop32(sp);
    if (!entry)
        return 0;
    switch (index) {
    case -4: return entry->procedure ? entry->procedure : default_window_proc;
    case -6: return entry->instance;
    case -8: return entry->parent;
    case -12: return entry->id;
    case -16:
        return (entry->style & ~0x18000000u)
            | (entry->widget->isHidden() ? 0u : 0x10000000u)
            | (entry->widget->isEnabledTo(entry->widget->parentWidget()) ? 0u : 0x08000000u);
    case -20: return entry->ex_style;
    case -21: return entry->user_data;
    default: return gui_error(1413);
    }
}

static uint32_t create_guest_menu()
{
    if (guest_menus.size() >= 4096)
        return gui_error(8);
    widget_app();
    const auto handle = next_menu++;
    guest_menus.emplace(handle, std::make_unique<QMenu>());
    return handle;
}

static uint32_t win_CreatePopupMenu(uint32_t*)
{
    return create_guest_menu();
}

static uint32_t win_AppendMenuW(uint32_t* sp)
{
    const auto handle = pop32(sp), flags = pop32(sp), id = pop32(sp), address = pop32(sp);
    const auto menu = guest_menus.find(handle);
    if (menu == guest_menus.end())
        return gui_error(1401);
    if (flags & ~0x80bu)
        return gui_error(50);
    QString title;
    if (!(flags & 0x800) && !widget_string(address, title))
        return gui_error(87);
    if (menu->second->actions().size() >= 4096)
        return gui_error(8);
    auto* action = flags & 0x800 ? menu->second->addSeparator() : menu->second->addAction(title);
    action->setData(id);
    if (flags & 8) {
        action->setCheckable(true);
        action->setChecked(true);
    }
    action->setEnabled(!(flags & 3));
    action->setProperty("win32EnabledState", flags & 3);
    return 1;
}

static uint32_t win_EnableMenuItem(uint32_t* sp)
{
    const auto handle = pop32(sp), item = pop32(sp), flags = pop32(sp);
    const auto menu = guest_menus.find(handle);
    if (menu == guest_menus.end() || (flags & ~0x403u)) {
        gui_error(menu == guest_menus.end() ? 1401 : 87);
        return ~0u;
    }
    const auto actions = menu->second->actions();
    for (qsizetype i = 0; i < actions.size(); ++i) {
        auto* action = actions[i];
        if ((flags & 0x400) ? uint32_t(i) == item : action->data().toUInt() == item) {
            const auto previous = action->property("win32EnabledState").toUInt();
            action->setEnabled(!(flags & 3));
            action->setProperty("win32EnabledState", flags & 3);
            return previous;
        }
    }
    return ~0u;
}

static uint32_t win_GetSystemMenu(uint32_t* sp)
{
    auto* entry = widget_entry(pop32(sp));
    const auto revert = pop32(sp);
    if (!entry || entry->parent || !(entry->style & 0x00080000u))
        return 0;
    if (revert) {
        guest_menus.erase(entry->system_menu);
        entry->system_menu = 0;
        return 0;
    }
    if (!entry->system_menu) {
        const auto handle = create_guest_menu();
        if (!handle)
            return 0;
        auto& menu = guest_menus.at(handle);
        auto* widget = entry->widget.data();
        auto add = [&](const QString& text, uint32_t command, void (QWidget::*action)()) {
            auto* item = menu->addAction(text, widget, action);
            item->setData(command);
        };
        add(QStringLiteral("Restore"), 0xf120, &QWidget::showNormal);
        add(QStringLiteral("Minimize"), 0xf020, &QWidget::showMinimized);
        add(QStringLiteral("Maximize"), 0xf030, &QWidget::showMaximized);
        menu->addSeparator();
        menu->addAction(QStringLiteral("Close"), widget, &QWidget::close)->setData(0xf060);
        entry->system_menu = handle;
    }
    return entry->system_menu;
}

static bool clipboard_open;
static uint32_t clipboard_window;
static bool clipboard_emptied;

static uint32_t win_OpenClipboard(uint32_t* sp)
{
    const auto window = pop32(sp);
    if (window && !widget_at(window))
        return 0;
    if (clipboard_open)
        return gui_error(5);
    widget_app();
    clipboard_window = window;
    clipboard_open = true;
    clipboard_emptied = false;
    return 1;
}

static void release_clipboard_blocks()
{
    std::erase_if(global_blocks, [](const auto& pair) {
        if (!pair.second.clipboard_owned)
            return false;
        global_bytes -= pair.second.data.size();
        return true;
    });
}

static uint32_t win_EmptyClipboard(uint32_t*)
{
    if (!clipboard_open)
        return gui_error(1418);
    widget_app().clipboard()->clear();
    release_clipboard_blocks();
    clipboard_emptied = true;
    return 1;
}

static uint32_t win_SetClipboardData(uint32_t* sp)
{
    const auto format = pop32(sp), handle = pop32(sp);
    if (!clipboard_open)
        return gui_error(1418);
    if (!clipboard_emptied || !clipboard_window || !widget_at(clipboard_window))
        return gui_error(5);
    if (format != 13)
        return gui_error(50);
    const auto block = global_block(handle);
    if (block == global_blocks.end() || !block->second.movable || block->second.clipboard_owned)
        return gui_error(6);
    if (block->second.locks)
        return gui_error(158);
    const auto& data = block->second.data;
    std::u16string text;
    const size_t limit = std::min<size_t>(data.size(), 2 * 32768);
    for (size_t i = 0; i + 1 < limit; i += 2) {
        const auto unit = char16_t(data[i] | (uint16_t(data[i + 1]) << 8));
        if (!unit) {
            const auto converted = QString::fromStdU16String(text);
            widget_app().clipboard()->setText(converted);
            release_clipboard_blocks();
            block->second.clipboard_owned = true;
            return handle;
        }
        text.push_back(unit);
    }
    return gui_error(87);
}

static uint32_t win_CloseClipboard(uint32_t*)
{
    if (!clipboard_open)
        return gui_error(1418);
    clipboard_open = false;
    clipboard_window = 0;
    widget_app().processEvents();
    return 1;
}
)MOST";
