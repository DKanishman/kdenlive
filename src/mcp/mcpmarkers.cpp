/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "mcpmarkers.h"
#include "bin/model/markerlistmodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "kdenlivesettings.h"
#include "mcphelpers.h"
#include "profiles/profilemodel.hpp"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include "utils/timecode.h"
#include <KLocalizedString>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QTemporaryFile>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr int MarkerMaxFrame = 10000000;
constexpr int MarkerMaxDuration = 1000000;
constexpr int MarkerMaxCount = 4096;
constexpr qint64 MarkerMaxBytes = 4 * 1024 * 1024;
// The server bounds the entire JSON-RPC response to 1 MiB. Reserve enough for
// its envelope and the worst escaped 128-code-unit request ID, not only data.
constexpr qsizetype MarkerMaxToolResultBytes = 1024 * 1024 - 4096;
QString mk(const char *text)
{
    return QString::fromLatin1(text);
}
QJsonObject markerText(const char *description)
{
    return Mcp::property(mk("string"), mk(description));
}
QJsonObject markerInteger(const char *description)
{
    return Mcp::property(mk("integer"), mk(description));
}
QJsonObject markerBoolean(const char *description)
{
    return Mcp::property(mk("boolean"), mk(description));
}
bool markerString(const QJsonValue &value, QString &text, int maximum, bool empty = false)
{
    if (!value.isString()) return false;
    text = value.toString();
    return (empty || !text.isEmpty()) && text.size() <= maximum && !text.contains(QChar::Null);
}
QString markerHash(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
bool markerPath(const QJsonValue &value, QString &path, bool existing)
{
    if (!markerString(value, path, 32760) || !QDir::isAbsolutePath(path) || path.startsWith(mk("//")) || path.startsWith(mk("\\\\")) ||
        path.contains(mk("://")))
        return false;
    path = QDir::cleanPath(path);
    const QFileInfo info(path);
    if (info.isSymLink() || info.fileName().isEmpty()) return false;
#ifdef Q_OS_WIN
    static const QRegularExpression forbidden(mk("[<>:\"/\\\\|?*\\x00-\\x1f\\x7f]"));
    static const QRegularExpression reserved(mk("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption);
    const auto leaf = info.fileName();
    if (forbidden.match(leaf).hasMatch() || reserved.match(leaf).hasMatch() || leaf.endsWith(QLatin1Char('.')) || leaf.endsWith(QLatin1Char(' '))) return false;
    if (path.mid(2).contains(QLatin1Char(':'))) return false;
#endif
    if (existing) return info.isFile() && info.isReadable() && info.size() > 0 && info.size() <= MarkerMaxBytes;
    const QFileInfo parent(info.absolutePath());
    return !info.exists() && parent.isDir() && parent.isWritable();
}
bool readMarkerFile(const QJsonValue &value, QString &path, QByteArray &bytes, QString &text)
{
    if (!markerPath(value, path, true)) return false;
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return false;
    bytes = input.read(MarkerMaxBytes + 1);
    if (input.error() != QFile::NoError || bytes.isEmpty() || bytes.size() > MarkerMaxBytes || !input.atEnd()) return false;
    QStringDecoder decoder(QStringDecoder::Utf8);
    text = decoder(bytes);
    if (decoder.hasError() || text.contains(QChar::Null)) return false;
    if (text.startsWith(QChar::ByteOrderMark)) text.remove(0, 1);
    return true;
}
struct MarkerTarget
{
    std::shared_ptr<MarkerListModel> model;
    QString scope, binId;
    int duration = MarkerMaxFrame + 1;
    int exportEnd = 0;
};
bool markerTarget(const QJsonObject &arguments, MarkerTarget &target)
{
    target.scope = arguments.value(mk("scope")).toString();
    if (!pCore || !pCore->currentDoc() || !pCore->projectManager()->getTimeline()) return false;
    if (target.scope == mk("guides")) {
        if (arguments.contains(mk("binId"))) return false;
        const auto timeline = pCore->projectManager()->getTimeline();
        target.model = timeline->getGuideModel();
        target.exportEnd = timeline->duration();
    } else if (target.scope == mk("clip")) {
        if (!markerString(arguments.value(mk("binId")), target.binId, 128) || !pCore->projectItemModel()->hasClip(target.binId)) return false;
        const auto source = pCore->projectItemModel()->getClipByBinID(target.binId);
        if (!source || !source->statusReady()) return false;
        target.model = source->markerModel();
        target.duration = source->getFramePlaytime();
        target.exportEnd = target.duration;
    } else
        return false;
    return target.model && target.duration > 0;
}
struct MarkerInput
{
    QJsonArray markers;
};
bool markerEntry(const QJsonObject &entry, QJsonObject &normalized)
{
    for (auto it = entry.begin(); it != entry.end(); ++it)
        if (it.key() != mk("pos") && it.key() != mk("duration") && it.key() != mk("comment") && it.key() != mk("type")) return false;
    int pos = 0, duration = 0, category = 0;
    QString comment = i18n("Marker");
    if (!Mcp::integer(entry, mk("pos"), 0, MarkerMaxFrame, pos) ||
        (entry.contains(mk("duration")) && !Mcp::integer(entry, mk("duration"), 0, MarkerMaxDuration, duration)) ||
        (entry.contains(mk("type")) && !Mcp::integer(entry, mk("type"), 0, MarkerMaxFrame, category)) ||
        (entry.contains(mk("comment")) && !markerString(entry.value(mk("comment")), comment, 8192, true)) || qint64(pos) + duration > MarkerMaxFrame)
        return false;
    normalized = {{mk("pos"), pos}, {mk("duration"), duration}, {mk("type"), category}, {mk("comment"), comment}};
    return true;
}
bool markerTime(const QString &token, double &seconds)
{
    // Same seconds, mm:ss and hh:mm:ss families accepted by native importFromTxt;
    // disallow malformed/negative/non-finite fields before its permissive parser.
    static const QRegularExpression time(mk("^(?:(?:[0-9]{1,7}:)?[0-9]{1,7}:)?[0-9]{1,10}(?:\\.[0-9]{1,12})?$"));
    if (!time.match(token).hasMatch()) return false;
    const auto parts = token.split(QLatin1Char(':'));
    seconds = 0;
    for (const auto &part : parts) {
        bool ok = false;
        const double value = part.toDouble(&ok);
        if (!ok) return false;
        seconds = seconds * 60 + value;
    }
    return std::isfinite(seconds) && seconds >= 0 && seconds <= std::numeric_limits<int>::max() && seconds * pCore->getCurrentFps() <= MarkerMaxFrame;
}
bool parseMarkers(const QString &text, const QString &format, MarkerInput &input)
{
    if (format == mk("json")) {
        QJsonParseError error;
        const auto json = QJsonDocument::fromJson(text.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !json.isArray() || json.array().size() > MarkerMaxCount) return false;
        for (const auto &value : json.array()) {
            QJsonObject entry;
            if (!value.isObject() || !markerEntry(value.toObject(), entry)) return false;
            input.markers.append(entry);
        }
    } else if (format == mk("text")) {
        for (QString line : text.split(QLatin1Char('\n'))) {
            if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
            if (line.isEmpty()) continue;
            if (line.size() > 8300 || input.markers.size() >= MarkerMaxCount) return false;
            const int separator = line.indexOf(QLatin1Char(' '));
            const QString token = separator < 0 ? line : line.left(separator);
            QString comment = separator < 0 ? QString() : line.mid(separator + 1);
            double seconds = 0, durationSeconds = 0;
            if (!markerTime(token, seconds)) return false;
            if (comment.endsWith(QLatin1Char(']'))) {
                const int bracket = comment.lastIndexOf(QLatin1Char('['));
                if (bracket > 0) {
                    bool ok = false;
                    const auto duration = comment.mid(bracket + 1, comment.size() - bracket - 2);
                    const double parsedDuration = duration.toDouble(&ok);
                    // Native text import treats nonpositive/nonnumeric brackets as
                    // literal comment text, and only a positive number as a range.
                    if (ok && parsedDuration > 0) {
                        if (!std::isfinite(parsedDuration) || parsedDuration * pCore->getCurrentFps() > MarkerMaxDuration) return false;
                        durationSeconds = parsedDuration;
                        comment = comment.left(bracket).trimmed();
                    }
                }
            }
            const int frame = GenTime(seconds).frames(pCore->getCurrentFps());
            const int duration = GenTime(durationSeconds).frames(pCore->getCurrentFps());
            if (comment.size() > 8192 || qint64(frame) + duration > MarkerMaxFrame) return false;
            input.markers.append(
                QJsonObject{{mk("pos"), frame}, {mk("duration"), duration}, {mk("type"), KdenliveSettings::default_marker_type()}, {mk("comment"), comment}});
        }
        if (input.markers.isEmpty()) return false;
    } else
        return false;
    QSet<int> positions;
    for (const auto &value : input.markers) {
        const int pos = value.toObject().value(mk("pos")).toInt();
        if (positions.contains(pos)) return false;
        positions.insert(pos);
    }
    return true;
}
bool equalMarker(const CommentedTime &actual, const CommentedTime &desired)
{
    const auto fps = pCore->getCurrentFps();
    // CommentedTime::operator== compares position only, not marker content.
    return actual.time().frames(fps) == desired.time().frames(fps) && actual.comment() == desired.comment() && actual.markerType() == desired.markerType() &&
           actual.duration().frames(fps) == desired.duration().frames(fps);
}
QString chapterTime(qint64 milliseconds)
{
    const qint64 seconds = std::abs(milliseconds / 1000), hours = seconds / 3600, minutes = seconds % 3600 / 60, remain = seconds % 60;
    const QString sign = milliseconds < 0 && seconds > 0 ? mk("-") : QString();
    return hours == 0 ? QStringLiteral("%1%2:%3").arg(sign).arg(minutes).arg(remain, 2, 10, QLatin1Char('0'))
                      : QStringLiteral("%1%2:%3:%4").arg(sign).arg(hours).arg(minutes, 2, 10, QLatin1Char('0')).arg(remain, 2, 10, QLatin1Char('0'));
}
QString ffmetadataValue(QString text)
{
    text.replace(mk("\\"), mk("\\\\"));
    for (const QChar ch : {QLatin1Char('='), QLatin1Char(';'), QLatin1Char('#')})
        text.replace(QString(ch), QLatin1Char('\\') + QString(ch));
    text.replace(mk("\r\n"), mk("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    text.replace(mk("\n"), mk("\\\n"));
    return text;
}
bool boundedMarkerReplace(QString &text, const QString &before, const QString &after)
{
    if (before.isEmpty() || text.size() > MarkerMaxBytes) return false;
    quint64 count = 0;
    qsizetype from = 0;
    // QString::replace does not rescan an inserted replacement or count
    // overlapping original matches. Future placeholder passes do inspect it.
    while ((from = text.indexOf(before, from, Qt::CaseSensitive)) >= 0) {
        ++count;
        from += before.size();
    }
    const quint64 retained = quint64(text.size()) - count * quint64(before.size());
    const quint64 remaining = quint64(MarkerMaxBytes) - retained;
    if (count > 0 && quint64(after.size()) > remaining / count) return false;
    text.replace(before, after, Qt::CaseSensitive);
    return true;
}
bool nativeExport(const MarkerTarget &target, const QString &format, int category, int offset, int end, const QString &pattern, QByteArray &bytes)
{
    if (target.model->rowCount() > MarkerMaxCount) return false;
    qint64 characters = 0;
    for (const auto &marker : target.model->getAllMarkers(category)) {
        characters += marker.comment().size();
        if (characters > MarkerMaxBytes) return false;
    }
    if (format == mk("json")) {
        bytes = target.model->toJson({category}).toUtf8();
        return bytes.size() <= MarkerMaxBytes;
    }
    auto markers = target.model->getAllMarkers(category);
    if (markers.size() > MarkerMaxCount) return false;
    QString result = format == mk("ffmetadata") ? mk(";FFMETADATA1\n\n") : QString();
    const auto fps = pCore->getCurrentFps();
    for (int i = 0; i < markers.size(); ++i) {
        const auto &marker = markers.at(i);
        const GenTime current = marker.time() + GenTime(offset, fps);
        const GenTime next = (i + 1 == markers.size() ? GenTime(end, fps) : markers.at(i + 1).time()) + GenTime(offset, fps);
        const GenTime markerEnd = marker.endTime() + GenTime(offset, fps);
        if (std::abs(current.seconds() * fps) > MarkerMaxFrame || std::abs(next.seconds() * fps) > MarkerMaxFrame ||
            std::abs(markerEnd.seconds() * fps) > MarkerMaxFrame)
            return false;
        if (format == mk("ffmetadata")) {
            const auto finish = marker.hasRange() ? marker.endTime() + GenTime(offset, fps) : next;
            if (finish <= current || current.frames(fps) < 0 || finish.frames(fps) > MarkerMaxFrame) return false;
            result += QStringLiteral("[CHAPTER]\nTIMEBASE=%1/%2\nSTART=%3\nEND=%4\ntitle=%5\n\n")
                          .arg(pCore->getCurrentProfile()->frame_rate_den())
                          .arg(pCore->getCurrentProfile()->frame_rate_num())
                          .arg(current.frames(fps))
                          .arg(finish.frames(fps))
                          .arg(ffmetadataValue(marker.comment()));
        } else if (format == mk("text")) {
            QString line = pattern;
            const auto finish = marker.endTime() + GenTime(offset, fps);
            if (!boundedMarkerReplace(line, mk("{{index}}"), QString::number(i + 1))) return false;
            if (!boundedMarkerReplace(line, mk("{{realtimecode}}"), pCore->timecode().getDisplayTimecode(current, false))) return false;
            if (!boundedMarkerReplace(line, mk("{{timecode}}"), chapterTime(current.ms()))) return false;
            if (!boundedMarkerReplace(line, mk("{{nexttimecode}}"), chapterTime(next.ms()))) return false;
            if (!boundedMarkerReplace(line, mk("{{frame}}"), QString::number(current.frames(fps)))) return false;
            if (!boundedMarkerReplace(line, mk("{{nextframe}}"), QString::number(next.frames(fps)))) return false;
            if (!boundedMarkerReplace(line, mk("{{comment}}"), marker.comment())) return false;
            if (!boundedMarkerReplace(line, mk("{{category}}"), pCore->markerTypes.value(marker.markerType()).displayName)) return false;
            if (!boundedMarkerReplace(line, mk("{{duration}}"), QString::number(marker.duration().frames(fps)))) return false;
            if (!boundedMarkerReplace(line, mk("{{durationtimecode}}"), pCore->timecode().getDisplayTimecode(marker.duration(), false))) return false;
            if (!boundedMarkerReplace(line, mk("{{endtimecode}}"), pCore->timecode().getDisplayTimecode(finish, false))) return false;
            if (!boundedMarkerReplace(line, mk("{{endframe}}"), QString::number(finish.frames(fps)))) return false;
            if (!boundedMarkerReplace(line, mk("{{hasrange}}"), marker.hasRange() ? mk("true") : mk("false"))) return false;
            const quint64 separator = i > 0 ? 1 : 0;
            if (quint64(line.size()) + separator > quint64(MarkerMaxBytes) - quint64(result.size())) return false;
            if (i > 0) result += QLatin1Char('\n');
            result += line;
        } else
            return false;
        if (result.size() > MarkerMaxBytes) return false;
    }
    bytes = result.toUtf8();
    return bytes.size() <= MarkerMaxBytes;
}
QJsonObject markerSettings()
{
    QJsonArray raw;
    const auto categories = KdenliveSettings::guidesCategories();
    if (categories.size() > 256 || KdenliveSettings::exportGuidesFormat().size() > 4096) return {};
    for (const auto &category : categories) {
        if (category.size() > 512) return {};
        raw.append(category);
    }
    QJsonObject data{{mk("exportTextTemplate"), KdenliveSettings::exportGuidesFormat()},
                     {mk("defaultCategoryId"), KdenliveSettings::default_marker_type()},
                     {mk("storedDefaultCategories"), raw}};
    data.insert(mk("expectedSettings"), markerHash(QJsonDocument(data).toJson(QJsonDocument::Compact)));
    const auto effective = categories.isEmpty() ? KdenliveDoc::getDefaultGuideCategories() : categories;
    data.insert(mk("defaultCategories"), QJsonDocument::fromJson(MarkerListModel::categoriesListToJSon(effective).toUtf8()).array());
    data.insert(mk("effectiveExportTextTemplate"),
                KdenliveSettings::exportGuidesFormat().isEmpty() ? mk("{{timecode}} {{comment}}") : KdenliveSettings::exportGuidesFormat());
    data.insert(mk("undoable"), false);
    return data;
}
bool markerCategories(const QJsonValue &value, QStringList &categories, QSet<int> &ids)
{
    if (!value.isArray() || value.toArray().isEmpty() || value.toArray().size() > 256) return false;
    QSet<QString> colors;
    QStringList proposed;
    QSet<int> proposedIds;
    for (const auto &entry : value.toArray()) {
        if (!entry.isObject()) return false;
        const auto object = entry.toObject();
        if (object.size() != 3 || !object.contains(mk("index")) || !object.contains(mk("comment")) || !object.contains(mk("color"))) return false;
        int id = 0;
        QString name, color;
        if (!Mcp::integer(object, mk("index"), 0, 1000000, id) || proposedIds.contains(id) || !markerString(object.value(mk("comment")), name, 160) ||
            !markerString(object.value(mk("color")), color, 7) || !QRegularExpression(mk("^#[0-9a-fA-F]{6}$")).match(color).hasMatch() ||
            name.contains(QLatin1Char('\n')) || name.contains(QLatin1Char('\r')))
            return false;
        color = color.toLower();
        if (colors.contains(color)) return false;
        proposedIds.insert(id);
        colors.insert(color);
        proposed.append(QStringLiteral("%1:%2:%3").arg(name, QString::number(id), color));
    }
    categories = std::move(proposed);
    ids = std::move(proposedIds);
    return true;
}
} // namespace

namespace McpMarkers {
QJsonArray toolCatalog()
{
    const auto scope = markerText("guides for the active sequence, or clip with an explicit ready binId. Rediscover IDs after project/sequence changes.");
    const auto bin = markerText("Native ready bin clip ID, required only for clip scope.");
    const auto path =
        markerText("Absolute local file path. Import/inspection accepts UTF-8 files up to 4 MiB. Export publishes a new file and never overwrites.");
    const auto format =
        markerText("json or text for import/inspection; json, text, or ffmetadata for export. JSON preserves exact native frames/categories/ranges.");
    const auto state = markerText("Fresh expectedState from get_state.");
    return {
        Mcp::tool(
            mk("inspect_marker_file"),
            mk("Strictly inspect an existing native JSON/text marker file for the explicit target. Returns content hash, source/category bounds and a "
               "bounded preview; does not import or repair it."),
            {{mk("scope"), scope},
             {mk("binId"), bin},
             {mk("path"), path},
             {mk("format"), format},
             {mk("offset"), markerInteger("First input marker preview index, default 0.")},
             {mk("limit"), markerInteger("Maximum preview count 1..32, default 16; the encoded response budget can return fewer. Resume at nextOffset.")}},
            {mk("scope"), mk("path"), mk("format")}, true),
        Mcp::tool(mk("marker_file"),
                  mk("Import native marker JSON/text as one undo transaction, or export JSON/text/FFmetadata to a new file. Rejects malformed rows, unknown "
                     "categories, source overruns and unacknowledged conflicting replacements before native edits. Text export follows native placeholders; "
                     "not every text template is reimportable."),
                  {{mk("expectedState"), state},
                   {mk("operation"), markerText("import or export.")},
                   {mk("scope"), scope},
                   {mk("binId"), bin},
                   {mk("path"), path},
                   {mk("format"), format},
                   {mk("expectedFileHash"), markerText("Exact fileHash from inspect_marker_file, required for import.")},
                   {mk("conflictPolicy"), markerText("refuse (default) or replace. Identical existing markers are harmless.")},
                   {mk("acknowledgeMarkerReplacement"), markerBoolean("Required true when replacing different existing markers.")},
                   {mk("expectedSettings"), markerText("Fresh marker settings token, required for text import or export using the stored text template.")},
                   {mk("categoryId"), markerInteger("Export one native category, or -1 for all (default).")},
                   {mk("offsetFrames"), markerInteger("Text/FFmetadata export offset in project frames, -10000000..10000000; default 0.")},
                   {mk("endFrame"), markerInteger("Text/FFmetadata export final point-marker chapter end before offset; defaults to native target duration.")},
                   {mk("textTemplate"), markerText("Text export template, at most 4096 characters. Native placeholders: "
                                                   "index,realtimecode,timecode,nexttimecode,frame,nextframe,comment,category,duration,durationtimecode,"
                                                   "endtimecode,endframe,hasrange. Unrecognized text remains literal.")}},
                  {mk("expectedState"), mk("operation"), mk("scope"), mk("path"), mk("format")}),
        Mcp::tool(mk("get_marker_settings"),
                  mk("Read native default marker category, default categories for future projects, and the native single guide-export text template. Returns "
                     "expectedSettings; these preferences are separate from project undo."),
                  {}, {}, true),
        Mcp::tool(
            mk("edit_marker_settings"),
            mk("Explicitly change native marker/export preferences without changing current project categories or marker content. Requires fresh settings and "
               "project tokens plus acknowledgement. Preferences have no document undo; read back their values."),
            {{mk("expectedState"), state},
             {mk("expectedSettings"), markerText("Exact settings token from get_marker_settings.")},
             {mk("acknowledgeSettingsChange"), markerBoolean("Required true: changes native preferences outside document undo.")},
             {mk("exportTextTemplate"), markerText("Native default text template, up to 4096 characters; empty restores native fallback.")},
             {mk("defaultCategoryId"), markerInteger("Existing current native category ID that is also present in the resulting defaults.")},
             {mk("defaultCategories"), Mcp::property(mk("array"), mk("Native default category objects {index,comment,color}, 1..256 unique IDs/colors, "
                                                                     "#RRGGBB, used for future projects only. The resulting defaultCategoryId must exist."))}},
            {mk("expectedState"), mk("expectedSettings"), mk("acknowledgeSettingsChange")})};
}
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState,
                     const std::function<QJsonObject()> &stateReader)
{
    Q_UNUSED(owner)
    if (name != mk("inspect_marker_file") && name != mk("marker_file") && name != mk("get_marker_settings") && name != mk("edit_marker_settings")) return {};
    auto reject = [&](const char *code, const char *message) { return Mcp::failure(mk(code), mk(message), nativeState); };
    auto finish = [&](QJsonObject data = {}) {
        data.insert(mk("ok"), true);
        data.insert(mk("state"), stateReader());
        return Mcp::toolResult(data);
    };
    const auto settings = markerSettings();
    if (name == mk("get_marker_settings")) {
        if (settings.isEmpty()) return reject("SETTINGS_LIMIT", "Native preferences exceed bounded MCP settings readback.");
        return finish({{mk("settings"), settings}});
    }
    if (!nativeState.value(mk("ready")).toBool() || !pCore || !pCore->currentDoc() || !pCore->projectManager()->getTimeline())
        return reject("NOT_READY", "Wait for the active document and marker models to become ready.");
    if (name == mk("edit_marker_settings")) {
        if (settings.isEmpty() || arguments.value(mk("expectedSettings")).toString() != settings.value(mk("expectedSettings")).toString())
            return reject("STALE_SETTINGS", "Read fresh native marker settings before changing preferences.");
        if (!arguments.value(mk("acknowledgeSettingsChange")).toBool())
            return reject("ACKNOWLEDGEMENT_REQUIRED", "Native preferences are outside project undo; acknowledgeSettingsChange must be true.");
        QString pattern = KdenliveSettings::exportGuidesFormat();
        int category = KdenliveSettings::default_marker_type();
        QStringList defaults = KdenliveSettings::guidesCategories();
        if (arguments.contains(mk("exportTextTemplate")) && !markerString(arguments.value(mk("exportTextTemplate")), pattern, 4096, true))
            return reject("INVALID_ARGUMENT", "Use a plain text export template up to 4096 characters.");
        if (arguments.contains(mk("defaultCategoryId")) &&
            (!Mcp::integer(arguments, mk("defaultCategoryId"), 0, 1000000, category) || !pCore->markerTypes.contains(category)))
            return reject("INVALID_CATEGORY", "The new default must be an existing current native category.");
        QSet<int> defaultIds;
        if (arguments.contains(mk("defaultCategories")) && !markerCategories(arguments.value(mk("defaultCategories")), defaults, defaultIds))
            return reject("INVALID_CATEGORY", "Use 1..256 complete native default categories with unique IDs/colors, bounded names and #RRGGBB colors.");
        if (arguments.contains(mk("defaultCategoryId")) || arguments.contains(mk("defaultCategories"))) {
            if (defaultIds.isEmpty()) {
                const auto effective = defaults.isEmpty() ? KdenliveDoc::getDefaultGuideCategories() : defaults;
                for (const auto &entry : effective)
                    defaultIds.insert(entry.section(QLatin1Char(':'), -2, -2).toInt());
            }
            if (!defaultIds.contains(category))
                return reject("INVALID_CATEGORY", "Keep the resulting default category in the categories used for new projects.");
        }
        const bool changed = pattern != KdenliveSettings::exportGuidesFormat() || category != KdenliveSettings::default_marker_type() ||
                             defaults != KdenliveSettings::guidesCategories();
        if (changed) {
            KdenliveSettings::setExportGuidesFormat(pattern);
            KdenliveSettings::setGuidesCategories(defaults);
            KdenliveSettings::setDefault_marker_type(category);
            Q_EMIT pCore->updateDefaultMarkerCategory();
        }
        return finish({{mk("settings"), markerSettings()}, {mk("changed"), changed}, {mk("undoable"), false}});
    }
    MarkerTarget target;
    if (!markerTarget(arguments, target)) return reject("INVALID_TARGET", "Choose guides for the active sequence, or clip with a ready native binId.");
    const auto format = arguments.value(mk("format")).toString();
    const auto operation = arguments.value(mk("operation")).toString();
    if (name == mk("inspect_marker_file") || operation == mk("import")) {
        if (name == mk("marker_file")) {
            for (const auto &field : {mk("categoryId"), mk("offsetFrames"), mk("endFrame"), mk("textTemplate")})
                if (arguments.contains(field)) return reject("INVALID_ARGUMENT", "Export-only fields are not valid for marker import.");
            if (format == mk("text") &&
                (settings.isEmpty() || arguments.value(mk("expectedSettings")).toString() != settings.value(mk("expectedSettings")).toString()))
                return reject("STALE_SETTINGS", "Text import uses the native default category; provide fresh expectedSettings.");
        }
        QString path, text;
        QByteArray bytes;
        MarkerInput input;
        if (!readMarkerFile(arguments.value(mk("path")), path, bytes, text))
            return reject("INVALID_PATH", "Choose an existing readable local UTF-8 marker file, 1 byte through 4 MiB.");
        if (!parseMarkers(text, format, input))
            return reject("INVALID_MARKER_FILE", "Use a complete native JSON array or native seconds/mm:ss/hh:mm:ss text. Every nonempty row and duplicate "
                                                 "position is validated; malformed rows are not skipped.");
        const auto hash = markerHash(bytes);
        bool withinSource = true;
        QSet<int> unknownCategories;
        QJsonArray changedEntries;
        int conflicts = 0;
        for (const auto &value : input.markers) {
            const auto entry = value.toObject();
            const int pos = entry.value(mk("pos")).toInt(), duration = entry.value(mk("duration")).toInt(), type = entry.value(mk("type")).toInt();
            withinSource = withinSource && pos < target.duration && qint64(pos) + duration <= target.duration;
            if (!pCore->markerTypes.contains(type)) unknownCategories.insert(type);
            const CommentedTime desired(GenTime(pos, pCore->getCurrentFps()), entry.value(mk("comment")).toString(), type,
                                        GenTime(duration, pCore->getCurrentFps()));
            if (target.model->hasMarker(pos)) {
                const auto previous = target.model->marker(pos);
                if (equalMarker(previous, desired)) continue;
                ++conflicts;
            }
            changedEntries.append(entry);
        }
        QJsonArray unknown;
        auto sorted = unknownCategories.values();
        std::sort(sorted.begin(), sorted.end());
        for (int id : sorted)
            unknown.append(id);
        if (name == mk("inspect_marker_file")) {
            int offset = 0, limit = 16;
            if ((arguments.contains(mk("offset")) && !Mcp::integer(arguments, mk("offset"), 0, MarkerMaxCount, offset)) ||
                (arguments.contains(mk("limit")) && !Mcp::integer(arguments, mk("limit"), 1, 32, limit)))
                return reject("INVALID_ARGUMENT", "Use bounded file preview parameters.");
            QJsonArray preview;
            QJsonObject data{{mk("ok"), true},
                             {mk("state"), stateReader()},
                             {mk("path"), path},
                             {mk("fileHash"), hash},
                             {mk("format"), format},
                             {mk("markerCount"), input.markers.size()},
                             {mk("withinSource"), withinSource},
                             {mk("unknownCategoryIds"), unknown},
                             {mk("conflictingCount"), conflicts},
                             {mk("markers"), preview},
                             {mk("offset"), offset},
                             {mk("nextOffset"), offset},
                             {mk("hasMore"), offset < input.markers.size()}};
            auto result = Mcp::toolResult(data);
            const auto fits = [](const QJsonObject &value) { return QJsonDocument(value).toJson(QJsonDocument::Compact).size() <= MarkerMaxToolResultBytes; };
            // Measuring the complete result includes UTF-8 encoding, JSON string
            // escaping and the text copy in addition to structuredContent/state.
            if (!fits(result)) return Mcp::failure(mk("RESPONSE_LIMIT"), mk("Native state leaves insufficient room for a bounded marker preview."));
            for (int i = offset; i < std::min(int(input.markers.size()), offset + limit); ++i) {
                preview.append(input.markers.at(i));
                data.insert(mk("markers"), preview);
                data.insert(mk("nextOffset"), i + 1);
                data.insert(mk("hasMore"), i + 1 < input.markers.size());
                const auto candidate = Mcp::toolResult(data);
                if (!fits(candidate)) {
                    if (i == offset)
                        return Mcp::failure(mk("RESPONSE_LIMIT"), mk("Native state and the first marker exceed the response budget; no marker was skipped."));
                    break;
                }
                result = candidate;
            }
            return result;
        }
        if (arguments.value(mk("expectedFileHash")).toString() != hash)
            return reject("STALE_FILE", "The marker file changed or its content hash was not provided; inspect it again.");
        if (!withinSource || !unknownCategories.isEmpty())
            return reject("INVALID_MARKER_FILE",
                          "Use existing native categories and point/range frames within the target source and bounded project frame range.");
        const auto policy = arguments.value(mk("conflictPolicy")).toString(mk("refuse"));
        if (policy != mk("refuse") && policy != mk("replace")) return reject("INVALID_ARGUMENT", "conflictPolicy must be refuse or replace.");
        if (conflicts > 0 && (policy != mk("replace") || !arguments.value(mk("acknowledgeMarkerReplacement")).toBool()))
            return reject("MARKER_CONFLICT", "Existing different markers require conflictPolicy=replace and acknowledgeMarkerReplacement=true.");
        if (target.model->rowCount() + changedEntries.size() - conflicts > MarkerMaxCount)
            return reject("LIMIT", "The resulting marker model exceeds 4096 entries.");
        if (changedEntries.isEmpty()) return finish({{mk("changed"), false}, {mk("importedCount"), 0}, {mk("fileHash"), hash}, {mk("undoable"), true}});
        Fun undo = [] { return true; }, redo = [] { return true; };
        // All data and conflicts were checked before entering native mutation.
        // Canonical JSON retains the exact native frame rounding of text input
        // without relying on its permissive skip-invalid-lines behavior.
        const auto canonical = QString::fromUtf8(QJsonDocument(changedEntries).toJson(QJsonDocument::Compact));
        if (!target.model->importFromJson(canonical, true, undo, redo))
            return reject("EDIT_FAILED", "Native marker import rejected its transaction and rolled it back.");
        bool exact = true;
        for (const auto &value : changedEntries) {
            const auto entry = value.toObject();
            const int pos = entry.value(mk("pos")).toInt();
            const CommentedTime desired(GenTime(pos, pCore->getCurrentFps()), entry.value(mk("comment")).toString(), entry.value(mk("type")).toInt(),
                                        GenTime(entry.value(mk("duration")).toInt(), pCore->getCurrentFps()));
            exact = exact && target.model->hasMarker(pos) && equalMarker(target.model->marker(pos), desired);
        }
        if (!exact) {
            undo();
            return reject("EDIT_FAILED", "Native marker readback did not retain the requested points/ranges; import was rolled back.");
        }
        pCore->pushUndo(undo, redo, mk("MCP import marker file"));
        return finish({{mk("changed"), true},
                       {mk("importedCount"), changedEntries.size()},
                       {mk("replacedCount"), conflicts},
                       {mk("markerCount"), target.model->rowCount()},
                       {mk("fileHash"), hash},
                       {mk("undoable"), true}});
    }
    if (operation != mk("export")) return reject("INVALID_ARGUMENT", "Choose marker file operation import or export.");
    for (const auto &field : {mk("expectedFileHash"), mk("conflictPolicy"), mk("acknowledgeMarkerReplacement")})
        if (arguments.contains(field)) return reject("INVALID_ARGUMENT", "Import-only fields are not valid for marker export.");
    if (format != mk("json") && format != mk("text") && format != mk("ffmetadata"))
        return reject("INVALID_ARGUMENT", "Export format must be json, text, or ffmetadata.");
    QString path;
    if (!markerPath(arguments.value(mk("path")), path, false))
        return reject("OUTPUT_EXISTS_OR_INVALID", "Choose a new absolute local file path in a writable existing directory; existing files are never replaced.");
    int category = -1, offset = 0, end = target.exportEnd;
    if ((arguments.contains(mk("categoryId")) && !Mcp::integer(arguments, mk("categoryId"), -1, MarkerMaxFrame, category)) ||
        (category >= 0 && !pCore->markerTypes.contains(category)))
        return reject("INVALID_CATEGORY", "Export an existing category or -1 for all categories.");
    if (format == mk("json") && (arguments.contains(mk("offsetFrames")) || arguments.contains(mk("endFrame")) || arguments.contains(mk("textTemplate"))))
        return reject("INVALID_ARGUMENT", "Native JSON export preserves original frames and does not accept text/chapter options.");
    if ((arguments.contains(mk("offsetFrames")) && !Mcp::integer(arguments, mk("offsetFrames"), -MarkerMaxFrame, MarkerMaxFrame, offset)) ||
        (arguments.contains(mk("endFrame")) && !Mcp::integer(arguments, mk("endFrame"), 0, MarkerMaxFrame, end)))
        return reject("INVALID_ARGUMENT", "Use bounded integral export offset/end frames.");
    QString pattern;
    if (format == mk("text")) {
        if (arguments.contains(mk("textTemplate"))) {
            if (!markerString(arguments.value(mk("textTemplate")), pattern, 4096, true))
                return reject("INVALID_ARGUMENT", "Use a text template up to 4096 characters.");
        } else {
            if (settings.isEmpty() || arguments.value(mk("expectedSettings")).toString() != settings.value(mk("expectedSettings")).toString())
                return reject("STALE_SETTINGS", "Export using stored native template requires fresh expectedSettings.");
            pattern = settings.value(mk("effectiveExportTextTemplate")).toString();
        }
    } else if (arguments.contains(mk("textTemplate")))
        return reject("INVALID_ARGUMENT", "textTemplate is only valid for text export.");
    QByteArray bytes;
    if (!nativeExport(target, format, category, offset, end, pattern, bytes))
        return reject("EXPORT_LIMIT", "Native export exceeds 4 MiB or contains an invalid chapter time range; supply a valid final endFrame.");
    QTemporaryFile temporary(QDir(QFileInfo(path).absolutePath()).filePath(mk(".kdenlive-mcp-markers-XXXXXX")));
    if (!temporary.open() || temporary.write(bytes) != bytes.size() || !temporary.flush())
        return reject("WRITE_FAILED", "Could not stage the new marker file.");
    temporary.close();
    // Renaming through the owner is necessary on Windows: QTemporaryFile keeps
    // its native handle after close(), and QFile(path).rename cannot remove it.
    if (!temporary.rename(path)) return reject("WRITE_FAILED", "Could not publish the marker file without replacing an existing target.");
    temporary.setAutoRemove(false);
    return finish({{mk("path"), path},
                   {mk("format"), format},
                   {mk("fileHash"), markerHash(bytes)},
                   {mk("bytes"), bytes.size()},
                   {mk("markerCount"), target.model->getAllMarkers(category).size()},
                   {mk("undoable"), false}});
}
} // namespace McpMarkers
