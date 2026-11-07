/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "doc/kthumb.h"

namespace {
struct MulticamFfmpeg
{
    QString old{KdenliveSettings::ffmpegpath()};
    MulticamFfmpeg()
    {
        const auto executable = QStandardPaths::findExecutable(key("ffmpeg"));
        REQUIRE_FALSE(executable.isEmpty());
        KdenliveSettings::setFfmpegpath(executable);
    }
    ~MulticamFfmpeg() { KdenliveSettings::setFfmpegpath(old); }
};
QString indexedCamera(const QTemporaryDir &directory, int angle)
{
    const auto output = directory.filePath(key("camera-%1.mkv").arg(angle));
    const auto audio = tone(directory, key("scratch-%1.wav").arg(angle), 160, 300 + angle * 200);
    QByteArray rgb;
    for (int frame = 0; frame < 160; ++frame) {
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 32; ++x) {
                if (y < 8) {
                    const char bit = frame & (1 << (x / 4)) ? char(240) : char(8);
                    rgb.append(bit);
                    rgb.append(bit);
                    rgb.append(bit);
                } else {
                    for (int channel = 0; channel < 3; ++channel)
                        rgb.append(channel == angle ? char(220) : char(16));
                }
            }
    }
    QProcess encoder;
    encoder.start(KdenliveSettings::ffmpegpath(), {key("-nostdin"),
                                                   key("-hide_banner"),
                                                   key("-v"),
                                                   key("error"),
                                                   key("-f"),
                                                   key("rawvideo"),
                                                   key("-pixel_format"),
                                                   key("rgb24"),
                                                   key("-video_size"),
                                                   key("32x24"),
                                                   key("-framerate"),
                                                   key("25"),
                                                   key("-i"),
                                                   key("pipe:0"),
                                                   key("-i"),
                                                   audio,
                                                   key("-map"),
                                                   key("0:v"),
                                                   key("-map"),
                                                   key("1:a"),
                                                   key("-c:v"),
                                                   key("ffv1"),
                                                   key("-c:a"),
                                                   key("copy"),
                                                   key("-shortest"),
                                                   output});
    REQUIRE(encoder.waitForStarted(3000));
    REQUIRE(encoder.write(rgb) == rgb.size());
    encoder.closeWriteChannel();
    if (!encoder.waitForFinished(30000)) {
        encoder.kill();
        encoder.waitForFinished(3000);
        FAIL("Indexed camera encoder exceeded its deadline");
    }
    INFO(encoder.readAllStandardError().constData());
    REQUIRE(encoder.exitStatus() == QProcess::NormalExit);
    REQUIRE(encoder.exitCode() == 0);
    return output;
}
std::pair<int, int> insertCamera(MulticamProject &project, const QString &source, int angle)
{
    const auto clip = pCore->projectItemModel()->getClipByBinID(source);
    REQUIRE(clip);
    REQUIRE_FALSE(clip->audioStreams().isEmpty());
    const auto created = project.success(
        "insert_media_streams",
        project.mutation({{key("binId"), source},
                          {key("videoTrackId"), project.video[angle]},
                          {key("audio"), QJsonArray{QJsonObject{{key("streamId"), clip->audioStreams().firstKey()}, {key("trackId"), project.audio[angle]}}}},
                          {key("group"), true},
                          {key("startFrame"), 20},
                          {key("sourceInFrame"), 10 + angle * 5},
                          {key("durationFrames"), 90}}));
    std::pair<int, int> ids{-1, -1};
    for (const auto &value : created.value(key("items")).toArray()) {
        const auto item = value.toObject();
        (item.value(key("stream")) == key("audio") ? ids.second : ids.first) = item.value(key("clipId")).toInt();
    }
    REQUIRE(ids.first >= 0);
    REQUIRE(ids.second >= 0);
    return ids;
}
QString saveMulticam(MulticamProject &project, const QTemporaryDir &directory, const QString &name)
{
    project.document->storeGroups(project.timeline->uuid());
    pCore->projectItemModel()->saveDocumentProperties(project.document->documentProperties(true), {});
    const auto scene = pCore->projectManager()->projectSceneList(directory.path()).first;
    REQUIRE_FALSE(scene.isEmpty());
    const auto path = directory.filePath(name);
    QString error;
    REQUIRE(project.document->saveSceneList(path, scene, false, &error, true));
    CHECK(error.isEmpty());
    return path;
}
QByteArray multicamAudio(const QString &project)
{
    Mlt::Producer reopened(pCore->getProjectProfile(), "xml", project.toUtf8().constData());
    REQUIRE(reopened.is_valid());
    reopened.set("set.test_image", 1);
    QByteArray pcm;
    for (int position = 0; position < 125; ++position) {
        reopened.seek(position);
        std::unique_ptr<Mlt::Frame> frame(reopened.get_frame());
        REQUIRE(frame);
        REQUIRE(frame->is_valid());
        mlt_audio_format format = mlt_audio_s16;
        int rate = 48000, channels = 2, samples = 1920;
        const auto *data = frame->get_audio(format, rate, channels, samples);
        REQUIRE(data);
        REQUIRE(format == mlt_audio_s16);
        REQUIRE(rate == 48000);
        REQUIRE(channels == 2);
        REQUIRE(samples == 1920);
        REQUIRE_FALSE(frame->get_int("test_audio"));
        pcm.append(static_cast<const char *>(data), samples * channels * int(sizeof(qint16)));
    }
    return pcm;
}
void multicamFrames(const QString &project, const std::vector<int> &tracks, const QJsonArray &selections, bool edited)
{
    Mlt::Producer reopened(pCore->getProjectProfile(), "xml", project.toUtf8().constData());
    REQUIRE(reopened.is_valid());
    for (int position = 0; position < 125; ++position) {
        int angle = position >= 20 && position < 110 ? 2 : -1;
        if (edited)
            for (const auto &value : selections) {
                const auto range = value.toObject();
                if (position >= range.value(key("startFrame")).toInt() && position < range.value(key("endFrame")).toInt())
                    angle = int(std::find(tracks.begin(), tracks.end(), range.value(key("trackId")).toInt()) - tracks.begin());
            }
        const auto image = KThumb::getFrame(reopened, position, 32, 24);
        REQUIRE_FALSE(image.isNull());
        INFO("project " << project.toStdString() << " frame " << position << " angle " << angle);
        const auto body = QColor(image.pixel(16, 16));
        if (position >= 5 && position < 12) {
            CHECK(body.red() > 200);
            CHECK(body.green() > 200);
            CHECK(body.blue() < 30);
            continue;
        }
        if (angle < 0) {
            CHECK(body.red() < 20);
            CHECK(body.green() < 20);
            CHECK(body.blue() < 20);
            continue;
        }
        const int channels[]{body.red(), body.green(), body.blue()};
        for (int channel = 0; channel < 3; ++channel)
            CHECK((channel == angle ? channels[channel] > 180 : channels[channel] < 50));
        const int sourceFrame = 10 + angle * 5 + position - 20;
        int decoded = 0;
        for (int bit = 0; bit < 8; ++bit) {
            const auto sample = QColor(image.pixel(bit * 4 + 2, 3));
            if (sample.red() > 180 && sample.green() > 180 && sample.blue() > 180)
                decoded |= 1 << bit;
            else {
                CHECK(sample.red() < 60);
                CHECK(sample.green() < 60);
                CHECK(sample.blue() < 60);
            }
        }
        CHECK(decoded == sourceFrame);
    }
}
} // namespace

TEST_CASE("Native multicamera indexed AV angles preserve bit-identical audio and exact camera frames through undo redo and save reload",
          "[MCP][Multicam][AV][Persistence]")
{
    MulticamFfmpeg ffmpeg;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    MulticamProject project;
    std::vector<std::pair<int, int>> cameras;
    for (int angle = 0; angle < 3; ++angle)
        cameras.push_back(insertCamera(project, project.importMedia(indexedCamera(directory, angle)), angle));
    const auto masterSource = project.importMedia(tone(directory, key("master-sync.wav"), 160, 1000));
    const int master = project.insert(masterSource, project.audio[3], 0, 3, 125);
    project.success("set_clip_audio", project.mutation({{key("clipId"), master}, {key("gainDb"), -6}, {key("fadeInFrames"), 3}, {key("fadeOutFrames"), 4}}));
    project.success("set_track_locked", project.mutation({{key("trackId"), project.audio[3]}, {key("locked"), true}}));
    const auto overlay = project.success("add_track", project.mutation({{key("position"), project.timeline->getTracksCount()}, {key("audio"), false}}))
                             .value(key("trackId"))
                             .toInt();
    color(project, overlay, 5, 7, key("#ffff00ff"));
    const auto before = saveMulticam(project, directory, key("before.kdenlive"));
    const auto originalAudio = multicamAudio(before);
    REQUIRE(originalAudio.size() == 125 * 1920 * 2 * 2);
    bool nonzero = false;
    for (char value : originalAudio)
        nonzero |= value != 0;
    REQUIRE(nonzero);
    const auto audio = audioRecords(project);
    const QJsonArray selections{selection(project.video[1], 25, 45), selection(project.video[0], 45, 65), selection(project.video[2], 65, 85),
                                selection(project.video[1], 90, 105)};
    multicamFrames(before, project.video, selections, false);
    const int index = project.history->index();
    const auto applied = project.success("multicam_cut", cutArgs(project, selections, true));
    CHECK(applied.value(key("affectedGroups")).toArray().size() == 3);
    CHECK(project.history->index() == index + 1);
    CHECK(audioRecords(project) == audio);
    const auto after = saveMulticam(project, directory, key("after.kdenlive"));
    CHECK(multicamAudio(after) == originalAudio);
    multicamFrames(after, project.video, selections, true);
    project.success("undo", project.mutation());
    const auto restoredGroups = QJsonDocument::fromJson(project.timeline->groupsData().toUtf8()).array();
    REQUIRE(restoredGroups.size() == 3);
    for (const auto &group : restoredGroups)
        CHECK(group.toObject().value(key("type")) == key("AVSplit"));
    for (const auto &camera : cameras)
        CHECK(project.timeline->getGroupElements(camera.first) == std::unordered_set<int>{camera.first, camera.second});
    CHECK(audioRecords(project) == audio);
    const auto undone = saveMulticam(project, directory, key("undone.kdenlive"));
    CHECK(multicamAudio(undone) == originalAudio);
    multicamFrames(undone, project.video, selections, false);
    project.success("redo", project.mutation());
    CHECK(audioRecords(project) == audio);
    const auto redone = saveMulticam(project, directory, key("redone.kdenlive"));
    CHECK(multicamAudio(redone) == originalAudio);
    multicamFrames(redone, project.video, selections, true);
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("Native multicamera rolls back an unsafe cut through a camera's existing mix", "[MCP][Multicam][Mix][Rollback]")
{
    MulticamFfmpeg ffmpeg;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    MulticamProject project;
    color(project, project.video[0], 0, 80, key("#00ff00ff"));
    const auto firstSource = project.importMedia(indexedCamera(directory, 0));
    const auto secondSource = project.importMedia(indexedCamera(directory, 2));
    const auto insert = [&](const QString &source, int position, int in) {
        return project
            .success("insert_media_clip", project.mutation({{key("binId"), source},
                                                            {key("trackId"), project.video[1]},
                                                            {key("startFrame"), position},
                                                            {key("sourceInFrame"), in},
                                                            {key("durationFrames"), 40},
                                                            {key("stream"), key("video")}}))
            .value(key("clipId"))
            .toInt();
    };
    const int first = insert(firstSource, 0, 10), second = insert(secondSource, 40, 50);
    project.success("edit_mix", project.mutation({{key("operation"), key("add")},
                                                  {key("firstClipId"), first},
                                                  {key("secondClipId"), second},
                                                  {key("transitionId"), key("dissolve")},
                                                  {key("beforeFrames"), 4},
                                                  {key("afterFrames"), 4}}));
    const auto audioSource = project.importMedia(tone(directory, key("rollback-audio.wav"), 160, 1000));
    project.insert(audioSource, project.audio[3], 0, 0, 125);
    const int scratch = project.insert(audioSource, project.audio[1], 0, 0, 80);
    project.success("group_items", project.mutation({{key("itemIds"), QJsonArray{first, scratch}}}));
    const int mixDuration = project.timeline->getMixDuration(second), cut = project.timeline->getMixCutPos(second);
    const auto before = saveMulticam(project, directory, key("mixed-before.kdenlive"));
    const auto audio = multicamAudio(before);
    Mlt::Producer reference(pCore->getProjectProfile(), "xml", before.toUtf8().constData());
    REQUIRE(reference.is_valid());
    std::vector<QImage> pixels;
    for (int frame = 0; frame < 125; ++frame)
        pixels.push_back(KThumb::getFrame(reference, frame, 32, 24));
    // Native single-clip cuts cannot yet preserve both sides of this mix.
    // The guarded operation must restore all preceding cuts and group edits.
    project.failure("multicam_cut", cutArgs(project, {selection(project.video[0], 38, 42)}, true), "NATIVE_EDIT_FAILED");
    CHECK(project.timeline->getGroupElements(first) == std::unordered_set<int>{first, scratch});
    CHECK(project.timeline->getMixDuration(second) == mixDuration);
    CHECK(project.timeline->getMixCutPos(second) == cut);
    const auto after = saveMulticam(project, directory, key("mixed-restored.kdenlive"));
    CHECK(multicamAudio(after) == audio);
    Mlt::Producer restored(pCore->getProjectProfile(), "xml", after.toUtf8().constData());
    REQUIRE(restored.is_valid());
    for (int frame = 0; frame < 125; ++frame) {
        INFO("frame " << frame);
        CHECK(KThumb::getFrame(restored, frame, 32, 24) == pixels[frame]);
    }
    REQUIRE(project.timeline->checkConsistency());
}
