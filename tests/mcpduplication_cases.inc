/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Included after the existing MCP cases; exercise native graph, history and pixels.
#include "bin/model/subtitlemodel.hpp"
#include "doc/kthumb.h"
#include "timeline2/model/groupsmodel.hpp"
#include "timeline2/model/trackmodel.hpp"
#include <mlt++/MltChain.h>
#include <mlt++/MltLink.h>

namespace {
int duplicateMappedId(const QJsonObject &reply, int original)
{
    for (const auto &value : reply.value(QStringLiteral("itemMapping")).toArray()) {
        const auto entry = value.toObject();
        if (entry.value(QStringLiteral("sourceId")).toInt(-1) == original) return entry.value(QStringLiteral("createdId")).toInt(-1);
    }
    FAIL("Duplicate did not return the source-to-copy native ID mapping");
    return -1;
}
QJsonObject duplicateFields(int clip, int track, int start, bool groups = false, bool partners = false)
{
    return {{QStringLiteral("clipId"), clip},
            {QStringLiteral("trackId"), track},
            {QStringLiteral("startFrame"), start},
            {QStringLiteral("includeGroup"), groups},
            {QStringLiteral("includeMixPartners"), partners}};
}
void duplicateMix(McpProject &p, int first, int second, const QString &asset, int before, int after)
{
    p.success(QStringLiteral("edit_mix"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("add")},
                                                           {QStringLiteral("firstClipId"), first},
                                                           {QStringLiteral("secondClipId"), second},
                                                           {QStringLiteral("transitionId"), asset},
                                                           {QStringLiteral("beforeFrames"), before},
                                                           {QStringLiteral("afterFrames"), after}}));
}
QImage duplicateFrame(McpProject &p, int frame)
{
    const auto size = pCore->getCurrentFrameSize();
    auto image = KThumb::getFrame(*p.timeline->tractor(), frame, size.width(), size.height());
    REQUIRE_FALSE(image.isNull());
    return image;
}
QString duplicateProperty(const QDomElement &element, const QString &name)
{
    for (auto node = element.firstChildElement(QStringLiteral("property")); !node.isNull(); node = node.nextSiblingElement(QStringLiteral("property")))
        if (node.attribute(QStringLiteral("name")) == name) return node.text();
    return {};
}

QString duplicateColorFixture(const QTemporaryDir &directory, const QString &ffmpeg)
{
    const auto path = directory.filePath(QStringLiteral("duplicate-colors.mkv"));
    mediaProcess(ffmpeg, {QStringLiteral("-v"),
                          QStringLiteral("error"),
                          QStringLiteral("-f"),
                          QStringLiteral("lavfi"),
                          QStringLiteral("-i"),
                          QStringLiteral("color=c=red:s=32x32:r=25:d=1"),
                          QStringLiteral("-f"),
                          QStringLiteral("lavfi"),
                          QStringLiteral("-i"),
                          QStringLiteral("color=c=blue:s=32x32:r=25:d=1"),
                          QStringLiteral("-f"),
                          QStringLiteral("lavfi"),
                          QStringLiteral("-i"),
                          QStringLiteral("color=c=green:s=32x32:r=25:d=1"),
                          QStringLiteral("-filter_complex"),
                          QStringLiteral("[0:v][1:v][2:v]concat=n=3:v=1:a=0"),
                          QStringLiteral("-c:v"),
                          QStringLiteral("ffv1"),
                          QStringLiteral("-an"),
                          QStringLiteral("-n"),
                          path});
    return path;
}
QMap<QString, QString> duplicateRemapSettings(McpProject &p, int id)
{
    QMap<QString, QString> values;
    const auto producer = p.timeline->getClipProducer(id);
    Mlt::Chain chain(producer->parent());
    REQUIRE(chain.is_valid());
    for (int row = 0; row < chain.link_count(); ++row) {
        std::unique_ptr<Mlt::Link> link(chain.link(row));
        if (!link || QString::fromUtf8(link->get("mlt_service")) != QLatin1String("timeremap")) continue;
        const char *map = link->property_exists("time_map") ? link->get("time_map") : link->get("map");
        values.insert(QStringLiteral("time_map"), QString::fromUtf8(map));
        values.insert(QStringLiteral("pitch"), QString::fromUtf8(link->get("pitch")));
        values.insert(QStringLiteral("image_mode"), QString::fromUtf8(link->get("image_mode")));
        break;
    }
    REQUIRE_FALSE(values.isEmpty());
    return values;
}
} // namespace

TEST_CASE("MCP duplication preserves nested AV split groups complete subtitle events effects and time maps", "[mcp][duplication][subtitle][groups]")
{
    McpProject p;
    p.open();
    p.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto bin = importTimelineFixture(p, avFixture(files));
    const int video = insertTimelineFixture(p, bin, p.videoTracks[0], 0, 3, 10, QStringLiteral("video"));
    const int audio = insertTimelineFixture(p, bin, p.audioTrack, 0, 3, 10, QStringLiteral("audio"));
    p.success(QStringLiteral("set_clip_time_map"),
              timelineArgs(p, {{QStringLiteral("clipId"), video},
                               {QStringLiteral("keyframes"), QJsonArray{QJsonObject{{QStringLiteral("frame"), 0}, {QStringLiteral("sourceFrame"), 3}},
                                                                        QJsonObject{{QStringLiteral("frame"), 4}, {QStringLiteral("sourceFrame"), 7}},
                                                                        QJsonObject{{QStringLiteral("frame"), 9}, {QStringLiteral("sourceFrame"), 3}}}},
                               {QStringLiteral("frameBlending"), true},
                               {QStringLiteral("pitchCompensate"), false}}));
    const auto sourceClip = KdenliveTests::getClipPtr(p.timeline, video);
    const auto sourceMap = sourceClip->getRemapValues();
    const auto stack = p.timeline->getClipEffectStackModel(video);
    REQUIRE(stack->appendEffect(QStringLiteral("brightness")));
    const auto effect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(0));
    REQUIRE(effect);
    effect->setParameter(QStringLiteral("level"), QStringLiteral("0=0.4;9=0.8"));
    p.success(QStringLiteral("edit_subtitle"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("layer_add")}}));
    const auto added = p.success(QStringLiteral("edit_subtitle"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("add")},
                                                                                   {QStringLiteral("text"), QStringLiteral("placeholder")},
                                                                                   {QStringLiteral("startFrame"), 2},
                                                                                   {QStringLiteral("endFrame"), 9},
                                                                                   {QStringLiteral("layer"), 1}}));
    const int subtitle = added.value(QStringLiteral("id")).toInt(-1);
    const auto model = p.timeline->getSubtitleModel();
    REQUIRE(model);
    const GenTime originalStart = GenTime(2, 25) + GenTime(0.005);
    const SubtitleEvent original(false, GenTime(9, 25) + GenTime(0.007), QStringLiteral("Default"), QStringLiteral("Actor, native"), 11, 22, 33,
                                 QStringLiteral("Scroll up;4;20;5"), QStringLiteral("  {\\i1}Native — שלום\\Nsecond line  "));
    REQUIRE(model->removeSubtitle(subtitle));
    REQUIRE(model->addSubtitle(subtitle, {1, originalStart}, original));
    REQUIRE(model->editSubtitle(subtitle, original.text()));
    const int split = p.timeline->requestClipsGroup({video, audio}, true, GroupType::AVSplit);
    REQUIRE(split >= 0);
    REQUIRE(p.timeline->requestClipsGroup({video, subtitle}) >= 0);
    REQUIRE(p.timeline->requestClearSelection());
    const auto originalLinks = KdenliveTests::groupUpLink(p.timeline);
    const auto originalMembers = p.timeline->getGroupElements(video);
    REQUIRE(originalMembers.size() == 3);
    const int history = p.undoStack->index();
    const auto copied = p.success(QStringLiteral("duplicate_clip"), timelineArgs(p, duplicateFields(video, p.videoTracks[0], 30, true)));
    const int copiedVideo = duplicateMappedId(copied, video), copiedAudio = duplicateMappedId(copied, audio),
              copiedSubtitle = duplicateMappedId(copied, subtitle);
    CHECK(copiedVideo != video);
    CHECK(copiedAudio != audio);
    CHECK(copiedSubtitle != subtitle);
    REQUIRE((p.timeline->getGroupElements(copiedVideo) == std::unordered_set<int>{copiedVideo, copiedAudio, copiedSubtitle}));
    CHECK(p.timeline->getClipSplitPartner(copiedVideo) == copiedAudio);
    CHECK(p.timeline->getClipSplitPartner(copiedAudio) == copiedVideo);
    CHECK(p.timeline->getClipInOut(copiedVideo) == p.timeline->getClipInOut(video));
    CHECK(p.timeline->getClipInOut(copiedAudio) == p.timeline->getClipInOut(audio));
    CHECK(KdenliveTests::getClipPtr(p.timeline, copiedVideo)->getRemapValues() == sourceMap);
    const auto copiedEffect = std::static_pointer_cast<EffectItemModel>(p.timeline->getClipEffectStackModel(copiedVideo)->getEffectStackRow(0));
    REQUIRE(copiedEffect);
    CHECK(copiedEffect->getAssetId() == effect->getAssetId());
    CHECK(QString::fromUtf8(copiedEffect->filter().get("level")) == QString::fromUtf8(effect->filter().get("level")));
    auto expected = original;
    expected.setEndTime(original.endTime() + GenTime(30, 25));
    CHECK(p.timeline->getSubtitleLayer(copiedSubtitle) == 1);
    CHECK(model->getSubtitlePosition(copiedSubtitle).frames(25) == 32);
    CHECK(model->getSubtitlePosition(copiedSubtitle) == originalStart + GenTime(30, 25));
    CHECK(model->getSubtitle(1, originalStart + GenTime(30, 25)) == expected);
    CHECK(model->getSubtitle(1, originalStart) == original);
    CHECK(p.undoStack->index() == history + 1);
    for (const auto &[id, parent] : originalLinks)
        CHECK(KdenliveTests::groupUpLink(p.timeline).at(id) == parent);
    const auto saved = p.timeline->sceneList(files.path());
    REQUIRE(saved.contains(QStringLiteral("timeremap")));
    REQUIRE(saved.contains(QStringLiteral("brightness")));
    p.success(QStringLiteral("undo"), p.expected());
    CHECK_FALSE(p.timeline->isItem(copiedVideo));
    CHECK_FALSE(p.timeline->isItem(copiedAudio));
    CHECK_FALSE(p.timeline->isItem(copiedSubtitle));
    CHECK(p.timeline->getGroupElements(video) == originalMembers);
    CHECK(KdenliveTests::groupUpLink(p.timeline) == originalLinks);
    REQUIRE(p.timeline->checkConsistency());
    p.success(QStringLiteral("redo"), p.expected());
    CHECK(p.timeline->getClipSplitPartner(copiedVideo) == copiedAudio);
    CHECK(model->getSubtitle(1, originalStart + GenTime(30, 25)) == expected);
    CHECK(KdenliveTests::getClipPtr(p.timeline, copiedVideo)->getRemapValues() == sourceMap);
    CHECK(model->getSubtitle(1, originalStart) == original);
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP duplicate mix chains preserve native overlap cut settings source and rendered red blue pixels", "[mcp][duplication][mix][pixels]")
{
    McpProject p;
    p.open();
    const int track = p.videoTracks[0];
    MediaFfmpeg ffmpeg;
    QTemporaryDir fixtureFiles;
    REQUIRE(fixtureFiles.isValid());
    const auto bin = importTimelineFixture(p, duplicateColorFixture(fixtureFiles, ffmpeg.path));
    const int first = insertTimelineFixture(p, bin, track, 0, 5, 12, QStringLiteral("video"));
    const int second = insertTimelineFixture(p, bin, track, 12, 30, 12, QStringLiteral("video"));
    const int third = insertTimelineFixture(p, bin, track, 24, 55, 12, QStringLiteral("video"));
    duplicateMix(p, first, second, QStringLiteral("dissolve"), 2, 3);
    duplicateMix(p, second, third, QStringLiteral("dissolve"), 1, 2);
    const auto nativeTrack = KdenliveTests::getTrackById_const(p.timeline, track);
    const auto sourceParams = nativeTrack->getMixParams(second);
    const auto sourceTracks = nativeTrack->getMixTracks(second);
    const auto sourceInfo = nativeTrack->getMixInfo(second).first;
    // A native setting not described by the transition's exposed parameter list.
    nativeTrack->mixModel(second)->getAsset()->set("accepts_blanks", 1);
    const auto links = KdenliveTests::groupUpLink(p.timeline);
    const auto firstRange = p.timeline->getClipInOut(first), secondRange = p.timeline->getClipInOut(second), thirdRange = p.timeline->getClipInOut(third);
    std::vector<QImage> pixels;
    for (int frame : {0, 10, 11, 12, 13, 14, 20, 23, 24, 25, 30})
        pixels.push_back(duplicateFrame(p, frame));
    const auto center = pixels.front().rect().center();
    const auto firstColor = pixels.front().pixelColor(center), lastColor = pixels.back().pixelColor(center);
    CHECK(firstColor.red() > firstColor.blue());
    CHECK(lastColor.green() > lastColor.red());
    REQUIRE(pixels.at(1) != pixels.at(4));
    p.rejectWithoutChange(QStringLiteral("duplicate_clip"), timelineArgs(p, duplicateFields(first, track, 50)));
    const int history = p.undoStack->index();
    const auto copied = p.success(QStringLiteral("duplicate_clip"), timelineArgs(p, duplicateFields(first, track, 50, false, true)));
    const int copiedFirst = duplicateMappedId(copied, first), copiedSecond = duplicateMappedId(copied, second), copiedThird = duplicateMappedId(copied, third);
    CHECK(copied.value(QStringLiteral("mixes")).toArray().size() == 2);
    CHECK(copied.value(QStringLiteral("includedMixPartnerIds")).toArray().size() == 2);
    CHECK(p.timeline->getClipInOut(copiedFirst) == firstRange);
    CHECK(p.timeline->getClipInOut(copiedSecond) == secondRange);
    CHECK(p.timeline->getClipInOut(copiedThird) == thirdRange);
    CHECK(p.timeline->getClipPosition(copiedSecond) == p.timeline->getClipPosition(second) + 50);
    CHECK(p.timeline->getMixDuration(copiedSecond) == p.timeline->getMixDuration(second));
    CHECK(p.timeline->getMixCutPos(copiedSecond) == p.timeline->getMixCutPos(second));
    CHECK(nativeTrack->getMixParams(copiedSecond) == sourceParams);
    CHECK(nativeTrack->getMixTracks(copiedSecond) == sourceTracks);
    CHECK(nativeTrack->mixModel(copiedSecond)->getAsset()->get_int("accepts_blanks") == 1);
    CHECK(nativeTrack->getMixInfo(copiedSecond).first.mixOffset == sourceInfo.mixOffset);
    CHECK(nativeTrack->getMixInfo(copiedThird).first.firstClipId == copiedSecond);
    CHECK(KdenliveTests::getClipPtr(p.timeline, copiedFirst)->getSubPlaylistIndex() == KdenliveTests::getClipPtr(p.timeline, first)->getSubPlaylistIndex());
    CHECK(KdenliveTests::getClipPtr(p.timeline, copiedSecond)->getSubPlaylistIndex() == KdenliveTests::getClipPtr(p.timeline, second)->getSubPlaylistIndex());
    CHECK(p.undoStack->index() == history + 1);
    size_t sample = 0;
    for (int frame : {0, 10, 11, 12, 13, 14, 20, 23, 24, 25, 30})
        CHECK(duplicateFrame(p, frame + 50) == pixels.at(sample++));
    QTemporaryDir files;
    QDomDocument graph;
    REQUIRE(graph.setContent(p.timeline->sceneList(files.path())));
    int savedMixes = 0, savedExplicitSettings = 0;
    const auto transitions = graph.elementsByTagName(QStringLiteral("transition"));
    for (int i = 0; i < transitions.size(); ++i) {
        const auto element = transitions.at(i).toElement();
        if (duplicateProperty(element, QStringLiteral("kdenlive_id")) != sourceParams.first) continue;
        ++savedMixes;
        if (duplicateProperty(element, QStringLiteral("accepts_blanks")) == QLatin1String("1")) ++savedExplicitSettings;
    }
    CHECK(savedMixes == 4);
    CHECK(savedExplicitSettings >= 2);
    REQUIRE(p.timeline->checkConsistency());
    p.success(QStringLiteral("undo"), p.expected());
    CHECK_FALSE(p.timeline->isItem(copiedFirst));
    CHECK_FALSE(p.timeline->isItem(copiedSecond));
    CHECK_FALSE(p.timeline->isItem(copiedThird));
    CHECK(KdenliveTests::groupUpLink(p.timeline) == links);
    CHECK(nativeTrack->getMixParams(second) == sourceParams);
    CHECK(p.timeline->getClipInOut(first) == firstRange);
    CHECK(p.timeline->getClipInOut(second) == secondRange);
    REQUIRE(p.timeline->checkConsistency());
    p.success(QStringLiteral("redo"), p.expected());
    CHECK(p.timeline->getMixDuration(copiedSecond) == p.timeline->getMixDuration(second));
    CHECK(nativeTrack->getMixParams(copiedSecond) == sourceParams);
    sample = 0;
    for (int frame : {0, 10, 11, 12, 13, 14, 20, 23, 24, 25, 30})
        CHECK(duplicateFrame(p, frame + 50) == pixels.at(sample++));
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP audio mix duplication retains native audio settings and one history step", "[mcp][duplication][mix][audio]")
{
    McpProject p;
    p.open();
    QTemporaryDir files;
    const auto bin = importTimelineFixture(p, avFixture(files));
    const int first = insertTimelineFixture(p, bin, p.audioTrack, 0, 0, 8, QStringLiteral("audio"));
    const int second = insertTimelineFixture(p, bin, p.audioTrack, 8, 8, 8, QStringLiteral("audio"));
    duplicateMix(p, first, second, QStringLiteral("mix"), 2, 2);
    const auto track = KdenliveTests::getTrackById_const(p.timeline, p.audioTrack);
    const auto source = track->mixModel(second)->getAsset();
    source->set("start", -1);
    source->set("accepts_blanks", 1);
    const auto copied = p.success(QStringLiteral("duplicate_clip"), timelineArgs(p, duplicateFields(second, p.audioTrack, 40, false, true)));
    const int copiedFirst = duplicateMappedId(copied, first), copiedSecond = duplicateMappedId(copied, second);
    const auto asset = track->mixModel(copiedSecond)->getAsset();
    CHECK(asset->get_int("start") == -1);
    CHECK(asset->get_int("accepts_blanks") == 1);
    CHECK(track->getMixParams(copiedSecond) == track->getMixParams(second));
    CHECK(p.timeline->getClipPosition(copiedSecond) == 40);
    CHECK(p.timeline->getClipPosition(copiedFirst) == p.timeline->getClipPosition(first) + 40 - p.timeline->getClipPosition(second));
    REQUIRE(p.timeline->checkConsistency());
    p.success(QStringLiteral("undo"), p.expected());
    CHECK_FALSE(p.timeline->isItem(copiedFirst));
    CHECK_FALSE(p.timeline->isItem(copiedSecond));
    REQUIRE(p.timeline->checkConsistency());
    p.success(QStringLiteral("redo"), p.expected());
    CHECK(track->mixModel(copiedSecond)->getAsset()->get_int("start") == -1);
    CHECK(track->mixModel(copiedSecond)->getAsset()->get_int("accepts_blanks") == 1);
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP duplication rejects incomplete groups subtitle collisions locks and partner destinations atomically", "[mcp][duplication][atomicity]")
{
    McpProject p;
    p.open();
    p.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    const int track = p.videoTracks[0];
    QTemporaryDir fixtureFiles;
    REQUIRE(fixtureFiles.isValid());
    const auto bin = importTimelineFixture(p, avFixture(fixtureFiles));
    const int first = insertTimelineFixture(p, bin, track, 0, 0, 8, QStringLiteral("video"));
    const int second = insertTimelineFixture(p, bin, track, 8, 8, 8, QStringLiteral("video"));
    duplicateMix(p, first, second, QStringLiteral("dissolve"), 2, 2);
    const int sub = p.success(QStringLiteral("edit_subtitle"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("add")},
                                                                                {QStringLiteral("text"), QStringLiteral("Grouped mix partner")},
                                                                                {QStringLiteral("startFrame"), 13},
                                                                                {QStringLiteral("endFrame"), 20}}))
                        .value(QStringLiteral("id"))
                        .toInt();
    REQUIRE(p.timeline->requestClipsGroup({second, sub}) >= 0);
    REQUIRE(p.timeline->requestClearSelection());
    const auto links = KdenliveTests::groupUpLink(p.timeline);
    const auto model = p.timeline->getSubtitleModel();
    const auto original = model->getSubtitle(0, GenTime(13, 25));
    auto fields = duplicateFields(first, track, 50, false, true);
    p.rejectWithoutChange(QStringLiteral("duplicate_clip"), timelineArgs(p, fields));
    fields.insert(QStringLiteral("includeGroup"), true);
    SECTION("A subtitle at the same destination start blocks the whole operation")
    {
        p.success(QStringLiteral("edit_subtitle"), timelineArgs(p, {{QStringLiteral("operation"), QStringLiteral("add")},
                                                                    {QStringLiteral("text"), QStringLiteral("Collision")},
                                                                    {QStringLiteral("startFrame"), 63},
                                                                    {QStringLiteral("endFrame"), 70}}));
        p.rejectWithoutChange(QStringLiteral("duplicate_clip"), timelineArgs(p, fields));
    }
    SECTION("A collision affecting only the expanded partner blocks the whole operation")
    {
        p.success(QStringLiteral("add_color_clip"), p.colorArgs(track, 63, 4));
        p.rejectWithoutChange(QStringLiteral("duplicate_clip"), timelineArgs(p, fields));
    }
    SECTION("A locked subtitle model blocks an expanded mix group")
    {
        model->switchLocked();
        REQUIRE(model->isLocked());
        p.rejectWithoutChange(QStringLiteral("duplicate_clip"), timelineArgs(p, fields));
        model->switchLocked();
    }
    SECTION("Translated incoming partner cannot leave frame bounds")
    {
        fields.insert(QStringLiteral("clipId"), second);
        fields.insert(QStringLiteral("startFrame"), 0);
        p.rejectWithoutChange(QStringLiteral("duplicate_clip"), timelineArgs(p, fields));
    }
    SECTION("Explicit group expansion copies the partner and its subtitle")
    {
        const auto result = p.success(QStringLiteral("duplicate_clip"), timelineArgs(p, fields));
        const int copy = duplicateMappedId(result, sub), partner = duplicateMappedId(result, second);
        CHECK(model->getSubtitlePosition(copy).frames(25) == 63);
        CHECK(p.timeline->getGroupElements(partner).contains(copy));
        p.success(QStringLiteral("undo"), p.expected());
        CHECK_FALSE(p.timeline->isItem(copy));
        CHECK_FALSE(p.timeline->isItem(partner));
    }
    for (const auto &[id, parent] : links)
        CHECK(KdenliveTests::groupUpLink(p.timeline).at(id) == parent);
    CHECK(model->getSubtitle(0, GenTime(13, 25)) == original);
    CHECK(p.timeline->getMixDuration(second) == 4);
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP duplicate shifted endless source ranges keeps exact animation and native history", "[mcp][duplication][ranges]")
{
    McpProject p;
    p.open();
    const int track = p.videoTracks[0];
    const int seed = p.success(QStringLiteral("add_color_clip"), p.colorArgs(track, 0, 36)).value(QStringLiteral("clipId")).toInt();
    const auto bin = p.timeline->getClipBinId(seed);
    p.success(QStringLiteral("delete_item"), timelineArgs(p, {{QStringLiteral("itemId"), seed}}));
    // Exercise the native project-loading constructor: imported endless cuts can
    // retain a nonzero source range unlike normal GUI endless resize.
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile().get_profile(), "color", "#ff0000ff");
    REQUIRE(producer->is_valid());
    producer->set("length", 36);
    producer->set("out", 35);
    const auto cut = std::shared_ptr<Mlt::Producer>(producer->cut(24, 35));
    const int source = ClipModel::construct(p.timeline, bin, cut, PlaylistState::VideoOnly, track, QString());
    REQUIRE(source >= 0);
    Fun fixtureUndo = [] { return true; }, fixtureRedo = [] { return true; };
    REQUIRE(p.timeline->requestClipMove(source, track, 0, false, true, true, true, fixtureUndo, fixtureRedo) == TimelineModel::MoveSuccess);
    REQUIRE(p.timeline->getClipInOut(source) == std::make_pair(24, 35));
    const auto stack = p.timeline->getClipEffectStackModel(source);
    REQUIRE(stack->appendEffect(QStringLiteral("brightness")));
    const auto effect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(0));
    REQUIRE(effect);
    effect->setParameter(QStringLiteral("level"), QStringLiteral("24=0.3;35=0.8"));
    const auto binProducer = pCore->projectItemModel()->getClipByBinID(bin)->originalProducer();
    const int binLength = binProducer->get_length(), binOut = binProducer->get_out();
    const int history = p.undoStack->index();
    const auto result = p.success(QStringLiteral("duplicate_clip"), timelineArgs(p, duplicateFields(source, track, 80)));
    const int copied = result.value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(p.timeline->getClipInOut(copied) == std::make_pair(24, 35));
    const auto copiedEffect = std::static_pointer_cast<EffectItemModel>(p.timeline->getClipEffectStackModel(copied)->getEffectStackRow(0));
    REQUIRE(copiedEffect);
    CHECK(QString::fromUtf8(copiedEffect->filter().get("level")) == QString::fromUtf8(effect->filter().get("level")));
    CHECK(copiedEffect->filter().anim_get_double("level", 24) == Approx(0.3));
    CHECK(copiedEffect->filter().anim_get_double("level", 35) == Approx(0.8));
    CHECK(p.undoStack->index() == history + 1);
    REQUIRE(p.timeline->checkConsistency());
    p.success(QStringLiteral("undo"), p.expected());
    CHECK_FALSE(p.timeline->isItem(copied));
    CHECK(p.timeline->getClipInOut(source) == std::make_pair(24, 35));
    CHECK(binProducer->get_length() == binLength);
    CHECK(binProducer->get_out() == binOut);
    REQUIRE(p.timeline->checkConsistency());
    p.success(QStringLiteral("redo"), p.expected());
    CHECK(p.timeline->getClipInOut(copied) == std::make_pair(24, 35));
    CHECK(copiedEffect->filter().anim_get_double("level", 24) == Approx(0.3));
    CHECK(copiedEffect->filter().anim_get_double("level", 35) == Approx(0.8));
    REQUIRE(p.timeline->checkConsistency());
}

TEST_CASE("MCP duplicate overlong native time remaps keeps full map configuration ranges and effect keys", "[mcp][duplication][timeremap][ranges]")
{
    McpProject p;
    p.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto bin = importTimelineFixture(p, avFixture(files));
    const int source = insertTimelineFixture(p, bin, p.videoTracks[0], 0, 3, 10, QStringLiteral("video"));
    const auto clip = KdenliveTests::getClipPtr(p.timeline, source);
    Fun undo = [] { return true; }, redo = [] { return true; };
    REQUIRE(p.timeline->requestClipTimeRemap(source, true, undo, redo));
    Mlt::Properties map;
    map.set("_profile", pCore->getProjectProfile().get_profile(), 0);
    map.anim_set("time_map", 3.0 / 25, 3, 0, mlt_keyframe_linear);
    map.anim_set("time_map", 24.0 / 25, 42, 0, mlt_keyframe_linear);
    int duration = 40;
    REQUIRE(p.timeline->requestItemResize(source, duration, true, true, undo, redo));
    clip->setRemapValue(QStringLiteral("time_map"), QString::fromUtf8(map.get("time_map")));
    clip->setRemapValue(QStringLiteral("pitch"), QStringLiteral("1"));
    clip->setRemapValue(QStringLiteral("image_mode"), QStringLiteral("blend"));
    pCore->pushUndo(undo, redo, QStringLiteral("Native long remap fixture"));
    const auto binProducer = pCore->projectItemModel()->getClipByBinID(bin)->originalProducer();
    const int binLength = binProducer->get_length(), binOut = binProducer->get_out();
    REQUIRE(p.timeline->getClipPlaytime(source) > binLength);
    REQUIRE(p.timeline->getClipInOut(source) == std::make_pair(3, 42));
    REQUIRE(p.timeline->checkConsistency());
    const auto settings = duplicateRemapSettings(p, source);
    REQUIRE(settings.value(QStringLiteral("pitch")) == QLatin1String("1"));
    REQUIRE(settings.value(QStringLiteral("image_mode")) == QLatin1String("blend"));
    const auto stack = p.timeline->getClipEffectStackModel(source);
    REQUIRE(stack->appendEffect(QStringLiteral("brightness")));
    const auto effect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(0));
    REQUIRE(effect);
    effect->setParameter(QStringLiteral("level"), QStringLiteral("3=0.3;42=0.9"));
    auto mapSample = [&](int id, int frame) {
        const auto producer = p.timeline->getClipProducer(id);
        Mlt::Chain chain(producer->parent());
        REQUIRE(chain.is_valid());
        for (int row = 0; row < chain.link_count(); ++row) {
            std::unique_ptr<Mlt::Link> link(chain.link(row));
            if (QString::fromUtf8(link->get("mlt_service")) == QLatin1String("timeremap")) return link->anim_get_double("time_map", frame) * 25;
        }
        FAIL("Native timeremap link missing");
        return -1.0;
    };
    CHECK(mapSample(source, 3) == Approx(3));
    CHECK(mapSample(source, 42) == Approx(24));
    const int history = p.undoStack->index();
    const auto result = p.success(QStringLiteral("duplicate_clip"), timelineArgs(p, duplicateFields(source, p.videoTracks[0], 70)));
    const int copied = result.value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(p.timeline->getClipInOut(copied) == std::make_pair(3, 42));
    CHECK(duplicateRemapSettings(p, copied) == settings);
    for (int frame : {3, 15, 30, 42})
        CHECK(mapSample(copied, frame) == Approx(mapSample(source, frame)));
    const auto copiedEffect = std::static_pointer_cast<EffectItemModel>(p.timeline->getClipEffectStackModel(copied)->getEffectStackRow(0));
    REQUIRE(copiedEffect);
    CHECK(copiedEffect->filter().anim_get_double("level", 3) == Approx(0.3));
    CHECK(copiedEffect->filter().anim_get_double("level", 42) == Approx(0.9));
    CHECK(p.undoStack->index() == history + 1);
    CHECK(binProducer->get_length() == binLength);
    CHECK(binProducer->get_out() == binOut);
    REQUIRE(p.timeline->checkConsistency());
    p.success(QStringLiteral("undo"), p.expected());
    CHECK_FALSE(p.timeline->isItem(copied));
    CHECK(duplicateRemapSettings(p, source) == settings);
    CHECK(p.timeline->getClipInOut(source) == std::make_pair(3, 42));
    CHECK(binProducer->get_length() == binLength);
    CHECK(binProducer->get_out() == binOut);
    REQUIRE(p.timeline->checkConsistency());
    p.success(QStringLiteral("redo"), p.expected());
    CHECK(p.timeline->getClipInOut(copied) == std::make_pair(3, 42));
    CHECK(duplicateRemapSettings(p, copied) == settings);
    for (int frame : {3, 15, 30, 42})
        CHECK(mapSample(copied, frame) == Approx(mapSample(source, frame)));
    CHECK(copiedEffect->filter().anim_get_double("level", 42) == Approx(0.9));
    CHECK(binProducer->get_length() == binLength);
    CHECK(binProducer->get_out() == binOut);
    REQUIRE(p.timeline->checkConsistency());
}
