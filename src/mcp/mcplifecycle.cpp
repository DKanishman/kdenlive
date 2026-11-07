/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "bin/bin.h"
#include "bin/model/markerlistmodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "doc/kthumb.h"
#include "jobs/taskmanager.h"
#include "kdenlivesettings.h"
#include "mcphelpers.h"
#include "mcpquick.h"
#include "mcprendercheck.h"
#include "mcpsequence.h"
#include "mcpserver.h"
#include "monitor/monitor.h"
#include "monitor/monitormanager.h"
#include "profiles/profilemodel.hpp"
#include "profiles/profilerepository.hpp"
#include "project/projectmanager.h"
#include "render/renderrequest.h"
#include "renderpresets/renderpresetmodel.hpp"
#include "renderpresets/renderpresetrepository.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QProcess>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <functional>
#include <limits>
#include <memory>
#include <mlt++/MltFrame.h>
#include <mlt++/MltProducer.h>
#include <mlt++/MltProfile.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#elif defined(Q_OS_LINUX)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(Q_OS_MACOS)
#include <stdio.h>
#endif
namespace {
constexpr int MaxJobs = 64;
QString key(const char *value)
{
    return QString::fromLatin1(value);
}
QJsonObject text(const char *description)
{
    return Mcp::property(key("string"), key(description));
}
QJsonObject number(const char *description)
{
    return Mcp::property(key("integer"), key(description));
}
QJsonObject boolean(const char *description)
{
    return Mcp::property(key("boolean"), key(description));
}
QJsonObject withState(QJsonObject properties)
{
    properties.insert(key("expectedState"), text("Exact expectedState from the most recent state."));
    return properties;
}
QJsonArray required(std::initializer_list<const char *> names)
{
    QJsonArray result{key("expectedState")};
    for (auto name : names)
        result.append(key(name));
    return result;
}
bool localPath(const QJsonValue &value, QString &path, bool existing)
{
    if (!value.isString()) return false;
    path = value.toString();
    if (path.isEmpty() || path.size() > 4096 || path.contains(QChar::Null) || !QDir::isAbsolutePath(path)) return false;
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(path);
    if (native.startsWith(QStringLiteral("\\\\.\\")) || native.startsWith(QStringLiteral("\\\\?\\GLOBALROOT"), Qt::CaseInsensitive)) return false;
    if (native.indexOf(QLatin1Char(':'), 2) >= 0) return false;
#endif
    const QFileInfo info(path);
    if (existing) return info.exists() && info.isFile() && info.isReadable();
    return !info.exists() && info.dir().exists() && QFileInfo(info.absolutePath()).isWritable();
}
bool terminal(const QString &status)
{
    return status == key("completed") || status == key("failed") || status == key("canceled");
}
bool makeRoom(QHash<QString, QJsonObject> &jobs, const QString &preserve = {})
{
    if (jobs.size() < MaxJobs) return true;
    for (auto it = jobs.begin(); it != jobs.end(); ++it) {
        if (it.key() != preserve && terminal(it.value().value(key("status")).toString())) {
            bool needed = false;
            for (auto candidate = jobs.cbegin(); candidate != jobs.cend(); ++candidate)
                if (candidate.value().value(key("status")) == key("queued") && candidate.value().value(key("queueAfterJobId")) == it.key()) needed = true;
            if (needed) continue;
            jobs.erase(it);
            return true;
        }
    }
    return false;
}
QJsonObject publicJob(QJsonObject job, bool dialog)
{
    if (job.value(key("kind")) == key("render") && !job.contains(key("outputs"))) {
        QJsonArray pending;
        for (const auto &entry : job.value(key("plannedOutputs")).toArray()) {
            auto output = entry.toObject();
            output.insert(key("path"), job.value(key("outputKind")) == key("directory")
                                           ? QDir(job.value(key("path")).toString()).absoluteFilePath(output.value(key("fileName")).toString())
                                           : job.value(key("path")).toString());
            output.remove(key("stagedPath"));
            output.remove(key("subtitlePath"));
            pending.append(output);
        }
        job.insert(key("outputs"), pending);
    }
    for (const char *name : {"steps", "stepIndex", "stagingDir", "stagingPath", "muxPath", "playlists", "plannedOutputs", "progressTail"})
        job.remove(key(name));
    if (dialog && job.value(key("kind")) == key("project") && (job.value(key("status")) == key("running") || job.value(key("status")) == key("queued"))) {
        if (const auto progress = qobject_cast<QProgressDialog *>(QApplication::activeModalWidget())) {
            // Native loading can process MCP polls inside a modal progress loop.
            // It is still working; only a nested decision dialog needs an answer.
            job.insert(key("progressUi"), QJsonObject{{key("active"), true},
                                                      {key("value"), progress->value()},
                                                      {key("minimum"), progress->minimum()},
                                                      {key("maximum"), progress->maximum()}});
        } else {
            job.insert(key("status"), key("needsUserAction"));
            job.insert(key("message"), key("A native project dialog needs a decision. Inspect get_ui and use the explicit UI controls."));
        }
    }
    return job;
}
QString taskStatus(TaskManagerStatus value)
{
    switch (value) {
    case TaskManagerStatus::NoJob:
        return key("none");
    case TaskManagerStatus::Pending:
        return key("pending");
    case TaskManagerStatus::Running:
        return key("running");
    case TaskManagerStatus::Finished:
        return key("finished");
    case TaskManagerStatus::Canceled:
        return key("canceled");
    }
    return key("unknown");
}
QJsonObject mediaJob(const QString &id, const std::shared_ptr<ProjectClip> &clip)
{
    const ObjectId owner(KdenliveObjectType::BinClip, id.toInt(), QUuid());
    QJsonArray types;
    for (int type = AbstractTask::PROXYJOB; type <= AbstractTask::MELTJOB; ++type)
        if (pCore->taskManager.hasPendingJob(owner, static_cast<AbstractTask::JOBTYPE>(type))) types.append(type);
    QJsonObject item{{key("binId"), id},
                     {key("status"), taskStatus(pCore->taskManager.jobStatus(owner))},
                     {key("progress"), pCore->taskManager.getJobProgressForClip(owner)},
                     {key("jobTypes"), types},
                     {key("ready"), clip && clip->statusReady()}};
    if (clip && clip->statusReady()) {
        item.insert(key("resource"), clip->url());
        const QString proxy = clip->getProducerProperty(key("kdenlive:proxy"));
        item.insert(key("proxy"), proxy);
        item.insert(key("proxyAvailable"), !proxy.isEmpty() && proxy != key("-") && QFileInfo::exists(proxy));
        item.insert(key("proxyActive"), clip->hasProxy());
        item.insert(key("activeResource"), clip->getProducerProperty(key("resource")));
    }
    return item;
}
void removeRenderArtifacts(const QJsonObject &job, bool removeStaging)
{
    for (const auto &file : job.value(key("playlists")).toArray()) {
        const QString path = file.toString();
        if (!path.isEmpty()) QFile::remove(path);
    }
    if (removeStaging) {
        const QFileInfo dir(job.value(key("stagingDir")).toString());
        if (dir.isDir() && !dir.isSymLink() && dir.fileName().startsWith(key(".kdenlive-mcp-render-"))) QDir(dir.absoluteFilePath()).removeRecursively();
    }
}
bool publishDirectory(const QString &source, const QString &destination)
{
    // Qt's preliminary exists check cannot make a directory rename exclusive
    // against an independent creator. Use the native no-replace operation.
#ifdef Q_OS_WIN
    const QString from = QDir::toNativeSeparators(source), to = QDir::toNativeSeparators(destination);
    return MoveFileW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16())) != 0;
#elif defined(Q_OS_LINUX) && defined(SYS_renameat2)
    const QByteArray from = QFile::encodeName(source), to = QFile::encodeName(destination);
    constexpr unsigned int NoReplace = 1;
    return syscall(SYS_renameat2, AT_FDCWD, from.constData(), AT_FDCWD, to.constData(), NoReplace) == 0;
#elif defined(Q_OS_MACOS) && defined(RENAME_EXCL)
    const QByteArray from = QFile::encodeName(source), to = QFile::encodeName(destination);
    return renamex_np(from.constData(), to.constData(), RENAME_EXCL) == 0;
#else
    Q_UNUSED(source)
    Q_UNUSED(destination)
    return false; // Retain verified staging when exclusive publication is unavailable.
#endif
}
bool safeOutputLeaf(const QString &name)
{
    static const QRegularExpression invalid(QStringLiteral("[<>:\"/\\\\|?*%\\x00-\\x1f]"));
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption);
    return !name.isEmpty() && name.size() <= 180 && name != key(".") && name != key("..") && QFileInfo(name).fileName() == name &&
           !name.endsWith(QLatin1Char('.')) && !name.endsWith(QLatin1Char(' ')) && !invalid.match(name).hasMatch() && !reserved.match(name).hasMatch();
}
bool ownedLeafPath(const QString &path, const QString &directory)
{
    const QFileInfo info(path);
    return QDir::isAbsolutePath(path) && info.absoluteDir().canonicalPath() == QDir(directory).canonicalPath() && !info.isSymLink();
}
QJsonObject nativeOutput(const RenderRequest::RenderJob &job, QString &error)
{
    QFile playlist(job.playlistPath);
    QDomDocument snapshot;
    if (!playlist.open(QIODevice::ReadOnly) || playlist.size() > 64 * 1024 * 1024 || !snapshot.setContent(&playlist)) {
        error = key("Cannot inspect the bounded native export snapshot.");
        return {};
    }
    const auto consumer = snapshot.documentElement().firstChildElement(key("consumer"));
    bool inValid = false, outValid = false;
    const int in = consumer.attribute(key("in")).toInt(&inValid), out = consumer.attribute(key("out")).toInt(&outValid);
    if (consumer.isNull() || !inValid || !outValid || in < 0 || out < in || qint64(out) - in + 1 > 1000000 ||
        consumer.attribute(key("target")) != job.outputPath) {
        error = key("Native export returned invalid bounds or target metadata.");
        return {};
    }
    // Installed MLT presets may leave f=image2 inside properties=stills/PNG.
    // Use the same native classifier as RenderRequest rather than requiring the
    // resolved format to appear explicitly in the generated consumer element.
    RenderPresetParams generatedParams;
    const auto attributes = consumer.attributes();
    for (int index = 0; index < attributes.size(); ++index) {
        const auto attribute = attributes.item(index).toAttr();
        generatedParams.insert(attribute.name(), attribute.value());
    }
    const bool sequence = generatedParams.isImageSequence();
    return {{key("stagedPath"), job.outputFile},
            {key("fileName"), QFileInfo(job.outputFile).fileName()},
            {key("kind"), sequence ? key("imageSequence") : key("media")},
            {key("inFrame"), in},
            {key("outFrame"), out},
            {key("expectedFrameCount"), out - in + 1},
            {key("subtitlePath"), job.subtitlePath},
            {key("audioOnly"), consumer.attribute(key("video_off")).toInt() == 1 || consumer.attribute(key("vn")).toInt() == 1},
            {key("audioDisabled"), consumer.attribute(key("audio_off")).toInt() == 1 || consumer.attribute(key("an")).toInt() == 1}};
}
class RenderOutputVerifier : public QObject
{
public:
    using Completion = std::function<void(bool, const QJsonArray &, const QString &)>;
    RenderOutputVerifier(const QJsonObject &job, Completion completion, QObject *parent)
        : QObject(parent)
        , m_stage(job.value(key("stagingDir")).toString())
        , m_target(job.value(key("path")).toString())
        , m_directoryOutput(job.value(key("outputKind")) == key("directory"))
        , m_planned(job.value(key("plannedOutputs")).toArray())
        , m_completion(std::move(completion))
    {
        m_profile.set_explicit(1);
        m_profile.set_frame_rate(job.value(key("frameRateNumerator")).toInt(), job.value(key("frameRateDenominator")).toInt());
    }
    void start()
    {
        QTimer::singleShot(0, this, [this] { advance(); });
    }
    void stop()
    {
        m_stopped = true;
        deleteLater();
    }

private:
    void finish(bool ok, const QString &message = {})
    {
        if (m_stopped) return;
        m_stopped = true;
        m_completion(ok, m_outputs, message);
        deleteLater();
    }
    bool beginOutput()
    {
        m_output = m_planned[m_index].toObject();
        const QString path = m_output.value(key("stagedPath")).toString();
        if (!ownedLeafPath(path, m_stage)) {
            finish(false, key("Native output escaped the owned staging directory."));
            return false;
        }
        m_bytes = 0;
        m_count = 0;
        m_first = std::numeric_limits<int>::max();
        m_last = -1;
        m_numbers.clear();
        if (m_output.value(key("kind")) == key("imageSequence")) {
            const QString leaf = m_output.value(key("fileName")).toString();
            static const QRegularExpression counter(QStringLiteral("%[0-9]*d"));
            const auto match = counter.match(leaf);
            if (!match.hasMatch() || counter.match(leaf, match.capturedEnd()).hasMatch()) {
                finish(false, key("Native image sequence has an invalid counter pattern."));
                return false;
            }
            m_numbered =
                QRegularExpression(QRegularExpression::anchoredPattern(QRegularExpression::escape(leaf.left(match.capturedStart())) +
                                                                       QStringLiteral("([0-9]+)") + QRegularExpression::escape(leaf.mid(match.capturedEnd()))));
            m_files = std::make_unique<QDirIterator>(m_stage, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        }
        return true;
    }
    void completeOutput()
    {
        const QString leaf = m_output.value(key("fileName")).toString();
        m_output.insert(key("path"), m_directoryOutput ? QDir(m_target).absoluteFilePath(leaf) : m_target);
        m_output.insert(key("bytes"), m_bytes);
        m_output.remove(key("stagedPath"));
        m_output.remove(key("subtitlePath"));
        m_outputs.append(m_output);
        m_output = {};
        m_files.reset();
        ++m_index;
    }
    bool verifyMedia()
    {
        const QString path = m_output.value(key("stagedPath")).toString(), leaf = m_output.value(key("fileName")).toString();
        const QFileInfo file(path);
        if (!file.isFile() || file.isSymLink() || file.size() <= 0) {
            finish(false, key("Native renderer did not produce a nonempty ordinary media file."));
            return false;
        }
        int frames = 0;
        {
            // Explicit avformat avoids an audio visualization being mistaken for video.
            Mlt::Producer rendered(m_profile, "avformat", path.toUtf8().constData());
            if (!rendered.is_valid()) {
                finish(false, key("Native rendered media cannot be opened."));
                return false;
            }
            QString reason;
            if (!McpRender::verifyStreams(rendered, m_profile, !m_output.value(key("audioOnly")).toBool(),
                                          !m_output.value(key("audioDisabled")).toBool(), reason)) {
                finish(false, reason);
                return false;
            }
            frames = rendered.get_length();
        }
        const int expected = m_output.value(key("expectedFrameCount")).toInt();
        const int tolerance = m_output.value(key("audioOnly")).toBool() ? 1 : 0;
        if (qAbs(qint64(frames) - expected) > tolerance) {
            finish(false, key("Native rendered media duration does not match the requested frame range."));
            return false;
        }
        m_bytes = double(file.size());
        m_output.insert(key("frameCount"), frames);
        if (m_directoryOutput && QFileInfo(path).fileName() != leaf) {
            const QString normalized = QDir(m_stage).absoluteFilePath(leaf);
            // Only the owned raw unembedded video may be removed inside staging.
            if ((QFileInfo::exists(normalized) && !QFile::remove(normalized)) || !QFile::rename(path, normalized)) {
                finish(false, key("Cannot finalize an owned subtitle-muxed output in staging."));
                return false;
            }
        }
        m_keep.insert(leaf);
        completeOutput();
        return true;
    }
    bool verifyImage(const QFileInfo &file)
    {
        const auto match = m_numbered.match(file.fileName());
        if (!match.hasMatch()) return true;
        bool valid = false;
        const int number = match.captured(1).toInt(&valid);
        QImageReader reader(file.absoluteFilePath());
        if (!valid || m_numbers.contains(number) || !file.isFile() || file.isSymLink() || file.size() <= 0 || !reader.canRead() || !reader.size().isValid()) {
            finish(false, key("Native image sequence contains an unreadable, duplicate, or invalid frame."));
            return false;
        }
        if (++m_count > m_output.value(key("expectedFrameCount")).toInt()) {
            finish(false, key("Native image sequence contains more frames than requested."));
            return false;
        }
        m_first = qMin(m_first, number);
        m_last = qMax(m_last, number);
        m_numbers.insert(number);
        m_keep.insert(file.fileName());
        m_bytes += double(file.size());
        return true;
    }
    void advance()
    {
        // Bounded batches yield between local headers so UI/cancel stay responsive.
        for (int budget = 0; budget < 16 && !m_stopped; ++budget) {
            if (m_index < m_planned.size()) {
                if (m_output.isEmpty() && !beginOutput()) return;
                if (m_output.value(key("kind")) != key("imageSequence")) {
                    if (!verifyMedia()) return;
                    break;
                }
                if (m_files->hasNext()) {
                    m_files->next();
                    if (!verifyImage(m_files->fileInfo())) return;
                    continue;
                }
                const int expected = m_output.value(key("expectedFrameCount")).toInt();
                if (m_count != expected || qint64(m_last) - m_first + 1 != expected) {
                    finish(false, key("Native image sequence frame count or numbering does not match the exact requested range."));
                    return;
                }
                m_output.insert(key("frameCount"), m_count);
                m_output.insert(key("firstNumber"), m_first);
                m_output.insert(key("lastNumber"), m_last);
                completeOutput();
            } else {
                if (!m_directoryOutput) {
                    finish(true);
                    return;
                }
                if (!m_files) m_files = std::make_unique<QDirIterator>(m_stage, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
                if (!m_files->hasNext()) {
                    finish(true);
                    return;
                }
                m_files->next();
                const QFileInfo file = m_files->fileInfo();
                if (file.isSymLink() || !file.isFile() || (!m_keep.contains(file.fileName()) && !QFile::remove(file.absoluteFilePath()))) {
                    finish(false, key("Cannot clean the private export staging directory before publication."));
                    return;
                }
            }
        }
        if (!m_stopped) QTimer::singleShot(0, this, [this] { advance(); });
    }
    QString m_stage, m_target;
    bool m_directoryOutput{false}, m_stopped{false};
    QJsonArray m_planned, m_outputs;
    QJsonObject m_output;
    Completion m_completion;
    Mlt::Profile m_profile;
    std::unique_ptr<QDirIterator> m_files;
    QRegularExpression m_numbered;
    QSet<QString> m_keep;
    QSet<int> m_numbers;
    int m_index{0}, m_count{0}, m_first{0}, m_last{-1};
    double m_bytes{0};
};
void stopRenderVerification(QProcess *process)
{
    if (!process) return;
    for (auto *child : process->children())
        if (auto *verifier = dynamic_cast<RenderOutputVerifier *>(child)) verifier->stop();
}
QStringList stepArgs(const QJsonObject &step)
{
    QStringList args;
    for (const auto &v : step.value(key("arguments")).toArray())
        args.append(v.toString());
    return args;
}
} // namespace
QJsonArray KdenliveMcpServer::lifecycleToolCatalog()
{
    QJsonArray result;
    const auto add = [&result](const char *name, const char *description, const QJsonObject &properties, const QJsonArray &mandatory, bool readOnly = false) {
        result.append(Mcp::tool(key(name), key(description), properties, mandatory, readOnly));
    };
    add("list_profiles", "Discover installed native project profiles with exact frame rates and dimensions.", {}, {}, true);
    add("list_render_presets", "Discover installed native export presets and their available quality/bitrate/speed choices.",
        {{key("presetName"), text("Optional exact native preset name to inspect.")}}, {}, true);
    add("project_job_status", "Inspect queued project operations, including native dialogs awaiting a decision.",
        {{key("jobId"), text("Optional returned project job ID. Omit to list project jobs.")}}, {}, true);
    add("new_project", "Queue a native empty project. Refuses a modified current document; inspect the returned job before editing.",
        withState({{key("profile"), text("Installed profile ID from list_profiles.")},
                   {key("videoTracks"), number("Number of video tracks, 1-32.")},
                   {key("audioTracks"), number("Number of audio tracks, 0-32.")}}),
        required({"profile", "videoTracks", "audioTracks"}));
    add("open_project", "Queue opening an existing local .kdenlive project without discarding unsaved edits. Native repair/lock dialogs remain explicit.",
        withState({{key("path"), text("Existing absolute local .kdenlive path.")}}), required({"path"}));
    add("save_document",
        "Queue native Save/Save As and keep its normal backup/cache decisions. A successful job updates the live document path and clean state.",
        withState({{key("path"), text("Optional absolute .kdenlive path; defaults to the live document path.")},
                   {key("overwrite"), boolean("Explicitly allow replacing an existing project file.")}}),
        required({"overwrite"}));
    add("set_project_profile", "Queue native project-profile conversion. Requires a clean document; native conversion/recovery dialogs remain explicit.",
        withState({{key("profile"), text("Installed profile ID from list_profiles.")}}), required({"profile"}));
    add("control_playback", "Seek, play, pause, or set the native project monitor zone. Seeking reports a request, not proof that a frame was displayed.",
        withState({{key("action"), text("seek, play, pause, or set_zone.")},
                   {key("frame"), number("Requested seek frame.")},
                   {key("inFrame"), number("Inclusive zone start.")},
                   {key("outFrame"), number("Inclusive zone end.")}}),
        required({"action"}));
    add("extract_preview", "Render an exact project frame from a detached native timeline snapshot to a new PNG file, without seeking the live monitor.",
        withState({{key("path"), text("New absolute .png output path.")},
                   {key("frame"), number("Exact project frame.")},
                   {key("width"), number("Optional preview width, 8-4096.")},
                   {key("height"), number("Optional preview height, 8-4096.")}}),
        required({"path", "frame"}));
    add("start_render",
        "Export a native snapshot or queue it after an owned job. Single files and complete multioutput directories publish without replacement; queues belong "
        "to this session.",
        withState(
            {{key("path"), text("New absolute file path, or new directory for image sequences, guide sections, or audio stems.")},
             {key("presetName"), text("Exact preset name from list_render_presets.")},
             {key("outputName"), text("Directory exports only: plain filename with the preset extension; defaults to export.<extension>.")},
             {key("guideSections"), boolean("Export each native guide section into a new output directory.")},
             {key("guideCategory"), number("Guide category ID, or -1 for all guides; used with guideSections.")},
             {key("audioFilePerTrack"), boolean("Native stem export: one audio file per unmuted audio track, plus video when enabled.")},
             {key("audioChannels"), number("Optional output channel count: 1, 2, 4, or 6. Defaults to preset or project channels.")},
             {key("queueAfterJobId"), text("Optional owned earlier render job. Starts only if it completes; session-only queue, canceled on editor exit.")},
             {key("inFrame"), number("Optional inclusive render start.")},
             {key("outFrame"), number("Optional inclusive render end.")},
             {key("twoPass"), boolean("Use native two-pass preparation.")},
             {key("useProxy"), boolean("Render proxies instead of originals.")},
             {key("embedSubtitles"), boolean("Embed a subtitle stream; requires MKV and ffmpeg.")},
             {key("videoQuality"), number("Optional native video-quality value, 0-100.")},
             {key("audioQuality"), number("Optional native audio-quality value, 0-100.")},
             {key("videoBitrateKbps"), number("Optional video bitrate, 1-1000000 kb/s.")},
             {key("audioBitrateKbps"), number("Optional audio bitrate, 1-10000 kb/s.")},
             {key("speedIndex"), number("Optional index into this preset's native speeds list.")}}),
        required({"path", "presetName"}));
    add("render_status", "Inspect actual render process state, progress, errors and published output.",
        {{key("jobId"), text("Optional render job ID. Omit to list render jobs.")}}, {}, true);
    add("cancel_render", "Cancel only a render process owned by this MCP session. The actual melt/ffmpeg child is stopped and staged output removed.",
        withState({{key("jobId"), text("Owned render job ID.")}}), QJsonArray{key("jobId")});
    add("media_job_status", "Inspect native per-asset loading, proxy, transcode, analysis and other background-job status.",
        {{key("binId"), text("Optional native bin clip ID. Omit to list media jobs.")}}, {}, true);
    add("cancel_media_job", "Cancel native background work for an explicit asset and job type. Query afterward to reconcile its state.",
        withState({{key("binId"), text("Native bin clip ID.")},
                   {key("jobType"), number("Native type 1-13 from media_job_status; 0 cancels all jobs for this asset.")}}),
        required({"binId", "jobType"}));
    add("set_media_proxy", "Request native proxy generation or disabling for one explicit source asset, with native undo and job reporting.",
        withState({{key("binId"), text("Native bin clip ID.")},
                   {key("enabled"), boolean("True to build/use a proxy; false to disable it.")},
                   {key("force"), boolean("Rebuild the proxy when enabled.")}}),
        required({"binId", "enabled"}));
    add("relink_media", "Relink one source asset using native undo. Refuses replacements shorter than the current native source to preserve timeline ranges.",
        withState({{key("binId"), text("Native bin clip ID.")}, {key("path"), text("Existing absolute local replacement media file.")}}),
        required({"binId", "path"}));
    return result;
}
QJsonObject KdenliveMcpServer::callLifecycleTool(const QString &name, const QJsonObject &arguments, const QJsonObject &state)
{
    using Mcp::failure;
    using Mcp::toolResult;
    if (name == key("list_profiles")) {
        QJsonArray profiles;
        for (const auto &entry : ProfileRepository::get()->getAllProfiles()) {
            const auto &profile = ProfileRepository::get()->getProfile(entry.second);
            if (!profile || !profile->is_valid()) continue;
            profiles.append(QJsonObject{{key("id"), entry.second},
                                        {key("description"), profile->description()},
                                        {key("width"), profile->width()},
                                        {key("height"), profile->height()},
                                        {key("frameRateNumerator"), profile->frame_rate_num()},
                                        {key("frameRateDenominator"), profile->frame_rate_den()},
                                        {key("progressive"), profile->progressive()},
                                        {key("sampleAspectNumerator"), profile->sample_aspect_num()},
                                        {key("sampleAspectDenominator"), profile->sample_aspect_den()},
                                        {key("colorspace"), profile->colorspace()}});
        }
        return toolResult({{key("ok"), true}, {key("profiles"), profiles}});
    }
    if (name == key("list_render_presets")) {
        const QString requested = arguments.value(key("presetName")).toString();
        if (!requested.isEmpty() && !RenderPresetRepository::get()->presetExists(requested))
            return failure(key("INVALID_PRESET"), key("Choose an installed render preset."), state);
        QJsonArray presets;
        for (const auto &id : RenderPresetRepository::get()->getAllPresets()) {
            if (!requested.isEmpty() && requested != id) continue;
            const auto &preset = RenderPresetRepository::get()->getPreset(id);
            if (!preset) continue;
            const auto strings = [](const QStringList &items) {
                QJsonArray out;
                for (const auto &s : items)
                    out.append(s);
                return out;
            };
            QJsonObject item{{key("id"), id},
                             {key("name"), preset->name()},
                             {key("extension"), preset->extension()},
                             {key("valid"), preset->isValid() && preset->error().isEmpty()},
                             {key("error"), preset->error()},
                             {key("warning"), preset->warning()},
                             {key("category"), preset->groupId()},
                             {key("imageSequence"), preset->params().isImageSequence()},
                             {key("typedExportSupported"), preset->isValid() && preset->error().isEmpty() && preset->renderer() == key("avformat")},
                             {key("audioOnly"), preset->hasParam(key("video_off")) || preset->hasParam(key("vn"))},
                             {key("videoQualities"), strings(preset->videoQualities())},
                             {key("audioQualities"), strings(preset->audioQualities())},
                             {key("videoBitrates"), strings(preset->videoBitrates())},
                             {key("audioBitrates"), strings(preset->audioBitrates())},
                             {key("speeds"), strings(preset->speeds())}};
            if (!requested.isEmpty()) {
                QJsonObject params;
                const auto native = preset->params();
                for (auto it = native.cbegin(); it != native.cend(); ++it)
                    params.insert(it.key(), it.value());
                item.insert(key("parameters"), params);
            }
            presets.append(item);
        }
        return toolResult({{key("ok"), true}, {key("presets"), presets}});
    }
    if (name == key("project_job_status") || name == key("render_status")) {
        const QString kind = name == key("render_status") ? key("render") : key("project");
        const QString id = arguments.value(key("jobId")).toString();
        if (!id.isEmpty()) {
            if (!m_lifecycleJobs.contains(id) || m_lifecycleJobs[id].value(key("kind")) != kind)
                return failure(key("INVALID_JOB"), key("Use a job ID returned by this MCP session."), state);
            return toolResult({{key("ok"), true},
                               {key("job"), publicJob(m_lifecycleJobs[id], QApplication::activeModalWidget() != nullptr || McpQuick::modalPopupActive(this))},
                               {key("state"), snapshot()}});
        }
        QJsonArray jobs;
        for (auto it = m_lifecycleJobs.cbegin(); it != m_lifecycleJobs.cend(); ++it)
            if (it.value().value(key("kind")) == kind) {
                auto item = publicJob(it.value(), QApplication::activeModalWidget() != nullptr || McpQuick::modalPopupActive(this));
                item.remove(key("logTail"));
                jobs.append(item);
            }
        return toolResult({{key("ok"), true}, {key("jobs"), jobs}, {key("state"), snapshot()}});
    }
    if (name == key("media_job_status")) {
        if (!pCore || !pCore->projectItemModel()) return failure(key("NO_PROJECT"), key("No native project is ready."));
        const QString requested = arguments.value(key("binId")).toString();
        if (!requested.isEmpty() && !pCore->projectItemModel()->getClipByBinID(requested))
            return failure(key("INVALID_BIN_CLIP"), key("Use a native source clip ID."), state);
        QJsonArray clips;
        for (const QString &id : pCore->projectItemModel()->getAllClipIds()) {
            if (!requested.isEmpty() && requested != id) continue;
            const auto clip = pCore->projectItemModel()->getClipByBinID(id);
            if (clip) clips.append(mediaJob(id, clip));
        }
        return toolResult({{key("ok"), true}, {key("clips"), clips}});
    }
    const QStringList mutations{key("new_project"),      key("open_project"),    key("save_document"), key("set_project_profile"),
                                key("control_playback"), key("extract_preview"), key("start_render"),  key("cancel_render"),
                                key("cancel_media_job"), key("set_media_proxy"), key("relink_media")};
    if (!mutations.contains(name)) return {};
    if (name == key("cancel_render")) {
        const QString id = arguments.value(key("jobId")).toString();
        if (!m_lifecycleJobs.contains(id) || m_lifecycleJobs[id].value(key("kind")) != key("render"))
            return failure(key("INVALID_JOB"), key("Use an owned render job ID."), state);
        auto &job = m_lifecycleJobs[id];
        if (terminal(job.value(key("status")).toString()))
            return toolResult({{key("ok"), true}, {key("job"), publicJob(job, false)}, {key("state"), snapshot()}});
        stopRenderVerification(m_renderProcesses.value(id));
        job.insert(key("cancelRequested"), true);
        job.insert(key("status"), key("canceling"));
        if (const auto process = m_renderProcesses.value(id); process && process->state() != QProcess::NotRunning)
            process->kill();
        else {
            job.insert(key("status"), key("canceled"));
            removeRenderArtifacts(job, true);
            if (const auto process = m_renderProcesses.take(id)) process->deleteLater();
        }
        return toolResult({{key("ok"), true}, {key("job"), publicJob(job, false)}, {key("state"), snapshot()}});
    }
    if (m_lifecycleBusy) return failure(key("PROJECT_BUSY"), key("Wait for the current project operation or resolve its native dialog."), state);
    if (QApplication::activeModalWidget() || QApplication::activePopupWidget() || McpQuick::modalPopupActive(this) ||
        QApplication::mouseButtons() != Qt::NoButton)
        return failure(key("UI_BUSY"), key("Close native dialogs/popups and finish dragging before this operation."), state);
    auto *document = pCore->projectManager()->current();
    const auto timeline = pCore->projectManager()->getTimeline();
    if (!document || !timeline || document->loading || document->closing)
        return failure(key("NO_PROJECT"), key("Wait for a native project to become ready."), state);
    if (name == key("new_project") || name == key("open_project") || name == key("save_document") || name == key("set_project_profile")) {
        if (!pCore->window()) return failure(key("GUI_REQUIRED"), key("Project lifecycle requires the running editor GUI."), state);
        const auto bin = pCore->projectItemModel();
        const auto guides = document->getGuideModel(timeline->uuid());
        const bool initialProfileSwitch = name == key("set_project_profile") && document->url().isEmpty() && timeline->getClipsCount() == 0 &&
                                          timeline->getCompositionsCount() == 0 && !timeline->hasSubtitleModel() && pCore->undoStack()->count() == 0 &&
                                          Mcp::liveSequenceClips(bin).size() == 1 && bin->getAllClipIds().size() == 1 && (!guides || guides->rowCount() == 0);
        // Native startup marks the initial empty document modified. Switching its
        // profile updates this same document and does not discard any content.
        if (name != key("save_document") && document->isModified() && !initialProfileSwitch)
            return failure(key("UNSAVED_CHANGES"), key("Save the live document or explicitly resolve its unsaved changes before replacing/converting it."),
                           state);
        const QString profile = arguments.value(key("profile")).toString();
        QString path;
        int video = 0, audio = 0;
        if (name != key("open_project") && name != key("save_document") && !ProfileRepository::get()->profileExists(profile))
            return failure(key("INVALID_PROFILE"), key("Choose a profile ID from list_profiles."), state);
        if (name == key("new_project") &&
            (!Mcp::integer(arguments, key("videoTracks"), 1, 32, video) || !Mcp::integer(arguments, key("audioTracks"), 0, 32, audio)))
            return failure(key("INVALID_ARGUMENT"), key("Use 1-32 video tracks and 0-32 audio tracks."), state);
        if (name == key("open_project") &&
            (!localPath(arguments.value(key("path")), path, true) || QFileInfo(path).suffix().compare(key("kdenlive"), Qt::CaseInsensitive) != 0 ||
             QFileInfo(path).size() > qint64(256) * 1024 * 1024))
            return failure(key("INVALID_PATH"), key("Use an existing readable local .kdenlive file up to 256 MiB."), state);
        const bool overwrite = arguments.value(key("overwrite")).toBool(false);
        if (name == key("save_document")) {
            path = arguments.contains(key("path")) ? arguments.value(key("path")).toString() : document->url().toLocalFile();
            QString checked;
            const bool exists = QFileInfo::exists(path);
            if (!localPath(QJsonValue(path), checked, exists) || QFileInfo(path).suffix().compare(key("kdenlive"), Qt::CaseInsensitive) != 0 ||
                !QFileInfo(QFileInfo(path).absolutePath()).isWritable())
                return failure(key("INVALID_PATH"), key("Choose a writable absolute .kdenlive path, or supply a path for the first save."), state);
            if (exists && !overwrite) return failure(key("OUTPUT_EXISTS"), key("Existing project requires explicit overwrite=true."), state);
        }
        if (!makeRoom(m_lifecycleJobs)) return failure(key("JOB_LIMIT"), key("Too many active native jobs."), state);
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonObject job{{key("jobId"), id}, {key("kind"), key("project")}, {key("operation"), name}, {key("status"), key("queued")}};
        m_lifecycleJobs.insert(id, job);
        m_lifecycleBusy = true;
        const QPointer<KdenliveDoc> original(document);
        const QString originalToken = state.value(key("expectedState")).toString();
        QTimer::singleShot(0, this, [this, id, name, profile, path, video, audio, original, originalToken, overwrite, initialProfileSwitch] {
            auto &record = m_lifecycleJobs[id];
            record.insert(key("status"), key("running"));
            const auto current = snapshot();
            if (!original || pCore->projectManager()->current() != original ||
                (name != key("save_document") && original->isModified() && !initialProfileSwitch) ||
                current.value(key("expectedState")).toString() != originalToken) {
                record.insert(key("status"), key("failed"));
                record.insert(key("message"), key("Project changed before queued operation; no replacement was attempted."));
                m_lifecycleBusy = false;
                return;
            }
            if (name == key("save_document") && !overwrite && QFileInfo::exists(path)) {
                record.insert(key("status"), key("failed"));
                record.insert(key("message"), key("The save target appeared before execution; no save was attempted."));
                m_lifecycleBusy = false;
                return;
            }
            bool saveSucceeded = false;
            if (name == key("save_document"))
                saveSucceeded = pCore->projectManager()->saveFileAs(path, overwrite, false, nullptr, !overwrite);
            else if (name == key("new_project")) {
                const int oldVideo = KdenliveSettings::videotracks(), oldAudio = KdenliveSettings::audiotracks();
                KdenliveSettings::setVideotracks(video);
                KdenliveSettings::setAudiotracks(audio);
                pCore->projectManager()->newFile(profile, false);
                KdenliveSettings::setVideotracks(oldVideo);
                KdenliveSettings::setAudiotracks(oldAudio);
            } else if (name == key("open_project"))
                pCore->projectManager()->openFile(QUrl::fromLocalFile(path));
            else if (original->url().isEmpty() && pCore->projectManager()->getTimeline()->getClipsCount() == 0 &&
                     pCore->projectManager()->getTimeline()->getCompositionsCount() == 0 && !pCore->projectManager()->getTimeline()->hasSubtitles()) {
                // There is no saved scene to convert. This is Kdenlive's own initial-profile switch.
                QMetaObject::invokeMethod(original.data(), "slotSwitchProfile", Qt::DirectConnection, Q_ARG(QString, profile), Q_ARG(bool, true));
            } else
                pCore->projectManager()->saveWithUpdatedProfile(profile);
            auto *opened = pCore->projectManager()->current();
            const auto active = pCore->projectManager()->getTimeline();
            bool applied = opened && active && !opened->loading && !opened->closing;
            if (applied && name == key("save_document"))
                applied =
                    saveSucceeded && !opened->isModified() && QFileInfo(opened->url().toLocalFile()).canonicalFilePath() == QFileInfo(path).canonicalFilePath();
            else if (applied && name == key("open_project"))
                applied = QFileInfo(opened->url().toLocalFile()).canonicalFilePath() == QFileInfo(path).canonicalFilePath();
            else if (applied && name == key("new_project"))
                applied = opened != original && pCore->getCurrentProfilePath() == profile && active->getClipsCount() == 0 &&
                          active->getTracksCount() == video + audio;
            else if (applied)
                applied = pCore->getCurrentProfilePath() == profile;
            record.insert(key("status"), applied ? key("completed") : key("failed"));
            if (!applied)
                record.insert(key("message"), key("Native operation did not produce the requested project/profile. Inspect current state before proceeding."));
            m_lifecycleBusy = false;
        });
        return toolResult({{key("ok"), true}, {key("job"), publicJob(job, false)}, {key("state"), state}});
    }
    if (name == key("control_playback")) {
        if (!pCore->monitorManager() || !pCore->monitorManager()->projectMonitor())
            return failure(key("GUI_REQUIRED"), key("Playback needs a native project monitor."), state);
        auto *monitor = pCore->monitorManager()->projectMonitor();
        const QString action = arguments.value(key("action")).toString();
        int frame = 0, in = 0, out = 0;
        const int last = qMax(0, timeline->duration() - 1);
        if (action == key("seek")) {
            if (!Mcp::integer(arguments, key("frame"), 0, last, frame))
                return failure(key("INVALID_ARGUMENT"), key("Choose a frame inside the current timeline."), state);
            monitor->slotSeek(frame);
        } else if (action == key("play"))
            monitor->switchPlay(true);
        else if (action == key("pause"))
            monitor->switchPlay(false);
        else if (action == key("set_zone")) {
            if (!Mcp::integer(arguments, key("inFrame"), 0, last, in) || !Mcp::integer(arguments, key("outFrame"), in, last, out))
                return failure(key("INVALID_ARGUMENT"), key("Choose an ordered inclusive zone within the current timeline."), state);
            monitor->loadZone(in, out + 1);
        } else
            return failure(key("INVALID_ARGUMENT"), key("Use seek, play, pause, or set_zone."), state);
        return toolResult({{key("ok"), true},
                           {key("position"), monitor->position()},
                           {key("playing"), monitor->isPlaying()},
                           {key("seekRequested"), action == key("seek")},
                           {key("state"), snapshot()}});
    }
    if (name == key("extract_preview")) {
        QString path;
        int frame = 0, width = pCore->getCurrentProfile()->width(), height = pCore->getCurrentProfile()->height();
        if (!localPath(arguments.value(key("path")), path, false) || QFileInfo(path).suffix().compare(key("png"), Qt::CaseInsensitive) != 0)
            return failure(key("INVALID_PATH"), key("Choose a new absolute PNG output path in a writable folder."), state);
        if (!Mcp::integer(arguments, key("frame"), 0, qMax(0, timeline->duration() - 1), frame))
            return failure(key("INVALID_ARGUMENT"), key("Choose a frame inside the timeline."), state);
        if (arguments.contains(key("width")) && !Mcp::integer(arguments, key("width"), 8, 4096, width))
            return failure(key("INVALID_ARGUMENT"), key("Preview dimensions must be 8-4096 pixels."), state);
        if (arguments.contains(key("height")) && !Mcp::integer(arguments, key("height"), 8, 4096, height))
            return failure(key("INVALID_ARGUMENT"), key("Preview dimensions must be 8-4096 pixels."), state);
        if (width < 8 || height < 8 || width > 4096 || height > 4096)
            return failure(key("INVALID_ARGUMENT"), key("Supply explicit preview dimensions within 8-4096 pixels."), state);
        const auto scene = pCore->projectManager()->projectSceneList(QFileInfo(path).absolutePath(), true);
        QByteArray xml = scene.first.toUtf8();
        if (!scene.second.isEmpty()) {
            QFile serialized(scene.second);
            if (!serialized.open(QIODevice::ReadOnly) || serialized.size() > 64 * 1024 * 1024)
                return failure(key("PREVIEW_FAILED"), key("Cannot read the bounded native timeline snapshot."), state);
            xml = serialized.readAll();
            serialized.close();
            QFile::remove(scene.second);
        }
        if (xml.isEmpty() || xml.size() > 64 * 1024 * 1024) return failure(key("PREVIEW_FAILED"), key("Native snapshot is empty or too large."), state);
        Mlt::Producer detached(pCore->getProjectProfile(), "xml-string", xml.constData());
        if (!detached.is_valid()) return failure(key("PREVIEW_FAILED"), key("Native timeline snapshot could not be opened."), state);
        const QImage image = KThumb::getFrame(detached, frame, width, height);
        if (image.isNull()) return failure(key("PREVIEW_FAILED"), key("Native renderer did not return an image."), state);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return failure(key("WRITE_FAILED"), file.errorString(), state);
        if (!image.save(&file, "PNG") || !file.flush()) {
            const QString error = file.errorString();
            file.close();
            QFile::remove(path);
            return failure(key("WRITE_FAILED"), error, state);
        }
        file.close();
        return toolResult({{key("ok"), true},
                           {key("path"), path},
                           {key("frame"), frame},
                           {key("width"), image.width()},
                           {key("height"), image.height()},
                           {key("state"), snapshot()}});
    }
    if (name == key("start_render")) {
        QString path;
        const QString presetName = arguments.value(key("presetName")).toString();
        if (!localPath(arguments.value(key("path")), path, false) || !safeOutputLeaf(QFileInfo(path).fileName()))
            return failure(key("INVALID_PATH"), key("Choose a new absolute output path in a writable directory."), state);
        if (!RenderPresetRepository::get()->presetExists(presetName))
            return failure(key("INVALID_PRESET"), key("Choose a native installed export preset."), state);
        const auto &preset = RenderPresetRepository::get()->getPreset(presetName);
        if (!preset || !preset->isValid() || !preset->error().isEmpty() || preset->renderer() != key("avformat"))
            return failure(key("INVALID_PRESET"), key("Choose an available native avformat preset without codec or format errors."), state);
        const bool imageSequence = preset->params().isImageSequence();
        const bool guides = arguments.value(key("guideSections")).toBool(false);
        const bool stems = arguments.value(key("audioFilePerTrack")).toBool(false);
        const bool directoryOutput = imageSequence || guides || stems;
        const QString outputName =
            directoryOutput ? arguments.value(key("outputName")).toString(key("export.") + preset->extension()) : QFileInfo(path).fileName();
        if (!safeOutputLeaf(outputName) || QFileInfo(outputName).suffix().compare(preset->extension(), Qt::CaseInsensitive) != 0 ||
            (!directoryOutput && arguments.contains(key("outputName"))))
            return failure(key("INVALID_PATH"), key("Use a plain output filename with the preset extension; outputName is for directory exports only."), state);
        if (!guides && arguments.contains(key("guideCategory")))
            return failure(key("INVALID_ARGUMENT"), key("guideCategory requires guideSections=true."), state);
        int guideCategory = -1;
        if (guides && arguments.contains(key("guideCategory")) && !Mcp::integer(arguments, key("guideCategory"), -1, 1000000, guideCategory))
            return failure(key("INVALID_ARGUMENT"), key("Use a guide category ID or -1."), state);
        const QString predecessor = arguments.value(key("queueAfterJobId")).toString();
        if (!predecessor.isEmpty() && (!m_lifecycleJobs.contains(predecessor) || m_lifecycleJobs[predecessor].value(key("kind")) != key("render")))
            return failure(key("INVALID_JOB"), key("queueAfterJobId must be an owned render job."), state);
        const QString melt = KdenliveSettings::meltpath();
        if (melt.isEmpty() || !QFileInfo(melt).isExecutable())
            return failure(key("RENDERER_MISSING"), key("The configured native melt renderer is unavailable."), state);
        const bool embed = arguments.value(key("embedSubtitles")).toBool(false);
        const QString ffmpeg = KdenliveSettings::ffmpegpath();
        if (embed && !document->hasSubtitles()) return failure(key("INVALID_ARGUMENT"), key("There are no subtitles to embed."), state);
        if (embed && (imageSequence || QFileInfo(outputName).suffix().compare(key("mkv"), Qt::CaseInsensitive) != 0 || ffmpeg.isEmpty() ||
                      !QFileInfo(ffmpeg).isExecutable()))
            return failure(key("INVALID_ARGUMENT"), key("Subtitle embedding requires MKV output and the configured ffmpeg executable."), state);
        const int last = timeline->duration() - 1;
        int in = 0, out = last;
        if (last < 0 || (arguments.contains(key("inFrame")) && !Mcp::integer(arguments, key("inFrame"), 0, last, in)) ||
            (arguments.contains(key("outFrame")) && !Mcp::integer(arguments, key("outFrame"), in, last, out)) || in > out)
            return failure(key("INVALID_ARGUMENT"), key("Choose an ordered inclusive range within a nonempty timeline."), state);
        const auto defaults = preset->defaultValues();
        if (defaults.size() < 5) return failure(key("INVALID_PRESET"), key("The native preset has incomplete defaults."), state);
        QString videoQuality = defaults[4], audioQuality = defaults[2], videoBitrate = defaults[3], audioBitrate = defaults[1];
        for (const auto &pair : {std::pair<const char *, QString *>("videoQuality", &videoQuality),
                                 {"audioQuality", &audioQuality},
                                 {"videoBitrateKbps", &videoBitrate},
                                 {"audioBitrateKbps", &audioBitrate}}) {
            if (!arguments.contains(key(pair.first))) continue;
            int value = 0;
            const bool bitrate = QString::fromLatin1(pair.first).contains(key("Bitrate"));
            const int maximum = key(pair.first) == key("videoBitrateKbps") ? 1000000 : (bitrate ? 10000 : 100);
            if (!Mcp::integer(arguments, key(pair.first), bitrate ? 1 : 0, maximum, value))
                return failure(key("INVALID_ARGUMENT"), key("Quality or bitrate is outside the bounded native parameter range."), state);
            *pair.second = QString::number(value);
        }
        RenderPresetParams params = preset->params();
        params.refreshX265Params();
        params.replacePlaceholder(key("%quality"), videoQuality);
        params.replacePlaceholder(key("%audioquality"), audioQuality);
        params.replacePlaceholder(key("%audiobitrate+'k'"), audioBitrate + QLatin1Char('k'));
        params.replacePlaceholder(key("%audiobitrate"), audioBitrate);
        params.replacePlaceholder(key("%bitrate+'k'"), videoBitrate + QLatin1Char('k'));
        params.replacePlaceholder(key("%bitrate"), videoBitrate);
        QString speed = defaults.first();
        if (arguments.contains(key("speedIndex"))) {
            int index = 0;
            const auto speeds = preset->speeds();
            if (!Mcp::integer(arguments, key("speedIndex"), 0, speeds.size() - 1, index))
                return failure(key("INVALID_ARGUMENT"), key("Choose an available native speed index."), state);
            speed = speeds[index];
        }
        if (speed.contains(QLatin1Char('='))) params.insertFromString(speed, false);
        int channels = params.contains(key("channels")) ? params.value(key("channels")).toInt() : pCore->audioChannels();
        if (arguments.contains(key("audioChannels")) &&
            (!Mcp::integer(arguments, key("audioChannels"), 1, 6, channels) || (channels != 1 && channels != 2 && channels != 4 && channels != 6)))
            return failure(key("INVALID_ARGUMENT"), key("Native output channels must be 1, 2, 4, or 6."), state);
        if (channels > 0) params.insert(key("channels"), QString::number(channels));
        if (imageSequence && arguments.value(key("twoPass")).toBool(false))
            return failure(key("INVALID_ARGUMENT"), key("Image sequences use a single native render pass."), state);
        if (!makeRoom(m_lifecycleJobs, predecessor)) return failure(key("JOB_LIMIT"), key("Too many active jobs."), state);
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QTemporaryDir staging(QFileInfo(path).dir().absoluteFilePath(key(".kdenlive-mcp-render-") + id + key("-XXXXXX")));
        if (!staging.isValid()) return failure(key("WRITE_FAILED"), key("Cannot create the owned render staging directory."), state);
        const QString staged = QDir(staging.path()).absoluteFilePath(outputName);
        RenderRequest request;
        request.setBounds(in, out);
        request.setOutputFile(staged);
        request.setPresetParams(params);
        request.setProxyRendering(arguments.value(key("useProxy")).toBool(false));
        request.setTwoPass(arguments.value(key("twoPass")).toBool(false));
        request.setEmbedSubtitles(embed);
        request.setAudioFilePerTrack(stems);
        request.setGuideParams(document->getGuideModel(pCore->currentTimelineId()), guides, guideCategory);
        QJsonArray sections;
        if (guides) {
            const auto bounds = request.getSectionsInOut();
            const auto labels = request.getSectionsNames();
            if (bounds.isEmpty()) return failure(key("INVALID_ARGUMENT"), key("No guides in the requested range/category; no export was queued."), state);
            if (bounds.size() > 128) return failure(key("EXPORT_LIMIT"), key("Export at most 128 guide sections per job."), state);
            for (int index = 0; index < bounds.size(); ++index)
                sections.append(
                    QJsonObject{{key("name"), labels.value(index)}, {key("inFrame"), bounds[index].first}, {key("outFrame"), bounds[index].second}});
        }
        const auto nativeJobs = request.process();
        if (nativeJobs.empty() || !request.errorMessages().isEmpty()) {
            for (const auto &job : nativeJobs) {
                QFile::remove(job.playlistPath);
                if (!job.subtitlePath.isEmpty()) QFile::remove(job.subtitlePath);
            }
            return failure(key("RENDER_PREPARE_FAILED"), request.errorMessages().join(QLatin1Char('\n')), state);
        }
        QJsonArray steps, playlists, plannedOutputs;
        QSet<QString> outputPaths;
        QString preparationError;
        for (const auto &job : nativeJobs) {
            playlists.append(job.playlistPath);
            if (!job.subtitlePath.isEmpty()) playlists.append(job.subtitlePath);
            if (!ownedLeafPath(job.outputFile, staging.path())) {
                preparationError = key("Native export generated an output outside its staging directory.");
                break;
            }
            QJsonArray args{key("-loglevel"), key("error"), key("-progress2"), QString::fromUtf8(QUrl::toPercentEncoding(job.playlistPath))};
            steps.append(QJsonObject{{key("program"), melt}, {key("arguments"), args}, {key("phase"), key("render")}});
            if (!outputPaths.contains(job.outputFile)) {
                auto output = nativeOutput(job, preparationError);
                if (!preparationError.isEmpty()) break;
                plannedOutputs.append(output);
                outputPaths.insert(job.outputFile);
            }
        }
        if (nativeJobs.size() > 512) preparationError = key("Export exceeds 512 native steps.");
        if (!preparationError.isEmpty()) {
            for (const auto &job : nativeJobs) {
                QFile::remove(job.playlistPath);
                if (!job.subtitlePath.isEmpty()) QFile::remove(job.subtitlePath);
            }
            return failure(key("RENDER_PREPARE_FAILED"), preparationError, state);
        }
        if (embed) {
            for (int index = 0; index < plannedOutputs.size(); ++index) {
                auto output = plannedOutputs[index].toObject();
                const QString subtitle = output.take(key("subtitlePath")).toString();
                if (subtitle.isEmpty()) continue;
                const QString mux = QDir(staging.path()).absoluteFilePath(key(".embedded-") + QString::number(index) + key(".mkv"));
                QJsonArray args{key("-nostdin"), key("-v"),   key("error"), key("-n"),       key("-i"),   output.value(key("stagedPath")),
                                key("-i"),       subtitle,    key("-map"),  key("0"),        key("-map"), key("1"),
                                key("-c"),       key("copy"), key("-f"),    key("matroska"), mux};
                steps.append(QJsonObject{{key("program"), ffmpeg}, {key("arguments"), args}, {key("phase"), key("subtitle_mux")}});
                output.insert(key("stagedPath"), mux);
                plannedOutputs[index] = output;
            }
        }
        QJsonObject record{{key("jobId"), id},
                           {key("kind"), key("render")},
                           {key("status"), key("queued")},
                           {key("path"), path},
                           {key("presetName"), presetName},
                           {key("inFrame"), in},
                           {key("outFrame"), out},
                           {key("progress"), 0},
                           {key("steps"), steps},
                           {key("stepIndex"), 0},
                           {key("stepCount"), steps.size()},
                           {key("outputKind"), directoryOutput ? key("directory") : key("file")},
                           {key("queueScope"), key("session")},
                           {key("queueAfterJobId"), predecessor},
                           {key("sections"), sections},
                           {key("audioChannels"), channels},
                           {key("frameRateNumerator"), pCore->getCurrentProfile()->frame_rate_num()},
                           {key("frameRateDenominator"), pCore->getCurrentProfile()->frame_rate_den()},
                           {key("stagingDir"), staging.path()},
                           {key("plannedOutputs"), plannedOutputs},
                           {key("playlists"), playlists}};
        staging.setAutoRemove(false);
        m_lifecycleJobs.insert(id, record);
        auto *process = new QProcess(this);
        m_renderProcesses.insert(id, process);
        process->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= 0x08000000; });
#endif
        connect(process, &QProcess::readyReadStandardOutput, this, [this, id, process] {
            if (!m_lifecycleJobs.contains(id)) return;
            auto &job = m_lifecycleJobs[id];
            const QString data = QString::fromLocal8Bit(process->readAllStandardOutput());
            job.insert(key("logTail"), QString(job.value(key("logTail")).toString() + data).right(8192));
            const QRegularExpression percentage(key("Percentage:\\s*(\\d+)"), QRegularExpression::CaseInsensitiveOption);
            const QString progressText = job.value(key("progressTail")).toString() + data;
            auto matches = percentage.globalMatch(progressText);
            job.insert(key("progressTail"), progressText.right(256));
            int progress = job.value(key("progress")).toInt();
            while (matches.hasNext()) {
                const auto match = matches.next();
                const int value = qBound(0, match.captured(1).toInt(), 99);
                const int count = job.value(key("steps")).toArray().size();
                progress = qMax(progress, (job.value(key("stepIndex")).toInt() * 100 + value) / qMax(1, count));
            }
            job.insert(key("progress"), progress);
        });
        connect(process, &QProcess::started, this, [this, id] { m_lifecycleJobs[id].insert(key("status"), key("running")); });
        connect(process, &QProcess::errorOccurred, this, [this, id, process](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || !m_lifecycleJobs.contains(id)) return;
            auto &job = m_lifecycleJobs[id];
            job.insert(key("status"), job.value(key("cancelRequested")).toBool() ? key("canceled") : key("failed"));
            job.insert(key("message"), process->errorString());
            removeRenderArtifacts(job, true);
            m_renderProcesses.remove(id);
            process->deleteLater();
        });
        connect(process, &QProcess::finished, this, [this, id, process](int exitCode, QProcess::ExitStatus exitStatus) {
            if (!m_lifecycleJobs.contains(id)) return;
            auto &job = m_lifecycleJobs[id];
            job.insert(key("logTail"), QString(job.value(key("logTail")).toString() + QString::fromLocal8Bit(process->readAllStandardOutput())).right(8192));
            job.insert(key("exitCode"), exitCode);
            if (job.value(key("cancelRequested")).toBool()) {
                job.insert(key("status"), key("canceled"));
                removeRenderArtifacts(job, true);
            } else if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                job.insert(key("status"), key("failed"));
                job.insert(key("message"), key("Native render process failed. Inspect logTail."));
                removeRenderArtifacts(job, true);
            } else {
                const auto nativeSteps = job.value(key("steps")).toArray();
                const int next = job.value(key("stepIndex")).toInt() + 1;
                if (next < nativeSteps.size()) {
                    job.insert(key("stepIndex"), next);
                    job.insert(key("progressTail"), QString());
                    job.insert(key("progress"), qMax(job.value(key("progress")).toInt(), next * 100 / int(nativeSteps.size())));
                    const auto step = nativeSteps[next].toObject();
                    job.insert(key("phase"), step.value(key("phase")));
                    process->start(step.value(key("program")).toString(), stepArgs(step));
                    return;
                }
                job.insert(key("status"), key("verifying"));
                auto *verifier = new RenderOutputVerifier(
                    job,
                    [this, id, process](bool verified, const QJsonArray &outputs, const QString &message) {
                        if (!m_lifecycleJobs.contains(id) || m_lifecycleJobs[id].value(key("cancelRequested")).toBool()) return;
                        auto &completed = m_lifecycleJobs[id];
                        if (!verified) {
                            completed.insert(key("status"), key("failed"));
                            completed.insert(key("message"), message);
                            completed.insert(key("recoveryPath"), completed.value(key("stagingDir")));
                            removeRenderArtifacts(completed, false);
                        } else {
                            const QString target = completed.value(key("path")).toString();
                            const QString stage = completed.value(key("stagingDir")).toString();
                            const bool directoryOutput = completed.value(key("outputKind")) == key("directory");
                            const QString publish =
                                directoryOutput ? stage
                                                : completed.value(key("plannedOutputs")).toArray().first().toObject().value(key("stagedPath")).toString();
                            const bool published = directoryOutput ? publishDirectory(publish, target) : QFile::rename(publish, target);
                            if (published) {
                                completed.insert(key("status"), key("completed"));
                                completed.insert(key("progress"), 100);
                                completed.insert(key("outputs"), outputs);
                                double bytes = 0;
                                for (const auto &output : outputs)
                                    bytes += output.toObject().value(key("bytes")).toDouble();
                                completed.insert(key("bytes"), bytes);
                                removeRenderArtifacts(completed, !directoryOutput);
                            } else {
                                completed.insert(key("status"), key("failed"));
                                completed.insert(key("message"),
                                                 key("Rendering verified, but non-replacing publication failed. Completed staged outputs are retained."));
                                completed.insert(key("recoveryPath"), publish);
                                removeRenderArtifacts(completed, false);
                            }
                        }
                        m_renderProcesses.remove(id);
                        process->deleteLater();
                    },
                    process);
                verifier->start();
                return;
            }
            m_renderProcesses.remove(id);
            process->deleteLater();
        });
        const auto launch = [this, id, process] {
            const auto first = m_lifecycleJobs[id].value(key("steps")).toArray().first().toObject();
            m_lifecycleJobs[id].insert(key("phase"), first.value(key("phase")));
            process->start(first.value(key("program")).toString(), stepArgs(first));
        };
        if (predecessor.isEmpty())
            launch();
        else {
            auto *queueTimer = new QTimer(process);
            queueTimer->setInterval(20);
            connect(queueTimer, &QTimer::timeout, this, [this, id, predecessor, process, queueTimer, launch] {
                if (!m_lifecycleJobs.contains(id) || terminal(m_lifecycleJobs[id].value(key("status")).toString())) {
                    queueTimer->stop();
                    return;
                }
                const QString priorStatus = m_lifecycleJobs.value(predecessor).value(key("status")).toString();
                if (priorStatus == key("completed")) {
                    queueTimer->stop();
                    launch();
                } else if (priorStatus.isEmpty() || terminal(priorStatus)) {
                    queueTimer->stop();
                    auto &job = m_lifecycleJobs[id];
                    job.insert(key("status"), key("canceled"));
                    job.insert(key("message"), key("The preceding render did not complete; dependent export was canceled."));
                    removeRenderArtifacts(job, true);
                    m_renderProcesses.remove(id);
                    process->deleteLater();
                }
            });
            queueTimer->start();
        }
        return toolResult({{key("ok"), true}, {key("job"), publicJob(m_lifecycleJobs[id], false)}, {key("state"), snapshot()}});
    }
    if (name == key("cancel_media_job") || name == key("set_media_proxy") || name == key("relink_media")) {
        const QString id = arguments.value(key("binId")).toString();
        const auto clip = pCore->projectItemModel()->getClipByBinID(id);
        if (!clip || clip->clipType() == ClipType::Timeline) return failure(key("INVALID_BIN_CLIP"), key("Use a native media source clip ID."), state);
        const ObjectId owner(KdenliveObjectType::BinClip, id.toInt(), QUuid());
        if (name == key("cancel_media_job")) {
            int type = 0;
            if (!Mcp::integer(arguments, key("jobType"), 0, AbstractTask::MELTJOB, type))
                return failure(key("INVALID_ARGUMENT"), key("Choose a reported native job type."), state);
            const bool requested = pCore->taskManager.requestCancellation(owner, static_cast<AbstractTask::JOBTYPE>(type));
            return toolResult({{key("ok"), true},
                               {key("cancellationRequested"), requested},
                               {key("allJobsFinished"), !pCore->taskManager.hasPendingJob(owner)},
                               {key("job"), mediaJob(id, clip)},
                               {key("state"), snapshot()}});
        }
        if (pCore->taskManager.hasPendingJob(owner, AbstractTask::LOADJOB) || clip->clipStatus() == FileStatus::StatusWaiting)
            return failure(key("MEDIA_NOT_READY"), key("Wait for this source to finish loading."), state);
        if (name == key("set_media_proxy")) {
            if (!clip->statusReady() || !clip->supportsProxy()) return failure(key("UNSUPPORTED_MEDIA"), key("This source cannot use a native proxy."), state);
            if (!pCore->window()) return failure(key("GUI_REQUIRED"), key("Proxy generation requires the native editor GUI."), state);
            document->slotProxyCurrentItem(arguments.value(key("enabled")).toBool(), {clip}, arguments.value(key("force")).toBool(false));
            return toolResult({{key("ok"), true}, {key("accepted"), true}, {key("job"), mediaJob(id, clip)}, {key("state"), snapshot()}});
        }
        QString path;
        if (!localPath(arguments.value(key("path")), path, true))
            return failure(key("INVALID_PATH"), key("Choose an existing readable absolute local media file."), state);
        const QStringList extensions{key("png"), key("jpg"), key("jpeg"), key("webp"), key("bmp"), key("gif"), key("tif"),  key("tiff"), key("wav"),
                                     key("mp3"), key("m4a"), key("aac"),  key("mp4"),  key("mov"), key("mkv"), key("webm"), key("ogg"),  key("flac")};
        if (!extensions.contains(QFileInfo(path).suffix().toLower()))
            return failure(key("UNSUPPORTED_MEDIA"), key("Use an ordinary supported image/audio/video replacement."), state);
        Mlt::Producer replacement(pCore->getProjectProfile(), path.toUtf8().constData());
        if (!replacement.is_valid()) return failure(key("INVALID_MEDIA"), key("Native decoder could not open the replacement."), state);
        if (clip->hasLimitedDuration() && replacement.get_length() < clip->frameDuration())
            return failure(key("SOURCE_TOO_SHORT"), key("Replacement would shorten the source and risk existing clip ranges; use a sufficiently long source."),
                           state);
        pCore->bin()->replaceSingleClip(id, path);
        if (m_imports.contains(id)) m_imports[id].path = path;
        return toolResult({{key("ok"), true}, {key("accepted"), true}, {key("job"), mediaJob(id, clip)}, {key("state"), snapshot()}});
    }
    return {};
}
void KdenliveMcpServer::lifecycleCleanup()
{
    for (auto it = m_renderProcesses.begin(); it != m_renderProcesses.end(); ++it) {
        if (const auto process = it.value()) {
            stopRenderVerification(process);
            disconnect(process, nullptr, this, nullptr);
            if (process->state() != QProcess::NotRunning) {
                process->kill();
                process->waitForFinished(1000);
            }
        }
        if (m_lifecycleJobs.contains(it.key())) removeRenderArtifacts(m_lifecycleJobs[it.key()], true);
    }
    m_renderProcesses.clear();
}
