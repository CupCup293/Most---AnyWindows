#include <memory>
#include <unordered_map>
#include <QStyle>

struct LogFontW {
    int32_t height, width, escapement, orientation, weight;
    uint8_t italic, underline, strike, charset, output, clip, quality, pitch;
    char16_t family[32];
};
static_assert(sizeof(LogFontW) == 92);

static std::unordered_map<void*, std::unique_ptr<QFont>> fonts;
static QHash<void*, void*> selected_fonts;

static QFont* get_font(void* handle)
{
    auto found = fonts.find(handle);
    return found != fonts.end() ? found->second.get() : nullptr;
}

static QFont selected_font(void* dc)
{
    auto* font = get_font(selected_fonts.value(dc));
    return font ? *font : application().font();
}

static void* make_font(int height, int weight, int italic, int underline, int strike, const QString& family)
{
    auto font = std::make_unique<QFont>(family.isEmpty() ? application().font().family() : family);
    if (height)
        font->setPixelSize(qMax(1, qAbs(height)));
    font->setWeight(static_cast<QFont::Weight>(qBound(1, weight ? weight : 400, 1000)));
    font->setItalic(italic);
    font->setUnderline(underline);
    font->setStrikeOut(strike);
    void* handle = font.get();
    fonts.emplace(handle, std::move(font));
    return handle;
}
WINAPI void* win_CreateFontA(int height, int width, int escape, int orient, int weight, uint32_t italic,
                            uint32_t underline, uint32_t strike, uint32_t charset, uint32_t output,
                            uint32_t clip, uint32_t quality, uint32_t pitch, const char* family)
{
    (void)width; (void)escape; (void)orient; (void)charset; (void)output;
    (void)clip; (void)quality; (void)pitch;
    return make_font(height, weight, italic, underline, strike,
                     family ? QString::fromLocal8Bit(family) : QString());
}
WINAPI void* win_CreateFontIndirectW(const LogFontW* font)
{
    if (!font)
        return nullptr;
    size_t count = 0;
    while (count < 32 && font->family[count])
        ++count;
    return make_font(font->height, font->weight, font->italic, font->underline,
                     font->strike, QString::fromUtf16(font->family, count));
}
WINAPI void* win_GetStockObject(int index)
{
    if (index == 17 || index == 13 || index == 16) {
        static void* font = make_font(0, 400, 0, 0, 0, {});
        return font;
    }
    return nullptr;
}
WINAPI void* win_SelectObject(void* dc, void* object)
{
    if (!dc || !get_font(object))
        return nullptr;
    auto* previous = selected_fonts.value(dc, win_GetStockObject(17));
    selected_fonts.insert(dc, object);
    return previous;
}
WINAPI int win_DeleteObject(void* object)
{
    if (object == win_GetStockObject(17))
        return 1;
    return fonts.erase(object) != 0;
}

struct TextMetricW {
    int32_t height, ascent, descent, internal, external, average, maximum, weight,
        overhang, digitized_x, digitized_y;
    char16_t first, last, replacement, break_char;
    uint8_t italic, underline, strike, pitch, charset;
};
static_assert(sizeof(TextMetricW) == 60);
WINAPI int win_GetTextMetricsW(void* dc, TextMetricW* output)
{
    if (!dc || !output)
        return 0;
    const auto font = selected_font(dc);
    const QFontMetrics metrics(font);
    *output = {metrics.height(), metrics.ascent(), metrics.descent(), 0, metrics.leading(),
        metrics.averageCharWidth(), metrics.maxWidth(), font.weight(), 0,
        qRound(screen()->logicalDotsPerInchX()), qRound(screen()->logicalDotsPerInchY()),
        32, 0xfffd, u'?', u' ', uint8_t(font.italic()), uint8_t(font.underline()),
        uint8_t(font.strikeOut()), 1, 1};
    return 1;
}
struct WinSize { int32_t width, height; };
WINAPI int win_GetTextExtentPoint32W(void* dc, const char16_t* text, int count, WinSize* output)
{
    if (!dc || !text || !output || count < 0)
        return 0;
    const QFontMetrics metrics(selected_font(dc));
    *output = {metrics.horizontalAdvance(QString::fromUtf16(text, count)), metrics.height()};
    return 1;
}
WINAPI int win_GetTextExtentPoint32A(void* dc, const char* text, int count, WinSize* output)
{
    if (!dc || !text || !output || count < 0)
        return 0;
    auto wide = QString::fromLocal8Bit(text, count);
    return win_GetTextExtentPoint32W(dc, reinterpret_cast<const char16_t*>(wide.utf16()), wide.size(), output);
}

WINAPI int win_GetSystemMetrics(int index)
{
    auto& app = application();
    const auto rect = screen()->geometry();
    switch (index) {
    case 0: return rect.width();
    case 1: return rect.height();
    case 2: case 3: return app.style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    case 4: return app.style()->pixelMetric(QStyle::PM_TitleBarHeight);
    case 5: case 6: case 7: case 8: return app.style()->pixelMetric(QStyle::PM_DefaultFrameWidth);
    case 11: case 12: return app.style()->pixelMetric(QStyle::PM_LargeIconSize);
    case 13: case 14: case 49: case 50: return app.style()->pixelMetric(QStyle::PM_SmallIconSize);
    case 19: return 1;
    case 23: return 0;
    case 43: return 3;
    case 76: return rect.left();
    case 77: return rect.top();
    case 78: return rect.width();
    case 79: return rect.height();
    case 80: return QGuiApplication::screens().size();
    default:
        std::fprintf(stderr, "GetSystemMetrics index=%d\n", index);
        return 0;
    }
}

WINAPI int win_GetObjectW(void* handle, int capacity, void* output)
{
    const auto* font = get_font(handle);
    if (!font)
        return 0;
    if (!output)
        return sizeof(LogFontW);
    if (capacity < int(sizeof(LogFontW)))
        return 0;
    LogFontW info{};
    info.height = -(font->pixelSize() > 0 ? font->pixelSize() : QFontMetrics(*font).height());
    info.weight = font->weight();
    info.italic = font->italic();
    info.underline = font->underline();
    info.strike = font->strikeOut();
    copy_text(font->family(), info.family, 32);
    std::memcpy(output, &info, sizeof(info));
    return sizeof(info);
}
