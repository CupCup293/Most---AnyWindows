#pragma once
inline constexpr auto runtimePaint = R"MOST(
struct GuestGdiObject {
    std::optional<QFont> font;
    QColor brush;
};
static std::map<uint32_t, GuestGdiObject> guest_gdi_objects;

struct GuestDc {
    uint32_t window, paint_struct;
    QRect clip;
    uint32_t font = 0;
    int background_mode = 2;
    QColor text = Qt::black, background = Qt::white;
};
static std::map<uint32_t, GuestDc> guest_dcs;
static uint32_t next_dc = 0x74000000u;

static void release_guest_dc(uint32_t window)
{
    std::erase_if(guest_dcs, [=](const auto& item) { return item.second.window == window; });
}

static void invalidate_guest_surface(uint32_t window, QRect area, bool erase)
{
    const auto it = guest_widgets.find(window);
    if (it == guest_widgets.end() || !it->second.widget)
        return;
    auto& entry = it->second;
    area = area.intersected(entry.widget->rect());
    if (area.isEmpty())
        return;
    if (entry.procedure) {
        entry.dirty = entry.dirty.united(area);
        entry.erase |= erase;
        queue_window_message(window, 0x0f, 0, 0);
    } else {
        entry.widget->update(area);
    }
}

static void paint_guest_surface(uint32_t handle, QWidget* widget)
{
    const auto it = guest_widgets.find(handle);
    if (it != guest_widgets.end() && !it->second.surface.isNull()) {
        QPainter painter(widget);
        painter.drawImage(QPoint(), it->second.surface);
    }
}

static uint32_t get_guest_dc(uint32_t window, uint32_t paint_struct, QRect clip)
{
    if (guest_dcs.size() >= 4096 || next_dc == 0x75000000u)
        return gui_error(8);
    const auto handle = next_dc++;
    guest_dcs.emplace(handle, GuestDc{window, paint_struct, clip});
    return handle;
}

static uint32_t win_GetDC(uint32_t* sp)
{
    const auto window = pop32(sp);
    const auto* widget = window ? widget_at(window) : nullptr;
    if (window && !widget)
        return 0;
    return get_guest_dc(window, 0, widget ? widget->rect() : QRect());
}

static uint32_t win_ReleaseDC(uint32_t* sp)
{
    const auto window = pop32(sp), handle = pop32(sp);
    const auto it = guest_dcs.find(handle);
    if (it == guest_dcs.end() || it->second.window != window || it->second.paint_struct)
        return gui_error(6);
    guest_dcs.erase(it);
    return 1;
}

static uint32_t win_BeginPaint(uint32_t* sp)
{
    const auto window = pop32(sp), address = pop32(sp);
    auto* entry = widget_entry(window);
    if (!entry || !address)
        return gui_error(entry ? 87 : 1400);
    auto* output = memory(address, 64, 1);
    const auto clip = entry->dirty.intersected(entry->widget->rect());
    const auto handle = get_guest_dc(window, address, clip);
    if (!handle)
        return 0;
    const uint32_t fields[] = {handle, entry->erase, uint32_t(clip.x()), uint32_t(clip.y()),
        uint32_t(clip.x() + clip.width()), uint32_t(clip.y() + clip.height())};
    memset(output, 0, 64);
    memcpy(output, fields, sizeof fields);
    entry->dirty = {};
    entry->erase = false;
    std::erase_if(messages, [=](const GuestMessage& item) {
        return item.window == window && item.message == 0x0f;
    });
    return handle;
}

static uint32_t win_EndPaint(uint32_t* sp)
{
    const auto window = pop32(sp), address = pop32(sp);
    if (!address)
        return gui_error(87);
    const auto it = guest_dcs.find(load32(address));
    if (it == guest_dcs.end() || it->second.window != window || it->second.paint_struct != address)
        return gui_error(6);
    guest_dcs.erase(it);
    return 1;
}

static std::optional<QRect> read_guest_rect(uint32_t address)
{
    if (!address)
        return {};
    int32_t fields[4];
    memcpy(fields, memory(address, sizeof fields, 0), sizeof fields);
    const int64_t width = int64_t(fields[2]) - fields[0], height = int64_t(fields[3]) - fields[1];
    if (width < 0 || width > INT32_MAX || height < 0 || height > INT32_MAX
        || fields[2] == INT32_MIN || fields[3] == INT32_MIN)
        return {};
    return QRect(fields[0], fields[1], width, height);
}

static GuestWidget* prepare_surface(const GuestDc& dc)
{
    auto* entry = widget_entry(dc.window);
    if (!entry || (!entry->owns_widget && !dynamic_cast<GuestTopWindow*>(entry->widget.data()))) {
        gui_error(50);
        return nullptr;
    }
    const auto size = entry->widget->size();
    if (size.isEmpty() || size.width() > 4096 || size.height() > 4096) {
        gui_error(8);
        return nullptr;
    }
    if (entry->surface.size() != size) {
        qsizetype bytes = qsizetype(size.width()) * size.height() * 4;
        for (const auto& [handle, window] : guest_widgets)
            bytes += window.surface.sizeInBytes();
        if (bytes > 256 * 1024 * 1024) {
            gui_error(8);
            return nullptr;
        }
        QImage surface(size, QImage::Format_ARGB32_Premultiplied);
        if (surface.isNull()) {
            gui_error(8);
            return nullptr;
        }
        surface.fill(Qt::transparent);
        if (!entry->surface.isNull()) {
            QPainter copy(&surface);
            copy.drawImage(QPoint(), entry->surface);
        }
        entry->surface = std::move(surface);
    }
    return entry;
}

static uint32_t win_FillRect(uint32_t* sp)
{
    const auto handle = pop32(sp), address = pop32(sp), brush = pop32(sp);
    const auto it = guest_dcs.find(handle);
    const auto rect = read_guest_rect(address);
    if (it == guest_dcs.end() || !rect || !brush)
        return gui_error(87);
    const auto object = guest_gdi_objects.find(brush);
    const auto color = object == guest_gdi_objects.end() ? guest_system_color(brush - 1)
                                                         : object->second.brush;
    if (!color.isValid())
        return gui_error(50);
    auto* entry = prepare_surface(it->second);
    if (!entry)
        return 0;
    const auto clipped = rect->intersected(it->second.clip);
    if (!clipped.isEmpty()) {
        QPainter painter(&entry->surface);
        painter.fillRect(clipped, color);
        entry->widget->update(clipped);
    }
    return 1;
}

static uint32_t win_DrawTextW(uint32_t* sp)
{
    const auto handle = pop32(sp), text_address = pop32(sp);
    const int count = pop32(sp);
    const auto rect_address = pop32(sp), flags = pop32(sp);
    const auto it = guest_dcs.find(handle);
    const auto rect = read_guest_rect(rect_address);
    if (it == guest_dcs.end() || !rect || !text_address || count < -1 || count > 32767)
        return gui_error(87);
    if (flags & ~0xdbfu)
        return gui_error(50);
    QString text;
    if (count < 0) {
        if (!widget_string(text_address, text))
            return 0;
    } else {
        std::u16string units(count, u'\0');
        if (count)
            memcpy(units.data(), memory(text_address, count * 2, 0), count * 2);
        text = QString::fromStdU16String(units);
    }
    int mode = flags & 1 ? Qt::AlignHCenter : flags & 2 ? Qt::AlignRight : Qt::AlignLeft;
    mode |= flags & 4 ? Qt::AlignVCenter : flags & 8 ? Qt::AlignBottom : Qt::AlignTop;
    if (flags & 0x20)
        mode |= Qt::TextSingleLine;
    else if (flags & 0x10)
        mode |= Qt::TextWordWrap;
    if (flags & 0x40)
        mode |= Qt::TextExpandTabs;
    if (!(flags & 0x800))
        mode |= Qt::TextShowMnemonic;
    if (flags & 0x100)
        mode |= Qt::TextDontClip;
    auto* widget = it->second.window ? widget_at(it->second.window) : nullptr;
    const auto selected = guest_gdi_objects.find(it->second.font);
    const auto font = selected != guest_gdi_objects.end() ? *selected->second.font
        : widget ? widget->font() : widget_app().font();
    const QFontMetrics metrics(font);
    const auto bounds = metrics.boundingRect(*rect, mode, text);
    if (flags & 0x400) {
        if (!write_rect(rect_address, QRect(rect->topLeft(), bounds.size())))
            return 0;
    } else {
        auto* entry = prepare_surface(it->second);
        if (!entry)
            return 0;
        QPainter painter(&entry->surface);
        painter.setFont(font);
        painter.setPen(it->second.text);
        painter.setBackground(it->second.background);
        painter.setBackgroundMode(it->second.background_mode == 1 ? Qt::TransparentMode : Qt::OpaqueMode);
        painter.setClipRect(it->second.clip);
        painter.drawText(*rect, mode, text);
        entry->widget->update(it->second.clip);
    }
    return bounds.height();
}

struct GuestImage { unsigned type; QImage image; };
static std::map<uint32_t, GuestImage> guest_images;
static uint32_t next_image = 0x75000000u;

static void append_le(QByteArray& bytes, uint32_t value, int count)
{
    for (int i = 0; i < count; ++i)
        bytes.append(char(value >> (i * 8)));
}

static QImage decode_resource_bitmap(std::span<const uint8_t> dib)
{
    ResourceReader reader{dib};
    const auto header = reader.read(4);
    const int32_t width = reader.read(4), height = reader.read(4);
    const auto planes = reader.read(2), depth = reader.read(2);
    const auto compression = reader.read(4);
    reader.position = 32;
    const auto used = reader.read(4);
    if (!reader.valid || header < 40 || header > dib.size() || width <= 0 || width > 4096
        || !height || height < -4096 || height > 4096 || planes != 1 || compression > 3
        || (depth != 1 && depth != 4 && depth != 8 && depth != 16 && depth != 24 && depth != 32))
        return {};
    const auto palette = depth <= 8 ? used ? used : (1u << depth) : used;
    const uint64_t offset = header + uint64_t(palette) * 4 + (header == 40 && compression == 3 ? 12 : 0);
    if (offset >= dib.size())
        return {};
    QByteArray bmp("BM");
    append_le(bmp, dib.size() + 14, 4);
    append_le(bmp, 0, 4);
    append_le(bmp, offset + 14, 4);
    bmp.append(reinterpret_cast<const char*>(dib.data()), dib.size());
    return QImage::fromData(bmp, "BMP");
}

static QImage decode_resource_icon(uint32_t name, int width, int height)
{
    const auto group = find_widget_resource(14, name);
    ResourceReader reader{group};
    if (reader.read(2) != 0 || reader.read(2) != 1)
        return {};
    const auto count = reader.read(2);
    uint32_t id = 0, rank = ~0u;
    std::span<const uint8_t> best;
    for (uint32_t i = 0; i < count && reader.valid; ++i) {
        const auto offset = reader.position;
        const int w = reader.read(1), h = reader.read(1);
        reader.read(2);
        reader.read(4);
        reader.read(4);
        const auto candidate = reader.read(2);
        const uint32_t score = std::abs((w ? w : 256) - (width ? width : 32))
            + std::abs((h ? h : 256) - (height ? height : 32));
        if (reader.valid && score < rank) {
            rank = score;
            id = candidate;
            best = group.subspan(offset, 8);
        }
    }
    const auto image = find_widget_resource(3, id);
    if (best.empty() || image.empty())
        return {};
    QByteArray ico;
    append_le(ico, 0, 2);
    append_le(ico, 1, 2);
    append_le(ico, 1, 2);
    ico.append(reinterpret_cast<const char*>(best.data()), best.size());
    append_le(ico, image.size(), 4);
    append_le(ico, 22, 4);
    ico.append(reinterpret_cast<const char*>(image.data()), image.size());
    return QImage::fromData(ico, "ICO");
}

static QImage resource_control_image(uint32_t id, uint32_t style)
{
    if ((style & 31) == 3)
        return decode_resource_icon(id, 32, 32);
    if ((style & 31) == 14)
        return decode_resource_bitmap(find_widget_resource(2, id));
    return {};
}

static uint32_t load_guest_image(uint32_t module, uint32_t name, unsigned type, int width, int height,
                                 uint32_t flags)
{
    if (type == 2 && !module && name <= 0xffffu && !width && !height
        && !(flags & ~0x8040u) && stock_cursor(stock_cursor_base + name))
        return stock_cursor_base + name;
    if (module != guest_image_base || (flags & ~0x8040u) || type > 1
        || width < 0 || width > 4096 || height < 0 || height > 4096)
        return gui_error(50);
    if (guest_images.size() >= 1024 || next_image == 0x76000000u)
        return gui_error(8);
    if (type == 1 && (flags & 0x40)) {
        width = width ? width : 32;
        height = height ? height : 32;
    }
    auto image = type == 0 ? decode_resource_bitmap(find_widget_resource(2, name))
                           : decode_resource_icon(name, width, height);
    if (image.isNull() || image.width() > 4096 || image.height() > 4096)
        return gui_error(1814);
    const QSize desired(width ? width : image.width(), height ? height : image.height());
    if (desired != image.size())
        image = image.scaled(desired, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    qsizetype bytes = image.sizeInBytes();
    for (const auto& [handle, entry] : guest_images)
        bytes += entry.image.sizeInBytes();
    if (image.isNull() || bytes > 128 * 1024 * 1024)
        return gui_error(8);
    const auto handle = next_image++;
    guest_images.emplace(handle, GuestImage{type, std::move(image)});
    return handle;
}

static uint32_t win_LoadBitmapW(uint32_t* sp)
{
    const auto module = pop32(sp), name = pop32(sp);
    return load_guest_image(module, name, 0, 0, 0, 0);
}

static uint32_t win_LoadImageW(uint32_t* sp)
{
    const auto module = pop32(sp), name = pop32(sp), type = pop32(sp);
    const int width = pop32(sp), height = pop32(sp);
    const auto flags = pop32(sp);
    return load_guest_image(module, name, type, width, height, flags);
}

static uint32_t widget_image_message(GuestWidget& entry, uint32_t message, uint32_t type, uint32_t image)
{
    if (type > 1)
        return gui_error(87);
    const bool icon = message == 0x7f || message == 0x80;
    if (!icon && (type != 0 || !qobject_cast<QLabel*>(entry.widget.data())))
        return gui_error(50);
    auto& handle = icon ? (type ? entry.icon_large : entry.icon_small) : entry.bitmap;
    if (message == 0x7f || message == 0x173)
        return handle;
    const auto it = guest_images.find(image);
    if (image && (it == guest_images.end() || it->second.type != unsigned(icon)))
        return gui_error(6);
    const auto pixmap = image ? QPixmap::fromImage(it->second.image) : QPixmap();
    if (icon)
        entry.widget->setWindowIcon(QIcon(pixmap));
    else
        static_cast<QLabel*>(entry.widget.data())->setPixmap(pixmap);
    return std::exchange(handle, image);
}
)MOST";
