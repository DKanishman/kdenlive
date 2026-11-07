/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "kdenlivesettings.h"
#include "mcp/mcpaudiomatch.h"
#include "mcp/mcpserver.h"
#include "mcp/mcpwire.h"
#include "test_utils.hpp"
#include <QDataStream>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

namespace {
QString key(const char *value)
{
    return QString::fromLatin1(value);
}
std::vector<qint64> amplitudes(int count)
{
    std::vector<qint64> values;
    quint32 seed = 4187;
    for (int i = 0; i < count; ++i) {
        seed = seed * 1664525u + 1013904223u;
        values.push_back(500 + (seed >> 8) % 14000);
    }
    return values;
}
QString wave(const QTemporaryDir &directory, const QString &name, int frames, int delay = 0, double gain = 1, bool silent = false, int period = 0)
{
    const auto values = amplitudes(frames + 100);
    QByteArray pcm;
    QDataStream samples(&pcm, QIODevice::WriteOnly);
    samples.setByteOrder(QDataStream::LittleEndian);
    for (int i = 0; i < frames; ++i) {
        const auto value = silent || i < delay ? 0 : qint16(values[period > 0 ? (i - delay) % period : i - delay] * gain);
        for (int sample = 0; sample < 1920; ++sample)
            samples << qint16(sample % 2 ? value : -value);
    }
    const QString path = directory.filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    QDataStream header(&file);
    header.setByteOrder(QDataStream::LittleEndian);
    REQUIRE(file.write("RIFF", 4) == 4);
    header << quint32(pcm.size() + 36);
    REQUIRE(file.write("WAVEfmt ", 8) == 8);
    header << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000) << quint16(2) << quint16(16);
    REQUIRE(file.write("data", 4) == 4);
    header << quint32(pcm.size());
    REQUIRE(file.write(pcm) == pcm.size());
    REQUIRE(header.status() == QDataStream::Ok);
    file.close();
    return path;
}
struct RestoreFfmpeg
{
    QString old{KdenliveSettings::ffmpegpath()};
    RestoreFfmpeg()
    {
        const auto path = QStandardPaths::findExecutable(key("ffmpeg"));
        REQUIRE(QFileInfo(path).isFile());
        KdenliveSettings::setFfmpegpath(path);
    }
    ~RestoreFfmpeg() { KdenliveSettings::setFfmpegpath(old); }
};
class AlignmentProject
{
public:
    AlignmentProject()
        : history(std::make_shared<DocUndoStack>(nullptr))
    {
        pCore->projectItemModel()->clean();
        REQUIRE(pCore->setCurrentProfile(key("dv_pal")));
        document = std::make_unique<KdenliveDoc>(history);
        pCore->projectManager()->testSetDocument(document.get());
        REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), false));
        timeline = document->getTimeline(document->uuid());
        REQUIRE(timeline);
        pCore->projectManager()->testSetActiveTimeline(timeline);
        document->loading = false;
        for (int position = 0; position < timeline->getTracksCount(); ++position) {
            const int track = timeline->getTrackIndexFromPosition(position);
            (timeline->isAudioTrack(track) ? audio : video).push_back(track);
        }
        REQUIRE(audio.size() >= 2);
        REQUIRE(video.size() >= 2);
        history->clear();
        const QJsonObject params{{key("protocolVersion"), key("2025-11-25")},
                                 {key("capabilities"), QJsonObject{}},
                                 {key("clientInfo"), QJsonObject{{key("name"), key("alignment-tests")}, {key("version"), key("1")}}}};
        const auto reply =
            server.handleRequest({{key("jsonrpc"), key("2.0")}, {key("id"), ++request}, {key("method"), key("initialize")}, {key("params"), params}});
        REQUIRE_FALSE(reply.contains(key("error")));
        CHECK(server.handleRequest({{key("jsonrpc"), key("2.0")}, {key("method"), key("notifications/initialized")}}).isEmpty());
    }
    ~AlignmentProject()
    {
        if (document && pCore->projectManager()->current() == document.get()) pCore->projectManager()->closeCurrentDocument(false, false);
    }
    QJsonObject call(const char *name, QJsonObject args = {})
    {
        const auto response = server.handleRequest({{key("jsonrpc"), key("2.0")},
                                                    {key("id"), ++request},
                                                    {key("method"), key("tools/call")},
                                                    {key("params"), QJsonObject{{key("name"), key(name)}, {key("arguments"), args}}}});
        INFO(QJsonDocument(response).toJson(QJsonDocument::Compact).constData());
        REQUIRE_FALSE(response.contains(key("error")));
        return response.value(key("result")).toObject();
    }
    QJsonObject success(const char *name, QJsonObject args = {})
    {
        const auto result = call(name, args);
        INFO(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        REQUIRE_FALSE(result.value(key("isError")).toBool());
        const auto content = result.value(key("structuredContent")).toObject();
        REQUIRE(content.value(key("ok")).toBool());
        return content;
    }
    QJsonObject state() { return success("get_state").value(key("state")).toObject(); }
    QJsonObject mutation(QJsonObject args = {})
    {
        args.insert(key("expectedState"), state().value(key("expectedState")));
        return args;
    }
    void failure(const char *name, QJsonObject args, const char *code)
    {
        const auto before = state();
        const int index = history->index(), count = history->count();
        const auto result = call(name, args);
        REQUIRE(result.value(key("isError")).toBool());
        CHECK(result.value(key("structuredContent")).toObject().value(key("error")).toObject().value(key("code")).toString() == key(code));
        CHECK(state().value(key("expectedState")) == before.value(key("expectedState")));
        CHECK(history->index() == index);
        CHECK(history->count() == count);
        REQUIRE(timeline->checkConsistency());
    }
    QString importMedia(const QString &path)
    {
        const auto id = success("import_media", mutation({{key("path"), path}})).value(key("binId")).toString();
        REQUIRE_FALSE(id.isEmpty());
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 10000) {
            const auto source = pCore->projectItemModel()->getClipByBinID(id);
            if (source && source->statusReady()) return id;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        FAIL("Native import did not become ready");
        return {};
    }
    int insert(const QString &bin, int track, int position, int in, int frames)
    {
        return success("insert_media_clip", mutation({{key("binId"), bin},
                                                      {key("trackId"), track},
                                                      {key("startFrame"), position},
                                                      {key("sourceInFrame"), in},
                                                      {key("durationFrames"), frames},
                                                      {key("stream"), key("audio")}}))
            .value(key("clipId"))
            .toInt(-1);
    }
    QJsonObject member(int clip, bool withStream = true)
    {
        QJsonObject value{{key("clipId"), clip}};
        if (withStream) {
            const auto source = pCore->projectItemModel()->getClipByBinID(timeline->getClipBinId(clip));
            REQUIRE(source);
            REQUIRE_FALSE(source->audioStreams().isEmpty());
            value.insert(key("streamId"), source->audioStreams().firstKey());
        }
        return value;
    }
    QString start(int reference, const QJsonArray &targets, const QString &mode = key("audio"), bool groups = false, const QJsonObject &extra = {})
    {
        QJsonObject args{
            {key("mode"), mode}, {key("reference"), member(reference, mode == key("audio"))}, {key("targets"), targets}, {key("includeGroups"), groups}};
        for (auto it = extra.begin(); it != extra.end(); ++it)
            args.insert(it.key(), it.value());
        return success("start_alignment_job", mutation(args)).value(key("job")).toObject().value(key("jobId")).toString();
    }
    QJsonObject await(const QString &id)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            const auto job = success("alignment_job_status", {{key("jobId"), id}}).value(key("job")).toObject();
            const auto status = job.value(key("status")).toString();
            if (status == key("ready") || status == key("failed") || status == key("cancelled") || status == key("obsolete")) return job;
            QThread::msleep(1);
        }
        FAIL("Owned alignment job did not finish");
        return {};
    }
    std::shared_ptr<DocUndoStack> history;
    std::unique_ptr<KdenliveDoc> document;
    std::shared_ptr<TimelineItemModel> timeline;
    KdenliveMcpServer server;
    std::vector<int> audio, video;
    int request{0};
};
} // namespace

TEST_CASE("Audio matching finds signed short and FFT lags with gain and bounded overlap", "[MCP][Alignment][Math]")
{
    for (int size : {80, 512})
        for (int lag : {-11, 7}) {
            const auto original = amplitudes(size + 100);
            std::vector<qint64> reference(original.begin() + 20, original.begin() + 20 + size), target;
            for (int i = 0; i < size - 5; ++i)
                target.push_back(original[20 + i + lag] / 2);
            const auto result = McpAudioMatch::match(reference, target, 30, size / 2, std::make_shared<std::atomic_bool>(false));
            INFO(result.error.toStdString());
            REQUIRE(result.ok);
            CHECK(result.lag == lag);
            CHECK(result.correlation > .999);
            CHECK_FALSE(result.weak);
        }
}

TEST_CASE("FFT alignment preserves small nonrepeating changes on loud constant backgrounds", "[MCP][Alignment][Math]")
{
    auto source = amplitudes(4200);
    for (auto &value : source)
        value = 32000 + (value % 5) * 4;
    const std::vector<qint64> reference(source.begin(), source.begin() + 4096);
    std::vector<qint64> target;
    for (int i = 0; i < 4000; ++i)
        target.push_back(source[i + 13] / 2);
    const auto result = McpAudioMatch::match(reference, target, 50, 2000, std::make_shared<std::atomic_bool>(false));
    REQUIRE(result.ok);
    CHECK(result.lag == 13);
    CHECK(result.correlation > .999);
    CHECK_FALSE(result.weak);
}

TEST_CASE("Audio matching never treats silence periodic ambiguity or cancellation as confident evidence", "[MCP][Alignment][Math]")
{
    const std::vector<qint64> silence(100, 0);
    const auto values = amplitudes(100);
    auto stop = std::make_shared<std::atomic_bool>(false);
    CHECK_FALSE(McpAudioMatch::match(values, silence, 30, 40, stop).ok);
    std::vector<qint64> periodic;
    for (int i = 0; i < 512; ++i)
        periodic.push_back((i % 10) * 1000);
    const auto ambiguous = McpAudioMatch::match(periodic, periodic, 50, 200, stop);
    REQUIRE(ambiguous.ok);
    CHECK(ambiguous.weak);
    stop->store(true);
    const auto cancelled = McpAudioMatch::match(values, values, 30, 40, stop);
    CHECK_FALSE(cancelled.ok);
    CHECK(cancelled.cancelled);
}

TEST_CASE("Native MCP audio alignment decodes exact visible windows and applies one undoable group move", "[MCP][Alignment][Native]")
{
    RestoreFfmpeg ffmpeg;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    AlignmentProject project;
    const QString referenceBin = project.importMedia(wave(directory, key("reference.wav"), 512));
    const QString targetBin = project.importMedia(wave(directory, key("delayed.wav"), 512, 7, .5));
    const int reference = project.insert(referenceBin, project.audio[0], 50, 5, 300);
    const int target = project.insert(targetBin, project.audio[1], 90, 9, 260);
    const auto color =
        project
            .success(
                "add_color_clip",
                project.mutation({{key("trackId"), project.video[0]}, {key("startFrame"), 90}, {key("durationFrames"), 260}, {key("color"), key("#ff0000ff")}}))
            .value(key("clipId"))
            .toInt();
    project.success("group_items", project.mutation({{key("itemIds"), QJsonArray{target, color}}}));
    const auto before = project.state();
    const int beforeUndo = project.history->index();
    const QString grouping = project.timeline->groupsData();
    const auto members = project.timeline->getGroupElements(target);
    const QString id = project.start(reference, {project.member(target)}, key("audio"), true, {{key("maxShiftFrames"), 20}});
    const auto job = project.await(id);
    INFO(QJsonDocument(job).toJson(QJsonDocument::Compact).constData());
    REQUIRE(job.value(key("status")) == key("ready"));
    CHECK(project.state().value(key("expectedState")) == before.value(key("expectedState")));
    CHECK(project.history->index() == beforeUndo);
    const auto proposal = job.value(key("proposals")).toArray().first().toObject();
    CHECK(proposal.value(key("lagFrames")).toInt() == -3);
    CHECK(proposal.value(key("proposedFrame")).toInt() == 47);
    CHECK_FALSE(proposal.value(key("requiresAcknowledgement")).toBool());
    project.success("apply_alignment", project.mutation({{key("jobId"), id}}));
    CHECK(project.history->index() == beforeUndo + 1);
    CHECK(project.timeline->getClipPosition(target) == 47);
    CHECK(project.timeline->getClipPosition(color) == 47);
    CHECK(project.timeline->getClipPosition(reference) == 50);
    CHECK(project.timeline->getGroupElements(target) == members);
    CHECK(project.timeline->getGroupElements(color) == members);
    CHECK(QJsonDocument::fromJson(project.timeline->groupsData().toUtf8()).array().first().toObject().value(key("type")) == key("Normal"));
    CHECK(project.timeline->getClipIn(target) == 9);
    CHECK(project.timeline->getClipPlaytime(target) == 260);
    project.success("undo", project.mutation());
    CHECK(project.timeline->getClipPosition(target) == 90);
    CHECK(project.timeline->getClipPosition(color) == 90);
    CHECK(project.timeline->groupsData() == grouping);
    project.success("redo", project.mutation());
    CHECK(project.timeline->getClipPosition(target) == 47);
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("Native MCP timecode alignment accepts zero timecode and preserves atomic multi-target swaps", "[MCP][Alignment][Timecode]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    AlignmentProject project;
    const QString firstBin = project.importMedia(wave(directory, key("first.wav"), 30));
    const QString secondBin = project.importMedia(wave(directory, key("second.wav"), 30));
    const QString referenceBin = project.importMedia(wave(directory, key("reference.wav"), 30));
    pCore->projectItemModel()->getClipByBinID(referenceBin)->setProducerProperty(key("timecode"), key("0"));
    pCore->projectItemModel()->getClipByBinID(firstBin)->setProducerProperty(key("timecode"), key("4000"));
    pCore->projectItemModel()->getClipByBinID(secondBin)->setProducerProperty(key("timecode"), key("0"));
    const int reference = project.insert(referenceBin, project.audio[0], 100, 0, 10);
    const int first = project.insert(firstBin, project.audio[1], 100, 0, 10), second = project.insert(secondBin, project.audio[1], 200, 0, 10);
    const int undo = project.history->index();
    const QString id = project.start(reference, {project.member(first, false), project.member(second, false)}, key("timecode"));
    const auto job = project.await(id);
    REQUIRE(job.value(key("status")) == key("ready"));
    project.success("apply_alignment", project.mutation({{key("jobId"), id}}));
    CHECK(project.history->index() == undo + 1);
    CHECK(project.timeline->getClipPosition(first) == 200);
    CHECK(project.timeline->getClipPosition(second) == 100);
    project.success("undo", project.mutation());
    CHECK(project.timeline->getClipPosition(first) == 100);
    CHECK(project.timeline->getClipPosition(second) == 200);
    project.success("redo", project.mutation());
    CHECK(project.timeline->getClipPosition(first) == 200);
    CHECK(project.timeline->getClipPosition(second) == 100);
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("Native MCP alignment refuses silent invalid stale locked and colliding results without history changes", "[MCP][Alignment][Failure]")
{
    RestoreFfmpeg ffmpeg;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    AlignmentProject project;
    const QString referenceBin = project.importMedia(wave(directory, key("reference.wav"), 80));
    const QString targetBin = project.importMedia(wave(directory, key("target.wav"), 80));
    const int reference = project.insert(referenceBin, project.audio[0], 50, 0, 40), target = project.insert(targetBin, project.audio[1], 100, 0, 40);
    SECTION("Disabled targets remain disabled through alignment undo and redo")
    {
        project.success("set_clips_enabled", project.mutation({{key("clipIds"), QJsonArray{target}}, {key("enabled"), false}}));
        REQUIRE(project.timeline->getClipState(target).first == PlaylistState::Disabled);
        const QString id = project.start(reference, {project.member(target)});
        REQUIRE(project.await(id).value(key("status")) == key("ready"));
        project.success("apply_alignment", project.mutation({{key("jobId"), id}}));
        CHECK(project.timeline->getClipPosition(target) == 50);
        CHECK(project.timeline->getClipState(target).first == PlaylistState::Disabled);
        project.success("undo", project.mutation());
        CHECK(project.timeline->getClipPosition(target) == 100);
        CHECK(project.timeline->getClipState(target).first == PlaylistState::Disabled);
        project.success("redo", project.mutation());
        CHECK(project.timeline->getClipPosition(target) == 50);
        CHECK(project.timeline->getClipState(target).first == PlaylistState::Disabled);
    }
    SECTION("Already aligned clips create no redundant undo command")
    {
        const int same = project.insert(referenceBin, project.audio[1], 50, 0, 40);
        const int undo = project.history->index();
        const QString id = project.start(reference, {project.member(same)});
        REQUIRE(project.await(id).value(key("status")) == key("ready"));
        const auto applied = project.success("apply_alignment", project.mutation({{key("jobId"), id}})).value(key("job")).toObject();
        CHECK_FALSE(applied.value(key("timelineEdited")).toBool());
        CHECK(project.history->index() == undo);
    }
    SECTION("One group cannot accept incompatible member proposals")
    {
        const int other = project.insert(referenceBin, project.audio[1], 150, 0, 40);
        project.success("group_items", project.mutation({{key("itemIds"), QJsonArray{target, other}}}));
        const auto id = project.start(reference, {project.member(target), project.member(other)}, key("audio"), true);
        REQUIRE(project.await(id).value(key("status")) == key("ready"));
        project.failure("apply_alignment", project.mutation({{key("jobId"), id}}), "INCOMPATIBLE_GROUP_PROPOSALS");
    }
    SECTION("Invalid actual stream")
    {
        auto invalid = project.member(target);
        invalid.insert(key("streamId"), 999);
        project.failure("start_alignment_job",
                        project.mutation({{key("mode"), key("audio")},
                                          {key("reference"), project.member(reference)},
                                          {key("targets"), QJsonArray{invalid}},
                                          {key("includeGroups"), false}}),
                        "INVALID_SOURCE");
    }
    SECTION("Silence is terminal failure")
    {
        const auto silent = project.importMedia(wave(directory, key("silent.wav"), 80, 0, 1, true));
        const int clip = project.insert(silent, project.audio[1], 150, 0, 40);
        const int undo = project.history->index();
        const auto job = project.await(project.start(reference, {project.member(clip)}));
        CHECK(job.value(key("status")) == key("failed"));
        CHECK(project.history->index() == undo);
    }
    SECTION("AlignmentProject changed after analysis")
    {
        const QString id = project.start(reference, {project.member(target)});
        REQUIRE(project.await(id).value(key("status")) == key("ready"));
        project.success("rename_track", project.mutation({{key("trackId"), project.audio[1]}, {key("name"), key("Changed after analysis")}}));
        project.failure("apply_alignment", project.mutation({{key("jobId"), id}}), "OBSOLETE_ALIGNMENT");
    }
    SECTION("Changed source bytes invalidate a ready proposal")
    {
        const QString id = project.start(reference, {project.member(target)});
        REQUIRE(project.await(id).value(key("status")) == key("ready"));
        QFile source(directory.filePath(key("target.wav")));
        REQUIRE(source.open(QIODevice::ReadWrite));
        REQUIRE(source.seek(44));
        REQUIRE(source.write(QByteArray(100, 'x')) == 100);
        source.close();
        const auto jobs = project.success("alignment_job_status").value(key("jobs")).toArray();
        REQUIRE(jobs.size() == 1);
        CHECK(jobs.first().toObject().value(key("status")) == key("obsolete"));
        project.failure("apply_alignment", project.mutation({{key("jobId"), id}}), "OBSOLETE_ALIGNMENT");
    }
    SECTION("Same-bin trimmed source windows use the actual source offset")
    {
        const int same = project.insert(referenceBin, project.audio[1], 150, 7, 40);
        const QString id = project.start(reference, {project.member(same)});
        const auto job = project.await(id);
        REQUIRE(job.value(key("status")) == key("ready"));
        CHECK(job.value(key("proposals")).toArray().first().toObject().value(key("proposedFrame")).toInt() == 57);
    }
    SECTION("Already locked target track")
    {
        project.success("set_track_locked", project.mutation({{key("trackId"), project.audio[1]}, {key("locked"), true}}));
        const QString id = project.start(reference, {project.member(target)});
        REQUIRE(project.await(id).value(key("status")) == key("ready"));
        project.failure("apply_alignment", project.mutation({{key("jobId"), id}}), "TRACK_LOCKED");
    }
    SECTION("Collision is preflighted")
    {
        project.insert(targetBin, project.audio[1], 50, 0, 40);
        const QString id = project.start(reference, {project.member(target)});
        REQUIRE(project.await(id).value(key("status")) == key("ready"));
        project.failure("apply_alignment", project.mutation({{key("jobId"), id}}), "ALIGNMENT_COLLISION");
    }
}

TEST_CASE("Native MCP owned alignment cancellation survives a pending result receipt and document close", "[MCP][Alignment][Cancellation]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    AlignmentProject project;
    const QString bin = project.importMedia(wave(directory, key("timecode.wav"), 30));
    pCore->projectItemModel()->getClipByBinID(bin)->setProducerProperty(key("timecode"), key("0"));
    const int reference = project.insert(bin, project.audio[0], 10, 0, 10), target = project.insert(bin, project.audio[1], 20, 0, 10);
    const auto id = project.start(reference, {project.member(target, false)}, key("timecode"));
    const auto oversized = McpWire::toolResult({{key("ok"), true}, {key("text"), QString(1100000, QLatin1Char('a'))}});
    McpWire::boundRpcResponse(&project.server, {{key("jsonrpc"), key("2.0")}, {key("id"), 999}, {key("result"), oversized}}, key("alignment_job_status"),
                              false);
    pCore->projectManager()->closeCurrentDocument(false, false);
    const auto job = project.success("cancel_alignment_job", {{key("jobId"), id}}).value(key("job")).toObject();
    CHECK(job.value(key("status")) == key("cancelled"));
    CHECK_FALSE(job.value(key("decoderRunning")).toBool());
    CHECK_FALSE(job.value(key("workerRunning")).toBool());
    CHECK(project.timeline->getClipPosition(target) == 20);
}

TEST_CASE("Native MCP alignment confirms its owned decoder stopped after cancellation", "[MCP][Alignment][Cancellation]")
{
    RestoreFfmpeg restore;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    AlignmentProject project;
    const auto bin = project.importMedia(wave(directory, key("audio.wav"), 80));
    const int reference = project.insert(bin, project.audio[0], 50, 0, 40), target = project.insert(bin, project.audio[1], 100, 0, 40);
    const auto path = directory.filePath(key("owned-decoder.py"));
    QFile decoder(path);
    REQUIRE(decoder.open(QIODevice::WriteOnly));
    REQUIRE(decoder.write("#!/usr/bin/python3\nimport time\ntime.sleep(120)\n") > 0);
    decoder.close();
    REQUIRE(decoder.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    KdenliveSettings::setFfmpegpath(path);
    const int undo = project.history->index();
    const QString id = project.start(reference, {project.member(target)});
    QElapsedTimer timer;
    timer.start();
    QJsonObject running;
    while (timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        running = project.success("alignment_job_status", {{key("jobId"), id}}).value(key("job")).toObject();
        if (running.value(key("decoderProcessId")).toString().toLongLong() > 0) break;
        QThread::msleep(1);
    }
    REQUIRE(running.value(key("decoderProcessId")).toString().toLongLong() > 0);
    project.success("cancel_alignment_job", {{key("jobId"), id}});
    const auto stopped = project.await(id);
    CHECK(stopped.value(key("status")) == key("cancelled"));
    CHECK_FALSE(stopped.value(key("decoderRunning")).toBool());
    CHECK_FALSE(stopped.value(key("workerRunning")).toBool());
    CHECK(project.history->index() == undo);
    CHECK(project.timeline->getClipPosition(target) == 100);
}

#include "mcpalignment_av_cases.inc"
