#include <QAction>
#include <QImage>
#include <QToolBar>
#include <QVector>

extern "C" __attribute__((ms_abi)) const void* win_FindResourceA(void*, const char*, const char*);
extern "C" __attribute__((ms_abi)) const void* win_LoadResource(void*, const void*);
extern "C" __attribute__((ms_abi)) uint32_t win_SizeofResource(void*, const void*);

static QHash<void*, QIcon> guest_icons;
struct ImageList { int width, height; QVector<QIcon> icons; };
static QSet<ImageList*> image_lists;
static QHash<QToolBar*, ImageList*> toolbar_images;

static void write_le(QByteArray& bytes, int offset, uint32_t value, int count)
{
    for (int i = 0; i < count; ++i)
        bytes[offset + i] = char(value >> (i * 8));
}
static uint32_t read_le(const unsigned char* bytes, int count)
{
    uint32_t value = 0;
    for (int i = 0; i < count; ++i)
        value |= uint32_t(bytes[i]) << (i * 8);
    return value;
}
static void* load_icon(const unsigned char* bytes, uint32_t size, int width, int height)
{
    if (!bytes || size < 8 || size > 16 * 1024 * 1024)
        return nullptr;
    QImage image;
    if (!image.loadFromData(bytes, size)) {
        if (size < 40)
            return nullptr;
        int w = read_le(bytes + 4, 4), h = read_le(bytes + 8, 4) / 2;
        if (w < 1 || w > 256 || h < 1 || h > 256)
            return nullptr;
        QByteArray ico(22, '\0');
        write_le(ico, 2, 1, 2);
        write_le(ico, 4, 1, 2);
        ico[6] = char(w);
        ico[7] = char(h);
        write_le(ico, 10, 1, 2);
        write_le(ico, 12, read_le(bytes + 14, 2), 2);
        write_le(ico, 14, size, 4);
        write_le(ico, 18, 22, 4);
        ico.append(reinterpret_cast<const char*>(bytes), size);
        if (!image.loadFromData(ico, "ICO"))
            return nullptr;
    }
    if (width > 0 && height > 0)
        image = image.scaled(width, height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    auto* handle = new char;
    guest_icons.insert(handle, QIcon(QPixmap::fromImage(image)));
    return handle;
}

WINAPI void* win_CreateIconFromResourceEx(const unsigned char* bytes, uint32_t size, int icon,
                                        uint32_t version, int w, int h, uint32_t flags)
{
    (void)version; (void)flags;
    return icon ? load_icon(bytes, size, w, h) : nullptr;
}
WINAPI int win_DestroyIcon(void* icon)
{
    if (!guest_icons.remove(icon))
        return 0;
    delete static_cast<char*>(icon);
    return 1;
}
WINAPI void* win_LoadImageW(void* instance, const char16_t* name, uint32_t type, int width, int height, uint32_t flags)
{
    (void)flags;
    if (type != 1 || reinterpret_cast<uintptr_t>(name) > 0xffff)
        return nullptr;
    auto* group = win_FindResourceA(instance, reinterpret_cast<const char*>(name), reinterpret_cast<const char*>(14));
    auto* data = static_cast<const unsigned char*>(win_LoadResource(instance, group));
    uint32_t size = win_SizeofResource(instance, group);
    if (!data || size < 20)
        return nullptr;
    const unsigned count = qMin(read_le(data + 4, 2), (size - 6) / 14);
    if (!count)
        return nullptr;
    unsigned best = 0;
    int difference = INT_MAX;
    for (unsigned i = 0; i < count; ++i) {
        const auto* item = data + 6 + i * 14;
        const int w = item[0] ? item[0] : 256;
        const int h = item[1] ? item[1] : 256;
        const int delta = qAbs(w - (width ? width : 32)) + qAbs(h - (height ? height : 32));
        if (delta < difference) {
            best = i;
            difference = delta;
        }
    }
    auto id = read_le(data + 6 + best * 14 + 12, 2);
    auto* resource = win_FindResourceA(instance, reinterpret_cast<const char*>(uintptr_t(id)),
                                      reinterpret_cast<const char*>(3));
    return load_icon(static_cast<const unsigned char*>(win_LoadResource(instance, resource)),
                     win_SizeofResource(instance, resource), width, height);
}
WINAPI void* win_LoadIconW(void* instance, const char16_t* name)
{
    return win_LoadImageW(instance, name, 1, 0, 0, 0);
}

WINAPI void* win_ImageList_Create(int width, int height, unsigned flags, int initial, int grow)
{
    (void)flags; (void)initial; (void)grow;
    auto* list = new ImageList{width, height, {}};
    image_lists.insert(list);
    return list;
}
WINAPI int win_ImageList_ReplaceIcon(ImageList* list, int index, void* handle)
{
    if (!image_lists.contains(list) || !guest_icons.contains(handle) || index >= list->icons.size())
        return -1;
    if (index < 0) {
        list->icons.append(guest_icons.value(handle));
        return list->icons.size() - 1;
    }
    list->icons[index] = guest_icons.value(handle);
    return index;
}
WINAPI int win_ImageList_Destroy(ImageList* list)
{
    if (!image_lists.remove(list))
        return 0;
    for (auto& active : toolbar_images)
        if (active == list)
            active = nullptr;
    delete list;
    return 1;
}

WINAPI void* win_CreateWindowExW(uint32_t extended, const char16_t* class_name, const char16_t* text,
                                uint32_t style, int x, int y, int w, int h, void* owner,
                                uintptr_t menu, void* instance, void* parameter)
{
    (void)instance; (void)parameter;
    auto* parent = as_widget(owner);
    const auto type = utf16(class_name);
    QWidget* widget;
    if (!type.compare(QStringLiteral("ToolbarWindow32"), Qt::CaseInsensitive)) {
        auto* toolbar = new QToolBar(parent);
        toolbar->setMovable(false);
        widget = toolbar;
    } else {
        std::fprintf(stderr, "Unsupported CreateWindowEx class %s\n", qPrintable(type));
        return nullptr;
    }
    window_classes[widget] = type;
    window_data[widget][-16] = style;
    window_data[widget][-20] = extended;
    window_data[widget][-12] = menu;
    widget->setWindowTitle(utf16(text));
    widget->setGeometry(x, y, w, h);
    widget->setVisible(style & 0x10000000);
    if (auto* dialog = dialogs.value(parent, nullptr))
        dialog->controls.insert(menu, widget);
    return widget;
}

struct ToolbarButton {
    int32_t image, command;
    uint8_t state, style, reserved[6];
    uintptr_t data, text;
};
static_assert(sizeof(ToolbarButton) == 32);

static bool toolbar_message(QToolBar* bar, uint32_t message, uintptr_t w, intptr_t l, intptr_t& result)
{
    result = 0;
    switch (message) {
    case 0x401:
        for (auto* action : bar->actions())
            if (action->data().toInt() == static_cast<int>(w)) {
                action->setEnabled(l & 0xffff);
                result = 1;
                break;
            }
        return true;
    case 0x41f:
        bar->setMinimumHeight(uint16_t(l >> 16));
        bar->setProperty("winButtonWidth", int(uint16_t(l)));
        result = 1;
        return true;
    case 0x456:
        result = bar->property("winPadding").toLongLong();
        return true;
    case 0x457:
        result = bar->property("winPadding").toLongLong();
        bar->setProperty("winPadding", qlonglong(l));
        return true;
    case 0x463:
        if (auto* size = reinterpret_cast<WinSize*>(l)) {
            const auto preferred = bar->sizeHint();
            size->width = preferred.width();
            size->height = preferred.height();
            result = 1;
        }
        return true;
    case 0x41e: return true; // TB_BUTTONSTRUCTSIZE
    case 0x430: {
        auto* list = reinterpret_cast<ImageList*>(l);
        result = reinterpret_cast<intptr_t>(toolbar_images.value(bar));
        if (image_lists.contains(list)) {
            toolbar_images.insert(bar, list);
            bar->setIconSize(QSize(list->width, list->height));
        }
        return true;
    }
    case 0x444: case 0x414: {
        auto* buttons = reinterpret_cast<const ToolbarButton*>(l);
        if (w > 128 || !buttons) return true;
        auto* list = toolbar_images.value(bar);
        for (uintptr_t i = 0; i < w; ++i) {
            const auto& button = buttons[i];
            if (button.style & 1) {
                bar->addSeparator();
                continue;
            }
            QAction* action = new QAction(bar);
            if (list && button.image >= 0 && button.image < list->icons.size())
                action->setIcon(list->icons[button.image]);
            if (button.text > 0xffff && button.text != UINTPTR_MAX)
                action->setText(message == 0x444 ? utf16(reinterpret_cast<const char16_t*>(button.text))
                    : QString::fromLocal8Bit(reinterpret_cast<const char*>(button.text)));
            action->setEnabled(button.state & 4);
            action->setVisible(!(button.state & 8));
            action->setData(button.command);
            bar->addAction(action);
            QObject::connect(action, &QAction::triggered, bar, [bar, id = button.command] {
                auto* dialog = dialogs.value(bar->parentWidget(), nullptr);
                if (dialog && dialog->proc)
                    dialog->proc(dialog->window, 0x111, id & 0xffff, reinterpret_cast<intptr_t>(bar));
            });
        }
        bar->adjustSize();
        result = 1;
        return true;
    }
    case 0x421: bar->adjustSize(); return true;
    default: return false;
    }
}
