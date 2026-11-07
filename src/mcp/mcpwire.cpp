/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "mcpwire.h"
#include <QByteArrayView>
#include <QJsonDocument>
#include <QObject>
#include <QStringList>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString wk(const char *text)
{
    return QString::fromLatin1(text);
}
constexpr qsizetype InlineValueBytes = 4096;
constexpr int MaxPointerUnits = 8192;
constexpr int MaxPointerSegments = 256;
struct Metrics
{
    quint64 bytes{0};
    quint64 quotes{0};
    quint64 slashes{0};
};
quint64 saturatedAdd(quint64 a, quint64 b)
{
    return b > std::numeric_limits<quint64>::max() - a ? std::numeric_limits<quint64>::max() : a + b;
}
quint64 saturatedMultiply(quint64 a, quint64 b)
{
    return a && b > std::numeric_limits<quint64>::max() / a ? std::numeric_limits<quint64>::max() : a * b;
}
void addMetrics(Metrics &a, const Metrics &b)
{
    a.bytes = saturatedAdd(a.bytes, b.bytes);
    a.quotes = saturatedAdd(a.quotes, b.quotes);
    a.slashes = saturatedAdd(a.slashes, b.slashes);
}
bool pairedAt(const QString &text, qsizetype low)
{
    return low > 0 && low < text.size() && text.at(low).isLowSurrogate() && text.at(low - 1).isHighSurrogate();
}
Metrics stringMetrics(const QString &text, quint64 stopAt)
{
    Metrics result{2, 2, 0};
    for (qsizetype first = 0; first < text.size();) {
        qsizetype end = std::min(first + qsizetype(2048), text.size());
        if (pairedAt(text, end)) --end;
        // Qt is the encoding oracle, including lone-surrogate escaping. Each
        // temporary serializes at most 2048 UTF-16 units, never the whole result.
        const QByteArray chunk = QJsonDocument(QJsonArray{text.mid(first, end - first)}).toJson(QJsonDocument::Compact);
        const auto encoded = QByteArrayView(chunk).sliced(2, chunk.size() - 4); // strip [" and "]
        result.bytes = saturatedAdd(result.bytes, quint64(encoded.size()));
        for (char c : encoded) {
            if (c == '"') ++result.quotes;
            if (c == '\\') ++result.slashes;
        }
        if (result.bytes > stopAt) return {stopAt + 1, result.quotes, result.slashes};
        first = end;
    }
    return result;
}
Metrics jsonMetrics(const QJsonValue &value, quint64 stopAt, int depth = 0)
{
    if (depth > 1024) return {stopAt + 1, 0, 0};
    if (value.isString()) return stringMetrics(value.toString(), stopAt);
    if (value.isArray()) {
        const auto array = value.toArray();
        Metrics result{2, 0, 0};
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (i) ++result.bytes;
            addMetrics(result, jsonMetrics(array.at(i), stopAt, depth + 1));
            if (result.bytes > stopAt) return {stopAt + 1, result.quotes, result.slashes};
        }
        return result;
    }
    if (value.isObject()) {
        const auto object = value.toObject();
        Metrics result{2, 0, 0};
        bool first = true;
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!first) ++result.bytes;
            first = false;
            addMetrics(result, stringMetrics(it.key(), stopAt));
            ++result.bytes; // colon
            addMetrics(result, jsonMetrics(it.value(), stopAt, depth + 1));
            if (result.bytes > stopAt) return {stopAt + 1, result.quotes, result.slashes};
        }
        return result;
    }
    // A scalar serializes to at most a few dozen bytes. QJsonValue maps an
    // undefined array value to JSON null, matching the actual Qt serializer.
    const QByteArray encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return {quint64(encoded.size() - 2), 0, 0};
}
quint64 upperBound(const QJsonValue &value, int depth = 0)
{
    if (depth > 1024) return std::numeric_limits<quint64>::max();
    if (value.isString()) return saturatedAdd(2, saturatedMultiply(quint64(value.toString().size()), 6));
    if (value.isArray()) {
        const auto array = value.toArray();
        quint64 result = saturatedAdd(2, array.isEmpty() ? 0 : quint64(array.size() - 1));
        for (const auto &entry : array)
            result = saturatedAdd(result, upperBound(entry, depth + 1));
        return result;
    }
    if (value.isObject()) {
        const auto object = value.toObject();
        quint64 result = saturatedAdd(2, object.isEmpty() ? 0 : quint64(object.size() - 1));
        for (auto it = object.begin(); it != object.end(); ++it) {
            result = saturatedAdd(result, saturatedAdd(3, saturatedMultiply(quint64(it.key().size()), 6))); // key quotes + colon
            result = saturatedAdd(result, upperBound(it.value(), depth + 1));
        }
        return result;
    }
    if (value.isBool()) return value.toBool() ? 4 : 5;
    if (value.isDouble()) return 32;
    return 4;
}
QJsonObject rawResult(const QJsonObject &data, const QString &text, bool error)
{
    return {{wk("content"), QJsonArray{QJsonObject{{wk("type"), wk("text")}, {wk("text"), text}}}}, {wk("structuredContent"), data}, {wk("isError"), error}};
}
QJsonObject wireFailure(const char *code, const QString &message, const QJsonObject &extra = {})
{
    QJsonObject data{{wk("ok"), false}, {wk("error"), QJsonObject{{wk("code"), wk(code)}, {wk("message"), message}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        data.insert(it.key(), it.value());
    return McpWire::toolResult(data, true);
}
QJsonObject pageFailure(const char *code, const char *message)
{
    return {{wk("ok"), false}, {wk("error"), QJsonObject{{wk("code"), wk(code)}, {wk("message"), wk(message)}}}};
}
QString valueKind(const QJsonValue &value)
{
    if (value.isArray()) return wk("array");
    if (value.isObject()) return wk("object");
    if (value.isString()) return wk("string");
    if (value.isBool()) return wk("boolean");
    if (value.isDouble()) return wk("number");
    return wk("null");
}
QString pointerToken(QString key)
{
    key.replace(QLatin1Char('~'), QStringLiteral("~0"));
    key.replace(QLatin1Char('/'), QStringLiteral("~1"));
    return key;
}
bool resolvePointer(const QJsonValue &root, const QString &pointer, QJsonValue &selected)
{
    selected = root;
    if (pointer.isEmpty()) return true;
    if (!pointer.startsWith(QLatin1Char('/')) || pointer.size() > MaxPointerUnits || pointer.contains(QChar::Null)) return false;
    const auto tokens = pointer.mid(1).split(QLatin1Char('/'), Qt::KeepEmptyParts);
    if (tokens.size() > MaxPointerSegments) return false;
    for (const auto &token : tokens) {
        QString decoded;
        decoded.reserve(token.size());
        for (qsizetype i = 0; i < token.size(); ++i) {
            const auto c = token.at(i);
            if (c != QLatin1Char('~')) {
                decoded += c;
                continue;
            }
            if (++i >= token.size()) return false;
            if (token.at(i) == QLatin1Char('0'))
                decoded += QLatin1Char('~');
            else if (token.at(i) == QLatin1Char('1'))
                decoded += QLatin1Char('/');
            else
                return false;
        }
        if (selected.isObject()) {
            const auto object = selected.toObject();
            if (!object.contains(decoded)) return false;
            selected = object.value(decoded);
        } else if (selected.isArray()) {
            if (decoded.isEmpty() || (decoded.size() > 1 && decoded.at(0) == QLatin1Char('0'))) return false;
            for (const auto c : decoded)
                if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
            bool converted = false;
            const auto index = decoded.toULongLong(&converted);
            const auto array = selected.toArray();
            if (!converted || index >= quint64(array.size())) return false;
            selected = array.at(qsizetype(index));
        } else
            return false;
    }
    return true;
}
QJsonObject valueEntry(const QJsonValue &value, const QString &pointer, bool inlineValue)
{
    QJsonObject entry{{wk("pointer"), pointer}, {wk("kind"), valueKind(value)}};
    if (inlineValue)
        entry.insert(wk("value"), value);
    else {
        entry.insert(wk("valueOmitted"), true);
        if (value.isString())
            entry.insert(wk("lengthUtf16"), qint64(value.toString().size()));
        else if (value.isArray())
            entry.insert(wk("length"), qint64(value.toArray().size()));
        else if (value.isObject())
            entry.insert(wk("keyCount"), qint64(value.toObject().size()));
    }
    return entry;
}
bool integer(const QJsonValue &value, int minimum, int maximum, int &out)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < minimum || number > maximum) return false;
    out = int(number);
    return true;
}
class ReceiptStore final : public QObject
{
public:
    explicit ReceiptStore(QObject *owner)
        : QObject(owner)
    {
    }
    QString id;
    QString tool;
    QJsonObject data;
    bool isError{false};
    bool isMutation{false};
};
ReceiptStore *store(QObject *owner, bool create = true)
{
    if (!owner) return nullptr;
    for (auto *child : owner->children())
        if (auto *existing = dynamic_cast<ReceiptStore *>(child)) return existing;
    return create ? new ReceiptStore(owner) : nullptr;
}
bool isCancellation(const QString &name)
{
    return name == wk("cancel_render") || name == wk("cancel_transcode") || name == wk("cancel_speech_job") || name == wk("cancel_effect_analysis") ||
           name == wk("cancel_media_job") || name == wk("cancel_alignment_job");
}
QJsonObject receiptInfo(const ReceiptStore *receipt)
{
    return {{wk("id"), receipt->id},
            {wk("tool"), receipt->tool},
            {wk("sessionScoped"), true},
            {wk("captured"), true},
            {wk("isMutation"), receipt->isMutation},
            {wk("toolIsError"), receipt->isError},
            {wk("outcomeMeaning"), wk("Original response captured; inspect its accepted, changed and job/status fields for execution status.")},
            {wk("detailsTool"), wk("get_result_page")},
            {wk("releaseTool"), wk("release_result_receipt")}};
}
QJsonObject pending(const ReceiptStore *receipt)
{
    return wireFailure("RESULT_RECEIPT_PENDING",
                       wk("Read and explicitly release the retained result before another mutation or oversized result. No new mutation was executed."),
                       {{wk("receipt"), receiptInfo(receipt)}});
}
QJsonObject errorSummary(const QJsonObject &error)
{
    QJsonObject result;
    const auto code = error.value(wk("code"));
    const auto message = error.value(wk("message"));
    if (code.isString()) {
        result.insert(wk("code"), code.toString().left(256));
        if (code.toString().size() > 256) result.insert(wk("codeTruncated"), true);
    }
    if (message.isString()) {
        auto text = message.toString().left(2048);
        if (pairedAt(message.toString(), text.size())) text.chop(1);
        result.insert(wk("message"), text);
        if (message.toString().size() > text.size()) result.insert(wk("messageTruncated"), true);
    }
    return result;
}
QJsonObject compactJobObject(const QJsonObject &source)
{
    if (McpWire::jsonEncodedSize(source, InlineValueBytes) <= InlineValueBytes) return source;
    QJsonObject result;
    // Keep native identity and outcome independently of logs, paths, playlists,
    // transcript segments and output collections. Fixed keys and bounded scalar
    // strings keep this view below 64 KiB, including escaped control characters.
    const QStringList fields{wk("jobId"),
                             wk("status"),
                             wk("kind"),
                             wk("operation"),
                             wk("mode"),
                             wk("engine"),
                             wk("phase"),
                             wk("ok"),
                             wk("isError"),
                             wk("accepted"),
                             wk("changed"),
                             wk("noop"),
                             wk("noOp"),
                             wk("finished"),
                             wk("nativeFinished"),
                             wk("cancelRequested"),
                             wk("cancellationRequested"),
                             wk("allJobsFinished"),
                             wk("binId"),
                             wk("clipId"),
                             wk("progress"),
                             wk("exitCode"),
                             wk("stepIndex"),
                             wk("stepCount"),
                             wk("code"),
                             wk("message"),
                             wk("errorCode"),
                             wk("errorMessage"),
                             wk("error")};
    for (const auto &key : fields) {
        if (!source.contains(key)) continue;
        const auto value = source.value(key);
        if (key == wk("error") && value.isObject()) {
            result.insert(key, errorSummary(value.toObject()));
        } else if (value.isString()) {
            const auto text = value.toString();
            const int units = key == wk("jobId") || key == wk("message") || key == wk("errorMessage") || key == wk("error") ? 512 : 128;
            auto bounded = text.left(units);
            if (pairedAt(text, bounded.size())) bounded.chop(1);
            result.insert(key, bounded);
            if (bounded.size() < text.size()) result.insert(key + wk("Truncated"), true);
        } else if (value.isBool() || value.isDouble() || value.isNull()) {
            result.insert(key, value);
        }
    }
    result.insert(wk("readbackTruncated"), true);
    return result;
}
QJsonObject receiptSummary(const ReceiptStore *receipt)
{
    const auto &data = receipt->data;
    QJsonObject summary{{wk("ok"), data.value(wk("ok")).toBool(!receipt->isError)},
                        {wk("receipt"), receiptInfo(receipt)},
                        {wk("detailsRetained"), true},
                        {wk("resultFieldCount"), qint64(data.size())}};
    if (data.value(wk("error")).isObject()) summary.insert(wk("error"), errorSummary(data.value(wk("error")).toObject()));
    if (data.value(wk("state")).isObject()) summary.insert(wk("state"), McpWire::compactState(data.value(wk("state")).toObject()));
    if (data.value(wk("job")).isObject()) summary.insert(wk("job"), compactJobObject(data.value(wk("job")).toObject()));
    // These original flags precede optional readback regardless of field order.
    for (const auto &key : {wk("isError"), wk("accepted"), wk("changed"), wk("noop"), wk("noOp")}) {
        const auto value = data.value(key);
        if (value.isBool() || value.isDouble() || value.isNull()) summary.insert(key, value);
    }
    QJsonArray fields;
    for (auto it = data.begin(); it != data.end(); ++it) {
        if (it.key() == wk("ok") || it.key() == wk("error") || it.key() == wk("state") || it.key() == wk("job") || it.key() == wk("receipt") ||
            it.key() == wk("detailsRetained") || it.key() == wk("resultFieldCount") || it.key() == wk("resultFields"))
            continue;
        // All native response field names are short. Keep pathological keys in
        // the capture without allocating huge summary keys/pointers.
        if (it.key().size() > 512) continue;
        const auto value = it.value();
        if (McpWire::jsonEncodedSize(value, 4096) <= 4096 && McpWire::jsonEncodedSize(QJsonValue(summary), 32 * 1024) <= 32 * 1024)
            summary.insert(it.key(), value);
        if (fields.size() < 64) fields.append(valueEntry(value, QLatin1Char('/') + pointerToken(it.key()), false));
    }
    summary.insert(wk("resultFields"), fields);
    // Native top-level names are normally short. Preserve the full captured
    // data even if adversarial metadata makes its outline too large to send.
    if (McpWire::jsonEncodedSize(summary, 128 * 1024) > 128 * 1024) summary.remove(wk("resultFields"));
    return summary;
}
QJsonObject compactCancelObject(const QJsonObject &source, int depth = 0)
{
    QJsonObject result;
    QStringList omitted;
    // Preserve the actual outcome and identity before optional paths/logs.
    const QStringList priority{wk("ok"),
                               wk("accepted"),
                               wk("changed"),
                               wk("isError"),
                               wk("noop"),
                               wk("noOp"),
                               wk("job"),
                               wk("status"),
                               wk("jobId"),
                               wk("binId"),
                               wk("clipId"),
                               wk("kind"),
                               wk("operation"),
                               wk("phase"),
                               wk("finished"),
                               wk("nativeFinished"),
                               wk("cancelRequested"),
                               wk("cancellationRequested"),
                               wk("allJobsFinished"),
                               wk("progress"),
                               wk("error")};
    QStringList keys = priority;
    for (auto it = source.begin(); it != source.end(); ++it)
        if (!keys.contains(it.key())) keys.append(it.key());
    for (const auto &key : keys) {
        if (!source.contains(key)) continue;
        const auto original = source.value(key);
        QJsonValue selected = original;
        if (key == wk("job") && original.isObject())
            selected = compactJobObject(original.toObject());
        else if (key == wk("error") && original.isObject())
            selected = errorSummary(original.toObject());
        else if (key == wk("state") && original.isObject())
            selected = McpWire::compactState(original.toObject());
        else if (original.isObject() && McpWire::jsonEncodedSize(original, 4096) > 4096 && depth < 1)
            selected = compactCancelObject(original.toObject(), depth + 1);
        const qsizetype fieldBudget = key == wk("job") ? 64 * 1024 : 32 * 1024;
        if (key.size() > 512 || McpWire::jsonEncodedSize(selected, fieldBudget) > fieldBudget) {
            if (omitted.size() < 8) omitted.append(key.left(64));
            continue;
        }
        auto candidate = result;
        candidate.insert(key, selected);
        const qsizetype budget = depth == 0 ? 64 * 1024 : 16 * 1024;
        if (McpWire::jsonEncodedSize(candidate, budget) <= budget)
            result = candidate;
        else if (omitted.size() < 8)
            omitted.append(key.left(64));
    }
    if (!omitted.isEmpty()) {
        result.insert(wk("readbackTruncated"), true);
        QJsonArray fields;
        for (const auto &key : omitted)
            fields.append(key);
        result.insert(wk("omittedReadbackFields"), fields);
    }
    return result;
}
QJsonObject cancellationSummary(const ReceiptStore *receipt, const QJsonObject &data, const QString &toolName)
{
    auto summary = compactCancelObject(data);
    summary.insert(wk("pendingResultReceipt"), receiptInfo(receipt));
    summary.insert(wk("detailsDeferred"), true);
    summary.insert(wk("detailsMeaning"), wk("The original cancellation outcome/status is preserved here. Additional readback was omitted while a prior result "
                                            "is retained; release it and inspect the owned job for current details."));
    const QString statusTool = toolName == wk("cancel_alignment_job")     ? wk("alignment_job_status")
                               : toolName == wk("cancel_render")          ? wk("render_status")
                               : toolName == wk("cancel_transcode")       ? wk("transcode_status")
                               : toolName == wk("cancel_speech_job")      ? wk("speech_job_status")
                               : toolName == wk("cancel_effect_analysis") ? wk("get_effect_analysis")
                                                                          : wk("media_job_status");
    summary.insert(wk("inspectCurrentJobWith"), statusTool);
    return summary;
}
QJsonObject rpcWithResult(const QJsonObject &response, const QJsonObject &result)
{
    return {{wk("jsonrpc"), wk("2.0")}, {wk("id"), response.value(wk("id"))}, {wk("result"), result}};
}
QJsonObject property(const char *type, const char *description)
{
    return {{wk("type"), wk(type)}, {wk("description"), wk(description)}};
}
QJsonObject tool(const char *name, const char *description, const QJsonObject &properties, const QJsonArray &required, bool readOnly)
{
    return {{wk("name"), wk(name)},
            {wk("description"), wk(description)},
            {wk("inputSchema"),
             QJsonObject{{wk("type"), wk("object")}, {wk("properties"), properties}, {wk("required"), required}, {wk("additionalProperties"), false}}},
            {wk("annotations"), QJsonObject{{wk("readOnlyHint"), readOnly}, {wk("destructiveHint"), !readOnly}, {wk("openWorldHint"), false}}}};
}
} // namespace

namespace McpWire {
quint64 jsonSizeUpperBound(const QJsonValue &value)
{
    return upperBound(value);
}
qsizetype jsonEncodedSize(const QJsonValue &value, qsizetype limit)
{
    if (limit < 0) return std::numeric_limits<qsizetype>::max();
    const auto safeLimit = limit == std::numeric_limits<qsizetype>::max() ? limit - 1 : limit;
    return qsizetype(jsonMetrics(value, quint64(safeLimit)).bytes);
}
QJsonObject pageValue(const QJsonValue &root, const QString &jsonPointer, int offset, int limit, qsizetype maxDataBytes)
{
    if (offset < 0 || limit < 1 || limit > 8192 || maxDataBytes < 256 || maxDataBytes > MaxPageBytes)
        return pageFailure("INVALID_PAGE", "Use nonnegative offset, limit 1..8192 and a page budget of 256..65536 bytes.");
    QJsonValue selected;
    if (!resolvePointer(root, jsonPointer, selected)) return pageFailure("INVALID_POINTER", "Use an existing bounded JSON pointer with valid ~0/~1 escapes.");
    const auto kind = valueKind(selected);
    QJsonObject page{{wk("ok"), true}, {wk("path"), jsonPointer}, {wk("kind"), kind}, {wk("offset"), offset}};
    const qsizetype total = selected.isObject()   ? selected.toObject().size()
                            : selected.isArray()  ? selected.toArray().size()
                            : selected.isString() ? selected.toString().size()
                                                  : 1;
    if (offset > total || ((!selected.isString() && !selected.isArray() && !selected.isObject()) && offset != 0))
        return pageFailure("INVALID_OFFSET", "The offset is outside the selected value.");
    page.insert(wk("total"), qint64(total));
    if (selected.isString()) {
        const QString text = selected.toString();
        if (pairedAt(text, offset)) return pageFailure("INVALID_OFFSET", "A text offset cannot split a UTF-16 surrogate pair.");
        qsizetype length = std::min(qsizetype(limit), total - offset);
        for (;;) {
            qsizetype end = offset + length;
            if (pairedAt(text, end)) --end;
            if (end == offset && length && pairedAt(text, offset + 1)) end = offset + 2;
            auto candidate = page;
            candidate.insert(wk("text"), text.mid(offset, end - offset));
            candidate.insert(wk("countUtf16"), qint64(end - offset));
            candidate.insert(wk("nextOffset"), qint64(end));
            candidate.insert(wk("hasMore"), end < total);
            if (jsonEncodedSize(candidate, maxDataBytes) <= maxDataBytes) return candidate;
            if (!length) return pageFailure("PAGE_ITEM_TOO_LARGE", "The selected pointer/context cannot fit this page budget.");
            length /= 2;
        }
    }
    if (selected.isObject() || selected.isArray()) {
        const auto object = selected.toObject();
        const auto array = selected.toArray();
        const auto keys = object.keys(); // native QJsonObject order is deterministic
        QJsonArray entries;
        const int count = std::min(limit, 128);
        qsizetype next = offset;
        while (next < total && next - offset < count) {
            const QString token = selected.isObject() ? keys.at(next) : QString::number(next);
            const auto value = selected.isObject() ? object.value(token) : array.at(next);
            const QString pointer = jsonPointer + QLatin1Char('/') + pointerToken(token);
            bool inlineValue = jsonEncodedSize(value, InlineValueBytes) <= InlineValueBytes;
            auto entry = valueEntry(value, pointer, inlineValue);
            entry.insert(selected.isObject() ? wk("key") : wk("index"), selected.isObject() ? QJsonValue(token) : QJsonValue(qint64(next)));
            auto candidateEntries = entries;
            candidateEntries.append(entry);
            auto candidate = page;
            candidate.insert(wk("entries"), candidateEntries);
            candidate.insert(wk("nextOffset"), qint64(next + 1));
            candidate.insert(wk("hasMore"), next + 1 < total);
            if (jsonEncodedSize(candidate, maxDataBytes) > maxDataBytes && inlineValue) {
                entry = valueEntry(value, pointer, false);
                entry.insert(selected.isObject() ? wk("key") : wk("index"), selected.isObject() ? QJsonValue(token) : QJsonValue(qint64(next)));
                candidateEntries = entries;
                candidateEntries.append(entry);
                candidate.insert(wk("entries"), candidateEntries);
            }
            if (jsonEncodedSize(candidate, maxDataBytes) > maxDataBytes) break;
            entries = candidateEntries;
            ++next;
        }
        if (next == offset && next < total) return pageFailure("PAGE_ITEM_TOO_LARGE", "The selected entry's key or pointer cannot fit this page budget.");
        page.insert(wk("entries"), entries);
        page.insert(wk("nextOffset"), qint64(next));
        page.insert(wk("hasMore"), next < total);
        if (jsonEncodedSize(page, maxDataBytes) > maxDataBytes)
            return pageFailure("PAGE_ITEM_TOO_LARGE", "The selected pointer/context cannot fit this page budget.");
        return page;
    }
    page.insert(wk("value"), selected);
    page.insert(wk("nextOffset"), 1);
    page.insert(wk("hasMore"), false);
    if (jsonEncodedSize(page, maxDataBytes) > maxDataBytes)
        return pageFailure("PAGE_ITEM_TOO_LARGE", "The selected pointer/context cannot fit this page budget.");
    return page;
}
QJsonObject compactState(const QJsonObject &state)
{
    QJsonObject result;
    const QStringList keys{wk("ready"),
                           wk("sessionId"),
                           wk("documentId"),
                           wk("sequenceId"),
                           wk("epoch"),
                           wk("revision"),
                           wk("expectedState"),
                           wk("modified"),
                           wk("profile"),
                           wk("audioChannels"),
                           wk("fps"),
                           wk("width"),
                           wk("height"),
                           wk("durationFrames"),
                           wk("editMode"),
                           wk("singleSelectionMode"),
                           wk("undoIndex"),
                           wk("undoCount"),
                           wk("canUndo"),
                           wk("canRedo"),
                           wk("reason"),
                           wk("freshProjectEligible"),
                           wk("masterEffectsHash"),
                           wk("binTreeHash"),
                           wk("guidesHash"),
                           wk("markerCategoriesHash"),
                           wk("groupsHash"),
                           wk("selectionHash"),
                           wk("counts")};
    for (const auto &key : keys) {
        if (!state.contains(key)) continue;
        const auto value = state.value(key);
        if (jsonEncodedSize(value, 8192) <= 8192) {
            auto candidate = result;
            candidate.insert(key, value);
            if (jsonEncodedSize(candidate, 32 * 1024) <= 32 * 1024) result = candidate;
        }
    }
    if (state.contains(wk("projectPath"))) {
        const QString original = state.value(wk("projectPath")).toString();
        auto path = original.left(2048);
        if (pairedAt(original, path.size())) path.chop(1);
        result.insert(wk("projectPath"), path);
        if (path.size() != original.size()) result.insert(wk("projectPathTruncated"), true);
    }
    if (!result.contains(wk("counts"))) {
        const auto tracks = state.value(wk("tracks")).toArray();
        qint64 clips = 0, compositions = 0;
        for (const auto &track : tracks) {
            clips += track.toObject().value(wk("clips")).toArray().size();
            compositions += track.toObject().value(wk("compositions")).toArray().size();
        }
        result.insert(wk("counts"), QJsonObject{{wk("tracks"), qint64(tracks.size())},
                                                {wk("clips"), clips},
                                                {wk("compositions"), compositions},
                                                {wk("sources"), qint64(state.value(wk("bin")).toArray().size())},
                                                {wk("imports"), qint64(state.value(wk("imports")).toArray().size())},
                                                {wk("subtitles"), qint64(state.value(wk("subtitles")).toArray().size())}});
    }
    result.insert(wk("compact"), true);
    result.insert(wk("viewComplete"), false);
    return result;
}
QJsonObject toolResult(const QJsonObject &data, bool error)
{
    const quint64 limit = quint64(MaxResponseBytes - RpcEnvelopeReserve);
    // Most editor replies are small. A cheap conservative bound avoids making
    // a Qt serialization temporary for every string on that common path.
    if (jsonSizeUpperBound(data) <= (limit - 512) / 3) return rawResult(data, QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)), error);
    const auto metrics = jsonMetrics(data, limit);
    const auto empty = rawResult({}, QStringLiteral("{}"), error);
    const quint64 emptyBytes = quint64(jsonEncodedSize(empty));
    const quint64 mirroredBytes =
        saturatedAdd(emptyBytes, saturatedAdd(saturatedMultiply(metrics.bytes, 2), saturatedAdd(metrics.quotes, metrics.slashes))) - 4;
    if (metrics.bytes <= limit && mirroredBytes <= limit) return rawResult(data, QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)), error);
    auto result = rawResult(
        data, wk("The complete native result is in structuredContent. Its duplicate text copy was omitted to keep this local response bounded."), error);
    result.insert(wk("_meta"), QJsonObject{{wk("textCopyOmitted"), true}});
    return result;
}
QJsonObject mutationPreflight(QObject *owner, const QString &toolName, bool isMutation)
{
    if (!isMutation || toolName == wk("release_result_receipt") || isCancellation(toolName)) return {};
    const auto receipt = store(owner, false);
    return receipt && !receipt->id.isEmpty() ? pending(receipt) : QJsonObject{};
}
QJsonObject boundRpcResponse(QObject *owner, const QJsonObject &response, const QString &toolName, bool isMutation)
{
    if (jsonSizeUpperBound(response) <= quint64(MaxResponseBytes) || jsonEncodedSize(response, MaxResponseBytes) <= MaxResponseBytes) return response;
    auto *receipt = store(owner);
    if (!receipt) {
        // The server always supplies its live QObject. Preserve outcome scalars
        // rather than falsely claiming that a committed tool was not executed.
        const auto original = response.value(wk("result")).toObject();
        const auto data = original.value(wk("structuredContent")).toObject();
        return rpcWithResult(response, toolResult({{wk("ok"), data.value(wk("ok")).toBool(!original.value(wk("isError")).toBool())},
                                                   {wk("detailsUnavailable"), true},
                                                   {wk("deliveryError"), wk("MCP response owner unavailable.")}},
                                                  original.value(wk("isError")).toBool()));
    }
    const auto original = response.value(wk("result")).toObject();
    const bool toolResultData = original.value(wk("structuredContent")).isObject();
    if (!toolResultData && !isMutation) {
        // tools/list/initialize have protocol-specific result schemas. Do not
        // disguise their read-only overflow as a tools/call receipt response.
        return {{wk("jsonrpc"), wk("2.0")},
                {wk("id"), response.value(wk("id"))},
                {wk("error"),
                 QJsonObject{{wk("code"), -32001}, {wk("message"), wk("Protocol response exceeds the local message limit; no mutation was executed.")}}}};
    }
    const auto data = toolResultData ? original.value(wk("structuredContent")).toObject() : QJsonObject{{wk("rpcResult"), original}};
    const bool originalError = original.value(wk("isError")).toBool();
    if (!receipt->id.isEmpty() && isCancellation(toolName))
        return rpcWithResult(response, toolResult(cancellationSummary(receipt, data, toolName), originalError));
    if (!receipt->id.isEmpty() && !isMutation) return rpcWithResult(response, pending(receipt));
    if (!receipt->id.isEmpty()) {
        // Defensive only: correct integration calls mutationPreflight before
        // execution. If that invariant is ever bypassed, never lose either
        // native result: retain the prior capture beneath this same bundle.
        const QJsonObject prior{{wk("receipt"), receiptInfo(receipt)}, {wk("data"), receipt->data}};
        QJsonObject combined = data;
        combined.insert(wk("priorUndeliveredResult"), prior);
        receipt->data = combined;
    } else
        receipt->data = data;
    receipt->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    receipt->tool = toolName.left(256);
    receipt->isError = originalError;
    receipt->isMutation = isMutation;
    return rpcWithResult(response, toolResult(receiptSummary(receipt), originalError));
}
QJsonArray toolCatalog()
{
    return {tool("get_result_page",
                 "Read an immutable oversized native result retained by this MCP session. Omit all arguments to inspect pending receipt metadata. This does "
                 "not repeat the original command.",
                 {{wk("receiptId"), property("string", "Exact retained receipt ID; omit all arguments to inspect pending receipt metadata.")},
                  {wk("path"), property("string", "JSON pointer within captured structured data; default empty root. Escape ~ as ~0 and / as ~1.")},
                  {wk("offset"), property("integer", "Zero-based entry or UTF-16 text offset, default 0.")},
                  {wk("limit"), property("integer", "Maximum entries 1..128 or text units 1..8192, default 64. Pages also have a 64 KiB byte budget.")}},
                 {}, true),
            tool("release_result_receipt",
                 "Explicitly release retained result details after reading them. This does not undo or replay the original native command.",
                 {{wk("receiptId"), property("string", "Exact retained receipt ID.")},
                  {wk("acknowledgeRelease"), property("boolean", "True acknowledges that unread captured details will become unavailable.")}},
                 {wk("receiptId"), wk("acknowledgeRelease")}, false)};
}
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments)
{
    if (name != wk("get_result_page") && name != wk("release_result_receipt")) return {};
    const QStringList allowed = name == wk("get_result_page") ? QStringList{wk("receiptId"), wk("path"), wk("offset"), wk("limit")}
                                                              : QStringList{wk("receiptId"), wk("acknowledgeRelease")};
    for (auto it = arguments.begin(); it != arguments.end(); ++it)
        if (!allowed.contains(it.key())) return wireFailure("INVALID_ARGUMENT", wk("This argument does not belong to the receipt operation."));
    if (name == wk("get_result_page") && !arguments.contains(wk("receiptId"))) {
        if (!arguments.isEmpty())
            return wireFailure("INVALID_RECEIPT", wk("Supply a receiptId to page details, or omit all arguments to inspect pending metadata."));
        const auto current = store(owner, false);
        const bool available = current && !current->id.isEmpty();
        return toolResult({{wk("ok"), true},
                           {wk("pending"), available},
                           {wk("receipt"), available ? receiptInfo(current) : QJsonObject{}},
                           {wk("nativeCommandReplayed"), false}});
    }
    const auto idValue = arguments.value(wk("receiptId"));
    if (!idValue.isString() || idValue.toString().isEmpty() || idValue.toString().size() > 128)
        return wireFailure("INVALID_RECEIPT", wk("Use the exact retained receipt ID."));
    auto *receipt = store(owner, false);
    if (!receipt || receipt->id.isEmpty() || receipt->id != idValue.toString())
        return wireFailure("INVALID_RECEIPT", wk("That receipt does not exist in this MCP session or was released."));
    if (name == wk("release_result_receipt")) {
        if (arguments.value(wk("acknowledgeRelease")) != QJsonValue(true))
            return wireFailure("ACKNOWLEDGEMENT_REQUIRED", wk("Explicitly acknowledgeRelease=true before discarding retained result details."));
        const QString id = receipt->id;
        receipt->data = {};
        receipt->id.clear();
        receipt->tool.clear();
        receipt->isError = false;
        receipt->isMutation = false;
        return toolResult({{wk("ok"), true}, {wk("releasedReceiptId"), id}, {wk("nativeCommandReplayed"), false}});
    }
    int offset = 0, limit = 64;
    const auto pathValue = arguments.value(wk("path"));
    if ((arguments.contains(wk("offset")) && !integer(arguments.value(wk("offset")), 0, std::numeric_limits<int>::max(), offset)) ||
        (arguments.contains(wk("limit")) && !integer(arguments.value(wk("limit")), 1, 8192, limit)) ||
        (arguments.contains(wk("path")) && !pathValue.isString()))
        return wireFailure("INVALID_ARGUMENT", wk("Use a JSON pointer string and bounded integral page parameters."));
    auto page = pageValue(receipt->data, pathValue.toString(), offset, limit);
    page.insert(wk("receipt"), receiptInfo(receipt));
    page.insert(wk("nativeCommandReplayed"), false);
    return toolResult(page, !page.value(wk("ok")).toBool());
}
} // namespace McpWire
