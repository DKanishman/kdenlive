/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "kdenlivesettings.h"
#include "mcp/mcpserver.h"
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
#include <cmath>
#include <mlt++/MltFrame.h>
namespace {
QString key(const char *value)
{
    return QString::fromLatin1(value);
}
QString tone(const QTemporaryDir &directory, const QString &name, int frames, int frequency)
{
    QByteArray pcm;
    QDataStream samples(&pcm, QIODevice::WriteOnly);
    samples.setByteOrder(QDataStream::LittleEndian);
    for (int sample = 0; sample < frames * 1920; ++sample)
        samples << qint16(1800 * std::sin(2 * 3.141592653589793 * frequency * sample / 48000));
    const auto path = directory.filePath(name);
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
    return path;
}
class MulticamProject
{
public:
    MulticamProject()
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
                                 {key("clientInfo"), QJsonObject{{key("name"), key("multicam-tests")}, {key("version"), key("1")}}}};
        const auto reply =
            server.handleRequest({{key("jsonrpc"), key("2.0")}, {key("id"), ++request}, {key("method"), key("initialize")}, {key("params"), params}});
        REQUIRE_FALSE(reply.contains(key("error")));
        CHECK(server.handleRequest({{key("jsonrpc"), key("2.0")}, {key("method"), key("notifications/initialized")}}).isEmpty());
        video.push_back(success("add_track", mutation({{key("position"), timeline->getTracksCount()}, {key("audio"), false}})).value(key("trackId")).toInt());
        for (int count = 0; count < 2; ++count)
            audio.push_back(success("add_track", mutation({{key("position"), 0}, {key("audio"), true}})).value(key("trackId")).toInt());
        history->clear();
    }
    ~MulticamProject()
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
    std::shared_ptr<DocUndoStack> history;
    std::unique_ptr<KdenliveDoc> document;
    std::shared_ptr<TimelineItemModel> timeline;
    KdenliveMcpServer server;
    std::vector<int> audio, video;
    int request{0};
};
int color(MulticamProject &project, int track, int start = 20, int duration = 90, const QString &value = key("#ff0000ff"))
{
    return project
        .success("add_color_clip",
                 project.mutation({{key("trackId"), track}, {key("startFrame"), start}, {key("durationFrames"), duration}, {key("color"), value}}))
        .value(key("clipId"))
        .toInt();
}
QJsonArray audioRecords(MulticamProject &project)
{
    QJsonArray result;
    for (const auto &value : project.state().value(key("tracks")).toArray()) {
        const auto track = value.toObject();
        if (!track.value(key("audio")).toBool()) continue;
        for (const auto &entry : track.value(key("clips")).toArray()) {
            auto clip = entry.toObject();
            clip.remove(key("grouped"));
            clip.insert(key("trackId"), track.value(key("id")));
            result.append(clip);
        }
    }
    return result;
}
QJsonObject cutArgs(MulticamProject &project, const QJsonArray &selections, bool acknowledge = false)
{
    QJsonArray tracks;
    for (int track : project.video)
        tracks.append(track);
    return project.mutation({{key("angleTrackIds"), tracks}, {key("selections"), selections}, {key("acknowledgeGroupChanges"), acknowledge}});
}
QJsonObject selection(int track, int start, int end)
{
    return {{key("trackId"), track}, {key("startFrame"), start}, {key("endFrame"), end}};
}
} // namespace
TEST_CASE("Native multicamera cut preserves linked scratch and master audio with exact half-open camera ranges", "[MCP][Multicam][Groups]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    MulticamProject project;
    const auto source = project.importMedia(tone(directory, key("master.wav"), 160, 1000));
    const int scratch = project.insert(source, project.audio[1], 20, 5, 90), master = project.insert(source, project.audio[3], 0, 0, 125);
    const std::vector<int> original{color(project, project.video[0]), color(project, project.video[1], 20, 90, key("#00ff00ff")),
                                    color(project, project.video[2], 20, 90, key("#0000ffff"))};
    project.success("group_items", project.mutation({{key("itemIds"), QJsonArray{original[1], scratch}}}));
    const auto audio = audioRecords(project);
    const auto groups = project.timeline->groupsData();
    const QJsonArray ranges{selection(project.video[1], 25, 45), selection(project.video[0], 45, 65), selection(project.video[2], 65, 85),
                            selection(project.video[1], 90, 105)};
    const auto refusal = project.call("multicam_cut", cutArgs(project, ranges));
    REQUIRE(refusal.value(key("isError")).toBool());
    const auto details = refusal.value(key("structuredContent")).toObject();
    CHECK(details.value(key("error")).toObject().value(key("code")) == key("GROUP_ACKNOWLEDGEMENT_REQUIRED"));
    REQUIRE(details.value(key("affectedGroups")).toArray().size() == 1);
    CHECK(details.value(key("affectedGroups")).toArray().first().toObject().value(key("preservedAudioIds")).toArray() == QJsonArray{scratch});
    REQUIRE(audioRecords(project) == audio);
    REQUIRE(project.timeline->groupsData() == groups);
    const int index = project.history->index();
    const auto applied = project.success("multicam_cut", cutArgs(project, ranges, true));
    CHECK(applied.value(key("changed")).toBool());
    CHECK(project.history->index() == index + 1);
    CHECK(audioRecords(project) == audio);
    auto verify = [&] {
        for (int frame = 0; frame < 125; ++frame)
            for (size_t angle = 0; angle < 3; ++angle) {
                bool retained = frame >= 20 && frame < 110;
                for (const auto &value : ranges) {
                    const auto range = value.toObject();
                    if (frame >= range.value(key("startFrame")).toInt() && frame < range.value(key("endFrame")).toInt())
                        retained = range.value(key("trackId")).toInt() == project.video[angle];
                }
                const int clip = project.timeline->getClipByPosition(project.video[angle], frame);
                INFO("frame " << frame << " angle " << angle);
                REQUIRE((clip >= 0) == retained);
                if (retained) CHECK(project.timeline->getClipIn(clip) + frame - project.timeline->getClipPosition(clip) == frame - 20);
            }
        CHECK(project.timeline->getClipPosition(master) == 0);
        CHECK(project.timeline->getClipPlaytime(master) == 125);
        CHECK(audioRecords(project) == audio);
        REQUIRE(project.timeline->checkConsistency());
    };
    verify();
    project.success("undo", project.mutation());
    CHECK(project.timeline->groupsData() == groups);
    CHECK(audioRecords(project) == audio);
    for (int clip : original) {
        CHECK(project.timeline->getClipPosition(clip) == 20);
        CHECK(project.timeline->getClipPlaytime(clip) == 90);
    }
    project.success("redo", project.mutation());
    verify();
}
TEST_CASE("Native multicamera input refusals preserve exact native state and history", "[MCP][Multicam][Failure]")
{
    MulticamProject project;
    const int first = color(project, project.video[0], 0, 40), second = color(project, project.video[1], 0, 40);
    (void)first;
    auto args = cutArgs(project, {selection(project.video[0], 5, 20)});
    SECTION("Overlapping selections")
    {
        args.insert(key("selections"), QJsonArray{selection(project.video[0], 5, 20), selection(project.video[1], 19, 25)});
        project.failure("multicam_cut", args, "INVALID_SELECTION");
    }
    SECTION("Unsorted selections")
    {
        args.insert(key("selections"), QJsonArray{selection(project.video[0], 20, 25), selection(project.video[1], 5, 10)});
        project.failure("multicam_cut", args, "INVALID_SELECTION");
    }
    SECTION("Empty range")
    {
        args.insert(key("selections"), QJsonArray{selection(project.video[0], 10, 10)});
        project.failure("multicam_cut", args, "INVALID_SELECTION");
    }
    SECTION("Camera gap")
    {
        args.insert(key("selections"), QJsonArray{selection(project.video[0], 35, 45)});
        project.failure("multicam_cut", args, "ANGLE_GAP");
    }
    SECTION("Audio is not an angle")
    {
        args.insert(key("angleTrackIds"), QJsonArray{project.video[0], project.audio[0]});
        project.failure("multicam_cut", args, "INVALID_TRACK");
    }
    SECTION("Duplicate angle")
    {
        args.insert(key("angleTrackIds"), QJsonArray{project.video[0], project.video[0]});
        project.failure("multicam_cut", args, "INVALID_TRACK");
    }
    SECTION("Fractional boundary")
    {
        auto range = selection(project.video[0], 5, 20);
        range.insert(key("endFrame"), 20.5);
        args.insert(key("selections"), QJsonArray{range});
        project.failure("multicam_cut", args, "INVALID_ARGUMENT");
    }
    SECTION("Unknown nested property")
    {
        auto range = selection(project.video[0], 5, 20);
        range.insert(key("ignored"), true);
        args.insert(key("selections"), QJsonArray{range});
        project.failure("multicam_cut", args, "INVALID_ARGUMENT");
    }
    SECTION("Acknowledgement must be boolean")
    {
        args.insert(key("acknowledgeGroupChanges"), key("true"));
        project.failure("multicam_cut", args, "INVALID_ARGUMENT");
    }
    SECTION("A camera needing cuts is locked")
    {
        project.success("set_track_locked", project.mutation({{key("trackId"), project.video[1]}, {key("locked"), true}}));
        args.insert(key("expectedState"), project.state().value(key("expectedState")));
        project.failure("multicam_cut", args, "TRACK_LOCKED");
    }
    SECTION("Chosen camera is muted")
    {
        project.success("set_track_muted", project.mutation({{key("trackId"), project.video[0]}, {key("muted"), true}}));
        args.insert(key("expectedState"), project.state().value(key("expectedState")));
        project.failure("multicam_cut", args, "ANGLE_HAS_NO_VIDEO");
    }
    SECTION("Chosen clip is disabled")
    {
        project.success("set_clips_enabled", project.mutation({{key("clipIds"), QJsonArray{first}}, {key("enabled"), false}}));
        args.insert(key("expectedState"), project.state().value(key("expectedState")));
        project.failure("multicam_cut", args, "ANGLE_GAP");
    }
    SECTION("Stale token")
    {
        project.success("rename_track", project.mutation({{key("trackId"), project.video[0]}, {key("name"), key("Changed after plan")}}));
        project.failure("multicam_cut", args, "STALE_STATE");
    }
    CHECK(project.timeline->getClipPosition(second) == 0);
    REQUIRE(project.timeline->checkConsistency());
}
TEST_CASE("Native multicamera preserves disabled retained fragments and permits locked chosen cameras", "[MCP][Multicam][State]")
{
    MulticamProject project;
    color(project, project.video[0], 0, 40);
    const int inactive = color(project, project.video[1], 0, 40);
    SECTION("Disabled other camera stays disabled outside the lifted range")
    {
        project.success("set_clips_enabled", project.mutation({{key("clipIds"), QJsonArray{inactive}}, {key("enabled"), false}}));
        project.success("multicam_cut", cutArgs(project, {selection(project.video[0], 10, 20)}));
        for (int frame : {5, 25}) {
            const int clip = project.timeline->getClipByPosition(project.video[1], frame);
            REQUIRE(clip >= 0);
            CHECK(project.timeline->getClipState(clip).first == PlaylistState::Disabled);
        }
        project.success("undo", project.mutation());
        CHECK(project.timeline->getClipState(inactive).first == PlaylistState::Disabled);
        project.success("redo", project.mutation());
        const int right = project.timeline->getClipByPosition(project.video[1], 25);
        REQUIRE(right >= 0);
        CHECK(project.timeline->getClipState(right).first == PlaylistState::Disabled);
    }
    SECTION("A locked chosen camera is preserved while the other camera is cut")
    {
        project.success("set_track_locked", project.mutation({{key("trackId"), project.video[0]}, {key("locked"), true}}));
        project.success("multicam_cut", cutArgs(project, {selection(project.video[0], 10, 20)}));
        CHECK(project.timeline->getClipByPosition(project.video[1], 15) < 0);
    }
    REQUIRE(project.timeline->checkConsistency());
}
TEST_CASE("Native multicamera no-op creates no history command", "[MCP][Multicam][NoOp]")
{
    MulticamProject project;
    color(project, project.video[0], 0, 40);
    const int index = project.history->index();
    const auto before = project.state();
    const auto result = project.success("multicam_cut", cutArgs(project, {selection(project.video[0], 10, 20)}));
    CHECK_FALSE(result.value(key("changed")).toBool());
    CHECK(project.history->index() == index);
    CHECK(project.state().value(key("expectedState")) == before.value(key("expectedState")));
}

#include "mcpmulticam_av_cases.inc"

TEST_CASE("Native multicamera retains endless generator ranges and rendered colors through undo and redo", "[MCP][Multicam][Endless]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    MulticamProject project;
    const std::vector<std::string> colors{"red", "green", "blue"};
    for (size_t angle = 0; angle < colors.size(); ++angle) {
        const auto bin = KdenliveTests::createProducer(pCore->getProjectProfile(), colors[angle], pCore->projectItemModel(), 90, false);
        const int clip = project
                             .success("insert_media_clip", project.mutation({{key("binId"), bin},
                                                                             {key("trackId"), project.video[angle]},
                                                                             {key("startFrame"), 0},
                                                                             {key("sourceInFrame"), 0},
                                                                             {key("durationFrames"), 90},
                                                                             {key("stream"), key("video")}}))
                             .value(key("clipId"))
                             .toInt(-1);
        REQUIRE(clip >= 0);
        REQUIRE(KdenliveTests::getClipPtr(project.timeline, clip)->getItemInfo().maxDuration < 0);
    }
    const auto before = project.state();
    const int index = project.history->index();
    const auto verify = [&](const QString &name, bool edited) {
        const auto path = saveMulticam(project, directory, name);
        Mlt::Producer reopened(pCore->getProjectProfile(), "xml", path.toUtf8().constData());
        REQUIRE(reopened.is_valid());
        for (int frame = 0; frame < 90; ++frame) {
            const int chosen = edited ? frame / 30 : 2;
            for (int angle = 0; angle < 3; ++angle) {
                const int clip = project.timeline->getClipByPosition(project.video[angle], frame);
                REQUIRE((clip >= 0) == (!edited || angle == chosen));
                if (clip >= 0) {
                    CHECK(project.timeline->getClipIn(clip) == 0);
                    CHECK(project.timeline->getClipPosition(clip) == (edited ? angle * 30 : 0));
                    CHECK(project.timeline->getClipPlaytime(clip) == (edited ? 30 : 90));
                }
            }
            const auto image = KThumb::getFrame(reopened, frame, 32, 24);
            REQUIRE_FALSE(image.isNull());
            const auto pixel = image.pixelColor(16, 16);
            const std::vector<int> channels{pixel.red(), pixel.green(), pixel.blue()};
            for (int channel = 0; channel < 3; ++channel) {
                if (channel == chosen)
                    CHECK(channels[channel] > 90);
                else
                    CHECK(channels[channel] < 30);
            }
        }
        REQUIRE(project.timeline->checkConsistency());
    };
    project.success("multicam_cut",
                    cutArgs(project, {selection(project.video[0], 0, 30), selection(project.video[1], 30, 60), selection(project.video[2], 60, 90)}));
    CHECK(project.history->index() == index + 1);
    verify(key("endless-cut.kdenlive"), true);
    project.success("undo", project.mutation());
    CHECK(project.state().value(key("tracks")) == before.value(key("tracks")));
    verify(key("endless-undo.kdenlive"), false);
    project.success("redo", project.mutation());
    verify(key("endless-redo.kdenlive"), true);
}

TEST_CASE("Native zone extraction stops when its requested initial ungroup fails", "[MCP][Multicam][NativeFailure]")
{
    MulticamProject project;
    const int ungrouped = color(project, project.video[0], 0, 40), target = color(project, project.video[1], 0, 40);
    const auto before = project.state();
    Fun undo = [] { return true; }, redo = [] { return true; };
    CHECK_FALSE(TimelineFunctions::extractZoneWithUndo(project.timeline, {project.video[1]}, QPoint(10, 20), true, ungrouped, {}, undo, redo));
    CHECK(project.timeline->getClipPosition(target) == 0);
    CHECK(project.timeline->getClipPlaytime(target) == 40);
    CHECK(project.state().value(key("expectedState")) == before.value(key("expectedState")));
    REQUIRE(undo());
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("Native multicamera consistency includes guide ranges and retained source marker snaps", "[MCP][Multicam][Snaps]")
{
    MulticamProject project;
    const int first = color(project, project.video[0], 0, 90);
    color(project, project.video[1], 0, 90, key("#00ff00ff"));
    color(project, project.video[2], 0, 90, key("#0000ffff"));
    const int category = pCore->markerTypes.firstKey();
    const auto guides = project.timeline->getGuideModel();
    REQUIRE(guides);
    REQUIRE(guides->addRangeMarker(GenTime(15, 25), GenTime(7, 25), key("Guide range"), category));
    const auto source = pCore->projectItemModel()->getClipByBinID(project.timeline->getClipBinId(first));
    REQUIRE(source);
    const auto markers = source->getMarkerModel();
    REQUIRE(markers);
    REQUIRE(markers->addMarker(GenTime(20, 25), key("Retained marker"), category));
    REQUIRE(markers->addRangeMarker(GenTime(40, 25), GenTime(10, 25), key("Lifted timeline marker range"), category));
    const auto guidePoints = guides->getSnapPoints(), sourcePoints = markers->getSnapPoints();
    // The basic diagnostic needs these additional snap owners explicitly.
    CHECK_FALSE(project.timeline->checkConsistency());
    const int index = project.history->index();
    project.success("multicam_cut",
                    cutArgs(project, {selection(project.video[0], 0, 30), selection(project.video[1], 30, 60), selection(project.video[2], 60, 90)}, true));
    CHECK(project.history->index() == index + 1);
    CHECK(guides->getSnapPoints() == guidePoints);
    CHECK(markers->getSnapPoints() == sourcePoints);
    project.success("undo", project.mutation());
    CHECK(project.timeline->getClipPlaytime(first) == 90);
    project.success("redo", project.mutation());
    CHECK(project.timeline->getClipPlaytime(first) == 30);
}
