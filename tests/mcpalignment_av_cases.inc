/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "doc/kthumb.h"
#include "timeline2/view/timelinecontroller.h"
#include <QThreadPool>
#include <mlt++/MltFrame.h>

namespace {
QString alignmentAvFixture(const QTemporaryDir &directory, const QString &name, const QString &audio, const QString &color)
{
    const auto output = directory.filePath(name);
    QProcess process;
    process.start(KdenliveSettings::ffmpegpath(), {key("-nostdin"),  key("-hide_banner"),
                                                   key("-v"),        key("error"),
                                                   key("-f"),        key("lavfi"),
                                                   key("-i"),        key("color=c=") + color + key(":s=32x32:r=25:d=12"),
                                                   key("-i"),        audio,
                                                   key("-map"),      key("0:v"),
                                                   key("-map"),      key("1:a"),
                                                   key("-c:v"),      key("ffv1"),
                                                   key("-c:a"),      key("copy"),
                                                   key("-shortest"), output});
    REQUIRE(process.waitForStarted(3000));
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(3000);
        FAIL("AV fixture encoder exceeded its deadline");
    }
    INFO(process.readAllStandardError().constData());
    REQUIRE(process.exitCode() == 0);
    return output;
}
std::pair<int, int> insertAlignmentAv(AlignmentProject &project, const QString &bin, int videoTrack, int audioTrack, int position, int in, int duration)
{
    const auto source = pCore->projectItemModel()->getClipByBinID(bin);
    REQUIRE(source);
    REQUIRE_FALSE(source->audioStreams().isEmpty());
    const auto result = project.success(
        "insert_media_streams",
        project.mutation({{key("binId"), bin},
                          {key("startFrame"), position},
                          {key("sourceInFrame"), in},
                          {key("durationFrames"), duration},
                          {key("videoTrackId"), videoTrack},
                          {key("audio"), QJsonArray{QJsonObject{{key("streamId"), source->audioStreams().firstKey()}, {key("trackId"), audioTrack}}}},
                          {key("group"), true}}));
    REQUIRE(result.value(key("groupId")).toInt() >= 0);
    std::pair<int, int> clips{-1, -1};
    for (const auto &value : result.value(key("items")).toArray()) {
        const auto item = value.toObject();
        (item.value(key("stream")) == key("audio") ? clips.second : clips.first) = item.value(key("clipId")).toInt();
    }
    REQUIRE(clips.first >= 0);
    REQUIRE(clips.second >= 0);
    CHECK(project.timeline->getGroupElements(clips.first).count(clips.second) == 1);
    return clips;
}
double decodedAlignmentAmplitude(Mlt::Producer &producer, int position)
{
    producer.seek(position);
    std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
    REQUIRE(frame);
    mlt_audio_format format = mlt_audio_f32le;
    int rate = 48000, channels = 2, samples = 1920;
    const auto *audio = static_cast<const float *>(frame->get_audio(format, rate, channels, samples));
    REQUIRE(audio);
    REQUIRE(format == mlt_audio_f32le);
    REQUIRE(rate == 48000);
    REQUIRE_FALSE(frame->get_int("test_audio"));
    REQUIRE(samples > 0);
    REQUIRE(channels > 0);
    double sum = 0;
    for (int sample = 0; sample < samples; ++sample)
        sum += std::abs(audio[sample * channels]);
    return sum / samples;
}
} // namespace

TEST_CASE("Native MCP alignment preserves AV split groups and exact audiovisual output through save and reload", "[MCP][Alignment][AV][Persistence]")
{
    RestoreFfmpeg ffmpeg;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    AlignmentProject project;
    const auto referencePath = alignmentAvFixture(directory, key("reference.mkv"), wave(directory, key("reference-audio.wav"), 300), key("red"));
    const auto targetPath = alignmentAvFixture(directory, key("target.mkv"), wave(directory, key("target-audio.wav"), 300, 9, .7), key("blue"));
    const auto referenceBin = project.importMedia(referencePath), targetBin = project.importMedia(targetPath);
    const auto reference = insertAlignmentAv(project, referenceBin, project.video[0], project.audio[0], 50, 4, 200);
    const auto target = insertAlignmentAv(project, targetBin, project.video[1], project.audio[1], 100, 8, 180);
    const QString groupsBefore = project.timeline->groupsData();
    const auto targetMembers = project.timeline->getGroupElements(target.first);
    const auto referenceMembers = project.timeline->getGroupElements(reference.first);
    auto invalid = project.mutation({{key("mode"), key("audio")},
                                     {key("reference"), project.member(reference.second)},
                                     {key("targets"), QJsonArray{project.member(target.second)}},
                                     {key("includeGroups"), false}});
    project.failure("start_alignment_job", invalid, "GROUP_ACKNOWLEDGEMENT_REQUIRED");
    const int before = project.history->index();
    const auto id =
        project.start(reference.second, {project.member(target.first), project.member(target.second)}, key("audio"), true, {{key("maxShiftFrames"), 20}});
    const auto job = project.await(id);
    INFO(QJsonDocument(job).toJson(QJsonDocument::Compact).constData());
    REQUIRE(job.value(key("status")) == key("ready"));
    REQUIRE(job.value(key("proposals")).toArray().size() == 2);
    const auto proposal = job.value(key("proposals")).toArray().first().toObject();
    CHECK(proposal.value(key("lagFrames")).toInt() == -5);
    CHECK(proposal.value(key("proposedFrame")).toInt() == 45);
    project.success("apply_alignment", project.mutation({{key("jobId"), id}}));
    CHECK(project.history->index() == before + 1);
    for (int item : {target.first, target.second}) {
        CHECK(project.timeline->getClipPosition(item) == 45);
        CHECK(project.timeline->getClipIn(item) == 8);
        CHECK(project.timeline->getClipPlaytime(item) == 180);
    }
    CHECK(project.timeline->getGroupElements(target.first) == targetMembers);
    CHECK(project.timeline->getGroupElements(reference.first) == referenceMembers);
    project.success("undo", project.mutation());
    CHECK(project.timeline->groupsData() == groupsBefore);
    project.success("redo", project.mutation());
    CHECK(project.timeline->getClipPosition(target.second) == 45);

    // Use the same native group/property preparation as normal project save;
    // only GUI window/layout handling is absent in this model-level fixture.
    project.document->storeGroups(project.timeline->uuid());
    pCore->projectItemModel()->saveDocumentProperties(project.document->documentProperties(true), {});
    const auto scene = pCore->projectManager()->projectSceneList(directory.path()).first;
    REQUIRE_FALSE(scene.isEmpty());
    const auto saved = directory.filePath(key("aligned.kdenlive"));
    QString error;
    REQUIRE(project.document->saveSceneList(saved, scene, false, &error, true));
    CHECK(error.isEmpty());
    Mlt::Producer reopened(pCore->getProjectProfile(), "xml", saved.toUtf8().constData());
    REQUIRE(reopened.is_valid());
    const auto early = KThumb::getFrame(reopened, 46, 32, 32), late = KThumb::getFrame(reopened, 240, 32, 32);
    REQUIRE_FALSE(early.isNull());
    REQUIRE_FALSE(late.isNull());
    CHECK(QColor(early.pixel(16, 16)).blue() > 220);
    CHECK(QColor(late.pixel(16, 16)).red() > 220);
    const auto values = amplitudes(400);
    const double lateAmplitude = decodedAlignmentAmplitude(reopened, 240), earlyAmplitude = decodedAlignmentAmplitude(reopened, 46),
                 mixedAmplitude = decodedAlignmentAmplitude(reopened, 60);
    REQUIRE(lateAmplitude > .001);
    const double scale = lateAmplitude / values[194];
    CHECK(earlyAmplitude / (int(values[0] * .7) * scale) == Approx(1).margin(.02));
    CHECK(mixedAmplitude / ((values[14] + int(values[14] * .7)) * scale) == Approx(1).margin(.02));
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("Native MCP timecode proposals become obsolete if embedded metadata changes", "[MCP][Alignment][Timecode][Stale]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    AlignmentProject project;
    const auto first = project.importMedia(wave(directory, key("first.wav"), 40)), second = project.importMedia(wave(directory, key("second.wav"), 40));
    pCore->projectItemModel()->getClipByBinID(first)->setProducerProperty(key("timecode"), key("0"));
    pCore->projectItemModel()->getClipByBinID(second)->setProducerProperty(key("timecode"), key("0"));
    const int reference = project.insert(first, project.audio[0], 50, 0, 20), target = project.insert(second, project.audio[1], 100, 0, 20);
    const auto id = project.start(reference, {project.member(target, false)}, key("timecode"));
    REQUIRE(project.await(id).value(key("status")) == key("ready"));
    pCore->projectItemModel()->getClipByBinID(second)->setProducerProperty(key("timecode"), key("4000"));
    project.failure("apply_alignment", project.mutation({{key("jobId"), id}}), "OBSOLETE_ALIGNMENT");
}

TEST_CASE("Native MCP ambiguous audio requires explicit acknowledgement before an undoable apply", "[MCP][Alignment][Ambiguity]")
{
    RestoreFfmpeg ffmpeg;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    AlignmentProject project;
    const auto bin = project.importMedia(wave(directory, key("periodic.wav"), 80, 0, 1, false, 8));
    const int reference = project.insert(bin, project.audio[0], 50, 0, 80), target = project.insert(bin, project.audio[1], 100, 0, 80);
    const auto id = project.start(reference, {project.member(target)}, key("audio"), false, {{key("maxShiftFrames"), 8}});
    const auto job = project.await(id);
    REQUIRE(job.value(key("status")) == key("ready"));
    CHECK(job.value(key("proposals")).toArray().first().toObject().value(key("requiresAcknowledgement")).toBool());
    project.failure("apply_alignment", project.mutation({{key("jobId"), id}}), "WEAK_ALIGNMENT_EVIDENCE");
    project.failure("apply_alignment", project.mutation({{key("jobId"), id}, {key("acknowledgeWeakEvidence"), key("true")}}), "INVALID_ARGUMENT");
    const int undo = project.history->index();
    project.success("apply_alignment", project.mutation({{key("jobId"), id}, {key("acknowledgeWeakEvidence"), true}}));
    CHECK(project.history->index() == undo + 1);
    CHECK(project.timeline->getClipPosition(target) == job.value(key("proposals")).toArray().first().toObject().value(key("proposedFrame")).toInt());
    project.success("undo", project.mutation());
    CHECK(project.timeline->getClipPosition(target) == 100);
}

TEST_CASE("Native MCP timecode alignment reads actual BWF zero and sample time references", "[MCP][Alignment][Timecode][BWF]")
{
    RestoreFfmpeg ffmpeg;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    AlignmentProject project;
    const auto source = wave(directory, key("source.wav"), 40);
    const auto encode = [&](const char *name, const char *timeReference) {
        const auto output = directory.filePath(key(name));
        QProcess process;
        process.start(KdenliveSettings::ffmpegpath(), {key("-nostdin"), key("-v"), key("error"), key("-i"), source, key("-c:a"), key("copy"),
                                                       key("-write_bext"), key("1"), key("-metadata"), key("time_reference=") + key(timeReference), output});
        REQUIRE(process.waitForStarted(3000));
        if (!process.waitForFinished(15000)) {
            process.kill();
            process.waitForFinished(3000);
            FAIL("BWF fixture encode exceeded deadline");
        }
        INFO(process.readAllStandardError().constData());
        REQUIRE(process.exitCode() == 0);
        return output;
    };
    const auto first = project.importMedia(encode("bwf-zero.wav", "0")), second = project.importMedia(encode("bwf-ten-seconds.wav", "480000"));
    const auto a = pCore->projectItemModel()->getClipByBinID(first), b = pCore->projectItemModel()->getClipByBinID(second);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->getStartTimecode() == 0);
    CHECK(b->getStartTimecode() == 250);
    const int reference = project.insert(first, project.audio[0], 100, 3, 10), target = project.insert(second, project.audio[1], 600, 9, 10);
    const auto id = project.start(reference, {project.member(target, false)}, key("timecode"));
    const auto job = project.await(id);
    REQUIRE(job.value(key("status")) == key("ready"));
    CHECK(job.value(key("proposals")).toArray().first().toObject().value(key("proposedFrame")).toInt() == 356);
    project.success("apply_alignment", project.mutation({{key("jobId"), id}}));
    CHECK(project.timeline->getClipPosition(target) == 356);
    project.success("undo", project.mutation());
    CHECK(project.timeline->getClipPosition(target) == 600);
}

TEST_CASE("Native GUI alignment completes or discards results across its reference lifecycle", "[MCP][Alignment][NativeGUI][Lifetime]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    AlignmentProject project;
    const auto first = project.importMedia(wave(directory, key("gui-reference.wav"), 80));
    const auto second = project.importMedia(wave(directory, key("gui-target.wav"), 80, 2));
    const int reference = project.insert(first, project.audio[0], 50, 0, 40);
    const int target = project.insert(second, project.audio[1], 100, 0, 40);
    TimelineController controller(nullptr);
    controller.setModel(project.timeline, false);
    controller.setAudioRef(reference);
    controller.alignAudio(target);
    int expectedPosition = 100;
    SECTION("A retained reference produces the decoded two-frame shift")
    {
        expectedPosition = 48;
    }
    SECTION("A deleted reference discards the completed result")
    {
        // Deliver completion after deleting the reference; source producers
        // remain owned and valid for decoding.
        project.success("delete_item", project.mutation({{key("itemId"), reference}}));
        REQUIRE_FALSE(project.timeline->isClip(reference));
    }
    SECTION("Closing the controller cancels its worker before releasing the model")
    {
        controller.prepareClose();
    }
    QElapsedTimer deadline;
    deadline.start();
    while (QThreadPool::globalInstance()->activeThreadCount() > 0 && deadline.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    REQUIRE(QThreadPool::globalInstance()->activeThreadCount() == 0);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    CHECK(project.timeline->getClipPosition(target) == expectedPosition);
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("Native GUI same-source alignment can move a clip to frame zero", "[MCP][Alignment][NativeGUI]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    AlignmentProject project;
    const auto source = project.importMedia(wave(directory, key("same-source.wav"), 80));
    const int reference = project.insert(source, project.audio[0], 0, 0, 40);
    const int target = project.insert(source, project.audio[1], 100, 0, 40);
    TimelineController controller(nullptr);
    controller.setModel(project.timeline, false);
    controller.setAudioRef(reference);
    controller.alignAudio(target);
    CHECK(project.timeline->getClipPosition(target) == 0);
    project.success("undo", project.mutation());
    CHECK(project.timeline->getClipPosition(target) == 100);
    controller.prepareClose();
    REQUIRE(project.timeline->checkConsistency());
}
