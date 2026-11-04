/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <functional>
#include <memory>
class QObject;
class ProjectClip;
namespace McpMedia {
QJsonArray toolCatalog();
QJsonObject sourceState(const std::shared_ptr<ProjectClip> &source);
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState,
                     const std::function<QJsonObject()> &stateReader);
} // namespace McpMedia
