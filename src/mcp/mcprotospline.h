/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once
#include "mcpwire.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QSet>
#include <cmath>
#include <limits>

namespace McpRoto {
inline constexpr int MaxPropertyBytes = 65536;
inline constexpr int MaxKeys = 512;
inline constexpr int MaxPoints = 256;

/** Validate requested native [handle1,anchor,handle2] point arrays. These
 * request limits do not constrain inspection/fingerprinting of existing data.
 * MLT's rasterizer uses a fixed 1024-node scanline scratch buffer: at most
 * 256 cubic segments have at most 768 scanline intersections. Four frame
 * widths/heights of offscreen coordinates also bound subdivision resources.
 */
inline bool validatePoints(const QJsonValue &value, QString &reason)
{
    if (!value.isArray()) {
        reason = QStringLiteral("Rotoscoping needs a native array of Bezier point triples.");
        return false;
    }
    const auto points = value.toArray();
    // Historical native data can start with a tracking flag; both the editor
    // and MLT skip that string. Empty curves are valid uninitialized/clear data.
    const qsizetype first = !points.isEmpty() && points.first().isString() ? 1 : 0;
    if (points.size() - first > MaxPoints) {
        reason = QStringLiteral("Edit at most 256 Bezier points per rotoscoping key.");
        return false;
    }
    for (qsizetype row = first; row < points.size(); ++row) {
        if (!points.at(row).isArray() || points.at(row).toArray().size() != 3) {
            reason = QStringLiteral("Every Bezier point needs exactly handle1, anchor and handle2.");
            return false;
        }
        for (const auto &handle : points.at(row).toArray()) {
            if (!handle.isArray() || handle.toArray().size() != 2) {
                reason = QStringLiteral("Every Bezier handle/anchor needs exactly two numeric coordinates.");
                return false;
            }
            for (const auto &coordinate : handle.toArray()) {
                if (!coordinate.isDouble() || !std::isfinite(coordinate.toDouble()) || std::abs(coordinate.toDouble()) > 4.0) {
                    reason = QStringLiteral("Use finite normalized rotoscoping coordinates between -4 and 4.");
                    return false;
                }
            }
        }
    }
    return true;
}

/** Normalize an entire native property, accepting constant curves or keyed
 * objects and empty-string/array/object clears. Different key point counts
 * are intentional native topology: interpolation keeps their common prefix.
 * Canonical equal-width frame keys preserve chronological order in MLT's
 * ordered JSON traversal, including frame 2 before frame 10.
 */
inline bool normalizeProperty(const QJsonValue &input, QString &converted, QString &reason)
{
    QJsonValue value = input;
    if (input.isString()) {
        const auto text = input.toString();
        if (text.size() > MaxPropertyBytes || text.contains(QChar::Null) || text.toUtf8().size() > MaxPropertyBytes) {
            reason = QStringLiteral("Rotoscoping properties must fit 65536 UTF-8 bytes.");
            return false;
        }
        if (text.trimmed().isEmpty()) {
            converted.clear();
            return true;
        }
        QJsonParseError error;
        const auto parsed = QJsonDocument::fromJson(text.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || (!parsed.isArray() && !parsed.isObject())) {
            reason = QStringLiteral("Use a native rotoscoping point array or frame-keyed JSON object.");
            return false;
        }
        value = parsed.isArray() ? QJsonValue(parsed.array()) : QJsonValue(parsed.object());
    }
    if ((!value.isArray() && !value.isObject()) || McpWire::jsonEncodedSize(value, MaxPropertyBytes) > MaxPropertyBytes) {
        reason = QStringLiteral("Use a native rotoscoping property of at most 65536 UTF-8 bytes.");
        return false;
    }
    if (value.isArray()) {
        if (!validatePoints(value, reason)) return false;
        converted = QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    } else {
        const auto object = value.toObject();
        if (object.size() > MaxKeys) {
            reason = QStringLiteral("Edit at most 512 native rotoscoping keys in one property.");
            return false;
        }
        QMap<int, QJsonArray> ordered;
        for (auto entry = object.begin(); entry != object.end(); ++entry) {
            const auto key = entry.key();
            if (key.isEmpty() || key.size() > 10) {
                reason = QStringLiteral("Use nonnegative native integer frame keys without aliases.");
                return false;
            }
            for (const auto digit : key)
                if (digit < QLatin1Char('0') || digit > QLatin1Char('9')) {
                    reason = QStringLiteral("Use nonnegative native integer frame keys without aliases.");
                    return false;
                }
            bool valid = false;
            const auto frame = key.toULongLong(&valid);
            if (!valid || frame >= quint64(std::numeric_limits<int>::max()) || ordered.contains(int(frame))) {
                reason = QStringLiteral("Use nonnegative native integer frame keys without aliases.");
                return false;
            }
            if (!validatePoints(entry.value(), reason)) return false;
            ordered.insert(int(frame), entry.value().toArray());
        }
        const int width = ordered.isEmpty() ? 1 : int(QString::number(ordered.lastKey()).size());
        QJsonObject normalized;
        for (auto entry = ordered.begin(); entry != ordered.end(); ++entry)
            normalized.insert(QString::number(entry.key()).rightJustified(width, QLatin1Char('0')), entry.value());
        converted = QString::fromUtf8(QJsonDocument(normalized).toJson(QJsonDocument::Compact));
    }
    if (converted.toUtf8().size() > MaxPropertyBytes) {
        reason = QStringLiteral("The complete native rotoscoping property exceeds 65536 UTF-8 bytes.");
        return false;
    }
    return true;
}
} // namespace McpRoto