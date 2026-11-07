#include "gui.hpp"
#include "engine.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStyle>
#include <QStyleHints>
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
        actions->addWidget(assemble_);
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
        const auto path = most::defaultOutput(input_);
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
    }

    bool busy_ = false;
    most::Report report_;
    QString input_, output_;
    QLabel *status_, *detail_, *result_, *validation_;
    QPushButton *choose_, *assemble_, *folder_;
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
