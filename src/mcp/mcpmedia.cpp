/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcpmedia.h"
#include "bin/bin.h"
#include "bin/clipcreator.hpp"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "jobs/transcodetask.h"
#include "kdenlivesettings.h"
#include "lib/audio/audioStreamInfo.h"
#include "mcphelpers.h"
#include "project/projectmanager.h"
#include "timeline2/model/clipmodel.hpp"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include <KConfig>
#include <KConfigGroup>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <mlt++/MltFilter.h>
#include <mlt++/MltProducer.h>
#include <unordered_set>
#include <vector>

class McpMediaAccess
{
public:
    static bool create(const std::shared_ptr<TimelineItemModel> &timeline, const QString &binRange, int &id, PlaylistState::ClipState state, int stream,
                       Fun &undo, Fun &redo)
    {
        return timeline->requestClipCreation(binRange, id, state, stream, 1.0, false, undo, redo);
    }
    static int audioStream(const std::shared_ptr<TimelineItemModel> &timeline, int id)
    {
        return timeline->data(timeline->makeClipIndexFromID(id), TimelineModel::AudioStreamRole).toInt();
    }
};
namespace {
constexpr int MaxFrame = 10000000;
QString k(const char *value)
{
    return QString::fromLatin1(value);
}
QJsonObject text(const char *description)
{
    return Mcp::property(k("string"), k(description));
}
QJsonObject number(const char *description)
{
    return Mcp::property(k("integer"), k(description));
}
QJsonObject boolean(const char *description)
{
    return Mcp::property(k("boolean"), k(description));
}
QJsonObject array(const char *description)
{
    auto result = Mcp::property(k("array"), k(description));
    result.insert(k("items"), QJsonObject{});
    return result;
}
bool boundedText(const QJsonValue &v, QString &result, int size = 4096)
{
    if (!v.isString()) return false;
    result = v.toString();
    return !result.isEmpty() && result.size() <= size && !result.contains(QChar::Null);
}
bool newOutput(const QJsonValue &v, QString &path)
{
    if (!boundedText(v, path) || !QDir::isAbsolutePath(path)) return false;
    path = QDir::fromNativeSeparators(path);
    if (path.startsWith(QLatin1String("//")) || path.startsWith(QLatin1String("/dev/"))) return false;
    const QFileInfo info(path);
    const QString leaf = info.fileName();
    const QString parent = info.absoluteDir().canonicalPath();
    if (parent.isEmpty() || info.exists() || info.isSymLink() || !QFileInfo(parent).isWritable() || leaf.isEmpty() || leaf.endsWith(QLatin1Char('.')) ||
        leaf.endsWith(QLatin1Char(' ')) || QRegularExpression(QStringLiteral("[<>:\\x00-\\x1f|?*]")).match(leaf).hasMatch())
        return false;
    path = QDir(parent).absoluteFilePath(leaf);
    return true;
}
std::shared_ptr<ProjectClip> sourceFor(const QJsonObject &args)
{
    const QString id = args.value(k("binId")).toString();
    if (!pCore || !pCore->projectItemModel() || !pCore->projectItemModel()->hasClip(id)) return {};
    return pCore->projectItemModel()->getClipByBinID(id);
}
QMap<QString, QString> transcodeProfiles()
{
    KConfig config(QStringLiteral("kdenlivetranscodingrc"), KConfig::CascadeConfig, QStandardPaths::AppDataLocation);
    return KConfigGroup(&config, QStringLiteral("intermediate")).entryMap();
}
QString profileKind(const QString &value)
{
    return value.section(QLatin1Char(';'), -1).trimmed();
}
QString profileExtension(const QString &value)
{
    const auto match = QRegularExpression(QStringLiteral("%1(\\.[A-Za-z0-9]{1,12})(?:\\s|;|$)")).match(value);
    return match.hasMatch() ? match.captured(1).toLower() : QString();
}
QStringList effectsFor(const std::shared_ptr<ProjectClip> &source, int stream)
{
    return source->getAudioStreamEffect(stream);
}
bool applyEffects(const std::shared_ptr<ProjectClip> &source, int stream, const QStringList &values)
{
    const auto previous = source->getAudioStreamEffect(stream);
    for (const auto &value : previous)
        source->removeAudioStreamEffect(stream, value.section(QLatin1Char(' '), 0, 0));
    for (const auto &value : values)
        source->addAudioStreamEffect(stream, value);
    Q_EMIT source->updateStreamInfo(stream);
    return source->getAudioStreamEffect(stream) == values;
}
class MediaBridge : public QObject
{
public:
    explicit MediaBridge(QObject *owner)
        : QObject(owner)
    {
        setObjectName(QStringLiteral("kdenlive-mcp-media"));
    }
    ~MediaBridge() override
    {
        for (const auto &task : tasks)
            if (task) task->requestStop();
    }
    QHash<QString, QJsonObject> jobs;
    QHash<QString, QPointer<TranscodeTask>> tasks;
    QHash<QString, std::weak_ptr<ProjectClip>> loadedSources;
    std::function<QJsonObject()> readState;
    QJsonObject state() const { return readState ? readState() : QJsonObject{}; }
    bool room()
    {
        if (jobs.size() < 64) return true;
        for (auto it = jobs.begin(); it != jobs.end(); ++it)
            if (it.value().value(k("nativeFinished")).toBool() && it.value().value(k("status")) != k("loading")) {
                tasks.remove(it.key());
                loadedSources.remove(it.key());
                jobs.erase(it);
                return true;
            }
        return false;
    }
    QJsonObject jobState(const QString &id)
    {
        auto job = jobs.value(id);
        if (job.value(k("status")) == k("loading")) {
            const auto source = loadedSources.value(id).lock();
            const bool current =
                source && pCore->projectItemModel()->hasClip(source->clipId()) && pCore->projectItemModel()->getClipByBinID(source->clipId()) == source;
            if (!current) {
                job.insert(k("status"), k("completed_detached"));
                job.insert(k("imported"), false);
            } else if (source->statusReady()) {
                const QString expected = QFileInfo(job.value(k("path")).toString()).canonicalFilePath();
                const QString actual = QFileInfo(source->url()).canonicalFilePath();
                if (!expected.isEmpty() && expected == actual) {
                    job.insert(k("status"), k("completed"));
                    job.insert(k("imported"), true);
                } else {
                    job.insert(k("status"), k("completed_detached"));
                    job.insert(k("imported"), false);
                    job.insert(k("error"), k("The native bin source was relinked before output loading completed; the published file was preserved."));
                }
            } else {
                const ObjectId owner(KdenliveObjectType::BinClip, source->clipId().toInt(), QUuid());
                const bool loading = source->clipStatus() == FileStatus::StatusWaiting && pCore->taskManager.hasPendingJob(owner, AbstractTask::LOADJOB);
                if (!loading) {
                    job.insert(k("status"), k("failed"));
                    job.insert(k("imported"), false);
                    job.insert(k("error"), k("Native output import failed or no native load job remains; the published file is available."));
                }
            }
            jobs[id] = job;
        }
        return job;
    }
};
MediaBridge *bridge(QObject *owner)
{
    for (QObject *child : owner->children())
        if (auto *existing = dynamic_cast<MediaBridge *>(child)) return existing;
    return new MediaBridge(owner);
}
} // namespace
namespace McpMedia {
QJsonObject sourceState(const std::shared_ptr<ProjectClip> &source)
{
    if (!source) return {};
    QJsonObject result{{k("binId"), source->clipId()}, {k("ready"), source->statusReady()}};
    if (!source->statusReady()) return result;
    QJsonArray audio, video;
    const auto active = source->activeStreams();
    const auto info = source->audioInfo().get();
    const auto streams = source->audioStreams();
    for (auto it = streams.cbegin(); it != streams.cend(); ++it) {
        QJsonArray effects;
        for (const auto &effect : source->getAudioStreamEffect(it.key()))
            effects.append(effect);
        const QString prefix = QStringLiteral("meta.media.%1.").arg(it.key());
        audio.append(QJsonObject{{k("id"), it.key()},
                                 {k("ffmpegAudioIndex"), source->audioStreamIndex(it.key())},
                                 {k("name"), it.value()},
                                 {k("channels"), info ? info->channelsForStream(it.key()) : 0},
                                 {k("active"), active.contains(it.key()) || active.contains(std::numeric_limits<int>::max())},
                                 {k("codec"), source->getProducerProperty(prefix + k("codec.name"))},
                                 {k("sampleRate"), source->getProducerIntProperty(prefix + k("codec.sample_rate"))},
                                 {k("effects"), effects}});
    }
    const int count = qBound(0, source->getProducerIntProperty(k("meta.media.nb_streams")), 128);
    for (int i = 0; i < count; ++i) {
        const QString prefix = QStringLiteral("meta.media.%1.").arg(i);
        if (source->getProducerProperty(prefix + k("stream.type")) == k("video"))
            video.append(QJsonObject{{k("id"), i},
                                     {k("codec"), source->getProducerProperty(prefix + k("codec.name"))},
                                     {k("width"), source->getProducerIntProperty(prefix + k("codec.width"))},
                                     {k("height"), source->getProducerIntProperty(prefix + k("codec.height"))}});
    }
    result.insert(k("audioStreams"), audio);
    result.insert(k("videoStreams"), video);
    QJsonArray activeIds;
    for (int id : active.keys())
        activeIds.append(id);
    result.insert(k("activeStreamIds"), activeIds);
    result.insert(k("defaultAudioStream"), source->getProducerIntProperty(k("audio_index")));
    result.insert(k("defaultVideoStream"), source->getProducerIntProperty(k("video_index")));
    result.insert(k("audioStreamOrdinal"), source->getProducerIntProperty(k("astream")));
    result.insert(k("videoStreamOrdinal"), source->getProducerIntProperty(k("vstream")));
    result.insert(k("testAudio"), source->getProducerIntProperty(k("set.test_audio")));
    result.insert(k("testImage"), source->getProducerIntProperty(k("set.test_image")));
    result.insert(k("activeStreamProperty"), source->getProducerProperty(k("kdenlive:active_streams")));
    const bool variableFrameRate = source->getProducerIntProperty(k("meta.media.variable_frame_rate")) != 0;
    const bool transcodeRequested = source->getProducerIntProperty(k("_wait_for_transcode")) != 0;
    const bool seekable = source->getProducerIntProperty(k("seekable")) != 0;
    const bool fileMedia = source->clipType() == ClipType::Audio || source->clipType() == ClipType::AV || source->clipType() == ClipType::Video;
    result.insert(k("variableFrameRate"), variableFrameRate);
    result.insert(k("seekable"), seekable);
    result.insert(k("nativeTranscodeRequested"), transcodeRequested);
    // The loader's private request flag can be consumed during producer setup.
    // Readiness must still reflect decoded VFR/non-seekable metadata, matching
    // the project's media status even after reload or reopening a saved file.
    result.insert(k("needsTranscode"), fileMedia && (transcodeRequested || variableFrameRate || !seekable));
    result.insert(k("resource"), source->url());
    result.insert(k("durationFrames"), source->getFramePlaytime());
    return result;
}

QJsonArray toolCatalog()
{
    const auto expected = text("Fresh expectedState from get_state.");
    const auto bin = text("Native bin clip ID.");
    QJsonArray tools;
    tools.append(Mcp::tool(k("list_media_streams"), k("Read actual native media stream IDs, channel counts, defaults, active streams and source effects."),
                           {{k("binId"), bin}}, {k("binId")}, true));
    tools.append(Mcp::tool(k("configure_source_streams"),
                           k("Undoably select native source audio/video streams. Video changes reload asynchronously; inspect ready before further edits."),
                           {{k("expectedState"), expected},
                            {k("binId"), bin},
                            {k("audioStreams"), array("Actual audio stream IDs; empty disables audio; [2147483647] merges all.")},
                            {k("defaultAudioStream"), number("Actual default audio stream ID, or -1 to disable.")},
                            {k("videoStream"), number("Actual native video stream ID, or -1 to disable.")}},
                           {k("expectedState"), k("binId")}));
    auto gain = Mcp::property(k("number"), k("Native source stream gain in dB, -100 to 60; zero removes gain."));
    tools.append(Mcp::tool(k("configure_source_audio"),
                           k("Undoably rename or adjust a native source audio stream: gain, normalization, channel swap and channel copy."),
                           {{k("expectedState"), expected},
                            {k("binId"), bin},
                            {k("streamId"), number("Actual native audio stream ID.")},
                            {k("name"), text("New stream name, up to 256 characters.")},
                            {k("gainDb"), gain},
                            {k("normalize"), boolean("Enable dynamic loudness normalization.")},
                            {k("swapChannels"), boolean("Swap the first two channels.")},
                            {k("copyChannel"), text("none, left_to_right, or right_to_left.")}},
                           {k("expectedState"), k("binId"), k("streamId")}));
    tools.append(Mcp::tool(k("insert_media_streams"),
                           k("Insert exact source frames using explicit native audio stream IDs and explicit destination tracks, as one undoable transaction."),
                           {{k("expectedState"), expected},
                            {k("binId"), bin},
                            {k("startFrame"), number("Timeline start, inclusive.")},
                            {k("sourceInFrame"), number("Source start, inclusive.")},
                            {k("durationFrames"), number("Exact positive duration in project frames.")},
                            {k("videoTrackId"), number("Optional unlocked video destination track.")},
                            {k("audio"), array("Objects {streamId,trackId}; each stream is placed only on its explicitly named audio track.")},
                            {k("group"), boolean("Group inserted clips; defaults true.")},
                            {k("mode"), text("place (empty range), insert (ripple chosen tracks), or overwrite (lift chosen range).")}},
                           {k("expectedState"), k("binId"), k("startFrame"), k("sourceInFrame"), k("durationFrames")}));
    tools.append(
        Mcp::tool(k("list_transcode_profiles"), k("List installed native transcoding presets and their media kind and output extension."), {}, {}, true));
    tools.append(Mcp::tool(
        k("start_transcode"),
        k("Start a native asynchronous transcode using an installed preset. Output is decoded, then published at a NEW explicit path without replacing "
          "existing files. Video output is constant frame rate."),
        {{k("expectedState"), expected},
         {k("binId"), bin},
         {k("profileId"), text("Exact installed profile ID.")},
         {k("path"), text("Absolute NEW local output path matching the preset extension.")},
         {k("inFrame"), number("Optional inclusive source start; paired with outFrame.")},
         {k("outFrame"), number("Optional inclusive source end.")},
         {k("importResult"), boolean("Import output into this same project when ready; defaults true.")},
         {k("replaceSource"), boolean("Relink original native bin item after full-range conversion; preserves original source file. Defaults false.")},
         {k("fpsNumerator"), number("Optional target constant frame rate numerator.")},
         {k("fpsDenominator"), number("Optional target constant frame rate denominator.")}},
        {k("expectedState"), k("binId"), k("profileId"), k("path")}));
    tools.append(Mcp::tool(k("transcode_status"), k("Read this MCP server's native transcoding jobs, progress, terminal errors and imported output readiness."),
                           {{k("jobId"), text("Optional job ID; omit to list owned jobs.")}}, {}, true));
    tools.append(
        Mcp::tool(k("cancel_transcode"),
                  k("Request cancellation of an owned native transcoding task. Poll status until terminal; cancellation cannot undo a completed publication."),
                  {{k("jobId"), text("Owned transcode job ID.")}}, {k("jobId")}));
    return tools;
}
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState,
                     const std::function<QJsonObject()> &stateReader)
{
    bool known = false;
    for (const auto &entry : toolCatalog())
        if (entry.toObject().value(k("name")) == name) {
            known = true;
            break;
        }
    if (!known) return {};
    auto reject = [&](const char *code, const char *message) { return Mcp::failure(k(code), k(message), nativeState); };
    auto *service = bridge(owner);
    service->readState = stateReader;
    auto finish = [&](QJsonObject data = {}) {
        data.insert(k("ok"), true);
        data.insert(k("state"), service->state());
        return Mcp::toolResult(data);
    };
    auto rollback = [&](const Fun &undo, const char *message) {
        const bool restored = undo();
        return Mcp::failure(restored ? k("EDIT_FAILED") : k("ROLLBACK_FAILED"),
                            restored ? k(message) + k(" Changes were rolled back.") : k("Native rollback failed; inspect the project."), service->state());
    };
    if (name == k("transcode_status") || name == k("cancel_transcode")) {
        if (!arguments.contains(k("jobId")) && name == k("transcode_status")) {
            QJsonArray jobs;
            auto ids = service->jobs.keys();
            std::sort(ids.begin(), ids.end());
            for (const auto &id : ids)
                jobs.append(service->jobState(id));
            return finish({{k("jobs"), jobs}});
        }
        QString id;
        if (!boundedText(arguments.value(k("jobId")), id, 128) || !service->jobs.contains(id))
            return reject("INVALID_JOB", "Unknown owned native transcode job.");
        auto job = service->jobState(id);
        if (name == k("cancel_transcode") && !job.value(k("nativeFinished")).toBool()) {
            const auto task = service->tasks.value(id);
            if (task && task->requestStop()) {
                job.insert(k("status"), k("cancelling"));
                service->jobs[id] = job;
            }
        }
        return finish({{k("job"), job}});
    }
    if (name == k("list_transcode_profiles")) {
        QJsonArray profiles;
        const auto installed = transcodeProfiles();
        for (auto it = installed.cbegin(); it != installed.cend(); ++it)
            profiles.append(QJsonObject{{k("id"), it.key()}, {k("kind"), profileKind(it.value())}, {k("extension"), profileExtension(it.value())}});
        return finish({{k("profiles"), profiles}});
    }
    const auto source = sourceFor(arguments);
    if (!source) return reject("INVALID_BIN_CLIP", "Unknown native bin clip ID.");
    if (name == k("list_media_streams")) return finish({{k("source"), sourceState(source)}});
    if (!source->statusReady()) return reject("MEDIA_NOT_READY", "Wait for native media loading to finish.");
    const auto timeline = pCore->projectManager()->getTimeline();
    if (!timeline || !pCore->undoStack()) return reject("NOT_READY", "No active native timeline or undo stack.");
    if (name == k("configure_source_streams")) {
        if (!arguments.contains(k("audioStreams")) && !arguments.contains(k("defaultAudioStream")) && !arguments.contains(k("videoStream")))
            return reject("INVALID_ARGUMENT", "Supply an audio or video stream setting.");
        QMap<QString, QString> changes, previous;
        const auto streams = source->audioStreams();
        int defaultAudio = source->getProducerIntProperty(k("audio_index"));
        if (arguments.contains(k("defaultAudioStream")) &&
            (!Mcp::integer(arguments, k("defaultAudioStream"), -1, std::numeric_limits<int>::max(), defaultAudio) ||
             (defaultAudio != -1 && !streams.contains(defaultAudio))))
            return reject("INVALID_STREAM", "Default audio must be an actual native audio stream ID, or -1.");
        if (arguments.contains(k("audioStreams"))) {
            const auto values = arguments.value(k("audioStreams")).toArray();
            if (!arguments.value(k("audioStreams")).isArray() || values.size() > 64)
                return reject("INVALID_ARGUMENT", "Audio streams must be a bounded array of actual stream IDs.");
            QSet<int> selected;
            QStringList active;
            for (const auto &value : values) {
                int id = -1;
                if (!Mcp::integer({{k("id"), value}}, k("id"), 0, std::numeric_limits<int>::max(), id) || selected.contains(id) ||
                    (!streams.contains(id) && id != std::numeric_limits<int>::max()))
                    return reject("INVALID_STREAM", "Audio streams must be distinct actual native stream IDs.");
                selected.insert(id);
                active << QString::number(id);
            }
            if (selected.contains(std::numeric_limits<int>::max()) && (selected.size() != 1 || streams.isEmpty()))
                return reject("INVALID_STREAM", "Merged audio cannot be combined with individual stream IDs.");
            if (values.isEmpty()) {
                defaultAudio = -1;
                active << k("-1");
            } else if (!arguments.contains(k("defaultAudioStream")) && !selected.contains(defaultAudio) && !selected.contains(std::numeric_limits<int>::max()))
                defaultAudio = values.first().toInt();
            if (selected.contains(std::numeric_limits<int>::max()) && !streams.contains(defaultAudio)) defaultAudio = streams.firstKey();
            if (defaultAudio != -1 && !selected.contains(defaultAudio) && !selected.contains(std::numeric_limits<int>::max()))
                return reject("INVALID_STREAM", "Default audio stream must belong to the selected active streams.");
            changes[k("kdenlive:active_streams")] = active.join(QLatin1Char(';'));
        }
        if (arguments.contains(k("audioStreams")) || arguments.contains(k("defaultAudioStream"))) {
            changes[k("audio_index")] = QString::number(defaultAudio);
            changes[k("astream")] =
                QString::number(defaultAudio == -1 || defaultAudio == std::numeric_limits<int>::max() ? defaultAudio : source->audioStreamIndex(defaultAudio));
            changes[k("set.test_audio")] = defaultAudio == -1 ? k("1") : k("0");
        }
        if (arguments.contains(k("videoStream"))) {
            int selected = -1;
            if (!Mcp::integer(arguments, k("videoStream"), -1, 127, selected)) return reject("INVALID_STREAM", "Invalid video stream ID.");
            int ordinal = -1, current = 0;
            for (const auto &entry : sourceState(source).value(k("videoStreams")).toArray()) {
                if (entry.toObject().value(k("id")).toInt() == selected) ordinal = current;
                ++current;
            }
            if (selected != -1 && ordinal < 0) return reject("INVALID_STREAM", "Video selection must name an actual native video stream.");
            changes[k("video_index")] = QString::number(selected);
            changes[k("vstream")] = QString::number(ordinal);
            changes[k("set.test_image")] = selected == -1 ? k("1") : k("0");
        }
        for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            previous[it.key()] = source->getProducerProperty(it.key());
        auto apply = [source](const QMap<QString, QString> &values) {
            source->setProperties(values);
            return true;
        };
        if (changes == previous) return finish({{k("source"), sourceState(source)}, {k("changed"), false}});
        apply(changes);
        pCore->pushUndo([apply, previous] { return apply(previous); }, [apply, changes] { return apply(changes); }, k("MCP configure source streams"));
        return finish({{k("source"), sourceState(source)}, {k("pending"), !source->statusReady()}, {k("changed"), true}});
    }
    if (name == k("configure_source_audio")) {
        int stream = -1;
        const auto streams = source->audioStreams();
        if (!Mcp::integer(arguments, k("streamId"), 0, std::numeric_limits<int>::max(), stream) || !streams.contains(stream))
            return reject("INVALID_STREAM", "Use an actual native audio stream ID.");
        const QString oldName = streams.value(stream);
        QString newName = oldName;
        if (arguments.contains(k("name")) && !boundedText(arguments.value(k("name")), newName, 256))
            return reject("INVALID_ARGUMENT", "Stream name must contain 1 to 256 characters.");
        if (arguments.size() <= 3) return reject("INVALID_ARGUMENT", "Supply at least one source audio setting.");
        auto values = source->getAudioStreamEffect(stream);
        const auto before = values;
        auto replace = [&](const QString &effect, const QString &value) {
            for (int i = values.size() - 1; i >= 0; --i)
                if (values.at(i).section(QLatin1Char(' '), 0, 0) == effect) values.removeAt(i);
            if (!value.isEmpty()) values << value;
        };
        if (arguments.contains(k("gainDb"))) {
            double gain = 0;
            if (!Mcp::real(arguments, k("gainDb"), -100, 60, gain)) return reject("INVALID_ARGUMENT", "Gain must be between -100 and 60 dB.");
            replace(k("volume"), qFuzzyIsNull(gain) ? QString() : k("volume level=") + QString::number(gain, 'g', 12));
        }
        for (const auto &pair : {std::pair<const char *, const char *>("normalize", "dynamic_loudness"), {"swapChannels", "channelswap"}}) {
            if (!arguments.contains(k(pair.first))) continue;
            if (!arguments.value(k(pair.first)).isBool()) return reject("INVALID_ARGUMENT", "Source audio switches must be boolean.");
            replace(k(pair.second), arguments.value(k(pair.first)).toBool() ? k(pair.second) : QString());
        }
        const auto info = source->audioInfo().get();
        if ((arguments.value(k("swapChannels")).toBool() || (arguments.contains(k("copyChannel")) && arguments.value(k("copyChannel")) != k("none"))) &&
            (!info || info->channelsForStream(stream) < 2))
            return reject("CHANNEL_MISMATCH", "Channel swap and copy require at least two native source channels.");
        if (arguments.contains(k("copyChannel"))) {
            const QString direction = arguments.value(k("copyChannel")).toString();
            if (direction != k("none") && direction != k("left_to_right") && direction != k("right_to_left"))
                return reject("INVALID_ARGUMENT", "copyChannel is none, left_to_right, or right_to_left.");
            replace(k("channelcopy"), direction == k("none")            ? QString()
                                      : direction == k("left_to_right") ? k("channelcopy from=0 to=1")
                                                                        : k("channelcopy from=1 to=0"));
        }
        for (const auto &effect : values) {
            Mlt::Filter filter(pCore->getProjectProfile(), effect.section(QLatin1Char(' '), 0, 0).toUtf8().constData());
            if (!filter.is_valid()) return reject("FILTER_UNAVAILABLE", "A requested native source audio filter is not installed.");
        }
        if (values == before && newName == oldName) return finish({{k("source"), sourceState(source)}, {k("changed"), false}});
        auto apply = [source, stream](const QStringList &effects, const QString &streamName) {
            const bool ok = applyEffects(source, stream, effects);
            source->renameAudioStream(stream, streamName);
            return ok && source->audioStreams().value(stream) == streamName;
        };
        if (!apply(values, newName)) {
            apply(before, oldName);
            return reject("EDIT_FAILED", "Native audio stream settings did not verify; prior settings were restored.");
        }
        pCore->pushUndo([apply, before, oldName] { return apply(before, oldName); }, [apply, values, newName] { return apply(values, newName); },
                        k("MCP configure source audio"));
        return finish({{k("source"), sourceState(source)}, {k("changed"), true}});
    }

    if (name == k("insert_media_streams")) {
        int start = 0, in = 0, duration = 0;
        if (!Mcp::integer(arguments, k("startFrame"), 0, MaxFrame, start) || !Mcp::integer(arguments, k("sourceInFrame"), 0, MaxFrame, in) ||
            !Mcp::integer(arguments, k("durationFrames"), 1, 1000000, duration) || qint64(start) + duration > MaxFrame ||
            qint64(in) + duration > source->getFramePlaytime())
            return reject("INVALID_SOURCE_RANGE", "Use a positive exact range within the ready native source and timeline bounds.");
        if (source->getProducerIntProperty(k("_wait_for_transcode")) != 0 || source->getProducerIntProperty(k("meta.media.variable_frame_rate")) != 0)
            return reject("TRANSCODE_REQUIRED", "Transcode variable-frame-rate or non-seekable media before frame-exact insertion.");
        if (arguments.contains(k("group")) && !arguments.value(k("group")).isBool()) return reject("INVALID_ARGUMENT", "group must be boolean.");
        const bool group = arguments.value(k("group")).toBool(true);
        const QString mode = arguments.contains(k("mode")) ? arguments.value(k("mode")).toString() : k("place");
        if (mode != k("place") && mode != k("insert") && mode != k("overwrite")) return reject("INVALID_ARGUMENT", "Mode must be place, insert, or overwrite.");
        struct Placement
        {
            int track;
            int stream;
            bool audio;
        };
        std::vector<Placement> placements;
        QVector<int> tracks;
        QSet<int> selectedTracks;
        auto add = [&](int track, int stream, bool audio) {
            if (!timeline->isTrack(track) || timeline->trackIsLocked(track) || timeline->isAudioTrack(track) != audio || selectedTracks.contains(track))
                return false;
            selectedTracks.insert(track);
            tracks << track;
            placements.push_back({track, stream, audio});
            return true;
        };
        if (arguments.contains(k("videoTrackId"))) {
            int track = -1;
            if (!source->hasVideo() || !Mcp::integer(arguments, k("videoTrackId"), 0, MaxFrame, track) || !add(track, -1, false))
                return reject("INVALID_TRACK", "Video destination must be a distinct existing unlocked video track and the source must have video.");
        }
        if (arguments.contains(k("audio"))) {
            if (!arguments.value(k("audio")).isArray() || arguments.value(k("audio")).toArray().size() > 32)
                return reject("INVALID_ARGUMENT", "Audio placements must be an array of up to 32 stream/track objects.");
            const auto streams = source->audioStreams();
            for (const auto &value : arguments.value(k("audio")).toArray()) {
                const auto entry = value.toObject();
                int stream = -1, track = -1;
                if (!value.isObject() || entry.size() != 2 || !Mcp::integer(entry, k("streamId"), 0, std::numeric_limits<int>::max(), stream) ||
                    !streams.contains(stream) || streams.isEmpty() || !Mcp::integer(entry, k("trackId"), 0, MaxFrame, track) || !add(track, stream, true))
                    return reject("INVALID_STREAM", "Each audio placement needs an actual source stream ID and a distinct existing unlocked audio track.");
            }
        }
        if (placements.empty()) return reject("INVALID_ARGUMENT", "Supply a video destination or at least one explicit audio stream destination.");
        std::unordered_set<int> affected;
        for (int track : tracks) {
            // Compositions have independent native occupancy and may legally
            // overlap a newly placed clip. Ripple/overwrite still inspect them.
            const auto items = timeline->getItemsInRange(track, 0, -1, mode != k("place"));
            for (int id : items) {
                const int at = timeline->getItemPosition(id), end = at + timeline->getItemPlaytime(id);
                if (end <= start || (mode != k("insert") && at >= start + duration)) continue;
                if (mode == k("place")) return reject("COLLISION", "A selected destination track is occupied in the requested range.");
                if (timeline->isInGroup(id)) return reject("GROUP_SCOPE", "Ungroup material affected by this insert/overwrite transaction before editing.");
                if (mode == k("insert") && qint64(end) + duration > MaxFrame)
                    return reject("LIMIT", "Ripple insertion would shift material beyond the timeline frame limit.");
                affected.insert(id);
            }
        }
        Fun undo = [] { return true; }, redo = [] { return true; };
        auto step = [&](const std::function<bool(Fun &, Fun &)> &operation, bool selfRollback) {
            Fun u = [] { return true; }, r = [] { return true; };
            if (!operation(u, r)) {
                if (!selfRollback) u();
                return false;
            }
            const Fun oldUndo = undo, oldRedo = redo;
            undo = [oldUndo, u] {
                const bool current = u();
                const bool previous = oldUndo();
                return current && previous;
            };
            redo = [oldRedo, r] { return oldRedo() && r(); };
            return true;
        };
        if (mode == k("insert")) {
            for (int track : tracks) {
                const int crossing = timeline->getClipByPosition(track, start);
                if (crossing >= 0 && timeline->getClipPosition(crossing) < start &&
                    !step([&](Fun &u, Fun &r) { return TimelineFunctions::requestClipCut(timeline, crossing, start, u, r); }, true))
                    return rollback(undo, "Native insertion boundary split failed.");
            }
            if (!step([&](Fun &u, Fun &r) { return TimelineFunctions::requestInsertSpace(timeline, QPoint(start, start + duration), u, r, tracks); }, true))
                return rollback(undo, "Native ripple insertion failed.");
        } else if (mode == k("overwrite")) {
            for (int track : tracks)
                if (!step([&](Fun &u, Fun &r) { return TimelineFunctions::liftZone(timeline, track, QPoint(start, start + duration), u, r); }, false))
                    return rollback(undo, "Native overwrite zone lift failed.");
        }
        QJsonArray inserted;
        std::unordered_set<int> created;
        const QString range = source->clipId() + QLatin1Char('/') + QString::number(in) + QLatin1Char('/') + QString::number(in + duration - 1);
        for (const auto &placement : placements) {
            int id = -1;
            if (!step(
                    [&](Fun &u, Fun &r) {
                        return McpMediaAccess::create(timeline, range, id, placement.audio ? PlaylistState::AudioOnly : PlaylistState::VideoOnly,
                                                      placement.stream, u, r);
                    },
                    true) ||
                !step(
                    [&](Fun &u, Fun &r) {
                        return timeline->requestClipMove(id, placement.track, start, false, true, true, true, u, r) == TimelineModel::MoveSuccess;
                    },
                    true))
                return rollback(undo, "Native explicit-stream insertion failed.");
            if (!timeline->isClip(id) || timeline->getClipTrackId(id) != placement.track || timeline->getClipPosition(id) != start ||
                timeline->getClipPlaytime(id) != duration || timeline->getClipInOut(id) != std::make_pair(in, in + duration - 1) ||
                (placement.audio && McpMediaAccess::audioStream(timeline, id) != placement.stream))
                return rollback(undo, "Native stream selection or exact source/timeline range did not verify.");
            created.insert(id);
            QJsonObject item{{k("clipId"), id},
                             {k("trackId"), placement.track},
                             {k("stream"), placement.audio ? k("audio") : k("video")},
                             {k("startFrame"), start},
                             {k("sourceInFrame"), in},
                             {k("sourceOutFrame"), in + duration - 1},
                             {k("durationFrames"), duration}};
            if (placement.audio) item.insert(k("streamId"), McpMediaAccess::audioStream(timeline, id));
            inserted.append(item);
        }
        int groupId = -1;
        if (group && created.size() > 1 &&
            !step(
                [&](Fun &u, Fun &r) {
                    groupId = timeline->requestClipsGroup(
                        created, u, r, arguments.contains(k("videoTrackId")) && created.size() == 2 ? GroupType::AVSplit : GroupType::Normal);
                    return groupId >= 0;
                },
                true))
            return rollback(undo, "Native grouping of inserted streams failed.");
        pCore->pushUndo(undo, redo, k("MCP insert explicit media streams"));
        timeline->requestClearSelection();
        return finish({{k("items"), inserted}, {k("groupId"), groupId}, {k("mode"), mode}});
    }

    if (name == k("start_transcode")) {
        QString path, profileId;
        const auto installed = transcodeProfiles();
        if (!boundedText(arguments.value(k("profileId")), profileId, 512) || !installed.contains(profileId))
            return reject("INVALID_PROFILE", "Choose an installed native transcode profile from list_transcode_profiles.");
        const QString preset = installed.value(profileId), extension = profileExtension(preset), kind = profileKind(preset);
        if (extension.isEmpty() || !newOutput(arguments.value(k("path")), path) || !path.endsWith(extension, Qt::CaseInsensitive))
            return reject("INVALID_PATH", "Use a NEW absolute local output path with the selected native profile extension and an existing writable parent.");
        const QString resource = source->url();
        if (!QFileInfo(resource).isFile() || QFileInfo(resource).canonicalFilePath().isEmpty() ||
            QDir::cleanPath(resource).compare(path, Qt::CaseInsensitive) == 0)
            return reject("INVALID_MEDIA", "Native transcoding requires a ready local file source and a different NEW output file.");
        const bool audioProfile = kind == k("audio");
        if ((audioProfile && !source->hasAudio()) || (!audioProfile && !source->hasVideo()) || (kind != k("audio") && kind != k("video") && kind != k("av")))
            return reject("STREAM_MISMATCH", "The installed native profile media kind must match an available source stream.");
        bool importResult = true, replaceSource = false;
        if ((arguments.contains(k("importResult")) && !arguments.value(k("importResult")).isBool()) ||
            (arguments.contains(k("replaceSource")) && !arguments.value(k("replaceSource")).isBool()))
            return reject("INVALID_ARGUMENT", "Import and replacement flags must be boolean.");
        importResult = arguments.value(k("importResult")).toBool(true);
        replaceSource = arguments.value(k("replaceSource")).toBool(false);
        int in = -1, out = -1;
        if (arguments.contains(k("inFrame")) != arguments.contains(k("outFrame")) ||
            (arguments.contains(k("inFrame")) && (!Mcp::integer(arguments, k("inFrame"), 0, MaxFrame, in) ||
                                                  !Mcp::integer(arguments, k("outFrame"), in, MaxFrame, out) || out >= source->getFramePlaytime())))
            return reject("INVALID_SOURCE_RANGE", "Supply a paired inclusive inFrame/outFrame range within the native source.");
        if (replaceSource && (in >= 0 || (source->hasAudio() && kind == k("video")) || (source->hasVideo() && kind == k("audio"))))
            return reject("INVALID_ARGUMENT", "Relinking requires a full-range conversion that preserves all source media kinds.");
        auto fps = pCore->getProjectFpsInfo();
        if (arguments.contains(k("fpsNumerator")) != arguments.contains(k("fpsDenominator")) ||
            (arguments.contains(k("fpsNumerator")) &&
             (!Mcp::integer(arguments, k("fpsNumerator"), 1, 240000, fps.first) || !Mcp::integer(arguments, k("fpsDenominator"), 1, 100000, fps.second))) ||
            double(fps.first) / fps.second < 1 || double(fps.first) / fps.second > 240)
            return reject("INVALID_ARGUMENT", "Use a paired target frame rate between 1 and 240 frames per second.");
        const ObjectId nativeOwner(KdenliveObjectType::BinClip, source->clipId().toInt(), QUuid());
        if (pCore->taskManager.hasPendingJob(nativeOwner, AbstractTask::TRANSCODEJOB))
            return reject("BUSY", "A native transcode is already running for this source.");
        if (!QFileInfo(KdenliveSettings::ffmpegpath()).isFile())
            return reject("TRANSCODER_UNAVAILABLE", "The application's configured native FFmpeg executable is unavailable.");
        if (!service->room()) return reject("BUSY", "This MCP server already owns 64 unfinished jobs.");
        if (importResult && !replaceSource && pCore->projectItemModel()->getAllClipIds().size() >= 511)
            return reject("LIMIT", "No room remains for the imported transcoding result.");
        QString params = preset.section(QLatin1Char(';'), 0, -2), pre = k("-noautorotate");
        if (params.contains(k(" -i "))) {
            pre = params.section(k(" -i "), 0, 0) + k(" -noautorotate");
            params = params.section(k(" -i "), 1);
        }
        auto pieces = params.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (!source->hasAudio() && kind == k("av")) {
            for (const auto &option : {k("-codec:a"), k("-c:a"), k("-ab"), k("-b:a")}) {
                const int index = pieces.indexOf(option);
                if (index >= 0) {
                    if (index + 1 < pieces.size()) pieces.removeAt(index + 1);
                    pieces.removeAt(index);
                }
            }
            pieces.insert(qMax(0, int(pieces.size()) - 1), k("-an"));
        }
        if (!audioProfile) {
            int index = pieces.indexOf(k("-vf"));
            if (index < 0) index = pieces.indexOf(k("-filter:v"));
            if (index < 0) index = pieces.indexOf(k("-f:v"));
            const QString filter = QStringLiteral("fps=fps=%1/%2").arg(fps.first).arg(fps.second);
            if (index < 0) {
                pieces.insert(qMax(0, int(pieces.size()) - 1), k("-filter:v"));
                pieces.insert(qMax(0, int(pieces.size()) - 1), filter);
            } else if (index + 1 < pieces.size())
                pieces[index + 1] += QLatin1Char(',') + filter;
            else
                return reject("INVALID_PROFILE", "The installed profile contains an incomplete native video filter option.");
        }
        params = pieces.join(QLatin1Char(' '));
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto document = QPointer<KdenliveDoc>(pCore->currentDoc());
        const std::weak_ptr<ProjectClip> weakSource = source;
        const QString binId = source->clipId();
        const int oldLength = source->getFramePlaytime();
        const QString folderId = source->parent() ? source->parent()->clipId() : pCore->projectItemModel()->getRootFolder()->clipId();
        service->jobs[id] = QJsonObject{{k("jobId"), id},
                                        {k("binId"), binId},
                                        {k("profileId"), profileId},
                                        {k("path"), path},
                                        {k("status"), k("queued")},
                                        {k("progress"), 0},
                                        {k("nativeFinished"), false},
                                        {k("imported"), false},
                                        {k("replaceSource"), replaceSource},
                                        {k("fpsNumerator"), fps.first},
                                        {k("fpsDenominator"), fps.second}};
        TranscodeSeek::TranscodeInfo info{resource, source->clipType(), fps, source->getProducerProperty(k("meta.media.video.codec.name"))};
        auto *task = new TranscodeTask(nativeOwner, QString(), pre, params, info, in, out, false, nullptr, false, path, false);
        service->tasks[id] = task;
        QObject::connect(
            task, &TranscodeTask::transcodeProgress, service,
            [service, id](int progress) {
                auto job = service->jobs.value(id);
                job.insert(k("progress"), qBound(0, progress, 99));
                if (job.value(k("status")) != k("cancelling")) job.insert(k("status"), k("running"));
                service->jobs[id] = job;
            },
            Qt::QueuedConnection);
        QObject::connect(
            task, &TranscodeTask::transcodeFinished, service,
            [service, id, document, weakSource, resource, binId, folderId, importResult, replaceSource,
             oldLength](bool success, bool cancelled, const QString &output, const QString &error) {
                auto job = service->jobs.value(id);
                job.insert(k("nativeFinished"), true);
                if (!success) {
                    job.insert(k("status"), cancelled ? k("cancelled") : k("failed"));
                    if (!error.isEmpty()) job.insert(k("error"), error.left(8192));
                    service->jobs[id] = job;
                    return;
                }
                job.insert(k("progress"), 100);
                job.insert(k("path"), output);
                Mlt::Producer decoded(pCore->getProjectProfile(), output.toUtf8().constData());
                if (!decoded.is_valid() || decoded.get_length() <= 0 || QFileInfo(output).size() <= 0) {
                    job.insert(k("status"), k("failed"));
                    job.insert(k("error"), k("Published native output could not be decoded; the file was preserved."));
                    service->jobs[id] = job;
                    return;
                }
                job.insert(k("nativeVerified"), true);
                job.insert(k("durationFrames"), decoded.get_length());
                job.insert(k("variableFrameRate"), decoded.get_int("meta.media.variable_frame_rate") != 0);
                const auto original = weakSource.lock();
                const bool sameProject = document && pCore->currentDoc() == document && original && pCore->projectItemModel()->hasClip(binId) &&
                                         pCore->projectItemModel()->getClipByBinID(binId) == original && original->statusReady() && original->url() == resource;
                if (!importResult && !replaceSource) {
                    job.insert(k("status"), k("completed"));
                    service->jobs[id] = job;
                    return;
                }
                if (!sameProject) {
                    job.insert(k("status"), k("completed_detached"));
                    job.insert(k("error"), k("Project or source changed during conversion; output was preserved without changing the current project."));
                    service->jobs[id] = job;
                    return;
                }
                if (replaceSource) {
                    if (decoded.get_length() < oldLength) {
                        job.insert(k("status"), k("failed"));
                        job.insert(k("error"), k("Native output is shorter than the original bin source; relinking was refused and the output was preserved."));
                        service->jobs[id] = job;
                        return;
                    }
                    QMap<QString, QString> changes{{k("resource"), output},
                                                   {k("kdenlive:originalurl"), output},
                                                   {k("kdenlive:proxy"), QString()},
                                                   {k("_fullreload"), k("1")},
                                                   {k("_wait_for_transcode"), QString()}};
                    QMap<QString, QString> previous;
                    for (auto it = changes.cbegin(); it != changes.cend(); ++it)
                        previous[it.key()] = original->getProducerProperty(it.key());
                    auto apply = [original](const QMap<QString, QString> &props) {
                        original->setProperties(props);
                        return true;
                    };
                    apply(changes);
                    pCore->pushUndo([apply, previous] { return apply(previous); }, [apply, changes] { return apply(changes); },
                                    k("MCP relink transcoded source"));
                    service->loadedSources[id] = original;
                    job.insert(k("outputBinId"), binId);
                    job.insert(k("status"), k("loading"));
                } else {
                    Fun undo = [] { return true; }, redo = [] { return true; };
                    const QString parent =
                        pCore->projectItemModel()->getFolderByBinId(folderId) ? folderId : pCore->projectItemModel()->getRootFolder()->clipId();
                    const QString imported =
                        ClipCreator::createClipFromFile(output, parent, pCore->projectItemModel(), undo, redo, [](const QString &) {}, false, true);
                    if (imported.isEmpty() || imported == k("-1") || !pCore->projectItemModel()->hasClip(imported)) {
                        undo();
                        job.insert(k("status"), k("failed"));
                        job.insert(k("error"), k("Native import failed; the published output remains available."));
                    } else {
                        pCore->pushUndo(undo, redo, k("MCP import transcoded output"));
                        service->loadedSources[id] = pCore->projectItemModel()->getClipByBinID(imported);
                        job.insert(k("outputBinId"), imported);
                        job.insert(k("status"), k("loading"));
                    }
                }
                service->jobs[id] = job;
            },
            Qt::QueuedConnection);
        pCore->taskManager.startTask(nativeOwner.itemId, task);
        return finish({{k("job"), service->jobs.value(id)}});
    }
    return reject("NOT_IMPLEMENTED", "Known media tool has no native handler.");
}
} // namespace McpMedia
