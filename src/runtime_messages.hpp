#pragma once
inline constexpr auto runtimeMessages = R"MOST(
static uint32_t callback_depth;

static uint32_t call_guest(uint32_t* sp, uint32_t proc, uint32_t first,
                           uint32_t second, uint32_t third, uint32_t fourth)
{
    if (!is_guest_callback(proc))
        return gui_error(87);
    if (callback_depth >= 64)
        guest_fault();
    ++callback_depth;
    uint32_t frame = *sp;
    push32(&frame, fourth);
    push32(&frame, third);
    push32(&frame, second);
    push32(&frame, first);
    push32(&frame, 0);
    const auto result = run_guest_code(proc, frame);
    --callback_depth;
    if (frame != *sp)
        guest_fault();
    return result;
}

static const GuestClass* find_guest_class(uint32_t address, uint32_t instance)
{
    if (address && address <= 0xffffu) {
        for (const auto& [name, entry] : guest_classes)
            if (entry.atom == address && (!instance || instance == entry.info[4]))
                return &entry;
        return nullptr;
    }
    QString name;
    if (!widget_string(address, name))
        return nullptr;
    const auto it = guest_classes.find(name.toUpper());
    return it != guest_classes.end() && (!instance || it->second.info[4] == instance)
        ? &it->second : nullptr;
}

static uint32_t win_RegisterClassW(uint32_t* sp)
{
    const auto address = pop32(sp);
    if (!address)
        return gui_error(87);
    std::array<uint32_t, 10> info;
    memcpy(info.data(), memory(address, sizeof info, 0), sizeof info);
    QString name;
    if (!is_guest_callback(info[1]) || !widget_string(info[9], name) || name.isEmpty()
        || name.size() > 255 || info[2] || info[3] || info[8])
        return gui_error(87);
    name = name.toUpper();
    if (name == "STATIC" || name == "BUTTON" || name == "EDIT" || guest_classes.contains(name))
        return gui_error(1410);
    if (guest_classes.size() >= 4096)
        return gui_error(8);
    const uint32_t atom = 0xc000 + guest_classes.size();
    guest_classes.emplace(name, GuestClass{info, atom});
    return atom;
}

static uint32_t win_GetClassInfoW(uint32_t* sp)
{
    const auto instance = pop32(sp), address = pop32(sp), output = pop32(sp);
    if (!output)
        return gui_error(87);
    const auto* entry = find_guest_class(address, instance);
    if (!entry)
        return gui_error(1411);
    memcpy(memory(output, sizeof entry->info, 1), entry->info.data(), sizeof entry->info);
    return 1;
}

static uint32_t win_SetWindowLongW(uint32_t* sp)
{
    auto* entry = widget_entry(pop32(sp));
    const int index = pop32(sp);
    const auto value = pop32(sp);
    if (!entry)
        return 0;
    switch (index) {
    case -4: {
        if (value != default_window_proc && !is_guest_callback(value))
            return gui_error(87);
        const auto previous = entry->procedure ? entry->procedure : default_window_proc;
        entry->procedure = value == default_window_proc ? 0 : value;
        return previous;
    }
    case -12: return std::exchange(entry->id, value);
    case -21: return std::exchange(entry->user_data, value);
    default: return gui_error(1413);
    }
}

static bool finish_modal_dialog(uint32_t, uint32_t);

static uint32_t default_window_message(uint32_t* sp, uint32_t handle,
                                       uint32_t message, uint32_t wparam, uint32_t lparam)
{
    auto* widget = widget_at(handle);
    if (!widget)
        return 0;
    if (message == 0x467 && wparam) { // BFFM_SETSELECTIONW
        if (auto* chooser = qobject_cast<QFileDialog*>(widget)) {
            const auto path = guest_path(lparam);
            if (!path.empty())
                chooser->setDirectory(QString::fromStdU16String(path.u16string()));
            return 0;
        }
    }
    if (auto* bar = qobject_cast<QProgressBar*>(widget)) {
        switch (message) {
        case 0x401: {
            const auto previous = (uint32_t(bar->maximum()) << 16) | (uint32_t(bar->minimum()) & 0xffffu);
            bar->setRange(int16_t(lparam), int16_t(lparam >> 16));
            return previous;
        }
        case 0x402: {
            const int previous = bar->value();
            bar->setValue(std::clamp(int(wparam), bar->minimum(), bar->maximum()));
            return previous;
        }
        case 0x406: {
            const auto previous = (uint32_t(bar->maximum()) << 16) | (uint32_t(bar->minimum()) & 0xffffu);
            bar->setRange(int(wparam), int(lparam));
            return previous;
        }
        case 0x408: return bar->value();
        default: break;
        }
    }
    switch (message) {
    case 0x81: return 1; // WM_NCCREATE
    case 0x10:
        if (!finish_modal_dialog(handle, 2))
            destroy_widget(sp, handle);
        return 0;
    case 0x0f:
        guest_widgets.at(handle).dirty = {};
        guest_widgets.at(handle).erase = false;
        return 0;
    case 0x30: case 0x31:
        return widget_font_message(guest_widgets.at(handle), message, wparam, lparam);
    case 0x7f: case 0x80: case 0x172: case 0x173:
        return widget_image_message(guest_widgets.at(handle), message, wparam, lparam);
    case 0x0c: return set_widget_text(handle, lparam);
    case 0x0e: return widget_text(widget).size();
    case 0x0d: {
        if (!wparam || !lparam)
            return 0;
        const auto text = widget_text(widget);
        const auto length = std::min<qsizetype>(text.size(), wparam - 1);
        auto* output = memory(lparam, (length + 1) * 2, 1);
        memcpy(output, text.utf16(), length * 2);
        memset(output + length * 2, 0, 2);
        return length;
    }
    case 0xf0: // BM_GETCHECK
        if (auto* check = qobject_cast<QCheckBox*>(widget))
            return check->checkState() == Qt::Checked ? 1 : check->checkState() == Qt::PartiallyChecked ? 2 : 0;
        return 0;
    case 0xf1: // BM_SETCHECK
        if (auto* check = qobject_cast<QCheckBox*>(widget); check && wparam <= 2)
            check->setCheckState(wparam == 0 ? Qt::Unchecked : wparam == 1 ? Qt::Checked : Qt::PartiallyChecked);
        return 0;
    default: return 0;
    }
}

static uint32_t dispatch_window(uint32_t* sp, uint32_t handle,
                                uint32_t message, uint32_t wparam, uint32_t lparam)
{
    const auto* entry = widget_entry(handle);
    if (!entry)
        return 0;
    if (entry->procedure) {
        const bool dialog = entry->dialog;
        const auto result = call_guest(sp, entry->procedure, handle, message, wparam, lparam);
        if (!dialog || result || !guest_widgets.contains(handle))
            return result;
    }
    return default_window_message(sp, handle, message, wparam, lparam);
}

static uint32_t win_DefWindowProcW(uint32_t* sp)
{
    const auto handle = pop32(sp), message = pop32(sp), wparam = pop32(sp), lparam = pop32(sp);
    return default_window_message(sp, handle, message, wparam, lparam);
}

static uint32_t win_CallWindowProcW(uint32_t* sp)
{
    const auto proc = pop32(sp);
    if (proc == default_window_proc)
        return win_DefWindowProcW(sp);
    const auto handle = pop32(sp), message = pop32(sp), wparam = pop32(sp), lparam = pop32(sp);
    return call_guest(sp, proc, handle, message, wparam, lparam);
}

static uint32_t win_SendMessageW(uint32_t* sp)
{
    const auto handle = pop32(sp), message = pop32(sp), wparam = pop32(sp), lparam = pop32(sp);
    return dispatch_window(sp, handle, message, wparam, lparam);
}

static uint32_t win_SendMessageTimeoutW(uint32_t* sp)
{
    const auto handle = pop32(sp), message = pop32(sp), wparam = pop32(sp), lparam = pop32(sp);
    const auto flags = pop32(sp);
    pop32(sp); // The timeout does not apply to windows on the calling thread.
    const auto output = pop32(sp);
    if ((flags & ~0x2bu) || !widget_at(handle))
        return 0;
    const auto result = dispatch_window(sp, handle, message, wparam, lparam);
    if (output)
        store32(output, result);
    return 1;
}

struct GuestMessage {
    uint32_t window, message, wparam, lparam, time, x, y;
};
static std::deque<GuestMessage> messages;
static uint32_t message_position;

static void queue_window_message(uint32_t handle, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    uint32_t scratch = 0;
    const auto time = win_GetTickCount(&scratch);
    const auto point = widget_app().primaryScreen() ? QCursor::pos() : QPoint();
    const auto same = [&](const GuestMessage& item) {
        return item.window == handle && item.message == message
            && (message != 0x113 || item.wparam == wparam);
    };
    if (message == 0x12)
        std::erase_if(messages, same);
    else if (messages.size() >= 4096 ||
        ((message == 0x113 || message == 0x10 || message == 0x0f) && std::any_of(messages.begin(), messages.end(), same)))
        return;
    messages.push_back({handle, message, wparam, lparam, time, uint32_t(point.x()), uint32_t(point.y())});
}

static uint32_t destroy_widget(uint32_t* sp, uint32_t handle)
{
    auto* entry = widget_entry(handle);
    if (!entry)
        return 0;
    if (entry->destroying)
        return 1;
    entry->destroying = true;
    dispatch_window(sp, handle, 2, 0, 0);
    std::vector<uint32_t> children;
    for (const auto& [id, child] : guest_widgets)
        if (child.parent == handle)
            children.push_back(id);
    for (const auto child : children)
        if (guest_widgets.contains(child))
            destroy_widget(sp, child);
    dispatch_window(sp, handle, 0x82, 0, 0);
    guest_menus.erase(entry->system_menu);
    release_guest_dc(handle);
    delete entry->widget.data();
    guest_widgets.erase(handle);
    std::erase_if(messages, [=](const GuestMessage& item) { return item.window == handle; });
    return 1;
}

static uint32_t win_PostQuitMessage(uint32_t* sp)
{
    queue_window_message(0, 0x12, pop32(sp), 0);
    return 0;
}

static auto find_message(uint32_t window, uint32_t minimum, uint32_t maximum)
{
    const auto matches = [&](const GuestMessage& item) {
        if (item.message == 0x12)
            return true;
        if ((minimum || maximum) && (item.message < minimum || item.message > maximum))
            return false;
        if (!window || window == item.window)
            return true;
        if (window == ~0u)
            return item.window == 0;
        for (auto handle = item.window; handle;) {
            const auto it = guest_widgets.find(handle);
            if (it == guest_widgets.end())
                break;
            handle = it->second.parent;
            if (handle == window)
                return true;
        }
        return false;
    };
    return std::find_if(messages.begin(), messages.end(), matches);
}

static uint32_t win_PeekMessageW(uint32_t* sp)
{
    const auto address = pop32(sp), window = pop32(sp), minimum = pop32(sp), maximum = pop32(sp);
    const auto flags = pop32(sp);
    if (!address || (flags & ~3u))
        return gui_error(87);
    if (window && window != ~0u && !widget_at(window))
        return 0;
    widget_app().processEvents();
    const auto it = find_message(window, minimum, maximum);
    if (it == messages.end())
        return 0;
    memcpy(memory(address, sizeof *it, 1), &*it, sizeof *it);
    message_position = (it->x & 0xffffu) | (it->y << 16);
    if (flags & 1)
        messages.erase(it);
    return 1;
}

static uint32_t deliver_message(uint32_t* sp, const GuestMessage& item)
{
    if (item.message == 0x113 && item.lparam)
        return call_guest(sp, item.lparam, item.window, item.message, item.wparam, item.time);
    return item.window && item.message != 0x12
        ? dispatch_window(sp, item.window, item.message, item.wparam, item.lparam) : 0;
}

static uint32_t win_DispatchMessageW(uint32_t* sp)
{
    const auto address = pop32(sp);
    if (!address)
        return gui_error(87);
    GuestMessage item;
    memcpy(&item, memory(address, sizeof item, 0), sizeof item);
    return deliver_message(sp, item);
}

static uint32_t win_GetMessagePos(uint32_t*)
{
    return message_position;
}

static uint32_t win_SetTimer(uint32_t* sp)
{
    const auto handle = pop32(sp), requested = pop32(sp), delay = pop32(sp), proc = pop32(sp);
    auto* owner = handle ? static_cast<QObject*>(widget_at(handle)) : &widget_app();
    if (!owner || (proc && !is_guest_callback(proc)))
        return gui_error(87);
    static uint32_t next_id = 0x10000;
    const auto id = handle && requested ? requested : next_id++;
    const auto name = QStringLiteral("most-timer-%1").arg(id);
    auto* timer = owner->findChild<QTimer*>(name, Qt::FindDirectChildrenOnly);
    if (timer) {
        QObject::disconnect(timer, nullptr, owner, nullptr);
    } else {
        if (owner->findChildren<QTimer*>(Qt::FindDirectChildrenOnly).size() >= 256)
            return gui_error(8);
        timer = new QTimer(owner);
        timer->setObjectName(name);
    }
    QObject::connect(timer, &QTimer::timeout, owner, [=] { queue_window_message(handle, 0x113, id, proc); });
    timer->start(std::clamp(delay, 10u, 0x7fffffffu));
    return id;
}
)MOST";
