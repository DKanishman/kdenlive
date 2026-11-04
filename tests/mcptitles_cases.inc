/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Included by mcptest.cpp; exercise actual title models, producers and native undo.
#include "doc/kthumb.h"
#include "mcp/mcptitles.h"
#include "titler/graphicsscenerectmove.h"
#include "titler/richtextgradient.h"
#include "titler/richtextoutline.h"
#include "titler/richtextshadow.h"
#include "titler/titledocument.h"
#include <QGraphicsScene>
#include <QPainter>
#include <QTextCursor>
#include <QTextDocument>
#include <mlt++/MltTractor.h>
namespace {
QJsonObject titleFields(McpProject &project, const QJsonObject &fields)
{
    auto arguments = project.expected();
    for (auto it = fields.begin(); it != fields.end(); ++it)
        arguments.insert(it.key(), it.value());
    return arguments;
}
QString titleFixture(McpProject &project, const QString &text = QStringLiteral("RED BLUE"))
{
    const auto result = project.success(
        QStringLiteral("create_title"),
        titleFields(project, {{QStringLiteral("durationFrames"), 25},
                              {QStringLiteral("items"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                               {QStringLiteral("text"), text},
                                                                               {QStringLiteral("x"), 40},
                                                                               {QStringLiteral("y"), 40},
                                                                               {QStringLiteral("width"), 600},
                                                                               {QStringLiteral("fontSize"), 64},
                                                                               {QStringLiteral("font"), QStringLiteral("Sans Serif")}},
                                                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                                                               {QStringLiteral("x"), 20},
                                                                               {QStringLiteral("y"), 360},
                                                                               {QStringLiteral("width"), 150},
                                                                               {QStringLiteral("height"), 40},
                                                                               {QStringLiteral("fill"), QStringLiteral("#444444")}},
                                                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                                               {QStringLiteral("text"), QString::fromUtf8("  שלום 👋  \n second line  ")},
                                                                               {QStringLiteral("x"), 40},
                                                                               {QStringLiteral("y"), 200},
                                                                               {QStringLiteral("width"), 600},
                                                                               {QStringLiteral("fontSize"), 32}}}}}));
    const auto bin = result.value(QStringLiteral("binId")).toString();
    project.awaitMediaReady(bin);
    return bin;
}
QString titleXml(const QString &bin)
{
    const auto source = pCore->projectItemModel()->getClipByBinID(bin);
    REQUIRE(source);
    return source->getProducerProperty(QStringLiteral("xmldata"));
}
QDomDocument titleDocument(const QString &xml)
{
    QDomDocument result;
    REQUIRE(result.setContent(xml, QDomDocument::ParseOption::PreserveSpacingOnlyNodes));
    return result;
}
QJsonObject titleStyleEdit(int item, int start, int length, const QJsonObject &style)
{
    return {{QStringLiteral("itemIndex"), item}, {QStringLiteral("start"), start}, {QStringLiteral("length"), length}, {QStringLiteral("style"), style}};
}
QJsonObject titleFormatArguments(McpProject &project, const QString &bin, const QJsonArray &edits)
{
    const auto title = project.success(QStringLiteral("get_title"), {{QStringLiteral("binId"), bin}});
    return titleFields(
        project,
        {{QStringLiteral("binId"), bin}, {QStringLiteral("expectedTitleHash"), title.value(QStringLiteral("titleHash"))}, {QStringLiteral("edits"), edits}});
}
std::unique_ptr<MyTextItem> restoredTitleText(const QString &xml, int index = 0)
{
    const auto document = titleDocument(xml);
    const auto item = document.elementsByTagName(QStringLiteral("item")).at(index);
    int missing = 0, maximumZ = 0;
    std::unique_ptr<QGraphicsItem> loaded(TitleDocument::loadItemFromXml(item, QString(), document.documentElement().attribute(QStringLiteral("width")).toInt(),
                                                                         document.documentElement().attribute(QStringLiteral("height")).toInt(), missing,
                                                                         maximumZ));
    REQUIRE(loaded);
    REQUIRE(missing == 0);
    REQUIRE(loaded->type() == QGraphicsTextItem::Type);
    return std::unique_ptr<MyTextItem>(static_cast<MyTextItem *>(loaded.release()));
}
QTextCharFormat titleFormatAt(MyTextItem &item, int start)
{
    QTextCursor cursor(item.document());
    cursor.setPosition(start);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    return cursor.charFormat();
}
int titleColorPixels(const QImage &image, bool red)
{
    int total = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (pixel.alpha() > 0 &&
                (red ? pixel.red() > 150 && pixel.green() < 100 && pixel.blue() < 100 : pixel.blue() > 150 && pixel.green() < 100 && pixel.red() < 100))
                ++total;
        }
    return total;
}
} // namespace
TEST_CASE("MCP native rich title edits preserve other objects used producers effects and one undo", "[MCP][MCPTitles][pixels]")
{
    McpProject project;
    project.open();
    const auto bin = titleFixture(project);
    auto source = pCore->projectItemModel()->getClipByBinID(bin);
    REQUIRE(source);
    // Unknown metadata and native positions/viewports must survive selective edits.
    auto fixture = titleDocument(titleXml(bin));
    fixture.documentElement().setAttribute(QStringLiteral("mcp-owned-test-marker"), QStringLiteral("preserve"));
    auto content = fixture.elementsByTagName(QStringLiteral("item")).at(0).firstChildElement(QStringLiteral("content"));
    content.setAttribute(QStringLiteral("mcp-owned-format-marker"), QStringLiteral("untouched"));
    auto extra = fixture.createElement(QStringLiteral("mcp-owned-format-extension"));
    extra.appendChild(fixture.createTextNode(QStringLiteral("owned test metadata")));
    content.appendChild(extra);
    source->setProperties({{QStringLiteral("xmldata"), fixture.toString()}});
    project.awaitMediaReady(bin);
    const auto original = titleXml(bin);
    const auto originalDoc = titleDocument(original);
    const int placed = insertTimelineFixture(project, bin, project.videoTracks[0], 0, 0, 25, QStringLiteral("video"));
    REQUIRE(placed >= 0);
    auto effect = effectArgs(project, QStringLiteral("clip"), QString::number(placed), QStringLiteral("add"));
    effect.insert(QStringLiteral("effectId"), QStringLiteral("brightness"));
    project.success(QStringLiteral("edit_effect"), effect);
    const auto effectCount = project.timeline->getClipEffectStackModel(placed)->rowCount();
    const auto imageBefore = KThumb::getFrame(*project.timeline->tractor(), 0, 720, 576);
    REQUIRE_FALSE(imageBefore.isNull());
    const int before = project.undoStack->index();
    const auto formatted = project.success(
        QStringLiteral("format_title_text"),
        titleFormatArguments(
            project, bin,
            QJsonArray{
                titleStyleEdit(0, 0, 3,
                               {{QStringLiteral("color"), QStringLiteral("#ff0000")}, {QStringLiteral("bold"), true}, {QStringLiteral("underline"), true}}),
                titleStyleEdit(
                    0, 4, 4,
                    {{QStringLiteral("color"), QStringLiteral("#0000ff")}, {QStringLiteral("italic"), true}, {QStringLiteral("letterSpacing"), 2.25}})}));
    CHECK(formatted.value(QStringLiteral("changed")).toBool());
    CHECK(project.undoStack->index() == before + 1);
    project.awaitMediaReady(bin);
    const auto changed = titleXml(bin);
    REQUIRE(changed != original);
    auto native = restoredTitleText(changed);
    CHECK(native->toPlainText() == QStringLiteral("RED BLUE"));
    CHECK(titleFormatAt(*native, 0).fontWeight() == QFont::Bold);
    CHECK(titleFormatAt(*native, 0).fontUnderline());
    CHECK(titleFormatAt(*native, 0).foreground().color() == QColor(Qt::red));
    CHECK_FALSE(titleFormatAt(*native, 4).fontUnderline());
    CHECK(titleFormatAt(*native, 4).fontItalic());
    CHECK(titleFormatAt(*native, 4).foreground().color() == QColor(Qt::blue));
    CHECK(titleFormatAt(*native, 4).font().letterSpacing() == Approx(2.25));
    auto afterDoc = titleDocument(changed);
    CHECK(afterDoc.documentElement().attribute(QStringLiteral("mcp-owned-test-marker")) == QStringLiteral("preserve"));
    for (int index : {1, 2}) {
        QDomDocument a, b;
        a.appendChild(a.importNode(originalDoc.elementsByTagName(QStringLiteral("item")).at(index), true));
        b.appendChild(b.importNode(afterDoc.elementsByTagName(QStringLiteral("item")).at(index), true));
        CHECK(a.toString() == b.toString());
    }
    CHECK(afterDoc.elementsByTagName(QStringLiteral("mcp-owned-format-extension")).at(0).toElement().text() == QStringLiteral("owned test metadata"));
    CHECK(afterDoc.elementsByTagName(QStringLiteral("content")).at(0).toElement().attribute(QStringLiteral("mcp-owned-format-marker")) ==
          QStringLiteral("untouched"));
    CHECK(project.timeline->getClipPosition(placed) == 0);
    CHECK(project.timeline->getClipPlaytime(placed) == 25);
    CHECK(project.timeline->getClipEffectStackModel(placed)->rowCount() == effectCount);
    const auto producer = project.timeline->getClipProducer(placed);
    REQUIRE(producer);
    CHECK(QString::fromUtf8(producer->parent().get("xmldata")) == changed);
    const auto imageAfter = KThumb::getFrame(*project.timeline->tractor(), 0, 720, 576);
    REQUIRE_FALSE(imageAfter.isNull());
    CHECK(imageAfter != imageBefore);
    CHECK(titleColorPixels(imageAfter, true) > 30);
    CHECK(titleColorPixels(imageAfter, false) > 30);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("formatted-title.kdenlive"));
    const auto scene = pCore->projectManager()->projectSceneList(directory.path()).first;
    REQUIRE(!scene.isEmpty());
    QString saveError;
    REQUIRE(project.document->saveSceneList(path, scene, false, &saveError, true));
    CHECK(saveError.isEmpty());
    REQUIRE(QFileInfo(path).isFile());
    Mlt::Producer reopened(pCore->getProjectProfile(), "xml", path.toUtf8().constData());
    REQUIRE(reopened.is_valid());
    const auto savedPixels = KThumb::getFrame(reopened, 0, 720, 576);
    CHECK(titleColorPixels(savedPixels, true) > 30);
    CHECK(titleColorPixels(savedPixels, false) > 30);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.undoStack->index() == before);
    CHECK(titleXml(bin) == original);
    const auto undoPixels = KThumb::getFrame(*project.timeline->tractor(), 0, 720, 576);
    CHECK(undoPixels == imageBefore);
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(titleXml(bin) == changed);
    CHECK(project.undoStack->index() == before + 1);
    CHECK(project.timeline->getClipEffectStackModel(placed)->rowCount() == effectCount);
    REQUIRE(project.timeline->checkConsistency());
}
TEST_CASE("MCP title gradients report native region splits and merges before recoloring outside selection", "[MCP][MCPTitles][gradients]")
{
    McpProject project;
    project.open();
    const auto bin = titleFixture(project, QStringLiteral("ABCDEFGHIJKL"));
    const QJsonObject gradient{
        {QStringLiteral("startColor"), QStringLiteral("#ff0000")}, {QStringLiteral("endColor"), QStringLiteral("#0000ff")}, {QStringLiteral("angle"), 0.0}};
    auto args = titleFormatArguments(project, bin, QJsonArray{titleStyleEdit(0, 0, 6, {{QStringLiteral("gradient"), gradient}})});
    const auto first = project.success(QStringLiteral("format_title_text"), args);
    CHECK(first.value(QStringLiteral("affectedGradientRegions")).toArray().isEmpty());
    const auto stored = titleDocument(titleXml(bin)).elementsByTagName(QStringLiteral("content")).at(0);
    REQUIRE(stored.firstChild().isText());
    CHECK(stored.firstChild().nodeValue() == QStringLiteral("ABCDEFGHIJKL"));
    project.success(QStringLiteral("get_title"), {{QStringLiteral("binId"), bin}, {QStringLiteral("itemIndex"), 0}});
    auto item = restoredTitleText(titleXml(bin));
    REQUIRE(TitlerGradientV1::regions(item->document()).size() == 1);
    const auto nativeRegions = TitlerGradientV1::regions(item->document());
    CHECK(nativeRegions[0].start == 0);
    CHECK(nativeRegions[0].length == 6);
    args = titleFormatArguments(project, bin, QJsonArray{titleStyleEdit(0, 2, 2, {{QStringLiteral("color"), QStringLiteral("#00ff00")}})});
    const auto rejected = project.call(QStringLiteral("format_title_text"), args);
    CHECK(rejected.value(QStringLiteral("isError")).toBool());
    const auto detail = rejected.value(QStringLiteral("structuredContent")).toObject();
    CHECK(detail.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")) == QStringLiteral("GRADIENT_REFLOW_ACKNOWLEDGEMENT_REQUIRED"));
    REQUIRE(detail.value(QStringLiteral("affectedGradientRegions")).toArray().size() == 1);
    const auto before = titleXml(bin);
    const int history = project.undoStack->index();
    project.rejectWithoutChange(QStringLiteral("format_title_text"), args);
    CHECK(titleXml(bin) == before);
    args.insert(QStringLiteral("acknowledgeGradientReflow"), true);
    project.success(QStringLiteral("format_title_text"), args);
    CHECK(project.undoStack->index() == history + 1);
    item = restoredTitleText(titleXml(bin));
    CHECK(titleFormatAt(*item, 2).foreground().color() == QColor(Qt::green));
    CHECK(TitlerGradientV1::regions(item->document()).size() == 2);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(titleXml(bin) == before);
    // Adjacent equal recipes merge and expand the original coordinate region.
    args = titleFormatArguments(project, bin, QJsonArray{titleStyleEdit(0, 6, 3, {{QStringLiteral("gradient"), gradient}})});
    project.rejectWithoutChange(QStringLiteral("format_title_text"), args);
    args.insert(QStringLiteral("acknowledgeGradientReflow"), true);
    const auto merged = project.success(QStringLiteral("format_title_text"), args);
    CHECK_FALSE(merged.value(QStringLiteral("affectedGradientRegions")).toArray().isEmpty());
    item = restoredTitleText(titleXml(bin));
    const auto regions = TitlerGradientV1::regions(item->document());
    REQUIRE(regions.size() == 1);
    CHECK(regions[0].start == 0);
    CHECK(regions[0].length == 9);
}
TEST_CASE("MCP title paragraph scope and selective outline shadow survive subsequent font formatting", "[MCP][MCPTitles][formatting]")
{
    McpProject project;
    project.open();
    const auto bin = titleFixture(project, QString::fromUtf8("Alpha שלום\nBeta"));
    const QJsonObject requestedShadow{{QStringLiteral("enabled"), true},
                                      {QStringLiteral("blur"), 3},
                                      {QStringLiteral("x"), 5},
                                      {QStringLiteral("y"), -2},
                                      {QStringLiteral("color"), QStringLiteral("#80000000")}};
    const QJsonObject style{{QStringLiteral("alignment"), QStringLiteral("center")},
                            {QStringLiteral("lineSpacing"), 7},
                            {QStringLiteral("outlineWidth"), 4.5},
                            {QStringLiteral("outlineColor"), QStringLiteral("#ffaa00")},
                            {QStringLiteral("shadow"), requestedShadow}};
    auto args = titleFormatArguments(project, bin, QJsonArray{titleStyleEdit(0, 0, 5, style)});
    const auto result = project.success(QStringLiteral("format_title_text"), args);
    REQUIRE(result.value(QStringLiteral("paragraphScope")).toArray().size() == 1);
    CHECK(result.value(QStringLiteral("paragraphScope")).toArray()[0].toObject().value(QStringLiteral("scope")) == QStringLiteral("whole_text_object"));
    auto native = restoredTitleText(titleXml(bin));
    CHECK(native->alignment() == Qt::AlignHCenter);
    CHECK(native->data(TitleDocument::LineSpacing).toInt() == 7);
    const auto originalStyle = titleFormatAt(*native, 0);
    const auto outline = TitlerOutline::effectivePen(originalStyle, native->defaultOutline());
    CHECK(TitlerOutline::width(outline) == Approx(4.5));
    const auto shadow = TitlerShadowV1::effective(originalStyle, native->defaultShadow());
    CHECK(shadow.enabled);
    CHECK(shadow.blur == 3);
    CHECK(shadow.x == 5);
    CHECK(shadow.y == -2);
    const auto outside = titleFormatAt(*native, 6);
    CHECK(TitlerOutline::width(TitlerOutline::effectivePen(outside, native->defaultOutline())) == Approx(0));
    CHECK_FALSE(TitlerShadowV1::effective(outside, native->defaultShadow()).enabled);
    project.success(QStringLiteral("format_title_text"),
                    titleFormatArguments(project, bin,
                                         QJsonArray{titleStyleEdit(0, 0, 5,
                                                                   {{QStringLiteral("fontSize"), 72},
                                                                    {QStringLiteral("weight"), 600},
                                                                    {QStringLiteral("strikeOut"), true},
                                                                    {QStringLiteral("letterSpacing"), 110.5},
                                                                    {QStringLiteral("letterSpacingType"), QStringLiteral("percentage")}})}));
    native = restoredTitleText(titleXml(bin));
    const auto changed = titleFormatAt(*native, 0);
    CHECK(changed.font().pixelSize() == 72);
    CHECK(changed.fontWeight() == 600);
    CHECK(changed.fontStrikeOut());
    CHECK(changed.font().letterSpacingType() == QFont::PercentageSpacing);
    CHECK(changed.font().letterSpacing() == Approx(110.5));
    CHECK(TitlerOutline::effectivePen(changed, native->defaultOutline()) == outline);
    CHECK(TitlerShadowV1::effective(changed, native->defaultShadow()) == shadow);
    CHECK(native->toPlainText() == QString::fromUtf8("Alpha שלום\nBeta"));
}
TEST_CASE("MCP title range validation rejects surrogate splits stale hashes malformed styles and overlap atomically", "[MCP][MCPTitles][guards]")
{
    McpProject project;
    project.open();
    const auto bin = titleFixture(project, QString::fromUtf8("A👋é שלום  \nTail"));
    const auto original = titleXml(bin);
    const auto detailed = project.success(QStringLiteral("get_title"), {{QStringLiteral("binId"), bin}, {QStringLiteral("itemIndex"), 0}});
    CHECK(detailed.value(QStringLiteral("text")) == QString::fromUtf8("A👋é שלום  \nTail"));
    for (const auto &edits :
         {QJsonArray{titleStyleEdit(0, 2, 1, {{QStringLiteral("bold"), true}})}, QJsonArray{titleStyleEdit(0, 1, 1, {{QStringLiteral("bold"), true}})},
          QJsonArray{titleStyleEdit(0, 0, 1, {{QStringLiteral("weight"), 0}})}, QJsonArray{titleStyleEdit(0, 0, 1, {{QStringLiteral("fontSize"), 0}})},
          QJsonArray{titleStyleEdit(0, 0, 1, {{QStringLiteral("unknownNativeProperty"), 3}})},
          QJsonArray{titleStyleEdit(0, 0, 1, {{QStringLiteral("gradient"), QJsonObject{{QStringLiteral("startColor"), QStringLiteral("bad;color")}}}})},
          QJsonArray{titleStyleEdit(0, 0, 3, {{QStringLiteral("bold"), true}}), titleStyleEdit(0, 1, 3, {{QStringLiteral("italic"), true}})}}) {
        project.rejectWithoutChange(QStringLiteral("format_title_text"), titleFormatArguments(project, bin, edits));
        CHECK(titleXml(bin) == original);
    }
    auto stale = titleFormatArguments(project, bin, QJsonArray{titleStyleEdit(0, 0, 1, {{QStringLiteral("bold"), true}})});
    stale.insert(QStringLiteral("expectedTitleHash"), QString(64, QLatin1Char('0')));
    project.rejectWithoutChange(QStringLiteral("format_title_text"), stale);
    // Legacy graphics-effect metadata is preserved without loading/replacing
    // MyTextItem's internal shadow effect in the private formatting item.
    auto withEffect = titleDocument(original);
    auto effectNode = withEffect.createElement(QStringLiteral("effect"));
    effectNode.setAttribute(QStringLiteral("type"), QStringLiteral("blur"));
    effectNode.setAttribute(QStringLiteral("blurradius"), 2);
    withEffect.elementsByTagName(QStringLiteral("item")).at(0).appendChild(effectNode);
    pCore->projectItemModel()->getClipByBinID(bin)->setProperties({{QStringLiteral("xmldata"), withEffect.toString()}});
    project.success(QStringLiteral("format_title_text"),
                    titleFormatArguments(project, bin, QJsonArray{titleStyleEdit(0, 0, 1, {{QStringLiteral("italic"), true}})}));
    const auto keptEffect = titleDocument(titleXml(bin)).elementsByTagName(QStringLiteral("effect"));
    REQUIRE(keptEffect.size() == 1);
    CHECK(keptEffect.at(0).toElement().attribute(QStringLiteral("type")) == QStringLiteral("blur"));
    CHECK(keptEffect.at(0).toElement().attribute(QStringLiteral("blurradius")) == QStringLiteral("2"));
    pCore->projectItemModel()->getClipByBinID(bin)->setProperties({{QStringLiteral("xmldata"), original}});
    const int noChangeHistory = project.undoStack->index();
    const auto noChange = project.success(QStringLiteral("format_title_text"),
                                          titleFormatArguments(project, bin, QJsonArray{titleStyleEdit(0, 0, 1, {{QStringLiteral("bold"), false}})}));
    CHECK_FALSE(noChange.value(QStringLiteral("changed")).toBool());
    CHECK(titleXml(bin) == original);
    CHECK(project.undoStack->index() == noChangeHistory);
    project.rejectWithoutChange(
        QStringLiteral("format_title_text"),
        titleFormatArguments(
            project, bin,
            QJsonArray{titleStyleEdit(0, 0, 1,
                                      {{QStringLiteral("fontSize"), 2000},
                                       {QStringLiteral("outlineWidth"), 200},
                                       {QStringLiteral("shadow"), QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("blur"), 200}}}})}));
    CHECK(titleXml(bin) == original);
    auto bad = titleDocument(original);
    bad.elementsByTagName(QStringLiteral("content")).at(0).toElement().removeAttribute(QStringLiteral("font-pixel-size"));
    pCore->projectItemModel()->getClipByBinID(bin)->setProperties({{QStringLiteral("xmldata"), bad.toString()}});
    project.rejectWithoutChange(QStringLiteral("format_title_text"),
                                titleFormatArguments(project, bin, QJsonArray{titleStyleEdit(0, 0, 1, {{QStringLiteral("bold"), true}})}));
    CHECK_FALSE(QApplication::activeModalWidget());
}
TEST_CASE("MCP title surface budget sums independently rasterized native shadow styles before allocation", "[MCP][MCPTitles][guards][shadows]")
{
    McpProject project;
    project.open();
    const auto bin = titleFixture(project, QString(128, QLatin1Char('I')));
    const auto source = pCore->projectItemModel()->getClipByBinID(bin);
    REQUIRE(source);
    const auto original = titleXml(bin);
    const int history = project.undoStack->index();
    QJsonArray edits;
    for (int index = 0; index < 128; ++index) {
        // Each legal native style needs its own ~0.7MP image. One padded
        // document rectangle is small, but their aggregate exceeds64MP.
        edits.append(titleStyleEdit(
            0, index, 1,
            {{QStringLiteral("shadow"), QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("blur"), 200}, {QStringLiteral("x"), index}}}}));
    }
    auto arguments = titleFormatArguments(project, bin, edits);
    const auto rejected = project.call(QStringLiteral("format_title_text"), arguments);
    REQUIRE(rejected.value(QStringLiteral("isError")).toBool());
    CHECK(rejected.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")) ==
          QStringLiteral("TITLE_SURFACE_LIMIT"));
    CHECK(titleXml(bin) == original);
    CHECK(project.undoStack->index() == history);
    project.rejectWithoutChange(QStringLiteral("format_title_text"), arguments);

    // Construct valid native metadata on a plain QTextDocument, which does
    // not rasterize shadows. Never instantiate MyTextItem for this fixture.
    auto oversized = titleDocument(original);
    auto content = oversized.elementsByTagName(QStringLiteral("content")).at(0).toElement();
    QTextDocument words;
    words.setHtml(content.firstChildElement(QStringLiteral("richtext")).text());
    for (int index = 0; index < 128; ++index) {
        QTextCursor cursor(&words);
        cursor.setPosition(index);
        cursor.setPosition(index + 1, QTextCursor::KeepAnchor);
        QTextCharFormat style;
        style.setProperty(TitlerShadowV1::Enabled, true);
        style.setProperty(TitlerShadowV1::Blur, 200);
        style.setProperty(TitlerShadowV1::OffsetX, index);
        cursor.mergeCharFormat(style);
    }
    const auto metadata = TitlerShadowV1::save(oversized, &words);
    REQUIRE(!metadata.isNull());
    content.appendChild(metadata);
    const auto oversizedXml = oversized.toString();
    const ObjectId owner(KdenliveObjectType::BinClip, bin.toInt(), QUuid());
    QElapsedTimer deadline;
    deadline.start();
    while ((pCore->taskManager.hasPendingJob(owner, AbstractTask::LOADJOB) || pCore->taskManager.hasPendingJob(owner, AbstractTask::THUMBJOB)) &&
           deadline.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    REQUIRE_FALSE(pCore->taskManager.hasPendingJob(owner, AbstractTask::LOADJOB));
    REQUIRE_FALSE(pCore->taskManager.hasPendingJob(owner, AbstractTask::THUMBJOB));
    struct RestoreSourceTitle
    {
        std::shared_ptr<ProjectClip> clip;
        QString xml;
        ~RestoreSourceTitle() { clip->setProducerProperty(QStringLiteral("xmldata"), xml); }
    } restore{source, original};
    // Direct property assignment deliberately avoids native thumbnail/reload
    // requests: only the bounded inspector may read this oversized fixture.
    source->setProducerProperty(QStringLiteral("xmldata"), oversizedXml);
    const auto inspected = project.call(QStringLiteral("get_title"), {{QStringLiteral("binId"), bin}, {QStringLiteral("itemIndex"), 0}});
    REQUIRE(inspected.value(QStringLiteral("isError")).toBool());
    CHECK(inspected.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")) ==
          QStringLiteral("UNSUPPORTED_TEXT"));
    CHECK(titleXml(bin) == oversizedXml);
    CHECK(project.undoStack->index() == history);
    source->setProducerProperty(QStringLiteral("xmldata"), original);
    const auto valid = project.success(QStringLiteral("get_title"), {{QStringLiteral("binId"), bin}, {QStringLiteral("itemIndex"), 0}});
    CHECK(valid.value(QStringLiteral("text")).toString() == QString(128, QLatin1Char('I')));
    // A real named declaration is refused; Qt's empty no-DTD placeholder is
    // accepted by all ordinary native-title cases above.
    const auto root = titleDocument(original).documentElement();
    QDomDocument declared(QDomImplementation().createDocumentType(QStringLiteral("kdenlivetitle"), QString(), QString()));
    declared.appendChild(declared.importNode(root, true));
    REQUIRE(declared.doctype().name() == QStringLiteral("kdenlivetitle"));
    source->setProducerProperty(QStringLiteral("xmldata"), declared.toString());
    project.rejectWithoutChange(QStringLiteral("get_title"), {{QStringLiteral("binId"), bin}, {QStringLiteral("itemIndex"), 0}});
    CHECK(project.undoStack->index() == history);
}
