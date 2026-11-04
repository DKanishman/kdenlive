/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>
class QObject;
namespace McpMonitor {
// Observational native monitor context; natural playback frames do not change it.
QString interactionFingerprint(QObject *owner);
QJsonArray toolCatalog();
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState,
                     const std::function<QJsonObject()> &stateReader);
} // namespace McpMonitor
