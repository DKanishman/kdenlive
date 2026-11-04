/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Native project category transactions: real models, IDs, snaps, queued imports,
// history, and saved native XML. CommentedTime equality alone is insufficient.
#include "bin/bin.h"
#include "bin/clipcreator.hpp"
#include "bin/projectfolder.h"
#include "timeline2/model/snapmodel.hpp"
#include <QScopeGuard>

namespace {
QJsonArray categoryMarkerState(const std::shared_ptr<MarkerListModel> &model)
{
    QJsonArray state;
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row);
        state.append(QJsonObject{{QStringLiteral("id"), model->data(index, MarkerListModel::IdRole).toInt()},
                                 {QStringLiteral("frame"), model->data(index, MarkerListModel::FrameRole).toInt()},
                                 {QStringLiteral("duration"), model->data(index, MarkerListModel::DurationRole).toInt()},
                                 {QStringLiteral("comment"), model->data(index, MarkerListModel::CommentRole).toString()},
                                 {QStringLiteral("type"), model->data(index, MarkerListModel::TypeRole).toInt()}});
    }
    return state;
}
QJsonObject categoryArguments(McpProject &project, const QJsonObject &fields)
{
    auto arguments = project.expected();
    for (auto i = fields.begin(); i != fields.end(); ++i) arguments.insert(i.key(), i.value());
    return arguments;
}
QStringList categoryWithout(const std::shared_ptr<MarkerListModel> &model, int id)
{
    auto categories = model->categoriesToStringList();
    for (auto i = categories.begin(); i != categories.end();) {
        if (i->section(QLatin1Char(':'), -2, -2).toInt() == id) i = categories.erase(i);
        else ++i;
    }
    return categories;
}
void addNativeCategory(McpProject &project, int id)
{
    auto categories = project.timeline->getGuideModel()->categoriesToStringList();
    REQUIRE_FALSE(pCore->markerTypes.contains(id));
    categories.append(QStringLiteral("Category transaction range:%1:#a13070").arg(id));
    REQUIRE(project.document->updateGuideCategories(categories));
}
std::shared_ptr<ProjectClip> categorySource(McpProject &project, const QString &parent = {}, const QString &storedMarkers = {})
{
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), "loader", "color:0x204060ff");
    REQUIRE(producer->is_valid());
    producer->set("length", 300);
    producer->set_in_and_out(0, 299);
    producer->set("kdenlive:duration", 300);
    producer->set("kdenlive:clipname", "Category transaction source");
    if (!storedMarkers.isEmpty()) producer->set("kdenlive:markers", storedMarkers.toUtf8().constData());
    const auto bin = pCore->projectItemModel();
    QString id;
    Fun undo = [] { return true; }, redo = [] { return true; };
    REQUIRE(bin->requestAddBinClip(id, producer, parent.isEmpty() ? bin->getRootFolder()->clipId() : parent, undo, redo));
    const auto source = bin->getClipByBinID(id);
    REQUIRE(source);
    REQUIRE(source->statusReady());
    return source;
}
std::shared_ptr<ProjectClip> categorySequence(McpProject &project, bool unbuilt)
{
    const auto bin = pCore->projectItemModel();
    const auto id = ClipCreator::createPlaylistClip(QStringLiteral("Category transaction sequence"), {1, 1}, bin->getRootFolder()->clipId(), bin);
    REQUIRE_FALSE(id.isEmpty());
    REQUIRE(id != QStringLiteral("-1"));
    const auto source = bin->getClipByBinID(id);
    REQUIRE(source);
    REQUIRE(source->statusReady());
    const auto sequence = project.document->getTimeline(source->getSequenceUuid(), true);
    REQUIRE(sequence);
    REQUIRE(sequence->getGuideModel() == source->getMarkerModel());
    pCore->projectManager()->testSetActiveTimeline(project.timeline);
    if (unbuilt) {
        REQUIRE(pCore->projectManager()->closeTimeline(source->getSequenceUuid(), true, true, true));
        REQUIRE_FALSE(project.document->getTimeline(source->getSequenceUuid(), true));
    }
    return source;
}
void checkNativeCategoryMarker(const std::shared_ptr<MarkerListModel> &model, int frame, int duration, const QString &comment, int category, int id)
{
    REQUIRE(model->hasMarker(frame));
    const auto marker = model->marker(frame);
    CHECK(model->markerIdAtFrame(frame) == id);
    CHECK(marker.time().frames(25) == frame);
    CHECK(marker.duration().frames(25) == duration);
    CHECK(marker.comment() == comment);
    CHECK(marker.markerType() == category);
}
} // namespace

TEST_CASE("Native category remap spans bin aliases inactive and unbuilt sequences in one undo", "[MCP][MCPCategories][native]")
{
    McpProject project;
    project.open();
    const auto originalTypes = pCore->markerTypes;
    const auto restoreTypes = qScopeGuard([originalTypes] { pCore->markerTypes = originalTypes; });
    const int defaultCategory = KdenliveSettings::default_marker_type();
    const auto defaults = KdenliveSettings::guidesCategories();
    const auto activeGuides = project.timeline->getGuideModel();
    const auto inactive = categorySequence(project, false);
    const auto unbuilt = categorySequence(project, true);
    const auto bin = pCore->projectItemModel();
    Fun fixtureUndo = [] { return true; }, fixtureRedo = [] { return true; };
    QString parent;
    REQUIRE(bin->requestAddFolder(parent, QStringLiteral("Nested marker source"), bin->getRootFolder()->clipId(), fixtureUndo, fixtureRedo));
    QString nested;
    REQUIRE(bin->requestAddFolder(nested, QStringLiteral("Nested twice"), parent, fixtureUndo, fixtureRedo));
    const auto source = categorySource(project, nested);
    constexpr int removed = 946201;
    addNativeCategory(project, removed);
    const int retained = pCore->markerTypes.firstKey();
    const QList<std::shared_ptr<MarkerListModel>> models{activeGuides, inactive->getMarkerModel(), source->getMarkerModel(), unbuilt->getMarkerModel()};
    const QList<int> frames{7, 17, 3, 33};
    const QList<int> durations{6, 9, 5, 8};
    QList<int> ids;
    QList<QJsonArray> before;
    QList<std::shared_ptr<SnapModel>> snaps;
    QList<std::map<int, int>> snapStates;
    for (int i = 0; i < models.size(); ++i) {
        REQUIRE(models[i]->addRangeMarker(GenTime(frames[i], 25), GenTime(durations[i], 25), QStringLiteral("range %1").arg(i), removed));
        REQUIRE(models[i]->addMarker(GenTime(frames[i] + durations[i], 25), QStringLiteral("shared endpoint"), retained));
        ids.append(models[i]->markerIdAtFrame(frames[i]));
        before.append(categoryMarkerState(models[i]));
        auto snap = std::make_shared<SnapModel>();
        models[i]->registerSnapModel(snap);
        snaps.append(snap);
        snapStates.append(snap->_snaps());
        REQUIRE(snap->_snaps().at(frames[i] + durations[i]) == 2);
    }
    const int first = project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks[0], 50, 5)).value(QStringLiteral("clipId")).toInt();
    const int second = project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks[1], 60, 5)).value(QStringLiteral("clipId")).toInt();
    project.success(QStringLiteral("group_items"), categoryArguments(project, {{QStringLiteral("itemIds"), QJsonArray{first, second}}}));
    REQUIRE(project.timeline->requestSetSelection({first, second}));
    const auto selection = project.timeline->getCurrentSelection();
    const auto groups = project.timeline->groupsData();
    const auto activeUuid = project.document->activeUuid;
    const auto timelineUuids = project.document->getTimelinesUuids();
    const auto oldCategories = activeGuides->categoriesToStringList();
    const QString oldProperty = project.document->getDocumentProperty(QStringLiteral("guidesCategories"));
    auto next = categoryWithout(activeGuides, removed);
    const int history = project.undoStack->index();
    const int count = project.undoStack->count();
    REQUIRE(project.document->updateGuideCategories(next, {{removed, retained}}));
    CHECK(project.undoStack->index() == history + 1);
    CHECK(project.undoStack->count() == count + 1);
    CHECK_FALSE(pCore->markerTypes.contains(removed));
    for (int i = 0; i < models.size(); ++i) {
        checkNativeCategoryMarker(models[i], frames[i], durations[i], QStringLiteral("range %1").arg(i), retained, ids[i]);
        CHECK(snaps[i]->_snaps() == snapStates[i]);
    }
    CHECK(project.timeline->getCurrentSelection() == selection);
    CHECK(project.timeline->groupsData() == groups);
    CHECK(project.document->activeUuid == activeUuid);
    CHECK(project.document->getTimelinesUuids() == timelineUuids);
    CHECK_FALSE(project.document->getTimeline(unbuilt->getSequenceUuid(), true));
    project.undoStack->undo();
    CHECK(project.undoStack->index() == history);
    CHECK(activeGuides->categoriesToStringList() == oldCategories);
    CHECK(project.document->getDocumentProperty(QStringLiteral("guidesCategories")) == oldProperty);
    for (int i = 0; i < models.size(); ++i) {
        CHECK(categoryMarkerState(models[i]) == before[i]);
        CHECK(snaps[i]->_snaps() == snapStates[i]);
    }
    project.undoStack->redo();
    for (int i = 0; i < models.size(); ++i) {
        checkNativeCategoryMarker(models[i], frames[i], durations[i], QStringLiteral("range %1").arg(i), retained, ids[i]);
        CHECK(snaps[i]->_snaps() == snapStates[i]);
    }
    CHECK(KdenliveSettings::default_marker_type() == defaultCategory);
    CHECK(KdenliveSettings::guidesCategories() == defaults);
    REQUIRE(project.timeline->checkConsistency(activeGuides->getSnapPoints()));

    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto path = files.filePath(QStringLiteral("category-remap.kdenlive"));
    // Native prepareSave copies document properties to the bin before its MLT
    // scene is serialized. This headless test must perform the same step.
    pCore->projectItemModel()->saveDocumentProperties(project.document->documentProperties(true), {});
    const auto scene = pCore->projectManager()->projectSceneList(files.path()).first;
    REQUIRE_FALSE(scene.isEmpty());
    QString error;
    REQUIRE(project.document->saveSceneList(path, scene, false, &error, true));
    CHECK(error.isEmpty());
    QFile saved(path);
    REQUIRE(saved.open(QIODevice::ReadOnly));
    QDomDocument xml;
    REQUIRE(xml.setContent(saved.readAll()));
    const auto properties = xml.elementsByTagName(QStringLiteral("property"));
    int savedRanges = 0;
    bool savedCategories = false;
    for (int row = 0; row < properties.size(); ++row) {
        const auto property = properties.at(row).toElement();
        const auto name = property.attribute(QStringLiteral("name"));
        if (name == QStringLiteral("kdenlive:docproperties.guidesCategories")) {
            savedCategories = true;
            const auto data = QJsonDocument::fromJson(property.text().toUtf8()).array();
            for (const auto &entry : data) CHECK(entry.toObject().value(QStringLiteral("index")).toInt() != removed);
        }
        if (name != QStringLiteral("kdenlive:markers")) continue;
        const auto markers = QJsonDocument::fromJson(property.text().toUtf8()).array();
        for (const auto &entry : markers) {
            const auto marker = entry.toObject();
            if (!marker.value(QStringLiteral("comment")).toString().startsWith(QStringLiteral("range "))) continue;
            const int i = marker.value(QStringLiteral("comment")).toString().section(QLatin1Char(' '), -1).toInt();
            REQUIRE(i >= 0);
            REQUIRE(i < models.size());
            CHECK(marker.value(QStringLiteral("pos")).toInt() == frames[i]);
            CHECK(marker.value(QStringLiteral("duration")).toInt() == durations[i]);
            CHECK(marker.value(QStringLiteral("type")).toInt() == retained);
            ++savedRanges;
        }
    }
    CHECK(savedCategories);
    CHECK(savedRanges >= 4);
    Mlt::Producer reopened(pCore->getProjectProfile(), "xml", path.toUtf8().constData());
    REQUIRE(reopened.is_valid());
}

TEST_CASE("MCP category marker deletion is explicit and restores exact IDs ranges and endpoint counts", "[MCP][MCPCategories][delete]")
{
    McpProject project;
    project.open();
    const auto originalTypes = pCore->markerTypes;
    const auto restoreTypes = qScopeGuard([originalTypes] { pCore->markerTypes = originalTypes; });
    const auto source = categorySource(project);
    const auto guides = project.timeline->getGuideModel();
    constexpr int removed = 946202;
    addNativeCategory(project, removed);
    const int retained = pCore->markerTypes.firstKey();
    const QList<std::shared_ptr<MarkerListModel>> models{guides, source->getMarkerModel()};
    QList<QJsonArray> before;
    QList<std::shared_ptr<SnapModel>> snaps;
    QList<std::map<int, int>> snapStates;
    for (const auto &model : models) {
        REQUIRE(model->addRangeMarker(GenTime(3, 25), GenTime(5, 25), QStringLiteral("deleted range"), removed));
        REQUIRE(model->addMarker(GenTime(8, 25), QStringLiteral("retained point"), retained));
        REQUIRE(model->addMarker(GenTime(20, 25), QStringLiteral("deleted point"), removed));
        before.append(categoryMarkerState(model));
        auto snap = std::make_shared<SnapModel>();
        model->registerSnapModel(snap);
        snaps.append(snap);
        snapStates.append(snap->_snaps());
    }
    // Keep a real redo branch, then verify every rejected policy preserves it.
    REQUIRE(guides->addMarker(GenTime(40, 25), QStringLiteral("redo branch"), retained));
    project.undoStack->undo();
    REQUIRE(project.undoStack->canRedo());
    const int history = project.undoStack->index();
    auto args = categoryArguments(project, {{QStringLiteral("operation"), QStringLiteral("delete")}, {QStringLiteral("categoryId"), removed}});
    project.rejectWithoutChange(QStringLiteral("edit_marker_category"), args);
    args.insert(QStringLiteral("markerPolicy"), QStringLiteral("delete"));
    project.rejectWithoutChange(QStringLiteral("edit_marker_category"), args);
    args.insert(QStringLiteral("acknowledgeMarkerDeletion"), false);
    project.rejectWithoutChange(QStringLiteral("edit_marker_category"), args);
    CHECK(project.undoStack->canRedo());
    args.insert(QStringLiteral("acknowledgeMarkerDeletion"), true);
    const auto deleted = project.success(QStringLiteral("edit_marker_category"), args);
    CHECK(deleted.value(QStringLiteral("affectedMarkerCount")).toInt() == 4);
    CHECK_FALSE(deleted.value(QStringLiteral("historyReset")).toBool(true));
    CHECK(project.undoStack->index() == history + 1);
    for (int i = 0; i < models.size(); ++i) {
        CHECK(models[i]->rowCount() == 1);
        CHECK_FALSE(models[i]->hasMarker(3));
        CHECK_FALSE(models[i]->hasMarker(20));
        CHECK(snaps[i]->_snaps() == std::map<int, int>{{8, 1}});
    }
    project.success(QStringLiteral("undo"), project.expected());
    for (int i = 0; i < models.size(); ++i) {
        CHECK(categoryMarkerState(models[i]) == before[i]);
        CHECK(snaps[i]->_snaps() == snapStates[i]);
    }
    project.success(QStringLiteral("redo"), project.expected());
    for (const auto &model : models) CHECK(model->rowCount() == 1);
}

TEST_CASE("MCP category remap validates policy and preserves point and range identities", "[MCP][MCPCategories][remap]")
{
    McpProject project;
    project.open();
    const auto originalTypes = pCore->markerTypes;
    const auto restoreTypes = qScopeGuard([originalTypes] { pCore->markerTypes = originalTypes; });
    const auto guides = project.timeline->getGuideModel();
    constexpr int removed = 946203;
    addNativeCategory(project, removed);
    const int retained = pCore->markerTypes.firstKey();
    REQUIRE(guides->addRangeMarker(GenTime(7, 25), GenTime(6, 25), QStringLiteral("remapped range"), removed));
    REQUIRE(guides->addMarker(GenTime(17, 25), QStringLiteral("remapped point"), removed));
    const auto before = categoryMarkerState(guides);
    const int rangeId = guides->markerIdAtFrame(7), pointId = guides->markerIdAtFrame(17);
    auto args = categoryArguments(project, {{QStringLiteral("operation"), QStringLiteral("delete")}, {QStringLiteral("categoryId"), removed},
                                            {QStringLiteral("markerPolicy"), QStringLiteral("remap")}});
    project.rejectWithoutChange(QStringLiteral("edit_marker_category"), args);
    args.insert(QStringLiteral("replacementCategoryId"), removed);
    project.rejectWithoutChange(QStringLiteral("edit_marker_category"), args);
    args.insert(QStringLiteral("replacementCategoryId"), 999999);
    project.rejectWithoutChange(QStringLiteral("edit_marker_category"), args);
    args.insert(QStringLiteral("replacementCategoryId"), retained);
    const int history = project.undoStack->index();
    const auto remapped = project.success(QStringLiteral("edit_marker_category"), args);
    CHECK(remapped.value(QStringLiteral("affectedMarkerCount")).toInt() == 2);
    CHECK(project.undoStack->index() == history + 1);
    checkNativeCategoryMarker(guides, 7, 6, QStringLiteral("remapped range"), retained, rangeId);
    checkNativeCategoryMarker(guides, 17, 0, QStringLiteral("remapped point"), retained, pointId);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(categoryMarkerState(guides) == before);
    project.success(QStringLiteral("redo"), project.expected());
    checkNativeCategoryMarker(guides, 7, 6, QStringLiteral("remapped range"), retained, rangeId);
}

TEST_CASE("Native category failures preserve all models metadata and prior redo history", "[MCP][MCPCategories][failure]")
{
    McpProject project;
    project.open();
    const auto originalTypes = pCore->markerTypes;
    const auto restoreTypes = qScopeGuard([originalTypes] { pCore->markerTypes = originalTypes; });
    const auto unbuilt = categorySequence(project, true);
    const auto source = categorySource(project);
    const auto guides = project.timeline->getGuideModel();
    constexpr int removed = 946204;
    addNativeCategory(project, removed);
    const int retained = pCore->markerTypes.firstKey();
    REQUIRE(guides->addRangeMarker(GenTime(7, 25), GenTime(6, 25), QStringLiteral("failure active"), removed));
    REQUIRE(source->getMarkerModel()->addRangeMarker(GenTime(3, 25), GenTime(5, 25), QStringLiteral("failure source"), removed));
    REQUIRE(unbuilt->getMarkerModel()->addRangeMarker(GenTime(17, 25), GenTime(9, 25), QStringLiteral("failure unbuilt"), removed));
    REQUIRE(guides->addMarker(GenTime(40, 25), QStringLiteral("redo sentinel"), retained));
    project.undoStack->undo();
    const auto next = categoryWithout(guides, removed);
    const auto beforeGuides = categoryMarkerState(guides), beforeSource = categoryMarkerState(source->getMarkerModel()),
               beforeUnbuilt = categoryMarkerState(unbuilt->getMarkerModel());
    const auto categories = guides->categoriesToStringList();
    const auto property = project.document->getDocumentProperty(QStringLiteral("guidesCategories"));
    const int history = project.undoStack->index(), count = project.undoStack->count();
    const auto unchanged = [&] {
        CHECK(categoryMarkerState(guides) == beforeGuides);
        CHECK(categoryMarkerState(source->getMarkerModel()) == beforeSource);
        CHECK(categoryMarkerState(unbuilt->getMarkerModel()) == beforeUnbuilt);
        CHECK(guides->categoriesToStringList() == categories);
        CHECK(project.document->getDocumentProperty(QStringLiteral("guidesCategories")) == property);
        CHECK(project.undoStack->index() == history);
        CHECK(project.undoStack->count() == count);
        CHECK(project.undoStack->canRedo());
    };
    source->setClipStatus(FileStatus::StatusWaiting);
    CHECK_FALSE(project.document->updateGuideCategories(next, {{removed, retained}}));
    source->setClipStatus(FileStatus::StatusReady);
    unchanged();
    project.document->setSequenceProperty(unbuilt->getSequenceUuid(), QStringLiteral("guides"), unbuilt->getMarkerModel()->toJson());
    CHECK_FALSE(project.document->updateGuideCategories(next, {{removed, retained}}));
    project.document->clearSequenceProperty(unbuilt->getSequenceUuid(), QStringLiteral("guides"));
    unchanged();
    unbuilt->setProducerProperty(QStringLiteral("kdenlive:sequenceproperties.guides"), unbuilt->getMarkerModel()->toJson());
    CHECK_FALSE(project.document->updateGuideCategories(next, {{removed, retained}}));
    unbuilt->resetProducerProperty(QStringLiteral("kdenlive:sequenceproperties.guides"));
    unchanged();
    CHECK_FALSE(project.document->updateGuideCategories(next, {{removed, 999999}}));
    unchanged();
    CHECK_FALSE(project.document->updateGuideCategories(next, {{removed, removed}}));
    unchanged();
    auto invalid = next;
    invalid.append(next.first());
    CHECK_FALSE(project.document->updateGuideCategories(invalid, {{removed, retained}}));
    unchanged();
    invalid = next;
    invalid.append(QStringLiteral("invalid:946299:not-a-color"));
    CHECK_FALSE(project.document->updateGuideCategories(invalid, {{removed, retained}}));
    unchanged();
    const int defaultCategory = KdenliveSettings::default_marker_type();
    if (pCore->markerTypes.contains(defaultCategory)) {
        CHECK_FALSE(project.document->updateGuideCategories(categoryWithout(guides, defaultCategory), {{defaultCategory, removed}}));
        unchanged();
    }
}

TEST_CASE("Native category changes wait for queued marker imports even after pre-import edits", "[MCP][MCPCategories][pending]")
{
    McpProject project;
    project.open();
    const auto originalTypes = pCore->markerTypes;
    const auto restoreTypes = qScopeGuard([originalTypes] { pCore->markerTypes = originalTypes; });
    constexpr int removed = 946205;
    addNativeCategory(project, removed);
    const auto guides = project.timeline->getGuideModel();
    const int retained = pCore->markerTypes.firstKey();
    const QJsonArray data{QJsonObject{{QStringLiteral("pos"), 3}, {QStringLiteral("duration"), 5},
                                     {QStringLiteral("comment"), QStringLiteral("queued range")}, {QStringLiteral("type"), removed}}};
    const auto source = categorySource(project, {}, QString::fromUtf8(QJsonDocument(data).toJson()));
    const auto markers = source->getMarkerModel();
    REQUIRE(markers->hasPendingImport());
    REQUIRE(markers->addMarker(GenTime(17, 25), QStringLiteral("pre-import edit"), removed));
    REQUIRE(markers->rowCount() == 1);
    const auto before = categoryMarkerState(markers);
    const auto next = categoryWithout(guides, removed);
    const int history = project.undoStack->index(), count = project.undoStack->count();
    CHECK_FALSE(project.document->updateGuideCategories(next, {{removed, retained}}));
    CHECK(categoryMarkerState(markers) == before);
    CHECK(project.undoStack->index() == history);
    CHECK(project.undoStack->count() == count);
    QCoreApplication::sendPostedEvents(markers.get(), QEvent::MetaCall);
    REQUIRE_FALSE(markers->hasPendingImport());
    REQUIRE(markers->rowCount() == 2);
    const int rangeId = markers->markerIdAtFrame(3), pointId = markers->markerIdAtFrame(17);
    REQUIRE(project.document->updateGuideCategories(next, {{removed, retained}}));
    CHECK(project.undoStack->index() == history + 1);
    checkNativeCategoryMarker(markers, 3, 5, QStringLiteral("queued range"), retained, rangeId);
    checkNativeCategoryMarker(markers, 17, 0, QStringLiteral("pre-import edit"), retained, pointId);

    // Failed/malformed queued imports must clear their pending bookkeeping too.
    auto invalid = std::make_shared<MarkerListModel>(QStringLiteral("malformed pending fixture"), project.undoStack);
    invalid->scheduleImportFromJson(QStringLiteral("invalid json"));
    REQUIRE(invalid->hasPendingImport());
    QCoreApplication::sendPostedEvents(invalid.get(), QEvent::MetaCall);
    CHECK_FALSE(invalid->hasPendingImport());
    invalid->scheduleImportFromJson(QStringLiteral("[{\"pos\":4,\"type\":999999}]"));
    QCoreApplication::sendPostedEvents(invalid.get(), QEvent::MetaCall);
    CHECK_FALSE(invalid->hasPendingImport());
}
