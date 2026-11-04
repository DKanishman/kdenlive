/*
    SPDX-FileCopyrightText: 2021 Jean-Baptiste Mardelle <jb@kdenlive.org>

SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "abstracttask.h"
#include "project/transcodeseek.h"
#include <atomic>

class QProcess;

class TranscodeTask : public AbstractTask
{
    Q_OBJECT
public:
    TranscodeTask(const ObjectId &owner, const QString &suffix, const QString &preParams, const QString &params, TranscodeSeek::TranscodeInfo info, int in,
                  int out, bool replaceProducer, QObject *object, bool checkProfile, const QString &outputPath = QString(), bool addToProject = true);
    ~TranscodeTask() override;
    static void start(const ObjectId &owner, const QString &suffix, const QString &preParams, const QString &params, TranscodeSeek::TranscodeInfo info, int in,
                      int out, bool replaceProducer, QObject *object, bool force = false, bool checkProfile = false);

Q_SIGNALS:
    /** Emitted on every native task exit, after publication and any native project handoff. */
    void transcodeFinished(bool success, bool cancelled, const QString &path, const QString &error);
    void transcodeProgress(int percent);

public:
    bool requestStop();

protected:
    void run() override;

private Q_SLOTS:
    void processLogInfo();

private:
    std::atomic_bool m_hasRun{false};
    int m_jobDuration;
    bool m_isFfmpegJob;
    QString m_suffix;
    QString m_transcodeParams;
    QString m_transcodePreParams;
    bool m_replaceProducer;
    int m_inPoint;
    int m_outPoint;
    bool m_checkProfile;
    TranscodeSeek::TranscodeInfo m_transcodeInfo;
    QProcess *m_jobProcess{nullptr};
    QString m_requestedOutputPath;
    bool m_addToProject{true};
    double m_projectFps{25.0};
    QString m_errorMessage;
    QString m_logDetails;
};
