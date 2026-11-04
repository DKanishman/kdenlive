/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>
class QObject;
namespace McpQuick {
QJsonArray toolCatalog();
/** Raw QtQuick/menu context only; never calls native/document or QWidget MCP snapshot readers. */
QString interactionFingerprint(QObject *owner);
/** Active reachable Quick modal popup; incomplete discovery is conservatively treated as busy. */
bool modalPopupActive(QObject *owner);
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState,
                     const std::function<QJsonObject()> &stateReader);
} // namespace McpQuick
