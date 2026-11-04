/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Shared native composition cuts preserve retained pixels, animation values and groups.
#include "doc/kthumb.h"
#include "timeline2/model/compositionmodel.hpp"
#include <QPainter>
#include <mlt++/MltTractor.h>
#include <mlt++/MltFrame.h>

namespace {
int zoneCompositionAt(McpProject &project, int track, int frame)
{
    for (int id : project.timeline->getItemsInRange(track, frame, frame + 1, true))
        if (project.timeline->isComposition(id)) return id;
    return -1;
}
QImage zoneFrame(McpProject &project, int frame)
{
    REQUIRE(project.timeline->tractor()->is_valid());
    const auto size = pCore->getCurrentFrameSize();
    auto image = KThumb::getFrame(*project.timeline->tractor(), frame, size.width(), size.height());
    REQUIRE_FALSE(image.isNull());
    return image;
}
int zoneComposition(McpProject &project, int track, const QString &asset, const QString &parameter, const QString &animation, int lower)
{
    auto properties = std::make_unique<Mlt::Properties>();
    properties->set("force_track", lower < 0 ? 0 : 1);
    properties->set(parameter.toUtf8().constData(), animation.toUtf8().constData());
    Fun undo = [] { return true; }, redo = [] { return true; };
    int composition = -1;
    REQUIRE(project.timeline->requestCompositionInsertion(asset, track, lower, 0, 20, std::move(properties), composition, undo, redo, true));
    REQUIRE(composition >= 0);
    pCore->pushUndo(undo, redo, QStringLiteral("Composition zone fixture"));
    return composition;
}
} // namespace

TEST_CASE("MCP zone edits preserve composition ranges animation samples and rendered frames", "[mcp][composition][zone][pixels]")
{
    McpProject p;
    p.open();
    const int lower = p.videoTracks[0], upper = p.videoTracks[1];
    auto background = p.colorArgs(lower, 0, 40);
    background.insert(QStringLiteral("color"), QStringLiteral("#0000ffff"));
    p.success(QStringLiteral("add_color_clip"), background);
    const auto backgroundPixels = zoneFrame(p, 0);
    QTemporaryDir fixtures;
    REQUIRE(fixtures.isValid());
    QImage pattern(160, 120, QImage::Format_ARGB32);
    pattern.fill(Qt::transparent);
    {
        QPainter painter(&pattern);
        painter.fillRect(0, 0, 100, 40, Qt::red);
        painter.fillRect(100, 40, 60, 80, Qt::yellow);
        painter.fillRect(20, 60, 30, 30, Qt::green);
    }
    const auto patternPath = fixtures.filePath(QStringLiteral("asymmetric.png"));
    REQUIRE(pattern.save(patternPath));
    // Exercise affine with real decoded frame metadata and the complete
    // project-sized image, independently of image-plugin fallback behaviour.
    MediaFfmpeg ffmpeg;
    const auto videoPath = fixtures.filePath(QStringLiteral("asymmetric.mkv"));
    mediaProcess(ffmpeg.path, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-loop"), QStringLiteral("1"), QStringLiteral("-framerate"),
                               QStringLiteral("25"), QStringLiteral("-i"), patternPath, QStringLiteral("-t"), QStringLiteral("0.8"), QStringLiteral("-c:v"),
                               QStringLiteral("ffv1"), QStringLiteral("-level"), QStringLiteral("3"), QStringLiteral("-pix_fmt"), QStringLiteral("bgra"),
                               QStringLiteral("-an"), QStringLiteral("-n"), videoPath});
    const auto patternBin = importTimelineFixture(p, videoPath);
    const int foreground = insertTimelineFixture(p, patternBin, upper, 0, 0, 20, QStringLiteral("video"));
    const auto source = p.timeline->getClipProducer(foreground);
    REQUIRE(source);
    INFO("foreground service=" << QString::fromUtf8(source->parent().get("mlt_service")).toStdString()
                               << ", width=" << source->parent().get_int("meta.media.width") << ", height=" << source->parent().get_int("meta.media.height"));
    REQUIRE(zoneFrame(p, 0) != backgroundPixels);
    const auto run = [&](const QString &asset, const QString &parameter, const QString &animation, bool rect) {
        REQUIRE(TransitionsRepository::get()->exists(asset));
        const int forced = p.timeline->getTrackMltIndex(lower);
        const int original = zoneComposition(p, upper, asset, parameter, animation, forced);
        p.success(QStringLiteral("group_items"), timelineArgs(p, {{QStringLiteral("itemIds"), QJsonArray{foreground, original}}}));
        const auto beforeGroups = KdenliveTests::groupUpLink(p.timeline);
        QVector<QImage> pixels;
        QVector<mlt_rect> rects;
        QVector<double> values;
        for (int frame = 0; frame < 20; ++frame) {
            pixels.append(zoneFrame(p, frame));
            auto *producer = p.timeline->getCompositionParameterModel(original)->getAsset();
            if (rect)
                rects.append(producer->anim_get_rect(parameter.toUtf8().constData(), frame, 20));
            else
                values.append(producer->anim_get_double(parameter.toUtf8().constData(), frame, 20));
        }
        // Native animation reads normalize relative -1 keys to absolute frame
        // numbers. Capture the exact pre-edit strings after those read probes.
        const auto originalParameters = p.timeline->getCompositionParameterModel(original)->getAllParameters();
        // The asymmetric fixture must exercise visible movement/blending.
        INFO("asset=" << asset.toStdString() << ", first=" << pixels.at(0).pixelColor(4, 4).name().toStdString()
                      << ", last=" << pixels.at(19).pixelColor(4, 4).name().toStdString());
        if (pixels.at(0) == pixels.at(19)) {
            // Retain only this owned failing fixture, including its exact
            // native graph and actual source, for external renderer diagnosis.
            fixtures.setAutoRemove(false);
            const auto scene = p.timeline->sceneList(fixtures.path());
            const auto path = fixtures.filePath(QStringLiteral("native-%1.mlt").arg(asset));
            QFile output(path);
            REQUIRE(output.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            REQUIRE(output.write(scene.toUtf8()) > 0);
            output.close();
            // Diagnose the exact in-memory frame path without substituting a
            // reopened graph for the semantic pixel assertions below.
            auto *native = p.timeline->getCompositionParameterModel(original)->getAsset();
            const auto nativeProfile = static_cast<mlt_profile>(native->get_data("_profile"));
            qInfo() << "COMPOSITION DIAGNOSTIC" << asset << "graph" << path
                    << "nativeProfile" << (nativeProfile ? nativeProfile->width : -1) << (nativeProfile ? nativeProfile->height : -1)
                    << (nativeProfile ? nativeProfile->sample_aspect_num : -1) << (nativeProfile ? nativeProfile->sample_aspect_den : -1)
                    << "currentProfile" << pCore->getProjectProfile().width() << pCore->getProjectProfile().height()
                    << "rect" << native->get("rect") << "rotate_z" << native->get("rotate_z");
            if (rect) {
                qInfo() << "COMPOSITION RECT SAMPLES" << asset << rects.first().x << rects.first().y << rects.first().w << rects.first().h
                        << rects.first().o << rects.last().x << rects.last().y << rects.last().w << rects.last().h << rects.last().o;
            } else {
                qInfo() << "COMPOSITION VALUE SAMPLES" << asset << values.first() << values.last();
            }
            const int oldSourcePosition = source->position();
            for (int frameIndex : {0, 19}) {
                source->seek(frameIndex);
                std::unique_ptr<Mlt::Frame> frame(source->get_frame());
                REQUIRE((frame && frame->is_valid()));
                qInfo() << "COMPOSITION SOURCE FRAME" << asset << frameIndex << "position" << frame->get_position()
                        << "size" << frame->get_int("width") << frame->get_int("height")
                        << "meta" << frame->get_int("meta.media.width") << frame->get_int("meta.media.height")
                        << "SAR" << frame->get_double("aspect_ratio") << "test_image" << frame->get_int("test_image");
                const auto image = KThumb::getFrame(frame.get(), pCore->getCurrentFrameSize().width(), pCore->getCurrentFrameSize().height());
                REQUIRE_FALSE(image.isNull());
                REQUIRE(image.save(fixtures.filePath(QStringLiteral("source-%1-%2.png").arg(asset).arg(frameIndex))));
                qInfo() << "COMPOSITION SOURCE IMAGE" << asset << frameIndex << image.pixelColor(4, 4)
                        << "afterSize" << frame->get_int("width") << frame->get_int("height")
                        << "afterMeta" << frame->get_int("meta.media.width") << frame->get_int("meta.media.height")
                        << "afterSAR" << frame->get_double("aspect_ratio");
            }
            source->seek(oldSourcePosition);
            const auto afterSourceFirst = zoneFrame(p, 0), afterSourceLast = zoneFrame(p, 19);
            qInfo() << "COMPOSITION AFTER DIRECT SOURCE" << asset << "different" << (afterSourceFirst != afterSourceLast)
                    << "first" << afterSourceFirst.pixelColor(4, 4) << "last" << afterSourceLast.pixelColor(4, 4);
            const bool hadThreads = native->property_exists("threads");
            const QByteArray oldThreads = native->get("threads") ? QByteArray(native->get("threads")) : QByteArray();
            native->set("threads", 1);
            const auto singleFirst = zoneFrame(p, 0), singleLast = zoneFrame(p, 19);
            qInfo() << "COMPOSITION SINGLE THREAD" << asset << "different" << (singleFirst != singleLast)
                    << "first" << singleFirst.pixelColor(4, 4) << "last" << singleLast.pixelColor(4, 4);
            if (hadThreads) native->set("threads", oldThreads.constData());
            else native->clear("threads");
            Mlt::Producer reopened(pCore->getProjectProfile(), "xml", path.toUtf8().constData());
            REQUIRE(reopened.is_valid());
            const auto first = KThumb::getFrame(reopened, 0, pCore->getCurrentFrameSize().width(), pCore->getCurrentFrameSize().height());
            const auto last = KThumb::getFrame(reopened, 19, pCore->getCurrentFrameSize().width(), pCore->getCurrentFrameSize().height());
            qInfo() << "COMPOSITION REOPENED GRAPH" << asset << "valid" << !first.isNull() << !last.isNull()
                    << "different" << (first != last) << "first" << first.pixelColor(4, 4) << "last" << last.pixelColor(4, 4);
            REQUIRE(first.save(fixtures.filePath(QStringLiteral("reopened-%1-first.png").arg(asset))));
            REQUIRE(last.save(fixtures.filePath(QStringLiteral("reopened-%1-last.png").arg(asset))));
            REQUIRE(pixels.at(0).save(fixtures.filePath(QStringLiteral("native-%1-first.png").arg(asset))));
            REQUIRE(pixels.at(19).save(fixtures.filePath(QStringLiteral("native-%1-last.png").arg(asset))));
            INFO("Preserved native graph and decoded source: " << path.toStdString());
        }
        REQUIRE(pixels.at(0) != pixels.at(19));
        QString action;
        int shift = 0, omittedEnd = 10;
        SECTION("Lift keeps outside parts at their original positions")
        {
            action = QStringLiteral("lift");
        }
        SECTION("Extract keeps outside parts and closes exactly five frames")
        {
            action = QStringLiteral("extract");
            shift = -5;
        }
        SECTION("Insert space keeps the left part and shifts the right part")
        {
            action = QStringLiteral("insert_space");
            shift = 5;
            omittedEnd = 5;
        }
        const int history = p.undoStack->index();
        p.success(QStringLiteral("edit_timeline_zone"), timelineArgs(p, {{QStringLiteral("trackIds"), QJsonArray{upper}},
                                                                         {QStringLiteral("action"), action},
                                                                         {QStringLiteral("startFrame"), 5},
                                                                         {QStringLiteral("durationFrames"), 5}}));
        REQUIRE(p.undoStack->index() == history + 1);
        const int right = zoneCompositionAt(p, upper, omittedEnd + shift);
        REQUIRE(right >= 0);
        REQUIRE(right != original);
        CHECK(p.timeline->getCompositionPosition(original) == 0);
        CHECK(p.timeline->getCompositionPlaytime(original) == 5);
        CHECK(p.timeline->getCompositionPosition(right) == omittedEnd + shift);
        CHECK(p.timeline->getCompositionPlaytime(right) == 20 - omittedEnd);
        const int leftClip = p.timeline->getClipByPosition(upper, 0);
        const int rightClip = p.timeline->getClipByPosition(upper, omittedEnd + shift);
        REQUIRE(p.timeline->getGroupElements(leftClip).contains(original));
        REQUIRE(p.timeline->getGroupElements(rightClip).contains(right));
        CHECK_FALSE(p.timeline->getGroupElements(leftClip).contains(right));
        const auto verify = [&] {
            for (int frame = 0; frame < 20; ++frame) {
                if (frame >= 5 && frame < omittedEnd) continue;
                const int target = frame < 5 ? frame : frame + shift;
                const int composition = zoneCompositionAt(p, upper, target);
                REQUIRE(composition >= 0);
                auto *native = p.timeline->getCompositionParameterModel(composition)->getAsset();
                REQUIRE(native->get_int("force_track") == 1);
                REQUIRE(native->get_int("a_track") == forced);
                const int offset = target - p.timeline->getCompositionPosition(composition);
                const int length = p.timeline->getCompositionPlaytime(composition);
                INFO("asset=" << asset.toStdString() << ", action=" << action.toStdString() << ", source=" << frame << ", target=" << target);
                if (rect) {
                    const auto sample = native->anim_get_rect(parameter.toUtf8().constData(), offset, length);
                    CHECK(sample.x == Approx(rects.at(frame).x).margin(1e-10));
                    CHECK(sample.y == Approx(rects.at(frame).y).margin(1e-10));
                    CHECK(sample.w == Approx(rects.at(frame).w).margin(1e-10));
                    CHECK(sample.h == Approx(rects.at(frame).h).margin(1e-10));
                    CHECK(sample.o == Approx(rects.at(frame).o).margin(1e-10));
                } else
                    CHECK(native->anim_get_double(parameter.toUtf8().constData(), offset, length) == Approx(values.at(frame)).margin(1e-10));
                CHECK(zoneFrame(p, target) == pixels.at(frame));
            }
            REQUIRE(p.timeline->checkConsistency());
        };
        verify();
        p.success(QStringLiteral("undo"), p.expected());
        REQUIRE(p.timeline->getCompositionPlaytime(original) == 20);
        CHECK(p.timeline->getCompositionParameterModel(original)->getAllParameters() == originalParameters);
        // Group JSON child order follows unordered containers. Verify exact
        // native node identities and parent links instead of serialization order.
        CHECK(KdenliveTests::groupUpLink(p.timeline) == beforeGroups);
        for (int frame = 0; frame < 20; ++frame)
            CHECK(zoneFrame(p, frame) == pixels.at(frame));
        REQUIRE(p.timeline->checkConsistency());
        p.success(QStringLiteral("redo"), p.expected());
        verify();
    };
    SECTION("Wipe retains its original percentage opacity ramp")
    {
        run(QStringLiteral("wipe"), QStringLiteral("geometry"), QStringLiteral("0=0% 0% 100% 100% 0%;-1=0% 0% 100% 100% 100%"), true);
    }
    SECTION("Slide retains its original percentage geometry ramp")
    {
        run(QStringLiteral("slide"), QStringLiteral("rect"), QStringLiteral("0=-100% 0% 100% 100% 100%;-1=0% 0% 100% 100% 100%"), true);
    }
    SECTION("Affine retains an eased rotation instead of retiming it")
    {
        run(QStringLiteral("affine"), QStringLiteral("rotate_z"), QStringLiteral("0g=0;19=90"), false);
    }
}

TEST_CASE("Native composition cut preserves automatic source binding and locked-track refusal", "[mcp][composition][zone][locking]")
{
    McpProject p;
    p.open();
    const int upper = p.videoTracks[1];
    const int original =
        zoneComposition(p, upper, QStringLiteral("wipe"), QStringLiteral("geometry"), QStringLiteral("0=0% 0% 100% 100% 0%;-1=0% 0% 100% 100% 100%"), -1);
    const int automatic = p.timeline->getCompositionParameterModel(original)->getAsset()->get_int("a_track");
    p.success(QStringLiteral("edit_timeline_zone"), timelineArgs(p, {{QStringLiteral("trackIds"), QJsonArray{upper}},
                                                                     {QStringLiteral("action"), QStringLiteral("insert_space")},
                                                                     {QStringLiteral("startFrame"), 5},
                                                                     {QStringLiteral("durationFrames"), 5}}));
    const int right = zoneCompositionAt(p, upper, 10);
    REQUIRE(right >= 0);
    auto *native = p.timeline->getCompositionParameterModel(right)->getAsset();
    CHECK(native->get_int("force_track") == 0);
    CHECK(native->get_int("a_track") == automatic);
    p.success(QStringLiteral("undo"), p.expected());
    p.success(QStringLiteral("set_track_locked"), timelineArgs(p, {{QStringLiteral("trackId"), upper}, {QStringLiteral("locked"), true}}));
    const auto parameters = p.timeline->getCompositionParameterModel(original)->getAllParameters();
    const int count = p.timeline->getCompositionsCount();
    Fun undo = [] { return true; }, redo = [] { return true; };
    CHECK_FALSE(TimelineFunctions::liftZone(p.timeline, upper, QPoint(5, 10), undo, redo));
    CHECK(p.timeline->getCompositionsCount() == count);
    CHECK(p.timeline->getCompositionPlaytime(original) == 20);
    CHECK(p.timeline->getCompositionParameterModel(original)->getAllParameters() == parameters);
    REQUIRE(p.timeline->checkConsistency());
}
