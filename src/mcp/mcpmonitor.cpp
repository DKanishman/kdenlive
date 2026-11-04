/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcpmonitor.h"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "doc/kthumb.h"
#include "mainwindow.h"
#include "mcphelpers.h"
#include "mcpmedia.h"
#include "mcpquick.h"
#include "monitor/monitor.h"
#include "monitor/monitormanager.h"
#include "monitor/monitorproxy.h"
#include "monitor/videowidget.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/view/timelinewidget.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <mlt++/MltFrame.h>
#include <mlt++/MltProducer.h>
namespace {
constexpr int MaxFrame = 10000000;
QString key(const char *value)
{
    return QString::fromLatin1(value);
}
QString digest(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
QJsonObject typed(const char *type, const char *description)
{
    return Mcp::property(key(type), key(description));
}
bool boundedString(const QJsonValue &value, QString &out, int maximum = 4096)
{
    if (!value.isString()) return false;
    out = value.toString();
    return !out.isEmpty() && out.size() <= maximum && !out.contains(QChar::Null);
}
QString sourceHash(const std::shared_ptr<ProjectClip> &source)
{
    if (!source || !source->statusReady()) return {};
    QDomDocument document;
    source->getProducerXML(document, false, true);
    // Native monitor audio-level filters and cursor/thumbnail preferences must
    // not turn a source selection or a completed preview into a false edit.
    auto filters = document.elementsByTagName(QStringLiteral("filter"));
    for (int i = filters.size() - 1; i >= 0; --i) {
        const auto filter = filters.at(i).toElement();
        bool internal = false;
        QString service;
        bool nativeEffect = false;
        const auto props = filter.elementsByTagName(QStringLiteral("property"));
        for (int j = 0; j < props.size(); ++j) {
            const auto property = props.at(j).toElement();
            const auto name = property.attribute(QStringLiteral("name"));
            if (name == QStringLiteral("internal_added")) internal = true;
            if (name == QStringLiteral("mlt_service")) service = property.text();
            if (name == QStringLiteral("kdenlive_id")) nativeEffect = true;
        }
        if (internal || (service == QStringLiteral("audiolevel") && !nativeEffect)) filter.parentNode().removeChild(filter);
    }
    auto properties = document.elementsByTagName(QStringLiteral("property"));
    for (int i = properties.size() - 1; i >= 0; --i) {
        const auto property = properties.at(i).toElement();
        const QString name = property.attribute(QStringLiteral("name"));
        if (name.startsWith(QLatin1Char('_')) || name.startsWith(QStringLiteral("set._kdenlive_monitor_")) || name.startsWith(QStringLiteral("meta.")) ||
            name == QStringLiteral("kdenlive:monitorPosition") || name.startsWith(QStringLiteral("kdenlive:thumbZoom")) ||
            name == QStringLiteral("kdenlive:waveform") || name.startsWith(QStringLiteral("kdenlive:audio_max")))
            property.parentNode().removeChild(property);
    }
    const QByteArray serialized = document.toByteArray(-1);
    if (serialized.isEmpty() || serialized.size() > 64 * 1024 * 1024) return {};
    const QFileInfo file(source->url());
    const QJsonObject state{{key("source"), McpMedia::sourceState(source)},
                            {key("producer"), digest(serialized)},
                            {key("nativeFileHash"), source->getProducerProperty(QStringLiteral("kdenlive:file_hash"))},
                            {key("fileBytes"), QString::number(file.size())},
                            {key("fileModifiedMs"), QString::number(file.lastModified().toMSecsSinceEpoch())}};
    return digest(QJsonDocument(state).toJson(QJsonDocument::Compact));
}
bool newPng(const QJsonValue &value, QString &path)
{
    if (!boundedString(value, path) || !QDir::isAbsolutePath(path)) return false;
    path = QDir::fromNativeSeparators(path);
    const QFileInfo info(path);
    const QString leaf = info.fileName();
    const QString parent = info.absoluteDir().canonicalPath();
    if (path.startsWith(QStringLiteral("//")) || parent.isEmpty() || !QFileInfo(parent).isWritable() || info.exists() || info.isSymLink() ||
        info.suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0 || leaf.endsWith(QLatin1Char('.')) || leaf.endsWith(QLatin1Char(' ')) ||
        QRegularExpression(QStringLiteral("[<>:\\x00-\\x1f|?*]")).match(leaf).hasMatch())
        return false;
    path = QDir(parent).absoluteFilePath(leaf);
    return true;
}
struct MonitorContext
{
    QPointer<Monitor> monitor;
    QPointer<MonitorProxy> proxy;
    QPointer<VideoWidget> video;
    QVector<QMetaObject::Connection> connections;
    quint64 frameSerial{0}, seekGeneration{0};
    int displayedFrame{-1}, lastRequestedFrame{-1};
    QString originalFrameId, originalFrameResource;
    QString frameBinId, frameGeneration, frameRequestId, frameSourceHash;
};
int maximumFrame(const QString &name, const MonitorContext &context)
{
    if (name == key("source")) {
        const auto source = context.monitor ? context.monitor->currentController() : nullptr;
        if (!source || !source->statusReady()) return 0;
        return qBound(0, qMin(source->getFramePlaytime() - 1, context.video ? context.video->duration() : 0), MaxFrame);
    }
    const auto timeline = pCore->projectManager()->getTimeline();
    return qBound(0, timeline ? timeline->duration() - 1 : 0, MaxFrame);
}
class MonitorBridge : public QObject
{
public:
    explicit MonitorBridge(QObject *owner)
        : QObject(owner)
    {
        setObjectName(QStringLiteral("kdenlive-mcp-monitors"));
        timer.setParent(this);
        timer.setInterval(40);
        QObject::connect(&timer, &QTimer::timeout, this, [this] { poll(); });
    }
    QHash<QString, MonitorContext> contexts;
    QHash<QString, QJsonObject> jobs;
    QHash<QString, QPointer<KdenliveDoc>> documents;
    QHash<QString, std::weak_ptr<ProjectClip>> sources;
    QHash<QString, qint64> deadlines;
    QHash<QString, quint64> baselineSerial, issuedSeek;
    std::function<QJsonObject()> stateReader;
    QTimer timer;
    QJsonObject state() const { return stateReader ? stateReader() : QJsonObject{}; }
    void attach(const QString &name, Monitor *monitor)
    {
        auto &context = contexts[name];
        if (context.monitor == monitor && (!monitor || context.proxy == monitor->getControllerProxy())) return;
        for (const auto &connection : std::as_const(context.connections))
            QObject::disconnect(connection);
        const quint64 generation = context.seekGeneration + 1;
        context = MonitorContext{};
        context.seekGeneration = generation;
        context.monitor = monitor;
        if (!monitor) return;
        context.proxy = monitor->getControllerProxy();
        if (!context.proxy) return;
        context.video = qobject_cast<VideoWidget *>(context.proxy->parent());
        const QPointer<Monitor> watched = monitor;
        context.lastRequestedFrame = context.proxy->getPosition();
        context.connections << QObject::connect(context.proxy, &MonitorProxy::requestSeek, this, [this, name, watched](int position, bool) {
            auto &current = contexts[name];
            if (current.monitor != watched) return;
            current.lastRequestedFrame = position;
            ++current.seekGeneration;
        });
        if (context.video)
            context.connections << QObject::connect(
                context.video, &VideoWidget::frameDisplayed, this,
                [this, name, watched](const SharedFrame &frame) {
                    auto &current = contexts[name];
                    if (current.monitor != watched || !frame.is_valid()) return;
                    current.displayedFrame = frame.get_position();
                    ++current.frameSerial;
                    current.frameBinId = QString::fromUtf8(frame.get("_kdenlive_monitor_bin_id"));
                    current.frameGeneration = QString::fromUtf8(frame.get("_kdenlive_monitor_generation"));
                    current.frameRequestId = QString::fromUtf8(frame.get("_kdenlive_monitor_request"));
                    current.frameSourceHash = QString::fromUtf8(frame.get("_kdenlive_monitor_source_hash"));
                    auto copy = frame.clone();
                    std::unique_ptr<Mlt::Producer> original(copy.get_original_producer());
                    current.originalFrameId.clear();
                    current.originalFrameResource.clear();
                    if (original && original->is_valid()) {
                        current.originalFrameId = QString::fromUtf8(original->get("kdenlive:id"));
                        if (current.originalFrameId.isEmpty()) current.originalFrameId = QString::fromUtf8(original->parent().get("kdenlive:id"));
                        current.originalFrameResource = QString::fromUtf8(original->get("resource"));
                    }
                },
                Qt::QueuedConnection);
    }
    bool attachMonitors()
    {
        auto *manager = pCore ? pCore->monitorManager() : nullptr;
        attach(key("source"), manager ? manager->clipMonitor() : nullptr);
        attach(key("project"), manager ? manager->projectMonitor() : nullptr);
        return contexts.value(key("source")).monitor && contexts.value(key("project")).monitor;
    }
    QJsonObject monitorState(const QString &name) const
    {
        const auto context = contexts.value(name);
        const auto monitor = context.monitor;
        const auto proxy = context.proxy;
        if (!monitor || !proxy || !context.video) return {{key("available"), false}};
        const auto controller = name == key("source") ? monitor->currentController() : std::shared_ptr<ProjectClip>{};
        const QString id = controller ? controller->clipId() : QString();
        const QString hash = sourceHash(controller);
        auto *installed = context.video->producer();
        const QString generation = installed ? QString::fromUtf8(installed->get("set._kdenlive_monitor_generation")) : QString();
        const bool identityVerified = !id.isEmpty() && context.frameBinId == id && !generation.isEmpty() && context.frameGeneration == generation;
        const bool sourceVerified =
            identityVerified && !context.frameRequestId.isEmpty() && !context.frameSourceHash.isEmpty() && context.frameSourceHash == hash;
        return {{key("available"), true},
                {key("active"), monitor->isActive()},
                {key("maskSceneActive"), monitor->effectSceneDisplayed(SceneType::MonitorSceneAutoMask)},
                {key("playing"), monitor->isPlaying()},
                {key("selectedBinId"), id},
                {key("selectedReady"), controller ? controller->statusReady() : pCore->currentDoc() != nullptr},
                {key("sourceHash"), hash},
                {key("producerGeneration"), generation},
                {key("requestedFrame"), proxy->getPosition()},
                {key("displayedFrame"), context.displayedFrame},
                {key("seekFinished"), proxy->property("seekFinished").toBool()},
                {key("frameObserved"), context.frameSerial > 0},
                {key("displayedClipIdentityVerified"), identityVerified},
                {key("displayedSourceVerified"), sourceVerified},
                {key("sourceVerificationMethod"), key("native_frame_bin_build_request_and_source_hash")},
                {key("displayedBinId"), context.frameBinId},
                {key("displayedProducerGeneration"), context.frameGeneration},
                {key("displayedRequestId"), context.frameRequestId},
                {key("displayedSourceHash"), context.frameSourceHash},
                {key("displayedOriginalBinId"), context.originalFrameId},
                {key("displayedOriginalResource"), context.originalFrameResource},
                {key("seekGeneration"), QString::number(context.seekGeneration)},
                {key("lastRequestedFrame"), context.lastRequestedFrame},
                {key("zone"), QJsonObject{{key("inFrame"), proxy->zoneIn()}, {key("outFrameExclusive"), proxy->zoneOut()}}},
                {key("maximumFrame"), maximumFrame(name, context)},
                {key("consumerReady"), context.video->isReady()}};
    }
    QString token() const
    {
        QJsonObject values;
        values.insert(key("document"), pCore->currentDoc() ? pCore->currentDoc()->uuid().toString() : QString());
        values.insert(key("sequence"), pCore->currentTimelineId().toString());
        for (const auto &name : {key("source"), key("project")}) {
            auto value = monitorState(name);
            for (const auto &field :
                 {key("displayedFrame"), key("seekFinished"), key("frameObserved"), key("displayedSourceVerified"), key("displayedClipIdentityVerified"),
                  key("displayedOriginalBinId"), key("displayedOriginalResource"), key("displayedBinId"), key("displayedProducerGeneration"),
                  key("displayedRequestId"), key("displayedSourceHash"), key("consumerReady")})
                value.remove(field);
            // Playback naturally advances. New seeks still change seekGeneration.
            if (value.value(key("playing")).toBool()) value.remove(key("requestedFrame"));
            values.insert(name, value);
        }
        return digest(QJsonDocument(values).toJson(QJsonDocument::Compact));
    }
    QJsonObject readback()
    {
        attachMonitors();
        return {{key("expectedMonitor"), token()}, {key("source"), monitorState(key("source"))}, {key("project"), monitorState(key("project"))}};
    }
    bool room()
    {
        if (jobs.size() < 64) return true;
        for (auto it = jobs.begin(); it != jobs.end(); ++it)
            if (it.value().value(key("finished")).toBool()) {
                const QString id = it.key();
                jobs.erase(it);
                documents.remove(id);
                sources.remove(id);
                deadlines.remove(id);
                baselineSerial.remove(id);
                issuedSeek.remove(id);
                return true;
            }
        return false;
    }
    bool busy(const QString &monitor) const
    {
        for (const auto &job : jobs)
            if (job.value(key("monitor")) == monitor && !job.value(key("finished")).toBool()) return true;
        return false;
    }
    void terminal(const QString &id, const QString &status, const QString &error = {})
    {
        auto job = jobs.value(id);
        job.insert(key("status"), status);
        job.insert(key("finished"), true);
        if (!error.isEmpty()) job.insert(key("error"), error);
        job.insert(key("completedMs"), QString::number(QDateTime::currentMSecsSinceEpoch()));
        jobs[id] = job;
    }
    bool sameContext(const QString &id) const
    {
        const auto job = jobs.value(id);
        const auto document = documents.value(id);
        if (!document || pCore->currentDoc() != document || pCore->currentTimelineId().toString() != job.value(key("sequenceId")).toString()) return false;
        const auto context = contexts.value(job.value(key("monitor")).toString());
        if (!context.monitor || !context.proxy || !context.video) return false;
        if (job.value(key("monitor")) == key("source")) {
            const auto source = sources.value(id).lock();
            return source && source->statusReady() && context.monitor->currentController() == source &&
                   sourceHash(source) == job.value(key("sourceHash")).toString();
        }
        return true;
    }
    void poll()
    {
        attachMonitors();
        bool pending = false;
        for (const auto &id : jobs.keys()) {
            auto job = jobs.value(id);
            if (job.value(key("finished")).toBool()) continue;
            pending = true;
            if (job.value(key("status")) == key("queued")) continue;
            if (job.value(key("executing")).toBool()) {
                if (QApplication::activeModalWidget()) {
                    job.insert(key("status"), key("needsUserAction"));
                    jobs[id] = job;
                }
                continue;
            }
            if (!sameContext(id)) {
                terminal(id, key("interrupted"), key("The native project, source or monitor changed before completion."));
                continue;
            }
            const auto context = contexts.value(job.value(key("monitor")).toString());
            const int target = job.value(key("requestedFrame")).toInt(-1);
            if (context.seekGeneration > issuedSeek.value(id) && context.lastRequestedFrame != target) {
                terminal(id, key("interrupted"), key("A newer native seek superseded this request."));
                continue;
            }
            const bool frame = context.frameSerial > baselineSerial.value(id) && context.displayedFrame == target &&
                               context.proxy->property("seekFinished").toBool() && context.proxy->getPosition() == target;
            auto *installed = context.video->producer();
            const QString generation = installed ? QString::fromUtf8(installed->get("set._kdenlive_monitor_generation")) : QString();
            if (generation.isEmpty() || generation != job.value(key("producerGeneration")).toString()) {
                terminal(id, key("interrupted"), key("The native monitor producer was replaced before frame completion."));
                continue;
            }
            bool provenance = context.frameRequestId == id && context.frameGeneration == generation;
            const bool sourceMonitor = job.value(key("monitor")) == key("source");
            if (sourceMonitor)
                provenance = provenance && context.frameBinId == job.value(key("binId")).toString() &&
                             context.frameSourceHash == job.value(key("sourceHash")).toString();
            if (frame && provenance) {
                job.insert(key("completedFrame"), context.displayedFrame);
                job.insert(key("displayedSourceVerified"), sourceMonitor);
                job.insert(key("displayedRequestId"), context.frameRequestId);
                job.insert(key("displayedSourceHash"), context.frameSourceHash);
                job.insert(key("completionEvidence"), key("native_seek_and_displayed_frame_request_provenance"));
                jobs[id] = job;
                terminal(id, key("completed"));
            } else if (QDateTime::currentMSecsSinceEpoch() >= deadlines.value(id)) {
                terminal(id, key("failed"), key("Native monitor did not confirm the requested frame before the bounded deadline."));
            }
        }
        if (!pending) timer.stop();
    }
};
MonitorBridge *bridge(QObject *owner)
{
    for (QObject *child : owner->children())
        if (auto *existing = dynamic_cast<MonitorBridge *>(child)) return existing;
    return new MonitorBridge(owner);
}
} // namespace
namespace McpMonitor {

QString interactionFingerprint(QObject *owner)
{
    if (!owner || !qApp || !pCore || !pCore->monitorManager() || QThread::currentThread() != qApp->thread() || owner->thread() != qApp->thread()) return {};
    // This reuses only native monitor reads and private observation connections.
    // It never calls stateReader(), uiSnapshot(), or the Quick snapshot bridge.
    auto *service = bridge(owner);
    service->attachMonitors();
    return service->token();
}

QJsonArray toolCatalog()
{
    QJsonArray tools;
    tools.append(Mcp::tool(key("get_monitors"),
                           key("Read source/project native monitor identity, requested and displayed frames, seek completion, half-open zones and playback. "
                               "expectedMonitor excludes natural playback drift."),
                           {}, {}, true));
    tools.append(Mcp::tool(key("monitor_job_status"),
                           key("Read queued native monitor operation completion; needsUserAction leaves the native dialog reachable through get_ui."),
                           {{key("jobId"), typed("string", "Optional owned monitor job ID; omit to list.")}}, {}, true));
    tools.append(Mcp::tool(key("control_monitor"),
                           key("Queue a fixed native source/project monitor operation. Returns accepted, then poll monitor_job_status; a requested seek is not "
                               "a completed displayed frame."),
                           {{key("expectedState"), typed("string", "Fresh native project state token.")},
                            {key("expectedMonitor"), typed("string", "Fresh expectedMonitor token from get_monitors.")},
                            {key("monitor"), typed("string", "source or project.")},
                            {key("operation"), typed("string", "activate, open_bin_clip, seek, step, play, pause, play_zone, loop_zone, set_zone, set_in, "
                                                               "set_out, start, end, zone_start, zone_end, next_snap, previous_snap.")},
                            {key("binId"), typed("string", "Required for source open_bin_clip.")},
                            {key("frame"), typed("integer", "Required for seek; optional exact starting frame when opening source.")},
                            {key("delta"), typed("integer", "Required nonzero frame delta for step.")},
                            {key("inFrame"), typed("integer", "Required inclusive in for set_zone; optional paired source-open zone.")},
                            {key("outFrameExclusive"), typed("integer", "Required exclusive out for set_zone; optional paired source-open zone.")}},
                           {key("expectedState"), key("expectedMonitor"), key("monitor"), key("operation")}));
    tools.append(Mcp::tool(key("preview_source_frame"),
                           key("Decode an explicit ready native bin source frame using a detached producer and save a NEW PNG. Live monitor selection, "
                               "position and playback are untouched. sourceHash records native source state and file metadata."),
                           {{key("expectedState"), typed("string", "Fresh native project state token.")},
                            {key("binId"), typed("string", "Ready native bin source ID.")},
                            {key("frame"), typed("integer", "Exact source frame in project time base.")},
                            {key("path"), typed("string", "Absolute NEW local PNG output path.")},
                            {key("width"), typed("integer", "Optional output width, 8-4096.")},
                            {key("height"), typed("integer", "Optional output height, 8-4096.")},
                            {key("expectedSourceHash"), typed("string", "Optional sourceHash from prior source/monitor readback.")}},
                           {key("expectedState"), key("binId"), key("frame"), key("path")}));
    return tools;
}
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState,
                     const std::function<QJsonObject()> &stateReader)
{
    bool known = false;
    for (const auto &entry : toolCatalog())
        if (entry.toObject().value(key("name")) == name) {
            known = true;
            break;
        }
    if (!known) return {};
    auto *service = bridge(owner);
    service->stateReader = stateReader;
    auto reject = [&](const char *code, const QString &message) { return Mcp::failure(key(code), message, nativeState); };
    auto finish = [&](QJsonObject data) {
        data.insert(key("ok"), true);
        data.insert(key("state"), service->state());
        return Mcp::toolResult(data);
    };
    if (name == key("monitor_job_status")) {
        service->poll();
        if (arguments.contains(key("jobId"))) {
            QString id;
            if (!boundedString(arguments.value(key("jobId")), id, 128) || !service->jobs.contains(id))
                return reject("INVALID_JOB", key("Unknown owned monitor job."));
            return finish({{key("job"), service->jobs.value(id)}, {key("monitors"), service->readback()}});
        }
        QJsonArray jobs;
        auto ids = service->jobs.keys();
        std::sort(ids.begin(), ids.end());
        for (const auto &id : ids)
            jobs.append(service->jobs.value(id));
        return finish({{key("jobs"), jobs}, {key("monitors"), service->readback()}});
    }
    if (name == key("get_monitors")) {
        if (!service->attachMonitors()) return reject("GUI_REQUIRED", key("Source/project monitor controls require the running native editor window."));
        return finish({{key("monitors"), service->readback()}});
    }
    const QString expected = arguments.value(key("expectedState")).toString();
    if (expected.isEmpty() || expected != nativeState.value(key("expectedState")).toString())
        return reject("STALE_STATE", key("Refresh get_state before native source or monitor operations."));
    if (!nativeState.value(key("ready")).toBool() || !pCore->currentDoc() || !pCore->projectManager()->getTimeline())
        return reject("NOT_READY", key("Open a native project before source or monitor operations."));
    if (name == key("preview_source_frame")) {
        const QString binId = arguments.value(key("binId")).toString();
        const auto source = pCore->projectItemModel()->hasClip(binId) ? pCore->projectItemModel()->getClipByBinID(binId) : nullptr;
        if (!source || !source->statusReady()) return reject("MEDIA_NOT_READY", key("Use a ready native bin source."));
        int frame = -1, width = 320, height = 180;
        QString path;
        if (!Mcp::integer(arguments, key("frame"), 0, qMax(0, source->getFramePlaytime() - 1), frame) ||
            (arguments.contains(key("width")) && !Mcp::integer(arguments, key("width"), 8, 4096, width)) ||
            (arguments.contains(key("height")) && !Mcp::integer(arguments, key("height"), 8, 4096, height)) || !newPng(arguments.value(key("path")), path))
            return reject("INVALID_ARGUMENT", key("Choose an exact valid source frame, bounded dimensions and a NEW local PNG path."));
        if (source->getProducerIntProperty(key("_wait_for_transcode")) != 0 || source->getProducerIntProperty(key("meta.media.variable_frame_rate")) != 0)
            return reject("TRANSCODE_REQUIRED", key("Convert variable-frame-rate/non-seekable sources before an exact detached frame preview."));
        const QPointer<KdenliveDoc> sourceDocument = pCore->currentDoc();
        const auto sourceSequence = pCore->currentTimelineId();
        const QString hash = sourceHash(source);
        if (hash.isEmpty()) return reject("LIMIT", key("Native source serialization is unavailable or exceeds the 64 MiB context limit."));
        if (arguments.contains(key("expectedSourceHash")) && arguments.value(key("expectedSourceHash")).toString() != hash)
            return reject("STALE_SOURCE", key("Native source state or file metadata changed; refresh source readback."));
        const auto original = source->originalProducer();
        if (!original || !original->is_valid()) return reject("MEDIA_NOT_READY", key("Native source producer is unavailable."));
        const int originalPosition = original->position();
        const auto detached = source->cloneProducer(false);
        if (!detached || !detached->is_valid()) return reject("PREVIEW_FAILED", key("Native source clone is invalid."));
        detached->set_speed(0);
        detached->seek(frame);
        std::unique_ptr<Mlt::Frame> decoded(detached->get_frame());
        if (!decoded || !decoded->is_valid() || decoded->get_position() != frame)
            return reject("PREVIEW_FAILED", key("Detached native decoder did not produce the exact requested frame."));
        const QImage image = KThumb::getFrame(decoded.get(), width, height);
        if (image.isNull() || image.width() != width || image.height() != height)
            return reject("PREVIEW_FAILED", key("Native decoder returned an invalid or incorrectly sized image."));
        if (!sourceDocument || pCore->currentDoc() != sourceDocument || pCore->currentTimelineId() != sourceSequence ||
            !pCore->projectItemModel()->hasClip(binId) || pCore->projectItemModel()->getClipByBinID(binId) != source || sourceHash(source) != hash ||
            (!original->get_speed() && original->position() != originalPosition))
            return reject("SOURCE_CHANGED", key("Native project or source changed while the detached preview was decoded; no output was published."));
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return reject("WRITE_FAILED", output.errorString());
        if (!image.save(&output, "PNG") || !output.flush()) {
            const QString error = output.errorString();
            output.close();
            QFile::remove(path);
            return reject("WRITE_FAILED", error);
        }
        output.close();
        return finish({{key("path"), path},
                       {key("binId"), binId},
                       {key("sourceHash"), hash},
                       {key("sourceHashKind"), key("native_state_and_file_metadata_sha256")},
                       {key("frame"), frame},
                       {key("nativeVerified"), true},
                       {key("detached"), true},
                       {key("width"), image.width()},
                       {key("height"), image.height()}});
    }
    if (!service->attachMonitors()) return reject("GUI_REQUIRED", key("Source/project monitor controls require the running native editor window."));
    const QString monitorName = arguments.value(key("monitor")).toString(), operation = arguments.value(key("operation")).toString();
    const QSet<QString> operations{key("activate"),  key("open_bin_clip"), key("seek"),     key("step"),      key("play"),         key("pause"),
                                   key("play_zone"), key("loop_zone"),     key("set_zone"), key("set_in"),    key("set_out"),      key("start"),
                                   key("end"),       key("zone_start"),    key("zone_end"), key("next_snap"), key("previous_snap")};
    if ((monitorName != key("source") && monitorName != key("project")) || !operations.contains(operation))
        return reject("INVALID_ARGUMENT", key("Choose source/project and one documented fixed native monitor operation."));
    if (QApplication::activeModalWidget() || QApplication::activePopupWidget() || McpQuick::modalPopupActive(owner) ||
        QApplication::mouseButtons() != Qt::NoButton)
        return reject("UI_BUSY", key("Finish dragging or resolve the current native dialog/popup with get_ui before issuing a monitor operation."));
    QString expectedMonitor;
    if (!boundedString(arguments.value(key("expectedMonitor")), expectedMonitor, 128) || expectedMonitor != service->token())
        return reject("STALE_MONITOR", key("Native monitor selection, seek, zone or playback changed; refresh get_monitors."));
    if (service->busy(monitorName)) return reject("BUSY", key("An owned operation is still pending for this native monitor; poll monitor_job_status."));
    auto context = service->contexts.value(monitorName);
    if (!context.monitor || !context.proxy || !context.video) return reject("GUI_REQUIRED", key("Native monitor/consumer is unavailable."));
    std::shared_ptr<ProjectClip> source = monitorName == key("source") ? context.monitor->currentController() : nullptr;
    if (operation == key("open_bin_clip")) {
        const QString binId = arguments.value(key("binId")).toString();
        if (monitorName != key("source") || !pCore->projectItemModel()->hasClip(binId))
            return reject("INVALID_BIN_CLIP", key("open_bin_clip requires a native bin source and the source monitor."));
        source = pCore->projectItemModel()->getClipByBinID(binId);
    } else if (arguments.contains(key("binId")))
        return reject("INVALID_ARGUMENT", key("binId belongs only to source open_bin_clip."));
    if (monitorName == key("source") && operation != key("activate") && (!source || !source->statusReady()))
        return reject("MEDIA_NOT_READY", key("Open a ready native bin source in the source monitor first."));
    int maximum = operation == key("open_bin_clip") ? qBound(0, source->getFramePlaytime() - 1, MaxFrame) : maximumFrame(monitorName, context);
    int frame = -1, delta = 0, in = -1, out = -1;
    if (arguments.contains(key("frame")) && !Mcp::integer(arguments, key("frame"), 0, maximum, frame))
        return reject("INVALID_FRAME", key("Choose a frame within this native monitor's current range."));
    if ((operation == key("seek") && frame < 0) || (arguments.contains(key("frame")) && operation != key("seek") && operation != key("open_bin_clip")))
        return reject("INVALID_ARGUMENT", key("frame belongs only to seek or open_bin_clip."));
    const bool changesZone = operation == key("set_zone") || operation == key("set_in") || operation == key("set_out") ||
                             (operation == key("open_bin_clip") && arguments.contains(key("inFrame")));
    if (changesZone && context.monitor->effectSceneDisplayed(SceneType::MonitorSceneAutoMask))
        return reject("MASK_MODE_ACTIVE", key("Finish the native mask-editing scene before changing monitor bounds."));
    if (operation == key("step")) {
        if (!Mcp::integer(arguments, key("delta"), -MaxFrame, MaxFrame, delta) || delta == 0)
            return reject("INVALID_ARGUMENT", key("step requires a bounded nonzero delta."));
    } else if (arguments.contains(key("delta")))
        return reject("INVALID_ARGUMENT", key("delta belongs only to step."));
    if (arguments.contains(key("inFrame")) != arguments.contains(key("outFrameExclusive")) ||
        ((arguments.contains(key("inFrame")) || operation == key("set_zone")) &&
         (!Mcp::integer(arguments, key("inFrame"), 0, maximum, in) || !Mcp::integer(arguments, key("outFrameExclusive"), in + 1, maximum + 1, out))) ||
        (arguments.contains(key("inFrame")) && operation != key("set_zone") && operation != key("open_bin_clip")))
        return reject("INVALID_ZONE", key("Use paired inclusive inFrame and exclusive outFrameExclusive within the native monitor range."));
    if ((operation == key("play_zone") || operation == key("loop_zone") || operation == key("zone_start") || operation == key("zone_end")) &&
        (context.proxy->zoneIn() < 0 || context.proxy->zoneOut() <= context.proxy->zoneIn() || context.proxy->zoneOut() > maximum + 1))
        return reject("INVALID_ZONE", key("Set a nonempty half-open native monitor zone first."));
    if (!service->room()) return reject("BUSY", key("This MCP server owns 64 unfinished monitor jobs."));
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QPointer<KdenliveDoc> document = pCore->currentDoc();
    const QString sequence = pCore->currentTimelineId().toString();
    const QString wantedSourceHash = sourceHash(source);
    if (source && wantedSourceHash.isEmpty()) return reject("LIMIT", key("Native source context is unavailable or exceeds its serialization limit."));
    service->documents[id] = document;
    service->sources[id] = source;
    service->jobs[id] = QJsonObject{{key("jobId"), id},
                                    {key("monitor"), monitorName},
                                    {key("operation"), operation},
                                    {key("status"), key("queued")},
                                    {key("finished"), false},
                                    {key("executing"), false},
                                    {key("sequenceId"), sequence},
                                    {key("binId"), source ? source->clipId() : QString()},
                                    {key("sourceHash"), wantedSourceHash}};
    QTimer::singleShot(
        0, service,
        [service = QPointer<MonitorBridge>(service), owner = QPointer<QObject>(owner), id, monitorName, operation, source, document, expected, expectedMonitor,
         frame, delta, in, out] {
            if (!service || !owner) return;
            if (QApplication::activeModalWidget() || QApplication::activePopupWidget() || McpQuick::modalPopupActive(owner) ||
                QApplication::mouseButtons() != Qt::NoButton) {
                service->terminal(id, key("interrupted"), key("A native dialog, popup or drag began before the queued monitor operation ran."));
                return;
            }
            service->attachMonitors();
            auto job = service->jobs.value(id);
            if (!document || pCore->currentDoc() != document || service->state().value(key("expectedState")).toString() != expected ||
                service->token() != expectedMonitor) {
                service->terminal(id, key("interrupted"), key("Project or monitor context changed before the queued native operation ran."));
                return;
            }
            auto context = service->contexts.value(monitorName);
            if (!context.monitor || !context.proxy || !context.video) {
                service->terminal(id, key("failed"), key("Native monitor was destroyed."));
                return;
            }
            const bool changesZone =
                operation == key("set_zone") || operation == key("set_in") || operation == key("set_out") || (operation == key("open_bin_clip") && in >= 0);
            if (changesZone && context.monitor->effectSceneDisplayed(SceneType::MonitorSceneAutoMask)) {
                service->terminal(id, key("failed"), key("Native mask-editing mode forbids changing monitor bounds."));
                return;
            }
            job.insert(key("status"), key("running"));
            job.insert(key("executing"), true);
            service->jobs[id] = job;
            const QPointer<Monitor> monitor = context.monitor;
            const QPointer<MonitorProxy> proxy = context.proxy;
            const QPointer<VideoWidget> video = context.video;
            service->baselineSerial[id] = context.frameSerial;
            bool ok = true, awaitFrame = false;
            int target = -1;
            auto *manager = pCore->monitorManager();
            manager->activateMonitor(monitorName == key("source") ? Kdenlive::ClipMonitor : Kdenlive::ProjectMonitor);
            // Native set.* properties are copied onto each generated frame.
            // A fresh request marker distinguishes even repeated same-frame
            // seeks from old queued frames of the same URL or bin producer.
            auto stampRequest = [&] {
                auto *installed = video ? video->producer() : nullptr;
                if (!installed || !installed->is_valid()) return false;
                std::shared_ptr<Mlt::Producer> privateProducer;
                if (QString::fromUtf8(installed->get("set._kdenlive_monitor_generation")).isEmpty()) {
                    // Project and special-display producers can be shared native
                    // graphs. Annotate a private cut, never their shared master.
                    privateProducer.reset(installed->cut(installed->get_in(), installed->get_out()));
                    if (!privateProducer || !privateProducer->is_valid()) return false;
                    installed = privateProducer.get();
                    installed->set("set._kdenlive_monitor_generation", QUuid::createUuid().toByteArray(QUuid::WithoutBraces).constData());
                    installed->set("set._kdenlive_monitor_bin_id", source ? source->clipId().toUtf8().constData() : nullptr);
                }
                const QByteArray hash = sourceHash(source).toUtf8();
                // get_frame uses the same native service lock. Finish any
                // in-flight old frame before atomically installing the tuple;
                // otherwise it could acquire a new marker at the end of decode.
                installed->lock();
                installed->set("set._kdenlive_monitor_request", id.toUtf8().constData());
                installed->set("set._kdenlive_monitor_source_hash", hash.isEmpty() ? nullptr : hash.constData());
                installed->unlock();
                // Consumer restart/seek can wait for workers, so never call it
                // while holding their producer service lock.
                if (privateProducer) {
                    // Use the native monitor's public producer replacement path,
                    // which owns its open mutex and consumer reconfiguration.
                    monitor->updateClipProducer(privateProducer);
                    auto *actual = video ? video->producer() : nullptr;
                    if (!actual || !actual->is_valid() || actual->get_producer() != privateProducer->get_producer()) return false;
                }
                return true;
            };
            const bool requestsFrame = operation == key("seek") || operation == key("step") || operation == key("start") || operation == key("end") ||
                                       operation == key("zone_start") || operation == key("zone_end") || operation == key("next_snap") ||
                                       operation == key("previous_snap");
            if (requestsFrame && !stampRequest()) {
                service->terminal(id, key("failed"), key("Native monitor has no valid producer for frame provenance."));
                return;
            }
            if (operation == key("open_bin_clip")) {
                ok = source && source->statusReady() && monitor->slotOpenClip(source, frame < 0 ? 0 : frame, -1);
                // Opening can enter a native mask-confirmation dialog. Nested UI
                // processing may close the document or destroy the owning server.
                if (!service) return;
                if (!monitor || !proxy || !video || !document || pCore->currentDoc() != document) {
                    service->terminal(id, key("interrupted"), key("Native monitor or document closed during source opening."));
                    return;
                }
                if (ok && in >= 0 && monitor->effectSceneDisplayed(SceneType::MonitorSceneAutoMask)) {
                    service->terminal(id, key("failed"), key("Opened source entered native mask-editing mode; requested bounds were not changed."));
                    return;
                }
                if (ok && in >= 0) {
                    const QPoint previous = source->zone(), zone(in, out);
                    auto apply = [source, monitor](const QPoint &value) {
                        source->setZone(value);
                        if (monitor && monitor->currentController() == source) monitor->loadZone(value.x(), value.y());
                        return source->zone() == value;
                    };
                    ok = apply(zone);
                    if (ok && previous != zone)
                        pCore->pushUndo([apply, previous] { return apply(previous); }, [apply, zone] { return apply(zone); }, key("MCP source monitor zone"));
                }
                if (ok) ok = stampRequest();
                if (ok) {
                    target = frame < 0 ? 0 : frame;
                    monitor->slotSeek(target);
                    awaitFrame = true;
                }
            } else if (operation == key("activate")) {
                ok = monitor->isActive();
            } else if (operation == key("set_zone")) {
                const QPoint previous = monitor->getZoneInfo(), zone(in, out);
                if (monitorName == key("source")) {
                    auto apply = [source, monitor](const QPoint &value) {
                        source->setZone(value);
                        if (monitor && monitor->currentController() == source) monitor->loadZone(value.x(), value.y());
                        return source->zone() == value;
                    };
                    ok = apply(zone);
                    if (ok && previous != zone)
                        pCore->pushUndo([apply, previous] { return apply(previous); }, [apply, zone] { return apply(zone); }, key("MCP source monitor zone"));
                } else {
                    Q_EMIT monitor->zoneUpdatedWithUndo(previous, zone);
                }
                ok = ok && monitor->getZoneInfo() == zone;
            } else if (operation == key("set_in") || operation == key("set_out")) {
                const int wantedEndpoint = monitor->position() + (operation == key("set_out") ? 1 : 0);
                if (operation == key("set_in"))
                    manager->slotSetInPoint();
                else
                    manager->slotSetOutPoint();
                ok = proxy && (operation == key("set_in") ? proxy->zoneIn() == wantedEndpoint : proxy->zoneOut() == wantedEndpoint) && proxy->zoneIn() >= 0 &&
                     proxy->zoneOut() > proxy->zoneIn();
            } else if (operation == key("play") || operation == key("play_zone") || operation == key("loop_zone")) {
                if (monitor->isPlaying()) monitor->pause();
                if (operation == key("play"))
                    monitor->switchPlay(true);
                else if (operation == key("play_zone"))
                    monitor->slotPlayZone(true);
                else
                    monitor->slotLoopZone();
                ok = monitor->isPlaying();
            } else if (operation == key("pause")) {
                monitor->pause();
                ok = !monitor->isPlaying();
            } else {
                if (monitor->isPlaying()) monitor->pause();
                if (operation == key("seek")) {
                    target = frame;
                    monitor->slotSeek(target);
                } else if (operation == key("step")) {
                    target = int(qBound<qint64>(qint64(0), qint64(proxy->getPosition()) + delta, qint64(maximumFrame(monitorName, context))));
                    monitor->slotSeek(target);
                } else if (operation == key("start")) {
                    monitor->slotStart();
                    target = proxy->getPosition();
                } else if (operation == key("end")) {
                    target = maximumFrame(monitorName, context);
                    monitor->slotSeek(target);
                } else if (operation == key("zone_start")) {
                    monitor->slotZoneStart();
                    target = proxy->getPosition();
                } else if (operation == key("zone_end")) {
                    target = qMin(maximumFrame(monitorName, context), proxy->zoneOut() - 1);
                    monitor->slotSeek(target);
                } else if (operation == key("next_snap") || operation == key("previous_snap")) {
                    if (monitorName == key("project")) {
                        const auto timelineWidget = pCore->window() ? pCore->window()->getCurrentTimeline() : nullptr;
                        if (!timelineWidget || !timelineWidget->controller())
                            ok = false;
                        else if (operation == key("next_snap"))
                            timelineWidget->controller()->gotoNextSnap();
                        else
                            timelineWidget->controller()->gotoPreviousSnap();
                    } else if (operation == key("next_snap"))
                        monitor->slotSeekToNextSnap();
                    else
                        monitor->slotSeekToPreviousSnap();
                    target = qBound(0, proxy->getPosition(), maximumFrame(monitorName, context));
                    if (proxy->getPosition() != target) monitor->slotSeek(target);
                }
                awaitFrame = true;
            }
            if (!monitor || !proxy || !video) {
                service->terminal(id, key("failed"), key("Native monitor was destroyed during its operation."));
                return;
            }
            job = service->jobs.value(id);
            job.insert(key("executing"), false);
            job.insert(key("sourceHash"), sourceHash(source));
            job.insert(key("producerGeneration"),
                       video->producer() ? QString::fromUtf8(video->producer()->get("set._kdenlive_monitor_generation")) : QString());
            job.insert(key("requestedFrame"), target);
            service->jobs[id] = job;
            if (!ok) {
                service->terminal(id, key("failed"), key("Native monitor refused the operation or readback did not match."));
                return;
            }
            if (!awaitFrame) {
                service->terminal(id, key("completed"));
                return;
            }
            context = service->contexts.value(monitorName);
            service->issuedSeek[id] = context.seekGeneration;
            service->deadlines[id] = QDateTime::currentMSecsSinceEpoch() + 15000;
            job.insert(key("status"), key("awaiting_frame"));
            service->jobs[id] = job;
            // The native proxy ignores an unchanged cursor position. After
            // stamping a request, discard buffered frames and seek the installed
            // private producer explicitly so frame zero/repeated seeks also
            // deliver fresh provenance instead of reusing an unstamped frame.
            video->purgeCache();
            video->requestSeek(target, true);
            service->timer.start();
            service->poll();
        });
    service->timer.start();
    return finish({{key("accepted"), true}, {key("jobId"), id}, {key("job"), service->jobs.value(id)}});
}
} // namespace McpMonitor
