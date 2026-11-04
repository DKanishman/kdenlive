/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QtTypes>
class QObject;
namespace McpWire {
inline constexpr qsizetype MaxResponseBytes = 1024 * 1024;
inline constexpr qsizetype RpcEnvelopeReserve = 4096;
inline constexpr qsizetype MaxPageBytes = 64 * 1024;
/** Conservative bound without allocating serialized JSON. Saturates at UINT64_MAX. */
quint64 jsonSizeUpperBound(const QJsonValue &value);
/** Exact Qt compact-JSON byte count, or limit+1 once that limit is exceeded. */
qsizetype jsonEncodedSize(const QJsonValue &value, qsizetype limit = MaxResponseBytes);
/** Stable JSON-pointer view. Text offsets are UTF-16 and cannot split a surrogate pair.
 * Returns unwrapped data/error; callers may add their own small context before wrapping.
 * Collections page at most 128 entries; text pages at most 8192 UTF-16 units.
 */
QJsonObject pageValue(const QJsonValue &root, const QString &jsonPointer, int offset, int limit, qsizetype maxDataBytes = MaxPageBytes);
/** Keep document identity, token, history and counts while omitting bulky views. */
QJsonObject compactState(const QJsonObject &state);
/** Small replies retain their matching text/structured copies. A large, complete
 * structured result may omit only its duplicate text copy. Oversized data is
 * retained by boundRpcResponse(), never silently converted to a transport error.
 */
QJsonObject toolResult(const QJsonObject &data, bool error = false);
/** Must be called BEFORE every native mutating tools/call. Empty means allowed.
 * A pending receipt must be explicitly released before another edit so its
 * immutable native response cannot be displaced. Owned-job cancellations are
 * exempt; a late large cancellation reply preserves status/IDs and defers only
 * extra readback. isMutation is catalog classification, not proof of completion.
 */
QJsonObject mutationPreflight(QObject *owner, const QString &toolName, bool isMutation);
/** Final dispatcher boundary, before send() and also used by handleRequest tests. */
QJsonObject boundRpcResponse(QObject *owner, const QJsonObject &response, const QString &toolName, bool isMutation);
QJsonArray toolCatalog();
/** Receipt tools are session control, routed before native project/UI readiness. */
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments);
} // namespace McpWire
