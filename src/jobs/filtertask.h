/*
    SPDX-FileCopyrightText: 2021 Jean-Baptiste Mardelle <jb@kdenlive.org>

SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "abstracttask.h"
#include <atomic>
#include <memory>
#include <unordered_map>

class AssetParameterModel;
class EffectStackModel;
class QProcess;

class FilterTask : public AbstractTask
{
    Q_OBJECT
public:
    FilterTask(const ObjectId &owner, const QString &binId, const std::weak_ptr<AssetParameterModel> &model, const QString &assetId, int in, int out,
               const std::unordered_map<QString, QVariant> &filterParams, const std::unordered_map<QString, QString> &filterData,
               const QStringList &consumerArgs, QObject *object);
    ~FilterTask() override;

    static void start(const ObjectId &owner, const QString &binId, const std::weak_ptr<AssetParameterModel> &model, const QString &assetId, int in, int out,
                      const std::unordered_map<QString, QVariant> &filterParams, const std::unordered_map<QString, QString> &filterData,
                      const QStringList &consumerArgs, QObject *object, bool force = false);
    int length;

Q_SIGNALS:
    /** Emitted on every run exit, after any queued native result parameter update. */
    void analysisFinished(bool success, bool cancelled, const QString &resultKey, const QString &resultData);

private Q_SLOTS:
    void processLogInfo();

protected:
    void run() override;

private:
    std::atomic_bool m_hasRun{false};
    QString m_binId;
    int m_inPoint;
    int m_outPoint;
    QString m_assetId;
    bool m_onPlaylist{false};
    int m_length{0};
    std::weak_ptr<AssetParameterModel> m_model;
    std::weak_ptr<EffectStackModel> m_sourceEffects;
    std::unordered_map<QString, QVariant> m_filterParams;
    std::unordered_map<QString, QString> m_filterData;
    QStringList m_consumerArgs;
    QString m_errorMessage;
    QString m_logDetails;
    QProcess *m_jobProcess;
};
