/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QPointer>
#include <memory>
#include <vector>

class QAbstractItemModel;
class AssetParameterModel;
class EffectItemModel;
class EffectStackModel;
class KdenliveDoc;
class QLocalSocket;
class ProjectClip;
class QProcess;
class QWidget;
class TimelineItemModel;

/** Opt-in, same-user local MCP endpoint. All dispatch runs on the GUI thread.
 *  Construct after Core::initGUI and destroy before Core::clean.
 *  Native model operations and an explicit, state-checked GUI control bridge.
 */
class KdenliveMcpServer : public QObject
{
    Q_OBJECT
public:
    explicit KdenliveMcpServer(QObject *parent = nullptr);
    ~KdenliveMcpServer() override;
    bool start(const QString &socketName, QString *error = nullptr);
    /** Exact transport dispatcher; an empty result means no notification reply. */
    QJsonObject handleRequest(const QJsonObject &request);

private:
    void acceptClient();
    void readClient();
    void send(const QJsonObject &response);
    QJsonObject snapshot();
    QJsonObject callTool(const QString &name, const QJsonObject &arguments);
    static QJsonArray toolCatalog();
    static QJsonArray timelineToolCatalog();
    QJsonObject callTimelineTool(const QString &, const QJsonObject &, const QJsonObject &);
    static QJsonArray effectToolCatalog();
    QJsonObject callEffectTool(const QString &, const QJsonObject &, const QJsonObject &);
    static QJsonArray alignmentToolCatalog();
    QJsonObject callAlignmentTool(const QString &, const QJsonObject &, const QJsonObject &);
    static QJsonArray lifecycleToolCatalog();
    QJsonObject callLifecycleTool(const QString &, const QJsonObject &, const QJsonObject &);
    void lifecycleCleanup();
    static QJsonArray uiToolCatalog();
    QJsonObject callUiTool(const QString &, const QJsonObject &, const QJsonObject &);
    QJsonObject uiSnapshot();
    QString uiModelToken(QAbstractItemModel *model);
    QHash<QAbstractItemModel *, QString> m_uiModelIds;
    QHash<QAbstractItemModel *, quint64> m_uiModelRevisions;
    QHash<QString, QJsonObject> m_analysisJobs;
    QHash<QString, std::weak_ptr<AssetParameterModel>> m_analysisModels;
    QHash<QString, QJsonObject> m_lifecycleJobs;
    QHash<QString, QPointer<QProcess>> m_renderProcesses;
    bool m_lifecycleBusy{false};
    QHash<QString, QPointer<QWidget>> m_uiControls;
    quint64 m_uiControlCounter{0};
    QHash<QString, QJsonObject> m_uiJobs;
    struct MediaImport
    {
        QString path;
        std::weak_ptr<ProjectClip> clip;
    };
    QHash<QString, MediaImport> m_imports;
    struct OwnedEffect
    {
        int clipId;
        QString kind;
        std::weak_ptr<EffectStackModel> stack;
        std::weak_ptr<EffectItemModel> effect;
    };
    std::vector<OwnedEffect> m_ownedEffects;
    QLocalServer m_server;
    QPointer<QLocalSocket> m_client;
    QByteArray m_input;
    QPointer<KdenliveDoc> m_document;
    std::weak_ptr<TimelineItemModel> m_timeline;
    QMetaObject::Connection m_undoConnection;
    const QString m_sessionId;
    quint64 m_epoch{0};
    quint64 m_revision{0};
    bool m_freshProject{false};
    bool m_initialized{false};
    bool m_reading{false};
    bool m_dispatching{false};
};
