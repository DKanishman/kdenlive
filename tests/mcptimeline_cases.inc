/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Included at the bottom of mcptest.cpp to exercise the exact native dispatcher.
#include "bin/projectfolder.h"
#include "bin/projectsubclip.h"
namespace {
QJsonObject timelineArgs(McpProject &p, const QJsonObject &fields)
{
    auto args = p.expected();
    for (auto i = fields.begin(); i != fields.end(); ++i)
        args.insert(i.key(), i.value());
    return args;
}
QString importTimelineFixture(McpProject &p, const QString &path)
{
    const auto imported = p.success(QStringLiteral("import_media"), timelineArgs(p, {{QStringLiteral("path"), path}}));
    const auto binId = imported.value(QStringLiteral("binId")).toString();
    p.awaitMediaReady(binId);
    return binId;
}
int insertTimelineFixture(McpProject &p, const QString &binId, int track, int start, int sourceIn, int duration, const QString &stream)
{
    return p.success(QStringLiteral("insert_media_clip"), p.mediaArgs(binId, track, start, sourceIn, duration, stream))
        .value(QStringLiteral("clipId"))
        .toInt(-1);
}
} // namespace

TEST_CASE("MCP ordinary image clips move trim duplicate cut and delete through native history", "[MCP][MCPTimeline]")
{
    McpProject p;
    p.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto binId = importTimelineFixture(p, pngFixture(files));
    const int clip = insertTimelineFixture(p, binId, p.videoTracks[0], 10, 0, 12, QStringLiteral("video"));
    REQUIRE(clip >= 0);
    const int initialHistory = p.undoStack->index();
    p.success(QStringLiteral("move_clip"),
              timelineArgs(p, {{QStringLiteral("clipId"), clip}, {QStringLiteral("trackId"), p.videoTracks[1]}, {QStringLiteral("startFrame"), 30}}));
    CHECK(p.timeline->getClipTrackId(clip) == p.videoTracks[1]);
    CHECK(p.timeline->getClipPosition(clip) == 30);
    CHECK(p.undoStack->index() == initialHistory + 1);
    p.success(QStringLiteral("trim_clip"),
              timelineArgs(p, {{QStringLiteral("clipId"), clip}, {QStringLiteral("durationFrames"), 8}, {QStringLiteral("edge"), QStringLiteral("right")}}));
    CHECK(p.timeline->getClipPlaytime(clip) == 8);
    p.success(QStringLiteral("set_clip_transform"), transformArgs(p, clip, QJsonArray{transformFrame(0, 3, 4, 40, 40, 0.75, 15)}));
    const auto duplicate =
        p.success(QStringLiteral("duplicate_clip"),
                  timelineArgs(p, {{QStringLiteral("clipId"), clip}, {QStringLiteral("trackId"), p.videoTracks[0]}, {QStringLiteral("startFrame"), 0}}));
    const int copy = duplicate.value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(copy >= 0);
    CHECK(p.timeline->getClipPosition(copy) == 0);
    CHECK(p.timeline->getClipPlaytime(copy) == 8);
    CHECK(p.timeline->getClipInOut(copy) == p.timeline->getClipInOut(clip));
    CHECK(p.timeline->getClipEffectStackModel(copy)->rowCount() == p.timeline->getClipEffectStackModel(clip)->rowCount());
    const auto cut = p.success(QStringLiteral("cut_clip"), timelineArgs(p, {{QStringLiteral("clipId"), clip}, {QStringLiteral("frame"), 34}}));
    const int right = cut.value(QStringLiteral("rightClipId")).toInt(-1);
    REQUIRE(right >= 0);
    CHECK(p.timeline->getClipPlaytime(clip) == 4);
    CHECK(p.timeline->getClipPosition(right) == 34);
    CHECK(p.timeline->getClipPlaytime(right) == 4);
    p.success(QStringLiteral("delete_item"), timelineArgs(p, {{QStringLiteral("itemId"), right}}));
    CHECK_FALSE(p.timeline->isClip(right));
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->isClip(right));
    p.success(QStringLiteral("undo"), p.expected());
    CHECK_FALSE(p.timeline->isClip(right));
    CHECK(p.timeline->getClipPlaytime(clip) == 8);
    REQUIRE(p.timeline->checkConsistency());
    p.rejectWithoutChange(
        QStringLiteral("duplicate_clip"),
        timelineArgs(p, {{QStringLiteral("clipId"), copy}, {QStringLiteral("trackId"), p.videoTracks[0]}, {QStringLiteral("startFrame"), 0}}));
}

TEST_CASE("MCP tracks expose native names order lock mute and reversible deletion", "[MCP][MCPTimeline]")
{
    McpProject p;
    p.open();
    const int originalCount = p.timeline->getTracksCount();
    const auto added =
        p.success(QStringLiteral("add_track"),
                  timelineArgs(p, {{QStringLiteral("position"), -1}, {QStringLiteral("audio"), false}, {QStringLiteral("name"), QStringLiteral("MCP video")}}));
    const int track = added.value(QStringLiteral("trackId")).toInt(-1);
    REQUIRE(p.timeline->isTrack(track));
    CHECK(p.timeline->getTracksCount() == originalCount + 1);
    CHECK(p.timeline->getTrackPosition(track) == originalCount);
    p.success(QStringLiteral("rename_track"), timelineArgs(p, {{QStringLiteral("trackId"), track}, {QStringLiteral("name"), QStringLiteral("Renamed")}}));
    CHECK(p.timeline->getTrackProperty(track, QStringLiteral("kdenlive:track_name")).toString() == QStringLiteral("Renamed"));
    const auto nativeTrack = KdenliveTests::getTrackById_const(p.timeline, track);
    const auto originalHide = p.timeline->getTrackProperty(track, QStringLiteral("hide")).toString();
    std::vector<int> originalPlaylistHides;
    for (int row = 0; row < nativeTrack->getTrackService()->count(); ++row) {
        std::unique_ptr<Mlt::Producer> playlist(nativeTrack->getTrackService()->track(row));
        originalPlaylistHides.push_back(playlist->get_int("hide"));
    }
    p.success(QStringLiteral("set_track_muted"), timelineArgs(p, {{QStringLiteral("trackId"), track}, {QStringLiteral("muted"), true}}));
    CHECK(p.timeline->getTrackProperty(track, QStringLiteral("hide")).toInt() == 3);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getTrackProperty(track, QStringLiteral("hide")).toString() == originalHide);
    for (int row = 0; row < nativeTrack->getTrackService()->count(); ++row) {
        std::unique_ptr<Mlt::Producer> playlist(nativeTrack->getTrackService()->track(row));
        CHECK(playlist->get_int("hide") == originalPlaylistHides.at(row));
    }
    p.success(QStringLiteral("redo"), p.expected());
    CHECK(p.timeline->getTrackProperty(track, QStringLiteral("hide")).toInt() == 3);
    p.success(QStringLiteral("set_track_muted"), timelineArgs(p, {{QStringLiteral("trackId"), track}, {QStringLiteral("muted"), false}}));
    CHECK(p.timeline->getTrackProperty(track, QStringLiteral("hide")).toInt() == 2);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getTrackProperty(track, QStringLiteral("hide")).toInt() == 3);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getTrackProperty(track, QStringLiteral("hide")).toString() == originalHide);
    p.success(QStringLiteral("set_track_locked"), timelineArgs(p, {{QStringLiteral("trackId"), track}, {QStringLiteral("locked"), true}}));
    CHECK(p.timeline->trackIsLocked(track));
    p.rejectWithoutChange(QStringLiteral("rename_track"),
                          timelineArgs(p, {{QStringLiteral("trackId"), track}, {QStringLiteral("name"), QStringLiteral("Blocked")}}));
    p.success(QStringLiteral("set_track_locked"), timelineArgs(p, {{QStringLiteral("trackId"), track}, {QStringLiteral("locked"), false}}));
    p.success(QStringLiteral("move_track"), timelineArgs(p, {{QStringLiteral("trackId"), track}, {QStringLiteral("direction"), QStringLiteral("down")}}));
    CHECK(p.timeline->getTrackPosition(track) == originalCount - 1);
    const int clip = p.success(QStringLiteral("add_color_clip"), p.colorArgs(track, 0, 10)).value(QStringLiteral("clipId")).toInt(-1);
    p.rejectWithoutChange(QStringLiteral("delete_track"), timelineArgs(p, {{QStringLiteral("trackId"), track}}));
    p.success(QStringLiteral("delete_track"), timelineArgs(p, {{QStringLiteral("trackId"), track}, {QStringLiteral("deleteContents"), true}}));
    CHECK_FALSE(p.timeline->isTrack(track));
    CHECK_FALSE(p.timeline->isClip(clip));
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->isTrack(track));
    CHECK(p.timeline->isClip(clip));
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP groups and selection have explicit scope and preserve native undo", "[MCP][MCPTimeline]")
{
    McpProject p;
    p.open();
    const int first = p.success(QStringLiteral("add_color_clip"), p.colorArgs(p.videoTracks[0], 0, 20)).value(QStringLiteral("clipId")).toInt(-1);
    const int second = p.success(QStringLiteral("add_color_clip"), p.colorArgs(p.videoTracks[1], 0, 20)).value(QStringLiteral("clipId")).toInt(-1);
    p.success(QStringLiteral("group_items"), timelineArgs(p, {{QStringLiteral("itemIds"), QJsonArray{first, second}}}));
    CHECK(p.timeline->isInGroup(first));
    CHECK(p.timeline->isInGroup(second));
    p.rejectWithoutChange(
        QStringLiteral("move_clip"),
        timelineArgs(p, {{QStringLiteral("clipId"), first}, {QStringLiteral("trackId"), p.videoTracks[0]}, {QStringLiteral("startFrame"), 5}}));
    p.success(QStringLiteral("move_clip"), timelineArgs(p, {{QStringLiteral("clipId"), first},
                                                            {QStringLiteral("trackId"), p.videoTracks[0]},
                                                            {QStringLiteral("startFrame"), 5},
                                                            {QStringLiteral("includeGroup"), true}}));
    CHECK(p.timeline->getClipPosition(first) == 5);
    CHECK(p.timeline->getClipPosition(second) == 5);
    p.success(QStringLiteral("set_track_locked"), timelineArgs(p, {{QStringLiteral("trackId"), p.videoTracks[1]}, {QStringLiteral("locked"), true}}));
    p.rejectWithoutChange(QStringLiteral("cut_clip"),
                          timelineArgs(p, {{QStringLiteral("clipId"), first}, {QStringLiteral("frame"), 10}, {QStringLiteral("includeGroup"), true}}));
    p.success(QStringLiteral("set_track_locked"), timelineArgs(p, {{QStringLiteral("trackId"), p.videoTracks[1]}, {QStringLiteral("locked"), false}}));
    const int history = p.undoStack->index();
    p.success(QStringLiteral("set_selection"), timelineArgs(p, {{QStringLiteral("itemIds"), QJsonArray{first}}}));
    CHECK(p.undoStack->index() == history);
    CHECK(p.timeline->getCurrentSelection().contains(first));
    p.rejectWithoutChange(QStringLiteral("delete_item"), timelineArgs(p, {{QStringLiteral("itemId"), first}, {QStringLiteral("includeGroup"), true}}));
    p.success(QStringLiteral("set_selection"), timelineArgs(p, {{QStringLiteral("itemIds"), QJsonArray{}}}));
    CHECK(p.timeline->getCurrentSelection().empty());
    p.success(QStringLiteral("ungroup_items"), timelineArgs(p, {{QStringLiteral("itemId"), first}}));
    CHECK_FALSE(p.timeline->isInGroup(first));
    CHECK_FALSE(p.timeline->isInGroup(second));
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->isInGroup(first));
    p.success(QStringLiteral("delete_item"), timelineArgs(p, {{QStringLiteral("itemId"), first}, {QStringLiteral("includeGroup"), true}}));
    CHECK(p.timeline->getClipsCount() == 0);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getClipsCount() == 2);
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP exact audio slip and native playback speed retain undo", "[MCP][MCPTimeline]")
{
    McpProject p;
    p.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto binId = importTimelineFixture(p, wavFixture(files));
    const int clip = insertTimelineFixture(p, binId, p.audioTrack, 0, 3, 10, QStringLiteral("audio"));
    p.success(QStringLiteral("slip_clip"), timelineArgs(p, {{QStringLiteral("clipId"), clip}, {QStringLiteral("offsetFrames"), 2}}));
    CHECK(p.timeline->getClipInOut(clip) == std::make_pair(5, 14));
    CHECK(p.timeline->getClipPosition(clip) == 0);
    p.rejectWithoutChange(QStringLiteral("slip_clip"), timelineArgs(p, {{QStringLiteral("clipId"), clip}, {QStringLiteral("offsetFrames"), -20}}));
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getClipInOut(clip) == std::make_pair(3, 12));
    p.success(QStringLiteral("set_clip_speed"), timelineArgs(p, {{QStringLiteral("clipId"), clip},
                                                                 {QStringLiteral("speedPercent"), 200},
                                                                 {QStringLiteral("pitchCompensate"), true},
                                                                 {QStringLiteral("changeDuration"), true}}));
    CHECK(p.timeline->getClipSpeed(clip) == Approx(2));
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getClipSpeed(clip) == Approx(1));
    CHECK(p.timeline->getClipInOut(clip) == std::make_pair(3, 12));
    p.success(QStringLiteral("set_clip_speed"), timelineArgs(p, {{QStringLiteral("clipId"), clip},
                                                                 {QStringLiteral("speedPercent"), -100},
                                                                 {QStringLiteral("pitchCompensate"), false},
                                                                 {QStringLiteral("changeDuration"), false}}));
    CHECK(p.timeline->getClipSpeed(clip) == Approx(-1));
    p.success(QStringLiteral("undo"), p.expected());
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP explicit-track lift extract space and gap preserve unrelated tracks", "[MCP][MCPTimeline]")
{
    McpProject p;
    p.open();
    const int track = p.videoTracks[0], otherTrack = p.videoTracks[1];
    const int first = p.success(QStringLiteral("add_color_clip"), p.colorArgs(track, 0, 20)).value(QStringLiteral("clipId")).toInt(-1);
    const int later = p.success(QStringLiteral("add_color_clip"), p.colorArgs(track, 30, 10)).value(QStringLiteral("clipId")).toInt(-1);
    const int untouched = p.success(QStringLiteral("add_color_clip"), p.colorArgs(otherTrack, 30, 10)).value(QStringLiteral("clipId")).toInt(-1);
    p.success(QStringLiteral("edit_timeline_zone"), timelineArgs(p, {{QStringLiteral("trackIds"), QJsonArray{track}},
                                                                     {QStringLiteral("action"), QStringLiteral("lift")},
                                                                     {QStringLiteral("startFrame"), 5},
                                                                     {QStringLiteral("durationFrames"), 5}}));
    CHECK(p.timeline->getClipPlaytime(first) == 5);
    CHECK(p.timeline->getClipByPosition(track, 7) == -1);
    CHECK(p.timeline->getClipPosition(later) == 30);
    CHECK(p.timeline->getClipPosition(untouched) == 30);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getClipPlaytime(first) == 20);
    p.success(QStringLiteral("edit_timeline_zone"), timelineArgs(p, {{QStringLiteral("trackIds"), QJsonArray{track}},
                                                                     {QStringLiteral("action"), QStringLiteral("extract")},
                                                                     {QStringLiteral("startFrame"), 5},
                                                                     {QStringLiteral("durationFrames"), 5}}));
    CHECK(p.timeline->getClipPosition(later) == 25);
    CHECK(p.timeline->getClipPosition(untouched) == 30);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getClipPosition(later) == 30);
    CHECK(p.timeline->getClipPlaytime(first) == 20);
    p.success(QStringLiteral("edit_timeline_zone"), timelineArgs(p, {{QStringLiteral("trackIds"), QJsonArray{track}},
                                                                     {QStringLiteral("action"), QStringLiteral("insert_space")},
                                                                     {QStringLiteral("startFrame"), 10},
                                                                     {QStringLiteral("durationFrames"), 5}}));
    CHECK(p.timeline->getClipPlaytime(first) == 10);
    CHECK(p.timeline->getClipByPosition(track, 12) == -1);
    CHECK(p.timeline->getClipPosition(later) == 35);
    CHECK(p.timeline->getClipPosition(untouched) == 30);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getClipPosition(later) == 30);
    CHECK(p.timeline->getClipPlaytime(first) == 20);
    p.success(QStringLiteral("remove_gap"),
              timelineArgs(p, {{QStringLiteral("trackIds"), QJsonArray{track}}, {QStringLiteral("startFrame"), 20}, {QStringLiteral("durationFrames"), 10}}));
    CHECK(p.timeline->getClipPosition(later) == 20);
    CHECK(p.timeline->getClipPosition(untouched) == 30);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getClipPosition(later) == 30);
    p.rejectWithoutChange(
        QStringLiteral("remove_gap"),
        timelineArgs(p, {{QStringLiteral("trackIds"), QJsonArray{track}}, {QStringLiteral("startFrame"), 19}, {QStringLiteral("durationFrames"), 10}}));
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP native insert and overwrite preserve source ranges and one undo command", "[MCP][MCPTimeline]")
{
    McpProject p;
    p.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto binId = importTimelineFixture(p, wavFixture(files));
    const int first = insertTimelineFixture(p, binId, p.audioTrack, 0, 0, 20, QStringLiteral("audio"));
    auto fields = QJsonObject{{QStringLiteral("binId"), binId},
                              {QStringLiteral("trackId"), p.audioTrack},
                              {QStringLiteral("startFrame"), 5},
                              {QStringLiteral("sourceInFrame"), 10},
                              {QStringLiteral("durationFrames"), 5},
                              {QStringLiteral("stream"), QStringLiteral("audio")},
                              {QStringLiteral("mode"), QStringLiteral("insert")}};
    const int history = p.undoStack->index();
    const int inserted = p.success(QStringLiteral("insert_zone"), timelineArgs(p, fields)).value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(inserted >= 0);
    CHECK(p.timeline->getClipInOut(inserted) == std::make_pair(10, 14));
    CHECK(p.timeline->getClipPosition(inserted) == 5);
    CHECK(p.timeline->getClipByPosition(p.audioTrack, 23) >= 0);
    CHECK(p.undoStack->index() == history + 1);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getClipPlaytime(first) == 20);
    CHECK(p.timeline->getClipsCount() == 1);
    fields.insert(QStringLiteral("mode"), QStringLiteral("overwrite"));
    const int replacement = p.success(QStringLiteral("insert_zone"), timelineArgs(p, fields)).value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(replacement >= 0);
    CHECK(p.timeline->getClipInOut(replacement) == std::make_pair(10, 14));
    CHECK(p.timeline->getClipByPosition(p.audioTrack, 20) == -1);
    CHECK(p.undoStack->index() == history + 1);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getClipPlaytime(first) == 20);
    CHECK(p.timeline->getClipsCount() == 1);
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP grouped duplication preserves separate native group hierarchies", "[MCP][MCPTimeline]")
{
    McpProject p;
    p.open();
    const int first = p.success(QStringLiteral("add_color_clip"), p.colorArgs(p.videoTracks[0], 0, 20)).value(QStringLiteral("clipId")).toInt(-1);
    const int second = p.success(QStringLiteral("add_color_clip"), p.colorArgs(p.videoTracks[1], 3, 12)).value(QStringLiteral("clipId")).toInt(-1);
    p.success(QStringLiteral("group_items"), timelineArgs(p, {{QStringLiteral("itemIds"), QJsonArray{first, second}}}));
    const auto duplicated = p.success(QStringLiteral("duplicate_clip"), timelineArgs(p, {{QStringLiteral("clipId"), first},
                                                                                         {QStringLiteral("trackId"), p.videoTracks[0]},
                                                                                         {QStringLiteral("startFrame"), 30},
                                                                                         {QStringLiteral("includeGroup"), true}}));
    const int copy = duplicated.value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(copy >= 0);
    CHECK(p.timeline->getClipPosition(copy) == 30);
    CHECK(p.timeline->getClipsCount() == 4);
    const auto copies = p.timeline->getGroupElements(copy);
    CHECK(copies.size() == 2);
    CHECK(copies.contains(copy));
    CHECK_FALSE(copies.contains(first));
    CHECK_FALSE(copies.contains(second));
    const int copySecond = p.timeline->getClipByPosition(p.videoTracks[1], 33);
    REQUIRE(copySecond >= 0);
    CHECK(copies.contains(copySecond));
    CHECK(p.timeline->getClipPlaytime(copySecond) == 12);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(p.timeline->getClipsCount() == 2);
    CHECK(p.timeline->isInGroup(first));
    p.success(QStringLiteral("redo"), p.expected());
    CHECK(p.timeline->getClipsCount() == 4);
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP sequence inventory and irreversible-deletion acknowledgement are explicit", "[MCP][MCPTimeline]")
{
    McpProject p;
    p.open();
    const auto listed = p.success(QStringLiteral("list_sequences"));
    const auto sequences = listed.value(QStringLiteral("sequences")).toArray();
    REQUIRE(sequences.size() == 1);
    const auto uuid = sequences.first().toObject().value(QStringLiteral("sequenceId")).toString();
    REQUIRE_FALSE(uuid.isEmpty());
    CHECK(sequences.first().toObject().value(QStringLiteral("active")).toBool());
    p.success(QStringLiteral("add_color_clip"), p.colorArgs(p.videoTracks[0], 0, 10));
    p.rejectWithoutChange(QStringLiteral("delete_sequence"),
                          timelineArgs(p, {{QStringLiteral("sequenceId"), uuid}, {QStringLiteral("acknowledgeHistoryReset"), false}}));
    p.rejectWithoutChange(QStringLiteral("delete_sequence"),
                          timelineArgs(p, {{QStringLiteral("sequenceId"), uuid}, {QStringLiteral("acknowledgeHistoryReset"), true}}));
    p.rejectWithoutChange(
        QStringLiteral("create_sequence"),
        timelineArgs(p, {{QStringLiteral("name"), QStringLiteral("Headless")}, {QStringLiteral("audioTracks"), 1}, {QStringLiteral("videoTracks"), 1}}));
}

#include <mlt++/MltChain.h>
#include <mlt++/MltLink.h>
TEST_CASE("MCP time mapping applies native ramp freeze and reverse keys in one undo command", "[MCP][MCPTimeline]")
{
    McpProject p;
    p.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto binId = importTimelineFixture(p, avFixture(files));
    const int clip = insertTimelineFixture(p, binId, p.videoTracks[0], 0, 3, 10, QStringLiteral("video"));
    const int history = p.undoStack->index();
    const QJsonArray keys{QJsonObject{{QStringLiteral("frame"), 0}, {QStringLiteral("sourceFrame"), 3}},
                          QJsonObject{{QStringLiteral("frame"), 4}, {QStringLiteral("sourceFrame"), 7}},
                          QJsonObject{{QStringLiteral("frame"), 9}, {QStringLiteral("sourceFrame"), 3}}};
    auto fields = QJsonObject{{QStringLiteral("clipId"), clip},
                              {QStringLiteral("keyframes"), keys},
                              {QStringLiteral("pitchCompensate"), false},
                              {QStringLiteral("frameBlending"), true}};
    const auto mapped = p.success(QStringLiteral("set_clip_time_map"), timelineArgs(p, fields));
    CHECK(p.undoStack->index() == history + 1);
    REQUIRE(p.timeline->data(p.timeline->makeClipIndexFromID(clip), TimelineModel::TimeRemapRole).toBool());
    REQUIRE(mapped.value(QStringLiteral("maps")).toArray().size() == 1);
    auto producer = p.timeline->getClipProducer(clip);
    Mlt::Chain chain(producer->parent());
    REQUIRE(chain.is_valid());
    bool found = false;
    for (int row = 0; row < chain.link_count(); ++row) {
        std::unique_ptr<Mlt::Link> link(chain.link(row));
        if (QString::fromUtf8(link->get("mlt_service")) != QStringLiteral("timeremap")) continue;
        found = true;
        CHECK(link->anim_get_double("time_map", 3) * 25 == Approx(3).margin(0.001));
        CHECK(link->anim_get_double("time_map", 7) * 25 == Approx(7).margin(0.001));
        CHECK(link->anim_get_double("time_map", 12) * 25 == Approx(3).margin(0.001));
    }
    REQUIRE(found);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK_FALSE(p.timeline->data(p.timeline->makeClipIndexFromID(clip), TimelineModel::TimeRemapRole).toBool());
    CHECK(p.timeline->getClipInOut(clip) == std::make_pair(3, 12));
    p.success(QStringLiteral("redo"), p.expected());
    CHECK(p.timeline->data(p.timeline->makeClipIndexFromID(clip), TimelineModel::TimeRemapRole).toBool());
    fields.insert(QStringLiteral("keyframes"), QJsonArray{QJsonObject{{QStringLiteral("frame"), 0}, {QStringLiteral("sourceFrame"), 5}},
                                                          QJsonObject{{QStringLiteral("frame"), 9}, {QStringLiteral("sourceFrame"), 5}}});
    p.success(QStringLiteral("set_clip_time_map"), timelineArgs(p, fields));
    fields.insert(QStringLiteral("keyframes"), QJsonArray{QJsonObject{{QStringLiteral("frame"), 0}, {QStringLiteral("sourceFrame"), 25}},
                                                          QJsonObject{{QStringLiteral("frame"), 9}, {QStringLiteral("sourceFrame"), 5}}});
    p.rejectWithoutChange(QStringLiteral("set_clip_time_map"), timelineArgs(p, fields));
    REQUIRE(p.timeline->checkConsistency());
}

#include "transitions/transitionsrepository.hpp"
TEST_CASE("MCP grouped composition duplication preserves explicit automatic and black A tracks", "[MCP][MCPTimeline]")
{
    McpProject p;
    p.open();
    const int lower = p.videoTracks[0];
    const int middle = p.videoTracks[1];
    const int upper = p.success(QStringLiteral("add_track"), timelineArgs(p, {{QStringLiteral("position"), -1}, {QStringLiteral("audio"), false}}))
                          .value(QStringLiteral("trackId"))
                          .toInt(-1);
    REQUIRE(p.timeline->isTrack(upper));
    int requestedLower = -1;
    SECTION("Forced nonadjacent V1 target remains V1 when copied on V3")
    {
        requestedLower = p.timeline->getTrackMltIndex(lower);
    }
    SECTION("Forced black-background target remains black")
    {
        requestedLower = 0;
    }
    SECTION("Automatic source target remains automatic")
    {
        requestedLower = -1;
    }
    const int expectedLower = requestedLower < 0 ? p.timeline->getTrackMltIndex(middle) : requestedLower;
    const int clip = p.success(QStringLiteral("add_color_clip"), p.colorArgs(upper, 0, 20)).value(QStringLiteral("clipId")).toInt(-1);
    QString transitionId;
    for (const auto &entry : TransitionsRepository::get()->getNames()) {
        if (TransitionsRepository::get()->isComposition(entry.first)) {
            transitionId = entry.first;
            break;
        }
    }
    REQUIRE_FALSE(transitionId.isEmpty());
    auto properties = std::make_unique<Mlt::Properties>();
    properties->set("force_track", requestedLower < 0 ? 0 : 1);
    Fun undo = [] { return true; }, redo = [] { return true; };
    int composition = -1;
    REQUIRE(p.timeline->requestCompositionInsertion(transitionId, upper, requestedLower, 0, 20, std::move(properties), composition, undo, redo, true));
    pCore->pushUndo(undo, redo, QStringLiteral("Set up native forced composition"));
    auto sourceAsset = p.timeline->getCompositionParameterModel(composition)->getAsset();
    REQUIRE(sourceAsset->get_int("a_track") == expectedLower);
    REQUIRE(sourceAsset->get_int("force_track") == (requestedLower < 0 ? 0 : 1));
    p.success(QStringLiteral("group_items"), timelineArgs(p, {{QStringLiteral("itemIds"), QJsonArray{clip, composition}}}));
    const int history = p.undoStack->index();
    const auto duplicated = p.success(QStringLiteral("duplicate_clip"), timelineArgs(p, {{QStringLiteral("clipId"), clip},
                                                                                         {QStringLiteral("trackId"), upper},
                                                                                         {QStringLiteral("startFrame"), 30},
                                                                                         {QStringLiteral("includeGroup"), true}}));
    const int copiedClip = duplicated.value(QStringLiteral("clipId")).toInt(-1);
    const int copiedComposition = p.timeline->getCompositionByPosition(upper, 30);
    REQUIRE(copiedComposition >= 0);
    REQUIRE(copiedComposition != composition);
    REQUIRE(p.timeline->getGroupElements(copiedClip).contains(copiedComposition));
    auto copiedAsset = p.timeline->getCompositionParameterModel(copiedComposition)->getAsset();
    CHECK(copiedAsset->get_int("a_track") == expectedLower);
    CHECK(copiedAsset->get_int("force_track") == (requestedLower < 0 ? 0 : 1));
    CHECK(p.undoStack->index() == history + 1);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK_FALSE(p.timeline->isComposition(copiedComposition));
    CHECK(p.timeline->isComposition(composition));
    p.success(QStringLiteral("redo"), p.expected());
    REQUIRE(p.timeline->isComposition(copiedComposition));
    copiedAsset = p.timeline->getCompositionParameterModel(copiedComposition)->getAsset();
    CHECK(copiedAsset->get_int("a_track") == expectedLower);
    CHECK(copiedAsset->get_int("force_track") == (requestedLower < 0 ? 0 : 1));
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP bin folders metadata source zones and unused deletion use native undo", "[MCP][MCPTimeline][MCPBin]")
{
    McpProject p;
    p.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());

    const auto folderResult =
        p.success(QStringLiteral("edit_bin"),
                  timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("create_folder")}, {QStringLiteral("name"), QStringLiteral("Takes")}}));
    const QString folderId = folderResult.value(QStringLiteral("item")).toObject().value(QStringLiteral("binId")).toString();
    REQUIRE_FALSE(folderId.isEmpty());
    const auto folder = pCore->projectItemModel()->getFolderByBinId(folderId);
    REQUIRE(folder);
    const QString binId = importTimelineFixture(p, wavFixture(files));
    const auto source = pCore->projectItemModel()->getClipByBinID(binId);
    REQUIRE(source);
    const auto originalParent = source->parent();
    p.success(
        QStringLiteral("edit_bin"),
        timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("move")}, {QStringLiteral("itemId"), binId}, {QStringLiteral("parentId"), folderId}}));
    CHECK(source->parent() == folder);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(source->parent() == originalParent);
    p.success(QStringLiteral("redo"), p.expected());
    CHECK(source->parent() == folder);
    p.success(QStringLiteral("edit_bin"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("rename")},
                                                           {QStringLiteral("itemId"), binId},
                                                           {QStringLiteral("name"), QStringLiteral("Dialogue")}}));
    CHECK(source->clipName() == QStringLiteral("Dialogue"));
    CHECK(source->getProducerProperty(QStringLiteral("kdenlive:clipname")) == QStringLiteral("Dialogue"));
    p.success(QStringLiteral("edit_bin"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("description")},
                                                           {QStringLiteral("itemId"), binId},
                                                           {QStringLiteral("description"), QStringLiteral("Original take")}}));
    CHECK(source->description() == QStringLiteral("Original take"));
    const auto originalRating = source->rating();
    const auto originalRatingProperty = source->getProducerProperty(QStringLiteral("kdenlive:rating"));
    const auto rated =
        p.success(QStringLiteral("edit_bin"),
                  timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("rating")}, {QStringLiteral("itemId"), binId}, {QStringLiteral("rating"), 4}}));
    CHECK(source->rating() == 4);
    CHECK(source->getProducerIntProperty(QStringLiteral("kdenlive:rating")) == 4);
    CHECK(source->getData(AbstractProjectItem::DataRating).toInt() == 4);
    CHECK(rated.value(QStringLiteral("item")).toObject().value(QStringLiteral("rating")).toInt() == 4);
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(source->rating() == originalRating);
    CHECK(source->getProducerProperty(QStringLiteral("kdenlive:rating")) == originalRatingProperty);
    p.success(QStringLiteral("redo"), p.expected());
    CHECK(source->rating() == 4);
    CHECK(source->getProducerIntProperty(QStringLiteral("kdenlive:rating")) == 4);
    const auto subclipResult = p.success(QStringLiteral("edit_bin"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("create_subclip")},
                                                                                      {QStringLiteral("itemId"), binId},
                                                                                      {QStringLiteral("name"), QStringLiteral("Line one")},
                                                                                      {QStringLiteral("inFrame"), 3},
                                                                                      {QStringLiteral("outFrame"), 9},
                                                                                      {QStringLiteral("rating"), 3}}));
    const auto subId = subclipResult.value(QStringLiteral("item")).toObject().value(QStringLiteral("binId")).toString();
    const auto subclip = pCore->projectItemModel()->getItemByBinId(subId);
    REQUIRE(subclip);
    CHECK(subclip->zone() == QPoint(3, 9));
    CHECK(subclip->parent() == source);
    auto zones = QJsonDocument::fromJson(source->getProducerProperty(QStringLiteral("kdenlive:clipzones")).toUtf8()).array();
    REQUIRE(zones.size() == 1);
    CHECK(zones.first().toObject().value(QStringLiteral("name")).toString() == QStringLiteral("Line one"));
    p.success(QStringLiteral("edit_bin"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("rename")},
                                                           {QStringLiteral("itemId"), subId},
                                                           {QStringLiteral("name"), QStringLiteral("Line one revised")}}));
    zones = QJsonDocument::fromJson(source->getProducerProperty(QStringLiteral("kdenlive:clipzones")).toUtf8()).array();
    CHECK(zones.first().toObject().value(QStringLiteral("name")).toString() == QStringLiteral("Line one revised"));
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(subclip->name() == QStringLiteral("Line one"));
    p.success(QStringLiteral("undo"), p.expected());
    CHECK_FALSE(pCore->projectItemModel()->getItemByBinId(subId));
    CHECK(source->getProducerProperty(QStringLiteral("kdenlive:clipzones")).isEmpty());
    p.success(QStringLiteral("redo"), p.expected());
    REQUIRE(pCore->projectItemModel()->getItemByBinId(subId));
    p.rejectWithoutChange(
        QStringLiteral("edit_bin"),
        timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("move")}, {QStringLiteral("itemId"), folderId}, {QStringLiteral("parentId"), folderId}}));
    p.rejectWithoutChange(QStringLiteral("edit_bin"),
                          timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("delete")}, {QStringLiteral("itemId"), folderId}}));
    const auto page =
        p.success(QStringLiteral("get_bin"), {{QStringLiteral("parentId"), folderId}, {QStringLiteral("recursive"), true}, {QStringLiteral("limit"), 1}});
    CHECK(page.value(QStringLiteral("items")).toArray().size() == 1);
    CHECK(page.value(QStringLiteral("hasMore")).toBool());
    p.success(QStringLiteral("edit_bin"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("delete")},
                                                           {QStringLiteral("itemId"), folderId},
                                                           {QStringLiteral("deleteContents"), true}}));
    CHECK_FALSE(pCore->projectItemModel()->hasClip(binId));
    CHECK(QFile::exists(files.filePath(QStringLiteral("fixture.wav"))));
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(pCore->projectItemModel()->hasClip(binId));
    CHECK(source->parent() == folder);
    const int clip = insertTimelineFixture(p, binId, p.audioTrack, 0, 0, 10, QStringLiteral("audio"));
    REQUIRE(clip >= 0);
    p.rejectWithoutChange(QStringLiteral("edit_bin"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("delete")},
                                                                       {QStringLiteral("itemId"), folderId},
                                                                       {QStringLiteral("deleteContents"), true}}));
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP markers guides ranges and global categories preserve native history", "[MCP][MCPTimeline][MCPMarkers]")
{
    McpProject p;
    p.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const int initialHistory = p.undoStack->index();
    const QString binId = importTimelineFixture(p, wavFixture(files));
    constexpr int category = 1001;
    REQUIRE_FALSE(pCore->markerTypes.contains(category));
    p.success(QStringLiteral("edit_marker_category"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("add")},
                                                                       {QStringLiteral("categoryId"), category},
                                                                       {QStringLiteral("name"), QStringLiteral("Scene beat")},
                                                                       {QStringLiteral("color"), QStringLiteral("#aa3377")}}));
    CHECK(pCore->markerTypes.contains(category));
    p.success(QStringLiteral("edit_marker"), timelineArgs(p, {{QStringLiteral("scope"), QStringLiteral("guides")},
                                                              {QStringLiteral("operation"), QStringLiteral("add")},
                                                              {QStringLiteral("frame"), 10},
                                                              {QStringLiteral("durationFrames"), 5},
                                                              {QStringLiteral("comment"), QStringLiteral("Opening range")},
                                                              {QStringLiteral("categoryId"), category}}));
    const auto guides = p.timeline->getGuideModel();
    REQUIRE(guides);
    CHECK(guides->hasMarker(10));
    CHECK(guides->marker(10).duration().frames(25) == 5);
    auto snaps = guides->getSnapPoints();
    CHECK(std::find(snaps.begin(), snaps.end(), 15) != snaps.end());
    REQUIRE(p.timeline->checkConsistency(guides->getSnapPoints()));
    const auto normalized = p.success(QStringLiteral("edit_marker"), timelineArgs(p, {{QStringLiteral("scope"), QStringLiteral("guides")},
                                                                                      {QStringLiteral("operation"), QStringLiteral("update")},
                                                                                      {QStringLiteral("frame"), 10},
                                                                                      {QStringLiteral("newFrame"), 20},
                                                                                      {QStringLiteral("durationFrames"), 0},
                                                                                      {QStringLiteral("comment"), QString()}}));
    CHECK_FALSE(guides->hasMarker(10));
    REQUIRE(guides->hasMarker(20));
    CHECK(guides->marker(20).duration().frames(25) == 0);
    CHECK(guides->marker(20).comment() == CommentedTime(GenTime(20, 25), QString(), category).comment());
    CHECK(normalized.value(QStringLiteral("commentNormalized")).toBool());
    snaps = guides->getSnapPoints();
    CHECK(std::find(snaps.begin(), snaps.end(), 15) == snaps.end());
    CHECK(std::find(snaps.begin(), snaps.end(), 20) != snaps.end());
    REQUIRE(p.timeline->checkConsistency(guides->getSnapPoints()));
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(guides->hasMarker(10));
    CHECK_FALSE(guides->hasMarker(20));
    CHECK(guides->marker(10).duration().frames(25) == 5);
    REQUIRE(p.timeline->checkConsistency(guides->getSnapPoints()));
    p.success(QStringLiteral("redo"), p.expected());
    CHECK(guides->hasMarker(20));
    REQUIRE(p.timeline->checkConsistency(guides->getSnapPoints()));
    p.success(QStringLiteral("edit_marker"), timelineArgs(p, {{QStringLiteral("scope"), QStringLiteral("clip")},
                                                              {QStringLiteral("binId"), binId},
                                                              {QStringLiteral("operation"), QStringLiteral("add")},
                                                              {QStringLiteral("frame"), 3},
                                                              {QStringLiteral("durationFrames"), 4},
                                                              {QStringLiteral("comment"), QStringLiteral("Source range")},
                                                              {QStringLiteral("categoryId"), category}}));
    const auto markers = pCore->projectItemModel()->getClipByBinID(binId)->markerModel();
    REQUIRE(markers);
    CHECK(markers->marker(3).duration().frames(25) == 4);
    REQUIRE(p.timeline->checkConsistency(guides->getSnapPoints()));
    const auto result = p.success(QStringLiteral("get_markers"),
                                  {{QStringLiteral("scope"), QStringLiteral("clip")}, {QStringLiteral("binId"), binId}, {QStringLiteral("limit"), 1}});
    REQUIRE(result.value(QStringLiteral("markers")).toArray().size() == 1);
    CHECK(result.value(QStringLiteral("markers")).toArray().first().toObject().value(QStringLiteral("frame")).toInt() == 3);
    p.rejectWithoutChange(QStringLiteral("edit_marker_category"),
                          timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("delete")}, {QStringLiteral("categoryId"), category}}));
    p.success(QStringLiteral("edit_marker"),
              timelineArgs(p, {{QStringLiteral("scope"), QStringLiteral("guides")}, {QStringLiteral("operation"), QStringLiteral("clear")}}));
    p.rejectWithoutChange(QStringLiteral("edit_marker_category"),
                          timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("delete")}, {QStringLiteral("categoryId"), category}}));
    p.success(QStringLiteral("edit_marker"), timelineArgs(p, {{QStringLiteral("scope"), QStringLiteral("clip")},
                                                              {QStringLiteral("binId"), binId},
                                                              {QStringLiteral("operation"), QStringLiteral("delete")},
                                                              {QStringLiteral("frame"), 3}}));
    p.success(QStringLiteral("edit_marker_category"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("update")},
                                                                       {QStringLiteral("categoryId"), category},
                                                                       {QStringLiteral("name"), QStringLiteral("Updated beat")},
                                                                       {QStringLiteral("color"), QStringLiteral("#112233")}}));
    CHECK(pCore->markerTypes.value(category).displayName == QStringLiteral("Updated beat"));
    p.success(QStringLiteral("edit_marker_category"),
              timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("delete")}, {QStringLiteral("categoryId"), category}}));
    CHECK_FALSE(pCore->markerTypes.contains(category));
    p.success(QStringLiteral("undo"), p.expected());
    CHECK(pCore->markerTypes.contains(category));
    while (p.undoStack->index() > initialHistory)
        p.success(QStringLiteral("undo"), p.expected());
    CHECK_FALSE(pCore->markerTypes.contains(category));
    REQUIRE(p.timeline->checkConsistency());
}
