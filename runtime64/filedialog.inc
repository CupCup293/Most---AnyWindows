#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStringList>
#include <array>

static constexpr int32_t no_interface = static_cast<int32_t>(0x80004002u);
static constexpr int32_t not_implemented = static_cast<int32_t>(0x80004001u);
static constexpr int32_t bad_pointer = static_cast<int32_t>(0x80004003u);
static constexpr int32_t cancelled = static_cast<int32_t>(0x800704c7u);
using Guid = std::array<unsigned char, 16>;
static constexpr Guid iid_unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static constexpr Guid iid_open{0x88,0x72,0x7c,0xd5,0xad,0xd4,0x68,0x47,0xbe,2,0x9d,0x96,0x95,0x32,0xd9,0x60};
static constexpr Guid iid_save{0x9a,0x71,0xb0,0x84,0x0d,0x59,0xe4,0x4a,0xb5,0xd8,0xb8,0x0d,0x5e,0x4b,0x03,0x67};
static constexpr Guid iid_file{0x36,0x51,0xf8,0x42,0x7e,0xdb,0x9c,0x43,0x85,0xf1,0xe4,0x07,0x5d,0x13,0x5f,0xc8};
static constexpr Guid iid_modal{0xb4,0xdb,0x5a,0xb4,0x87,0xc2,0x67,0x47,0x89,0x2f,0x46,0x8b,0x5e,0x0d,0x6b,0x3a};
static constexpr Guid iid_item{0x1e,0x6d,0x82,0x43,0xe8,0xe7,0xee,0x42,0xbc,0x55,0xa1,0xe2,0x61,0xc3,0x7b,0xfe};
static constexpr Guid cls_open{0x9c,0x5a,0x1c,0xdc,0x8a,0xe8,0xde,0x4d,0xa5,0xa1,0x60,0xf8,0x2a,0x20,0xae,0xf7};
static constexpr Guid cls_save{0xf3,0xe2,0xb4,0xc0,0x21,0xba,0x73,0x47,0x8d,0xba,0x33,0x5e,0xc9,0x46,0xeb,0x8b};

static bool guid_is(const void* actual, const Guid& expected)
{
    return actual && !std::memcmp(actual, expected.data(), expected.size());
}
static int32_t com_string(const QString& text, char16_t** output)
{
    if (!output)
        return bad_pointer;
    size_t size = (text.size() + 1) * sizeof(char16_t);
    *output = static_cast<char16_t*>(std::malloc(size));
    if (!*output)
        return static_cast<int32_t>(0x8007000eu);
    std::memcpy(*output, text.utf16(), size);
    return 0;
}
static QString windows_path(QString path)
{
    return QStringLiteral("Z:") + path.replace('/', '\\');
}
extern "C" int native_launch;

static QString picker_path(QString path)
{
    path.replace('\\', '/');
    if (path.startsWith(QStringLiteral("Z:"), Qt::CaseInsensitive))
        path.remove(0, 2);
    else if (!path.startsWith('/'))
        path = QStringLiteral("/scratch/") + path;
    path = QDir::cleanPath(path);
    if (native_launch && path.startsWith('/'))
        return path;
    return path == QStringLiteral("/scratch") || path.startsWith(QStringLiteral("/scratch/")) ? path : QString();
}

struct ShellItem { const void* table; unsigned refs = 1; QString path; };
static ShellItem* make_shell_item(const QString& path);
static __attribute__((ms_abi)) int32_t item_query(ShellItem* self, const void* iid, void** output)
{
    if (!output) return bad_pointer;
    *output = nullptr;
    if (!guid_is(iid, iid_unknown) && !guid_is(iid, iid_item)) return no_interface;
    ++self->refs;
    *output = self;
    return 0;
}
static __attribute__((ms_abi)) unsigned item_add(ShellItem* self) { return ++self->refs; }
static __attribute__((ms_abi)) unsigned item_release(ShellItem* self)
{
    if (const unsigned left = --self->refs) return left;
    delete self;
    return 0;
}
static __attribute__((ms_abi)) int32_t item_bind(ShellItem*, void*, const void*, const void*, void** output)
{
    if (output) *output = nullptr;
    return not_implemented;
}
static __attribute__((ms_abi)) int32_t item_parent(ShellItem* self, ShellItem** output)
{
    if (!output) return bad_pointer;
    *output = make_shell_item(QFileInfo(self->path).absolutePath());
    return 0;
}
static __attribute__((ms_abi)) int32_t item_name(ShellItem* self, unsigned kind, char16_t** output)
{
    if (kind == 0) return com_string(QFileInfo(self->path).fileName(), output);
    if (kind == 0x80058000 || kind == 0x80028000) return com_string(windows_path(self->path), output);
    return not_implemented;
}
static __attribute__((ms_abi)) int32_t item_attr(ShellItem* self, unsigned mask, unsigned* output)
{
    if (!output) return bad_pointer;
    const unsigned attributes = 0x40000000u | (QFileInfo(self->path).isDir() ? 0x20000000u : 0);
    *output = mask & attributes;
    return *output == mask ? 0 : 1;
}
static __attribute__((ms_abi)) int32_t item_compare(ShellItem* self, ShellItem* other, unsigned, int* output)
{
    if (!other || !output) return bad_pointer;
    *output = self->path.compare(other->path);
    return 0;
}
static ShellItem* make_shell_item(const QString& path)
{
    static const void* table[] = {
        reinterpret_cast<void*>(item_query), reinterpret_cast<void*>(item_add), reinterpret_cast<void*>(item_release),
        reinterpret_cast<void*>(item_bind), reinterpret_cast<void*>(item_parent), reinterpret_cast<void*>(item_name),
        reinterpret_cast<void*>(item_attr), reinterpret_cast<void*>(item_compare)
    };
    return new ShellItem{table, 1, path};
}

struct FileDialog {
    const void* table;
    unsigned refs = 1, options = 0, selected_filter = 1;
    bool save;
    QString title, name, folder = native_launch ? QDir::homePath() : QStringLiteral("/scratch"), result, suffix, accept, label;
    QStringList filters;
    QFileDialog* open = nullptr;
};
struct FileSpec { const char16_t* title; const char16_t* pattern; };

static __attribute__((ms_abi)) int32_t fd_query(FileDialog* self, const void* iid, void** output)
{
    if (!output) return bad_pointer;
    *output = nullptr;
    if (!guid_is(iid, iid_unknown) && !guid_is(iid, iid_file) && !guid_is(iid, iid_modal) &&
        !guid_is(iid, self->save ? iid_save : iid_open)) return no_interface;
    ++self->refs;
    *output = self;
    return 0;
}
static __attribute__((ms_abi)) unsigned fd_add(FileDialog* self) { return ++self->refs; }
static __attribute__((ms_abi)) unsigned fd_release(FileDialog* self)
{
    if (const unsigned left = --self->refs) return left;
    delete self;
    return 0;
}
static __attribute__((ms_abi)) int32_t fd_show(FileDialog* self, void* owner)
{
    QFileDialog picker(as_widget(owner), self->title, self->folder);
    picker.setOption(QFileDialog::DontUseNativeDialog);
    picker.setAcceptMode(self->save ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
    picker.setFileMode(self->options & 0x20 ? QFileDialog::Directory
        : self->save ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
    if (!self->accept.isEmpty()) picker.setLabelText(QFileDialog::Accept, self->accept);
    if (!self->label.isEmpty()) picker.setLabelText(QFileDialog::FileName, self->label);
    if (!self->name.isEmpty()) picker.selectFile(self->name);
    if (!self->suffix.isEmpty()) picker.setDefaultSuffix(self->suffix);
    if (!self->filters.isEmpty()) {
        picker.setNameFilters(self->filters);
        if (self->selected_filter > 0 && self->selected_filter <= unsigned(self->filters.size()))
            picker.selectNameFilter(self->filters[self->selected_filter - 1]);
    }
    const auto source = !native_launch && !self->save && qEnvironmentVariableIsSet("MOST_RUFUS_IMAGE")
        ? picker_path(qEnvironmentVariable("MOST_RUFUS_IMAGE")) : QString();
    if (QFileInfo(source).isFile())
        picker.selectFile(source);
    self->open = &picker;
    const auto scripted = picker_path(qEnvironmentVariable("RUFUS_PROBE_PICK"));
    std::fprintf(stderr, "Rufus requested a %s file picker\n", self->save ? "save" : "open");
    if (!qEnvironmentVariableIsEmpty("RUFUS_PROBE_PICK") && QFileInfo(scripted).isFile()) {
        picker.selectFile(scripted);
        QTimer::singleShot(0, &picker, [&picker] {
            QMetaObject::invokeMethod(&picker, "accept", Qt::DirectConnection);
        });
    }
    const int status = picker.exec();
    self->open = nullptr;
    const auto path = picker.selectedFiles().value(0);
    self->result = picker_path(path);
    if (!self->result.isEmpty() && status == QDialog::Accepted &&
        (source.isEmpty() || self->result == source)) {
        std::fprintf(stderr, "Rufus file selected: %s\n", qPrintable(self->result));
        return 0;
    }
    self->result.clear();
    return cancelled;
}

static __attribute__((ms_abi)) int32_t fd_types(FileDialog* self, unsigned count, const FileSpec* specs)
{
    if (count > 64 || (count && !specs)) return bad_pointer;
    self->filters.clear();
    for (unsigned i = 0; i < count; ++i) {
        auto pattern = utf16(specs[i].pattern).replace(';', ' ');
        self->filters << QStringLiteral("%1 (%2)").arg(utf16(specs[i].title), pattern);
    }
    return 0;
}
static __attribute__((ms_abi)) int32_t fd_set_type(FileDialog* self, unsigned type)
{ self->selected_filter = type; return 0; }
static __attribute__((ms_abi)) int32_t fd_get_type(FileDialog* self, unsigned* type)
{ if (!type) return bad_pointer; *type = self->selected_filter; return 0; }
static __attribute__((ms_abi)) int32_t fd_options(FileDialog* self, unsigned flags)
{ self->options = flags; return 0; }
static __attribute__((ms_abi)) int32_t fd_get_options(FileDialog* self, unsigned* flags)
{ if (!flags) return bad_pointer; *flags = self->options; return 0; }
static __attribute__((ms_abi)) int32_t fd_folder(FileDialog* self, ShellItem* item)
{ if (!item) return bad_pointer; self->folder = item->path; return 0; }
static __attribute__((ms_abi)) int32_t fd_get_folder(FileDialog* self, ShellItem** item)
{ if (!item) return bad_pointer; *item = make_shell_item(self->folder); return 0; }
static __attribute__((ms_abi)) int32_t fd_name(FileDialog* self, const char16_t* name)
{ self->name = utf16(name); return 0; }
static __attribute__((ms_abi)) int32_t fd_get_name(FileDialog* self, char16_t** output)
{ return com_string(self->name, output); }
static __attribute__((ms_abi)) int32_t fd_title(FileDialog* self, const char16_t* value)
{ self->title = utf16(value); return 0; }
static __attribute__((ms_abi)) int32_t fd_accept(FileDialog* self, const char16_t* value)
{ self->accept = utf16(value); return 0; }
static __attribute__((ms_abi)) int32_t fd_label(FileDialog* self, const char16_t* value)
{ self->label = utf16(value); return 0; }
static __attribute__((ms_abi)) int32_t fd_result(FileDialog* self, ShellItem** output)
{
    if (!output) return bad_pointer;
    *output = self->result.isEmpty() ? nullptr : make_shell_item(self->result);
    return *output ? 0 : static_cast<int32_t>(0x8000ffffu);
}
static __attribute__((ms_abi)) int32_t fd_suffix(FileDialog* self, const char16_t* value)
{ self->suffix = utf16(value); return 0; }
static __attribute__((ms_abi)) int32_t fd_close(FileDialog* self, int32_t)
{ if (self->open) self->open->reject(); return 0; }
static __attribute__((ms_abi)) int32_t fd_unsupported(void*) { return not_implemented; }

extern "C" int32_t create_file_dialog(const void* clsid, const void* iid, void** output)
{
    const bool save = guid_is(clsid, cls_save);
    if (!save && !guid_is(clsid, cls_open))
        return static_cast<int32_t>(0x80040154u);
    if (!output)
        return bad_pointer;
    static const void* table[] = {
        reinterpret_cast<void*>(fd_query), reinterpret_cast<void*>(fd_add), reinterpret_cast<void*>(fd_release),
        reinterpret_cast<void*>(fd_show), reinterpret_cast<void*>(fd_types), reinterpret_cast<void*>(fd_set_type),
        reinterpret_cast<void*>(fd_get_type), reinterpret_cast<void*>(fd_unsupported), reinterpret_cast<void*>(fd_unsupported),
        reinterpret_cast<void*>(fd_options), reinterpret_cast<void*>(fd_get_options), reinterpret_cast<void*>(fd_folder),
        reinterpret_cast<void*>(fd_folder), reinterpret_cast<void*>(fd_get_folder), reinterpret_cast<void*>(fd_result),
        reinterpret_cast<void*>(fd_name), reinterpret_cast<void*>(fd_get_name), reinterpret_cast<void*>(fd_title),
        reinterpret_cast<void*>(fd_accept), reinterpret_cast<void*>(fd_label), reinterpret_cast<void*>(fd_result),
        reinterpret_cast<void*>(fd_unsupported), reinterpret_cast<void*>(fd_suffix), reinterpret_cast<void*>(fd_close),
        reinterpret_cast<void*>(fd_unsupported), reinterpret_cast<void*>(fd_unsupported), reinterpret_cast<void*>(fd_unsupported),
        reinterpret_cast<void*>(fd_unsupported), reinterpret_cast<void*>(fd_unsupported)
    };
    auto* dialog = new FileDialog;
    dialog->table = table;
    dialog->save = save;
    int32_t status = fd_query(dialog, iid, output);
    fd_release(dialog);
    return status;
}

WINAPI int32_t win_SHCreateItemFromParsingName(const char16_t* name, void* context,
                                             const void* iid, void** output)
{
    (void)context;
    if (!output) return bad_pointer;
    *output = nullptr;
    auto path = picker_path(utf16(name));
    if (path.isEmpty() || !QFileInfo::exists(path))
        return static_cast<int32_t>(0x80070002u);
    auto* item = make_shell_item(path);
    int32_t status = item_query(item, iid, output);
    item_release(item);
    return status;
}
