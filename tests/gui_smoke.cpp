#include "../src/gui.cpp"

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>

static bool hasLabel(Window& window, const QString& text)
{
    for (const auto* label : window.findChildren<QLabel*>())
        if (label->isVisible() && label->text().contains(text))
            return true;
    return false;
}

static bool waitFor(Window& window, const QString& text)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 60000) {
        QApplication::processEvents();
        if (hasLabel(window, text))
            return true;
        QThread::msleep(10);
    }
    return false;
}

static bool drop(Window& window, const QString& file)
{
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath())});
    QDragEnterEvent enter(QPointF(50, 120), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &enter);
    QDropEvent event(QPointF(50, 120), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &event);
    return enter.isAccepted() && event.isAccepted();
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (app.arguments().size() != 3 && app.arguments().size() != 4)
        return 2;
    QApplication::setStyle(u"Fusion"_s);
    const auto dark = windowPalette(Qt::ColorScheme::Dark);
    const auto light = windowPalette(Qt::ColorScheme::Light);
    if (dark.color(QPalette::WindowText).lightness() < 220 ||
        dark.color(QPalette::Window).lightness() > 60 ||
        light.color(QPalette::WindowText).lightness() > 70 ||
        light.color(QPalette::Window).lightness() < 190)
        return 8;
    QApplication::setPalette(dark);
    Window window;
    window.show();
    QApplication::processEvents();
    QTemporaryDir output;
    const auto input = output.filePath(u"packed sample.exe"_s);
    const auto elf = output.filePath(u"packed sample-linux"_s);
    if (!QFile::copy(app.arguments()[1], input) || !drop(window, input))
        return 3;
    if (!waitFor(window, u"Собрано"_s) || !hasLabel(window, u"Запуск не проверен"_s) ||
        !hasLabel(window, u"UPX распакован автоматически."_s) || !QFileInfo::exists(elf))
        return 4;
    const auto rufus = output.filePath(u"rufus sample.exe"_s);
    const auto rufusElf = output.filePath(u"rufus sample-linux"_s);
    // A previous sibling build must survive while the GUI builds an updated Rufus.
    QFile prior(rufusElf);
    if (!prior.open(QIODevice::WriteOnly | QIODevice::NewOnly) || prior.write("previous") != 8)
        return 5;
    prior.close();
    if (!QFile::copy(app.arguments()[2], rufus) || !drop(window, rufus) ||
        !waitFor(window, u"Собрано · Rufus 4.15"_s) ||
        !hasLabel(window, u"Запуск в песочнице Wayland."_s) || !QFileInfo::exists(rufusElf + u"-2"_s) ||
        !prior.open(QIODevice::ReadOnly) || prior.readAll() != "previous")
        return 5;
    bool launchVisible = false;
    for (auto* button : window.findChildren<QPushButton*>()) {
        if (button->text() == u"Открыть Rufus" && button->isVisible() && button->isEnabled() &&
            button->menu() && button->menu()->actions().size() == 2)
            launchVisible = true;
        if (button->text() == u"Копировать журнал")
            button->click();
    }
    const auto journal = QApplication::clipboard()->text();
    if (!launchVisible || !journal.startsWith(most::buildVersionLine() + u'\n') ||
        !journal.contains(u"KERNEL32.DLL!DeviceIoControl"))
        return 7;
    if (app.arguments().size() == 4) {
        const auto w32ver = output.filePath(u"w32ver sample.exe"_s);
        const auto w32verElf = output.filePath(u"w32ver sample-linux"_s);
        if (!QFile::copy(app.arguments()[3], w32ver) || !drop(window, w32ver) ||
            !waitFor(window, u"Собрано · W32Ver x64"_s) ||
            !hasLabel(window, u"Запуск не проверен"_s) || !QFileInfo::exists(w32verElf))
            return 9;
        std::puts("Qt PASS — W32Ver drag and drop and sibling ELF");
    }
    const auto batch = output.filePath(u"batch"_s);
    const auto nested = batch + u"/nested"_s;
    if (!QDir().mkpath(nested))
        return 10;
    const auto batchExe = nested + u"/sample.EXE"_s;
    const auto batchElf = nested + u"/sample-linux"_s;
    QFile bad(batch + u"/bad.exe"_s);
    QFile existing(batchElf);
    if (!QFile::copy(app.arguments()[1], batchExe) ||
        !bad.open(QIODevice::WriteOnly | QIODevice::NewOnly) || bad.write("invalid") != 7 ||
        !existing.open(QIODevice::WriteOnly | QIODevice::NewOnly) || existing.write("existing") != 8)
        return 10;
    bad.close();
    existing.close();
    if (!drop(window, batch) || !waitFor(window, u"Обработка папки завершена"_s) ||
        !hasLabel(window, u"Собрано: 1 · Сборка недоступна: 1 · Ошибок сборки: 0"_s) ||
        !QFileInfo::exists(batchElf + u"-2"_s) ||
        !existing.open(QIODevice::ReadOnly) || existing.readAll() != "existing")
        return 10;
    const auto batchLog = window.findChild<QPlainTextEdit*>()->toPlainText();
    if (!batchLog.contains(u"bad.exe") || !batchLog.contains(u"nested/sample.EXE"))
        return 10;
    std::puts("Qt PASS — recursive folder build, per-file diagnostics and existing ELF preserved");
    std::puts("Qt PASS — light/dark palette, drag and drop, automatic UPX unpacking and sibling build, separate run status, experimental Rufus build, clipboard");
}
