/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Exercise native sequence membership and models, including the stale native
// UUID cache left by sequence creation undo/deletion. No map is mocked.
#include "bin/clipcreator.hpp"
#include "mcp/mcpsequence.h"
#include <QScopeGuard>

namespace {
QString stateSequenceFixture(McpProject &project)
{
    const auto bin = pCore->projectItemModel();
    const auto id = ClipCreator::createPlaylistClip(QStringLiteral("Sequence state fixture"), {1, 1}, bin->getRootFolder()->clipId(), bin);
    REQUIRE_FALSE(id.isEmpty());
    REQUIRE(id != QStringLiteral("-1"));
    const auto source = bin->getClipByBinID(id);
    REQUIRE(source);
    REQUIRE(source->statusReady());
    REQUIRE_FALSE(source->getSequenceUuid().isNull());
    REQUIRE(project.document->getTimeline(source->getSequenceUuid(), true));
    // Headless openTimeline has no tab switch. Keep the original timeline
    // explicitly active, as a user can do before undoing another sequence.
    pCore->projectManager()->testSetActiveTimeline(project.timeline);
    return id;
}
QJsonObject stateSequenceEntry(McpProject &project, const QUuid &uuid)
{
    const auto entries = project.success(QStringLiteral("list_sequences")).value(QStringLiteral("sequences")).toArray();
    for (const auto &value : entries) {
        const auto entry = value.toObject();
        if (entry.value(QStringLiteral("sequenceId")).toString() == uuid.toString(QUuid::WithoutBraces)) return entry;
    }
    return {};
}
} // namespace

TEST_CASE("MCP sequence creation undo and redo inspect actual live bin membership", "[MCP][MCPSequenceState][undo]")
{
    McpProject project;
    project.open();
    const auto bin = pCore->projectItemModel();
    const auto original = project.timeline->uuid();
    const int before = project.undoStack->index();
    REQUIRE(Mcp::liveSequenceClips(bin).size() == 1);
    const QString id = stateSequenceFixture(project);
    const QUuid uuid = bin->getClipByBinID(id)->getSequenceUuid();
    REQUIRE(project.undoStack->index() == before + 1);
    REQUIRE(Mcp::liveSequenceClips(bin).size() == 2);
    REQUIRE_FALSE(stateSequenceEntry(project, uuid).isEmpty());
    CHECK(project.state().value(QStringLiteral("sequenceId")).toString() == original.toString(QUuid::WithoutBraces));

    project.success(QStringLiteral("undo"), project.expected());
    REQUIRE(project.undoStack->index() == before);
    REQUIRE_FALSE(bin->hasClip(id));
    REQUIRE_FALSE(project.document->getTimeline(uuid, true));
    INFO("Native cached UUID retained after undo: " << bin->getAllSequenceClips().contains(uuid));
    CHECK(Mcp::liveSequenceClips(bin).size() == 1);
    CHECK(stateSequenceEntry(project, uuid).isEmpty());
    CHECK(project.state().value(QStringLiteral("ready")).toBool());

    project.success(QStringLiteral("redo"), project.expected());
    REQUIRE(project.undoStack->index() == before + 1);
    REQUIRE(bin->hasClip(id));
    REQUIRE(project.document->getTimeline(uuid, true));
    CHECK(Mcp::liveSequenceClips(bin).value(uuid) == id);
    CHECK(Mcp::liveSequenceClips(bin).size() == 2);
    const auto restored = stateSequenceEntry(project, uuid);
    CHECK(restored.value(QStringLiteral("binId")).toString() == id);
    CHECK(restored.value(QStringLiteral("loaded")).toBool());
    CHECK(project.state().value(QStringLiteral("sequenceId")).toString() == original.toString(QUuid::WithoutBraces));
}

TEST_CASE("MCP state survives the native close then bin deletion sequence transaction", "[MCP][MCPSequenceState][deletion]")
{
    McpProject project;
    project.open();
    const auto bin = pCore->projectItemModel();
    const QString id = stateSequenceFixture(project);
    const auto source = bin->getClipByBinID(id);
    const QUuid uuid = source->getSequenceUuid();
    // This is the same ordered native close/bin deletion used by the GUI-only
    // delete_sequence tool after explicit history-reset acknowledgement.
    REQUIRE(pCore->projectManager()->closeTimeline(uuid, true, true, true));
    REQUIRE_FALSE(project.document->getTimeline(uuid, true));
    REQUIRE(bin->hasClip(id));
    CHECK(project.state().value(QStringLiteral("ready")).toBool());
    const auto intermediate = stateSequenceEntry(project, uuid);
    CHECK_FALSE(intermediate.isEmpty());
    CHECK_FALSE(intermediate.value(QStringLiteral("loaded")).toBool());

    Fun undo = [] { return true; }, redo = [] { return true; };
    REQUIRE(bin->requestBinClipDeletion(source, undo, redo));
    REQUIRE_FALSE(bin->hasClip(id));
    INFO("Native cached UUID retained after deletion: " << bin->getAllSequenceClips().contains(uuid));
    CHECK(Mcp::liveSequenceClips(bin).size() == 1);
    CHECK(stateSequenceEntry(project, uuid).isEmpty());
    CHECK(project.state().value(QStringLiteral("ready")).toBool());
    // Ordinary editing and its state-bearing undo reply must remain available.
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 5));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getClipsCount() == 0);
    CHECK(project.state().value(QStringLiteral("ready")).toBool());
}

TEST_CASE("MCP hashes guides in closed or unbuilt sequences and protects their used categories", "[MCP][MCPSequenceState][guides]")
{
    McpProject project;
    project.open();
    const auto originalCategories = pCore->markerTypes;
    const auto restoreCategories = qScopeGuard([originalCategories] { pCore->markerTypes = originalCategories; });
    int category = 987600;
    while (pCore->markerTypes.contains(category))
        ++category;
    project.success(QStringLiteral("edit_marker_category"), timelineArgs(project, {{QStringLiteral("operation"), QStringLiteral("add")},
                                                                                   {QStringLiteral("categoryId"), category},
                                                                                   {QStringLiteral("name"), QStringLiteral("Inactive sequence range")},
                                                                                   {QStringLiteral("color"), QStringLiteral("#123456")}}));
    const auto bin = pCore->projectItemModel();
    const QString id = stateSequenceFixture(project);
    const auto source = bin->getClipByBinID(id);
    const QUuid uuid = source->getSequenceUuid();
    const auto model = project.document->getTimeline(uuid, true);
    const auto markers = source->markerModel();
    REQUIRE(markers);
    REQUIRE(model->getGuideModel() == markers);
    REQUIRE(markers->addRangeMarker(GenTime(3, 25), GenTime(7, 25), QStringLiteral("Closed range"), category));
    REQUIRE(pCore->projectManager()->closeTimeline(uuid, false, false));
    REQUIRE(project.document->getTimeline(uuid, true));
    REQUIRE(model->isClosed);
    project.rejectWithoutChange(QStringLiteral("edit_marker_category"),
                                timelineArgs(project, {{QStringLiteral("operation"), QStringLiteral("delete")}, {QStringLiteral("categoryId"), category}}));
    CHECK(pCore->markerTypes.contains(category));
    CHECK(markers->marker(3).duration().frames(25) == 7);
    const QString closedHash = project.state().value(QStringLiteral("guidesHash")).toString();

    REQUIRE(pCore->projectManager()->closeTimeline(uuid, true, true, true));
    REQUIRE_FALSE(project.document->getTimeline(uuid, true));
    REQUIRE(source->statusReady());
    const auto unbuilt = project.state();
    CHECK(unbuilt.value(QStringLiteral("guidesHash")).toString() != closedHash);
    CHECK_FALSE(stateSequenceEntry(project, uuid).value(QStringLiteral("loaded")).toBool());
    REQUIRE(markers->addRangeMarker(GenTime(17, 25), GenTime(4, 25), QStringLiteral("Unbuilt range"), category));
    const auto edited = project.state();
    CHECK(edited.value(QStringLiteral("guidesHash")) != unbuilt.value(QStringLiteral("guidesHash")));
    const int history = project.undoStack->index();
    // Serialized pending guide data is distinct native state even before a
    // sequence model exists. Its changes must invalidate prepared operations.
    project.document->setSequenceProperty(uuid, QStringLiteral("guides"), markers->toJson());
    const auto stored = project.state();
    CHECK(stored.value(QStringLiteral("guidesHash")) != edited.value(QStringLiteral("guidesHash")));
    CHECK(project.undoStack->index() == history);
    source->setProducerProperty(QStringLiteral("kdenlive:sequenceproperties.guides"), markers->toJson());
    const auto serialized = project.state();
    CHECK(serialized.value(QStringLiteral("guidesHash")) != stored.value(QStringLiteral("guidesHash")));
    CHECK(project.undoStack->index() == history);
    project.rejectWithoutChange(QStringLiteral("undo"), {{QStringLiteral("expectedState"), stored.value(QStringLiteral("expectedState"))}});
    CHECK(markers->marker(3).duration().frames(25) == 7);
    CHECK(markers->marker(17).duration().frames(25) == 4);
}
