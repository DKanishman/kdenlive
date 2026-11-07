/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "test_utils.hpp"
#include "bin/projectfolder.h"
#include "mcp/mcpserver.h"

#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>

namespace {
QString key(const char *value)
{
    return QString::fromLatin1(value);
}

class ExtrasProject
{
public:
    ExtrasProject()
        : undoStack(std::make_shared<DocUndoStack>(nullptr))
    {
        pCore->projectItemModel()->clean();
        pCore->setCurrentProfile(key("dv_pal"));
        document = std::make_unique<KdenliveDoc>(undoStack);
        pCore->projectManager()->testSetDocument(document.get());
        REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), false));
        timeline = document->getTimeline(document->uuid());
        REQUIRE(timeline);
        pCore->projectManager()->testSetActiveTimeline(timeline);
        document->loading = false;
        undoStack->clear();
        const QJsonObject params{{key("protocolVersion"), key("2025-06-18")},
                                 {key("capabilities"), QJsonObject{}},
                                 {key("clientInfo"), QJsonObject{{key("name"), key("extras-test")}, {key("version"), key("1")}}}};
        const QJsonObject reply = server.handleRequest({{key("jsonrpc"), key("2.0")},
                                                        {key("id"), ++requestId},
                                                        {key("method"), key("initialize")},
                                                        {key("params"), params}});
        REQUIRE_FALSE(reply.contains(key("error")));
        CHECK(server.handleRequest({{key("jsonrpc"), key("2.0")}, {key("method"), key("notifications/initialized")}}).isEmpty());
    }

    ~ExtrasProject()
    {
        if (document && pCore->projectManager()->current() == document.get()) {
            pCore->projectManager()->closeCurrentDocument(false, false);
        }
    }

    QJsonObject call(const char *name, const QJsonObject &args = {})
    {
        const auto reply = server.handleRequest(
            {{key("jsonrpc"), key("2.0")},
             {key("id"), ++requestId},
             {key("method"), key("tools/call")},
             {key("params"), QJsonObject{{key("name"), key(name)}, {key("arguments"), args}}}});
        INFO(QJsonDocument(reply).toJson(QJsonDocument::Compact).constData());
        REQUIRE_FALSE(reply.contains(key("error")));
        REQUIRE(reply.value(key("result")).isObject());
        return reply.value(key("result")).toObject();
    }

    QJsonObject success(const char *name, const QJsonObject &args = {})
    {
        const auto reply = call(name, args);
        REQUIRE_FALSE(reply.value(key("isError")).toBool());
        const auto content = reply.value(key("structuredContent")).toObject();
        REQUIRE(content.value(key("ok")).toBool());
        return content;
    }

    void failure(const char *name, const QJsonObject &args, const char *code)
    {
        const auto reply = call(name, args);
        REQUIRE(reply.value(key("isError")).toBool());
        CHECK(reply.value(key("structuredContent")).toObject().value(key("error")).toObject().value(key("code")).toString() == key(code));
    }

    QJsonObject expected()
    {
        const auto state = success("get_state").value(key("state")).toObject();
        REQUIRE(state.value(key("ready")).toBool());
        REQUIRE_FALSE(state.value(key("expectedState")).toString().isEmpty());
        return {{key("expectedState"), state.value(key("expectedState"))}};
    }

    QJsonObject cleanup(const QJsonObject &inspection, const QJsonArray &ids)
    {
        auto args = expected();
        args.insert(key("cleanupToken"), inspection.value(key("cleanupToken")));
        args.insert(key("binIds"), ids);
        return args;
    }

    QString imageSource(const QString &path, const QString &parent)
    {
        auto bin = pCore->projectItemModel();
        auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), nullptr, path.toUtf8().constData());
        REQUIRE(producer->is_valid());
        producer->set("length", 50);
        producer->set_in_and_out(0, 49);
        QString id;
        Fun undo = [] { return true; }, redo = [] { return true; };
        REQUIRE(bin->requestAddBinClip(id, producer, parent, undo, redo));
        const auto clip = bin->getClipByBinID(id);
        REQUIRE(clip);
        REQUIRE(clip->statusReady());
        return id;
    }

    int videoTrack(const std::shared_ptr<TimelineItemModel> &model)
    {
        for (int position = 0; position < model->getTracksCount(); ++position) {
            const int id = model->getTrackIndexFromPosition(position);
            if (!model->isAudioTrack(id)) return id;
        }
        FAIL("Fixture needs a native video track");
        return -1;
    }

    std::shared_ptr<DocUndoStack> undoStack;
    std::unique_ptr<KdenliveDoc> document;
    std::shared_ptr<TimelineItemModel> timeline;
    KdenliveMcpServer server;
    int requestId{0};
};

QSet<QString> unusedIds(const QJsonObject &inspection)
{
    QSet<QString> ids;
    for (const auto &value : inspection.value(key("sources")).toArray()) ids.insert(value.toObject().value(key("binId")).toString());
    return ids;
}

QByteArray fileHash(const QString &path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
} // namespace

TEST_CASE("MCP reads native saved notes and refuses edits without the notes widget", "[McpProjectExtras]")
{
    ExtrasProject project;
    const QString html = QStringLiteral("<p>שלום 🎬 <a href=\"{00000000-0000-0000-0000-000000000001}!12\">00:00:00:12</a></p>");
    pCore->projectItemModel()->saveProperty(key("kdenlive:documentnotes"), html);
    const auto beforeState = project.expected().value(key("expectedState"));
    const auto notes = project.success("get_project_notes");
    CHECK(notes.value(key("html")).toString() == html);
    CHECK(notes.value(key("text")).toString().startsWith(QStringLiteral("שלום 🎬 ")));
    CHECK(notes.value(key("historyScope")).toString() == key("notes"));
    CHECK_FALSE(notes.value(key("guiAvailable")).toBool());
    CHECK_FALSE(notes.value(key("canUndo")).toBool());
    CHECK_FALSE(notes.value(key("timelineHistoryChanged")).toBool());
    REQUIRE(notes.value(key("anchors")).toArray().size() == 1);
    CHECK(notes.value(key("anchors")).toArray().first().toObject().value(key("href")).toString().endsWith(key("!12")));
    const int history = project.undoStack->count();
    auto args = project.expected();
    args.insert(key("expectedNotes"), notes.value(key("notesToken")));
    args.insert(key("operation"), key("replace"));
    args.insert(key("text"), key("replacement"));
    project.failure("edit_project_notes", args, "GUI_REQUIRED");
    CHECK(project.undoStack->count() == history);
    CHECK(project.success("get_project_notes").value(key("notesToken")) == notes.value(key("notesToken")));
    pCore->projectItemModel()->saveProperty(key("kdenlive:documentnotes"), key("<p>Manual saved-note change</p>"));
    CHECK(project.expected().value(key("expectedState")) != beforeState);
    project.failure("edit_project_notes", args, "STALE_STATE");
}

TEST_CASE("MCP cleanup removes exact unused attached sources with one undo and preserves shared files", "[McpProjectExtras]")
{
    ExtrasProject project;
    auto bin = pCore->projectItemModel();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const QString imagePath = files.filePath(key("shared.png"));
    QImage image(32, 24, QImage::Format_ARGB32);
    image.fill(Qt::red);
    REQUIRE(image.save(imagePath));
    const auto originalHash = fileHash(imagePath);
    const QString root = bin->getRootFolder()->clipId();
    QString folder;
    Fun undo = [] { return true; }, redo = [] { return true; };
    REQUIRE(bin->requestAddFolder(folder, key("Nested"), root, undo, redo));
    const QString usedAlias = project.imageSource(imagePath, root);
    const QString removeId = project.imageSource(imagePath, folder);
    const QString leaveId = project.imageSource(imagePath, root);
    const auto removedClip = bin->getClipByBinID(removeId);
    removedClip->setDescription(QStringLiteral("Preserve metadata שלום"));
    removedClip->setRating(4);
    QString zone;
    REQUIRE(bin->requestAddBinSubClip(zone, 2, 8, {}, removeId));
    int placed = -1;
    REQUIRE(project.timeline->requestClipInsertion(usedAlias, project.videoTrack(project.timeline), 0, placed, true, true, false));
    const auto prepared = project.success("inspect_unused_sources");
    CHECK(unusedIds(prepared) == QSet<QString>{removeId, leaveId});
    project.undoStack->clear();
    const auto result = project.success("clean_project", project.cleanup(prepared, {removeId}));
    CHECK(result.value(key("sourceFilesPreserved")).toBool());
    CHECK(result.value(key("undoable")).toBool());
    CHECK_FALSE(bin->hasClip(removeId));
    CHECK(bin->hasClip(leaveId));
    CHECK(bin->hasClip(usedAlias));
    CHECK_FALSE(bin->getItemByBinId(zone));
    CHECK(project.timeline->getClipBinId(placed) == usedAlias);
    CHECK(fileHash(imagePath) == originalHash);
    CHECK(project.undoStack->count() == 1);
    project.undoStack->undo();
    REQUIRE(bin->hasClip(removeId));
    CHECK(bin->getClipByBinID(removeId) == removedClip);
    CHECK(removedClip->description() == QStringLiteral("Preserve metadata שלום"));
    CHECK(removedClip->parent()->clipId() == folder);
    CHECK(bin->getItemByBinId(zone));
    CHECK(fileHash(imagePath) == originalHash);
    project.undoStack->redo();
    CHECK_FALSE(bin->hasClip(removeId));
    CHECK(fileHash(imagePath) == originalHash);
    CHECK(project.timeline->checkConsistency());
}

TEST_CASE("MCP cleanup refuses stale proposals duplicate IDs and sources used in inactive sequences", "[McpProjectExtras]")
{
    ExtrasProject project;
    auto bin = pCore->projectItemModel();
    const QString unused = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", bin, 20);
    const QString inactiveUsed = KdenliveTests::createProducer(pCore->getProjectProfile(), "green", bin, 20);
    const auto old = project.success("inspect_unused_sources");
    const QString secondaryId = ClipCreator::createPlaylistClip(key("Inactive"), {2, 2}, key("-1"), bin);
    QUuid secondaryUuid;
    const auto sequences = bin->getAllSequenceClips();
    for (auto item = sequences.begin(); item != sequences.end(); ++item)
        if (item.value() == secondaryId) secondaryUuid = item.key();
    REQUIRE_FALSE(secondaryUuid.isNull());
    const auto secondary = project.document->getTimeline(secondaryUuid);
    REQUIRE(secondary);
    pCore->projectManager()->testSetActiveTimeline(secondary);
    int placed = -1;
    REQUIRE(secondary->requestClipInsertion(inactiveUsed, project.videoTrack(secondary), 0, placed, true, true, false));
    pCore->projectManager()->testSetActiveTimeline(project.timeline);
    project.undoStack->clear();
    const auto prepared = project.success("inspect_unused_sources");
    CHECK(unusedIds(prepared) == QSet<QString>{unused});
    project.failure("clean_project", project.cleanup(old, {unused}), "STALE_CLEANUP");
    project.failure("clean_project", project.cleanup(prepared, {unused, unused}), "INVALID_BIN_ITEM");
    project.failure("clean_project", project.cleanup(prepared, {inactiveUsed}), "INVALID_BIN_ITEM");
    CHECK(bin->hasClip(unused));
    CHECK(bin->hasClip(inactiveUsed));
    CHECK(project.undoStack->count() == 0);
    CHECK(secondary->getClipBinId(placed) == inactiveUsed);
    project.success("clean_project", project.cleanup(prepared, {unused}));
    CHECK_FALSE(bin->hasClip(unused));
    CHECK(bin->hasClip(inactiveUsed));
    project.undoStack->undo();
    CHECK(bin->hasClip(unused));
    CHECK(secondary->getClipBinId(placed) == inactiveUsed);
}
