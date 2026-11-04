#include "bin/clipcreator.hpp"
#include "bin/projectfolder.h"
#include <QUrl>

TEST_CASE("MCP operates on an existing document and native bin sources outside its import session", "[MCP][Resume]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("existing.jpg"));
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(Qt::blue);
    REQUIRE(image.save(path, "JPEG"));
    McpProject project;
    project.open();
    project.document->setUrl(QUrl::fromLocalFile(directory.filePath(QStringLiteral("existing.kdenlive"))));
    auto nativeBin = pCore->projectItemModel();
    Fun undo = [] { return true; }, redo = [] { return true; };
    const QString binId = ClipCreator::createClipFromFile(path, nativeBin->getRootFolder()->clipId(), nativeBin, undo, redo);
    REQUIRE(binId != QStringLiteral("-1"));
    project.awaitMediaReady(binId);
    const auto initial = project.state();
    CHECK_FALSE(initial.value(QStringLiteral("freshProjectEligible")).toBool());
    CHECK_FALSE(project.binItem(binId).value(QStringLiteral("imported")).toBool());
    auto inserted =
        project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.videoTracks.at(0), 0, 0, 10, QStringLiteral("video")));
    const int clipId = inserted.value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(project.timeline->isClip(clipId));
    CHECK(project.timeline->getClipPlaytime(clipId) == 10);
    auto args = project.expected();
    args.insert(QStringLiteral("clipId"), clipId);
    args.insert(QStringLiteral("trackId"), project.videoTracks.at(1));
    args.insert(QStringLiteral("startFrame"), 20);
    project.success(QStringLiteral("move_clip"), args);
    CHECK(project.timeline->getClipPosition(clipId) == 20);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getClipPosition(clipId) == 0);
    REQUIRE(project.timeline->checkConsistency());
}
TEST_CASE("MCP imports JPEG through the same asynchronous native loader", "[MCP][Resume]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("import.jpg"));
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(Qt::green);
    REQUIRE(image.save(path, "JPEG"));
    McpProject project;
    project.open();
    auto args = project.expected();
    args.insert(QStringLiteral("path"), path);
    const auto imported = project.success(QStringLiteral("import_media"), args);
    project.awaitMediaReady(imported.value(QStringLiteral("binId")).toString());
    CHECK(project.binItem(imported.value(QStringLiteral("binId")).toString()).value(QStringLiteral("mediaType")).toString() == QStringLiteral("image"));
}

TEST_CASE("MCP state tokens include manually edited subtitle style and metadata", "[MCP][Resume]")
{
    McpProject project;
    project.open();
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("startFrame"), 0);
    args.insert(QStringLiteral("endFrame"), 10);
    args.insert(QStringLiteral("text"), QStringLiteral("Native subtitle"));
    project.success(QStringLiteral("edit_subtitle"), args);
    const auto model = project.timeline->getSubtitleModel();
    REQUIRE(model);
    REQUIRE_FALSE(model->getAllSubtitleStyles().empty());
    auto style = *model->getAllSubtitleStyles().begin();
    const auto previous = project.token();
    const int undoIndex = project.undoStack->index();
    style.second.setFontSize(style.second.fontSize() + 1);
    model->setSubtitleStyle(style.first, style.second);
    CHECK(project.undoStack->index() == undoIndex);
    CHECK(project.token() != previous);
    project.rejectWithoutChange(QStringLiteral("undo"), {{QStringLiteral("expectedState"), previous}});
}
