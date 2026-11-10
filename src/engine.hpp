#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QList>
#include <functional>

namespace most {
struct Import {
    QString dll, name;
    quint64 slot;
    bool delayed;
};

struct Report {
    QString architecture = QStringLiteral("Не определена");
    QList<Import> imports;
    QStringList problems;
    QStringList notes;
    QString source;
    QByteArray win64Image;
    QByteArray peData;
    QString peAssembly;
    QString profile;
    qsizetype instructions = 0;
    bool needsWidgets = false;
    bool hasWin64Profile() const { return !win64Image.isEmpty(); }
    bool ready() const { return (!source.isEmpty() || hasWin64Profile()) && problems.isEmpty(); }
    QString log() const;
};

extern const QString dataNote;
QString buildVersionLine();
Report analyze(const QString& path);
QString defaultOutput(const QString& input);
void build(const Report& report, const QString& output,
           const std::function<void(const QString&)>& log);
}
