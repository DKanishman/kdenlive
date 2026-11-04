/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <functional>
class QObject;
namespace McpProfiles {
QJsonArray toolCatalog();
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState,
                     const std::function<QJsonObject()> &stateReader);
} // namespace McpProfiles
