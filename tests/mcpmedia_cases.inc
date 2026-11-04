
/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Included by mcptest.cpp; these cases decode real native PCM and native transcode output.
#include "kdenlivesettings.h"
#include "mcp/mcpmedia.h"
#include <QCryptographicHash>
#include <QApplication>
#include <QProcess>
#include <QScopeGuard>
#include "jobs/transcodetask.h"
#include "transitions/transitionsrepository.hpp"
#include <mlt++/MltProducer.h>
namespace {
QJsonObject mediaFields(McpProject &project, const QJsonObject &fields)
{
    auto arguments = project.expected();
    for (auto it = fields.begin(); it != fields.end(); ++it) arguments.insert(it.key(), it.value());
    return arguments;
}
struct MediaFfmpeg
{
    QString previous = KdenliveSettings::ffmpegpath();
    QString path;
    MediaFfmpeg()
    {
        path = QFileInfo(previous).isFile() ? previous : QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (path.isEmpty()) path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ffmpeg.exe"));
        REQUIRE(QFileInfo(path).isFile());
        KdenliveSettings::setFfmpegpath(path);
    }
    ~MediaFfmpeg() { KdenliveSettings::setFfmpegpath(previous); }
};
void mediaProcess(const QString &executable, const QStringList &arguments)
{
    QProcess process;
    process.start(executable, arguments);
    REQUIRE(process.waitForStarted(3000));
    if (!process.waitForFinished(30000)) { process.kill(); process.waitForFinished(3000); FAIL("Media fixture process exceeded its bounded deadline"); }
    INFO(process.readAllStandardError().constData());
    REQUIRE(process.exitStatus() == QProcess::NormalExit);
    REQUIRE(process.exitCode() == 0);
}
QString multistreamFixture(const QTemporaryDir &directory, const QString &ffmpeg, bool variable = false)
{
    const QString path = directory.filePath(variable ? QStringLiteral("variable.mp4") : QStringLiteral("multistream.mkv"));
    QStringList args{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                     QStringLiteral("color=c=red:s=32x32:r=25:d=2"), QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                     QStringLiteral("aevalsrc=0.25*sin(2*PI*440*t)|0.125*sin(2*PI*660*t):s=48000:d=2"),
                     QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                     QStringLiteral("aevalsrc=0.125*sin(2*PI*880*t)|0.0625*sin(2*PI*1100*t):s=48000:d=2"),
                     QStringLiteral("-map"), QStringLiteral("0:v"), QStringLiteral("-map"), QStringLiteral("1:a"), QStringLiteral("-map"), QStringLiteral("2:a"),
                     QStringLiteral("-c:v"), variable ? QStringLiteral("libx264") : QStringLiteral("ffv1"),
                     QStringLiteral("-c:a"), variable ? QStringLiteral("aac") : QStringLiteral("pcm_s16le")};
    if (variable) {
        // Keep the alternating 50/30 ms PTS intervals through the encoder.
        // Merely changing filter time_base lets the default encoder time_base
        // quantize them back to constant 40 ms frames.
        args << QStringLiteral("-vf") << QStringLiteral("settb=1/1000,setpts=40*N+10*mod(N\\,2)")
             << QStringLiteral("-enc_time_base") << QStringLiteral("1/1000") << QStringLiteral("-video_track_timescale") << QStringLiteral("1000")
             << QStringLiteral("-fps_mode") << QStringLiteral("vfr") << QStringLiteral("-g") << QStringLiteral("1")
             << QStringLiteral("-bf") << QStringLiteral("0") << QStringLiteral("-preset") << QStringLiteral("ultrafast");
    }
    args << QStringLiteral("-n") << path;
    mediaProcess(ffmpeg, args);
    REQUIRE(QFileInfo(path).size() > 0);
    return path;
}
QByteArray mediaHash(const QString &path)
{
    QFile file(path); REQUIRE(file.open(QIODevice::ReadOnly));
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
QVector<float> nativeClipAudio(McpProject &project, int clipId)
{
    const auto producer = project.timeline->getClipProducer(clipId);
    REQUIRE(producer); REQUIRE(producer->is_valid());
    producer->seek(2);
    std::unique_ptr<Mlt::Frame> frame(producer->get_frame());
    REQUIRE(frame);
    mlt_audio_format format = mlt_audio_f32le;
    int frequency = 48000, channels = 2, samples = 1920;
    const auto *audio = static_cast<float *>(frame->get_audio(format, frequency, channels, samples));
    REQUIRE(audio); REQUIRE(format == mlt_audio_f32le); REQUIRE(frequency == 48000); REQUIRE(channels == 2); REQUIRE(samples >= 1000);
    return QVector<float>(audio, audio + samples * channels);
}
double nativeTone(const QVector<float> &samples, double hz, int channel = 0)
{
    double real = 0, imaginary = 0;
    for (int i = 0; i < samples.size() / 2; ++i) {
        const double phase = 6.283185307179586 * hz * i / 48000.0;
        real += samples.at(i * 2 + channel) * std::cos(phase); imaginary += samples.at(i * 2 + channel) * std::sin(phase);
    }
    return std::hypot(real, imaginary) / (samples.size() / 2);
}
QJsonObject nativeTranscodeTerminal(McpProject &project, const QString &id)
{
    QElapsedTimer deadline; deadline.start();
    while (deadline.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const auto job = project.success(QStringLiteral("transcode_status"), {{QStringLiteral("jobId"), id}}).value(QStringLiteral("job")).toObject();
        const auto status = job.value(QStringLiteral("status")).toString();
        if (status != QStringLiteral("queued") && status != QStringLiteral("running") && status != QStringLiteral("loading") && status != QStringLiteral("cancelling")) {
            INFO(QJsonDocument(job).toJson(QJsonDocument::Compact).constData());
            return job;
        }
        QThread::msleep(2);
    }
    FAIL("Native transcode did not reach a terminal state within its bounded deadline");
    return {};
}
QJsonObject explicitMediaArguments(McpProject &project, const QString &binId, int stream, int track, int start = 5)
{
    return mediaFields(project, {{QStringLiteral("binId"), binId}, {QStringLiteral("startFrame"), start}, {QStringLiteral("sourceInFrame"), 3},
                                 {QStringLiteral("durationFrames"), 12},
                                 {QStringLiteral("audio"), QJsonArray{QJsonObject{{QStringLiteral("streamId"), stream}, {QStringLiteral("trackId"), track}}}}});
}
}
TEST_CASE("MCP actual multistream IDs select distinct native decoded audio in one undo transaction", "[MCP][MCPMediaStreams]")
{
    MediaFfmpeg ffmpeg;
    QTemporaryDir directory; REQUIRE(directory.isValid());
    const auto path = multistreamFixture(directory, ffmpeg.path);
    McpProject project; project.open();
    const QString bin = importFixture(project, path);
    const auto source = project.success(QStringLiteral("list_media_streams"), {{QStringLiteral("binId"), bin}}).value(QStringLiteral("source")).toObject();
    const auto audio = source.value(QStringLiteral("audioStreams")).toArray();
    REQUIRE(audio.size() == 2); REQUIRE(source.value(QStringLiteral("videoStreams")).toArray().size() == 1);
    const int first = audio.at(0).toObject().value(QStringLiteral("id")).toInt(-1);
    const int second = audio.at(1).toObject().value(QStringLiteral("id")).toInt(-1);
    REQUIRE(first >= 0); REQUIRE(second >= 0); REQUIRE(first != second);
    CHECK(audio.at(0).toObject().value(QStringLiteral("channels")).toInt() == 2);
    CHECK(audio.at(1).toObject().value(QStringLiteral("channels")).toInt() == 2);
    CHECK(audio.at(0).toObject().value(QStringLiteral("ffmpegAudioIndex")).toInt() == 0);
    CHECK(audio.at(1).toObject().value(QStringLiteral("ffmpegAudioIndex")).toInt() == 1);
    const int otherTrack = project.success(QStringLiteral("add_track"), mediaFields(project, {{QStringLiteral("position"), 0}, {QStringLiteral("audio"), true}}))
                               .value(QStringLiteral("trackId")).toInt(-1);
    REQUIRE(project.timeline->isAudioTrack(otherTrack));
    auto args = explicitMediaArguments(project, bin, first, project.audioTrack);
    args[QStringLiteral("audio")] = QJsonArray{QJsonObject{{QStringLiteral("streamId"), first}, {QStringLiteral("trackId"), project.audioTrack}},
                                               QJsonObject{{QStringLiteral("streamId"), second}, {QStringLiteral("trackId"), otherTrack}}};
    args[QStringLiteral("videoTrackId")] = project.videoTracks.at(0);
    const int before = project.undoStack->index();
    const auto inserted = project.success(QStringLiteral("insert_media_streams"), args);
    const auto items = inserted.value(QStringLiteral("items")).toArray();
    REQUIRE(items.size() == 3); CHECK(project.undoStack->index() == before + 1);
    const int firstClip = items.at(1).toObject().value(QStringLiteral("clipId")).toInt(-1);
    const int secondClip = items.at(2).toObject().value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(project.timeline->isClip(firstClip)); REQUIRE(project.timeline->isClip(secondClip));
    CHECK(project.timeline->getClipTrackId(firstClip) == project.audioTrack);
    CHECK(project.timeline->getClipTrackId(secondClip) == otherTrack);
    CHECK(project.timeline->getGroupElements(firstClip).size() == 3);
    CHECK(project.timeline->getClipInOut(firstClip) == std::make_pair(3, 14));
    CHECK(project.timeline->getClipInOut(secondClip) == std::make_pair(3, 14));
    const auto firstPcm = nativeClipAudio(project, firstClip), secondPcm = nativeClipAudio(project, secondClip);
    CHECK(nativeTone(firstPcm, 440) > nativeTone(firstPcm, 880) * 10);
    CHECK(nativeTone(secondPcm, 880) > nativeTone(secondPcm, 440) * 10);
    CHECK(nativeTone(firstPcm, 660, 1) > nativeTone(firstPcm, 1100, 1) * 10);
    CHECK(nativeTone(secondPcm, 1100, 1) > nativeTone(secondPcm, 660, 1) * 10);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getClipsCount() == 0); CHECK(project.undoStack->index() == before);
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(project.timeline->getClipsCount() == 3);
    CHECK(nativeTone(nativeClipAudio(project, secondClip), 880) > nativeTone(nativeClipAudio(project, secondClip), 440) * 10);
    project.rejectWithoutChange(QStringLiteral("insert_media_streams"), explicitMediaArguments(project, bin, first, project.audioTrack));
    project.rejectWithoutChange(QStringLiteral("insert_media_streams"), explicitMediaArguments(project, bin, 127, project.audioTrack, 30));
    project.rejectWithoutChange(QStringLiteral("insert_media_streams"), explicitMediaArguments(project, bin, first, project.videoTracks.at(1), 30));
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP source audio routing and parameter changes restore native PCM through undo", "[MCP][MCPMediaStreams]")
{
    MediaFfmpeg ffmpeg; QTemporaryDir directory; REQUIRE(directory.isValid());
    const auto path = multistreamFixture(directory, ffmpeg.path);
    McpProject project; project.open(); const auto bin = importFixture(project, path);
    const auto streams = project.success(QStringLiteral("list_media_streams"), {{QStringLiteral("binId"), bin}})
                             .value(QStringLiteral("source")).toObject().value(QStringLiteral("audioStreams")).toArray();
    REQUIRE(streams.size() == 2);
    const int stream = streams.first().toObject().value(QStringLiteral("id")).toInt(-1);
    const int second = streams.last().toObject().value(QStringLiteral("id")).toInt(-1);
    const auto result = project.success(QStringLiteral("insert_media_streams"), explicitMediaArguments(project, bin, stream, project.audioTrack));
    const int clip = result.value(QStringLiteral("items")).toArray().first().toObject().value(QStringLiteral("clipId")).toInt(-1);
    const auto baseline = nativeClipAudio(project, clip); const double base = nativeTone(baseline, 440);
    REQUIRE(base > 0.05);
    project.success(QStringLiteral("configure_source_audio"),
                    mediaFields(project, {{QStringLiteral("binId"), bin}, {QStringLiteral("streamId"), stream}, {QStringLiteral("gainDb"), -6.0}}));
    CHECK(nativeTone(nativeClipAudio(project, clip), 440) / base == Approx(std::pow(10.0, -6.0 / 20.0)).margin(0.01));
    project.success(QStringLiteral("configure_source_audio"),
                    mediaFields(project, {{QStringLiteral("binId"), bin}, {QStringLiteral("streamId"), stream}, {QStringLiteral("gainDb"), -12.0}}));
    CHECK(nativeTone(nativeClipAudio(project, clip), 440) / base == Approx(std::pow(10.0, -12.0 / 20.0)).margin(0.01));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(nativeTone(nativeClipAudio(project, clip), 440) / base == Approx(std::pow(10.0, -6.0 / 20.0)).margin(0.01));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(nativeTone(nativeClipAudio(project, clip), 440) / base == Approx(1.0).margin(0.01));
    project.success(QStringLiteral("configure_source_audio"),
                    mediaFields(project, {{QStringLiteral("binId"), bin}, {QStringLiteral("streamId"), stream},
                                          {QStringLiteral("copyChannel"), QStringLiteral("left_to_right")}, {QStringLiteral("name"), QStringLiteral("Dialogue")}}));
    const auto copied = nativeClipAudio(project, clip);
    CHECK(nativeTone(copied, 440, 1) / nativeTone(copied, 440, 0) == Approx(1.0).margin(0.01));
    CHECK(nativeTone(copied, 440, 1) > nativeTone(copied, 660, 1) * 10);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(nativeTone(nativeClipAudio(project, clip), 660, 1) > nativeTone(nativeClipAudio(project, clip), 440, 1) * 10);
    project.success(QStringLiteral("configure_source_streams"),
                    mediaFields(project, {{QStringLiteral("binId"), bin}, {QStringLiteral("audioStreams"), QJsonArray{second}}, {QStringLiteral("defaultAudioStream"), second}}));
    const auto selected = project.success(QStringLiteral("list_media_streams"), {{QStringLiteral("binId"), bin}}).value(QStringLiteral("source")).toObject();
    CHECK(selected.value(QStringLiteral("defaultAudioStream")).toInt(-1) == second);
    CHECK(selected.value(QStringLiteral("activeStreamProperty")).toString() == QString::number(second));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(nativeTone(nativeClipAudio(project, clip), 440) / base == Approx(1.0).margin(0.01));
    project.rejectWithoutChange(QStringLiteral("configure_source_audio"),
                                mediaFields(project, {{QStringLiteral("binId"), bin}, {QStringLiteral("streamId"), stream}, {QStringLiteral("gainDb"), 100}}));
    project.rejectWithoutChange(QStringLiteral("configure_source_streams"),
                                mediaFields(project, {{QStringLiteral("binId"), bin}, {QStringLiteral("audioStreams"), QJsonArray{127}}}));
    REQUIRE(project.timeline->checkConsistency());
}
TEST_CASE("MCP explicit stream ripple and overwrite preserve exact native ranges", "[MCP][MCPMediaStreams]")
{
    MediaFfmpeg ffmpeg; QTemporaryDir directory; REQUIRE(directory.isValid());
    const auto path = multistreamFixture(directory, ffmpeg.path);
    McpProject project; project.open(); const auto bin = importFixture(project, path);
    const auto streams = project.success(QStringLiteral("list_media_streams"), {{QStringLiteral("binId"), bin}})
                             .value(QStringLiteral("source")).toObject().value(QStringLiteral("audioStreams")).toArray();
    REQUIRE(streams.size() == 2);
    const int first = streams.first().toObject().value(QStringLiteral("id")).toInt(-1), second = streams.last().toObject().value(QStringLiteral("id")).toInt(-1);
    project.success(QStringLiteral("insert_media_streams"), explicitMediaArguments(project, bin, first, project.audioTrack));
    const int before = project.undoStack->index();
    auto arguments = explicitMediaArguments(project, bin, second, project.audioTrack, 10);
    arguments[QStringLiteral("durationFrames")] = 4;
    SECTION("Ripple splits and moves only the named track") {
        arguments[QStringLiteral("mode")] = QStringLiteral("insert");
        const auto inserted = project.success(QStringLiteral("insert_media_streams"), arguments).value(QStringLiteral("items")).toArray();
        REQUIRE(inserted.size() == 1);
        const int id = inserted.first().toObject().value(QStringLiteral("clipId")).toInt(-1);
        CHECK(project.timeline->getClipsCount() == 3);
        CHECK(project.timeline->getClipPosition(id) == 10); CHECK(project.timeline->getClipInOut(id) == std::make_pair(3, 6));
        CHECK(project.timeline->getClipByPosition(project.audioTrack, 14) >= 0);
    }
    SECTION("Overwrite lifts only the requested inclusive frames") {
        arguments[QStringLiteral("mode")] = QStringLiteral("overwrite");
        const auto inserted = project.success(QStringLiteral("insert_media_streams"), arguments).value(QStringLiteral("items")).toArray();
        REQUIRE(inserted.size() == 1);
        CHECK(project.timeline->getClipsCount() == 3);
        const int id = inserted.first().toObject().value(QStringLiteral("clipId")).toInt(-1);
        CHECK(project.timeline->getClipPosition(id) == 10); CHECK(project.timeline->getClipInOut(id) == std::make_pair(3, 6));
        CHECK(nativeTone(nativeClipAudio(project, id), 880) > nativeTone(nativeClipAudio(project, id), 440) * 10);
    }
    CHECK(project.undoStack->index() == before + 1);
    project.success(QStringLiteral("undo"), project.expected()); CHECK(project.timeline->getClipsCount() == 1);
    project.success(QStringLiteral("redo"), project.expected()); CHECK(project.timeline->getClipsCount() == 3);
    REQUIRE(project.timeline->checkConsistency());
}
TEST_CASE("MCP native transcoding publishes new decoded CFR output preserves sources and imports undoably", "[MCP][MCPTranscode]")
{
    MediaFfmpeg ffmpeg; QTemporaryDir directory; REQUIRE(directory.isValid());
    const auto path = multistreamFixture(directory, ffmpeg.path, true);
    const auto sourceHash = mediaHash(path);
    const QString sourceProbePath = QDir(QFileInfo(ffmpeg.path).absolutePath()).filePath(QStringLiteral("ffprobe") +
        (QFileInfo(ffmpeg.path).suffix().isEmpty() ? QString() : QLatin1Char('.') + QFileInfo(ffmpeg.path).suffix()));
    REQUIRE(QFileInfo(sourceProbePath).isFile());
    QProcess sourceProbe;
    sourceProbe.start(sourceProbePath, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QStringLiteral("v:0"),
        QStringLiteral("-show_entries"), QStringLiteral("frame=best_effort_timestamp_time"), QStringLiteral("-of"), QStringLiteral("json"), path});
    REQUIRE(sourceProbe.waitForFinished(5000)); REQUIRE(sourceProbe.exitCode() == 0);
    const auto inputFrames = QJsonDocument::fromJson(sourceProbe.readAllStandardOutput()).object().value(QStringLiteral("frames")).toArray();
    REQUIRE(inputFrames.size() > 20);
    double shortest = 1000.0, longest = 0.0;
    for (int index = 1; index < inputFrames.size(); ++index) {
        const double previous = inputFrames.at(index - 1).toObject().value(QStringLiteral("best_effort_timestamp_time")).toString().toDouble();
        const double current = inputFrames.at(index).toObject().value(QStringLiteral("best_effort_timestamp_time")).toString().toDouble();
        REQUIRE(current > previous);
        shortest = qMin(shortest, current - previous); longest = qMax(longest, current - previous);
    }
    // This is actual encoded timing evidence before Kdenlive imports the file.
    REQUIRE(shortest < 0.035); REQUIRE(longest > 0.045);
    McpProject project; project.open();
    const auto acceptedImport = project.success(QStringLiteral("import_media"), mediaFields(project, {{QStringLiteral("path"), path}}));
    const QString bin = acceptedImport.value(QStringLiteral("binId")).toString();
    const auto variableSource = pCore->projectItemModel()->getClipByBinID(bin);
    REQUIRE(variableSource);
    QElapsedTimer loading; loading.start();
    while (!variableSource->statusReady() && loading.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1);
    }
    REQUIRE(variableSource->statusReady());
    const auto nativeSource = project.success(QStringLiteral("list_media_streams"), {{QStringLiteral("binId"), bin}}).value(QStringLiteral("source")).toObject();
    CHECK(nativeSource.value(QStringLiteral("variableFrameRate")).toBool());
    CHECK(nativeSource.value(QStringLiteral("needsTranscode")).toBool());
    CHECK(project.binItem(bin).value(QStringLiteral("status")) == QStringLiteral("needs_transcode"));
    CHECK_FALSE(QApplication::activeModalWidget());
    const auto profiles = project.success(QStringLiteral("list_transcode_profiles")).value(QStringLiteral("profiles")).toArray();
    bool installed = false;
    for (const auto &value : profiles) if (value.toObject().value(QStringLiteral("id")) == QStringLiteral("Lossy x264 I frame only")) installed = true;
    REQUIRE(installed);
    const QString output = directory.filePath(QStringLiteral("constant.mp4"));
    const int before = project.undoStack->index();
    auto args = mediaFields(project, {{QStringLiteral("binId"), bin}, {QStringLiteral("profileId"), QStringLiteral("Lossy x264 I frame only")},
                                      {QStringLiteral("path"), output}});
    const auto accepted = project.success(QStringLiteral("start_transcode"), args).value(QStringLiteral("job")).toObject();
    const QString id = accepted.value(QStringLiteral("jobId")).toString(); REQUIRE_FALSE(id.isEmpty());
    const auto terminal = nativeTranscodeTerminal(project, id);
    CHECK(terminal.value(QStringLiteral("status")) == QStringLiteral("completed"));
    CHECK(terminal.value(QStringLiteral("nativeVerified")).toBool());
    CHECK(terminal.value(QStringLiteral("nativeFinished")).toBool());
    CHECK(terminal.value(QStringLiteral("progress")).toInt() == 100);
    CHECK(terminal.value(QStringLiteral("imported")).toBool());
    CHECK_FALSE(terminal.value(QStringLiteral("variableFrameRate")).toBool());
    REQUIRE(QFileInfo(output).size() > 0);
    CHECK(mediaHash(path) == sourceHash);
    CHECK(pCore->projectItemModel()->getClipByBinID(bin)->url() == path);
    CHECK(project.undoStack->index() == before + 1);
    const QString imported = terminal.value(QStringLiteral("outputBinId")).toString(); REQUIRE_FALSE(imported.isEmpty());
    CHECK(pCore->projectItemModel()->getClipByBinID(imported)->statusReady());
    const auto outputStreams = project.success(QStringLiteral("list_media_streams"), {{QStringLiteral("binId"), imported}}).value(QStringLiteral("source")).toObject();
    CHECK(outputStreams.value(QStringLiteral("audioStreams")).toArray().size() == 2);
    CHECK(outputStreams.value(QStringLiteral("videoStreams")).toArray().size() == 1);
    const QString ffprobe = QDir(QFileInfo(ffmpeg.path).absolutePath()).filePath(QStringLiteral("ffprobe") + (QFileInfo(ffmpeg.path).suffix().isEmpty() ? QString() : QLatin1Char('.') + QFileInfo(ffmpeg.path).suffix()));
    REQUIRE(QFileInfo(ffprobe).isFile());
    QProcess probe;
    probe.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                         QStringLiteral("-show_entries"), QStringLiteral("frame=best_effort_timestamp_time"), QStringLiteral("-of"), QStringLiteral("json"), output});
    REQUIRE(probe.waitForFinished(5000)); REQUIRE(probe.exitCode() == 0);
    const auto frames = QJsonDocument::fromJson(probe.readAllStandardOutput()).object().value(QStringLiteral("frames")).toArray();
    REQUIRE(frames.size() > 20);
    for (int i = 1; i < frames.size(); ++i) {
        const double now = frames.at(i).toObject().value(QStringLiteral("best_effort_timestamp_time")).toString().toDouble();
        const double previous = frames.at(i - 1).toObject().value(QStringLiteral("best_effort_timestamp_time")).toString().toDouble();
        CHECK(now - previous == Approx(0.04).margin(0.00001));
    }
    const auto outputHash = mediaHash(output);
    args[QStringLiteral("expectedState")] = project.token();
    project.rejectWithoutChange(QStringLiteral("start_transcode"), args);
    CHECK(mediaHash(output) == outputHash); CHECK(mediaHash(path) == sourceHash);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK_FALSE(pCore->projectItemModel()->hasClip(imported)); CHECK(QFileInfo(output).isFile());
    project.success(QStringLiteral("redo"), project.expected()); project.awaitMediaReady(imported);
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP native transcode errors and cancellation preserve every existing file", "[MCP][MCPTranscode]")
{
    MediaFfmpeg ffmpeg; QTemporaryDir directory; REQUIRE(directory.isValid());
    const auto path = multistreamFixture(directory, ffmpeg.path);
    McpProject project; project.open(); const auto bin = importFixture(project, path);
    const auto before = mediaHash(path);
    const QString output = directory.filePath(QStringLiteral("cancelled.mp4"));
    auto args = mediaFields(project, {{QStringLiteral("binId"), bin}, {QStringLiteral("profileId"), QStringLiteral("Lossy x264 I frame only")},
                                      {QStringLiteral("path"), output}, {QStringLiteral("importResult"), false}});
    SECTION("Owned cancellation returns promptly and waits for actual native termination") {
        const auto accepted = project.success(QStringLiteral("start_transcode"), args).value(QStringLiteral("job")).toObject();
        const QString id = accepted.value(QStringLiteral("jobId")).toString();
        REQUIRE_FALSE(id.isEmpty());
        QElapsedTimer latency; latency.start();
        project.success(QStringLiteral("cancel_transcode"), {{QStringLiteral("jobId"), id}});
        CHECK(latency.elapsed() < 200);
        const auto terminal = nativeTranscodeTerminal(project, id);
        REQUIRE(terminal.value(QStringLiteral("nativeFinished")).toBool());
        CHECK(terminal.value(QStringLiteral("status")) == QStringLiteral("cancelled"));
        CHECK_FALSE(QFileInfo(output).exists()); CHECK(mediaHash(path) == before);
        const auto unknown = project.call(QStringLiteral("cancel_transcode"), {{QStringLiteral("jobId"), QStringLiteral("not-owned")}});
        CHECK(unknown.value(QStringLiteral("isError")).toBool());
    }
    SECTION("A decoder failure returns native nonzero exit failure and never imports") {
        QFile corrupt(path); REQUIRE(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
        REQUIRE(corrupt.write("not valid video") == 15); corrupt.close();
        const auto corrupted = mediaHash(path); const int history = project.undoStack->index();
        const auto accepted = project.success(QStringLiteral("start_transcode"), args).value(QStringLiteral("job")).toObject();
        const auto terminal = nativeTranscodeTerminal(project, accepted.value(QStringLiteral("jobId")).toString());
        CHECK(terminal.value(QStringLiteral("status")) == QStringLiteral("failed"));
        CHECK_FALSE(terminal.value(QStringLiteral("error")).toString().isEmpty());
        CHECK_FALSE(QFileInfo(output).exists()); CHECK(mediaHash(path) == corrupted);
        CHECK(project.undoStack->index() == history);
    }
    SECTION("Existing destination bytes are unchanged on rejection") {
        QFile sentinel(output); REQUIRE(sentinel.open(QIODevice::WriteOnly));
        REQUIRE(sentinel.write("preserve me") == 11); sentinel.close();
        const auto bytes = mediaHash(output);
        project.rejectWithoutChange(QStringLiteral("start_transcode"), args);
        CHECK(mediaHash(output) == bytes); CHECK(mediaHash(path) == before);
    }
}

TEST_CASE("MCP source routing configured before producer creation is decoded natively", "[MCP][MCPMediaStreams]")
{
    MediaFfmpeg ffmpeg; QTemporaryDir directory; REQUIRE(directory.isValid());
    const auto path = multistreamFixture(directory, ffmpeg.path);
    McpProject project; project.open(); const auto bin = importFixture(project, path);
    const auto streams = project.success(QStringLiteral("list_media_streams"), {{QStringLiteral("binId"), bin}})
                             .value(QStringLiteral("source")).toObject().value(QStringLiteral("audioStreams")).toArray();
    REQUIRE(streams.size() == 2);
    const int stream = streams.first().toObject().value(QStringLiteral("id")).toInt(-1);
    project.success(QStringLiteral("configure_source_audio"),
                    mediaFields(project, {{QStringLiteral("binId"), bin}, {QStringLiteral("streamId"), stream}, {QStringLiteral("gainDb"), -6.0},
                                          {QStringLiteral("copyChannel"), QStringLiteral("left_to_right")}}));
    const auto inserted = project.success(QStringLiteral("insert_media_streams"), explicitMediaArguments(project, bin, stream, project.audioTrack));
    const int clip = inserted.value(QStringLiteral("items")).toArray().first().toObject().value(QStringLiteral("clipId")).toInt(-1);
    const auto pcm = nativeClipAudio(project, clip);
    // Input left amplitude is 0.25; the DFT coefficient is half the sine amplitude.
    CHECK(nativeTone(pcm, 440) == Approx(0.125 * std::pow(10.0, -6.0 / 20.0)).margin(0.003));
    CHECK(nativeTone(pcm, 440, 1) / nativeTone(pcm, 440) == Approx(1.0).margin(0.01));
    CHECK(nativeTone(pcm, 440, 1) > nativeTone(pcm, 660, 1) * 10);
    project.success(QStringLiteral("undo"), project.expected());
    project.success(QStringLiteral("redo"), project.expected());
    const auto restored = nativeClipAudio(project, clip);
    CHECK(nativeTone(restored, 440) == Approx(nativeTone(pcm, 440)).margin(0.001));
    CHECK(nativeTone(restored, 440, 1) == Approx(nativeTone(pcm, 440, 1)).margin(0.001));
}

namespace {
struct MediaQueuedState { QAtomicInt started{0}; QAtomicInt release{0}; QAtomicInt finished{0}; };
class MediaQueuedBlocker : public AbstractTask
{
public:
    MediaQueuedBlocker(const ObjectId &owner, const std::shared_ptr<MediaQueuedState> &state)
        : AbstractTask(owner, AbstractTask::TRANSCODEJOB, nullptr), state(state) {}
protected:
    void run() override {
        AbstractTaskDone completion(m_owner.itemId, this);
        QMutexLocker lock(&m_runMutex); m_running = true; m_progress = 0; state->started.storeRelease(1);
        QElapsedTimer deadline; deadline.start();
        while (!state->release.loadAcquire() && !m_isCanceled.loadAcquire() && deadline.elapsed() < 5000) QThread::msleep(1);
        m_progress = 100; state->finished.storeRelease(1);
    }
private:
    std::shared_ptr<MediaQueuedState> state;
};
struct MediaTranscodeConcurrency
{
    int previous = KdenliveSettings::proxythreads();
    MediaTranscodeConcurrency() { KdenliveSettings::setProxythreads(1); pCore->taskManager.updateConcurrency(); }
    ~MediaTranscodeConcurrency() { KdenliveSettings::setProxythreads(previous); pCore->taskManager.updateConcurrency(); }
};
}
TEST_CASE("MCP queued native transcode deletion reaches terminal cancellation", "[MCP][MCPTranscode][Cancellation]")
{
    MediaFfmpeg ffmpeg; QTemporaryDir directory; REQUIRE(directory.isValid());
    const auto path = multistreamFixture(directory, ffmpeg.path);
    McpProject project; project.open(); const auto bin = importFixture(project, path);
    MediaTranscodeConcurrency serial;
    auto state = std::make_shared<MediaQueuedState>();
    const auto release = qScopeGuard([state] { state->release.storeRelease(1); });
    const ObjectId blockerOwner(KdenliveObjectType::BinClip, 999998, QUuid());
    pCore->taskManager.startTask(blockerOwner.itemId, new MediaQueuedBlocker(blockerOwner, state));
    QElapsedTimer started; started.start();
    while (!state->started.loadAcquire() && started.elapsed() < 3000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    REQUIRE(state->started.loadAcquire());
    const QString output = directory.filePath(QStringLiteral("never-started.mp4"));
    const auto accepted = project.success(QStringLiteral("start_transcode"),
        mediaFields(project, {{QStringLiteral("binId"), bin}, {QStringLiteral("profileId"), QStringLiteral("Lossy x264 I frame only")},
                              {QStringLiteral("path"), output}, {QStringLiteral("importResult"), false}})).value(QStringLiteral("job")).toObject();
    const ObjectId sourceOwner(KdenliveObjectType::BinClip, bin.toInt(), QUuid());
    REQUIRE(pCore->taskManager.hasPendingJob(sourceOwner, AbstractTask::TRANSCODEJOB));
    CHECK(accepted.value(QStringLiteral("status")) == QStringLiteral("queued"));
    pCore->taskManager.discardJobs(sourceOwner, AbstractTask::TRANSCODEJOB);
    const auto terminal = nativeTranscodeTerminal(project, accepted.value(QStringLiteral("jobId")).toString());
    CHECK(terminal.value(QStringLiteral("status")) == QStringLiteral("cancelled"));
    CHECK(terminal.value(QStringLiteral("nativeFinished")).toBool());
    CHECK_FALSE(terminal.value(QStringLiteral("imported")).toBool());
    CHECK_FALSE(QFileInfo(output).exists());
    state->release.storeRelease(1); started.restart();
    while ((!state->finished.loadAcquire() || pCore->taskManager.hasPendingJob(blockerOwner)) && started.elapsed() < 3000) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    REQUIRE(state->finished.loadAcquire());
    CHECK_FALSE(pCore->taskManager.hasPendingJob(blockerOwner));
}
TEST_CASE("MCP explicit video placement can overlap a native composition", "[MCP][MCPMediaStreams]")
{
    McpProject project; project.open(); QTemporaryDir directory; REQUIRE(directory.isValid());
    const auto bin = importFixture(project, pngFixture(directory));
    const int upper = project.videoTracks.at(1), lower = project.videoTracks.at(0);
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(lower, 0, 30));
    QString transition;
    for (const auto &entry : TransitionsRepository::get()->getNames()) if (TransitionsRepository::get()->isComposition(entry.first)) { transition = entry.first; break; }
    REQUIRE_FALSE(transition.isEmpty());
    const auto added = project.success(QStringLiteral("edit_composition"),
        mediaFields(project, {{QStringLiteral("operation"), QStringLiteral("add")}, {QStringLiteral("transitionId"), transition},
                              {QStringLiteral("trackId"), upper}, {QStringLiteral("aTrackId"), lower},
                              {QStringLiteral("startFrame"), 0}, {QStringLiteral("durationFrames"), 25}}));
    const int composition = added.value(QStringLiteral("compositionId")).toInt(-1);
    REQUIRE(project.timeline->isComposition(composition));
    const auto inserted = project.success(QStringLiteral("insert_media_streams"),
        mediaFields(project, {{QStringLiteral("binId"), bin}, {QStringLiteral("videoTrackId"), upper}, {QStringLiteral("startFrame"), 5},
                              {QStringLiteral("sourceInFrame"), 0}, {QStringLiteral("durationFrames"), 12}}));
    const int clip = inserted.value(QStringLiteral("items")).toArray().first().toObject().value(QStringLiteral("clipId")).toInt(-1);
    CHECK(project.timeline->getClipTrackId(clip) == upper); CHECK(project.timeline->getClipPosition(clip) == 5);
    CHECK(project.timeline->getCompositionPosition(composition) == 0); CHECK(project.timeline->getCompositionPlaytime(composition) == 25);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK_FALSE(project.timeline->isClip(clip)); CHECK(project.timeline->isComposition(composition));
    project.success(QStringLiteral("redo"), project.expected()); CHECK(project.timeline->isClip(clip));
    REQUIRE(project.timeline->checkConsistency());
}
