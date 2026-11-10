#pragma once
inline constexpr auto runtimeGdi = R"MOST(
static QColor from_colorref(uint32_t color)
{
    return QColor(color & 255, (color >> 8) & 255, (color >> 16) & 255);
}

static uint32_t to_colorref(const QColor& color)
{
    return color.red() | (color.green() << 8) | (color.blue() << 16);
}

static uint32_t add_gdi_object(GuestGdiObject object)
{
    static uint32_t next_object = 0x76000000u;
    if (guest_gdi_objects.size() >= 4096 || next_object == 0x77000000u)
        return gui_error(8);
    const auto handle = next_object++;
    guest_gdi_objects.emplace(handle, std::move(object));
    return handle;
}

static uint32_t widget_font_message(GuestWidget& entry, uint32_t message, uint32_t handle, uint32_t redraw)
{
    if (message == 0x31)
        return entry.font;
    const auto found = guest_gdi_objects.find(handle);
    if (handle && (found == guest_gdi_objects.end() || !found->second.font))
        return 0;
    entry.font = handle;
    entry.widget->setFont(handle ? *found->second.font : widget_app().font());
    if (redraw)
        entry.widget->update();
    return 0;
}

static uint32_t win_CreateBrushIndirect(uint32_t* sp)
{
    const auto address = pop32(sp);
    if (!address)
        return gui_error(87);
    if (load32(address) != 0)
        return gui_error(50);
    return add_gdi_object({std::nullopt, from_colorref(load32(address + 4))});
}

static uint32_t win_CreateFontIndirectW(uint32_t* sp)
{
    const auto address = pop32(sp);
    if (!address)
        return gui_error(87);
    const auto* fields = memory(address, 92, 0);
    int32_t height, width, escapement, orientation, weight;
    memcpy(&height, fields, 4);
    memcpy(&width, fields + 4, 4);
    memcpy(&escapement, fields + 8, 4);
    memcpy(&orientation, fields + 12, 4);
    memcpy(&weight, fields + 16, 4);
    if (height < -4096 || height > 4096 || width || escapement || orientation || weight < 0 || weight > 1000)
        return gui_error(50);
    char16_t face[32];
    memcpy(face, fields + 28, sizeof face);
    const auto* end = std::find(std::begin(face), std::end(face), char16_t(0));
    auto font = widget_app().font();
    if (end != face)
        font.setFamily(QString::fromUtf16(face, end - face));
    if (height)
        font.setPixelSize(std::abs(height));
    if (weight)
        font.setWeight(QFont::Weight(weight));
    font.setItalic(fields[20] != 0);
    font.setUnderline(fields[21] != 0);
    font.setStrikeOut(fields[22] != 0);
    return add_gdi_object({font, {}});
}

static uint32_t win_SelectObject(uint32_t* sp)
{
    const auto handle = pop32(sp), object = pop32(sp);
    const auto dc = guest_dcs.find(handle);
    const auto selected = guest_gdi_objects.find(object);
    constexpr uint32_t default_font = 0xffff0200u;
    if (dc == guest_dcs.end() || (object != default_font &&
        (selected == guest_gdi_objects.end() || !selected->second.font)))
        return gui_error(6);
    const auto previous = std::exchange(dc->second.font, object == default_font ? 0 : object);
    return previous ? previous : default_font;
}

static uint32_t win_DeleteObject(uint32_t* sp)
{
    const auto object = pop32(sp);
    if (object == 0xffff0200u)
        return 1;
    for (const auto& [handle, dc] : guest_dcs)
        if (dc.font == object && object)
            return gui_error(170);
    if (guest_gdi_objects.erase(object))
        return 1;
    if (const auto it = guest_images.find(object); it != guest_images.end() && it->second.type == 0) {
        guest_images.erase(it);
        return 1;
    }
    return gui_error(6);
}

static uint32_t win_SetBkMode(uint32_t* sp)
{
    const auto handle = pop32(sp), mode = pop32(sp);
    const auto dc = guest_dcs.find(handle);
    if (dc == guest_dcs.end() || (mode != 1 && mode != 2))
        return gui_error(87);
    return std::exchange(dc->second.background_mode, mode);
}

static uint32_t set_dc_color(uint32_t* sp, QColor GuestDc::* member)
{
    const auto handle = pop32(sp), color = pop32(sp);
    const auto dc = guest_dcs.find(handle);
    if (dc == guest_dcs.end()) {
        gui_error(6);
        return ~0u;
    }
    return to_colorref(std::exchange(dc->second.*member, from_colorref(color)));
}

static uint32_t win_SetTextColor(uint32_t* sp) { return set_dc_color(sp, &GuestDc::text); }
static uint32_t win_SetBkColor(uint32_t* sp) { return set_dc_color(sp, &GuestDc::background); }

static uint32_t win_GetDeviceCaps(uint32_t* sp)
{
    const auto handle = pop32(sp), index = pop32(sp);
    if (!guest_dcs.contains(handle))
        return gui_error(6);
    const auto* screen = widget_app().primaryScreen();
    if (!screen)
        return 0;
    switch (index) {
    case 2: return 1; // DT_RASDISPLAY
    case 4: return qRound(screen->physicalSize().width());
    case 6: return qRound(screen->physicalSize().height());
    case 8: return screen->size().width();
    case 10: return screen->size().height();
    case 12: return screen->depth();
    case 14: return 1;
    case 88: return qRound(screen->logicalDotsPerInchX());
    case 90: return qRound(screen->logicalDotsPerInchY());
    default: return 0;
    }
}

struct GuestImageList {
    QSize size;
    std::vector<QImage> images;
};
static std::map<uint32_t, GuestImageList> guest_image_lists;
static size_t image_list_bytes;

static uint32_t win_InitCommonControls(uint32_t*)
{
    common_controls_initialized = true;
    return 0;
}

static uint32_t win_ImageList_Create(uint32_t* sp)
{
    const int width = pop32(sp), height = pop32(sp);
    const auto flags = pop32(sp), initial = pop32(sp), growth = pop32(sp);
    if (width <= 0 || width > 1024 || height <= 0 || height > 1024
        || (flags & ~0x3fu) || initial > 4096 || growth > 4096)
        return gui_error(87);
    static uint32_t next_list = 0x77000000u;
    if (guest_image_lists.size() >= 1024 || next_list == 0x78000000u)
        return gui_error(8);
    const auto handle = next_list++;
    GuestImageList list{QSize(width, height), {}};
    list.images.reserve(initial);
    guest_image_lists.emplace(handle, std::move(list));
    return handle;
}

static uint32_t win_ImageList_AddMasked(uint32_t* sp)
{
    const auto handle = pop32(sp), bitmap = pop32(sp), color = pop32(sp);
    const auto list = guest_image_lists.find(handle);
    const auto image = guest_images.find(bitmap);
    if (list == guest_image_lists.end() || image == guest_images.end() || image->second.type != 0) {
        gui_error(6);
        return ~0u;
    }
    const auto& source = image->second.image;
    const auto size = list->second.size;
    const auto count = source.width() / size.width();
    const size_t bytes = size_t(count) * size.width() * size.height() * 4;
    if (source.width() % size.width() || source.height() != size.height() || !count ||
        list->second.images.size() + count > 4096 || bytes > 128 * 1024 * 1024 - image_list_bytes) {
        gui_error(87);
        return ~0u;
    }
    const auto key = color == 0xff000000u ? source.pixelColor(0, 0).rgb() : from_colorref(color).rgb();
    std::vector<QImage> staged;
    for (int i = 0; i < count; ++i) {
        auto tile = source.copy(i * size.width(), 0, size.width(), size.height()).convertToFormat(QImage::Format_ARGB32);
        if (tile.isNull()) {
            gui_error(8);
            return ~0u;
        }
        for (int y = 0; y < tile.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(tile.scanLine(y));
            for (int x = 0; x < tile.width(); ++x)
                if ((row[x] | 0xff000000u) == key)
                    row[x] = 0;
        }
        staged.push_back(std::move(tile));
    }
    auto& images = list->second.images;
    const auto index = images.size();
    images.reserve(index + count);
    for (auto& tile : staged)
        images.push_back(std::move(tile));
    image_list_bytes += bytes;
    return index;
}

static uint32_t win_ImageList_Destroy(uint32_t* sp)
{
    const auto handle = pop32(sp);
    const auto list = guest_image_lists.find(handle);
    if (list == guest_image_lists.end())
        return gui_error(6);
    image_list_bytes -= size_t(list->second.size.width()) * list->second.size.height()
                        * 4 * list->second.images.size();
    guest_image_lists.erase(list);
    return 1;
}
)MOST";
