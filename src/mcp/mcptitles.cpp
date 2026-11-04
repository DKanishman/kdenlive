/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "mcptitles.h"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "mcphelpers.h"
#include "titler/graphicsscenerectmove.h"
#include "titler/richtextformat.h"
#include "titler/richtextgradient.h"
#include "titler/richtextoutline.h"
#include "titler/richtextshadow.h"
#include "titler/richtextspacing.h"
#include "titler/titledocument.h"
#include <QAbstractTextDocumentLayout>
#include <QCryptographicHash>
#include <QDomDocument>
#include <QFontInfo>
#include <QPointer>
#include <QSet>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <algorithm>
#include <map>
#include <memory>

namespace {
constexpr int MaxTitleBytes = 524288;
constexpr int MaxText = 32768;
QString k(const char *value)
{
    return QString::fromLatin1(value);
}
QJsonObject text(const char *description)
{
    return Mcp::property(k("string"), k(description));
}
QJsonObject integer(const char *description)
{
    return Mcp::property(k("integer"), k(description));
}
QJsonObject boolean(const char *description)
{
    return Mcp::property(k("boolean"), k(description));
}
QString hash(const QString &xml)
{
    return QString::fromLatin1(QCryptographicHash::hash(xml.toUtf8(), QCryptographicHash::Sha256).toHex());
}
bool only(const QJsonObject &object, const QSet<QString> &keys)
{
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!keys.contains(it.key())) return false;
    return true;
}
bool color(const QJsonValue &value, QColor &result)
{
    if (!value.isString() || value.toString().size() > 32) return false;
    result = QColor(value.toString());
    return result.isValid();
}
bool boundary(const QString &plain, int position)
{
    return position >= 0 && position <= plain.size() &&
           !(position > 0 && position < plain.size() && plain.at(position - 1).isHighSurrogate() && plain.at(position).isLowSurrogate());
}
QString plainText(const QDomElement &content)
{
    // TitleDocument's first text child is the legacy fallback, never its HTML.
    const auto first = content.firstChild();
    return first.isText() || first.isCDATASection() ? first.nodeValue() : QString();
}
struct NativeTitle
{
    std::shared_ptr<ProjectClip> clip;
    QString original;
    QDomDocument xml;
    QList<QDomElement> items;
    int width = 0, height = 0;
};
bool loadTitle(const QString &binId, NativeTitle &title, QString &reason)
{
    const auto bin = pCore ? pCore->projectItemModel() : nullptr;
    title.clip = bin && bin->hasClip(binId) ? bin->getClipByBinID(binId) : nullptr;
    if (!title.clip || !title.clip->statusReady() || title.clip->clipType() != ClipType::Text) {
        reason = k("Choose a ready native title bin clip.");
        return false;
    }
    title.original = title.clip->getProducerProperty(k("xmldata"));
    if (title.original.isEmpty() || title.original.toUtf8().size() > MaxTitleBytes) {
        reason = k("The native title XML is empty or exceeds 512 KiB.");
        return false;
    }
    const auto parsed = title.xml.setContent(title.original, QDomDocument::ParseOption::PreserveSpacingOnlyNodes);
    if (!parsed) {
        reason = k("The native title XML is invalid: ") + parsed.errorMessage;
        return false;
    }
    // QDomDocument retains an empty document-type placeholder even when no DTD
    // was declared. Its name, rather than isNull(), identifies a real declaration.
    if (!title.xml.doctype().name().isEmpty()) {
        reason = k("Native title document-type declarations are unsupported.");
        return false;
    }
    const auto root = title.xml.documentElement();
    bool widthOk = false, heightOk = false;
    title.width = root.attribute(k("width")).toInt(&widthOk);
    title.height = root.attribute(k("height")).toInt(&heightOk);
    if (root.tagName() != k("kdenlivetitle") || !widthOk || !heightOk || title.width < 1 || title.width > 16384 || title.height < 1 || title.height > 16384) {
        reason = k("Choose a native title with valid bounded frame dimensions.");
        return false;
    }
    for (auto item = root.firstChildElement(k("item")); !item.isNull(); item = item.nextSiblingElement(k("item"))) {
        if (title.items.size() >= 512) {
            reason = k("The title exceeds 512 native objects.");
            return false;
        }
        title.items.append(item);
    }
    return true;
}
bool layoutBudget(QTextDocument &document, const QPen &outline, const TitlerShadowV1::Style &shadow, qint64 &pixels)
{
    int maximumFont = 1, maximumBlur = shadow.enabled ? shadow.blur : 0;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const auto format = fragment.charFormat();
            const auto font = format.font().resolve(document.defaultFont());
            const int size = font.pixelSize() > 0 ? font.pixelSize() : QFontInfo(font).pixelSize();
            if (size < 1 || size > 2000 || !std::isfinite(font.letterSpacing()) || font.letterSpacing() < -100 || font.letterSpacing() > 1000) return false;
            maximumFont = qMax(maximumFont, size);
            const auto effective = TitlerShadowV1::effective(format, shadow);
            if (effective.enabled) maximumBlur = qMax(maximumBlur, effective.blur);
        }
    const auto size = document.documentLayout()->documentSize();
    const qreal padding = 2 * maximumFont + 2 * TitlerOutline::margin(&document, outline) + 4 * maximumBlur;
    const qreal width = std::ceil(size.width() + padding), height = std::ceil(size.height() + padding);
    constexpr qint64 SingleSurfaceLimit = 16777216;
    constexpr qint64 AggregateSurfaceLimit = 67108864;
    pixels = 0;
    const auto addSurface = [&pixels](qreal requestedWidth, qreal requestedHeight) {
        if (!std::isfinite(requestedWidth) || !std::isfinite(requestedHeight) || requestedWidth < 1 || requestedHeight < 1 || requestedWidth > 32768 ||
            requestedHeight > 32768)
            return false;
        const qint64 w = qint64(std::ceil(requestedWidth)), h = qint64(std::ceil(requestedHeight));
        // Check before multiplying or adding, including independently allocated
        // native shadow styles whose offsets do not enlarge a shared image.
        if (w > SingleSurfaceLimit / h) return false;
        const qint64 area = w * h;
        if (area > AggregateSurfaceLimit - pixels) return false;
        pixels += area;
        return true;
    };
    // Reserve the document/legacy shadow rectangle as well: the native loader
    // installs the legacy shadow before restoring selective shadow overrides.
    if (!addSurface(width, height)) return false;
    if (TitlerShadowV1::hasOverrides(&document)) {
        const qreal margin = TitlerOutline::margin(&document, outline);
        if (!std::isfinite(margin) || margin < 0 || margin > TitlerOutline::MaximumWidth * 2) return false;
        for (const auto &group : TitlerShadowV1::groups(&document, shadow)) {
            QRectF source;
            for (const auto &span : group.spans) {
                const auto ink = TitlerShadowV1::sourceBounds(&document, span);
                if (!std::isfinite(ink.left()) || !std::isfinite(ink.top()) || !std::isfinite(ink.right()) || !std::isfinite(ink.bottom()) ||
                    qAbs(ink.left()) > 1000000000 || qAbs(ink.top()) > 1000000000 || qAbs(ink.right()) > 1000000000 || qAbs(ink.bottom()) > 1000000000)
                    return false;
                source = source.isEmpty() ? ink : source.united(ink);
            }
            if (source.isEmpty()) continue;
            // Same aligned source rectangle/padding as native render(), using
            // qreal until bounds are checked rather than risking QRect overflow.
            const qreal pad = std::ceil(margin) + 2 * group.style.blur + 2;
            if (!addSurface(std::ceil(source.right()) - std::floor(source.left()) + 2 * pad, std::ceil(source.bottom()) - std::floor(source.top()) + 2 * pad))
                return false;
        }
    }
    return true;
}
bool nativeText(const NativeTitle &title, int index, std::unique_ptr<MyTextItem> &result, QString &reason, qint64 *remaining = nullptr)
{
    const auto item = title.items.at(index), content = item.firstChildElement(k("content"));
    const QString plain = plainText(content);
    if (item.attribute(k("type")) != k("QGraphicsTextItem") || content.isNull() || plain.isEmpty() || plain.size() > MaxText || plain.contains(QChar::Null)) {
        reason = k("Choose a nonempty native text object up to 32768 UTF-16 units.");
        return false;
    }
    bool sizeOk = false;
    const int fontPixels = content.attribute(k("font-pixel-size")).toInt(&sizeOk);
    if (!sizeOk || fontPixels < 1 || fontPixels > 2000) {
        reason = k("This text needs native legacy-font conversion or has an unsupported font size.");
        return false;
    }
    const auto transform = item.firstChildElement(k("position")).firstChildElement(k("transform"));
    for (const auto &entry : {QPair<QString, int>{transform.attribute(k("rotation")), 3}, QPair<QString, int>{transform.text(), 9}}) {
        if (entry.first.isEmpty()) continue;
        const auto parts = entry.first.split(QLatin1Char(','));
        if (parts.size() != entry.second) {
            reason = k("The text transform metadata is malformed.");
            return false;
        }
        for (const auto &part : parts) {
            bool ok = false;
            const double value = part.toDouble(&ok);
            if (!ok || !std::isfinite(value) || qAbs(value) > 1000000000) {
                reason = k("The text transform metadata is not finite/bounded.");
                return false;
            }
        }
    }
    const auto rich = content.firstChildElement(k("richtext"));
    QTextDocument check;
    QFont fallback(content.attribute(k("font")));
    fallback.setPixelSize(fontPixels);
    bool spacingOk = true;
    const double spacing = content.attribute(k("letter-spacing"), k("0")).toDouble(&spacingOk);
    if (!spacingOk || !std::isfinite(spacing) || spacing < -100 || spacing > 1000) {
        reason = k("The legacy text spacing is outside the native MCP bounds.");
        return false;
    }
    fallback.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    check.setDefaultFont(fallback);
    if (!rich.isNull()) {
        if (rich.attribute(k("format")) != k("qt-html-v1") || !rich.nextSiblingElement(k("richtext")).isNull()) {
            reason = k("The native rich-text representation is unsupported.");
            return false;
        }
        const QString html = rich.text();
        if (html.contains(k("<img"), Qt::CaseInsensitive) || html.contains(k("<object"), Qt::CaseInsensitive)) {
            reason = k("Inline HTML image/object resources require the native title editor.");
            return false;
        }
        check.setHtml(html);
        if (check.toPlainText() != plain || !TitlerSpacingV1::restore(content, &check) || !TitlerGradientV1::restore(content, &check) ||
            !TitlerOutline::restore(content, &check) || !TitlerShadowV1::restore(content, &check)) {
            reason = k("The native rich-text fallback or format metadata is inconsistent.");
            return false;
        }
    } else
        check.setPlainText(plain);
    if (content.hasAttribute(k("line-spacing"))) {
        bool ok = false;
        const int line = content.attribute(k("line-spacing")).toInt(&ok);
        if (!ok || line < 0 || line > 1000) {
            reason = k("The native line spacing is outside the MCP bounds.");
            return false;
        }
        QTextCursor cursor(&check);
        cursor.select(QTextCursor::Document);
        QTextBlockFormat block;
        block.setLineHeight(line, QTextBlockFormat::LineDistanceHeight);
        cursor.mergeBlockFormat(block);
    }
    TitlerShadowV1::Style shadow;
    if (content.hasAttribute(k("shadow"))) {
        const auto fields = content.attribute(k("shadow")).split(QLatin1Char(';'));
        bool a = false, b = false, c = false, d = false;
        if (fields.size() != 5) {
            reason = k("The native shadow metadata is malformed.");
            return false;
        }
        const int enabled = fields[0].toInt(&a);
        shadow.color = QColor(fields[1]);
        shadow.blur = fields[2].toInt(&b);
        shadow.x = fields[3].toInt(&c);
        shadow.y = fields[4].toInt(&d);
        shadow.enabled = enabled != 0;
        if (!a || !b || !c || !d || (enabled != 0 && enabled != 1) || !shadow.color.isValid() || shadow.blur < 0 || shadow.blur > TitlerShadowV1::MaximumBlur ||
            qAbs(qint64(shadow.x)) > TitlerShadowV1::MaximumOffset || qAbs(qint64(shadow.y)) > TitlerShadowV1::MaximumOffset) {
            reason = k("The native shadow metadata is outside bounded native ranges.");
            return false;
        }
    }
    bool outlineOk = true;
    const double outlineWidth = content.attribute(k("font-outline"), k("0")).toDouble(&outlineOk);
    if (!outlineOk || !std::isfinite(outlineWidth) || outlineWidth < 0 || outlineWidth > TitlerOutline::MaximumWidth) {
        reason = k("The legacy title outline is outside the native MCP bounds.");
        return false;
    }
    qint64 budget = 0;
    if (!layoutBudget(check, QPen(Qt::black, outlineWidth, outlineWidth > 0 ? Qt::SolidLine : Qt::NoPen), shadow, budget) ||
        (remaining && budget > *remaining)) {
        reason = k("Native text/shadow layout exceeds the bounded title surface budget.");
        return false;
    }
    // Text format inspection does not need legacy QGraphics effects. Their XML
    // remains untouched; omitting them from this private load also avoids replacing
    // MyTextItem's internal shadow effect while later geometry still refers to it.
    auto privateItem = item.cloneNode(true);
    for (auto effect = privateItem.firstChildElement(k("effect")); !effect.isNull();) {
        const auto next = effect.nextSiblingElement(k("effect"));
        privateItem.removeChild(effect);
        effect = next;
    }
    int missing = 0, maxZ = 0;
    std::unique_ptr<QGraphicsItem> loaded(TitleDocument::loadItemFromXml(privateItem, QString(), title.width, title.height, missing, maxZ));
    if (!loaded || missing || loaded->type() != QGraphicsTextItem::Type) {
        reason = k("The native title text could not be loaded.");
        return false;
    }
    auto *textItem = static_cast<MyTextItem *>(loaded.get());
    if (textItem->toPlainText() != plain) {
        reason = k("Native loading would change this text; edit it in the title editor first.");
        return false;
    }
    if (remaining) *remaining -= budget;
    loaded.release();
    result.reset(textItem);
    return true;
}
QJsonObject gradientReadback(const QString &recipe)
{
    const auto fields = recipe.split(QLatin1Char(';'));
    if (fields.size() < 5) return {{k("nativeRecipe"), recipe.left(256)}};
    return {{k("startColor"), fields[0]},
            {k("endColor"), fields[1]},
            {k("startPercent"), fields[2].toDouble()},
            {k("endPercent"), fields[3].toDouble()},
            {k("angle"), fields[4].toDouble()}};
}
QJsonObject formatReadback(const QTextCharFormat &format, const MyTextItem &item)
{
    const QFont font = format.font().resolve(item.document()->defaultFont());
    const QBrush brush = format.foreground().style() == Qt::NoBrush ? QBrush(item.defaultTextColor()) : format.foreground();
    QJsonObject data{{k("font"), font.family().left(256)},
                     {k("fontSize"), font.pixelSize() > 0 ? font.pixelSize() : QFontInfo(font).pixelSize()},
                     {k("weight"), font.weight()},
                     {k("italic"), font.italic()},
                     {k("underline"), font.underline()},
                     {k("overline"), font.overline()},
                     {k("strikeOut"), font.strikeOut()},
                     {k("letterSpacing"), font.letterSpacing()},
                     {k("letterSpacingType"), font.letterSpacingType() == QFont::AbsoluteSpacing ? k("absolute") : k("percentage")}};
    const QString recipe = format.property(TitlerGradientV1::Property).toString();
    if (!recipe.isEmpty())
        data.insert(k("gradient"), gradientReadback(recipe));
    else if (!item.data(TitleDocument::Gradient).toString().isEmpty())
        data.insert(k("objectGradient"), gradientReadback(item.data(TitleDocument::Gradient).toString()));
    else if (brush.style() == Qt::SolidPattern)
        data.insert(k("color"), brush.color().name(QColor::HexArgb));
    const auto outline = TitlerOutline::effectivePen(format, item.defaultOutline());
    data.insert(k("outlineWidth"), TitlerOutline::width(outline));
    data.insert(k("outlineColor"), outline.color().name(QColor::HexArgb));
    const auto shadow = TitlerShadowV1::effective(format, item.defaultShadow());
    data.insert(k("shadow"), QJsonObject{{k("enabled"), shadow.enabled},
                                         {k("color"), shadow.color.name(QColor::HexArgb)},
                                         {k("blur"), shadow.blur},
                                         {k("x"), shadow.x},
                                         {k("y"), shadow.y}});
    return data;
}
QJsonObject semanticFormat(const QTextCharFormat &format, const MyTextItem &item)
{
    auto result = formatReadback(format, item);
    const auto font = format.font().resolve(item.document()->defaultFont());
    result.insert(k("resolvedFontStyle"), QFontInfo(font).styleName());
    result.insert(k("nativeGradientRecipe"), format.property(TitlerGradientV1::Property).toString());
    // Selective overrides are deliberate future defaults. Inherited font masks
    // and redundant empty style names are not changes to a title's appearance.
    QJsonObject overrides;
    for (int property = TitlerOutline::WidthProperty; property <= TitlerShadowV1::OffsetY; ++property) {
        if (property == QTextFormat::UserProperty + 4 || !format.hasProperty(property)) continue;
        const auto value = format.property(property);
        overrides.insert(QString::number(property), property == TitlerOutline::ColorProperty || property == TitlerShadowV1::Color
                                                        ? QJsonValue(value.value<QColor>().name(QColor::HexArgb))
                                                        : QJsonValue::fromVariant(value));
    }
    result.insert(k("selectiveOverrides"), overrides);
    return result;
}
QByteArray semanticText(const MyTextItem &item)
{
    QJsonArray runs, blocks;
    QJsonObject previous;
    int previousEnd = -1;
    for (auto block = item.document()->begin(); block.isValid(); block = block.next()) {
        const auto blockFormat = block.blockFormat();
        blocks.append(QJsonObject{{k("position"), block.position()},
                                  {k("alignment"), int(blockFormat.alignment())},
                                  {k("lineHeight"), blockFormat.lineHeight()},
                                  {k("lineHeightType"), blockFormat.lineHeightType()},
                                  {k("separatorFormat"), semanticFormat(block.charFormat(), item)}});
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const auto style = semanticFormat(fragment.charFormat(), item);
            if (fragment.position() == previousEnd && previous.value(k("style")).toObject() == style) {
                previous.insert(k("length"), previous.value(k("length")).toInt() + fragment.length());
                runs[runs.size() - 1] = previous;
            } else {
                previous = {{k("start"), fragment.position()}, {k("length"), fragment.length()}, {k("style"), style}};
                runs.append(previous);
            }
            previousEnd = fragment.position() + fragment.length();
        }
    }
    const QJsonObject state{{k("text"), item.toPlainText()},
                            {k("runs"), runs},
                            {k("blocks"), blocks},
                            {k("alignment"), int(item.alignment())},
                            {k("objectGradient"), item.data(TitleDocument::Gradient).toString()},
                            {k("lineSpacing"), QJsonValue::fromVariant(item.data(TitleDocument::LineSpacing))}};
    return QCryptographicHash::hash(QJsonDocument(state).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}
struct Edit
{
    int item = 0, start = 0, length = 0;
    bool foreground = false;
    QTextCharFormat delta;
    QString alignment;
    int lineSpacing = -1;
};
bool parseStyle(const QJsonObject &style, Edit &edit, QString &reason)
{
    const QSet<QString> keys{k("font"),      k("fontSize"),   k("weight"),       k("bold"),          k("italic"),
                             k("underline"), k("overline"),   k("strikeOut"),    k("letterSpacing"), k("letterSpacingType"),
                             k("color"),     k("gradient"),   k("outlineWidth"), k("outlineColor"),  k("shadow"),
                             k("alignment"), k("lineSpacing")};
    if (style.isEmpty() || !only(style, keys)) {
        reason = k("Supply recognized native font, foreground, outline, shadow or whole-item paragraph properties.");
        return false;
    }
    if (style.contains(k("font"))) {
        const auto font = style.value(k("font"));
        if (!font.isString() || font.toString().isEmpty() || font.toString().size() > 256 || font.toString().contains(QChar::Null)) return false;
        edit.delta.setFontFamilies({font.toString()});
        edit.delta.setFontStyleName(QString());
    }
    int value = 0;
    if (style.contains(k("fontSize"))) {
        if (!Mcp::integer(style, k("fontSize"), 1, 2000, value)) return false;
        edit.delta.setProperty(QTextFormat::FontPixelSize, value);
    }
    if (style.contains(k("weight"))) {
        if (style.contains(k("bold")) || !Mcp::integer(style, k("weight"), 1, 1000, value)) return false;
        edit.delta.setFontWeight(value);
        edit.delta.setFontStyleName(QString());
    }
    for (const auto &name : {k("bold"), k("italic"), k("underline"), k("overline"), k("strikeOut")}) {
        if (!style.contains(name)) continue;
        if (!style.value(name).isBool()) return false;
        const bool enabled = style.value(name).toBool();
        if (name == k("bold")) {
            edit.delta.setFontWeight(enabled ? QFont::Bold : QFont::Normal);
            edit.delta.setFontStyleName(QString());
        } else if (name == k("italic")) {
            edit.delta.setFontItalic(enabled);
            edit.delta.setFontStyleName(QString());
        } else if (name == k("underline"))
            edit.delta.setFontUnderline(enabled);
        else if (name == k("overline"))
            edit.delta.setFontOverline(enabled);
        else
            edit.delta.setFontStrikeOut(enabled);
    }
    if (style.contains(k("letterSpacing"))) {
        double spacing = 0;
        if (!Mcp::real(style, k("letterSpacing"), -100, 1000, spacing)) return false;
        const auto type = style.value(k("letterSpacingType")).toString(k("absolute"));
        if (type != k("absolute") && type != k("percentage")) return false;
        edit.delta.setFontLetterSpacingType(type == k("absolute") ? QFont::AbsoluteSpacing : QFont::PercentageSpacing);
        edit.delta.setFontLetterSpacing(spacing);
    } else if (style.contains(k("letterSpacingType")))
        return false;
    if (style.contains(k("color"))) {
        QColor fill;
        if (style.contains(k("gradient")) || !color(style.value(k("color")), fill)) return false;
        edit.delta.setForeground(fill);
        edit.delta.setProperty(TitlerGradientV1::Property, QString());
        edit.foreground = true;
    }
    if (style.contains(k("gradient"))) {
        if (!style.value(k("gradient")).isObject()) return false;
        const auto gradient = style.value(k("gradient")).toObject();
        if (!only(gradient, {k("startColor"), k("endColor"), k("startPercent"), k("endPercent"), k("angle")})) return false;
        QColor first, last;
        double begin = 0, end = 100, angle = 90;
        if (!color(gradient.value(k("startColor")), first) || !color(gradient.value(k("endColor")), last) ||
            (gradient.contains(k("startPercent")) && !Mcp::real(gradient, k("startPercent"), 0, 100, begin)) ||
            (gradient.contains(k("endPercent")) && !Mcp::real(gradient, k("endPercent"), 0, 100, end)) ||
            (gradient.contains(k("angle")) && !Mcp::real(gradient, k("angle"), 0, 180, angle)) || begin >= end)
            return false;
        const QString recipe = first.name(QColor::HexArgb) + QLatin1Char(';') + last.name(QColor::HexArgb) + QLatin1Char(';') +
                               QString::number(begin, 'g', 17) + QLatin1Char(';') + QString::number(end, 'g', 17) + QLatin1Char(';') +
                               QString::number(angle, 'g', 17);
        edit.delta.setProperty(TitlerGradientV1::Property, recipe);
        edit.foreground = true;
    }
    double width = 0;
    if (style.contains(k("outlineWidth"))) {
        if (!Mcp::real(style, k("outlineWidth"), 0, TitlerOutline::MaximumWidth, width)) return false;
        edit.delta.setProperty(TitlerOutline::WidthProperty, width);
    }
    if (style.contains(k("outlineColor"))) {
        QColor stroke;
        if (!color(style.value(k("outlineColor")), stroke)) return false;
        edit.delta.setProperty(TitlerOutline::ColorProperty, stroke);
    }
    if (style.contains(k("shadow"))) {
        if (!style.value(k("shadow")).isObject()) return false;
        const auto shadow = style.value(k("shadow")).toObject();
        if (shadow.isEmpty() || !only(shadow, {k("enabled"), k("color"), k("blur"), k("x"), k("y")})) return false;
        for (const auto &name : {k("enabled"), k("color"), k("blur"), k("x"), k("y")}) {
            if (!shadow.contains(name)) continue;
            if (name == k("enabled")) {
                if (!shadow.value(name).isBool()) return false;
                edit.delta.setProperty(TitlerShadowV1::Enabled, shadow.value(name).toBool());
            } else if (name == k("color")) {
                QColor c;
                if (!color(shadow.value(name), c)) return false;
                edit.delta.setProperty(TitlerShadowV1::Color, c);
            } else {
                const int minimum = name == k("blur") ? 0 : -TitlerShadowV1::MaximumOffset;
                const int maximum = name == k("blur") ? TitlerShadowV1::MaximumBlur : TitlerShadowV1::MaximumOffset;
                if (!Mcp::integer(shadow, name, minimum, maximum, value)) return false;
                edit.delta.setProperty(name == k("blur") ? TitlerShadowV1::Blur : name == k("x") ? TitlerShadowV1::OffsetX : TitlerShadowV1::OffsetY, value);
            }
        }
    }
    if (style.contains(k("alignment"))) {
        edit.alignment = style.value(k("alignment")).toString();
        if (edit.alignment != k("left") && edit.alignment != k("center") && edit.alignment != k("right") && edit.alignment != k("justify")) return false;
    }
    if (style.contains(k("lineSpacing")) && !Mcp::integer(style, k("lineSpacing"), 0, 1000, edit.lineSpacing)) return false;
    return true;
}
void replaceTextContent(QDomDocument &xml, QDomElement &original, const QDomElement &generated)
{
    // Retain unknown content attributes/children and all item/position/effect
    // nodes. Only the native text and supported text-format representation move.
    const auto attributes = generated.attributes();
    for (int i = 0; i < attributes.size(); ++i)
        original.setAttribute(attributes.item(i).nodeName(), attributes.item(i).nodeValue());
    if (!generated.hasAttribute(k("gradient"))) original.removeAttribute(k("gradient"));
    const QSet<QString> native{k("richtext"), k("richtext-spacing"), k("richtext-gradients"), k("richtext-outlines"), k("richtext-shadows")};
    for (auto node = original.firstChild(); !node.isNull();) {
        const auto next = node.nextSibling();
        if (node.isText() || node.isCDATASection() || (node.isElement() && native.contains(node.nodeName()))) original.removeChild(node);
        node = next;
    }
    QDomNode before = original.firstChild();
    for (auto node = generated.firstChild(); !node.isNull(); node = node.nextSibling()) {
        if (node.isText() || node.isCDATASection() || (node.isElement() && native.contains(node.nodeName()))) {
            const auto imported = xml.importNode(node, true);
            if (before.isNull())
                original.appendChild(imported);
            else
                original.insertBefore(imported, before);
        }
    }
}
} // namespace
namespace McpTitles {
QJsonArray toolCatalog()
{
    auto style = Mcp::property(
        k("object"),
        k("Native selected character properties: font/fontSize/weight or bold, italic/underline/overline/strikeOut, letterSpacing+letterSpacingType, color or "
          "gradient, outlineWidth/outlineColor and shadow. alignment and lineSpacing affect the WHOLE text object, as in the native editor."));
    auto edit = Mcp::property(k("object"),
                              k("Text-object index plus start/length in UTF-16 units and a style object; omit BOTH range fields to format the whole item."));
    edit.insert(k("properties"), QJsonObject{{k("itemIndex"), integer("Index from get_title.")},
                                             {k("start"), integer("First UTF-16 unit; no split surrogate pairs.")},
                                             {k("length"), integer("Positive UTF-16 length.")},
                                             {k("style"), style}});
    edit.insert(k("required"), QJsonArray{k("itemIndex"), k("style")});
    edit.insert(k("additionalProperties"), false);
    auto edits = Mcp::property(k("array"), k("1..128 non-overlapping native text range edits, applied in one document undo step."));
    edits.insert(k("items"), edit);
    edits.insert(k("minItems"), 1);
    edits.insert(k("maxItems"), 128);
    return {Mcp::tool(k("get_title"),
                      k("Inspect native title object indices and hashes. With itemIndex, read exact plain text and paged native UTF-16 format runs; never "
                        "returns caller-executable XML/HTML."),
                      {{k("binId"), text("Ready native title bin ID.")},
                       {k("itemIndex"), integer("Optional text object for detailed formatting readback.")},
                       {k("offset"), integer("First object/run index, default zero.")},
                       {k("limit"), integer("Page size1..64, default16.")}},
                      {k("binId")}, true),
            Mcp::tool(k("format_title_text"),
                      k("Format existing native title text ranges without replacing other objects, text, position/effects/viewports or duration. One native "
                        "bin-property undo; title placements refresh through the native reload path. Paragraph alignment/spacing applies to whole items. "
                        "Gradient-region splits can change other glyph colors and require explicit acknowledgment."),
                      {{k("expectedState"), text("Fresh document state.")},
                       {k("binId"), text("Ready native title bin ID.")},
                       {k("expectedTitleHash"), text("Exact titleHash from get_title.")},
                       {k("edits"), edits},
                       {k("acknowledgeGradientReflow"),
                        boolean("Allow native gradient-region coordinate changes reported in affectedGradientRegions; defaultfalse.")}},
                      {k("expectedState"), k("binId"), k("expectedTitleHash"), k("edits")})};
}
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &state,
                     const std::function<QJsonObject()> &stateReader)
{
    Q_UNUSED(owner);
    if (name != k("get_title") && name != k("format_title_text")) return {};
    auto reject = [&](const char *code, const QString &reason) { return Mcp::failure(k(code), reason, state); };
    auto finish = [&](QJsonObject data) {
        data.insert(k("ok"), true);
        data.insert(k("state"), stateReader());
        return Mcp::toolResult(data);
    };
    NativeTitle title;
    QString reason;
    if (!loadTitle(arguments.value(k("binId")).toString(), title, reason)) return reject("INVALID_TITLE", reason);
    const auto titleHash = hash(title.original);
    if (name == k("get_title")) {
        int offset = 0, limit = 16;
        if ((arguments.contains(k("offset")) && !Mcp::integer(arguments, k("offset"), 0, 100000, offset)) ||
            (arguments.contains(k("limit")) && !Mcp::integer(arguments, k("limit"), 1, 64, limit)))
            return reject("INVALID_ARGUMENT", k("Use a nonnegative page offset and limit1..64."));
        QJsonObject data{{k("binId"), title.clip->clipId()},
                         {k("titleHash"), titleHash},
                         {k("width"), title.width},
                         {k("height"), title.height},
                         {k("durationFrames"), title.clip->getFramePlaytime()},
                         {k("offset"), offset},
                         {k("scope"), k("Native title XML; indices remain valid only for this titleHash.")}};
        QJsonArray entries;
        if (arguments.contains(k("itemIndex"))) {
            int index = 0;
            if (!Mcp::integer(arguments, k("itemIndex"), 0, int(title.items.size()) - 1, index))
                return reject("INVALID_ARGUMENT", k("Choose an existing title object index."));
            std::unique_ptr<MyTextItem> item;
            if (!nativeText(title, index, item, reason)) return reject("UNSUPPORTED_TEXT", reason);
            int total = 0;
            for (QTextBlock block = item->document()->begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    if (!fragment.isValid()) continue;
                    if (total >= offset && total < offset + limit)
                        entries.append(QJsonObject{
                            {k("start"), fragment.position()}, {k("length"), fragment.length()}, {k("style"), formatReadback(fragment.charFormat(), *item)}});
                    ++total;
                }
            data.insert(k("itemIndex"), index);
            data.insert(k("text"), item->toPlainText());
            data.insert(k("characters"), item->toPlainText().size());
            data.insert(k("runs"), entries);
            data.insert(k("total"), total);
            data.insert(k("nextOffset"), offset + entries.size() < total ? offset + entries.size() : -1);
            data.insert(k("alignment"), int(item->alignment()));
            data.insert(k("lineSpacing"),
                        item->data(TitleDocument::LineSpacing).isNull() ? QJsonValue() : QJsonValue(item->data(TitleDocument::LineSpacing).toInt()));
        } else {
            for (int index = offset; index < title.items.size() && index < offset + limit; ++index) {
                const auto item = title.items.at(index), content = item.firstChildElement(k("content")), pos = item.firstChildElement(k("position"));
                QJsonObject summary{{k("itemIndex"), index},
                                    {k("type"), item.attribute(k("type"))},
                                    {k("z"), item.attribute(k("z-index")).toDouble()},
                                    {k("x"), pos.attribute(k("x")).toDouble()},
                                    {k("y"), pos.attribute(k("y")).toDouble()}};
                if (item.attribute(k("type")) == k("QGraphicsTextItem")) {
                    const auto plain = plainText(content);
                    summary.insert(k("characters"), plain.size());
                    summary.insert(k("textPreview"), plain.left(256));
                }
                entries.append(summary);
            }
            data.insert(k("items"), entries);
            data.insert(k("total"), title.items.size());
            data.insert(k("nextOffset"), offset + entries.size() < title.items.size() ? offset + entries.size() : -1);
        }
        return finish(data);
    }
    if (arguments.value(k("expectedTitleHash")).toString() != titleHash)
        return reject("STALE_TITLE", k("The title changed. Inspect it again before preparing text ranges."));
    const auto values = arguments.value(k("edits"));
    if (!values.isArray() || values.toArray().isEmpty() || values.toArray().size() > 128)
        return reject("INVALID_ARGUMENT", k("Supply1..128 structured text edits."));
    std::map<int, std::unique_ptr<MyTextItem>> items;
    QList<Edit> edits;
    qint64 remainingSurface = 67108864;
    QJsonArray affected, paragraphScope;
    for (const auto &value : values.toArray()) {
        const auto object = value.toObject();
        Edit edit;
        if (!value.isObject() || !only(object, {k("itemIndex"), k("start"), k("length"), k("style")}) ||
            !Mcp::integer(object, k("itemIndex"), 0, int(title.items.size()) - 1, edit.item) || !object.value(k("style")).isObject())
            return reject("INVALID_ARGUMENT", k("Each edit needs an existing text item and structured style."));
        if (!items.contains(edit.item)) {
            std::unique_ptr<MyTextItem> loaded;
            if (!nativeText(title, edit.item, loaded, reason, &remainingSurface)) return reject("UNSUPPORTED_TEXT", reason);
            items.emplace(edit.item, std::move(loaded));
        }
        const auto &item = items.at(edit.item);
        const auto plain = item->toPlainText();
        edit.length = plain.size();
        if (object.contains(k("start")) != object.contains(k("length")) ||
            (object.contains(k("start")) &&
             (!Mcp::integer(object, k("start"), 0, int(plain.size()), edit.start) || !Mcp::integer(object, k("length"), 1, int(plain.size()), edit.length))) ||
            edit.start > plain.size() - edit.length || !boundary(plain, edit.start) || !boundary(plain, edit.start + edit.length))
            return reject("INVALID_RANGE", k("Use a positive native UTF-16 range without splitting a surrogate pair."));
        reason = k("The text style contains an invalid type, color, font size, weight or native range.");
        if (!parseStyle(object.value(k("style")).toObject(), edit, reason)) return reject("INVALID_STYLE", reason);
        for (const auto &prior : edits)
            if (prior.item == edit.item) {
                if (edit.start < prior.start + prior.length && prior.start < edit.start + edit.length)
                    return reject("OVERLAPPING_RANGES", k("Combine properties for overlapping ranges into one style."));
                if ((!edit.alignment.isEmpty() && !prior.alignment.isEmpty() && edit.alignment != prior.alignment) ||
                    (edit.lineSpacing >= 0 && prior.lineSpacing >= 0 && edit.lineSpacing != prior.lineSpacing))
                    return reject("PARAGRAPH_CONFLICT", k("Whole-item paragraph properties must agree across ranges."));
            }
        if (!edit.alignment.isEmpty() || edit.lineSpacing >= 0)
            paragraphScope.append(QJsonObject{{k("itemIndex"), edit.item}, {k("start"), 0}, {k("length"), plain.size()}, {k("scope"), k("whole_text_object")}});
        edits.append(edit);
    }
    std::map<int, QVector<TitlerGradientV1::Region>> originalGradients;
    std::map<int, QString> originalContent;
    std::map<int, QByteArray> originalSemantics;
    QSet<int> legacyGradients;
    for (auto &[index, item] : items) {
        originalGradients[index] = TitlerGradientV1::regions(item->document());
        if (!item->data(TitleDocument::Gradient).toString().isEmpty()) legacyGradients.insert(index);
        item->doUpdateGeometry();
        originalContent[index] = TitleDocument::xmlItem(item.get(), title.width, title.height).toString();
        originalSemantics[index] = semanticText(*item);
        // Delay native shadow rasterization until every private edit has passed
        // its layout budget. This private item is not connected to an editor.
        QObject::disconnect(item->document(), &QTextDocument::contentsChanged, item.get(), &MyTextItem::doUpdateGeometry);
    }
    for (const auto &edit : edits) {
        auto &item = items.at(edit.item);
        QTextCursor cursor(item->document());
        if (edit.foreground) {
            const auto legacy = item->data(TitleDocument::Gradient).toString();
            if (!legacy.isEmpty()) {
                QTextCursor all(item->document());
                all.select(QTextCursor::Document);
                QTextCharFormat promoted;
                promoted.setProperty(TitlerGradientV1::Property, legacy);
                all.mergeCharFormat(promoted);
                item->setData(TitleDocument::Gradient, QString());
            }
        }
        cursor.setPosition(edit.start);
        cursor.setPosition(edit.start + edit.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(edit.delta);
        if (!edit.alignment.isEmpty())
            item->setAlignment(edit.alignment == k("center")    ? Qt::AlignHCenter
                               : edit.alignment == k("right")   ? Qt::AlignRight
                               : edit.alignment == k("justify") ? Qt::AlignJustify
                                                                : Qt::AlignLeft);
        if (edit.lineSpacing >= 0) {
            QTextCursor all(item->document());
            all.select(QTextCursor::Document);
            QTextBlockFormat block;
            block.setLineHeight(edit.lineSpacing, QTextBlockFormat::LineDistanceHeight);
            all.mergeBlockFormat(block);
            item->setData(TitleDocument::LineSpacing, edit.lineSpacing);
        }
    }
    QSet<int> changedItems;
    for (const auto &[index, item] : items)
        if (semanticText(*item) != originalSemantics[index]) changedItems.insert(index);
    if (changedItems.isEmpty()) return finish({{k("binId"), title.clip->clipId()}, {k("titleHash"), titleHash}, {k("changed"), false}});
    // Compare complete original/native gradient regions after all private edits.
    // Splits AND merges can recolor glyphs outside selected foreground ranges.
    for (auto &[index, item] : items) {
        QList<QPair<int, int>> selections;
        for (const auto &edit : edits)
            if (edit.item == index && edit.foreground) selections.append({edit.start, edit.start + edit.length});
        std::sort(selections.begin(), selections.end());
        const auto outside = [&selections](int start, int end) {
            QList<QPair<int, int>> ranges;
            int cursor = start;
            for (const auto &selection : selections) {
                if (selection.second <= cursor || selection.first >= end) continue;
                if (selection.first > cursor) ranges.append({cursor, qMin(end, selection.first)});
                cursor = qMax(cursor, selection.second);
                if (cursor >= end) break;
            }
            if (cursor < end) ranges.append({cursor, end});
            return ranges;
        };
        if (!selections.isEmpty() && legacyGradients.contains(index) && !outside(0, item->toPlainText().size()).isEmpty()) {
            affected.append(QJsonObject{
                {k("itemIndex"), index}, {k("start"), 0}, {k("length"), item->toPlainText().size()}, {k("reason"), k("legacy_object_gradient_promotion")}});
        }
        const auto after = TitlerGradientV1::regions(item->document());
        for (const auto &before : originalGradients[index]) {
            const auto untouched = outside(before.start, before.start + before.length);
            if (untouched.isEmpty()) continue;
            bool changed = false;
            for (const auto &region : after) {
                if (region.data != before.data || (region.start == before.start && region.length == before.length)) continue;
                for (const auto &range : untouched)
                    if (range.first < region.start + region.length && region.start < range.second) changed = true;
            }
            if (changed)
                affected.append(QJsonObject{
                    {k("itemIndex"), index}, {k("start"), before.start}, {k("length"), before.length}, {k("reason"), k("native_gradient_region_coordinates")}});
        }
    }
    if (!affected.isEmpty() && !arguments.value(k("acknowledgeGradientReflow")).toBool()) {
        auto data = Mcp::failure(k("GRADIENT_REFLOW_ACKNOWLEDGEMENT_REQUIRED"),
                                 k("This foreground edit changes native gradient coordinates outside its selection. Acknowledge that scope explicitly or "
                                   "choose a whole gradient region."),
                                 state);
        auto content = data.value(k("structuredContent")).toObject();
        content.insert(k("affectedGradientRegions"), affected);
        return Mcp::toolResult(content, true);
    }
    qint64 finalSurface = 0;
    for (auto &[index, item] : items) {
        qint64 pixels = 0;
        if (!layoutBudget(*item->document(), item->defaultOutline(), item->defaultShadow(), pixels) || pixels > 67108864 - finalSurface)
            return reject("TITLE_SURFACE_LIMIT", k("Requested formatting exceeds the bounded native text/shadow surface budget; no edit was applied."));
        finalSurface += pixels;
    }
    bool changed = false;
    for (auto &[index, item] : items) {
        if (!changedItems.contains(index)) continue;
        if (item->toPlainText() != plainText(title.items.at(index).firstChildElement(k("content"))))
            return reject("TEXT_CHANGED", k("Formatting changed native text; no document edit was applied."));
        if (item->data(TitleDocument::Gradient).toString().isEmpty())
            TitlerGradientV1::applyBrushes(item->document(), int(item->baseBoundingRect().width()), int(item->baseBoundingRect().height()));
        item->doUpdateGeometry();
        const auto serialized = TitleDocument::xmlItem(item.get(), title.width, title.height);
        if (serialized.toString() == originalContent[index]) continue;
        changed = true;
        const auto generated = serialized.documentElement().firstChildElement(k("content"));
        if (generated.isNull()) return reject("SERIALIZATION_FAILED", k("Native title text could not be serialized; no edit was applied."));
        auto original = title.items[index].firstChildElement(k("content"));
        replaceTextContent(title.xml, original, generated);
    }
    const auto updated = title.xml.toString();
    if (updated.toUtf8().size() > MaxTitleBytes) return reject("TITLE_LIMIT", k("Formatted native title exceeds512KiB."));
    if (!changed || hash(updated) == titleHash) return finish({{k("binId"), title.clip->clipId()}, {k("titleHash"), titleHash}, {k("changed"), false}});
    const QPointer<KdenliveDoc> document = pCore->currentDoc();
    const auto source = title.clip;
    const auto apply = [document, source](const QString &xml) {
        if (!document || pCore->currentDoc() != document || !pCore->projectItemModel()->hasClip(source->clipId()) ||
            pCore->projectItemModel()->getClipByBinID(source->clipId()) != source)
            return false;
        // Match Bin's native title edit command for both redo and undo.
        source->setProperties({{k("xmldata"), xml}, {k("force_reload"), k("1")}});
        pCore->refreshProjectMonitorOnce();
        return source->getProducerProperty(k("xmldata")) == xml;
    };
    if (!apply(updated)) {
        apply(title.original);
        return reject("EDIT_FAILED", k("The native title did not retain the edit; its original XML was restored."));
    }
    pCore->pushUndo([apply, before = title.original] { return apply(before); }, [apply, updated] { return apply(updated); }, k("MCP format native title text"));
    return finish({{k("binId"), source->clipId()},
                   {k("titleHash"), hash(updated)},
                   {k("changed"), true},
                   {k("undoable"), true},
                   {k("reloadStatus"), source->statusReady() ? k("ready") : k("loading")},
                   {k("affectedGradientRegions"), affected},
                   {k("paragraphScope"), paragraphScope}});
}
} // namespace McpTitles