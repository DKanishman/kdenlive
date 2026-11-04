/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Native fixtures exceed the former complete-project boundaries; setup bypasses
// MCP so it is not quadratic repeated project fingerprinting.
#include "bin/clipcreator.hpp"
#include "mcp/mcphelpers.h"
#include "mcp/mcpsequence.h"
#include "mcp/mcpstate.h"
#include "mcp/mcpwire.h"
#include "timeline2/model/clipmodel.hpp"
#include <QScopeGuard>
#include <QSet>
#include <algorithm>
#include <mlt++/MltChain.h>
#include <mlt++/MltFilter.h>
#include <mlt++/MltLink.h>
#include <mlt++/MltPlaylist.h>
#include <mlt++/MltTractor.h>
#include <mlt++/MltTransition.h>

namespace {
QString largeStateColor(McpProject &project, Fun &undo, Fun &redo, const QString &name = QStringLiteral("Large native source"))
{
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), "loader", "color:0x204060ff");
    REQUIRE(producer->is_valid());
    producer->set("length", 1);
    producer->set_in_and_out(0, 0);
    producer->set("kdenlive:duration", 1);
    producer->set("kdenlive:clipname", name.toUtf8().constData());
    const auto bin = pCore->projectItemModel();
    QString id;
    REQUIRE(bin->requestAddBinClip(id, producer, bin->getRootFolder()->clipId(), undo, redo));
    const auto source = bin->getClipByBinID(id);
    REQUIRE(source);
    REQUIRE(source->statusReady());
    source->forceLimitedDuration();
    return id;
}
QJsonObject largeStatePage(McpProject &project, const QString &token, const QString &section, int offset, int limit = 64)
{
    return project.success(
        QStringLiteral("get_state_page"),
        {{QStringLiteral("expectedState"), token}, {QStringLiteral("section"), section}, {QStringLiteral("offset"), offset}, {QStringLiteral("limit"), limit}});
}
QJsonObject largeStateField(McpProject &project, const QString &token, const QString &section, const QString &id, const QString &path, int offset = 0,
                            int limit = 8192)
{
    return project.success(QStringLiteral("get_state_page"), {{QStringLiteral("expectedState"), token},
                                                              {QStringLiteral("section"), section},
                                                              {QStringLiteral("recordId"), id},
                                                              {QStringLiteral("path"), path},
                                                              {QStringLiteral("offset"), offset},
                                                              {QStringLiteral("limit"), limit}});
}
std::shared_ptr<ProjectClip> largeStateUnbuiltSequence(McpProject &project)
{
    const auto bin = pCore->projectItemModel();
    const QString id = ClipCreator::createPlaylistClip(QStringLiteral("Unbuilt native fixture"), {1, 1}, bin->getRootFolder()->clipId(), bin);
    REQUIRE_FALSE(id.isEmpty());
    REQUIRE(id != QStringLiteral("-1"));
    const auto source = bin->getClipByBinID(id);
    REQUIRE(source);
    REQUIRE(source->statusReady());
    const auto uuid = source->getSequenceUuid();
    REQUIRE(project.document->getTimeline(uuid, true));
    pCore->projectManager()->testSetActiveTimeline(project.timeline);
    REQUIRE(pCore->projectManager()->closeTimeline(uuid, true, true, true));
    REQUIRE_FALSE(project.document->getTimeline(uuid, true));
    REQUIRE(bin->hasClip(id));
    return source;
}
void largeStateBounded(const QJsonObject &toolResult)
{
    // Include the largest permitted string request ID, not just the tool body.
    const auto rpc = QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), QString(128, QChar(0x4e2d))}, {QStringLiteral("result"), toolResult}};
    CHECK(QJsonDocument(rpc).toJson(QJsonDocument::Compact).size() <= 1024 * 1024);
}
} // namespace

TEST_CASE("MCP fingerprints and edits more than512 native timeline clips with stable bounded pages", "[MCP][MCPLargeState][timeline]")
{
    McpProject project;
    project.open();
    Fun undo = [] { return true; }, redo = [] { return true; };
    const QString source = largeStateColor(project, undo, redo);
    const int track = project.videoTracks.front();
    QSet<QString> original;
    for (int index = 0; index < 600; ++index) {
        int id = -1;
        REQUIRE(
            project.timeline->requestClipInsertion(QStringLiteral("V") + source, track, 2 * index, id, true, true, false, undo, redo, QVector<int>{track}, 1));
        original.insert(QString::number(id));
    }
    pCore->pushUndo(undo, redo, QStringLiteral("Large native fixture"));
    REQUIRE(project.timeline->getClipsCount() == 600);
    const auto state = project.state();
    CHECK(state.value(QStringLiteral("compact")).toBool());
    CHECK_FALSE(state.value(QStringLiteral("projectionComplete")).toBool(true));
    CHECK(state.value(QStringLiteral("counts")).toObject().value(QStringLiteral("clips")).toInt() == 600);
    CHECK_FALSE(state.contains(QStringLiteral("tracks")));
    const auto token = state.value(QStringLiteral("expectedState")).toString();
    QSet<QString> observed;
    int offset = 0;
    do {
        const auto page = largeStatePage(project, token, QStringLiteral("clips"), offset);
        REQUIRE(page.value(QStringLiteral("total")).toInt() == 600);
        const auto items = page.value(QStringLiteral("items")).toArray();
        REQUIRE_FALSE(items.isEmpty());
        for (const auto &entry : items) {
            const auto item = entry.toObject();
            const QString id = item.value(QStringLiteral("recordId")).toString();
            CHECK_FALSE(observed.contains(id));
            observed.insert(id);
            CHECK(item.value(QStringLiteral("value")).toObject().value(QStringLiteral("trackId")).toInt() == track);
        }
        CHECK(page.value(QStringLiteral("expectedState")).toString() == token);
        const int next = page.value(QStringLiteral("nextOffset")).toInt();
        REQUIRE(next > offset);
        offset = next;
        largeStateBounded(Mcp::toolResult(page));
        if (!page.value(QStringLiteral("hasMore")).toBool()) break;
    } while (offset < 600);
    CHECK(observed == original);
    const auto accepted = project.call(QStringLiteral("add_color_clip"), project.colorArgs(track, 1202, 5));
    largeStateBounded(accepted);
    REQUIRE_FALSE(accepted.value(QStringLiteral("isError")).toBool());
    const auto result = accepted.value(QStringLiteral("structuredContent")).toObject();
    REQUIRE(result.value(QStringLiteral("clipId")).toInt(-1) >= 0);
    REQUIRE_FALSE(result.value(QStringLiteral("binId")).toString().isEmpty());
    CHECK(project.timeline->getClipsCount() == 601);
    CHECK(result.value(QStringLiteral("state")).toObject().value(QStringLiteral("compact")).toBool());
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getClipsCount() == 600);
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(project.timeline->getClipsCount() == 601);
    // A representative optimized-build latency observation, not a fragile pass
    // threshold or a claimed speedup over the old refusal at512 items.
    QList<qint64> samples;
    for (int index = 0; index < 5; ++index) {
        QElapsedTimer clock;
        clock.start();
        const auto current = project.state();
        samples.append(clock.nsecsElapsed());
        CHECK(current.value(QStringLiteral("counts")).toObject().value(QStringLiteral("clips")).toInt() == 601);
    }
    std::sort(samples.begin(), samples.end());
    qInfo() << "MCP_LARGE_STATE_METRICS" << "clips" << project.timeline->getClipsCount() << "sources" << pCore->projectItemModel()->getAllClipIds().size()
            << "samples_ns" << samples << "median_ns" << samples.at(2) << "public_state_bytes"
            << QJsonDocument(project.state()).toJson(QJsonDocument::Compact).size();
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP large bin metadata pages preserve exact Unicode and detect edits outside undo", "[MCP][MCPLargeState][bin]")
{
    McpProject project;
    project.open();
    const auto bin = pCore->projectItemModel();
    Fun undo = [] { return true; }, redo = [] { return true; };
    QString last;
    const QString description = QString(8192, QChar(0x4e2d)) + QString::fromUtf8("😀");
    for (int index = 0; index < 600; ++index) {
        last = largeStateColor(project, undo, redo, QStringLiteral("Native source %1").arg(index));
        bin->getClipByBinID(last)->setDescription(description);
    }
    pCore->pushUndo(undo, redo, QStringLiteral("Large native bin fixture"));
    const auto state = project.state();
    REQUIRE(state.value(QStringLiteral("compact")).toBool());
    CHECK(state.value(QStringLiteral("counts")).toObject().value(QStringLiteral("sources")).toInt() == 601);
    const auto token = state.value(QStringLiteral("expectedState")).toString();
    QString restored;
    int offset = 0;
    do {
        const auto page = largeStateField(project, token, QStringLiteral("bin"), last, QStringLiteral("/description"), offset);
        restored += page.value(QStringLiteral("text")).toString();
        const int next = page.value(QStringLiteral("nextOffset")).toInt();
        REQUIRE(next > offset);
        offset = next;
        largeStateBounded(Mcp::toolResult(page));
        if (!page.value(QStringLiteral("hasMore")).toBool()) break;
    } while (offset < description.size());
    CHECK(restored == description);
    const int history = project.undoStack->index();
    const auto source = bin->getClipByBinID(last);
    source->setDescription(description + QStringLiteral("changed without native undo"));
    CHECK(project.undoStack->index() == history);
    REQUIRE(project.token() != token);
    const auto stale =
        project.call(QStringLiteral("get_state_page"), {{QStringLiteral("expectedState"), token}, {QStringLiteral("section"), QStringLiteral("sources")}});
    CHECK(stale.value(QStringLiteral("isError")).toBool());
    CHECK(stale.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString() ==
          QStringLiteral("STALE_STATE"));
    const auto beforeProperties = project.token();
    source->setProducerProperty(QStringLiteral("force_aspect_ratio"), QStringLiteral("1.25"));
    CHECK(project.undoStack->index() == history);
    REQUIRE(project.token() != beforeProperties);
    const auto aspect = largeStateField(project, project.token(), QStringLiteral("source_properties"), last, QStringLiteral("/force_aspect_ratio"));
    CHECK(aspect.value(QStringLiteral("text")).toString() == QStringLiteral("1.25"));
    const auto renamed = project.success(QStringLiteral("edit_bin"), {{QStringLiteral("expectedState"), project.token()},
                                                                      {QStringLiteral("operation"), QStringLiteral("rename")},
                                                                      {QStringLiteral("itemId"), last},
                                                                      {QStringLiteral("name"), QStringLiteral("Renamed beyond512")}});
    CHECK(renamed.value(QStringLiteral("item")).toObject().value(QStringLiteral("name")).toString() == QStringLiteral("Renamed beyond512"));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(source->name() != QStringLiteral("Renamed beyond512"));
    CHECK(source->getProducerProperty(QStringLiteral("force_aspect_ratio")) == QStringLiteral("1.25"));
}

TEST_CASE("MCP bin traversal and folder edits remain available beyond4096 native nodes", "[MCP][MCPLargeState][hierarchy]")
{
    McpProject project;
    project.open();
    const auto bin = pCore->projectItemModel();
    Fun undo = [] { return true; }, redo = [] { return true; };
    for (int index = 0; index < 4200; ++index) {
        QString id;
        REQUIRE(bin->requestAddFolder(id, QStringLiteral("Native folder %1").arg(index), bin->getRootFolder()->clipId(), undo, redo));
    }
    QString deepest = bin->getRootFolder()->clipId();
    for (int depth = 0; depth < 70; ++depth) {
        QString id;
        REQUIRE(bin->requestAddFolder(id, QStringLiteral("Native depth %1").arg(depth), deepest, undo, redo));
        deepest = id;
    }
    pCore->pushUndo(undo, redo, QStringLiteral("Large native hierarchy fixture"));
    const auto state = project.state();
    REQUIRE(state.value(QStringLiteral("ready")).toBool());
    CHECK(state.value(QStringLiteral("counts")).toObject().value(QStringLiteral("bin")).toInt() >= 4201);
    const auto page = project.success(QStringLiteral("get_bin"), {{QStringLiteral("offset"), 4096}, {QStringLiteral("limit"), 64}});
    CHECK(page.value(QStringLiteral("items")).toArray().size() == 64);
    CHECK(page.value(QStringLiteral("total")).toInt() >= 4200);
    const auto added = project.success(QStringLiteral("edit_bin"), {{QStringLiteral("expectedState"), project.token()},
                                                                    {QStringLiteral("operation"), QStringLiteral("create_folder")},
                                                                    {QStringLiteral("name"), QStringLiteral("Created beyond4096")},
                                                                    {QStringLiteral("parentId"), deepest}});
    const auto created = added.value(QStringLiteral("item")).toObject();
    const auto createdId = created.value(QStringLiteral("binId")).toString();
    REQUIRE_FALSE(createdId.isEmpty());
    CHECK(created.value(QStringLiteral("parentId")).toString() == deepest);
    const auto folder = bin->getFolderByBinId(createdId);
    REQUIRE(folder);
    CHECK(folder->parent()->clipId() == deepest);
    CHECK(folder->name() == QStringLiteral("Created beyond4096"));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK_FALSE(bin->getFolderByBinId(createdId));
    CHECK(project.state().value(QStringLiteral("ready")).toBool());
}

TEST_CASE("MCP pages and edits native projects containing more than64 tracks", "[MCP][MCPLargeState][tracks]")
{
    McpProject project;
    project.open();
    while (project.timeline->getTracksCount() < 66) {
        int track = -1;
        REQUIRE(project.timeline->requestTrackInsertion(-1, track, QStringLiteral("Native track"), false));
    }
    const auto state = project.state();
    REQUIRE(state.value(QStringLiteral("compact")).toBool());
    CHECK(state.value(QStringLiteral("counts")).toObject().value(QStringLiteral("tracks")).toInt() == 66);
    const auto page = largeStatePage(project, project.token(), QStringLiteral("tracks"), 64);
    CHECK(page.value(QStringLiteral("items")).toArray().size() == 2);
    const auto added = project.success(QStringLiteral("add_track"), {{QStringLiteral("expectedState"), project.token()},
                                                                     {QStringLiteral("position"), -1},
                                                                     {QStringLiteral("audio"), false},
                                                                     {QStringLiteral("name"), QStringLiteral("Beyond64")}});
    REQUIRE(added.value(QStringLiteral("trackId")).toInt(-1) >= 0);
    CHECK(project.timeline->getTracksCount() == 67);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getTracksCount() == 66);
}

TEST_CASE("MCP fingerprint includes full native remap configuration outside undo", "[MCP][MCPLargeState][fingerprint]")
{
    McpProject project;
    project.open();
    const auto bin = pCore->projectItemModel();
    const QString source = KdenliveTests::createAVProducer(pCore->getProjectProfile(), bin);
    int clip = -1;
    REQUIRE(project.timeline->requestClipInsertion(QStringLiteral("V") + source, project.videoTracks.front(), 0, clip, true));
    Fun undo = [] { return true; }, redo = [] { return true; };
    REQUIRE(project.timeline->requestClipTimeRemap(clip, true, undo, redo));
    pCore->pushUndo(undo, redo, QStringLiteral("Native remap fixture"));
    const auto model = KdenliveTests::getClipPtr(project.timeline, clip);
    REQUIRE(model->hasTimeRemap());
    model->setRemapValue(QStringLiteral("pitch"), QStringLiteral("1"));
    const auto before = project.token();
    const int history = project.undoStack->index();
    model->setRemapValue(QStringLiteral("pitch"), QStringLiteral("0"));
    CHECK(project.undoStack->index() == history);
    REQUIRE(project.token() != before);
    const auto pitch = largeStateField(project, project.token(), QStringLiteral("clips"), QString::number(clip), QStringLiteral("/timeMap/pitch"));
    CHECK(pitch.value(QStringLiteral("text")).toString() == QStringLiteral("0"));
    project.rejectWithoutChange(QStringLiteral("get_state_page"),
                                {{QStringLiteral("expectedState"), before}, {QStringLiteral("section"), QStringLiteral("clips")}});
}

TEST_CASE("MCP fingerprint binds exact native profile aspect and color metadata", "[MCP][MCPLargeState][profile]")
{
    McpProject project;
    project.open();
    auto &profile = pCore->getProjectProfile();
    const int originalColor = profile.colorspace();
    const auto restore = qScopeGuard([&profile, originalColor] { profile.set_colorspace(originalColor); });
    const auto before = project.token();
    const int history = project.undoStack->index();
    const int next = originalColor == 709 ? 601 : 709;
    profile.set_colorspace(next);
    CHECK(project.undoStack->index() == history);
    REQUIRE(project.token() != before);
    const auto state = project.state();
    CHECK(state.value(QStringLiteral("profileData")).toObject().value(QStringLiteral("colorspace")).toInt() == next);
    const auto observed =
        largeStateField(project, project.token(), QStringLiteral("header"), QStringLiteral("all"), QStringLiteral("/profileData/colorspace"), 0, 1);
    CHECK(observed.value(QStringLiteral("value")).toInt() == next);
}

TEST_CASE("MCP whole project fingerprint includes inactive closed and unbuilt sequence graphs", "[MCP][MCPLargeState][sequences]")
{
    McpProject project;
    project.open();
    const auto bin = pCore->projectItemModel();
    const QString id = ClipCreator::createPlaylistClip(QStringLiteral("Inactive fingerprint fixture"), {1, 1}, bin->getRootFolder()->clipId(), bin);
    REQUIRE_FALSE(id.isEmpty());
    REQUIRE(id != QStringLiteral("-1"));
    const auto source = bin->getClipByBinID(id);
    REQUIRE(source);
    REQUIRE(source->statusReady());
    const auto uuid = source->getSequenceUuid();
    const auto other = project.document->getTimeline(uuid, true);
    REQUIRE(other);
    pCore->projectManager()->testSetActiveTimeline(project.timeline);
    int track = -1;
    for (int candidate : other->getAllTracksIds())
        if (!other->isAudioTrack(candidate)) {
            track = candidate;
            break;
        }
    REQUIRE(track >= 0);
    const int nativeTrackIndex = other->getTrackMltIndex(track);
    const auto active = project.timeline->uuid().toString(QUuid::WithoutBraces);
    const auto before = project.token();
    const int history = project.undoStack->index();
    other->setTrackProperty(track, QStringLiteral("kdenlive:track_name"), QStringLiteral("Inactive edit outside undo"));
    CHECK(project.undoStack->index() == history);
    REQUIRE(project.token() != before);
    CHECK(project.state().value(QStringLiteral("sequenceId")).toString() == active);
    REQUIRE(pCore->projectManager()->closeTimeline(uuid, false, false, true));
    REQUIRE(other->isClosed);
    const auto closedToken = project.token();
    const int closedHistory = project.undoStack->index();
    other->setTrackProperty(track, QStringLiteral("kdenlive:track_name"), QStringLiteral("Closed edit outside undo"));
    CHECK(project.undoStack->index() == closedHistory);
    REQUIRE(project.token() != closedToken);
    // Native deletion staging can remove the model before the live bin item.
    // Its producer graph must still affect project freshness without frame pulls.
    REQUIRE(pCore->projectManager()->closeTimeline(uuid, true, true, true));
    REQUIRE_FALSE(project.document->getTimeline(uuid, true));
    REQUIRE(bin->hasClip(id));
    const auto producer = source->originalProducer();
    REQUIRE(producer);
    REQUIRE(producer->is_valid());
    const int position = producer->position();
    Mlt::Tractor graph(producer->parent());
    REQUIRE(graph.is_valid());
    std::unique_ptr<Mlt::Producer> nativeTrack(graph.track(nativeTrackIndex));
    REQUIRE(nativeTrack);
    REQUIRE(nativeTrack->is_valid());
    const auto unbuiltToken = project.token();
    const int unbuiltHistory = project.undoStack->index();
    nativeTrack->set("kdenlive:track_name", "Unbuilt graph edit outside undo");
    CHECK(project.undoStack->index() == unbuiltHistory);
    REQUIRE(project.token() != unbuiltToken);
    CHECK(producer->position() == position);
    CHECK(project.state().value(QStringLiteral("ready")).toBool());
    CHECK(project.state().value(QStringLiteral("fingerprintScope")).toString() == QStringLiteral("whole_project"));
}

TEST_CASE("Native direct child snapshots preserve membership and child lifetime across reparenting", "[MCP][MCPLargeState][hierarchy]")
{
    McpProject project;
    project.open();
    const auto bin = pCore->projectItemModel();
    const auto root = bin->getRootFolder();
    Fun undo = [] { return true; }, redo = [] { return true; };
    QString first, second;
    REQUIRE(bin->requestAddFolder(first, QStringLiteral("First child"), root->clipId(), undo, redo));
    REQUIRE(bin->requestAddFolder(second, QStringLiteral("Second child"), root->clipId(), undo, redo));
    const auto a = bin->getItemByBinId(first), b = bin->getItemByBinId(second);
    REQUIRE(a);
    REQUIRE(b);
    const auto captured = root->directChildren();
    REQUIRE(std::find(captured.begin(), captured.end(), a) != captured.end());
    REQUIRE(a->changeParent(b));
    const auto current = root->directChildren();
    CHECK(std::find(current.begin(), current.end(), a) == current.end());
    CHECK(std::find(captured.begin(), captured.end(), a) != captured.end());
    CHECK(a->parent()->clipId() == second);
    const auto children = b->directChildren();
    REQUIRE(children.size() == 1);
    CHECK(children.front() == a);
    CHECK(project.state().value(QStringLiteral("ready")).toBool());
}

TEST_CASE("MCP project token excludes saved timeline view state but retains sequence zones", "[MCP][MCPLargeState][view]")
{
    McpProject project;
    project.open();
    const auto bin = pCore->projectItemModel();
    const auto all = Mcp::liveSequenceClips(bin);
    REQUIRE(all.contains(project.timeline->uuid()));
    const auto source = bin->getClipByBinID(all.value(project.timeline->uuid()));
    REQUIRE(source);
    const auto token = project.token();
    const int history = project.undoStack->index();
    for (const QString &name :
         {QStringLiteral("position"), QStringLiteral("scrollPos"), QStringLiteral("activeTrack"), QStringLiteral("zoom"), QStringLiteral("verticalzoom")}) {
        project.document->setSequenceProperty(project.timeline->uuid(), name, QStringLiteral("127"));
        source->setProducerProperty(QStringLiteral("kdenlive:sequenceproperties.") + name, QStringLiteral("127"));
    }
    project.timeline->tractor()->seek(3);
    project.timeline->tractor()->set_speed(0);
    CHECK(project.undoStack->index() == history);
    CHECK(project.token() == token);
    project.document->setSequenceProperty(project.timeline->uuid(), QStringLiteral("zoneout"), QStringLiteral("19"));
    CHECK(project.undoStack->index() == history);
    CHECK(project.token() != token);
}

TEST_CASE("MCP reads saved native bin notes and detects a second dirty edit without undo", "[MCP][MCPLargeState][notes]")
{
    McpProject project;
    project.open();
    const auto bin = pCore->projectItemModel();
    REQUIRE(bin->projectTractor());
    project.document->setModified(true);
    bin->saveProperty(QStringLiteral("kdenlive:documentnotes"), QStringLiteral("<p>First native notes</p>"));
    const auto first = project.state();
    REQUIRE(first.value(QStringLiteral("documentNotes")).toString() == QStringLiteral("<p>First native notes</p>"));
    const int history = project.undoStack->index();
    bin->saveProperty(QStringLiteral("kdenlive:documentnotes"), QStringLiteral("<p>Second native notes</p>"));
    const auto second = project.state();
    CHECK(second.value(QStringLiteral("modified")).toBool());
    CHECK(project.undoStack->index() == history);
    REQUIRE(second.value(QStringLiteral("expectedState")) != first.value(QStringLiteral("expectedState")));
    const auto notes = largeStateField(project, project.token(), QStringLiteral("header"), QStringLiteral("all"), QStringLiteral("/documentNotes"));
    CHECK(notes.value(QStringLiteral("text")).toString() == QStringLiteral("<p>Second native notes</p>"));
}

TEST_CASE("MCP unbuilt graph keeps editable effect speed and ignores service local render telemetry", "[MCP][MCPLargeState][graph]")
{
    McpProject project;
    project.open();
    const auto source = largeStateUnbuiltSequence(project);
    const auto original = source->originalProducer();
    REQUIRE(original);
    Mlt::Tractor graph(original->parent());
    REQUIRE(graph.is_valid());
    const int history = project.undoStack->index();
    for (const char *service : {"wave", "timer"}) {
        Mlt::Filter filter(pCore->getProjectProfile(), service);
        REQUIRE(filter.is_valid());
        filter.set("speed", 1.0);
        REQUIRE(graph.attach(filter) == 0);
        const auto token = project.token();
        filter.set("speed", 2.0);
        REQUIRE(project.token() != token);
        CHECK(project.undoStack->index() == history);
        REQUIRE(graph.detach(filter) == 0);
    }
    Mlt::Producer color(pCore->getProjectProfile(), "color", "red");
    REQUIRE(color.is_valid());
    color.set_in_and_out(0, 24);
    Mlt::Chain chain(pCore->getProjectProfile());
    chain.set_source(color);
    Mlt::Link remap("timeremap");
    REQUIRE(remap.is_valid());
    remap.set("time_map", "0=0;24=24");
    REQUIRE(chain.attach(remap) == 0);
    REQUIRE(graph.set_track(chain, 0) == 0);
    remap.set("speed", 1.0);
    const auto remapToken = project.token();
    remap.set("speed", 1.25);
    CHECK(project.token() == remapToken);
    remap.set("time_map", "0=0;24=12");
    CHECK(project.token() != remapToken);
    // Real native audio evaluation changes mix.duck_level every processed frame.
    // Warm service initialization first; then prove an actual telemetry change
    // is ignored while the editable ducking input still invalidates the token.
    Mlt::Producer toneA(pCore->getProjectProfile(), "tone"), toneB(pCore->getProjectProfile(), "tone");
    REQUIRE(toneA.is_valid());
    REQUIRE(toneB.is_valid());
    toneA.set("level", -2.0);
    toneB.set("level", -9.0);
    toneA.set_in_and_out(0, 24);
    toneB.set_in_and_out(0, 24);
    REQUIRE(graph.set_track(toneA, 0) == 0);
    REQUIRE(graph.set_track(toneB, 1) == 0);
    Mlt::Transition mix(pCore->getProjectProfile(), "mix");
    REQUIRE(mix.is_valid());
    mix.set("duck_threshold", -20.0);
    mix.set("duck_attenuation", -12.0);
    mix.set("duck_fade_in", 0);
    mix.set("duck_fade_out", 0);
    mix.set_in_and_out(0, 24);
    graph.plant_transition(mix, 0, 1);
    graph.refresh();
    auto evaluate = [&](int position) {
        // Evaluate this exact native transition with real producer frames.
        // The saved sequence's pre-existing field graph can route its output
        // through other tracks, so its final frame need not pull this mix.
        toneA.seek(position);
        toneB.seek(position);
        toneA.set_speed(1);
        toneB.set_speed(1);
        std::unique_ptr<Mlt::Frame> frameA(toneA.get_frame()), frameB(toneB.get_frame());
        REQUIRE(frameA);
        REQUIRE(frameB);
        REQUIRE(frameA->is_valid());
        REQUIRE(frameB->is_valid());
        REQUIRE(mlt_transition_process(mix.get_transition(), frameA->get_frame(), frameB->get_frame()) == frameA->get_frame());
        mlt_audio_format format = mlt_audio_f32le;
        int frequency = 48000, channels = 2, samples = 1920;
        const auto audio = static_cast<float *>(frameA->get_audio(format, frequency, channels, samples));
        REQUIRE(audio);
        REQUIRE(format == mlt_audio_f32le);
        REQUIRE(samples > 0);
        REQUIRE(channels > 0);
        REQUIRE(std::any_of(audio, audio + samples * channels, [](float value) { return std::abs(value) > 0.000001f; }));
    };
    evaluate(0);
    mix.set("duck_level", 123.0);
    const auto renderedToken = project.token();
    evaluate(1);
    REQUIRE(mix.get_double("duck_level") != 123.0);
    CHECK(project.token() == renderedToken);
    mix.set("duck_attenuation", -6.0);
    CHECK(project.token() != renderedToken);
    CHECK(project.undoStack->index() == history);
}

TEST_CASE("MCP unbuilt large playlists keep repeat semantics or report the pending dependency explicitly", "[MCP][MCPLargeState][graph][performance]")
{
    McpProject project;
    project.open();
    const auto source = largeStateUnbuiltSequence(project);
    const auto original = source->originalProducer();
    REQUIRE(original);
    Mlt::Tractor graph(original->parent());
    Mlt::Producer color(pCore->getProjectProfile(), "color", "blue");
    REQUIRE(color.is_valid());
    color.set_in_and_out(0, 2);
    Mlt::Playlist playlist(pCore->getProjectProfile());
    for (int row = 0; row < 3; ++row)
        REQUIRE(playlist.append(color, 0, 2) == 0);
    REQUIRE(mlt_playlist_repeat_clip(playlist.get_playlist(), 0, 2) == 0);
    REQUIRE(graph.set_track(playlist, 0) == 0);
    graph.refresh();
    const auto smallToken = project.token();
    const int smallDuration = playlist.get_playtime();
    REQUIRE(mlt_playlist_repeat_clip(playlist.get_playlist(), 0, 1) == 0);
    REQUIRE(mlt_playlist_repeat_clip(playlist.get_playlist(), 1, 2) == 0);
    CHECK(playlist.get_playtime() == smallDuration);
    REQUIRE(project.token() != smallToken);
    for (int row = 3; row < 600; ++row)
        REQUIRE(playlist.append(color, 0, 2) == 0);
    graph.refresh();
#if defined(MLT_PLAYLIST_ENTRY_INFO_API) && MLT_PLAYLIST_ENTRY_INFO_API >= 1
    const auto token = project.token();
    // Redistribute repeats without changing total duration or cut properties.
    REQUIRE(mlt_playlist_repeat_clip(playlist.get_playlist(), 0, 2) == 0);
    REQUIRE(mlt_playlist_repeat_clip(playlist.get_playlist(), 1, 1) == 0);
    REQUIRE(project.token() != token);
    QElapsedTimer clock;
    QJsonArray samples;
    for (int index = 0; index < 5; ++index) {
        clock.start();
        const auto state = project.state();
        samples.append(double(clock.nsecsElapsed()) / 1000000.0);
        CHECK(state.value(QStringLiteral("ready")).toBool());
    }
    qInfo() << "MCP_RAW_GRAPH_METRICS" << "playlist_entries" << 600 << "snapshot_samples_ms" << samples;
#else
    const auto state = project.success(QStringLiteral("get_state")).value(QStringLiteral("state")).toObject();
    CHECK_FALSE(state.value(QStringLiteral("ready")).toBool());
    CHECK_FALSE(state.contains(QStringLiteral("expectedState")));
    CHECK(state.value(QStringLiteral("sequenceId")).toString() == source->getSequenceUuid().toString(QUuid::WithoutBraces));
    CHECK(state.value(QStringLiteral("binId")).toString() == source->clipId());
    CHECK(state.value(QStringLiteral("limitation")).toString() == QStringLiteral("UNBUILT_SEQUENCE_GRAPH_UNVERIFIED"));
    CHECK_FALSE(state.value(QStringLiteral("recovery")).toString().isEmpty());
    CHECK_FALSE(project.success(QStringLiteral("list_sequences")).value(QStringLiteral("sequences")).toArray().isEmpty());
    CHECK_FALSE(project.success(QStringLiteral("get_bin")).value(QStringLiteral("items")).toArray().isEmpty());
    CHECK(project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).isObject());
#endif
}
