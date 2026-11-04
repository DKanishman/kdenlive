/*
    SPDX-FileCopyrightText: 2011 Jean-Baptiste Mardelle <jb@kdenlive.org>

SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "transcodetask.h"
#include "bin/bin.h"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "kdenlivesettings.h"
#include "mainwindow.h"

#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QThread>
#include <mlt++/MltProducer.h>

#include <KLocalizedString>

TranscodeTask::TranscodeTask(const ObjectId &owner, const QString &suffix, const QString &preParams, const QString &params, TranscodeSeek::TranscodeInfo info,
                             int in, int out, bool replaceProducer, QObject *object, bool checkProfile, const QString &outputPath, bool addToProject)
    : AbstractTask(owner, AbstractTask::TRANSCODEJOB, object)
    , m_jobDuration(0)
    , m_isFfmpegJob(true)
    , m_suffix(suffix)
    , m_transcodeParams(params)
    , m_transcodePreParams(preParams)
    , m_replaceProducer(replaceProducer)
    , m_inPoint(in)
    , m_outPoint(out)
    , m_checkProfile(checkProfile)
    , m_transcodeInfo(info)
    , m_requestedOutputPath(outputPath)
    , m_addToProject(addToProject)
    , m_projectFps(pCore->getCurrentFps())
{
    m_description = i18n("Transcoding");
}

TranscodeTask::~TranscodeTask()
{
    // TaskManager may remove a queued QRunnable before run() is entered.
    // Its observers still need exactly one terminal cancellation result.
    if (!m_hasRun.load()) Q_EMIT transcodeFinished(false, true, QString(), QString());
}

void TranscodeTask::start(const ObjectId &owner, const QString &suffix, const QString &preParams, const QString &params, TranscodeSeek::TranscodeInfo info,
                          int in, int out, bool replaceProducer, QObject *object, bool force, bool checkProfile)
{
    // See if there is already a task for this MLT service and resource.
    if (pCore->taskManager.hasPendingJob(owner, AbstractTask::TRANSCODEJOB)) {
        // return;
    }
    TranscodeTask *task = new TranscodeTask(owner, suffix, preParams, params, info, in, out, replaceProducer, object, checkProfile);
    if (task) {
        // Otherwise, start a new audio levels generation thread.
        task->m_isForce = force;
        pCore->taskManager.startTask(owner.itemId, task);
    }
}

bool TranscodeTask::requestStop()
{
    if (m_isCanceled.testAndSetAcquire(0, 1)) {
        Q_EMIT jobCanceled();
        return true;
    }
    return false;
}

void TranscodeTask::run()
{
    m_hasRun.store(true);
    AbstractTaskDone whenFinished(m_owner.itemId, this);
    bool succeeded = false;
    QString output;
    QString terminalError = i18n("Native transcoding did not create a valid output.");
    const auto notifyCompletion = qScopeGuard([this, &succeeded, &output, &terminalError] {
        Q_EMIT transcodeFinished(succeeded, bool(m_isCanceled) && !succeeded, output, m_isCanceled && !succeeded ? QString() : terminalError);
    });
    if (m_isCanceled || pCore->taskManager.isBlocked()) {
        return;
    }
    QMutexLocker lock(&m_runMutex);
    m_progress = 0;
    m_running = true;
    Q_EMIT transcodeProgress(0);
    auto binClip = pCore->projectItemModel()->getClipByBinID(QString::number(m_owner.itemId));
    QTemporaryFile src;
    if (m_transcodeInfo.type == ClipType::Text || m_transcodeInfo.type == ClipType::Timeline) {
        src.setFileTemplate(QDir::temp().absoluteFilePath(QStringLiteral("XXXXXX.mlt")));
        if (src.open()) {
            m_transcodeInfo.url = src.fileName();
            QDomDocument doc;
            binClip->getProducerXML(doc, false, true);
            QTextStream out(&src);
            out << doc.toString();
            src.close();
        }
    }
    if (m_transcodeInfo.url.isEmpty()) {
        qDebug() << "::: URL TO TRANSCODE IS EMPTY...";
        return;
    }

    QString transcoderExt = m_transcodeParams.section(QLatin1String("%1"), 1).section(QLatin1Char(' '), 0, 0);
    if (transcoderExt.isEmpty()) {
        qDebug() << "// INVALID TRANSCODING PROFILE";
        m_progress = 100;
        return;
    }
    QFileInfo finfo(m_transcodeInfo.url);
    QString fileName;
    QDir dir;
    if (m_transcodeInfo.type == ClipType::Text) {
        fileName = binClip->name();
        dir = QDir(pCore->currentDoc()->url().isValid() ? pCore->currentDoc()->url().adjusted(QUrl::RemoveFilename).toLocalFile()
                                                        : KdenliveSettings::defaultprojectfolder());
    } else {
        fileName = finfo.completeBaseName();
        dir = finfo.absoluteDir();
    }
    int fileCount = 1;
    QString num = QString::number(fileCount).rightJustified(4, '0', false);
    QString path;
    if (m_suffix.isEmpty()) {
        path = fileName + num + transcoderExt;
    } else {
        path = fileName + m_suffix + transcoderExt;
        fileCount = 0;
    }
    while (dir.exists(path)) {
        ++fileCount;
        num = QString::number(fileCount).rightJustified(4, '0', false);
        path = fileName + num + m_suffix + transcoderExt;
    }
    QString destUrl = dir.absoluteFilePath(path.section(QLatin1Char('.'), 0, -2));
    std::unique_ptr<QTemporaryDir> staging;
    QString requestedOutput;
    if (!m_requestedOutputPath.isEmpty()) {
        const QFileInfo requested(m_requestedOutputPath);
        const QString parent = requested.absoluteDir().canonicalPath();
        const QString leaf = requested.fileName();
        if (!QDir::isAbsolutePath(m_requestedOutputPath) || parent.isEmpty() || requested.exists() || requested.isSymLink() || leaf.isEmpty() ||
            leaf.contains(QLatin1Char(':')) || leaf.contains(QChar::Null) || leaf.endsWith(QLatin1Char('.')) || leaf.endsWith(QLatin1Char(' ')) ||
            !QFileInfo(parent).isWritable() || requested.suffix().compare(transcoderExt.mid(1), Qt::CaseInsensitive) != 0) {
            terminalError = i18n("Use a new local output file matching the transcoding profile.");
            return;
        }
        requestedOutput = QDir(parent).absoluteFilePath(leaf);
        staging = std::make_unique<QTemporaryDir>(QDir(parent).filePath(QStringLiteral(".kdenlive-transcode-XXXXXX")));
        if (!staging->isValid()) {
            terminalError = i18n("Cannot create a private transcoding staging folder.");
            return;
        }
        destUrl = staging->filePath(QStringLiteral("result"));
    }
    auto awaitProcess = [this]() {
        if (m_requestedOutputPath.isEmpty()) {
            m_jobProcess->waitForFinished(-1);
        } else {
            // Cancellation is polled on the QProcess-owning worker thread.
            while (m_jobProcess->state() != QProcess::NotRunning && !m_jobProcess->waitForFinished(100)) {
                if (m_isCanceled) {
                    m_jobProcess->kill();
                    m_jobProcess->waitForFinished(3000);
                    break;
                }
            }
        }
        processLogInfo();
        return m_jobProcess->exitStatus() == QProcess::NormalExit && m_jobProcess->exitCode() == 0 && !m_isCanceled;
    };

    bool result;
    if (m_transcodeInfo.type == ClipType::Playlist || m_transcodeInfo.type == ClipType::SlideShow || m_transcodeInfo.type == ClipType::Text ||
        m_transcodeInfo.type == ClipType::Timeline) {
        // change FFmpeg params to MLT format
        m_isFfmpegJob = false;
        // insert transcoded filename
        m_transcodeParams.replace(QStringLiteral("%1"), QStringLiteral("-consumer %1"));
        // Convert param style
        QStringList params = m_transcodeParams.split(QLatin1Char('-'), Qt::SkipEmptyParts);
        QStringList mltParameters;
        for (const QString &s : std::as_const(params)) {
            QString t = s.simplified();
            if (t.count(QLatin1Char(' ')) == 0) {
                t.append(QLatin1String("=1"));
            } else {
                if (t.contains(QLatin1String("%1"))) {
                    // file name
                    mltParameters.prepend(t.section(QLatin1Char(' '), 1).replace(QLatin1String("%1"), QStringLiteral("avformat:%1").arg(destUrl)));
                    mltParameters.prepend(QStringLiteral("-consumer"));
                    continue;
                }
                if (t.startsWith(QLatin1String("aspect "))) {
                    // Fix aspect ratio calculation
                    t.replace(QLatin1Char(' '), QLatin1String("=@"));
                    t.replace(QLatin1Char(':'), QLatin1String("/"));
                } else {
                    t.replace(QLatin1Char(' '), QLatin1String("="));
                }
            }
            mltParameters << t;
        }
        int threadCount = QThread::idealThreadCount();
        if (threadCount > 2) {
            threadCount = qMin(threadCount - 1, 4);
        } else {
            threadCount = 1;
        }
        mltParameters.append(QStringLiteral("real_time=-%1").arg(threadCount));
        mltParameters.append(QStringLiteral("threads=%1").arg(threadCount));

        // Ask for progress reporting
        mltParameters << QStringLiteral("progress=1");
        if (m_outPoint > 0) {
            mltParameters.prepend(QStringLiteral("out=%1").arg(m_outPoint));
            mltParameters.prepend(QStringLiteral("in=%1").arg(m_inPoint));
        }
        mltParameters.prepend(m_transcodeInfo.url);
        mltParameters.prepend(QStringLiteral("error"));
        mltParameters.prepend(QStringLiteral("-loglevel"));
        mltParameters.prepend(QStringLiteral("file,pipe"));
        mltParameters.prepend(QStringLiteral("-protocol_whitelist"));
        m_jobProcess = new QProcess();
        // m_jobProcess->setProcessChannelMode(QProcess::MergedChannels);
        if (m_requestedOutputPath.isEmpty()) QObject::connect(this, &TranscodeTask::jobCanceled, m_jobProcess, &QProcess::kill, Qt::DirectConnection);
        QObject::connect(m_jobProcess, &QProcess::readyReadStandardError, this, &TranscodeTask::processLogInfo, Qt::DirectConnection);
        m_jobProcess->start(KdenliveSettings::meltpath(), mltParameters);
        AbstractTask::setPreferredPriority(m_jobProcess->processId());
        result = awaitProcess();
        delete m_jobProcess;
        m_jobProcess = nullptr;
    } else {
        m_isFfmpegJob = true;
        QStringList parameters;
        if (KdenliveSettings::ffmpegpath().isEmpty()) {
            // FFmpeg not detected, cannot process the Job
            QMetaObject::invokeMethod(pCore.get(), "displayBinMessage", Qt::QueuedConnection,
                                      Q_ARG(QString, i18n("FFmpeg not found, please set path in Kdenlive's settings Environment")),
                                      Q_ARG(int, int(KMessageWidget::Warning)));
            return;
        }
        if (binClip) {
            m_jobDuration = int(binClip->duration().seconds());
        } else {
            m_jobDuration = -1;
        }
        parameters << QStringLiteral("-y");
        if (m_inPoint > -1) {
            parameters << QStringLiteral("-ss") << QString::number(GenTime(m_inPoint, m_projectFps).seconds());
        }
        parameters << QStringLiteral("-stats");
        if (!m_transcodePreParams.isEmpty()) {
            parameters << m_transcodePreParams.split(QStringLiteral(" "));
        }
        parameters << QStringLiteral("-i") << m_transcodeInfo.url;
        if (m_outPoint > -1) {
            parameters << QStringLiteral("-to")
                       << QString::number(GenTime(m_outPoint - m_inPoint + (m_requestedOutputPath.isEmpty() ? 0 : 1), m_projectFps).seconds());
        }
        // Only output error data
        parameters << QStringLiteral("-v") << QStringLiteral("error");
        // Make sure we keep the stream order
        parameters << QStringLiteral("-sn") << QStringLiteral("-dn");
        if (!m_transcodeParams.contains(QStringLiteral("-map ")) && !m_transcodeParams.contains(QStringLiteral(" amerge="))) {
            if (!m_transcodeParams.contains(QStringLiteral("-vn "))) {
                // Use 0:V to drop cover art streams
                parameters << QStringLiteral("-map") << QStringLiteral("0:V");
            }
            if (!m_transcodeParams.contains(QStringLiteral("-an "))) {
                parameters << QStringLiteral("-map") << QStringLiteral("0:a");
            }
        }
        QStringList params = m_transcodeParams.split(QLatin1Char(' '));
        for (const QString &s : std::as_const(params)) {
            QString t = s.simplified();
            if (t.startsWith(QLatin1String("%1"))) {
                parameters << t.replace(QLatin1String("%1"), destUrl);
            } else {
                parameters << t;
            }
        }
        qDebug() << "/// FULL TRANSCODE PARAMS:\n" << parameters << "\n------";
        m_jobProcess = new QProcess();
        if (m_requestedOutputPath.isEmpty()) QObject::connect(this, &TranscodeTask::jobCanceled, m_jobProcess, &QProcess::kill, Qt::DirectConnection);
        QObject::connect(m_jobProcess, &QProcess::readyReadStandardError, this, &TranscodeTask::processLogInfo, Qt::DirectConnection);
        m_jobProcess->start(KdenliveSettings::ffmpegpath(), parameters, QIODevice::ReadOnly);
        AbstractTask::setPreferredPriority(m_jobProcess->processId());
        result = awaitProcess();
        delete m_jobProcess;
        m_jobProcess = nullptr;
    }
    destUrl.append(transcoderExt);
    if (!m_requestedOutputPath.isEmpty() && result) {
        if (m_isCanceled) return;
        {
            Mlt::Producer decoded(pCore->getProjectProfile(), destUrl.toUtf8().constData());
            if (!decoded.is_valid() || decoded.get_length() <= 0 || QFileInfo(destUrl).size() <= 0) {
                terminalError = i18n("Native transcoding output could not be decoded.");
                return;
            }
        } // Release the decoder's file handle before publishing on Windows.
        // QFile::rename never replaces an existing destination. Publication and
        // cleanup therefore cannot delete a file created by another application.
        if (m_isCanceled) return;
        if (!QFile::rename(destUrl, requestedOutput)) {
            terminalError = i18n("Cannot publish transcoded output; the destination may already exist.");
            return;
        }
        destUrl = requestedOutput;
    }
    if (result && QFileInfo(destUrl).isFile() && QFileInfo(destUrl).size() > 0) {
        succeeded = true;
        terminalError.clear();
        output = destUrl;
    } else if (!m_logDetails.isEmpty())
        terminalError = m_logDetails.right(8192);
    if (!m_addToProject) {
        m_progress = 100;
        return;
    }
    // remove temporary playlist if it exists
    m_progress = 100;
    if (m_addToProject && m_object) QMetaObject::invokeMethod(m_object, "updateJobProgress");
    if (result) {
        if (QFileInfo(destUrl).size() == 0) {
            QFile::remove(destUrl);
            // File was not created
            QMetaObject::invokeMethod(pCore.get(), "displayBinLogMessage", Qt::QueuedConnection, Q_ARG(QString, i18n("Failed to create file.")),
                                      Q_ARG(int, int(KMessageWidget::Warning)), Q_ARG(QString, m_logDetails));
        } else {
            if (m_replaceProducer && binClip && binClip->clipType() != ClipType::Timeline) {
                QMap<QString, QString> sourceProps;
                QMap<QString, QString> newProps;
                sourceProps.insert(QStringLiteral("resource"), binClip->url());
                sourceProps.insert(QStringLiteral("kdenlive:originalurl"), binClip->url());
                sourceProps.insert(QStringLiteral("kdenlive:proxy"), binClip->getProducerProperty(QStringLiteral("kdenlive:proxy")));
                sourceProps.insert(QStringLiteral("kdenlive:clipname"), binClip->clipName());
                sourceProps.insert(QStringLiteral("_fullreload"), QStringLiteral("1"));
                newProps.insert(QStringLiteral("resource"), destUrl);
                newProps.insert(QStringLiteral("kdenlive:originalurl"), destUrl);
                newProps.insert(QStringLiteral("kdenlive:clipname"), QFileInfo(destUrl).fileName());
                newProps.insert(QStringLiteral("kdenlive:proxy"), QString());
                newProps.insert(QStringLiteral("_fullreload"), QStringLiteral("1"));
                if (m_checkProfile) {
                    pCore->bin()->shouldCheckProfile = true;
                }
                pCore->bin()->slotEditClipCommand(binClip->clipId(), sourceProps, newProps);
            } else {
                QString folder = QStringLiteral("-1");
                if (binClip) {
                    auto containingFolder = std::static_pointer_cast<ProjectFolder>(binClip->parent());
                    if (containingFolder) {
                        folder = containingFolder->clipId();
                    }
                }
                QMetaObject::invokeMethod(pCore->window(), "addProjectClip", Qt::QueuedConnection, Q_ARG(QString, destUrl), Q_ARG(QString, folder));
                // id = ClipCreator::createClipFromFile(destUrl, folderId, pCore->projectItemModel());
            }
        }
    } else {
        // Proxy process crashed
        QFile::remove(destUrl);
        if (!m_isCanceled) {
            QMetaObject::invokeMethod(pCore.get(), "displayBinLogMessage", Qt::QueuedConnection, Q_ARG(QString, i18n("Failed to create file.")),
                                      Q_ARG(int, int(KMessageWidget::Warning)), Q_ARG(QString, m_logDetails));
        }
    }
}

void TranscodeTask::processLogInfo()
{
    if (!m_jobProcess) return;
    const QString buffer = QString::fromUtf8(m_jobProcess->readAllStandardError());
    m_logDetails.append(buffer);
    if (!m_requestedOutputPath.isEmpty() && m_logDetails.size() > 65536) m_logDetails = m_logDetails.right(65536);
    if (m_isFfmpegJob) {
        // Parse FFmpeg output
        if (m_jobDuration == 0) {
            if (buffer.contains(QLatin1String("Duration:"))) {
                const QString data = buffer.section(QStringLiteral("Duration:"), 1, 1).section(QLatin1Char(','), 0, 0).simplified();
                if (!data.isEmpty()) {
                    QStringList numbers = data.split(QLatin1Char(':'));
                    if (numbers.size() < 3) {
                        return;
                    }
                    m_jobDuration = numbers.at(0).toInt() * 3600 + numbers.at(1).toInt() * 60 + numbers.at(2).toInt();
                }
            }
        } else if (buffer.contains(QLatin1String("time="))) {
            int progress = 0;
            QString time = buffer.section(QStringLiteral("time="), 1, 1).simplified().section(QLatin1Char(' '), 0, 0);
            if (!time.isEmpty()) {
                QStringList numbers = time.split(QLatin1Char(':'));
                if (numbers.size() < 3) {
                    progress = time.toInt();
                    if (progress == 0) {
                        return;
                    }
                } else {
                    progress = numbers.at(0).toInt() * 3600 + numbers.at(1).toInt() * 60 + qRound(numbers.at(2).toDouble());
                }
            }
            int val = 100 * progress / m_jobDuration;
            if (m_progress != val) {
                m_progress = val;
                Q_EMIT transcodeProgress(qBound(0, val, 100));
                if (m_addToProject && m_object) QMetaObject::invokeMethod(m_object, "updateJobProgress");
            }
            // Q_EMIT jobProgress(int(100.0 * progress / m_jobDuration));
        }
    } else {
        // Parse MLT output
        if (buffer.contains(QLatin1String("percentage:"))) {
            int val = buffer.section(QStringLiteral("percentage:"), 1).simplified().section(QLatin1Char(' '), 0, 0).toInt();
            if (m_progress != val) {
                m_progress = val;
                Q_EMIT transcodeProgress(qBound(0, val, 100));
                if (m_addToProject && m_object) QMetaObject::invokeMethod(m_object, "updateJobProgress");
            }
        }
    }
}
