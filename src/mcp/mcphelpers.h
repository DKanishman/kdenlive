/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once
#include "mcpwire.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
namespace Mcp {
inline QJsonObject toolResult(const QJsonObject &data, bool error = false)
{
    return McpWire::toolResult(data, error);
}
inline QJsonObject failure(const QString &code, const QString &message, const QJsonObject &state = {})
{
    QJsonObject result{{QStringLiteral("ok"), false},
                       {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
    if (!state.isEmpty()) result.insert(QStringLiteral("state"), state);
    return toolResult(result, true);
}
inline bool integer(const QJsonObject &object, const QString &key, int minimum, int maximum, int &result)
{
    const auto v = object.value(key);
    if (!v.isDouble()) return false;
    const double n = v.toDouble();
    if (!std::isfinite(n) || std::floor(n) != n || n < minimum || n > maximum) return false;
    result = static_cast<int>(n);
    return true;
}
inline bool real(const QJsonObject &object, const QString &key, double minimum, double maximum, double &result)
{
    const auto v = object.value(key);
    if (!v.isDouble()) return false;
    const double n = v.toDouble();
    if (!std::isfinite(n) || n < minimum || n > maximum) return false;
    result = n;
    return true;
}
inline QJsonObject property(const QString &type, const QString &description)
{
    return {{QStringLiteral("type"), type}, {QStringLiteral("description"), description}};
}
inline QJsonObject tool(const QString &name, const QString &description, const QJsonObject &properties, const QJsonArray &required, bool readOnly = false)
{
    return {
        {QStringLiteral("name"), name},
        {QStringLiteral("description"), description},
        {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                                    {QStringLiteral("properties"), properties},
                                                    {QStringLiteral("required"), required},
                                                    {QStringLiteral("additionalProperties"), false}}},
        {QStringLiteral("annotations"),
         QJsonObject{{QStringLiteral("readOnlyHint"), readOnly}, {QStringLiteral("destructiveHint"), !readOnly}, {QStringLiteral("openWorldHint"), false}}}};
}
} // namespace Mcp
