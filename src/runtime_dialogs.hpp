#pragma once
inline constexpr auto runtimeDialogs = R"MOST(
static std::span<const uint8_t> resource_span(uint32_t rva, uint32_t size)
{
    const auto address = uint64_t(guest_image_base) + rva;
    for (const auto& region : regions)
        if (region.data != stack && address >= region.address
            && address + size <= uint64_t(region.address) + region.length)
            return {region.data + (address - region.address), size};
    return {};
}

struct ResourceReader {
    std::span<const uint8_t> data;
    size_t position = 0;
    bool valid = true;

    uint32_t read(unsigned size)
    {
        if (position > data.size() || size > data.size() - position) {
            valid = false;
            return 0;
        }
        uint32_t result = 0;
        memcpy(&result, data.data() + position, size);
        position += size;
        return result;
    }

    QString text(uint16_t first)
    {
        QString result;
        for (auto unit = first; unit && valid; unit = read(2)) {
            if (result.size() >= 32767) {
                valid = false;
                break;
            }
            result += QChar(unit);
        }
        return result;
    }

    void align()
    {
        position = (position + 3) & ~size_t(3);
    }
};

static std::span<const uint8_t> find_widget_resource(uint32_t type, uint32_t name)
{
    auto root = resource_span(guest_resource_rva, guest_resource_size);
    if (root.empty() || !name)
        return {};
    QString text;
    if (name > 0xffffu && !widget_string(name, text))
        return {};
    const auto child = [&](uint32_t offset, uint32_t key, const QString* named, bool fallback) {
        ResourceReader table{root, uint64_t(offset) + 12};
        const auto named_count = table.read(2);
        const auto id_count = table.read(2);
        uint32_t first = 0;
        for (uint32_t i = 0; i < named_count + id_count && table.valid; ++i) {
            const auto value = table.read(4);
            const auto target = table.read(4);
            if (!i)
                first = target;
            if (!named && value == key)
                return target;
            if (named && (value & 0x80000000u)) {
                ResourceReader label{root, value & 0x7fffffffu};
                const auto length = label.read(2);
                if (label.valid && length == named->size() && length * 2 <= root.size() - label.position) {
                    std::u16string units(length, u'\0');
                    memcpy(units.data(), root.data() + label.position, length * 2);
                    if (QString::fromStdU16String(units).compare(*named, Qt::CaseInsensitive) == 0)
                        return target;
                }
            }
        }
        return fallback ? first : 0u;
    };
    const auto types = child(0, type, nullptr, false);
    if (!(types & 0x80000000u))
        return {};
    const auto names = child(types & 0x7fffffffu, name, name > 0xffffu ? &text : nullptr, false);
    if (!(names & 0x80000000u))
        return {};
    const auto leaf = child(names & 0x7fffffffu, 0x409, nullptr, true);
    if (!leaf || (leaf & 0x80000000u))
        return {};
    ResourceReader entry{root, leaf};
    const auto rva = entry.read(4);
    const auto size = entry.read(4);
    return entry.valid && size <= 1024 * 1024 ? resource_span(rva, size) : std::span<const uint8_t>();
}

static QImage resource_control_image(uint32_t id, uint32_t style);

struct DialogField {
    uint16_t atom = 0;
    QString text;
};

static DialogField dialog_field(ResourceReader& reader)
{
    const auto first = reader.read(2);
    if (first == 0xffff)
        return {uint16_t(reader.read(2)), {}};
    return {0, reader.text(first)};
}

struct DialogControl {
    uint32_t id, style, ex_style;
    QRect geometry;
    uint16_t title_resource = 0;
    QString class_name, title;
};

struct DialogTemplate {
    uint32_t style = 0, ex_style = 0;
    QRect geometry;
    QString title, font_family;
    int font_size = 0;
    std::vector<DialogControl> controls;
};

static QRect read_dialog_rect(ResourceReader& reader)
{
    const int x = int16_t(reader.read(2)), y = int16_t(reader.read(2));
    const int width = int16_t(reader.read(2)), height = int16_t(reader.read(2));
    return {x, y, width, height};
}

static bool parse_dialog(std::span<const uint8_t> bytes, DialogTemplate& dialog)
{
    ResourceReader reader{bytes};
    const auto signature = reader.read(4);
    const bool extended = signature == 0xffff0001u;
    if (extended) {
        reader.read(4); // help ID
        dialog.ex_style = reader.read(4);
        dialog.style = reader.read(4);
    } else {
        dialog.style = signature;
        dialog.ex_style = reader.read(4);
    }
    const auto count = reader.read(2);
    dialog.geometry = read_dialog_rect(reader);
    const auto menu = dialog_field(reader);
    const auto type = dialog_field(reader);
    dialog.title = reader.text(reader.read(2));
    if (menu.atom || !menu.text.isEmpty() || type.atom || !type.text.isEmpty() || count > 512)
        return false;
    if (dialog.style & 0x40) {
        dialog.font_size = reader.read(2);
        if (extended)
            reader.read(4); // weight, italic, charset
        dialog.font_family = reader.text(reader.read(2));
    }
    for (uint32_t i = 0; i < count && reader.valid; ++i) {
        reader.align();
        DialogControl control;
        if (extended) {
            reader.read(4);
            control.ex_style = reader.read(4);
            control.style = reader.read(4);
        } else {
            control.style = reader.read(4);
            control.ex_style = reader.read(4);
        }
        control.geometry = read_dialog_rect(reader);
        control.id = reader.read(extended ? 4 : 2);
        if (!extended && control.id == 0xffff)
            control.id = ~0u;
        auto kind = dialog_field(reader);
        const auto title = dialog_field(reader);
        const auto extra = reader.read(2);
        switch (kind.atom) {
        case 0: break;
        case 0x80: kind.text = QStringLiteral("BUTTON"); break;
        case 0x81: kind.text = QStringLiteral("EDIT"); break;
        case 0x82: kind.text = QStringLiteral("STATIC"); break;
        default: return false;
        }
        if (extra)
            return false;
        control.class_name = kind.text.toUpper();
        if (control.class_name != "BUTTON" && control.class_name != "EDIT" && control.class_name != "STATIC"
            && !(common_controls_initialized && (control.class_name == "MSCTLS_PROGRESS32"
                || control.class_name == "SYSLISTVIEW32")))
            return false;
        if (title.atom && control.class_name != "STATIC")
            return false;
        control.title_resource = title.atom;
        control.title = title.text;
        dialog.controls.push_back(std::move(control));
    }
    return reader.valid;
}

class GuestDialog : public QDialog {
public:
    explicit GuestDialog(QWidget* parent) : QDialog(parent) {}
    uint32_t guest_handle = 0;
    void reject() override { queue_window_message(guest_handle, 0x10, 0, 0); }
protected:
    void paintEvent(QPaintEvent* event) override
    {
        QDialog::paintEvent(event);
        paint_guest_surface(guest_handle, this);
    }
    void resizeEvent(QResizeEvent* event) override
    {
        QDialog::resizeEvent(event);
        invalidate_guest_surface(guest_handle, rect(), true);
    }
};

struct ModalDialogState { bool finished = false; uint32_t result = 0; };
static std::map<uint32_t, ModalDialogState> modal_dialogs;

static bool finish_modal_dialog(uint32_t handle, uint32_t result)
{
    const auto it = modal_dialogs.find(handle);
    if (it == modal_dialogs.end())
        return false;
    it->second = {true, result};
    if (auto* widget = widget_at(handle))
        widget->hide();
    return true;
}

static uint32_t win_EndDialog(uint32_t* sp)
{
    const auto handle = pop32(sp), result = pop32(sp);
    return finish_modal_dialog(handle, result) ? 1 : gui_error(1400);
}

static uint32_t create_dialog(uint32_t* sp, bool modal)
{
    const auto instance = pop32(sp), name = pop32(sp), owner_handle = pop32(sp);
    const auto procedure = pop32(sp), parameter = pop32(sp);
    if ((instance && instance != guest_image_base) || !is_guest_callback(procedure)) {
        if (trace_guest())
            fprintf(stderr, "Most PE32: dialog %u, unsupported instance or procedure 0x%08x\n",
                    name, procedure);
        return gui_error(87);
    }
    auto* owner = owner_handle ? widget_at(owner_handle) : nullptr;
    if (owner_handle && (!owner || guest_widgets.at(owner_handle).destroying))
        return gui_error(1400);
    DialogTemplate model;
    const auto resource = find_widget_resource(5, name);
    if (resource.empty())
        return gui_error(1814);
    if (!parse_dialog(resource, model)) {
        if (trace_guest())
            fprintf(stderr, "Most PE32: dialog %u, unsupported template\n", name);
        return gui_error(50);
    }
    if (guest_widgets.size() + model.controls.size() >= 4096
        || next_widget + model.controls.size() >= 0x72000000u)
        return gui_error(8);
    widget_app();
    auto* dialog = new GuestDialog(owner);
    const auto handle = next_widget++;
    dialog->guest_handle = handle;
    dialog->setWindowTitle(model.title);
    if (model.font_size > 0 && !model.font_family.isEmpty()) {
        QFont font(model.font_family, model.font_size);
        dialog->setFont(font);
    }
    const QFontMetrics metrics(dialog->font());
    const auto horizontal = (metrics.horizontalAdvance(QStringLiteral(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")) / 26 + 1) / 2;
    const auto vertical = metrics.height();
    const auto scale = [&](const QRect& rect) {
        return QRect(rect.x() * horizontal / 4, rect.y() * vertical / 8,
                     rect.width() * horizontal / 4, rect.height() * vertical / 8);
    };
    const bool child_dialog = model.style & 0x40000000u;
    if (child_dialog && owner) {
        dialog->setWindowFlags(Qt::Widget);
        dialog->setGeometry(scale(model.geometry));
    } else {
        dialog->resize(scale(model.geometry).size());
    }
    dialog->setWindowModality(modal ? Qt::ApplicationModal : Qt::NonModal);
    auto& entry = guest_widgets.emplace(handle, GuestWidget{dialog, QStringLiteral("#32770"),
        owner_handle, 0, instance, model.style, model.ex_style}).first->second;
    entry.procedure = procedure;
    entry.dialog = true;
    entry.owns_widget = true;
    uint32_t focus = 0;
    for (const auto& control : model.controls) {
        const auto rect = scale(control.geometry);
        QWidget* widget;
        if (control.class_name == "BUTTON") {
            widget = create_guest_button(dialog, control.style);
            if (!widget) {
                if (trace_guest())
                    fprintf(stderr, "Most PE32: dialog %u, unsupported button style 0x%08x\n",
                            name, control.style);
                destroy_widget(sp, handle);
                return gui_error(50);
            }
        } else if (control.class_name == "MSCTLS_PROGRESS32") {
            auto* bar = new QProgressBar(dialog);
            bar->setTextVisible(false);
            bar->setValue(0);
            widget = bar;
        } else if (control.class_name == "SYSLISTVIEW32") {
            auto* view = new QTreeWidget(dialog);
            view->setRootIsDecorated(false);
            view->setHeaderHidden(true);
            widget = view;
        } else if (control.class_name == "EDIT") {
            widget = create_guest_edit(dialog, control.style);
        } else {
            auto* label = new QLabel(dialog);
            label->setTextFormat(Qt::PlainText);
            widget = label;
        }
        widget_set_text(widget, control.title);
        if (control.title_resource) {
            const auto image = resource_control_image(control.title_resource, control.style);
            if (!image.isNull())
                static_cast<QLabel*>(widget)->setPixmap(QPixmap::fromImage(image));
        }
        widget->setGeometry(rect);
        widget->setEnabled(!(control.style & 0x08000000u));
        widget->setVisible(control.style & 0x10000000u);
        const auto child = next_widget++;
        guest_widgets.emplace(child, GuestWidget{widget, control.class_name, handle,
            control.id, instance, control.style, control.ex_style});
        if (!focus && (control.style & 0x10010000u) == 0x10010000u && widget->isEnabled())
            focus = child;
        wire_widget_notifications(widget, handle, control.id, child);
    }
    if (modal)
        modal_dialogs.emplace(handle, ModalDialogState{});
    if (trace_guest())
        fprintf(stderr, "Most PE32: dialog %u created (%zu controls)\n", name, model.controls.size());
    const auto init = call_guest(sp, procedure, handle, 0x110, focus, parameter);
    if (guest_widgets.contains(handle) && (!modal || !modal_dialogs.at(handle).finished)) {
        if (init && focus && guest_widgets.contains(focus))
            widget_at(focus)->setFocus();
        if (modal || (model.style & 0x10000000u))
            dialog->show();
    }
    return handle;
}

static uint32_t win_CreateDialogParamW(uint32_t* sp)
{
    const auto handle = create_dialog(sp, false);
    return guest_widgets.contains(handle) ? handle : 0;
}

static uint32_t win_DialogBoxParamW(uint32_t* sp)
{
    const auto handle = create_dialog(sp, true);
    if (!handle)
        return ~0u;
    auto& state = modal_dialogs.at(handle);
    while (!state.finished && guest_widgets.contains(handle)) {
        const auto it = find_message(handle, 0, 0);
        if (it == messages.end()) {
            widget_app().processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents);
            continue;
        }
        if (it->message == 0x12)
            break;
        const auto item = *it;
        messages.erase(it);
        deliver_message(sp, item);
    }
    const auto result = state.finished ? state.result : 0;
    modal_dialogs.erase(handle);
    if (guest_widgets.contains(handle))
        destroy_widget(sp, handle);
    return result;
}
)MOST";
