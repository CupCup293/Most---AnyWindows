#pragma once
#include <QByteArray>
#include <QString>
#include <functional>

namespace most {
QString win64Profile(const QByteArray& image);
void linkWin64(const QByteArray& image, const QString& stage, const QString& result,
               const std::function<void(const QString&)>& log);
}
