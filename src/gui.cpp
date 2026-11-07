#include "gui.hpp"
#include "engine.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStyle>
#include <QStyleHints>
#include <QTemporaryFile>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

namespace {
using namespace Qt::StringLiterals;

QPalette windowPalette(Qt::ColorScheme scheme)
{
    auto palette = QApplication::style()->standardPalette();
    const bool dark = scheme == Qt::ColorScheme::Dark;
    const QColor surface(dark ? "#2b2d31" : "#f3f4f6");
    const QColor text(dark ? "#eff1f5" : "#20232a");
    palette.setColor(QPalette::Window, surface);
    palette.setColor(QPalette::Base, QColor(dark ? "#1f2124" : "#ffffff"));
    palette.setColor(QPalette::AlternateBase, surface);
    palette.setColor(QPalette::Button, QColor(dark ? "#3a3d43" : "#e6e8ed"));
    palette.setColor(QPalette::ToolTipBase, surface);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
                            QPalette::ToolTipText})
        palette.setColor(role, text);
    palette.setColor(QPalette::PlaceholderText, QColor(dark ? "#abb0b9" : "#565d69"));
    palette.setColor(QPalette::Mid, QColor(dark ? "#707784" : "#828994"));
    palette.setColor(QPalette::Highlight, QColor("#406098"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor(dark ? "#a6abb6" : "#646b78"));
    return palette;
}

struct BuildResult {
    QString path;
    QStringList log;
    QString error;
};

class Window : public QWidget {
public:
    Window()
    {
        setWindowTitle(u"Мост · PE → ELF"_s);
        resize(710, 590);
        setMinimumWidth(470);
        auto* page = new QVBoxLayout(this);
        page->setContentsMargins(26, 24, 26, 24);
        page->setSpacing(14);
        auto* title = new QLabel(u"Мост"_s);
        auto font = title->font();
        font.setPointSize(24);
        font.setBold(true);
        title->setFont(font);
        page->addWidget(title);
        page->addWidget(makeLabel(u"Статическая рекомпиляция Windows PE в Linux ELF"_s));

        auto* drop = new QFrame;
        drop->setObjectName(u"drop"_s);
        drop->setStyleSheet(u"QFrame#drop { border: 2px dashed palette(mid); border-radius: 12px; }"_s);
        drop->setMinimumHeight(170);
        auto* dropLayout = new QVBoxLayout(drop);
        dropLayout->setAlignment(Qt::AlignCenter);
        dropLayout->addWidget(new QLabel(u"Перетащите сюда файл .exe"_s), 0, Qt::AlignHCenter);
        dropLayout->addWidget(new QLabel(u"Для консольных программ x86 PE32"_s), 0, Qt::AlignHCenter);
        choose_ = new QPushButton(u"Выбрать файл…"_s);
        dropLayout->addWidget(choose_, 0, Qt::AlignHCenter);
        page->addWidget(drop);
        setAcceptDrops(true);
        connect(choose_, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getOpenFileName(this, u"Выберите Windows EXE"_s, {},
                u"Исполняемые файлы Windows (*.exe *.EXE);;Все файлы (*)"_s);
            if (!path.isEmpty())
                load(path);
        });

        auto* card = new QFrame;
        card->setFrameShape(QFrame::StyledPanel);
        auto* summary = new QVBoxLayout(card);
        status_ = makeLabel(u"Ожидается файл"_s);
        auto heading = status_->font();
        heading.setBold(true);
        status_->setFont(heading);
        detail_ = makeLabel(u"Совместимый EXE будет собран в ELF рядом с исходным файлом."_s);
        result_ = makeLabel({});
        validation_ = makeLabel({});
        for (auto* line : {status_, detail_, result_, validation_})
            summary->addWidget(line);
        result_->hide();
        validation_->hide();
        page->addWidget(card);

        auto* actions = new QHBoxLayout;
        assemble_ = new QPushButton(u"Собрать для Linux"_s);
        assemble_->setDefault(true);
        assemble_->setEnabled(false);
        folder_ = new QPushButton(u"Открыть папку"_s);
        folder_->hide();
        launch_ = new QPushButton(u"Открыть Rufus"_s);
        auto* launchModes = new QMenu(launch_);
        launchModes->addAction(u"Виртуальная флешка (.img)…"_s, this, [this] { launchRufus(); });
        launchModes->addAction(u"Физический USB…"_s, this, [this] { launchUsb(); });
        launch_->setMenu(launchModes);
        launch_->hide();
        actions->addWidget(assemble_);
        actions->addWidget(launch_);
        actions->addWidget(folder_);
        actions->addStretch();
        page->addLayout(actions);
        connect(assemble_, &QPushButton::clicked, this, [this] { compile(); });
        connect(folder_, &QPushButton::clicked, this, [this] {
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(output_).absolutePath()))) {
                log_->appendPlainText(u"Не удалось открыть папку."_s);
                toggle_->setChecked(true);
            }
        });
        progress_ = new QProgressBar;
        progress_->setRange(0, 0);
        progress_->setTextVisible(false);
        progress_->hide();
        page->addWidget(progress_);

        toggle_ = new QToolButton;
        toggle_->setText(u"Технические подробности и журнал"_s);
        toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle_->setArrowType(Qt::RightArrow);
        toggle_->setCheckable(true);
        page->addWidget(toggle_, 0, Qt::AlignLeft);
        auto* panel = new QWidget;
        auto* details = new QVBoxLayout(panel);
        details->setContentsMargins(0, 0, 0, 0);
        log_ = new QPlainTextEdit;
        log_->setReadOnly(true);
        log_->setMinimumHeight(120);
        details->addWidget(log_);
        auto* copy = new QPushButton(u"Копировать журнал"_s);
        details->addWidget(copy, 0, Qt::AlignLeft);
        page->addWidget(panel, 1);
        panel->hide();
        page->addStretch();
        connect(toggle_, &QToolButton::toggled, this, [this, panel](bool open) {
            toggle_->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            panel->setVisible(open);
        });
        connect(copy, &QPushButton::clicked, this, [this] {
            QApplication::clipboard()->setText(log_->toPlainText());
        });

        connect(&analysis_, &QFutureWatcher<most::Report>::finished, this, [this] {
            report_ = analysis_.result();
            log_->setPlainText(report_.log());
            QSet<QString> dlls;
            for (const auto& item : report_.imports)
                dlls.insert(item.dll.toLower());
            QStringList lines{QFileInfo(input_).fileName(), u"%1 · DLL: %2 · Импорты: %3"_s
                                  .arg(report_.architecture).arg(dlls.size()).arg(report_.imports.size())};
            if (report_.ready()) {
                status_->setText(u"Готово к сборке"_s);
                if (report_.hasWin64Profile())
                    lines << u"Экспериментальный профиль %1"_s.arg(report_.profile);
                else
                    lines << most::dataNote;
            } else {
                status_->setText(u"Сборка недоступна"_s);
                lines << report_.problems.mid(0, 3);
                if (report_.problems.size() > 3)
                    lines << u"Остальные причины в технических подробностях."_s;
            }
            if (!report_.notes.isEmpty())
                lines << report_.notes.last();
            detail_->setText(lines.join(u'\n'));
            setBusy(false);
            if (report_.ready())
                compile();
        });

        connect(&build_, &QFutureWatcher<BuildResult>::finished, this, [this] {
            const auto done = build_.result();
            log_->appendPlainText(done.log.join(u'\n'));
            if (!done.error.isEmpty()) {
                status_->setText(u"Ошибка сборки"_s);
                result_->setText(done.error);
                log_->appendPlainText(done.error);
            } else {
                output_ = done.path;
                status_->setText(report_.hasWin64Profile() ? u"Собрано · %1"_s.arg(report_.profile)
                                                         : u"Собрано"_s);
                result_->setText(output_);
                validation_->setText(u"Запуск не проверен"_s);
                validation_->show();
                folder_->show();
                if (isRufus()) {
                    launch_->show();
                    launch_->setDefault(true);
                }
            }
            result_->show();
            setBusy(false);
        });
    }

    void load(const QString& path)
    {
        if (busy_)
            return;
        clearResult();
        input_ = path;
        report_ = {};
        status_->setText(u"Анализ…"_s);
        detail_->setText(QFileInfo(path).fileName());
        log_->clear();
        toggle_->setChecked(false);
        setBusy(true);
        analysis_.setFuture(QtConcurrent::run([path] { return most::analyze(path); }));
    }

    void compile()
    {
        if (busy_ || !report_.ready())
            return;
        clearResult();
        auto path = most::defaultOutput(input_);
        if (isRufus()) {
            const auto base = path;
            for (int index = 2; QFileInfo::exists(path) || QFileInfo(path).isSymLink(); ++index)
                path = u"%1-%2"_s.arg(base).arg(index);
        }
        status_->setText(u"Сборка…"_s);
        setBusy(true);
        build_.setFuture(QtConcurrent::run([report = report_, path] {
            BuildResult result{path, {}, {}};
            try {
                most::build(report, path, [&](const QString& message) { result.log << message; });
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        }));
    }

protected:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (!busy_ && localFile(event->mimeData()))
            event->acceptProposedAction();
    }

    void dropEvent(QDropEvent* event) override
    {
        if (!busy_ && localFile(event->mimeData())) {
            load(event->mimeData()->urls().front().toLocalFile());
            event->acceptProposedAction();
        }
    }

private:
    bool isRufus() const { return report_.profile == u"Rufus 4.15"; }

    void launchRufus()
    {
        const auto image = QFileDialog::getOpenFileName(this, u"Выберите исходный образ для Rufus"_s,
            QFileInfo(input_).absolutePath(), u"Все файлы (*)"_s);
        if (image.isEmpty())
            return;
        const auto target = QFileDialog::getSaveFileName(this, u"Создайте файл виртуальной флешки"_s,
            output_ + u"-usb.img"_s, u"Файловый носитель (*.img)"_s, nullptr, QFileDialog::DontConfirmOverwrite);
        if (target.isEmpty())
            return;
        const auto path = target.endsWith(u".img") ? target : target + u".img"_s;
        QFile disk(path);
        constexpr qint64 mib = 1024 * 1024;
        const auto size = qMax(8 * 1024 * mib, QFileInfo(image).size() + 256 * mib);
        if (!disk.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            QMessageBox::warning(this, u"Rufus"_s, u"Выберите новое имя для файла носителя. %1"_s.arg(disk.errorString()));
            return;
        }
        if (!disk.resize(size)) {
            const auto error = disk.errorString();
            disk.remove();
            QMessageBox::warning(this, u"Rufus"_s, u"Не удалось создать носитель. %1"_s.arg(error));
            return;
        }
        disk.close();

        const auto journal = startRufus({u"--image"_s, image, u"--target-image"_s, path});
        if (journal.isEmpty()) {
            disk.remove();
            return;
        }
        validation_->setText(u"В Rufus нажмите SELECT, подтвердите образ и нажмите START. "
                             "Результат записи останется в %1.\nЖурнал запуска находится в %2."_s
                             .arg(path, journal));
    }

    void launchUsb()
    {
        QProcess listing;
        listing.start(output_, {u"--list-usb"_s});
        if (!listing.waitForFinished(2000) || listing.exitStatus() != QProcess::NormalExit ||
            listing.exitCode() != 0) {
            QMessageBox::warning(this, u"Rufus"_s, u"Не удалось получить список USB. %1"_s
                .arg(QString::fromLocal8Bit(listing.readAllStandardError()).trimmed()));
            return;
        }
        const auto devices = QString::fromLocal8Bit(listing.readAllStandardOutput())
            .split(u'\n', Qt::SkipEmptyParts);
        if (devices.isEmpty()) {
            QMessageBox::information(this, u"Rufus"_s, u"Съёмные USB-диски не найдены."_s);
            return;
        }

        const auto selection = QInputDialog::getItem(this, u"Физический USB"_s,
            u"Выберите накопитель, который можно стереть. Rufus откроется в нативном режиме."_s,
            devices, 0, false);
        if (selection.isEmpty())
            return;
        const auto denied = u" (no read/write permission)"_s;
        const bool authorize = selection.endsWith(denied);
        const auto device = authorize ? selection.chopped(denied.size()) : selection;
        if (authorize && QMessageBox::question(this, u"Доступ к USB"_s,
                u"Запросить у Linux права чтения и записи для выбранного накопителя?\n%1"_s.arg(device),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;

        const auto option = authorize ? u"--authorize-usb"_s : u"--usb"_s;
        const auto journal = startRufus({u"--native"_s, option, device});
        if (!journal.isEmpty())
            validation_->setText(u"Выбран USB %1. При запросе системы подтвердите доступ.\n"
                                 "Журнал запуска находится в %2."_s.arg(device, journal));
    }

    QString startRufus(const QStringList& arguments)
    {
        QTemporaryFile journal(QDir::tempPath() + u"/most-rufus-XXXXXX.log"_s);
        if (!journal.open()) {
            QMessageBox::warning(this, u"Rufus"_s, u"Не удалось создать журнал запуска."_s);
            return {};
        }
        journal.close();

        QProcess process;
        process.setProgram(output_);
        process.setArguments(arguments);
        process.setStandardOutputFile(journal.fileName(), QIODevice::Append);
        process.setStandardErrorFile(journal.fileName(), QIODevice::Append);
        if (!process.startDetached()) {
            QMessageBox::warning(this, u"Rufus"_s, u"Не удалось запустить Rufus. %1"_s.arg(process.errorString()));
            return {};
        }
        journal.setAutoRemove(false);
        return journal.fileName();
    }

    static bool localFile(const QMimeData* data)
    {
        const auto urls = data->urls();
        return urls.size() == 1 && urls.front().isLocalFile();
    }

    static QLabel* makeLabel(const QString& text)
    {
        auto* label = new QLabel(text);
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    }

    void setBusy(bool busy)
    {
        busy_ = busy;
        progress_->setVisible(busy);
        choose_->setEnabled(!busy);
        assemble_->setEnabled(!busy && report_.ready());
    }

    void clearResult()
    {
        output_.clear();
        result_->hide();
        validation_->hide();
        folder_->hide();
        launch_->hide();
    }

    bool busy_ = false;
    most::Report report_;
    QString input_, output_;
    QLabel *status_, *detail_, *result_, *validation_;
    QPushButton *choose_, *assemble_, *folder_, *launch_;
    QToolButton* toggle_;
    QProgressBar* progress_;
    QPlainTextEdit* log_;
    QFutureWatcher<most::Report> analysis_;
    QFutureWatcher<BuildResult> build_;
};
}

int runGui(int argc, char** argv, bool openInput)
{
    QApplication app(argc, argv);
    QApplication::setStyle(u"Fusion"_s);
    const auto* hints = QApplication::styleHints();
    auto updatePalette = [hints] { QApplication::setPalette(windowPalette(hints->colorScheme())); };
    QObject::connect(hints, &QStyleHints::colorSchemeChanged, &app, updatePalette);
    updatePalette();
    QApplication::setApplicationDisplayName(u"Мост"_s);
    Window window;
    window.show();
    if (openInput && app.arguments().size() > 1)
        window.load(app.arguments()[1]);
    return app.exec();
}
