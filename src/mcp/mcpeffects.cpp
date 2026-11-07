/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcphelpers.h"
#include "mcprotospline.h"
#include "mcpserver.h"

#include "assets/keyframes/model/keyframemodel.hpp"
#include "assets/keyframes/model/keyframemodellist.hpp"
#include "assets/model/assetparametermodel.hpp"
#include "bin/clipcreator.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "effects/effectsrepository.hpp"
#include "effects/effectstack/model/effectitemmodel.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "jobs/filtertask.h"
#include "jobs/taskmanager.h"
#include "project/projectmanager.h"
#include "timeline2/model/compositionmodel.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/model/trackmodel.hpp"
#include "titler/graphicsscenerectmove.h"
#include "titler/titledocument.h"
#include "transitions/transitionsrepository.hpp"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGraphicsEllipseItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mlt++/MltFilter.h>
#include <mlt++/MltProperties.h>

using namespace Mcp;
namespace {
constexpr int MaxFrame = 10000000;
constexpr int MaxEffects = 512;

bool text(const QJsonObject &object, const QString &name, QString &result, int limit = 4096, bool empty = false)
{
    const auto value = object.value(name);
    if (!value.isString()) return false;
    result = value.toString();
    return (empty || !result.isEmpty()) && result.size() <= limit && !result.contains(QChar::Null);
}
QString scalar(const QJsonValue &value)
{
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    if (value.isDouble() && std::isfinite(value.toDouble())) return QString::number(value.toDouble(), 'g', 15);
    if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    return {};
}
QString parameterType(ParamType type)
{
    switch (type) {
    case ParamType::Double:
        return QStringLiteral("number");
    case ParamType::List:
        return QStringLiteral("list");
    case ParamType::ListWithDependency:
        return QStringLiteral("dependent_list");
    case ParamType::UrlList:
        return QStringLiteral("file_list");
    case ParamType::Bool:
        return QStringLiteral("boolean");
    case ParamType::Switch:
        return QStringLiteral("switch");
    case ParamType::EffectButtons:
        return QStringLiteral("effect_buttons");
    case ParamType::MultiSwitch:
        return QStringLiteral("multi_switch");
    case ParamType::AnimatedRect:
        return QStringLiteral("animated_rectangle");
    case ParamType::AnimatedFakeRect:
        return QStringLiteral("animated_fake_rectangle");
    case ParamType::FakeRect:
        return QStringLiteral("fake_rectangle");
    case ParamType::AnimatedPoint:
        return QStringLiteral("animated_point");
    case ParamType::Point:
        return QStringLiteral("point");
    case ParamType::AnimatedFakePoint:
        return QStringLiteral("animated_fake_point");
    case ParamType::FakePoint:
        return QStringLiteral("fake_point");
    case ParamType::Geometry:
        return QStringLiteral("geometry");
    case ParamType::KeyframeParam:
        return QStringLiteral("animated_number");
    case ParamType::Color:
        return QStringLiteral("animated_color");
    case ParamType::FixedColor:
        return QStringLiteral("color");
    case ParamType::ColorWheel:
        return QStringLiteral("color_wheel");
    case ParamType::Position:
        return QStringLiteral("position");
    case ParamType::Curve:
        return QStringLiteral("curve");
    case ParamType::Bezier_spline:
        return QStringLiteral("bezier_spline");
    case ParamType::AvCurve:
        return QStringLiteral("av_curve");
    case ParamType::Roto_spline:
        return QStringLiteral("roto_spline");
    case ParamType::Wipe:
        return QStringLiteral("wipe");
    case ParamType::Url:
        return QStringLiteral("file");
    case ParamType::Keywords:
        return QStringLiteral("text");
    case ParamType::Fontfamily:
        return QStringLiteral("font");
    case ParamType::Filterjob:
        return QStringLiteral("analysis_job");
    case ParamType::Readonly:
        return QStringLiteral("readonly");
    case ParamType::Hidden:
        return QStringLiteral("hidden");
    case ParamType::GradientEditor:
        return QStringLiteral("gradient");
    default:
        return QStringLiteral("unknown");
    }
}

bool validAssStyle(const QString &line)
{
    if (!line.startsWith(QStringLiteral("Style:")) || line.contains(QLatin1Char('\n')) || line.contains(QChar::Null)) return false;
    const auto fields = line.section(QLatin1Char(':'), 1).trimmed().split(QLatin1Char(','));
    if (fields.size() != 23 || fields.at(0).trimmed().isEmpty() || fields.at(1).trimmed().isEmpty()) return false;
    for (int index = 2; index < fields.size(); ++index) {
        if (index >= 3 && index <= 6) {
            if (!QRegularExpression(QStringLiteral("^&H[0-9a-fA-F]{6,8}&?$"), QRegularExpression::CaseInsensitiveOption)
                     .match(fields.at(index).trimmed())
                     .hasMatch())
                return false;
        } else {
            bool valid = false;
            const double number = fields.at(index).trimmed().toDouble(&valid);
            if (!valid || !std::isfinite(number)) return false;
        }
    }
    return true;
}

// File intake mirrors the stricter native ASS transaction. The generic
// style editor above retains its existing color and numeric input contract.
bool validAssImportStyle(const QString &line)
{
    if (!validAssStyle(line)) return false;
    const auto fields = line.section(QLatin1Char(':'), 1).trimmed().split(QLatin1Char(','));
    static const QRegularExpression color(QStringLiteral("^&H[0-9a-fA-F]{8}&?$"), QRegularExpression::CaseInsensitiveOption);
    for (int index = 2; index < fields.size(); ++index) {
        if (index >= 3 && index <= 6) {
            if (!color.match(fields.at(index).trimmed()).hasMatch()) return false;
        } else if ((index >= 7 && index <= 10) || index == 15 || index >= 18) {
            bool valid = false;
            fields.at(index).toInt(&valid);
            if (!valid) return false;
        }
    }
    return true;
}

QJsonArray parameters(const std::shared_ptr<AssetParameterModel> &model)
{
    QJsonArray result;
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto ix = model->index(row, 0);
        const QString name = model->data(ix, AssetParameterModel::NameRole).toString();
        const auto type = model->data(ix, AssetParameterModel::TypeRole).value<ParamType>();
        QJsonObject entry{{QStringLiteral("name"), name},
                          {QStringLiteral("label"), model->data(ix, Qt::DisplayRole).toString()},
                          {QStringLiteral("type"), parameterType(type)},
                          {QStringLiteral("animated"), AssetParameterModel::isAnimated(type)},
                          {QStringLiteral("value"), QJsonValue::fromVariant(model->getParamFromName(name))},
                          {QStringLiteral("rawNativeValue"), model->getParam(name)}};
        for (const auto &role : {std::pair<const char *, int>{"minimum", AssetParameterModel::MinRole},
                                 {"maximum", AssetParameterModel::MaxRole},
                                 {"default", AssetParameterModel::DefaultRole},
                                 {"choices", AssetParameterModel::ListValuesRole},
                                 {"choiceNames", AssetParameterModel::ListNamesRole},
                                 {"comment", AssetParameterModel::CommentRole}}) {
            const auto value = model->data(ix, role.second);
            if (value.isValid()) entry.insert(QString::fromLatin1(role.first), QJsonValue::fromVariant(value));
        }
        result.append(entry);
    }
    return result;
}
QJsonObject parameterReadback(const std::shared_ptr<AssetParameterModel> &model)
{
    QJsonObject logical, raw;
    for (const auto &entry : model->getAllParameters()) {
        raw.insert(entry.first, model->getParam(entry.first));
        const auto ix = model->getParamIndexFromName(entry.first);
        logical.insert(entry.first, ix.isValid() ? QJsonValue::fromVariant(model->getParamFromName(entry.first)) : QJsonValue(entry.second.toString()));
    }
    return {{QStringLiteral("assetId"), model->getAssetId()},
            {QStringLiteral("parameters"), logical},
            {QStringLiteral("rawNativeParameters"), raw},
            {QStringLiteral("parameterSchema"), parameters(model)}};
}
void collectEffects(const std::shared_ptr<AbstractEffectItem> &item, QJsonArray &result, int parent = -1)
{
    if (!item || result.size() >= MaxEffects) return;
    QJsonObject entry{{QStringLiteral("instanceId"), item->getId()},
                      {QStringLiteral("parentId"), parent},
                      {QStringLiteral("index"), item->row()},
                      {QStringLiteral("enabled"), item->isAssetEnabled()},
                      {QStringLiteral("group"), item->effectItemType() == EffectItemType::Group}};
    if (const auto effect = std::dynamic_pointer_cast<EffectItemModel>(item)) {
        const auto values = parameterReadback(effect);
        for (auto i = values.begin(); i != values.end(); ++i)
            entry.insert(i.key(), i.value());
        entry.insert(QStringLiteral("inFrame"), effect->filter().get_in());
        entry.insert(QStringLiteral("outFrame"), effect->filter().get_out());
        entry.insert(QStringLiteral("forcedZone"), effect->hasForcedInOut());
        entry.insert(QStringLiteral("builtIn"), effect->isBuiltIn());
    }
    result.append(entry);
    for (int row = 0; row < item->childCount(); ++row)
        collectEffects(std::dynamic_pointer_cast<AbstractEffectItem>(item->child(row)), result, item->getId());
}
QJsonObject stackReadback(const std::shared_ptr<EffectStackModel> &stack)
{
    QJsonArray result;
    for (int row = 0; row < stack->rowCount(); ++row)
        collectEffects(stack->getEffectStackRow(row), result);
    return {{QStringLiteral("enabled"), stack->isStackEnabled()}, {QStringLiteral("effects"), result}};
}
std::shared_ptr<AbstractEffectItem> findEffect(const std::shared_ptr<EffectStackModel> &stack, int id)
{
    std::function<std::shared_ptr<AbstractEffectItem>(std::shared_ptr<AbstractEffectItem>)> find = [&](auto item) -> std::shared_ptr<AbstractEffectItem> {
        if (!item) return {};
        if (item->getId() == id) return item;
        for (int row = 0; row < item->childCount(); ++row) {
            if (auto found = find(std::dynamic_pointer_cast<AbstractEffectItem>(item->child(row)))) return found;
        }
        return {};
    };
    for (int row = 0; row < stack->rowCount(); ++row)
        if (auto found = find(stack->getEffectStackRow(row))) return found;
    return {};
}
bool validateParameter(const std::shared_ptr<AssetParameterModel> &model, const QString &name, const QJsonValue &value, QString &converted, QString &reason)
{
    const auto ix = model->getParamIndexFromName(name);
    if (!ix.isValid()) {
        reason = QStringLiteral("Parameter is absent from the native editable schema: ") + name;
        return false;
    }
    const auto type = model->data(ix, AssetParameterModel::TypeRole).value<ParamType>();
    if (type == ParamType::Filterjob || type == ParamType::Readonly || type == ParamType::Hidden || type == ParamType::Unknown ||
        type == ParamType::EffectButtons) {
        reason = QStringLiteral("This parameter requires its native action or analysis job rather than a value assignment: ") + name;
        return false;
    }
    if (type == ParamType::Roto_spline) return McpRoto::normalizeProperty(value, converted, reason);
    converted = scalar(value);
    if (converted.size() > 65536 || converted.contains(QChar::Null) || value.isNull() || value.isUndefined()) {
        reason = QStringLiteral("Invalid or oversized parameter value.");
        return false;
    }
    if ((type == ParamType::Double || type == ParamType::Position || type == ParamType::KeyframeParam) && !converted.contains(QLatin1Char('='))) {
        bool ok = false;
        const double number = converted.toDouble(&ok);
        bool hasMin = false, hasMax = false;
        const double min = model->data(ix, AssetParameterModel::MinRole).toDouble(&hasMin);
        const double max = model->data(ix, AssetParameterModel::MaxRole).toDouble(&hasMax);
        if (!ok || !std::isfinite(number) || (hasMin && number < min) || (hasMax && number > max)) {
            reason = QStringLiteral("Numeric parameter is outside native limits: ") + name;
            return false;
        }
    }
    if ((type == ParamType::List || type == ParamType::ListWithDependency) &&
        !model->data(ix, AssetParameterModel::ListValuesRole).toStringList().contains(converted)) {
        reason = QStringLiteral("Use one of the native list choices: ") + name;
        return false;
    }
    if (type == ParamType::Url || type == ParamType::UrlList) {
        const auto choices = model->data(ix, AssetParameterModel::ListValuesRole).toStringList();
        if (!converted.isEmpty() && !choices.contains(converted)) {
            const QFileInfo file(converted);
            if (!file.isAbsolute() || !file.isFile() || file.isSymLink()) {
                reason = QStringLiteral("Use a native choice or an existing absolute local file: ") + name;
                return false;
            }
        }
    }
    return true;
}
bool applyParameters(const std::shared_ptr<AssetParameterModel> &model, const QJsonObject &object, Fun &undo, Fun &redo, QString &reason)
{
    if (object.isEmpty() || object.size() > 128) {
        reason = QStringLiteral("Use 1-128 named native parameters.");
        return false;
    }
    paramVector before, after;
    for (auto i = object.begin(); i != object.end(); ++i) {
        QString value;
        if (!validateParameter(model, i.key(), i.value(), value, reason)) return false;
        before.append({i.key(), model->getParamFromName(i.key())});
        // Animated constants need an explicit native keyframe to survive native paste/save serialization.
        const auto type = model->data(model->getParamIndexFromName(i.key()), AssetParameterModel::TypeRole).value<ParamType>();
        if (AssetParameterModel::isAnimated(type) && !value.contains(QLatin1Char('=')) && type != ParamType::Roto_spline)
            value = QString::number(pCore->getItemIn(model->getOwnerId())) + QLatin1Char('=') + value;
        after.append({i.key(), value});
    }
    const auto set = [model](const paramVector &values) {
        model->setParameters(values);
        return true;
    };
    Fun reverse = [set, before] { return set(before); };
    Fun operation = [set, after] { return set(after); };
    if (!operation()) return false;
    const auto previousUndo = undo, previousRedo = redo;
    undo = [reverse, previousUndo] { return reverse() && previousUndo(); };
    redo = [operation, previousRedo] { return previousRedo() && operation(); };
    return true;
}
// Validate complete files before entering the permissive native parser. Its time
// constructor asserts on malformed timestamps and its import loop skips bad cues.
bool subtitleTimestamp(const QString &value, const QString &format, double fps, double multiplier, GenTime &result)
{
    const QString fraction = format == QLatin1String("ass") ? QStringLiteral("[0-9]{2}") : QStringLiteral("[0-9]{3}");
    const QString separator = format == QLatin1String("srt") ? QStringLiteral(",") : QStringLiteral("\\.");
    const QRegularExpression grammar(QStringLiteral("^(?:([0-9]{1,3}):)?([0-5][0-9]):([0-5][0-9])") + separator + QStringLiteral("(") + fraction +
                                     QStringLiteral(")$"));
    const auto match = grammar.match(value);
    if (!match.hasMatch() || (format != QLatin1String("vtt") && match.captured(1).isEmpty())) return false;
    const qint64 hours = match.captured(1).toLongLong();
    // Native stringtoTime multiplies the hour component in a signed int.
    if (hours > 596) return false;
    const qint64 milliseconds = ((hours * 60 + match.captured(2).toLongLong()) * 60 + match.captured(3).toLongLong()) * 1000 +
                                match.captured(4).toLongLong() * (format == QLatin1String("ass") ? 10 : 1);
    const double frame = double(milliseconds) * fps / (1000.0 * multiplier);
    const double outputMilliseconds = double(milliseconds) / multiplier;
    if (!std::isfinite(frame) || frame < 0 || frame > 2.0 * MaxFrame + 1 || outputMilliseconds >= std::numeric_limits<int>::max()) return false;
    result = SubtitleEvent::stringtoTime(value, fps, multiplier);
    return true;
}

bool subtitleFilePreflight(const QString &contents, const QString &format, const std::shared_ptr<SubtitleModel> &model, double fps, double sourceFps,
                           int offset, int &candidateCount, QString &code, QString &reason)
{
    code = QStringLiteral("INVALID_SUBTITLE_FILE");
    reason = QStringLiteral("Malformed or unsupported native subtitle file record.");
    QSet<QPair<int, int>> starts;
    // importSubtitle accepts floats and divides them before promoting to double.
    const double multiplier = float(fps) / float(sourceFps);
    const auto candidate = [&](int layer, const GenTime &start, const GenTime &end) {
        const qint64 first = qint64(start.frames(fps)) + offset, last = qint64(end.frames(fps)) + offset;
        if (layer < 0 || layer > 50 || first < 0 || last <= first || last > MaxFrame || GenTime(int(last), fps).ms() >= std::numeric_limits<int>::max()) {
            reason = QStringLiteral("Subtitle ranges must remain inside the native frame and layer limits after conversion and offset.");
            return false;
        }
        const QPair<int, int> key(layer, int(first));
        if (starts.contains(key) || (model && model->getIdForStartPos(layer, GenTime(int(first), fps)) >= 0)) {
            code = QStringLiteral("SUBTITLE_CONFLICT");
            reason = QStringLiteral("Imported subtitle starts collide with another event in the same layer.");
            return false;
        }
        starts.insert(key);
        if (++candidateCount > 10000) {
            reason = QStringLiteral("Import at most 10000 subtitle events.");
            return false;
        }
        return true;
    };
    const auto lines = contents.split(QLatin1Char('\n'));
    for (const auto &line : lines) {
        if (line.size() > 65536) {
            reason = QStringLiteral("Subtitle records must not exceed 65536 characters.");
            return false;
        }
    }
    if (format != QLatin1String("ass")) {
        QStringList block;
        bool vttHeader = format != QLatin1String("vtt");
        const auto cue = [&]() {
            if (block.isEmpty()) return true;
            if (!vttHeader) {
                vttHeader = true;
                const bool valid = block.size() == 1 && (block.first() == QLatin1String("WEBVTT") || block.first().startsWith(QStringLiteral("WEBVTT ")));
                block.clear();
                if (!valid) reason = QStringLiteral("Native WebVTT requires a WEBVTT header followed by a blank line.");
                return valid;
            }
            const int timingIndex = format == QLatin1String("srt") ? 1 : 0;
            if (block.size() < timingIndex + 2 || (timingIndex == 1 && !QRegularExpression(QStringLiteral("^[0-9]{1,10}$")).match(block.first()).hasMatch())) {
                reason =
                    QStringLiteral("Each subtitle cue needs its native index/timing line and nonempty text; cue identifiers and metadata are unsupported.");
                return false;
            }
            const auto times = block.at(timingIndex).split(format == QLatin1String("sbv") ? QStringLiteral(",") : QStringLiteral(" --> "));
            GenTime start, end;
            if (times.size() != 2 || !subtitleTimestamp(times.first(), format, fps, multiplier, start) ||
                !subtitleTimestamp(times.last(), format, fps, multiplier, end)) {
                reason = QStringLiteral("Malformed subtitle timestamps or unsupported WebVTT cue settings.");
                return false;
            }
            block.clear();
            return candidate(0, start, end);
        };
        for (const auto &line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) {
                if (!cue()) return false;
            } else {
                block.append(trimmed);
            }
        }
        if (!cue() || !vttHeader) return false;
    } else {
        QString section;
        bool turn = false;
        const QString styleFormat =
            QStringLiteral("name,fontname,fontsize,primarycolour,secondarycolour,outlinecolour,backcolour,bold,italic,underline,"
                           "strikeout,scalex,scaley,spacing,angle,borderstyle,outline,shadow,alignment,marginl,marginr,marginv,encoding");
        const QString eventFormat = QStringLiteral("layer,start,end,style,name,marginl,marginr,marginv,effect,text");
        for (const auto &lineValue : lines) {
            const QString line = lineValue.simplified();
            if (line.isEmpty()) {
                turn = false;
                continue;
            }
            QString compact = line;
            compact.remove(QLatin1Char(' '));
            const bool sectionHeader =
                line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')) &&
                (compact.contains(QStringLiteral("ScriptInfo")) || compact.contains(QStringLiteral("KdenliveExtradata")) ||
                 line.contains(QStringLiteral("Styles")) || line.contains(QStringLiteral("Fonts")) || line.contains(QStringLiteral("Events")));
            if (turn && sectionHeader) {
                reason = QStringLiteral("Native ASS sections must be separated by a blank line.");
                return false;
            }
            if (!turn) {
                if (compact.contains(QStringLiteral("ScriptInfo")))
                    section = QStringLiteral("script");
                else if (compact.contains(QStringLiteral("KdenliveExtradata")))
                    section = QStringLiteral("extra");
                else if (line.contains(QStringLiteral("Styles")))
                    section = QStringLiteral("styles");
                else if (line.contains(QStringLiteral("Fonts")))
                    section = QStringLiteral("fonts");
                else if (line.contains(QStringLiteral("Events")))
                    section = QStringLiteral("events");
                else
                    section.clear();
                if (!section.isEmpty()) {
                    turn = true;
                    continue;
                }
            }
            if (section == QLatin1String("styles")) {
                if (line.startsWith(QStringLiteral("Format:"))) {
                    QString columns = line.section(QLatin1Char(':'), 1).toLower();
                    columns.remove(QLatin1Char(' '));
                    if (columns != styleFormat) {
                        reason = QStringLiteral("Native ASS imports require the standard V4+ style column order.");
                        return false;
                    }
                } else {
                    if (!validAssImportStyle(line)) return false;
                }
            } else if (section == QLatin1String("script")) {
                // Native ASS import stages script and embedded-font metadata
                // and restores it together with the events on undo.
            } else if (section == QLatin1String("fonts")) {
            } else if (section == QLatin1String("extra")) {
                const auto fields = line.split(QLatin1Char(':'));
                if (fields.size() != 2) return false;
                if (fields.first().trimmed() == QLatin1String("MaxLayer")) {
                    bool valid = false;
                    const int layer = fields.last().trimmed().toInt(&valid);
                    if (!valid || layer < 0 || layer > 50) return false;
                } else if (fields.first().trimmed() == QLatin1String("DefaultStyles")) {
                    if (fields.last().split(QLatin1Char(',')).size() > 51) return false;
                }
            } else if (section == QLatin1String("events")) {
                if (line.startsWith(QStringLiteral("Format:"))) {
                    QString columns = line.section(QLatin1Char(':'), 1).toLower();
                    columns.remove(QLatin1Char(' '));
                    if (columns != eventFormat && columns != QString(eventFormat).replace(QStringLiteral(",name,"), QStringLiteral(",actor,"))) {
                        reason = QStringLiteral("Native ASS imports require the standard event column order.");
                        return false;
                    }
                    continue;
                }
                if ((!line.startsWith(QStringLiteral("Dialogue:")) && !line.startsWith(QStringLiteral("Comment:"))) || line.contains(QStringLiteral("Format:")))
                    return false;
                const auto fields = line.section(QLatin1Char(':'), 1).split(QLatin1Char(','));
                if (fields.size() < 10) return false;
                bool valid = false;
                const int layer = fields.first().trimmed().toInt(&valid);
                GenTime start, end;
                if (!valid || !subtitleTimestamp(fields.at(1), format, fps, multiplier, start) ||
                    !subtitleTimestamp(fields.at(2), format, fps, multiplier, end))
                    return false;
                for (int index = 5; index <= 7; ++index) {
                    fields.at(index).toInt(&valid);
                    if (!valid) return false;
                }
                if (!candidate(layer, start, end)) return false;
            }
            // Native turn advances even for a line in an unknown section. A
            // later header is recognized only after an actual blank separator.
            turn = true;
        }
    }
    if (candidateCount == 0) {
        reason = QStringLiteral("Import 1-10000 complete native subtitle events.");
        return false;
    }
    return true;
}

QJsonObject subtitleReadback(const std::shared_ptr<SubtitleModel> &model)
{
    QJsonArray entries, styles, languages;
    if (!model) return {{QStringLiteral("subtitles"), entries}, {QStringLiteral("styles"), styles}};
    auto ids = model->getAllSubIds();
    std::vector<int> ordered(ids.begin(), ids.end());
    std::sort(ordered.begin(), ordered.end());
    for (int id : ordered) {
        const int layer = model->getLayerForId(id);
        const auto start = model->getStartPosForId(id);
        const auto event = model->getSubtitle(layer, start);
        entries.append(QJsonObject{{QStringLiteral("id"), id},
                                   {QStringLiteral("layer"), layer},
                                   {QStringLiteral("startFrame"), start.frames(pCore->getCurrentFps())},
                                   {QStringLiteral("endFrame"), event.endTime().frames(pCore->getCurrentFps())},
                                   {QStringLiteral("text"), event.text()},
                                   {QStringLiteral("style"), event.styleName()},
                                   {QStringLiteral("name"), event.name()},
                                   {QStringLiteral("marginL"), event.marginL()},
                                   {QStringLiteral("marginR"), event.marginR()},
                                   {QStringLiteral("marginV"), event.marginV()},
                                   {QStringLiteral("effects"), event.effect()},
                                   {QStringLiteral("dialogue"), event.isDialogue()}});
    }
    for (const auto &entry : model->getAllSubtitleStyles())
        styles.append(QJsonObject{{QStringLiteral("name"), entry.first}, {QStringLiteral("assStyle"), entry.second.toString(entry.first)}});
    const auto languageFiles = model->getSubtitlesList();
    for (auto i = languageFiles.begin(); i != languageFiles.end(); ++i)
        languages.append(QJsonObject{{QStringLiteral("id"), i.key().first},
                                     {QStringLiteral("name"), i.key().second},
                                     {QStringLiteral("path"), i.value()},
                                     {QStringLiteral("workPath"), pCore->currentDoc()->subTitlePath(model->timeline()->uuid(), i.key().first, false)},
                                     {QStringLiteral("savedPath"), pCore->currentDoc()->subTitlePath(model->timeline()->uuid(), i.key().first, true)}});
    return {{QStringLiteral("subtitles"), entries},
            {QStringLiteral("styles"), styles},
            {QStringLiteral("languageTracks"), languages},
            {QStringLiteral("activeLanguageId"),
             pCore->currentDoc()->getSequenceProperty(model->timeline()->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt()},
            {QStringLiteral("locked"), model->isLocked()},
            {QStringLiteral("disabled"), model->isDisabled()},
            {QStringLiteral("maxLayer"), model->getMaxLayer()}};
}
bool analysisEffectPresent(const std::shared_ptr<AssetParameterModel> &model)
{
    const auto effect = std::dynamic_pointer_cast<EffectItemModel>(model);
    if (!effect || !pCore || !pCore->currentDoc()) return false;
    const auto owner = effect->getOwnerId();
    std::shared_ptr<EffectStackModel> stack;
    if (owner.type == KdenliveObjectType::BinClip) {
        const QString id = QString::number(owner.itemId);
        const auto bin = pCore->projectItemModel();
        if (!bin || !bin->hasClip(id)) return false;
        const auto source = bin->getClipByBinID(id);
        if (!source || !source->statusReady()) return false;
        stack = source->getEffectStack();
    } else {
        const auto timeline = pCore->currentDoc()->getTimeline(owner.uuid, true);
        if (!timeline) return false;
        if (owner.type == KdenliveObjectType::TimelineClip && timeline->isClip(owner.itemId))
            stack = timeline->getClipEffectStackModel(owner.itemId);
        else if (owner.type == KdenliveObjectType::TimelineTrack && timeline->isTrack(owner.itemId))
            stack = timeline->getTrackEffectStackModel(owner.itemId);
        else if (owner.type == KdenliveObjectType::Master)
            stack = timeline->getMasterEffectStackModel();
    }
    return stack && findEffect(stack, effect->getId()) == effect;
}
} // namespace

QJsonArray KdenliveMcpServer::effectToolCatalog()
{
    const auto s = [](const char *description) { return Mcp::property(QStringLiteral("string"), QString::fromLatin1(description)); };
    const auto n = [](const char *description) { return Mcp::property(QStringLiteral("integer"), QString::fromLatin1(description)); };
    const auto o = [](const char *description) { return Mcp::property(QStringLiteral("object"), QString::fromLatin1(description)); };
    const auto a = [](const char *description) { return Mcp::property(QStringLiteral("array"), QString::fromLatin1(description)); };
    const auto b = [](const char *description) { return Mcp::property(QStringLiteral("boolean"), QString::fromLatin1(description)); };
    const auto token = s("Fresh state.expectedState; required for every mutation.");
    QJsonObject target{{QStringLiteral("scope"), s("clip, track, master, or bin")},
                       {QStringLiteral("targetId"), s("Timeline item ID as decimal string, or bin ID. Omit for master.")}};
    QJsonObject edit = target;
    edit.insert(QStringLiteral("expectedState"), token);
    edit.insert(QStringLiteral("operation"), s("add, remove, move, enable, stack_enable, parameters, or zone"));
    edit.insert(QStringLiteral("effectId"), s("Installed effect catalog ID for add"));
    edit.insert(QStringLiteral("instanceId"), n("Native instance ID from get_effect_stack"));
    edit.insert(QStringLiteral("index"), n("New zero-based effect index for move"));
    edit.insert(QStringLiteral("enabled"), b("Effect/stack/zone enabled state"));
    edit.insert(QStringLiteral("parameters"), o("Named native editable parameter values; read the parameterSchema first"));
    edit.insert(QStringLiteral("inFrame"), n("Native effect source-frame zone start"));
    edit.insert(QStringLiteral("outFrame"), n("Native effect source-frame zone end, inclusive"));
    auto animation = target;
    animation.insert(QStringLiteral("expectedState"), token);
    animation.insert(QStringLiteral("instanceId"), n("Native effect instance ID"));
    animation.insert(QStringLiteral("keyframes"), a("Complete ordered keyframes {frame,values:{parameter:value},interpolation}; frames relative to target "
                                                    "visible start. All animated parameters share the same positions."));
    return {
        tool(QStringLiteral("asset_catalog"),
             QStringLiteral(
                 "Discover installed effects/transitions and their native parameter definitions. Paginated; discovery does not execute analysis jobs."),
             {{QStringLiteral("kind"), s("effect or transition")},
              {QStringLiteral("assetId"), s("Optional single catalog ID for detailed metadata")},
              {QStringLiteral("query"), s("Optional name/id substring")},
              {QStringLiteral("offset"), n("Result offset")},
              {QStringLiteral("limit"), n("Page size, 1-100")}},
             {QStringLiteral("kind")}, true),
        tool(QStringLiteral("start_effect_analysis"),
             QStringLiteral("Run the installed effect's native analysis job (for example tracking or two-pass loudness). Poll get_effect_analysis; a "
                            "successful start is not completion."),
             {{QStringLiteral("expectedState"), token},
              {QStringLiteral("scope"), s("clip, track, master, or bin")},
              {QStringLiteral("targetId"), s("Target ID string; omit for master")},
              {QStringLiteral("instanceId"), n("Effect instance ID")},
              {QStringLiteral("jobParameter"), s("Native analysis parameter name, default filterjob")}},
             {QStringLiteral("expectedState"), QStringLiteral("scope"), QStringLiteral("instanceId")}),
        tool(QStringLiteral("get_effect_analysis"),
             QStringLiteral(
                 "Read actual native analysis progress/result. completed requires the native result parameter to have been delivered and be nonempty."),
             {{QStringLiteral("jobId"), s("Analysis job ID")}}, {QStringLiteral("jobId")}, true),
        tool(QStringLiteral("cancel_effect_analysis"),
             QStringLiteral("Request cancellation of this session's active native filter-analysis job without blocking the GUI. Already applied results are "
                            "reported, not undone."),
             {{QStringLiteral("expectedState"), token}, {QStringLiteral("jobId"), s("Analysis job ID")}},
             {QStringLiteral("expectedState"), QStringLiteral("jobId")}),
        tool(QStringLiteral("subtitle_file"),
             QStringLiteral(
                 "Import a bounded local subtitle file or export the active native subtitles to a new ASS/SRT file. Import uses native subtitle "
                 "parsing and undo. Imports ASS events and script/style/font/layer metadata, SRT, plain WebVTT cues and SBV in one native undo command."),
             {{QStringLiteral("expectedState"), token},
              {QStringLiteral("operation"), s("import or export")},
              {QStringLiteral("path"), s("Absolute local subtitle path; export refuses overwrite")},
              {QStringLiteral("offsetFrames"), n("Import frame offset, default zero")},
              {QStringLiteral("sourceFps"), Mcp::property(QStringLiteral("number"), QStringLiteral("Optional source subtitle frame rate"))},
              {QStringLiteral("encoding"), s("Import codec recognized by this Qt build, default UTF-8; Unicode BOM takes precedence")}},
             {QStringLiteral("expectedState"), QStringLiteral("operation"), QStringLiteral("path")}),
        tool(QStringLiteral("subtitle_language"),
             QStringLiteral(
                 "Create, activate, rename or delete a native subtitle language/file track with native undo. Language changes preserve document history, "
                 "event identity, styles and groups across undo/redo; layer/clip edits use edit_subtitle."),
             {{QStringLiteral("expectedState"), token},
              {QStringLiteral("operation"), s("create, activate, rename or delete")},
              {QStringLiteral("trackId"), n("Native subtitle language track ID")},
              {QStringLiteral("name"), s("Language track display name")},
              {QStringLiteral("copyTrackId"), n("Optional existing language track to duplicate")}},
             {QStringLiteral("expectedState"), QStringLiteral("operation")}),
        tool(QStringLiteral("get_effect_stack"),
             QStringLiteral("Read native effect instance IDs, ordering, zones, values and editable parameter schemas for a clip, track, master or bin source."),
             target, {QStringLiteral("scope")}, true),
        tool(QStringLiteral("edit_effect"),
             QStringLiteral("Edit the actual native effect stack with undo. Parameter assignments accept only names from the native editable schema; analysis "
                            "buttons are separate jobs."),
             edit, {QStringLiteral("expectedState"), QStringLiteral("scope"), QStringLiteral("operation")}),
        tool(QStringLiteral("set_effect_keyframes"),
             QStringLiteral("Replace animated effect parameters using native interpolation including easing. Returns actual native parameter animations."),
             animation, {QStringLiteral("expectedState"), QStringLiteral("scope"), QStringLiteral("instanceId"), QStringLiteral("keyframes")}),
        tool(QStringLiteral("get_compositions"), QStringLiteral("Read native cross-track compositions and same-track mixes with their parameter schemas."), {},
             {}, true),
        tool(QStringLiteral("edit_composition"),
             QStringLiteral("Add, move, resize, delete, or set native cross-track composition parameters; undoable and exact frame ranges."),
             {{QStringLiteral("expectedState"), token},
              {QStringLiteral("operation"), s("add, move, resize, delete, or parameters")},
              {QStringLiteral("compositionId"), n("Existing native composition ID")},
              {QStringLiteral("transitionId"), s("Installed composition catalog ID")},
              {QStringLiteral("trackId"), n("Upper destination track ID")},
              {QStringLiteral("aTrackId"), n("Lower source track ID, or -1 for automatic")},
              {QStringLiteral("startFrame"), n("Timeline position")},
              {QStringLiteral("durationFrames"), n("Exact duration")},
              {QStringLiteral("right"), b("Resize right edge; default true")},
              {QStringLiteral("parameters"), o("Native named parameter values")}},
             {QStringLiteral("expectedState"), QStringLiteral("operation")}),
        tool(QStringLiteral("edit_mix"), QStringLiteral("Add, resize, remove, change transition type or update parameters of a native same-track clip mix."),
             {{QStringLiteral("expectedState"), token},
              {QStringLiteral("operation"), s("add, resize, remove, type, or parameters")},
              {QStringLiteral("firstClipId"), n("First clip for add")},
              {QStringLiteral("secondClipId"), n("Second clip, which owns the mix")},
              {QStringLiteral("transitionId"), s("Installed transition ID, default dissolve")},
              {QStringLiteral("beforeFrames"), n("Frames before edit point")},
              {QStringLiteral("afterFrames"), n("Frames after edit point")},
              {QStringLiteral("parameters"), o("Native named mix parameters")}},
             {QStringLiteral("expectedState"), QStringLiteral("operation"), QStringLiteral("secondClipId")}),
        tool(QStringLiteral("create_title"),
             QStringLiteral("Create an editable native title from structured text/rectangle/ellipse/image items and optional animated start/end viewports. "
                            "Import is asynchronous; await bin readiness."),
             {{QStringLiteral("expectedState"), token},
              {QStringLiteral("name"), s("Title name")},
              {QStringLiteral("durationFrames"), n("Title duration")},
              {QStringLiteral("items"),
               a("1-128 objects: type text/rectangle/ellipse/image; x,y,width,height,rotation,z; "
                 "text/font/fontSize/color/bold/italic/underline/alignment/outlineWidth/outlineColor; image path; shapes fill/stroke/strokeWidth")},
              {QStringLiteral("startViewport"), o("Optional x,y,width,height")},
              {QStringLiteral("endViewport"), o("Optional x,y,width,height")},
              {QStringLiteral("binId"), s("Optional existing title bin ID to replace with native undo")}},
             {QStringLiteral("expectedState"), QStringLiteral("durationFrames"), QStringLiteral("items")}),
        tool(QStringLiteral("get_subtitles"), QStringLiteral("Read native subtitle IDs, frames, text, styles, layers and state."), {}, {}, true),
        tool(QStringLiteral("edit_subtitle"), QStringLiteral("Create, update, remove subtitles and change native styles/layers/lock/disabled state with undo."),
             {{QStringLiteral("expectedState"), token},
              {QStringLiteral("operation"), s("add, update, remove, style, remove_style, layer_add, layer_remove, lock, or disable")},
              {QStringLiteral("id"), n("Existing subtitle ID")},
              {QStringLiteral("startFrame"), n("Start frame")},
              {QStringLiteral("endFrame"), n("Exclusive end frame")},
              {QStringLiteral("layer"), n("Subtitle layer")},
              {QStringLiteral("text"), s("Unicode subtitle text")},
              {QStringLiteral("style"), s("Native style name")},
              {QStringLiteral("assStyle"), s("Complete native ASS style string for style operation")},
              {QStringLiteral("enabled"), b("Desired lock or disabled state")},
              {QStringLiteral("name"), s("Speaker/event name")},
              {QStringLiteral("marginL"), n("Left margin")},
              {QStringLiteral("marginR"), n("Right margin")},
              {QStringLiteral("marginV"), n("Vertical margin")},
              {QStringLiteral("effects"), s("ASS event effect")},
              {QStringLiteral("dialogue"), b("Dialogue rather than comment")}},
             {QStringLiteral("expectedState"), QStringLiteral("operation")})};
}

QJsonObject KdenliveMcpServer::callEffectTool(const QString &name, const QJsonObject &arguments, const QJsonObject &state)
{
    if (name == QLatin1String("asset_catalog")) {
        const QString kind = arguments.value(QStringLiteral("kind")).toString();
        if (kind != QLatin1String("effect") && kind != QLatin1String("transition"))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("kind must be effect or transition."), state);
        int offset = 0, limit = 50;
        if ((arguments.contains(QStringLiteral("offset")) && !integer(arguments, QStringLiteral("offset"), 0, 100000, offset)) ||
            (arguments.contains(QStringLiteral("limit")) && !integer(arguments, QStringLiteral("limit"), 1, 100, limit)))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use a nonnegative offset and page limit 1-100."), state);
        const QString id = arguments.value(QStringLiteral("assetId")).toString();
        const QString query = arguments.value(QStringLiteral("query")).toString();
        auto names = kind == QLatin1String("effect") ? EffectsRepository::get()->getNames() : TransitionsRepository::get()->getNames();
        std::sort(names.begin(), names.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        QJsonArray page;
        int total = 0;
        for (const auto &asset : names) {
            if ((!id.isEmpty() && asset.first != id) ||
                (!query.isEmpty() && !asset.first.contains(query, Qt::CaseInsensitive) && !asset.second.contains(query, Qt::CaseInsensitive)))
                continue;
            if (total++ < offset || page.size() >= limit) continue;
            const auto xml =
                kind == QLatin1String("effect") ? EffectsRepository::get()->getXml(asset.first) : TransitionsRepository::get()->getXml(asset.first);
            const QString description = kind == QLatin1String("effect") ? EffectsRepository::get()->getDescription(asset.first)
                                                                        : TransitionsRepository::get()->getDescription(asset.first);
            QJsonObject entry{{QStringLiteral("id"), asset.first},
                              {QStringLiteral("name"), asset.second},
                              {QStringLiteral("description"), description},
                              {QStringLiteral("audio"), kind == QLatin1String("effect") ? EffectsRepository::get()->isAudioEffect(asset.first)
                                                                                        : TransitionsRepository::get()->isAudio(asset.first)}};
            if (kind == QLatin1String("effect")) {
                entry.insert(QStringLiteral("group"), EffectsRepository::get()->isGroup(asset.first));
                entry.insert(QStringLiteral("unique"), EffectsRepository::get()->isUnique(asset.first));
            } else
                entry.insert(QStringLiteral("composition"), TransitionsRepository::get()->isComposition(asset.first));
            if (!id.isEmpty()) {
                QJsonArray definitions;
                const auto params = xml.elementsByTagName(QStringLiteral("parameter"));
                for (int i = 0; i < params.size(); ++i) {
                    const auto parameter = params.at(i).toElement();
                    QJsonObject definition;
                    const auto attrs = parameter.attributes();
                    for (int j = 0; j < attrs.size(); ++j)
                        definition.insert(attrs.item(j).nodeName(), attrs.item(j).nodeValue());
                    definition.insert(QStringLiteral("label"), parameter.firstChildElement(QStringLiteral("name")).text());
                    definition.insert(QStringLiteral("comment"), parameter.firstChildElement(QStringLiteral("comment")).text());
                    definitions.append(definition);
                }
                entry.insert(QStringLiteral("parameterDefinitions"), definitions);
                entry.insert(QStringLiteral("service"), xml.attribute(QStringLiteral("tag")));
            }
            page.append(entry);
        }
        if (!id.isEmpty() && total == 0) return failure(QStringLiteral("ASSET_NOT_FOUND"), QStringLiteral("This catalog ID is not installed."), state);
        QJsonArray interpolation;
        const auto enumInfo = QMetaEnum::fromType<KeyframeType::KeyframeEnum>();
        for (int i = 0; i < enumInfo.keyCount(); ++i)
            interpolation.append(QString::fromLatin1(enumInfo.key(i)));
        return toolResult({{QStringLiteral("ok"), true},
                           {QStringLiteral("assets"), page},
                           {QStringLiteral("total"), total},
                           {QStringLiteral("offset"), offset},
                           {QStringLiteral("nextOffset"), offset + page.size() < total ? QJsonValue(offset + page.size()) : QJsonValue()},
                           {QStringLiteral("interpolationTypes"), interpolation}});
    }
    if (name == QLatin1String("get_effect_analysis") || name == QLatin1String("cancel_effect_analysis")) {
        const QString id = arguments.value(QStringLiteral("jobId")).toString();
        if (!m_analysisJobs.contains(id))
            return failure(QStringLiteral("INVALID_JOB"), QStringLiteral("Use an analysis jobId created by this MCP session."), state);
        auto job = m_analysisJobs.value(id);
        const auto model = m_analysisModels.value(id).lock();
        if (!model || !pCore->currentDoc() || job.value(QStringLiteral("documentId")).toString() != pCore->currentDoc()->uuid().toString() ||
            !analysisEffectPresent(model)) {
            job.insert(QStringLiteral("status"), QStringLiteral("obsolete"));
        } else {
            const auto owner = model->getOwnerId();
            const bool pending = pCore->taskManager.hasPendingJob(owner, AbstractTask::FILTERCLIPJOB);
            const auto ix = model->getParamIndexFromName(job.value(QStringLiteral("jobParameter")).toString());
            const int progress = ix.isValid() ? model->data(ix, AssetParameterModel::FilterProgressRole).toInt() : 0;
            if (name == QLatin1String("cancel_effect_analysis") && pending) {
                const bool accepted = pCore->taskManager.requestCancellation(owner, AbstractTask::FILTERCLIPJOB);
                job.insert(QStringLiteral("cancelRequested"), accepted);
                if (accepted) job.insert(QStringLiteral("status"), QStringLiteral("cancelling"));
            } else if (!job.value(QStringLiteral("nativeFinished")).toBool())
                job.insert(QStringLiteral("status"), job.value(QStringLiteral("cancelRequested")).toBool() ? QStringLiteral("cancelling")
                                                     : pending                                             ? QStringLiteral("running")
                                                                                                           : QStringLiteral("finishing"));
            job.insert(QStringLiteral("progress"), progress);
            job.insert(QStringLiteral("result"), model->getParam(job.value(QStringLiteral("resultKey")).toString()));
            job.insert(QStringLiteral("effect"), parameterReadback(model));
        }
        m_analysisJobs[id] = job;
        if (name == QLatin1String("cancel_effect_analysis")) ++m_revision;
        return toolResult({{QStringLiteral("ok"), true}, {QStringLiteral("job"), job}, {QStringLiteral("state"), snapshot()}});
    }
    if (name != QLatin1String("get_effect_stack") && name != QLatin1String("edit_effect") && name != QLatin1String("set_effect_keyframes") &&
        name != QLatin1String("get_compositions") && name != QLatin1String("edit_composition") && name != QLatin1String("edit_mix") &&
        name != QLatin1String("create_title") && name != QLatin1String("get_subtitles") && name != QLatin1String("edit_subtitle") &&
        name != QLatin1String("start_effect_analysis") && name != QLatin1String("subtitle_file") && name != QLatin1String("subtitle_language"))
        return {};
    const auto timeline = m_timeline.lock();
    const auto bin = pCore->projectItemModel();
    if (!timeline || !m_document || !bin)
        return failure(QStringLiteral("NOT_READY"), QStringLiteral("Open a native project and inspect its state first."), state);
    auto finish = [this](QJsonObject data = {}) {
        ++m_revision;
        data.insert(QStringLiteral("ok"), true);
        data.insert(QStringLiteral("state"), snapshot());
        return toolResult(data);
    };
    auto failedTransaction = [this, &state](Fun &undo, const QString &message) {
        if (!undo())
            return failure(QStringLiteral("ROLLBACK_FAILED"), message + QStringLiteral(" Native rollback also failed; inspect state before further editing."),
                           snapshot());
        return failure(QStringLiteral("EDIT_FAILED"), message, snapshot());
    };
    if (name == QLatin1String("get_effect_stack") || name == QLatin1String("edit_effect") || name == QLatin1String("set_effect_keyframes") ||
        name == QLatin1String("start_effect_analysis")) {
        const QString scope = arguments.value(QStringLiteral("scope")).toString();
        const QString id = arguments.value(QStringLiteral("targetId")).toString();
        int numericId = -1;
        bool numberOk = false;
        numericId = id.toInt(&numberOk);
        std::shared_ptr<EffectStackModel> stack;
        if (scope == QLatin1String("clip") && numberOk && timeline->isClip(numericId)) {
            if (name != QLatin1String("get_effect_stack") && timeline->trackIsLocked(timeline->getClipTrackId(numericId)))
                return failure(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Unlock the clip's track before editing effects."), state);
            stack = timeline->getClipEffectStackModel(numericId);
        } else if (scope == QLatin1String("track") && numberOk && timeline->isTrack(numericId)) {
            if (name != QLatin1String("get_effect_stack") && timeline->trackIsLocked(numericId))
                return failure(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Unlock the track before editing effects."), state);
            stack = timeline->getTrackEffectStackModel(numericId);
        } else if (scope == QLatin1String("master") && id.isEmpty())
            stack = timeline->getMasterEffectStackModel();
        else if (scope == QLatin1String("bin") && bin->hasClip(id)) {
            const auto clip = bin->getClipByBinID(id);
            if (!clip->statusReady()) return failure(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Wait for this source to load."), state);
            stack = clip->getEffectStack();
        }
        if (!stack)
            return failure(QStringLiteral("INVALID_TARGET"), QStringLiteral("Use clip/track/master/bin and a valid targetId string; omit targetId for master."),
                           state);
        if (name == QLatin1String("get_effect_stack"))
            return toolResult({{QStringLiteral("ok"), true}, {QStringLiteral("stack"), stackReadback(stack)}, {QStringLiteral("state"), state}});
        if (stack->rowCount() >= MaxEffects) return failure(QStringLiteral("LIMIT"), QStringLiteral("Effect stack size limit reached."), state);
        int instanceId = -1;
        const auto operation = arguments.value(QStringLiteral("operation")).toString();
        const bool needsEffect =
            name == QLatin1String("set_effect_keyframes") || (operation != QLatin1String("add") && operation != QLatin1String("stack_enable"));
        std::shared_ptr<AbstractEffectItem> item;
        std::shared_ptr<EffectItemModel> effect;
        if (needsEffect) {
            if (!integer(arguments, QStringLiteral("instanceId"), 0, MaxFrame, instanceId) || !(item = findEffect(stack, instanceId)))
                return failure(QStringLiteral("INVALID_EFFECT"), QStringLiteral("Use a current native instanceId from get_effect_stack."), state);
            effect = std::dynamic_pointer_cast<EffectItemModel>(item);
        }
        if (name == QLatin1String("start_effect_analysis")) {
            if (!effect) return failure(QStringLiteral("INVALID_EFFECT"), QStringLiteral("Choose an actual effect instance."), state);
            const QString jobParameter = arguments.value(QStringLiteral("jobParameter")).toString(QStringLiteral("filterjob"));
            const auto ix = effect->getParamIndexFromName(jobParameter);
            if (!ix.isValid() || effect->data(ix, AssetParameterModel::TypeRole).value<ParamType>() != ParamType::Filterjob)
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("This effect has no such native analysis button."), state);
            const auto owner = effect->getOwnerId();
            if (pCore->taskManager.hasPendingJob(owner, AbstractTask::FILTERCLIPJOB) || pCore->taskManager.isBlocked())
                return failure(QStringLiteral("JOB_BUSY"), QStringLiteral("Wait for existing native filter processing or project loading to finish."), state);
            if (m_analysisJobs.size() >= 512) return failure(QStringLiteral("LIMIT"), QStringLiteral("This session's analysis job history is full."), state);
            std::unordered_map<QString, QString> jobData;
            for (const auto &entry : effect->data(ix, AssetParameterModel::FilterJobParamsRole).toList()) {
                const auto pair = entry.toStringList();
                if (pair.size() == 2) jobData[pair.at(0)] = pair.at(1);
            }
            const QString key = jobData.count(QStringLiteral("key")) ? jobData.at(QStringLiteral("key")) : QStringLiteral("results");
            std::unordered_map<QString, QVariant> filterParams;
            const QString service = effect->getParam(QStringLiteral("mlt_service"));
            if (service.isEmpty()) return failure(QStringLiteral("EFFECT_UNAVAILABLE"), QStringLiteral("The native effect has no MLT service."), state);
            for (const auto &param : effect->getAllParameters()) {
                if (param.first == key) {
                    if (service == QLatin1String("opencv.tracker")) {
                        QString initial = effect->getParam(param.first).section(QLatin1Char(';'), 0, 0);
                        if (initial.contains(QLatin1Char('='))) initial = initial.section(QLatin1Char('='), 1);
                        filterParams[QStringLiteral("rect")] = initial;
                    }
                } else if (service != QLatin1String("opencv.tracker") || param.first != QLatin1String("rect"))
                    filterParams[param.first] = effect->getParam(param.first);
            }
            for (const QString &param : effect->data(ix, AssetParameterModel::FilterParamsRole).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                if (param.contains(QLatin1Char('='))) filterParams[param.section(QLatin1Char('='), 0, 0)] = param.section(QLatin1Char('='), 1);
            }
            QString sourceId = QStringLiteral("-1");
            int in = 0, out = std::max(0, pCore->getItemDuration(owner) - 1);
            if (owner.type == KdenliveObjectType::BinClip) {
                sourceId = QString::number(owner.itemId);
                in = -1;
                out = -1;
            } else if (owner.type == KdenliveObjectType::TimelineClip) {
                sourceId = timeline->getClipBinId(owner.itemId);
                in = pCore->getItemIn(owner);
                out = in + pCore->getItemDuration(owner) - 1;
            }
            if (sourceId != QLatin1String("-1")) {
                const auto source = bin->hasClip(sourceId) ? bin->getClipByBinID(sourceId) : nullptr;
                if (!source || !source->statusReady() || source->url().isEmpty())
                    return failure(QStringLiteral("INVALID_SOURCE"), QStringLiteral("Native filter analysis requires a ready file-backed source."), state);
            }
            const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_analysisJobs.insert(jobId, QJsonObject{{QStringLiteral("id"), jobId},
                                                     {QStringLiteral("documentId"), m_document->uuid().toString()},
                                                     {QStringLiteral("status"), QStringLiteral("running")},
                                                     {QStringLiteral("jobParameter"), jobParameter},
                                                     {QStringLiteral("resultKey"), key},
                                                     {QStringLiteral("resultObserved"), false},
                                                     {QStringLiteral("nativeFinished"), false},
                                                     {QStringLiteral("cancelRequested"), false},
                                                     {QStringLiteral("instanceId"), effect->getId()},
                                                     {QStringLiteral("progress"), 0}});
            m_analysisModels.insert(jobId, effect);
            std::weak_ptr<AssetParameterModel> weak = effect;
            effect->setProgress(0);
            const auto consumer = effect->data(ix, AssetParameterModel::FilterConsumerParamsRole).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            auto *task = new FilterTask(owner, sourceId, effect, service, in, out, filterParams, jobData, consumer, this);
            connect(
                task, &FilterTask::analysisFinished, this,
                [this, weak, jobId](bool success, bool cancelled, const QString &resultKey, const QString &resultData) {
                    if (!m_analysisJobs.contains(jobId)) return;
                    auto &job = m_analysisJobs[jobId];
                    const auto model = weak.lock();
                    const bool sameDocument = model && pCore->currentDoc() &&
                                              job.value(QStringLiteral("documentId")).toString() == pCore->currentDoc()->uuid().toString() &&
                                              analysisEffectPresent(model);
                    const QString actual = sameDocument ? model->getParam(job.value(QStringLiteral("resultKey")).toString()) : QString();
                    const bool delivered = sameDocument && success && !resultData.isEmpty() && resultKey == job.value(QStringLiteral("resultKey")).toString() &&
                                           actual == resultData;
                    job.insert(QStringLiteral("nativeFinished"), true);
                    job.insert(QStringLiteral("resultObserved"), delivered);
                    job.insert(QStringLiteral("status"), !sameDocument ? QStringLiteral("obsolete")
                                                         : cancelled   ? QStringLiteral("cancelled")
                                                         : delivered   ? QStringLiteral("completed")
                                                                       : QStringLiteral("failed"));
                    job.insert(QStringLiteral("result"), actual);
                    if (sameDocument && success && !delivered)
                        job.insert(QStringLiteral("error"), QStringLiteral("Native analysis result did not match its delivered parameter."));
                    ++m_revision;
                },
                Qt::QueuedConnection);
            pCore->taskManager.startTask(owner.itemId, task);
            return finish({{QStringLiteral("jobId"), jobId}, {QStringLiteral("job"), m_analysisJobs.value(jobId)}});
        }
        Fun undo = [] { return true; }, redo = [] { return true; };
        QString reason;
        if (name == QLatin1String("set_effect_keyframes")) {
            if (!effect) return failure(QStringLiteral("INVALID_EFFECT"), QStringLiteral("Animate an effect rather than an effect group."), state);
            const auto animated = effect->getKeyframableParameters();
            const auto keysValue = arguments.value(QStringLiteral("keyframes"));
            const auto keys = keysValue.toArray();
            if (animated.isEmpty() || !keysValue.isArray() || keys.isEmpty() || keys.size() > 512)
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use 1-512 keys for an effect with animated parameters."), state);
            QMap<QString, QStringList> animations;
            QMap<QString, QJsonObject> rotoAnimations;
            const int sourceIn = pCore->getItemIn(effect->getOwnerId());
            const int duration = pCore->getItemDuration(effect->getOwnerId());
            int previous = -1;
            const auto types = QMetaEnum::fromType<KeyframeType::KeyframeEnum>();
            for (const auto &value : keys) {
                if (!value.isObject()) return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Each keyframe must be an object."), state);
                const auto key = value.toObject();
                int frame = -1;
                if (!integer(key, QStringLiteral("frame"), 0, std::max(0, duration - 1), frame) || frame <= previous || (previous == -1 && frame != 0) ||
                    !key.value(QStringLiteral("values")).isObject())
                    return failure(QStringLiteral("INVALID_ARGUMENT"),
                                   QStringLiteral("Use increasing clip-local frame keys starting at zero and a values object."), state);
                const auto values = key.value(QStringLiteral("values")).toObject();
                if (values.size() != animated.size())
                    return failure(QStringLiteral("INVALID_ARGUMENT"),
                                   QStringLiteral("Every key must include every native animated parameter, preserving shared native keyframe positions."),
                                   state);
                const QString interpolation = key.value(QStringLiteral("interpolation")).toString(QStringLiteral("Linear"));
                bool known = false;
                const int type = types.keyToValue(interpolation.toLatin1().constData(), &known);
                if (!known)
                    return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Unknown native interpolation type; query asset_catalog."), state);
                const QString separator = KeyframeModel::getSeparatorForKeyframeType(static_cast<mlt_keyframe_type>(type));
                for (auto i = animated.begin(); i != animated.end(); ++i) {
                    if (!values.contains(i.key()))
                        return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Missing animated parameter: ") + i.key(), state);
                    QString converted;
                    if (!validateParameter(effect, i.key(), values.value(i.key()), converted, reason))
                        return failure(QStringLiteral("INVALID_ARGUMENT"), reason, state);
                    if (i.value().first == ParamType::Roto_spline) {
                        if (type != int(mlt_keyframe_linear))
                            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Native rotoscoping supports Linear interpolation only."), state);
                        if (!McpRoto::validatePoints(values.value(i.key()), reason)) return failure(QStringLiteral("INVALID_ARGUMENT"), reason, state);
                        const qint64 absoluteFrame = qint64(sourceIn) + frame;
                        if (absoluteFrame < 0 || absoluteFrame >= std::numeric_limits<int>::max())
                            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Native rotoscoping frame is outside its integer range."), state);
                        rotoAnimations[i.key()].insert(QString::number(absoluteFrame), values.value(i.key()));
                    } else {
                        if (converted.contains(QLatin1Char('=')) || converted.contains(QLatin1Char(';')))
                            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("A key value cannot contain another animation."), state);
                        animations[i.key()].append(QString::number(sourceIn + frame) + separator + QLatin1Char('=') + converted);
                    }
                }
                previous = frame;
            }
            QJsonObject params;
            for (auto i = animations.begin(); i != animations.end(); ++i)
                params.insert(i.key(), i.value().join(QLatin1Char(';')));
            for (auto i = rotoAnimations.begin(); i != rotoAnimations.end(); ++i)
                params.insert(i.key(), QString::fromUtf8(QJsonDocument(i.value()).toJson(QJsonDocument::Compact)));
            if (!applyParameters(effect, params, undo, redo, reason)) return failure(QStringLiteral("INVALID_ARGUMENT"), reason, state);
            effect->prepareKeyframes();
        } else if (operation == QLatin1String("add")) {
            QString assetId;
            if (!text(arguments, QStringLiteral("effectId"), assetId, 256) || !EffectsRepository::get()->exists(assetId))
                return failure(QStringLiteral("ASSET_NOT_FOUND"), QStringLiteral("Choose an installed effectId from asset_catalog."), state);
            if (EffectsRepository::get()->isGroup(assetId)) {
                const auto xml = EffectsRepository::get()->getXml(assetId);
                if (!stack->copyXmlEffectWithUndo(xml, undo, redo)) return failedTransaction(undo, QStringLiteral("Native effect group insertion failed."));
            } else if (!stack->appendEffectWithUndo(assetId, undo, redo).first)
                return failedTransaction(undo, QStringLiteral("Native effect insertion was rejected for this target."));
            if (arguments.contains(QStringLiteral("parameters"))) {
                effect = std::dynamic_pointer_cast<EffectItemModel>(stack->getEffectStackRow(stack->rowCount() - 1));
                if (!effect || !arguments.value(QStringLiteral("parameters")).isObject() ||
                    !applyParameters(effect, arguments.value(QStringLiteral("parameters")).toObject(), undo, redo, reason))
                    return failedTransaction(undo, QStringLiteral("Effect parameter setup failed: ") + reason);
            }
        } else if (operation == QLatin1String("remove")) {
            if (!effect)
                return failure(QStringLiteral("INVALID_EFFECT"), QStringLiteral("Remove individual effect IDs; preserve groups containing other effects."),
                               state);
            QString effectName;
            stack->removeEffectWithUndo(effect, effectName, undo, redo);
            if (findEffect(stack, instanceId)) return failedTransaction(undo, QStringLiteral("Native effect removal failed."));
        } else if (operation == QLatin1String("move")) {
            int index = -1;
            if (!integer(arguments, QStringLiteral("index"), 0, stack->rowCount() - 1, index))
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use an existing zero-based effect destination index."), state);
            if (!stack->moveEffectWithUndo(index, item, undo, redo)) return failedTransaction(undo, QStringLiteral("Native effect reordering failed."));
        } else if (operation == QLatin1String("enable")) {
            if (!arguments.value(QStringLiteral("enabled")).isBool())
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("enabled must be boolean."), state);
            item->markEnabled(arguments.value(QStringLiteral("enabled")).toBool(), undo, redo);
        } else if (operation == QLatin1String("stack_enable")) {
            if (!arguments.value(QStringLiteral("enabled")).isBool())
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("enabled must be boolean."), state);
            const bool before = stack->isStackEnabled(), after = arguments.value(QStringLiteral("enabled")).toBool();
            undo = [stack, before] {
                stack->setEffectStackEnabled(before);
                return stack->isStackEnabled() == before;
            };
            redo = [stack, after] {
                stack->setEffectStackEnabled(after);
                return stack->isStackEnabled() == after;
            };
            if (!redo()) return failedTransaction(undo, QStringLiteral("Native stack activation failed."));
        } else if (operation == QLatin1String("parameters")) {
            if (!effect || !arguments.value(QStringLiteral("parameters")).isObject() ||
                !applyParameters(effect, arguments.value(QStringLiteral("parameters")).toObject(), undo, redo, reason))
                return failure(QStringLiteral("INVALID_ARGUMENT"), reason.isEmpty() ? QStringLiteral("Choose an effect and named parameters.") : reason, state);
        } else if (operation == QLatin1String("zone")) {
            int start = 0, end = 0;
            if (!effect || !integer(arguments, QStringLiteral("inFrame"), 0, MaxFrame, start) ||
                !integer(arguments, QStringLiteral("outFrame"), start, MaxFrame, end) || !arguments.value(QStringLiteral("enabled")).isBool())
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use an effect, ordered in/out frames and boolean enabled."), state);
            effect->setInOut(effect->getAssetId(), {start, end}, arguments.value(QStringLiteral("enabled")).toBool(), true);
            return finish({{QStringLiteral("stack"), stackReadback(stack)}});
        } else
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Unknown effect operation."), state);
        if (!stack->checkConsistency()) return failedTransaction(undo, QStringLiteral("The native effect stack failed consistency validation."));
        pCore->pushUndo(undo, redo, QStringLiteral("MCP edit native effects"));
        return finish({{QStringLiteral("stack"), stackReadback(stack)}});
    }
    const auto compositions = [&]() {
        QJsonArray result, mixes;
        const auto items = timeline->getItemsInRange(-1, 0, -1, true);
        std::vector<int> ordered(items.begin(), items.end());
        std::sort(ordered.begin(), ordered.end());
        for (int id : ordered) {
            if (timeline->isComposition(id)) {
                const auto model = timeline->getCompositionParameterModel(id);
                auto entry = parameterReadback(model);
                entry.insert(QStringLiteral("id"), id);
                entry.insert(QStringLiteral("trackId"), timeline->getCompositionTrackId(id));
                entry.insert(QStringLiteral("startFrame"), timeline->getCompositionPosition(id));
                entry.insert(QStringLiteral("durationFrames"), timeline->getCompositionPlaytime(id));
                const int lower = timeline->getCompositionPtr(id)->getATrack();
                entry.insert(QStringLiteral("aTrackId"), lower > 0 ? timeline->getTrackIndexFromPosition(lower - 1) : -1);
                result.append(entry);
            } else if (timeline->isClip(id) && timeline->getMixDuration(id) > 0) {
                const int trackId = timeline->getClipTrackId(id);
                const auto track = timeline->getTrackById(trackId);
                const auto model = track->mixModel(id);
                auto entry = parameterReadback(model);
                entry.insert(QStringLiteral("secondClipId"), id);
                entry.insert(QStringLiteral("firstClipId"), track->getMixInfo(id).first.firstClipId);
                entry.insert(QStringLiteral("trackId"), trackId);
                entry.insert(QStringLiteral("startFrame"), timeline->getClipPosition(id));
                entry.insert(QStringLiteral("durationFrames"), timeline->getMixDuration(id));
                entry.insert(QStringLiteral("afterFrames"), timeline->getMixCutPos(id));
                entry.insert(QStringLiteral("beforeFrames"), timeline->getMixDuration(id) - timeline->getMixCutPos(id));
                mixes.append(entry);
            }
        }
        return QJsonObject{{QStringLiteral("compositions"), result}, {QStringLiteral("mixes"), mixes}};
    };
    if (name == QLatin1String("get_compositions")) {
        auto data = compositions();
        data.insert(QStringLiteral("ok"), true);
        data.insert(QStringLiteral("state"), state);
        return toolResult(data);
    }
    if (name == QLatin1String("edit_composition")) {
        const QString operation = arguments.value(QStringLiteral("operation")).toString();
        int id = -1, trackId = -1, lower = -1, position = 0, duration = 0;
        if (operation != QLatin1String("add")) {
            if (!integer(arguments, QStringLiteral("compositionId"), 0, MaxFrame, id) || !timeline->isComposition(id))
                return failure(QStringLiteral("INVALID_COMPOSITION"), QStringLiteral("Use a native compositionId from get_compositions."), state);
            trackId = timeline->getCompositionTrackId(id);
            if (timeline->isInGroup(id))
                return failure(QStringLiteral("GROUPED_ITEM"), QStringLiteral("Ungroup the composition before changing its individual geometry."), state);
            if (timeline->trackIsLocked(trackId)) return failure(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Unlock the composition track."), state);
        }
        Fun undo = [] { return true; }, redo = [] { return true; };
        QString reason;
        if (operation == QLatin1String("add") || operation == QLatin1String("move")) {
            if (!integer(arguments, QStringLiteral("trackId"), 0, MaxFrame, trackId) || !timeline->isTrack(trackId) || timeline->isAudioTrack(trackId) ||
                timeline->trackIsLocked(trackId) || !integer(arguments, QStringLiteral("startFrame"), 0, MaxFrame, position) ||
                (arguments.contains(QStringLiteral("aTrackId")) && !integer(arguments, QStringLiteral("aTrackId"), -1, MaxFrame, lower)))
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use an unlocked video track, startFrame and optional lower aTrackId."),
                               state);
            if (lower >= 0 &&
                (!timeline->isTrack(lower) || timeline->isAudioTrack(lower) || timeline->getTrackPosition(lower) >= timeline->getTrackPosition(trackId)))
                return failure(QStringLiteral("INVALID_TRACK"), QStringLiteral("aTrackId must be a video track below the destination."), state);
            const int compositionTrack = lower < 0 ? -1 : timeline->getTrackMltIndex(lower);
            if (operation == QLatin1String("add")) {
                QString transitionId;
                if (!text(arguments, QStringLiteral("transitionId"), transitionId, 256) || !TransitionsRepository::get()->exists(transitionId) ||
                    !TransitionsRepository::get()->isComposition(transitionId) || !integer(arguments, QStringLiteral("durationFrames"), 1, 1000000, duration) ||
                    qint64(position) + duration > MaxFrame)
                    return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Choose an installed composition transition and bounded duration."),
                                   state);
                if (!timeline->requestCompositionInsertion(transitionId, trackId, compositionTrack, position, duration, nullptr, id, undo, redo, true))
                    return failedTransaction(undo, QStringLiteral("Native composition insertion failed."));
            } else if (!timeline->requestCompositionMove(id, trackId, compositionTrack, position, true, true, undo, redo))
                return failedTransaction(undo, QStringLiteral("Native composition move failed."));
            if (timeline->getCompositionPosition(id) != position || timeline->getCompositionTrackId(id) != trackId ||
                (duration > 0 && timeline->getCompositionPlaytime(id) != duration))
                return failedTransaction(undo, QStringLiteral("Native composition geometry differed from the request."));
        } else if (operation == QLatin1String("resize")) {
            if (!integer(arguments, QStringLiteral("durationFrames"), 1, 1000000, duration) ||
                (arguments.contains(QStringLiteral("right")) && !arguments.value(QStringLiteral("right")).isBool()))
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use durationFrames and optional boolean right."), state);
            int requested = duration;
            if (!timeline->requestItemResize(id, requested, arguments.value(QStringLiteral("right")).toBool(true), true, undo, redo) ||
                timeline->getCompositionPlaytime(id) != duration)
                return failedTransaction(undo, QStringLiteral("Native composition resize could not apply the exact duration."));
        } else if (operation == QLatin1String("delete")) {
            if (!timeline->requestItemDeletion(id, undo, redo, true)) return failedTransaction(undo, QStringLiteral("Native composition deletion failed."));
        } else if (operation != QLatin1String("parameters"))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Unknown composition operation."), state);
        if (operation == QLatin1String("parameters") || arguments.contains(QStringLiteral("parameters"))) {
            if (operation == QLatin1String("delete") || !arguments.value(QStringLiteral("parameters")).isObject() ||
                !applyParameters(timeline->getCompositionParameterModel(id), arguments.value(QStringLiteral("parameters")).toObject(), undo, redo, reason))
                return failedTransaction(undo, QStringLiteral("Composition parameters failed: ") + reason);
        }
        if (!timelineConsistent(timeline)) return failedTransaction(undo, QStringLiteral("Native composition consistency check failed."));
        pCore->pushUndo(undo, redo, QStringLiteral("MCP edit composition"));
        auto data = compositions();
        data.insert(QStringLiteral("compositionId"), id);
        return finish(data);
    }
    if (name == QLatin1String("edit_mix")) {
        const QString operation = arguments.value(QStringLiteral("operation")).toString();
        int second = -1, first = -1, before = 0, after = 0;
        if (!integer(arguments, QStringLiteral("secondClipId"), 0, MaxFrame, second) || !timeline->isClip(second))
            return failure(QStringLiteral("INVALID_CLIP"), QStringLiteral("Use a valid secondClipId."), state);
        const int trackId = timeline->getClipTrackId(second);
        if (trackId < 0 || timeline->trackIsLocked(trackId))
            return failure(QStringLiteral("TRACK_LOCKED"), QStringLiteral("Use an unlocked timeline clip."), state);
        const auto track = timeline->getTrackById(trackId);
        const bool existing = timeline->getMixDuration(second) > 0;
        if (operation != QLatin1String("add") && !existing)
            return failure(QStringLiteral("INVALID_MIX"), QStringLiteral("The second clip has no start mix."), state);
        QString transitionId =
            arguments.value(QStringLiteral("transitionId")).toString(timeline->isAudioTrack(trackId) ? QStringLiteral("mix") : QStringLiteral("dissolve"));
        if ((operation == QLatin1String("add") || operation == QLatin1String("type")) &&
            (!TransitionsRepository::get()->exists(transitionId) || TransitionsRepository::get()->isComposition(transitionId)))
            return failure(QStringLiteral("ASSET_NOT_FOUND"), QStringLiteral("Choose an installed same-track transition ID."), state);
        Fun undo = [] { return true; }, redo = [] { return true; };
        QString reason;
        if (operation == QLatin1String("add") || operation == QLatin1String("resize")) {
            if (!integer(arguments, QStringLiteral("beforeFrames"), 0, 1000000, before) ||
                !integer(arguments, QStringLiteral("afterFrames"), 0, 1000000, after) || before + after == 0)
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use nonnegative beforeFrames/afterFrames with positive total duration."),
                               state);
            if (operation == QLatin1String("add")) {
                if (existing || !integer(arguments, QStringLiteral("firstClipId"), 0, MaxFrame, first) || !timeline->isClip(first) || first == second ||
                    timeline->getClipTrackId(first) != trackId ||
                    timeline->getClipPosition(first) + timeline->getClipPlaytime(first) != timeline->getClipPosition(second) || timeline->hasClipEndMix(first))
                    return failure(QStringLiteral("INVALID_MIX"),
                                   QStringLiteral("Add a mix between adjacent clips on the same track without an existing mix at their edit point."), state);
                if (!timeline->requestClipMix(transitionId, {first, second}, {after, before}, trackId, timeline->getClipPosition(second), true, true, true,
                                              undo, redo, false))
                    return failedTransaction(undo, QStringLiteral("Native mix insertion failed; check source handles."));
            } else if (!track->requestResizeMix(second, {before, after}, undo, redo))
                return failedTransaction(undo, QStringLiteral("Native mix resize failed; check source handles."));
            if (timeline->getMixDuration(second) != before + after || timeline->getMixCutPos(second) != after)
                return failedTransaction(undo, QStringLiteral("Native mix limits prevented the exact requested duration."));
        } else if (operation == QLatin1String("remove")) {
            if (!timeline->removeMixWithUndo(second, undo, redo)) return failedTransaction(undo, QStringLiteral("Native mix removal failed."));
        } else if (operation == QLatin1String("type")) {
            track->switchMix(second, transitionId, undo, redo);
            if (track->mixModel(second)->getAssetId() != transitionId)
                return failedTransaction(undo, QStringLiteral("Native mix type did not match the request."));
        } else if (operation != QLatin1String("parameters"))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Unknown mix operation."), state);
        if (operation == QLatin1String("parameters") || arguments.contains(QStringLiteral("parameters"))) {
            if (operation == QLatin1String("remove") || !arguments.value(QStringLiteral("parameters")).isObject() ||
                !applyParameters(track->mixModel(second), arguments.value(QStringLiteral("parameters")).toObject(), undo, redo, reason))
                return failedTransaction(undo, QStringLiteral("Mix parameters failed: ") + reason);
        }
        if (!timelineConsistent(timeline)) return failedTransaction(undo, QStringLiteral("Native mix consistency check failed."));
        pCore->pushUndo(undo, redo, QStringLiteral("MCP edit clip mix"));
        return finish(compositions());
    }
    if (name == QLatin1String("create_title")) {
        int duration = 0;
        const auto values = arguments.value(QStringLiteral("items"));
        const auto items = values.toArray();
        if (!integer(arguments, QStringLiteral("durationFrames"), 1, 1000000, duration) || !values.isArray() || items.isEmpty() || items.size() > 128)
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use a duration and 1-128 structured title items."), state);
        const QSize size = pCore->getCurrentFrameSize();
        std::vector<std::unique_ptr<QGraphicsItem>> owned;
        QList<QGraphicsItem *> sceneItems;
        QString reason;
        for (const auto &value : items) {
            if (!value.isObject()) return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Each title item must be an object."), state);
            const auto item = value.toObject();
            const QString type = item.value(QStringLiteral("type")).toString();
            double x = 0, y = 0, width = size.width(), height = size.height(), rotation = 0, z = owned.size();
            const auto number = [&item](const QString &key, double min, double max, double &result) {
                return !item.contains(key) || real(item, key, min, max, result);
            };
            if (!number(QStringLiteral("x"), -4 * size.width(), 4 * size.width(), x) ||
                !number(QStringLiteral("y"), -4 * size.height(), 4 * size.height(), y) || !number(QStringLiteral("width"), 1, 4 * size.width(), width) ||
                !number(QStringLiteral("height"), 1, 4 * size.height(), height) || !number(QStringLiteral("rotation"), -3600, 3600, rotation) ||
                !number(QStringLiteral("z"), -999, 999, z))
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Title geometry must be finite and within four project dimensions."), state);
            std::unique_ptr<QGraphicsItem> native;
            if (type == QLatin1String("text")) {
                QString content;
                double fontSize = 64, outline = 0, spacing = 0;
                if (!text(item, QStringLiteral("text"), content, 32768) || !number(QStringLiteral("fontSize"), 1, 2000, fontSize) ||
                    !number(QStringLiteral("outlineWidth"), 0, 100, outline) || !number(QStringLiteral("letterSpacing"), -100, 1000, spacing))
                    return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use title text and bounded font/outline sizes."), state);
                auto textItem = std::make_unique<MyTextItem>(content);
                // Persist a real installed family, as the native font selector
                // does. Generic aliases render but trigger missing-font repair
                // when Kdenlive later checks the saved title.
                const QString defaultFamily = QFontInfo(QFontDatabase::systemFont(QFontDatabase::GeneralFont)).family();
                QFont font(item.value(QStringLiteral("font")).toString(defaultFamily));
                font.setPixelSize(qRound(fontSize));
                font.setBold(item.value(QStringLiteral("bold")).toBool());
                font.setItalic(item.value(QStringLiteral("italic")).toBool());
                font.setUnderline(item.value(QStringLiteral("underline")).toBool());
                font.setStrikeOut(item.value(QStringLiteral("strikeOut")).toBool());
                font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
                textItem->setFont(font);
                textItem->setTextWidth(width);
                const QColor color(item.value(QStringLiteral("color")).toString(QStringLiteral("#ffffff")));
                const QColor outlineColor(item.value(QStringLiteral("outlineColor")).toString(QStringLiteral("#000000")));
                if (!color.isValid() || !outlineColor.isValid())
                    return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use valid title colors."), state);
                textItem->setTextColor(color);
                textItem->setOutline(outline, outlineColor);
                const auto alignment = item.value(QStringLiteral("alignment")).toString(QStringLiteral("left"));
                if (alignment != QLatin1String("left") && alignment != QLatin1String("center") && alignment != QLatin1String("right") &&
                    alignment != QLatin1String("justify"))
                    return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use left/center/right/justify text alignment."), state);
                textItem->setAlignment(alignment == QLatin1String("center")    ? Qt::AlignHCenter
                                       : alignment == QLatin1String("right")   ? Qt::AlignRight
                                       : alignment == QLatin1String("justify") ? Qt::AlignJustify
                                                                               : Qt::AlignLeft);
                if (item.contains(QStringLiteral("shadow"))) {
                    const auto shadow = item.value(QStringLiteral("shadow")).toObject();
                    int blur = 0, offsetX = 0, offsetY = 0;
                    const QColor shadowColor(shadow.value(QStringLiteral("color")).toString(QStringLiteral("#000000")));
                    if (!item.value(QStringLiteral("shadow")).isObject() || !integer(shadow, QStringLiteral("blur"), 0, 100, blur) ||
                        !integer(shadow, QStringLiteral("x"), -1000, 1000, offsetX) || !integer(shadow, QStringLiteral("y"), -1000, 1000, offsetY) ||
                        !shadowColor.isValid())
                        return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Shadow requires bounded blur/x/y and color."), state);
                    textItem->updateShadow(true, blur, offsetX, offsetY, shadowColor);
                }
                if (item.contains(QStringLiteral("typewriter"))) {
                    const auto tw = item.value(QStringLiteral("typewriter")).toObject();
                    int step = 1, mode = 0, sigma = 0, seed = 0;
                    if (!item.value(QStringLiteral("typewriter")).isObject() || !integer(tw, QStringLiteral("step"), 1, 10000, step) ||
                        !integer(tw, QStringLiteral("mode"), 0, 3, mode) ||
                        (tw.contains(QStringLiteral("sigma")) && !integer(tw, QStringLiteral("sigma"), 0, 10000, sigma)) ||
                        (tw.contains(QStringLiteral("seed")) && !integer(tw, QStringLiteral("seed"), 0, MaxFrame, seed)))
                        return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Typewriter requires step/mode and optional sigma/seed."), state);
                    textItem->updateTW(true, step, mode, sigma, seed);
                }
                native = std::move(textItem);
            } else if (type == QLatin1String("rectangle") || type == QLatin1String("ellipse")) {
                const QColor fill(item.value(QStringLiteral("fill")).toString(QStringLiteral("#00000000")));
                const QColor stroke(item.value(QStringLiteral("stroke")).toString(QStringLiteral("#ffffff")));
                double strokeWidth = 0;
                if (!fill.isValid() || !stroke.isValid() || !number(QStringLiteral("strokeWidth"), 0, 200, strokeWidth))
                    return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use valid shape colors and stroke width."), state);
                QPen pen(stroke);
                pen.setWidthF(strokeWidth);
                if (strokeWidth == 0) pen.setStyle(Qt::NoPen);
                if (type == QLatin1String("rectangle")) {
                    auto shape = std::make_unique<MyRectItem>();
                    shape->setRect(QRectF(0, 0, width, height));
                    shape->setBrush(fill);
                    shape->setPen(pen);
                    int radius = 0;
                    if (item.contains(QStringLiteral("cornerRadius")) && !integer(item, QStringLiteral("cornerRadius"), 0, 10000, radius))
                        return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use a nonnegative corner radius."), state);
                    shape->setCornerRadius(radius);
                    native = std::move(shape);
                } else {
                    auto shape = std::make_unique<MyEllipseItem>();
                    shape->setRect(QRectF(0, 0, width, height));
                    shape->setBrush(fill);
                    shape->setPen(pen);
                    native = std::move(shape);
                }
            } else if (type == QLatin1String("image")) {
                QString path;
                if (!text(item, QStringLiteral("path"), path, 32768))
                    return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("An image item requires an absolute local path."), state);
                const QFileInfo file(path);
                QImageReader reader(path);
                const QSize imageSize = reader.size();
                if (!file.isAbsolute() || !file.isFile() || file.isSymLink() || file.size() > (qint64(1) << 30) || !reader.canRead() || imageSize.width() < 1 ||
                    imageSize.height() < 1 || qint64(imageSize.width()) * imageSize.height() > 100000000)
                    return failure(QStringLiteral("INVALID_PATH"), QStringLiteral("Use a bounded local image for the title."), state);
                const QPixmap bitmap(path);
                if (bitmap.isNull()) return failure(QStringLiteral("INVALID_PATH"), QStringLiteral("The image could not be decoded."), state);
                auto image = std::make_unique<MyPixmapItem>(bitmap);
                image->setData(Qt::UserRole, file.absoluteFilePath());
                image->setTransform(QTransform::fromScale(width / bitmap.width(), height / bitmap.height()));
                native = std::move(image);
            } else
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Title item type must be text, rectangle, ellipse or image."), state);
            native->setFlag(QGraphicsItem::ItemIsSelectable);
            native->setPos(x, y);
            native->setZValue(z);
            auto transform = native->transform();
            transform.rotate(rotation);
            native->setTransform(transform);
            native->setData(TitleDocument::RotateFactor, QVariantList{0.0, 0.0, rotation});
            sceneItems.append(native.get());
            owned.push_back(std::move(native));
        }
        QGraphicsRectItem startView(QRectF(0, 0, size.width(), size.height())), endView(QRectF(0, 0, size.width(), size.height()));
        const auto viewport = [&](const QString &key, QGraphicsRectItem &view) {
            if (!arguments.contains(key)) return true;
            if (!arguments.value(key).isObject()) return false;
            const auto rect = arguments.value(key).toObject();
            double x = 0, y = 0, width = 0, height = 0;
            if (!real(rect, QStringLiteral("x"), -4 * size.width(), 4 * size.width(), x) ||
                !real(rect, QStringLiteral("y"), -4 * size.height(), 4 * size.height(), y) ||
                !real(rect, QStringLiteral("width"), 1, 4 * size.width(), width) || !real(rect, QStringLiteral("height"), 1, 4 * size.height(), height))
                return false;
            view.setRect(0, 0, width, height);
            view.setPos(x, y);
            return true;
        };
        if (!viewport(QStringLiteral("startViewport"), startView) || !viewport(QStringLiteral("endViewport"), endView))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("A viewport requires finite x/y/width/height."), state);
        auto xml = TitleDocument::xml(sceneItems, size.width(), size.height(), &startView, &endView, false);
        xml.documentElement().setAttribute(QStringLiteral("duration"), duration);
        const QString xmlText = xml.toString();
        if (xmlText.toUtf8().size() > 524288) return failure(QStringLiteral("LIMIT"), QStringLiteral("The native title is too large."), state);
        QString binId = arguments.value(QStringLiteral("binId")).toString();
        if (!binId.isEmpty()) {
            const auto clip = bin->hasClip(binId) ? bin->getClipByBinID(binId) : nullptr;
            if (!clip || !clip->statusReady() || clip->clipType() != ClipType::Text || duration != clip->getFramePlaytime())
                return failure(QStringLiteral("INVALID_TITLE"),
                               QStringLiteral("Replace an existing ready title with its current duration; resize placements separately."), state);
            const QMap<QString, QString> before{{QStringLiteral("xmldata"), clip->getProducerProperty(QStringLiteral("xmldata"))}};
            const QMap<QString, QString> after{{QStringLiteral("xmldata"), xmlText}};
            Fun undo = [clip, before] {
                clip->setProperties(before);
                return true;
            };
            Fun redo = [clip, after] {
                clip->setProperties(after);
                return true;
            };
            redo();
            pCore->pushUndo(undo, redo, QStringLiteral("MCP edit native title"));
        } else {
            binId = ClipCreator::createTitleClip({{QStringLiteral("xmldata"), xmlText}}, duration,
                                                 arguments.value(QStringLiteral("name")).toString(QStringLiteral("MCP title")), bin->getRootFolder()->clipId(),
                                                 bin);
            if (binId == QLatin1String("-1") || !bin->hasClip(binId))
                return failure(QStringLiteral("TITLE_FAILED"), QStringLiteral("Native title creation failed."), snapshot());
        }
        m_imports.insert(binId, MediaImport{QString(), bin->getClipByBinID(binId)});
        return finish({{QStringLiteral("binId"), binId},
                       {QStringLiteral("status"), bin->getClipByBinID(binId)->statusReady() ? QStringLiteral("ready") : QStringLiteral("loading")},
                       {QStringLiteral("itemCount"), sceneItems.size()},
                       {QStringLiteral("durationFrames"), duration}});
    }
    if (name == QLatin1String("subtitle_file")) {
        QString path;
        const QString operation = arguments.value(QStringLiteral("operation")).toString();
        if (!text(arguments, QStringLiteral("path"), path, 32768) || !QFileInfo(path).isAbsolute() ||
            (operation != QLatin1String("import") && operation != QLatin1String("export")))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use import/export and an absolute subtitle file path."), state);
        const QFileInfo file(path);
        const QString extension = file.suffix().toLower();
        auto model = timeline->getSubtitleModel();
        if (operation == QLatin1String("export")) {
            if (!model || (extension != QLatin1String("ass") && extension != QLatin1String("srt")) || file.exists() || !file.dir().exists())
                return failure(QStringLiteral("INVALID_PATH"),
                               QStringLiteral("Export active subtitles to a new .ass or .srt path with an existing parent folder."), state);
            // Native saveSubtitleData opens its own QFile. On Windows a live
            // QTemporaryFile retains its native handle after close(), preventing
            // that writer from opening/truncating the same file.
            QTemporaryDir staged(file.dir().absoluteFilePath(QStringLiteral(".mcp-subtitles-XXXXXX")));
            if (!staged.isValid()) return failure(QStringLiteral("SAVE_FAILED"), QStringLiteral("Cannot create the subtitle export staging directory."), state);
            const QString temporary = staged.filePath(QStringLiteral("subtitles.") + extension);
            const int count = model->saveSubtitleData(model->toJson(), temporary);
            if (count != model->count() || !QFileInfo(temporary).isFile())
                return failure(QStringLiteral("SAVE_FAILED"), QStringLiteral("The native subtitle writer did not produce the expected event count."), state);
            QFile output(temporary);
            if (!output.rename(path)) return failure(QStringLiteral("SAVE_FAILED"), output.errorString(), state);
            return finish({{QStringLiteral("path"), path}, {QStringLiteral("subtitleCount"), count}, {QStringLiteral("undoable"), false}});
        }
        int offset = 0;
        double sourceFps = pCore->getCurrentFps();
        if ((arguments.contains(QStringLiteral("offsetFrames")) && !integer(arguments, QStringLiteral("offsetFrames"), -MaxFrame, MaxFrame, offset)) ||
            (arguments.contains(QStringLiteral("sourceFps")) && !real(arguments, QStringLiteral("sourceFps"), 1, 240, sourceFps)) || !file.isFile() ||
            file.isSymLink() || file.size() <= 0 || file.size() > 16 * 1024 * 1024 ||
            (extension != QLatin1String("ass") && extension != QLatin1String("srt") && extension != QLatin1String("vtt") && extension != QLatin1String("sbv")))
            return failure(QStringLiteral("INVALID_PATH"), QStringLiteral("Import a bounded local ASS/SRT/VTT/SBV subtitle file and valid frame-rate offset."),
                           state);
        QString encoding = QStringLiteral("UTF-8");
        if (arguments.contains(QStringLiteral("encoding")) && !text(arguments, QStringLiteral("encoding"), encoding, 128))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use a bounded subtitle codec name."), state);
        auto encodingId = QStringConverter::encodingForName(encoding.toLatin1().constData());
        if (!encodingId) return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use a subtitle encoding recognized by this Qt build."), state);
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly)) return failure(QStringLiteral("INVALID_PATH"), input.errorString(), state);
        const QByteArray bytes = input.read(16 * 1024 * 1024 + 1);
        if (input.error() != QFileDevice::NoError || !input.atEnd() || bytes.size() > 16 * 1024 * 1024)
            return failure(QStringLiteral("INVALID_PATH"), QStringLiteral("Subtitle input could not be read within the file limit."), state);
        input.close();
        // QTextStream enables Unicode BOM detection even when a codec is selected.
        // Decode once with the same BOM precedence, reject incomplete sequences,
        // and hand the native parser immutable UTF-8 in a lowercase-suffix file.
        if (bytes.startsWith(QByteArray::fromHex("efbbbf")) || bytes.startsWith(QByteArray::fromHex("fffe")) || bytes.startsWith(QByteArray::fromHex("feff")) ||
            bytes.startsWith(QByteArray::fromHex("0000feff")))
            encodingId = QStringConverter::encodingForData(bytes);
        if (!encodingId) return failure(QStringLiteral("INVALID_SUBTITLE_FILE"), QStringLiteral("Unrecognized subtitle Unicode BOM."), state);
        QStringDecoder decoder(*encodingId, QStringConverter::Flag::Stateless);
        const QString contents = decoder.decode(bytes);
        if (decoder.hasError() || contents.contains(QChar::Null))
            return failure(QStringLiteral("INVALID_SUBTITLE_FILE"), QStringLiteral("Subtitle decoding failed for this encoding."), state);
        int candidateCount = 0;
        QString preflightCode, reason;
        if (!subtitleFilePreflight(contents, extension, model, pCore->getCurrentFps(), sourceFps, offset, candidateCount, preflightCode, reason))
            return failure(preflightCode, reason, state);
        const QByteArray utf8 = contents.toUtf8();
        if (utf8.size() > 16 * 1024 * 1024)
            return failure(QStringLiteral("INVALID_SUBTITLE_FILE"), QStringLiteral("Normalized UTF-8 subtitle data exceeds the native 16 MiB input limit."),
                           state);
        QTemporaryDir staged;
        if (!staged.isValid()) return failure(QStringLiteral("SAVE_FAILED"), QStringLiteral("Cannot stage the validated subtitle file."), state);
        const QString validatedPath = staged.filePath(QStringLiteral("subtitles.") + extension);
        QFile validated(validatedPath);
        if (!validated.open(QIODevice::WriteOnly | QIODevice::NewOnly) || validated.write(utf8) != utf8.size() || !validated.flush())
            return failure(QStringLiteral("SAVE_FAILED"), QStringLiteral("Cannot stage the validated subtitle file."), state);
        validated.close();
        if (!model) model = timeline->createSubtitleModel();
        if (model->isLocked()) return failure(QStringLiteral("SUBTITLES_LOCKED"), QStringLiteral("Unlock subtitles before importing."), state);
        const int before = model->count();
        const auto imported = model->importSubtitleWithUndo(validatedPath, offset, true, float(sourceFps), float(pCore->getCurrentFps()),
                                                            QByteArrayLiteral("UTF-8"), candidateCount);
        if (!imported.success) return failure(QStringLiteral("SUBTITLE_IMPORT_FAILED"), imported.error, snapshot());
        if (imported.importedCount != candidateCount || model->count() - before != imported.importedCount)
            return failure(QStringLiteral("SUBTITLE_IMPORT_FAILED"), QStringLiteral("The native subtitle transaction returned an inconsistent result."),
                           snapshot());
        auto data = subtitleReadback(model);
        data.insert(QStringLiteral("importedCount"), model->count() - before);
        data.insert(QStringLiteral("format"), extension);
        data.insert(QStringLiteral("encoding"), QString::fromLatin1(QStringConverter::nameForEncoding(*encodingId)));
        return finish(data);
    }
    if (name == QLatin1String("subtitle_language")) {
        const QString operation = arguments.value(QStringLiteral("operation")).toString();
        QString displayName = arguments.value(QStringLiteral("name")).toString();
        int id = -1, copy = -1;
        auto model = timeline->getSubtitleModel();
        if (!model && operation != QLatin1String("create"))
            return failure(QStringLiteral("INVALID_SUBTITLE"), QStringLiteral("Create subtitle data before selecting a language track."), state);
        if (displayName.size() > 256 || displayName.contains(QChar::Null) ||
            (operation != QLatin1String("create") && !integer(arguments, QStringLiteral("trackId"), 0, MaxFrame, id)))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use a native language trackId and bounded display name."), state);
        if (!model && arguments.contains(QStringLiteral("copyTrackId")) && (!integer(arguments, QStringLiteral("copyTrackId"), 0, MaxFrame, copy) || copy != 0))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("A new subtitle model has only language track 0 to duplicate."), state);
        if (!model) model = timeline->createSubtitleModel();
        const auto before = model->getSubtitlesList();
        const auto find = [&before](int wanted) {
            for (auto i = before.begin(); i != before.end(); ++i)
                if (i.key().first == wanted) return true;
            return false;
        };
        if (operation == QLatin1String("create")) {
            if (before.size() >= 64 ||
                (arguments.contains(QStringLiteral("copyTrackId")) && (!integer(arguments, QStringLiteral("copyTrackId"), 0, MaxFrame, copy) || !find(copy))))
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use an existing copyTrackId and at most 64 language tracks."), state);
            model->createNewSubtitle(displayName, copy);
            const auto after = model->getSubtitlesList();
            for (auto i = after.begin(); i != after.end(); ++i)
                if (!find(i.key().first)) {
                    id = i.key().first;
                    break;
                }
            if (id < 0) return failure(QStringLiteral("SUBTITLE_FAILED"), QStringLiteral("Native language track creation failed."), snapshot());
        } else {
            if (!find(id)) return failure(QStringLiteral("INVALID_SUBTITLE"), QStringLiteral("Language track ID does not exist."), state);
            if (operation == QLatin1String("activate")) {
                model->activateSubtitle(id);
                if (m_document->getSequenceProperty(timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt() != id)
                    return failure(QStringLiteral("SUBTITLE_FAILED"), QStringLiteral("Native language track activation failed."), snapshot());
            } else if (operation == QLatin1String("rename")) {
                if (displayName.isEmpty()) return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Language display name cannot be empty."), state);
                model->updateModelName(id, displayName);
            } else if (operation == QLatin1String("delete")) {
                const int active =
                    m_document->getSequenceProperty(timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt();
                if (before.size() < 2 || active == id)
                    return failure(QStringLiteral("INVALID_ARGUMENT"),
                                   QStringLiteral("Activate another language before deleting this one; retain at least one language."), state);
                if (!model->deleteSubtitle(id))
                    return failure(QStringLiteral("SUBTITLE_FAILED"), QStringLiteral("Native language track deletion failed."), snapshot());
            } else
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Unknown language track operation."), state);
        }
        auto data = subtitleReadback(model);
        data.insert(QStringLiteral("trackId"), id);
        data.insert(QStringLiteral("undoable"), true);
        return finish(data);
    }
    if (name == QLatin1String("get_subtitles")) {
        auto data = subtitleReadback(timeline->getSubtitleModel());
        data.insert(QStringLiteral("ok"), true);
        data.insert(QStringLiteral("state"), state);
        return toolResult(data);
    }
    if (name == QLatin1String("edit_subtitle")) {
        const QString operation = arguments.value(QStringLiteral("operation")).toString();
        const QStringList operations{QStringLiteral("add"),          QStringLiteral("update"),       QStringLiteral("remove"),
                                     QStringLiteral("style"),        QStringLiteral("remove_style"), QStringLiteral("layer_add"),
                                     QStringLiteral("layer_remove"), QStringLiteral("lock"),         QStringLiteral("disable")};
        if (!operations.contains(operation)) return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Unknown subtitle operation."), state);
        int startFrame = 0, endFrame = 0, layer = 0, id = -1;
        QString content;
        if (operation == QLatin1String("add") && (!integer(arguments, QStringLiteral("startFrame"), 0, MaxFrame - 1, startFrame) ||
                                                  !integer(arguments, QStringLiteral("endFrame"), startFrame + 1, MaxFrame, endFrame) ||
                                                  !text(arguments, QStringLiteral("text"), content, 32768, true)))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("A subtitle needs text and a positive bounded [startFrame,endFrame) range."),
                           state);
        if (arguments.contains(QStringLiteral("layer")) && !integer(arguments, QStringLiteral("layer"), 0, 50, layer))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Subtitle layer must be 0-50."), state);
        QString styleName = arguments.value(QStringLiteral("style")).toString(QStringLiteral("Default"));
        if (styleName.size() > 256 || styleName.contains(QLatin1Char(',')) || styleName.contains(QLatin1Char('\n')) || styleName.contains(QChar::Null))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use a bounded native subtitle style name."), state);
        QString assStyle;
        if (operation == QLatin1String("style")) {
            if (!text(arguments, QStringLiteral("assStyle"), assStyle, 16384) || !validAssStyle(assStyle))
                return failure(QStringLiteral("INVALID_ARGUMENT"),
                               QStringLiteral("Provide a complete single-line native ASS style with 23 comma-separated fields."), state);
        }
        auto model = timeline->getSubtitleModel();
        if (!model && operation != QLatin1String("add") && operation != QLatin1String("style") && operation != QLatin1String("layer_add"))
            return failure(QStringLiteral("INVALID_SUBTITLE"), QStringLiteral("This sequence has no subtitles yet."), state);
        if (!model) {
            if ((operation == QLatin1String("add") && (layer != 0 || styleName != QLatin1String("Default"))) ||
                (operation == QLatin1String("layer_add") && arguments.contains(QStringLiteral("layer")) && layer > 1))
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("A new subtitle model starts with layer 0 and Default style."), state);
            for (const QString &field : {QStringLiteral("marginL"), QStringLiteral("marginR"), QStringLiteral("marginV")}) {
                int margin = 0;
                if (arguments.contains(field) && !integer(arguments, field, 0, 100000, margin))
                    return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use nonnegative bounded subtitle margins."), state);
            }
            const QString speaker = arguments.value(QStringLiteral("name")).toString(), effects = arguments.value(QStringLiteral("effects")).toString();
            if (speaker.size() > 1024 || effects.size() > 4096 || speaker.contains(QLatin1Char(',')) || speaker.contains(QLatin1Char('\n')) ||
                effects.contains(QLatin1Char(',')) || effects.contains(QLatin1Char('\n')))
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Invalid subtitle event metadata."), state);
        }
        if (!model) model = timeline->createSubtitleModel();
        if (operation != QLatin1String("lock") && model->isLocked())
            return failure(QStringLiteral("SUBTITLES_LOCKED"), QStringLiteral("Unlock the subtitle track before editing."), state);
        if (operation == QLatin1String("lock") || operation == QLatin1String("disable")) {
            if (!arguments.value(QStringLiteral("enabled")).isBool())
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("enabled must be boolean."), state);
            const bool lock = operation == QLatin1String("lock"), after = arguments.value(QStringLiteral("enabled")).toBool();
            const bool before = lock ? model->isLocked() : model->isDisabled();
            const auto set = [model, lock](bool enabled) {
                if (lock ? model->isLocked() != enabled : model->isDisabled() != enabled) {
                    if (lock)
                        model->switchLocked();
                    else
                        model->switchDisabled();
                }
                return lock ? model->isLocked() == enabled : model->isDisabled() == enabled;
            };
            Fun undo = [set, before] { return set(before); }, redo = [set, after] { return set(after); };
            if (!redo()) return failedTransaction(undo, QStringLiteral("Native subtitle state change failed."));
            pCore->pushUndo(undo, redo, QStringLiteral("MCP subtitle state"));
        } else if (operation == QLatin1String("layer_add") || operation == QLatin1String("layer_remove")) {
            if (operation == QLatin1String("layer_add")) {
                if (!arguments.contains(QStringLiteral("layer"))) layer = model->getMaxLayer() + 1;
                if (layer > model->getMaxLayer() + 1 || model->getMaxLayer() >= 50)
                    return failure(QStringLiteral("INVALID_ARGUMENT"),
                                   QStringLiteral("Insert at an existing layer position or immediately after the last layer."), state);
                model->requestCreateLayer(-1, layer);
            } else {
                if (!arguments.contains(QStringLiteral("layer")) || layer > model->getMaxLayer() || model->getMaxLayer() == 0)
                    return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Remove an existing layer while retaining at least one layer."), state);
                model->requestDeleteLayer(layer);
            }
        } else if (operation == QLatin1String("style") || operation == QLatin1String("remove_style")) {
            const auto styles = model->getAllSubtitleStyles();
            const auto existing = styles.find(styleName);
            const bool hadStyle = existing != styles.end();
            if (operation == QLatin1String("remove_style") && (!hadStyle || styleName == QLatin1String("Default")))
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Remove an existing non-Default style."), state);
            const auto before = hadStyle ? existing->second : model->getSubtitleStyle(QStringLiteral("Default"));
            const auto after = operation == QLatin1String("style") ? SubtitleStyle(assStyle) : before;
            QMap<int, QString> assignments;
            for (int sid : model->getAllSubIds())
                assignments.insert(sid, model->getStyleName(sid));
            Fun undo = [model, styleName, hadStyle, before, assignments] {
                if (hadStyle)
                    model->setSubtitleStyle(styleName, before);
                else
                    model->deleteSubtitleStyle(styleName);
                for (auto i = assignments.begin(); i != assignments.end(); ++i)
                    if (model->hasSubtitle(i.key())) {
                        const auto pos = model->getStartPosForId(i.key());
                        const int layer = model->getLayerForId(i.key());
                        auto event = model->getSubtitle(layer, pos);
                        event.setStyleName(i.value());
                        model->updateSubtitleEvent(i.key(), {layer, pos}, event);
                    }
                return true;
            };
            Fun redo = [model, styleName, after, operation] {
                if (operation == QLatin1String("style"))
                    model->setSubtitleStyle(styleName, after);
                else
                    model->deleteSubtitleStyle(styleName);
                return true;
            };
            redo();
            pCore->pushUndo(undo, redo, QStringLiteral("MCP subtitle style"));
        } else {
            const bool add = operation == QLatin1String("add");
            if (!add && (!integer(arguments, QStringLiteral("id"), 0, MaxFrame, id) || !model->hasSubtitle(id)))
                return failure(QStringLiteral("INVALID_SUBTITLE"), QStringLiteral("Use a subtitle ID from get_subtitles."), state);
            const int oldLayer = add ? 0 : model->getLayerForId(id);
            const GenTime oldStart = add ? GenTime() : model->getStartPosForId(id);
            const SubtitleEvent before = add ? SubtitleEvent(true, GenTime(), QStringLiteral("Default"), QString(), 0, 0, 0, QString(), QString())
                                             : model->getSubtitle(oldLayer, oldStart);
            if (!add) {
                startFrame = oldStart.frames(pCore->getCurrentFps());
                endFrame = before.endTime().frames(pCore->getCurrentFps());
                if (!arguments.contains(QStringLiteral("layer"))) layer = oldLayer;
                if (!arguments.contains(QStringLiteral("style"))) styleName = before.styleName();
                content = arguments.contains(QStringLiteral("text")) ? arguments.value(QStringLiteral("text")).toString() : before.text();
                if ((arguments.contains(QStringLiteral("startFrame")) && !integer(arguments, QStringLiteral("startFrame"), 0, MaxFrame - 1, startFrame)) ||
                    (arguments.contains(QStringLiteral("endFrame")) && !integer(arguments, QStringLiteral("endFrame"), 1, MaxFrame, endFrame)) ||
                    endFrame <= startFrame || (arguments.contains(QStringLiteral("text")) && !text(arguments, QStringLiteral("text"), content, 32768, true)))
                    return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use bounded subtitle text and positive ordered start/end frames."),
                                   state);
            }
            if (operation != QLatin1String("remove") &&
                (layer > model->getMaxLayer() || model->getAllSubtitleStyles().find(styleName) == model->getAllSubtitleStyles().end()))
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Create the requested subtitle layer/style before assigning it."), state);
            int marginL = before.marginL(), marginR = before.marginR(), marginV = before.marginV();
            for (const auto &entry : {std::pair<const char *, int *>{"marginL", &marginL}, {"marginR", &marginR}, {"marginV", &marginV}}) {
                const auto key = QString::fromLatin1(entry.first);
                if (arguments.contains(key) && !integer(arguments, key, 0, 100000, *entry.second))
                    return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use nonnegative bounded subtitle margins."), state);
            }
            const QString speaker = arguments.value(QStringLiteral("name")).toString(before.name());
            const QString effects = arguments.value(QStringLiteral("effects")).toString(before.effect());
            if (speaker.size() > 1024 || effects.size() > 4096 || speaker.contains(QLatin1Char(',')) || speaker.contains(QLatin1Char('\n')) ||
                effects.contains(QLatin1Char('\n')) || effects.contains(QLatin1Char(',')))
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Subtitle event metadata cannot contain field delimiters."), state);
            const auto start = std::pair<int, GenTime>{layer, GenTime(startFrame, pCore->getCurrentFps())};
            const SubtitleEvent after(arguments.value(QStringLiteral("dialogue")).toBool(add ? true : before.isDialogue()),
                                      GenTime(endFrame, pCore->getCurrentFps()), styleName, speaker, marginL, marginR, marginV, effects, content);
            if (operation != QLatin1String("remove")) {
                const int other = model->getIdForStartPos(layer, start.second);
                if (other >= 0 && other != id)
                    return failure(QStringLiteral("SUBTITLE_CONFLICT"), QStringLiteral("Another subtitle already starts at this frame in this layer."), state);
            }
            if (operation == QLatin1String("remove")) {
                if (!timeline->requestSingleSubtitleDeletion(id))
                    return failure(QStringLiteral("SUBTITLE_FAILED"), QStringLiteral("Native single-subtitle deletion failed."), snapshot());
            } else {
                if (add) id = TimelineModel::getNextId();
                Fun undo = [model, id, add, oldLayer, oldStart, before] {
                    return add ? model->removeSubtitle(id) : model->updateSubtitleEvent(id, {oldLayer, oldStart}, before);
                };
                Fun redo = [model, id, add, start, after] { return add ? model->addSubtitle(id, start, after) : model->updateSubtitleEvent(id, start, after); };
                if (!redo())
                    return failure(QStringLiteral("SUBTITLE_FAILED"), QStringLiteral("Native subtitle operation failed; inspect the current state."),
                                   snapshot());
                pCore->pushUndo(undo, redo, QStringLiteral("MCP edit subtitle"));
            }
        }
        auto data = subtitleReadback(model);
        if (id >= 0) data.insert(QStringLiteral("id"), id);
        return finish(data);
    }
    return {};
}
