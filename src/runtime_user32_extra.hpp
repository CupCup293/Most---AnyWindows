#pragma once
inline constexpr auto runtimeUser32Extra = R"MOST(
static constexpr uint32_t stock_cursor_base = 0x73000000u;

static std::optional<Qt::CursorShape> stock_cursor(uint32_t handle)
{
    if (handle < stock_cursor_base)
        return std::nullopt;
    switch (handle - stock_cursor_base) {
    case 32512: return Qt::ArrowCursor;
    case 32513: return Qt::IBeamCursor;
    case 32514: return Qt::WaitCursor;
    case 32515: return Qt::CrossCursor;
    case 32516: return Qt::UpArrowCursor;
    case 32642: return Qt::SizeFDiagCursor;
    case 32643: return Qt::SizeBDiagCursor;
    case 32644: return Qt::SizeHorCursor;
    case 32645: return Qt::SizeVerCursor;
    case 32646: return Qt::SizeAllCursor;
    case 32648: return Qt::ForbiddenCursor;
    case 32649: return Qt::PointingHandCursor;
    case 32650: return Qt::BusyCursor;
    case 32651: return Qt::WhatsThisCursor;
    default: return std::nullopt;
    }
}

static uint32_t win_LoadCursorW(uint32_t* sp)
{
    const auto instance = pop32(sp), id = pop32(sp);
    if (instance || id > 0xffff || !stock_cursor(stock_cursor_base + id))
        return gui_error(1814);
    return stock_cursor_base + id;
}

static uint32_t win_SetCursor(uint32_t* sp)
{
    const auto handle = pop32(sp);
    const auto shape = handle ? stock_cursor(handle) : std::optional<Qt::CursorShape>(Qt::BlankCursor);
    if (!shape)
        return gui_error(1402);
    static uint32_t current = stock_cursor_base + 32512;
    static bool overridden = false;
    auto& app = widget_app();
    if (overridden)
        app.changeOverrideCursor(QCursor(*shape));
    else
        app.setOverrideCursor(QCursor(*shape));
    overridden = true;
    return std::exchange(current, handle);
}

static uint32_t win_SetClassLongW(uint32_t* sp)
{
    const auto* entry = widget_entry(pop32(sp));
    const int index = pop32(sp);
    const auto value = pop32(sp);
    if (!entry)
        return 0;
    const auto it = guest_classes.find(entry->class_name);
    if (it == guest_classes.end())
        return gui_error(1411);
    auto& info = it->second.info;
    switch (index) {
    case -26: return std::exchange(info[0], value);
    case -24:
        if (!is_guest_callback(value))
            return gui_error(87);
        return std::exchange(info[1], value);
    case -12:
        if (value && !stock_cursor(value))
            return gui_error(1402);
        for (const auto& [handle, window] : guest_widgets)
            if (window.class_name == entry->class_name && window.widget) {
                if (const auto shape = stock_cursor(value))
                    window.widget->setCursor(*shape);
                else
                    window.widget->unsetCursor();
            }
        return std::exchange(info[6], value);
    default: return gui_error(1413);
    }
}

static uint32_t win_ExitWindowsEx(uint32_t* sp)
{
    const auto flags = pop32(sp);
    pop32(sp); // Shutdown reason.
    constexpr uint32_t operation_mask = 0x4bu;
    constexpr uint32_t modifier_mask = 0x00400014u;
    const auto operation = flags & operation_mask;
    if ((flags & ~(operation_mask | modifier_mask)) || (operation & (operation - 1))
        || ((flags & 0x00400000u) && operation != 1))
        return gui_error(87);
    return gui_error(operation ? 1314 : 5);
}

static uint32_t win_SystemParametersInfoW(uint32_t* sp)
{
    const auto action = pop32(sp), size = pop32(sp), output = pop32(sp), flags = pop32(sp);
    if (!output || flags)
        return gui_error(87);
    if (action == 0x30) {
        const auto* screen = widget_app().primaryScreen();
        return screen ? write_rect(output, screen->availableGeometry()) : 0;
    }
    if (action == 0x1f && size == 92) {
        const auto font = widget_app().font();
        uint8_t fields[92]{};
        const int32_t height = font.pixelSize() > 0 ? -font.pixelSize()
            : -QFontMetrics(font).height();
        const int32_t weight = font.weight();
        memcpy(fields, &height, 4);
        memcpy(fields + 16, &weight, 4);
        fields[20] = font.italic();
        fields[21] = font.underline();
        fields[22] = font.strikeOut();
        fields[23] = 1;
        const auto family = font.family().left(31);
        memcpy(fields + 28, family.utf16(), family.size() * 2);
        memcpy(memory(output, sizeof fields, 1), fields, sizeof fields);
        return 1;
    }
    return gui_error(50);
}

static uint32_t win_TrackPopupMenu(uint32_t* sp)
{
    const auto handle = pop32(sp), flags = pop32(sp);
    const int x = pop32(sp), y = pop32(sp);
    const auto reserved = pop32(sp), window = pop32(sp), ignored_rect = pop32(sp);
    const auto menu = guest_menus.find(handle);
    if (menu == guest_menus.end())
        return gui_error(1401);
    if (!widget_at(window))
        return 0;
    if (reserved || ignored_rect || (flags & ~0x1beu))
        return gui_error(50);
    if (menu->second->isEmpty())
        return 0;
    const auto size = menu->second->sizeHint();
    const int dx = flags & 8 ? size.width() : flags & 4 ? size.width() / 2 : 0;
    const int dy = flags & 0x20 ? size.height() : flags & 0x10 ? size.height() / 2 : 0;
    if (x < INT32_MIN + dx || y < INT32_MIN + dy)
        return gui_error(87);
    const auto* action = menu->second->exec(QPoint(x - dx, y - dy));
    const auto command = action ? action->data().toUInt() : 0;
    if (flags & 0x100)
        return command;
    if (command && !(flags & 0x80))
        queue_window_message(window, 0x111, command & 0xffff, 0);
    return 1;
}

static uint32_t win_MessageBoxIndirectW(uint32_t* sp)
{
    const auto address = pop32(sp);
    if (!address || load32(address) != 40)
        return gui_error(87);
    uint32_t fields[10];
    memcpy(fields, memory(address, sizeof fields, 0), sizeof fields);
    auto* owner = fields[1] ? widget_at(fields[1]) : nullptr;
    if (fields[1] && !owner)
        return 0;
    const auto style = fields[5];
    if (fields[6] || fields[8] || (style & ~0x1d337fu) || (style & 15) > 5)
        return gui_error(50);
    QString text, caption;
    if (!widget_string(fields[3], text) || !widget_string(fields[4], caption))
        return gui_error(87);
    using Box = QMessageBox;
    static const Box::StandardButtons buttons[]{
        Box::Ok, Box::Ok | Box::Cancel, Box::Abort | Box::Retry | Box::Ignore,
        Box::Yes | Box::No | Box::Cancel, Box::Yes | Box::No, Box::Retry | Box::Cancel
    };
    const auto icon_flags = style & 0xf0;
    if (icon_flags > 0x40)
        return gui_error(87);
    const auto icon = icon_flags == 0x10 ? Box::Critical : icon_flags == 0x20 ? Box::Question :
        icon_flags == 0x30 ? Box::Warning : icon_flags == 0x40 ? Box::Information : Box::NoIcon;
    if (trace_guest())
        fprintf(stderr, "Most PE32: message box [%s] %s\n", qPrintable(caption), qPrintable(text));
    widget_app();
    Box box(icon, caption, text, buttons[style & 15], owner);
    box.setTextFormat(Qt::PlainText);
    if (style & 0x40000)
        box.setWindowFlag(Qt::WindowStaysOnTopHint);
    if (style & 0x100000)
        box.setLayoutDirection(Qt::RightToLeft);
    const auto default_index = (style >> 8) & 3;
    if (const auto ordered = box.buttons(); qsizetype(default_index) < ordered.size())
        box.setDefaultButton(qobject_cast<QPushButton*>(ordered[default_index]));
    switch (box.exec()) {
    case Box::Ok: return 1;
    case Box::Cancel: return 2;
    case Box::Abort: return 3;
    case Box::Retry: return 4;
    case Box::Ignore: return 5;
    case Box::Yes: return 6;
    case Box::No: return 7;
    default: return 0;
    }
}

static bool read_printf_text(uint32_t address, bool wide, int limit, QString& output)
{
    if (!address) {
        output = QStringLiteral("(null)").left(limit);
        return true;
    }
    const unsigned unit = wide ? 2 : 1;
    int count = 0;
    while (count < limit) {
        uint16_t code = 0;
        memcpy(&code, memory(address + count * unit, unit, 0), unit);
        if (!code)
            break;
        ++count;
    }
    if (!count) {
        output.clear();
        return true;
    }
    const auto* data = memory(address, count * unit, 0);
    if (wide) {
        std::u16string text(count, u'\0');
        memcpy(text.data(), data, count * unit);
        output = QString::fromStdU16String(text);
        return true;
    }
    std::string converted;
    bool replaced = false;
    if (!transcode_guest("CP1252", "UTF-8", data, count, "?", 1, 1, false, converted, replaced))
        return false;
    output = QString::fromUtf8(converted.data(), converted.size());
    return true;
}

static uint32_t wsprintf_guest(uint32_t frame, bool wide)
{
    const auto address = pop32(&frame), format_address = pop32(&frame);
    const auto fail = [](uint32_t error) { last_error = error; return ~0u; };
    QString format, output;
    if (!address || !format_address || !read_printf_text(format_address, wide, 1024, format)
        || format.size() == 1024)
        return fail(87);
    for (qsizetype i = 0; i < format.size() && output.size() < 1024; ++i) {
        if (format[i] != u'%') {
            output += format[i];
            continue;
        }
        ++i;
        if (i < format.size() && format[i] == u'%') {
            output += u'%';
            continue;
        }
        bool left = false, zero = false, alternate = false;
        while (i < format.size()) {
            if (format[i] == u'-') left = true;
            else if (format[i] == u'0') zero = true;
            else if (format[i] == u'#') alternate = true;
            else break;
            ++i;
        }
        const auto number = [&] {
            int value = 0;
            while (i < format.size() && format[i] >= u'0' && format[i] <= u'9') {
                value = std::min(1024, value * 10 + format[i].unicode() - '0');
                ++i;
            }
            return value;
        };
        const int width = number();
        int precision = -1;
        if (i < format.size() && format[i] == u'.') {
            ++i;
            precision = number();
        }
        QChar modifier;
        if (i < format.size() && (format[i] == u'h' || format[i] == u'l' || format[i] == u'w'))
            modifier = format[i++];
        if (i == format.size())
            return fail(87);
        const auto type = format[i].unicode();
        const auto argument = pop32(&frame);
        QString field, prefix;
        switch (type) {
        case 's': case 'S': case 'c': case 'C': {
            const bool utf16 = modifier == u'h' ? false :
                modifier == u'l' || modifier == u'w' ? true :
                (type == 's' || type == 'c') == wide;
            if (type == 's' || type == 'S') {
                if (!read_printf_text(argument, utf16, precision < 0 ? 1024 : precision, field))
                    return fail(87);
            } else if (utf16) {
                field += QChar(uint16_t(argument));
            } else {
                const auto byte = uint8_t(argument);
                std::string converted;
                bool replaced = false;
                if (!transcode_guest("CP1252", "UTF-8", &byte, 1, "?", 1, 1, false, converted, replaced))
                    return fail(87);
                field = QString::fromUtf8(converted.data(), converted.size());
            }
            break;
        }
        case 'i': case 'd': case 'u': case 'x': case 'X': case 'p': {
            const bool sign = type == 'i' || type == 'd';
            const auto signed_value = modifier == u'h' ? int32_t(int16_t(argument)) : int32_t(argument);
            const auto unsigned_value = modifier == u'h' ? uint32_t(uint16_t(argument)) : argument;
            const bool hex = type == 'x' || type == 'X' || type == 'p';
            if (sign && signed_value < 0)
                prefix = QStringLiteral("-");
            else if (alternate && hex && type != 'p')
                prefix = type == 'X' ? QStringLiteral("0X") : QStringLiteral("0x");
            field = QString::number(sign ? uint32_t(std::abs(int64_t(signed_value))) : unsigned_value,
                                    hex ? 16 : 10);
            if (type == 'X' || type == 'p')
                field = field.toUpper();
            field = field.rightJustified(type == 'p' ? 8 : precision, u'0');
            if (zero && !left && precision < 0)
                field = field.rightJustified(width - prefix.size(), u'0');
            break;
        }
        default: return fail(50);
        }
        field.prepend(prefix);
        output += left ? field.leftJustified(width) : field.rightJustified(width);
    }
    const int limit = wide ? 511 : 1023;
    if (output.size() > limit)
        return fail(122);
    if (wide) {
        const auto text = output.toStdU16String();
        memcpy(memory(address, (text.size() + 1) * 2, 1), text.c_str(), (text.size() + 1) * 2);
        return text.size();
    }
    const auto utf8 = output.toUtf8();
    std::string encoded;
    bool replaced = false;
    if (!transcode_guest("UTF-8", "CP1252", reinterpret_cast<const uint8_t*>(utf8.constData()),
                        utf8.size(), "?", 1, 1, false, encoded, replaced))
        return fail(1113);
    memcpy(memory(address, encoded.size() + 1, 1), encoded.c_str(), encoded.size() + 1);
    return encoded.size();
}

static uint32_t win_wsprintfA(uint32_t* sp) { return wsprintf_guest(*sp, false); }
static uint32_t win_wsprintfW(uint32_t* sp) { return wsprintf_guest(*sp, true); }
)MOST";
