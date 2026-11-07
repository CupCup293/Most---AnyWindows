#pragma once

#include "engine.hpp"
#include <QByteArray>
#include <array>
#include <optional>
#include <stdexcept>

namespace most {
using namespace Qt::StringLiterals;

struct Failure : std::runtime_error {
    explicit Failure(const QString& text) : std::runtime_error(text.toStdString()) {}
};

struct Directory {
    quint32 rva = 0, size = 0;
    explicit operator bool() const { return rva || size; }
};

struct Section {
    QByteArray name;
    quint32 rva, size, raw, rawSize, flags;
    bool executable() const { return flags & 0x20000000; }
    bool writable() const { return flags & 0x80000000; }
};

struct Pe {
    QByteArray bytes;
    quint16 machine, magic, subsystem, characteristics;
    quint32 entry, imageSize, headers;
    quint64 base;
    QList<Section> sections;
    std::array<Directory, 16> directories{};

    explicit Pe(QByteArray input);
    void readImports(bool delayed, QList<Import>& result) const;
    QByteArray codeAt(quint32 address) const;
    void validateImage() const;

private:
    quint64 number(quint64 offset, int size) const;
    std::optional<quint64> fileOffset(quint64 rva, quint64 size) const;
    quint64 mapped(quint64 rva, quint64 size) const;
    QString name(quint64 rva) const;
};
}
