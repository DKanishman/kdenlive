/* Native MCP asset/effect/transition/title/subtitle regression cases. */
#include <QFontInfo>
#include "bin/model/subtitlemodel.hpp"
#include "effects/effectsrepository.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "jobs/filtertask.h"
#include "transitions/transitionsrepository.hpp"
#include <QUndoGroup>

namespace {
QJsonObject effectArgs(McpProject &project, const QString &scope, const QString &id, const QString &operation)
{
    auto args = project.expected();
    args.insert(QStringLiteral("scope"), scope);
    if (!id.isEmpty()) args.insert(QStringLiteral("targetId"), id);
    args.insert(QStringLiteral("operation"), operation);
    return args;
}
QJsonObject effectEntry(const QJsonObject &reply, const QString &assetId)
{
    for (const auto &value : reply.value(QStringLiteral("stack")).toObject().value(QStringLiteral("effects")).toArray()) {
        const auto entry = value.toObject();
        if (entry.value(QStringLiteral("assetId")).toString() == assetId) return entry;
    }
    FAIL("Native effect was absent from readback");
    return {};
}
} // namespace

TEST_CASE("MCP installed effect catalog is paginated and reports native definitions", "[mcp][assets]")
{
    McpProject project;
    project.open();
    const auto first = project.success(QStringLiteral("asset_catalog"), {{QStringLiteral("kind"), QStringLiteral("effect")}, {QStringLiteral("limit"), 2}});
    REQUIRE(first.value(QStringLiteral("assets")).toArray().size() == 2);
    REQUIRE(first.value(QStringLiteral("total")).toInt() > 2);
    REQUIRE(first.value(QStringLiteral("nextOffset")).toInt() == 2);
    const auto detail = project.success(QStringLiteral("asset_catalog"),
                                        {{QStringLiteral("kind"), QStringLiteral("effect")}, {QStringLiteral("assetId"), QStringLiteral("brightness")}});
    REQUIRE(detail.value(QStringLiteral("assets")).toArray().size() == 1);
    CHECK_FALSE(detail.value(QStringLiteral("assets")).toArray().first().toObject().value(QStringLiteral("parameterDefinitions")).toArray().isEmpty());
    CHECK(detail.value(QStringLiteral("interpolationTypes")).toArray().contains(QStringLiteral("CubicIn")));
    project.rejectWithoutChange(QStringLiteral("asset_catalog"),
                                {{QStringLiteral("kind"), QStringLiteral("effect")}, {QStringLiteral("assetId"), QStringLiteral("not-installed-mcp-test")}});
}

TEST_CASE("MCP general effects preserve native parameter values and undo", "[mcp][effects]")
{
    McpProject project;
    project.open();
    const int clipId =
        project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 40)).value(QStringLiteral("clipId")).toInt();
    const auto stack = project.timeline->getClipEffectStackModel(clipId);
    auto add = effectArgs(project, QStringLiteral("clip"), QString::number(clipId), QStringLiteral("add"));
    add.insert(QStringLiteral("effectId"), QStringLiteral("brightness"));
    const int beforeAdd = project.undoStack->index();
    const auto applied = project.success(QStringLiteral("edit_effect"), add);
    CHECK(project.undoStack->index() == beforeAdd + 1);
    const auto entry = effectEntry(applied, QStringLiteral("brightness"));
    const int instanceId = entry.value(QStringLiteral("instanceId")).toInt(-1);
    REQUIRE(instanceId >= 0);
    CHECK_FALSE(entry.value(QStringLiteral("parameterSchema")).toArray().isEmpty());
    auto set = effectArgs(project, QStringLiteral("clip"), QString::number(clipId), QStringLiteral("parameters"));
    set.insert(QStringLiteral("instanceId"), instanceId);
    set.insert(QStringLiteral("parameters"), QJsonObject{{QStringLiteral("level"), 0.5}});
    project.success(QStringLiteral("edit_effect"), set);
    auto native = std::dynamic_pointer_cast<EffectItemModel>(stack->getEffectStackRow(stack->rowCount() - 1));
    REQUIRE(native);
    CHECK(native->filter().anim_get_double("level", 0) == Approx(0.5));
    const int beforeInvalid = project.undoStack->index();
    set = effectArgs(project, QStringLiteral("clip"), QString::number(clipId), QStringLiteral("parameters"));
    set.insert(QStringLiteral("instanceId"), instanceId);
    set.insert(QStringLiteral("parameters"), QJsonObject{{QStringLiteral("mlt_service"), QStringLiteral("injected")}});
    project.rejectWithoutChange(QStringLiteral("edit_effect"), set);
    CHECK(project.undoStack->index() == beforeInvalid);
    auto enable = effectArgs(project, QStringLiteral("clip"), QString::number(clipId), QStringLiteral("enable"));
    enable.insert(QStringLiteral("instanceId"), instanceId);
    enable.insert(QStringLiteral("enabled"), false);
    project.success(QStringLiteral("edit_effect"), enable);
    CHECK_FALSE(native->isAssetEnabled());
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(native->isAssetEnabled());
    auto remove = effectArgs(project, QStringLiteral("clip"), QString::number(clipId), QStringLiteral("remove"));
    remove.insert(QStringLiteral("instanceId"), instanceId);
    project.success(QStringLiteral("edit_effect"), remove);
    CHECK_FALSE(stack->hasFilter(QStringLiteral("brightness")));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(stack->hasFilter(QStringLiteral("brightness")));
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP native animation supports easing and exact endpoint frames", "[mcp][effects][keyframes]")
{
    McpProject project;
    project.open();
    const int clipId =
        project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 25)).value(QStringLiteral("clipId")).toInt();
    auto add = effectArgs(project, QStringLiteral("clip"), QString::number(clipId), QStringLiteral("add"));
    add.insert(QStringLiteral("effectId"), QStringLiteral("brightness"));
    const int instanceId =
        effectEntry(project.success(QStringLiteral("edit_effect"), add), QStringLiteral("brightness")).value(QStringLiteral("instanceId")).toInt();
    auto args = project.expected();
    args.insert(QStringLiteral("scope"), QStringLiteral("clip"));
    args.insert(QStringLiteral("targetId"), QString::number(clipId));
    args.insert(QStringLiteral("instanceId"), instanceId);
    args.insert(QStringLiteral("keyframes"), QJsonArray{QJsonObject{{QStringLiteral("frame"), 0},
                                                                    {QStringLiteral("interpolation"), QStringLiteral("CubicIn")},
                                                                    {QStringLiteral("values"), QJsonObject{{QStringLiteral("level"), 0.5}}}},
                                                        QJsonObject{{QStringLiteral("frame"), 24},
                                                                    {QStringLiteral("interpolation"), QStringLiteral("Linear")},
                                                                    {QStringLiteral("values"), QJsonObject{{QStringLiteral("level"), 2.0}}}}});
    project.success(QStringLiteral("set_effect_keyframes"), args);
    auto stack = project.timeline->getClipEffectStackModel(clipId);
    auto effect = std::dynamic_pointer_cast<EffectItemModel>(stack->getEffectStackRow(stack->rowCount() - 1));
    REQUIRE(effect);
    CHECK(effect->filter().anim_get_double("level", 0) == Approx(0.5));
    CHECK(effect->filter().anim_get_double("level", 24) == Approx(2.0));
    CHECK(mlt_animation_key_count(effect->filter().get_animation("level")) == 2);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(effect->filter().anim_get_double("level", 0) == Approx(1.0));
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(effect->filter().anim_get_double("level", 24) == Approx(2.0));
}

TEST_CASE("MCP native cross-track compositions support exact geometry and undo", "[mcp][composition]")
{
    McpProject project;
    project.open();
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.at(0), 0, 40));
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.at(1), 0, 40));
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("transitionId"), QStringLiteral("luma"));
    args.insert(QStringLiteral("trackId"), project.videoTracks.at(1));
    args.insert(QStringLiteral("aTrackId"), project.videoTracks.at(0));
    args.insert(QStringLiteral("startFrame"), 5);
    args.insert(QStringLiteral("durationFrames"), 20);
    const auto added = project.success(QStringLiteral("edit_composition"), args);
    const int id = added.value(QStringLiteral("compositionId")).toInt(-1);
    REQUIRE(id >= 0);
    REQUIRE(project.timeline->isComposition(id));
    CHECK(project.timeline->getCompositionPosition(id) == 5);
    CHECK(project.timeline->getCompositionPlaytime(id) == 20);
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("resize"));
    args.insert(QStringLiteral("compositionId"), id);
    args.insert(QStringLiteral("durationFrames"), 10);
    project.success(QStringLiteral("edit_composition"), args);
    CHECK(project.timeline->getCompositionPlaytime(id) == 10);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getCompositionPlaytime(id) == 20);
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("delete"));
    args.insert(QStringLiteral("compositionId"), id);
    project.success(QStringLiteral("edit_composition"), args);
    CHECK_FALSE(project.timeline->isComposition(id));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->isComposition(id));
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP same-track transitions preserve their edit point on resize", "[mcp][mix]")
{
    McpProject project;
    project.open();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto args = project.expected();
    args.insert(QStringLiteral("path"), avFixture(directory));
    const QString binId = project.success(QStringLiteral("import_media"), args).value(QStringLiteral("binId")).toString();
    project.awaitMediaReady(binId);
    const int first =
        project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.videoTracks.front(), 0, 0, 8, QStringLiteral("video")))
            .value(QStringLiteral("clipId"))
            .toInt();
    const int second =
        project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.videoTracks.front(), 8, 8, 8, QStringLiteral("video")))
            .value(QStringLiteral("clipId"))
            .toInt();
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("transitionId"), QStringLiteral("dissolve"));
    args.insert(QStringLiteral("firstClipId"), first);
    args.insert(QStringLiteral("secondClipId"), second);
    args.insert(QStringLiteral("beforeFrames"), 2);
    args.insert(QStringLiteral("afterFrames"), 2);
    project.success(QStringLiteral("edit_mix"), args);
    CHECK(project.timeline->getMixDuration(second) == 4);
    CHECK(project.timeline->getMixCutPos(second) == 2);
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("resize"));
    args.insert(QStringLiteral("secondClipId"), second);
    args.insert(QStringLiteral("beforeFrames"), 1);
    args.insert(QStringLiteral("afterFrames"), 3);
    project.success(QStringLiteral("edit_mix"), args);
    CHECK(project.timeline->getClipPosition(second) == 7);
    CHECK(project.timeline->getMixCutPos(second) == 3);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getClipPosition(second) == 6);
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP title creation retains editable native Unicode text and shapes", "[mcp][title]")
{
    McpProject project;
    project.open();
    auto args = project.expected();
    args.insert(QStringLiteral("durationFrames"), 25);
    args.insert(QStringLiteral("items"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                                                {QStringLiteral("x"), 20},
                                                                {QStringLiteral("y"), 20},
                                                                {QStringLiteral("width"), 300},
                                                                {QStringLiteral("height"), 150},
                                                                {QStringLiteral("fill"), QStringLiteral("#102040")},
                                                                {QStringLiteral("cornerRadius"), 12}},
                                                    QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                {QStringLiteral("text"), QStringLiteral("Native MCP — שלום")},
                                                                {QStringLiteral("x"), 30},
                                                                {QStringLiteral("y"), 40},
                                                                {QStringLiteral("width"), 280},
                                                                {QStringLiteral("fontSize"), 32},
                                                                {QStringLiteral("bold"), true},
                                                                {QStringLiteral("rotation"), 5.0}}});
    const auto result = project.success(QStringLiteral("create_title"), args);
    const QString binId = result.value(QStringLiteral("binId")).toString();
    REQUIRE_FALSE(binId.isEmpty());
    project.awaitMediaReady(binId);
    const auto clip = pCore->projectItemModel()->getClipByBinID(binId);
    REQUIRE(clip);
    CHECK(clip->clipType() == ClipType::Text);
    CHECK(clip->getFramePlaytime() == 25);
    const auto xmlText = clip->getProducerProperty(QStringLiteral("xmldata"));
    QDomDocument title;
    REQUIRE(title.setContent(xmlText));
    CHECK(title.documentElement().attribute(QStringLiteral("duration")).toInt() == 25);
    CHECK(title.elementsByTagName(QStringLiteral("item")).size() == 2);
    const auto objects = title.elementsByTagName(QStringLiteral("item"));
    for (int index = 0; index < objects.size(); ++index) {
        const auto object = objects.at(index).toElement();
        if (object.attribute(QStringLiteral("type")) != QStringLiteral("QGraphicsTextItem")) continue;
        const QString family = object.firstChildElement(QStringLiteral("content")).attribute(QStringLiteral("font"));
        REQUIRE_FALSE(family.isEmpty());
        // This is the native saved-project missing-font check: a default title
        // must not require repair merely because a generic alias was persisted.
        CHECK(QFontInfo(QFont(family)).family() == family);
    }
    CHECK(xmlText.contains(QStringLiteral("שלום")));
    CHECK(xmlText.contains(QStringLiteral("0,0,5")));
}

TEST_CASE("MCP subtitle creation editing layers and deletion use native undo", "[mcp][subtitle]")
{
    McpProject project;
    project.open();
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("text"), QStringLiteral("Hello — שלום"));
    args.insert(QStringLiteral("startFrame"), 5);
    args.insert(QStringLiteral("endFrame"), 30);
    const auto result = project.success(QStringLiteral("edit_subtitle"), args);
    const int id = result.value(QStringLiteral("id")).toInt(-1);
    REQUIRE(id >= 0);
    const auto model = project.timeline->getSubtitleModel();
    REQUIRE(model);
    REQUIRE(model->hasSubtitle(id));
    CHECK(model->getText(id) == QStringLiteral("Hello — שלום"));
    CHECK(model->getSubtitlePlaytime(id) == 25);
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("update"));
    args.insert(QStringLiteral("id"), id);
    args.insert(QStringLiteral("text"), QStringLiteral("Edited"));
    args.insert(QStringLiteral("startFrame"), 10);
    args.insert(QStringLiteral("endFrame"), 35);
    project.success(QStringLiteral("edit_subtitle"), args);
    CHECK(model->getText(id) == QStringLiteral("Edited"));
    CHECK(model->getSubtitlePosition(id).frames(25) == 10);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(model->getText(id) == QStringLiteral("Hello — שלום"));
    CHECK(model->getSubtitlePosition(id).frames(25) == 5);
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("layer_add"));
    project.success(QStringLiteral("edit_subtitle"), args);
    CHECK(model->getMaxLayer() == 1);
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("remove"));
    args.insert(QStringLiteral("id"), id);
    project.success(QStringLiteral("edit_subtitle"), args);
    CHECK_FALSE(model->hasSubtitle(id));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(model->hasSubtitle(id));
    CHECK(QFileInfo(model->getUrl()).isFile());
}

#include "kdenlivesettings.h"
#include <QScopeGuard>

TEST_CASE("MCP native filter analysis completes only after a real loudness result", "[mcp][analysis]")
{
    McpProject project;
    project.open();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString melt = QStandardPaths::findExecutable(QStringLiteral("melt"));
    REQUIRE_FALSE(melt.isEmpty());
    const QString previousMelt = KdenliveSettings::meltpath();
    const auto restoreMelt = qScopeGuard([previousMelt] { KdenliveSettings::setMeltpath(previousMelt); });
    KdenliveSettings::setMeltpath(melt);
    auto args = project.expected();
    args.insert(QStringLiteral("path"), wavFixture(directory));
    const QString binId = project.success(QStringLiteral("import_media"), args).value(QStringLiteral("binId")).toString();
    project.awaitMediaReady(binId);
    const int clipId = project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.audioTrack, 0, 0, 25, QStringLiteral("audio")))
                           .value(QStringLiteral("clipId"))
                           .toInt();
    args = effectArgs(project, QStringLiteral("clip"), QString::number(clipId), QStringLiteral("add"));
    args.insert(QStringLiteral("effectId"), QStringLiteral("loudness"));
    const int instanceId =
        effectEntry(project.success(QStringLiteral("edit_effect"), args), QStringLiteral("loudness")).value(QStringLiteral("instanceId")).toInt();
    args = project.expected();
    args.insert(QStringLiteral("scope"), QStringLiteral("clip"));
    args.insert(QStringLiteral("targetId"), QString::number(clipId));
    args.insert(QStringLiteral("instanceId"), instanceId);
    const QString jobId = project.success(QStringLiteral("start_effect_analysis"), args).value(QStringLiteral("jobId")).toString();
    REQUIRE_FALSE(jobId.isEmpty());
    QElapsedTimer deadline;
    deadline.start();
    QJsonObject job;
    while (deadline.elapsed() < 30000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        job = project.success(QStringLiteral("get_effect_analysis"), {{QStringLiteral("jobId"), jobId}}).value(QStringLiteral("job")).toObject();
        const QString status = job.value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("completed") || status == QStringLiteral("failed") || status == QStringLiteral("obsolete")) break;
        QThread::msleep(10);
    }
    INFO(QJsonDocument(job).toJson(QJsonDocument::Compact).constData());
    REQUIRE(job.value(QStringLiteral("status")).toString() == QStringLiteral("completed"));
    CHECK(job.value(QStringLiteral("resultObserved")).toBool());
    CHECK_FALSE(job.value(QStringLiteral("result")).toString().isEmpty());
    CHECK(job.value(QStringLiteral("progress")).toInt() == 100);
    const auto stack = project.timeline->getClipEffectStackModel(clipId);
    const auto effect = std::dynamic_pointer_cast<EffectItemModel>(stack->getEffectStackRow(stack->rowCount() - 1));
    REQUIRE(effect);
    CHECK(effect->getParam(QStringLiteral("results")) == job.value(QStringLiteral("result")).toString());
    // A failed external process must reach failed even though native progress reaches 100.
    KdenliveSettings::setMeltpath(directory.filePath(QStringLiteral("missing-melt.exe")));
    args = project.expected();
    args.insert(QStringLiteral("scope"), QStringLiteral("clip"));
    args.insert(QStringLiteral("targetId"), QString::number(clipId));
    args.insert(QStringLiteral("instanceId"), instanceId);
    const QString failedId = project.success(QStringLiteral("start_effect_analysis"), args).value(QStringLiteral("jobId")).toString();
    deadline.restart();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
        job = project.success(QStringLiteral("get_effect_analysis"), {{QStringLiteral("jobId"), failedId}}).value(QStringLiteral("job")).toObject();
    } while (!job.value(QStringLiteral("nativeFinished")).toBool() && deadline.elapsed() < 10000);
    REQUIRE(job.value(QStringLiteral("status")).toString() == QStringLiteral("failed"));
    CHECK_FALSE(job.value(QStringLiteral("resultObserved")).toBool());
    args = effectArgs(project, QStringLiteral("clip"), QString::number(clipId), QStringLiteral("parameters"));
    args.insert(QStringLiteral("instanceId"), instanceId);
    args.insert(QStringLiteral("parameters"), QJsonObject{{QStringLiteral("program"), -24}});
    project.success(QStringLiteral("edit_effect"), args);
    job = project.success(QStringLiteral("get_effect_analysis"), {{QStringLiteral("jobId"), failedId}}).value(QStringLiteral("job")).toObject();
    CHECK(job.value(QStringLiteral("status")).toString() == QStringLiteral("failed"));
    CHECK_FALSE(job.value(QStringLiteral("resultObserved")).toBool());
    // Undo retains the effect model after removal. A result for that detached
    // object must not be reported as an effect applied to the current project.
    KdenliveSettings::setMeltpath(melt);
    args = project.expected();
    args.insert(QStringLiteral("scope"), QStringLiteral("clip"));
    args.insert(QStringLiteral("targetId"), QString::number(clipId));
    args.insert(QStringLiteral("instanceId"), instanceId);
    const QString removedId = project.success(QStringLiteral("start_effect_analysis"), args).value(QStringLiteral("jobId")).toString();
    args = effectArgs(project, QStringLiteral("clip"), QString::number(clipId), QStringLiteral("remove"));
    args.insert(QStringLiteral("instanceId"), instanceId);
    project.success(QStringLiteral("edit_effect"), args);
    deadline.restart();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
        job = project.success(QStringLiteral("get_effect_analysis"), {{QStringLiteral("jobId"), removedId}}).value(QStringLiteral("job")).toObject();
    } while (!job.value(QStringLiteral("nativeFinished")).toBool() && deadline.elapsed() < 30000);
    REQUIRE(job.value(QStringLiteral("nativeFinished")).toBool());
    CHECK(job.value(QStringLiteral("status")).toString() == QStringLiteral("obsolete"));
    CHECK_FALSE(job.value(QStringLiteral("resultObserved")).toBool());
}

TEST_CASE("MCP subtitle files export without overwriting and language tracks are selectable", "[mcp][subtitle][files]")
{
    McpProject project;
    project.open();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    const QString path = directory.filePath(QStringLiteral("source.srt"));
    QFile fixture(path);
    REQUIRE(fixture.open(QIODevice::WriteOnly));
    REQUIRE(fixture.write("1\n00:00:00,200 --> 00:00:01,200\nNative subtitle\n\n") > 0);
    fixture.close();
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("import"));
    args.insert(QStringLiteral("path"), path);
    const auto imported = project.success(QStringLiteral("subtitle_file"), args);
    CHECK(imported.value(QStringLiteral("importedCount")).toInt() == 1);
    const auto model = project.timeline->getSubtitleModel();
    REQUIRE(model);
    REQUIRE(model->count() == 1);
    const QString output = directory.filePath(QStringLiteral("export.ass"));
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("export"));
    args.insert(QStringLiteral("path"), output);
    project.success(QStringLiteral("subtitle_file"), args);
    REQUIRE(QFileInfo(output).isFile());
    QFile written(output);
    REQUIRE(written.open(QIODevice::ReadOnly));
    CHECK(written.readAll().contains("Native subtitle"));
    written.close();
    args.insert(QStringLiteral("expectedState"), project.token());
    project.rejectWithoutChange(QStringLiteral("subtitle_file"), args);
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("create"));
    args.insert(QStringLiteral("name"), QStringLiteral("English"));
    args.insert(QStringLiteral("copyTrackId"), 0);
    const auto language = project.success(QStringLiteral("subtitle_language"), args);
    const int trackId = language.value(QStringLiteral("trackId")).toInt(-1);
    REQUIRE(trackId > 0);
    CHECK(language.value(QStringLiteral("undoable")).toBool());
    const int originalId = *model->getAllSubIds().begin();
    const int beforeSwitch = project.undoStack->index();
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("activate"));
    args.insert(QStringLiteral("trackId"), trackId);
    const auto active = project.success(QStringLiteral("subtitle_language"), args);
    CHECK(project.undoStack->index() == beforeSwitch + 1);
    CHECK(active.value(QStringLiteral("activeLanguageId")).toInt() == trackId);
    CHECK(model->count() == 1);
    CHECK(model->getText(*model->getAllSubIds().begin()) == QStringLiteral("Native subtitle"));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.success(QStringLiteral("get_subtitles")).value(QStringLiteral("activeLanguageId")).toInt() == 0);
    REQUIRE(model->hasSubtitle(originalId));
    CHECK(project.undoStack->index() == beforeSwitch);
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(project.success(QStringLiteral("get_subtitles")).value(QStringLiteral("activeLanguageId")).toInt() == trackId);
    REQUIRE(model->count() == 1);
    CHECK(model->getText(*model->getAllSubIds().begin()) == QStringLiteral("Native subtitle"));
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP grouped subtitle edits and single deletion preserve native group undo", "[mcp][subtitle][groups]")
{
    McpProject project;
    project.open();
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    const int videoId =
        project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 50)).value(QStringLiteral("clipId")).toInt();
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("text"), QStringLiteral("Grouped"));
    args.insert(QStringLiteral("startFrame"), 5);
    args.insert(QStringLiteral("endFrame"), 30);
    const int subId = project.success(QStringLiteral("edit_subtitle"), args).value(QStringLiteral("id")).toInt();
    const auto model = project.timeline->getSubtitleModel();
    REQUIRE(model);
    REQUIRE(project.timeline->requestClipsGroup({videoId, subId}) >= 0);
    REQUIRE(project.timeline->requestClearSelection());
    const auto group = project.timeline->getGroupElements(subId);
    REQUIRE(group.count(videoId) == 1);
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("update"));
    args.insert(QStringLiteral("id"), subId);
    args.insert(QStringLiteral("text"), QStringLiteral("Edited in place"));
    args.insert(QStringLiteral("startFrame"), 7);
    args.insert(QStringLiteral("endFrame"), 35);
    project.success(QStringLiteral("edit_subtitle"), args);
    CHECK(project.timeline->getGroupElements(subId) == group);
    CHECK(model->getText(subId) == QStringLiteral("Edited in place"));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getGroupElements(subId) == group);
    CHECK(model->getText(subId) == QStringLiteral("Grouped"));
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("remove"));
    args.insert(QStringLiteral("id"), subId);
    project.success(QStringLiteral("edit_subtitle"), args);
    CHECK_FALSE(model->hasSubtitle(subId));
    REQUIRE(project.timeline->isClip(videoId));
    project.success(QStringLiteral("undo"), project.expected());
    REQUIRE(model->hasSubtitle(subId));
    CHECK(project.timeline->getGroupElements(subId) == group);
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("Native subtitle language boundaries preserve document history IDs styles and groups", "[mcp][subtitle][language][undo]")
{
    McpProject project;
    project.open();
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    const int videoId =
        project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 75)).value(QStringLiteral("clipId")).toInt();
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("text"), QStringLiteral("Language A original"));
    args.insert(QStringLiteral("startFrame"), 5);
    args.insert(QStringLiteral("endFrame"), 30);
    const int originalId = project.success(QStringLiteral("edit_subtitle"), args).value(QStringLiteral("id")).toInt();
    const auto model = project.timeline->getSubtitleModel();
    REQUIRE(model);
    model->setSubtitleStyle(QStringLiteral("Alternate"), model->getSubtitleStyle(QStringLiteral("Default")));
    model->setStyleName(originalId, QStringLiteral("Alternate"));
    const int beforeGroup = project.undoStack->index();
    REQUIRE(project.timeline->requestClipsGroup({videoId, originalId}) >= 0);
    REQUIRE(project.timeline->requestClearSelection());
    const auto originalGroup = project.timeline->getGroupElements(originalId);
    REQUIRE(originalGroup.count(videoId) == 1);
    const int baseline = project.undoStack->index();
    REQUIRE(baseline == beforeGroup + 1);
    const auto oldLanguages = model->getSubtitlesList();
    REQUIRE(model->createNewSubtitle(QStringLiteral("Language B"), 0) > 0);
    int languageB = -1;
    for (const auto &key : model->getSubtitlesList().keys())
        if (!oldLanguages.contains(key)) languageB = key.first;
    REQUIRE(languageB > 0);
    const int afterCreate = project.undoStack->index();
    CHECK(afterCreate == baseline + 1);
    const auto readNativeFile = [](const QString &path) {
        QFile file(path);
        REQUIRE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    };
    const QString workA = project.document->subTitlePath(project.timeline->uuid(), 0, false);
    const QString workB = project.document->subTitlePath(project.timeline->uuid(), languageB, false);
    const auto originalABytes = readNativeFile(workA);
    REQUIRE(model->activateSubtitle(languageB));
    CHECK(readNativeFile(workA) == originalABytes);
    CHECK(readNativeFile(workB).contains("Language A original"));
    CHECK(project.undoStack->index() == afterCreate + 1);
    REQUIRE(model->count() == 1);
    const int bId = *model->getAllSubIds().begin();
    CHECK(bId != originalId);
    CHECK_FALSE(model->hasSubtitle(originalId));
    REQUIRE(project.timeline->checkConsistency());
    REQUIRE(model->setText(bId, QStringLiteral("Language B edited")));
    const auto editedBBytes = readNativeFile(workB);
    REQUIRE(model->activateSubtitle(0));
    CHECK(readNativeFile(workA) == originalABytes);
    CHECK(readNativeFile(workB) == editedBBytes);
    const int aAgainId = *model->getAllSubIds().begin();
    REQUIRE(aAgainId != bId);
    REQUIRE(model->setText(aAgainId, QStringLiteral("Language A second visit")));
    const int finalIndex = project.undoStack->index(), finalCount = project.undoStack->count();
    project.undoStack->undo();
    CHECK(model->getText(aAgainId) == QStringLiteral("Language A original"));
    project.undoStack->undo();
    CHECK(model->hasSubtitle(bId));
    CHECK(model->getText(bId) == QStringLiteral("Language B edited"));
    CHECK(readNativeFile(workA) == originalABytes);
    CHECK(readNativeFile(workB) == editedBBytes);
    REQUIRE(project.timeline->checkConsistency());
    project.undoStack->undo();
    CHECK(model->getText(bId) == QStringLiteral("Language A original"));
    project.undoStack->undo();
    REQUIRE(model->hasSubtitle(originalId));
    CHECK_FALSE(model->hasSubtitle(bId));
    CHECK(model->getStyleName(originalId) == QStringLiteral("Alternate"));
    CHECK(readNativeFile(workA) == originalABytes);
    CHECK(readNativeFile(workB).contains("Language A original"));
    CHECK_FALSE(readNativeFile(workB).contains("Language B edited"));
    CHECK(project.timeline->getGroupElements(originalId) == originalGroup);
    REQUIRE(project.timeline->checkConsistency());
    // Saving/exporting after a language undo must use the restored original live events.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString ass = directory.filePath(QStringLiteral("restored.ass"));
    REQUIRE(model->saveSubtitleData(model->toJson(), ass) == 1);
    QFile subtitle(ass);
    REQUIRE(subtitle.open(QIODevice::ReadOnly));
    const auto bytes = subtitle.readAll();
    CHECK(bytes.contains("Language A original"));
    CHECK(bytes.contains("Alternate"));
    project.undoStack->undo();
    CHECK(model->getSubtitlesList().size() == 1);
    REQUIRE(model->hasSubtitle(originalId));
    CHECK(project.timeline->getGroupElements(originalId) == originalGroup);
    project.undoStack->undo();
    CHECK_FALSE(project.timeline->isInGroup(originalId));
    project.undoStack->undo();
    CHECK(model->getStyleName(originalId) == QStringLiteral("Default"));
    while (project.undoStack->index() < finalIndex) {
        project.undoStack->redo();
        REQUIRE(project.timeline->checkConsistency());
    }
    CHECK(project.undoStack->count() == finalCount);
    REQUIRE(model->hasSubtitle(aAgainId));
    CHECK(model->getText(aAgainId) == QStringLiteral("Language A second visit"));
}

TEST_CASE("Rejected initial subtitle commands do not create model state or files", "[mcp][subtitle][guards]")
{
    McpProject project;
    project.open();
    REQUIRE_FALSE(project.timeline->hasSubtitleModel());
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("layer_add"));
    args.insert(QStringLiteral("layer"), 50);
    project.rejectWithoutChange(QStringLiteral("edit_subtitle"), args);
    CHECK_FALSE(project.timeline->hasSubtitleModel());
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("startFrame"), 0);
    args.insert(QStringLiteral("endFrame"), 10);
    args.insert(QStringLiteral("text"), QStringLiteral("Invalid style"));
    args.insert(QStringLiteral("style"), QStringLiteral("Absent"));
    project.rejectWithoutChange(QStringLiteral("edit_subtitle"), args);
    CHECK_FALSE(project.timeline->hasSubtitleModel());
}

TEST_CASE("MCP complex native effect parameters roundtrip their editable representations", "[mcp][effects][parameters]")
{
    struct Example
    {
        QString asset;
        QString parameter;
        QString value;
    };
    const std::vector<Example> examples{
        {QStringLiteral("fade_from_black"), QStringLiteral("level\nalpha"), QStringLiteral("0=0;-1=1\n1")},
        {QStringLiteral("avfilter.curves"), QStringLiteral("av.master"), QStringLiteral("0/0;0.5/0.8;1/1;")},
        {QStringLiteral("gradientmap"), QStringLiteral("gradient"), QStringLiteral("#ff102030 0.0|#ff80a0ff 0.5|#ffffffff 1.0")}};
    for (const auto &example : examples) {
        INFO(example.asset.toStdString());
        REQUIRE(EffectsRepository::get()->exists(example.asset));
        McpProject project;
        project.open();
        const int id =
            project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 40)).value(QStringLiteral("clipId")).toInt();
        auto args = effectArgs(project, QStringLiteral("clip"), QString::number(id), QStringLiteral("add"));
        args.insert(QStringLiteral("effectId"), example.asset);
        const auto added = effectEntry(project.success(QStringLiteral("edit_effect"), args), example.asset);
        const int instanceId = added.value(QStringLiteral("instanceId")).toInt();
        const auto stack = project.timeline->getClipEffectStackModel(id);
        const auto effect = std::dynamic_pointer_cast<EffectItemModel>(stack->getEffectStackRow(stack->rowCount() - 1));
        REQUIRE(effect);
        const auto before = effect->getParamFromName(example.parameter).toString();
        args = effectArgs(project, QStringLiteral("clip"), QString::number(id), QStringLiteral("parameters"));
        args.insert(QStringLiteral("instanceId"), instanceId);
        args.insert(QStringLiteral("parameters"), QJsonObject{{example.parameter, example.value}});
        const auto changed = effectEntry(project.success(QStringLiteral("edit_effect"), args), example.asset);
        const auto logical = effect->getParamFromName(example.parameter).toString();
        CHECK(changed.value(QStringLiteral("parameters")).toObject().value(example.parameter).toString() == logical);
        REQUIRE_FALSE(logical.isEmpty());
        // Feed the advertised editable value back unchanged; it must not use raw MLT syntax.
        args.insert(QStringLiteral("expectedState"), project.token());
        args.insert(QStringLiteral("parameters"), QJsonObject{{example.parameter, logical}});
        project.success(QStringLiteral("edit_effect"), args);
        CHECK(effect->getParamFromName(example.parameter).toString() == logical);
        project.success(QStringLiteral("undo"), project.expected());
        CHECK(effect->getParamFromName(example.parameter).toString() == logical);
        project.success(QStringLiteral("undo"), project.expected());
        CHECK(effect->getParamFromName(example.parameter).toString() == before);
        REQUIRE(project.timeline->checkConsistency());
    }
}

TEST_CASE("Malformed ASS sections are rejected before native subtitle state is created", "[mcp][subtitle][files][guards]")
{
    for (const QByteArray &contents :
         {QByteArray("[V4+ Styles]\ngarbage\n\n[Events]\nDialogue: 0,0:00:00.20,0:00:01.00,Default,,0,0,0,,Text\n"),
          QByteArray("[Kdenlive Extradata]\nMaxLayer\n\n[Events]\nDialogue: 0,0:00:00.20,0:00:01.00,Default,,0,0,0,,Text\n"),
          QByteArray("[Kdenlive Extradata]\nDefaultStyles\n\n[Events]\nDialogue: 0,0:00:00.20,0:00:01.00,Default,,0,0,0,,Text\n")}) {
        McpProject project;
        project.open();
        QTemporaryDir directory;
        REQUIRE(directory.isValid());
        QFile fixture(directory.filePath(QStringLiteral("invalid.ass")));
        REQUIRE(fixture.open(QIODevice::WriteOnly));
        REQUIRE(fixture.write(contents) == contents.size());
        fixture.close();
        auto args = project.expected();
        args.insert(QStringLiteral("operation"), QStringLiteral("import"));
        args.insert(QStringLiteral("path"), fixture.fileName());
        project.rejectWithoutChange(QStringLiteral("subtitle_file"), args);
        CHECK_FALSE(project.timeline->hasSubtitleModel());
    }
}

TEST_CASE("Native subtitle language undo preserves published assets across save and Save As", "[mcp][subtitle][language][save][undo]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 75));
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("text"), QStringLiteral("Published language A"));
    args.insert(QStringLiteral("startFrame"), 5);
    args.insert(QStringLiteral("endFrame"), 30);
    project.success(QStringLiteral("edit_subtitle"), args);
    auto model = project.timeline->getSubtitleModel();
    REQUIRE(model);
    const auto readFile = [](const QString &path) {
        QFile file(path);
        REQUIRE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    };
    const auto saveNative = [&](const QString &path) {
        // Exercise the same native subtitle copy/restore operations as normal
        // Save As. The full document wrapper also calls the GUI Bin timewarp
        // handler, which is absent from this headless native fixture.
        project.document->setSequenceProperty(project.timeline->uuid(), QStringLiteral("subtitlesList"), model->subtitlesFilesToJson());
        for (const auto &entry : model->getSubtitlesList().keys()) {
            QString finalName = path;
            if (project.timeline->uuid() != project.document->uuid()) finalName += project.timeline->uuid().toString();
            if (entry.first > 0) finalName += QStringLiteral("-%1").arg(entry.first);
            const QFileInfo info(finalName);
            model->copySubtitle(info.dir().absoluteFilePath(info.fileName() + QStringLiteral(".ass")), entry.first, false, true);
        }
        REQUIRE(pCore->projectManager()->testSaveFileAs(path));
        const int active =
            project.document->getSequenceProperty(project.timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt();
        model->restoreTmpFile(active);
        project.document->setUrl(QUrl::fromLocalFile(path));
    };
    const QString projectA = directory.filePath(QStringLiteral("saved-A.kdenlive"));
    const QString projectB = directory.filePath(QStringLiteral("saved-B.kdenlive"));
    saveNative(projectA);
    const int beforeCreation = project.undoStack->index();
    REQUIRE(model->createNewSubtitle(QStringLiteral("Second language"), 0) > 0);
    int languageB = -1;
    for (const auto &key : model->getSubtitlesList().keys())
        if (key.first > 0) languageB = key.first;
    REQUIRE(languageB > 0);
    const QString publishedA0 = project.document->subTitlePath(project.timeline->uuid(), 0, true);
    const QString publishedA1 = project.document->subTitlePath(project.timeline->uuid(), languageB, true);
    const QString work0 = project.document->subTitlePath(project.timeline->uuid(), 0, false);
    const QString work1 = project.document->subTitlePath(project.timeline->uuid(), languageB, false);
    REQUIRE(model->activateSubtitle(languageB));
    REQUIRE(model->setText(*model->getAllSubIds().begin(), QStringLiteral("Published language B")));
    REQUIRE(model->activateSubtitle(0));
    saveNative(projectA);
    const QByteArray a0 = readFile(publishedA0), a1 = readFile(publishedA1);
    REQUIRE(a0.contains("Published language A"));
    REQUIRE(a1.contains("Published language B"));
    const int savedIndex = project.undoStack->index();
    // Same-file save followed by undo must keep that saved snapshot reopenable.
    while (project.undoStack->index() > beforeCreation)
        project.undoStack->undo();
    CHECK(readFile(publishedA0) == a0);
    CHECK(readFile(publishedA1) == a1);
    while (project.undoStack->index() < savedIndex)
        project.undoStack->redo();
    REQUIRE(model->count() == 1);
    saveNative(projectB);
    const QString publishedB0 = project.document->subTitlePath(project.timeline->uuid(), 0, true);
    const QString publishedB1 = project.document->subTitlePath(project.timeline->uuid(), languageB, true);
    const QByteArray b0 = readFile(publishedB0), b1 = readFile(publishedB1);
    // Commands created under A must resolve B's current work context and never
    // delete A's saved language assets or B's published snapshot.
    while (project.undoStack->index() > beforeCreation)
        project.undoStack->undo();
    CHECK_FALSE(QFileInfo::exists(work1));
    CHECK(readFile(publishedA0) == a0);
    CHECK(readFile(publishedA1) == a1);
    CHECK(readFile(publishedB0) == b0);
    CHECK(readFile(publishedB1) == b1);
    while (project.undoStack->index() < savedIndex)
        project.undoStack->redo();
    REQUIRE(model->deleteSubtitle(languageB));
    CHECK_FALSE(QFileInfo::exists(work1));
    CHECK(readFile(publishedA1) == a1);
    CHECK(readFile(publishedB1) == b1);
    project.undoStack->undo();
    CHECK(readFile(work1).contains("Published language B"));
    project.undoStack->redo();
    CHECK(readFile(publishedA1) == a1);
    CHECK(readFile(publishedB1) == b1);
    project.undoStack->undo();
    REQUIRE(project.timeline->checkConsistency());
    // Discard the live edits and reopen both actual saved Kdenlive projects.
    // This verifies native subtitle-filter initialization without GUI widgets.
    REQUIRE(pCore->window() == nullptr);
    const QUuid savedSequenceUuid = project.timeline->uuid();
    model.reset();
    project.timeline.reset();
    REQUIRE(pCore->projectManager()->closeCurrentDocument(false, false));
    for (const QString &path : {projectA, projectB}) {
        QFile::remove(work0);
        QFile::remove(work1);
        QUndoGroup undoGroup;
        auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(path), directory.path(), &undoGroup, false, nullptr);
        REQUIRE(opened.isSuccessful());
        auto document = opened.getDocument();
        pCore->projectManager()->testSetDocument(document.get());
        REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(path).lastModified(), false));
        // Headless updateTimeline restores the bin/MLT sequence producers; like
        // the native sequence tests, explicitly open the saved sequence model.
        const auto sequences = pCore->projectItemModel()->getAllSequenceClips();
        REQUIRE(sequences.contains(savedSequenceUuid));
        REQUIRE_FALSE(sequences.value(savedSequenceUuid).isEmpty());
        REQUIRE(pCore->projectManager()->openTimeline(sequences.value(savedSequenceUuid), -1, savedSequenceUuid));
        auto timeline = document->getTimeline(savedSequenceUuid);
        REQUIRE(timeline);
        pCore->projectManager()->testSetActiveTimeline(timeline);
        document->loading = false;
        auto subtitles = timeline->getSubtitleModel();
        REQUIRE(subtitles);
        REQUIRE(subtitles->count() == 1);
        CHECK(subtitles->getText(*subtitles->getAllSubIds().begin()) == QStringLiteral("Published language A"));
        REQUIRE(subtitles->activateSubtitle(languageB));
        REQUIRE(subtitles->count() == 1);
        CHECK(subtitles->getText(*subtitles->getAllSubIds().begin()) == QStringLiteral("Published language B"));
        CHECK(timeline->checkConsistency());
        subtitles.reset();
        timeline.reset();
        REQUIRE(pCore->projectManager()->closeCurrentDocument(false, false));
    }
}

TEST_CASE("Native analysis task deletion before worker start signals terminal cancellation", "[mcp][effects][analysis][cancel]")
{
    McpProject project;
    project.open();
    auto *task = new FilterTask(ObjectId(KdenliveObjectType::BinClip, -1, QUuid()), QString(), {}, QStringLiteral("loudness"), 0, 0, {}, {}, {}, nullptr);
    int completions = 0;
    bool succeeded = true, canceled = false;
    QObject::connect(task, &FilterTask::analysisFinished, QCoreApplication::instance(),
                     [&](bool success, bool cancelled, const QString &key, const QString &value) {
                         ++completions;
                         succeeded = success;
                         canceled = cancelled;
                         CHECK(key.isEmpty());
                         CHECK(value.isEmpty());
                     });
    delete task;
    CHECK(completions == 1);
    CHECK_FALSE(succeeded);
    CHECK(canceled);
}

TEST_CASE("Native locked subtitle language activation loads events without exposing an unlocked state", "[mcp][subtitle][language][locked][undo]")
{
    McpProject project;
    project.open();
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("text"), QStringLiteral("Locked language content"));
    args.insert(QStringLiteral("startFrame"), 5);
    args.insert(QStringLiteral("endFrame"), 30);
    const int originalId = project.success(QStringLiteral("edit_subtitle"), args).value(QStringLiteral("id")).toInt();
    const auto model = project.timeline->getSubtitleModel();
    REQUIRE(model);
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("lock"));
    args.insert(QStringLiteral("enabled"), true);
    project.success(QStringLiteral("edit_subtitle"), args);
    REQUIRE(model->isLocked());
    REQUIRE(model->createNewSubtitle(QStringLiteral("Locked second language"), 0) > 0);
    int languageB = -1;
    for (const auto &entry : model->getSubtitlesList().keys())
        if (entry.first > 0) languageB = entry.first;
    REQUIRE(languageB > 0);
    QObject observer;
    int changes = 0;
    QObject::connect(model.get(), &SubtitleModel::activeSubtitleChanged, &observer, [&] {
        ++changes;
        CHECK(model->isLocked());
    });
    QObject::connect(model.get(), &SubtitleModel::modelReset, &observer, [&] { CHECK(model->isLocked()); });
    REQUIRE(model->activateSubtitle(languageB));
    REQUIRE(model->count() == 1);
    const int secondId = *model->getAllSubIds().begin();
    CHECK(model->getText(secondId) == QStringLiteral("Locked language content"));
    CHECK(model->isLocked());
    CHECK(project.timeline->checkConsistency());
    // Ordinary MCP edits and imports must still obey the observable native lock.
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("update"));
    args.insert(QStringLiteral("id"), secondId);
    args.insert(QStringLiteral("text"), QStringLiteral("Forbidden edit"));
    project.rejectWithoutChange(QStringLiteral("edit_subtitle"), args);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QFile srt(directory.filePath(QStringLiteral("locked.srt")));
    REQUIRE(srt.open(QIODevice::WriteOnly));
    REQUIRE(srt.write("1\n00:00:02,000 --> 00:00:03,000\nForbidden import\n\n") > 0);
    srt.close();
    args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("import"));
    args.insert(QStringLiteral("path"), srt.fileName());
    project.rejectWithoutChange(QStringLiteral("subtitle_file"), args);
    project.undoStack->undo();
    REQUIRE(model->hasSubtitle(originalId));
    CHECK(model->getText(originalId) == QStringLiteral("Locked language content"));
    CHECK(model->isLocked());
    project.undoStack->redo();
    REQUIRE(model->hasSubtitle(secondId));
    CHECK(model->isLocked());
    CHECK(changes == 3);
    CHECK(project.timeline->checkConsistency());
}
