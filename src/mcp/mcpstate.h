/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <functional>
#include <memory>
#include <vector>
class QObject;
class AbstractProjectItem;
class KdenliveDoc;
class ProjectClip;
class ProjectItemModel;
class TimelineItemModel;
namespace McpState {
struct ImportedSource
{
    QString binId;
    QString path;
    std::weak_ptr<ProjectClip> clip;
};
using ClipStateReader = std::function<QJsonObject(const std::shared_ptr<TimelineItemModel> &, int)>;
/** GUI-thread snapshot. Every native record is fingerprinted, independently of
 * the bounded public projection. Header contains document/session/history fields.
 */
QJsonObject snapshot(QObject *owner, QJsonObject header, const std::shared_ptr<TimelineItemModel> &timeline, const std::shared_ptr<ProjectItemModel> &bin,
                     KdenliveDoc *document, const QList<ImportedSource> &imports, const ClipStateReader &clipStateReader);
/** Native preorder records below parent, without fixed node/depth limits. */
bool binItems(const std::shared_ptr<AbstractProjectItem> &parent, bool recursive, std::vector<std::shared_ptr<AbstractProjectItem>> &items);
QJsonArray toolCatalog();
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState,
                     const std::function<QJsonObject()> &stateReader);
} // namespace McpState
