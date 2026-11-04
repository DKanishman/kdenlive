/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcpprofiles.h"
#include "core.h"
#include "kdenlivesettings.h"
#include "mainwindow.h"
#include "mcphelpers.h"
#include "profiles/profilemodel.hpp"
#include "profiles/profilerepository.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace {
constexpr qint64 MaxProfileBytes = 65536;
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
QString directory()
{
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base.isEmpty() ? QString() : QDir::cleanPath(base + QStringLiteral("/profiles"));
}
bool boundedText(const QJsonValue &value, QString &result, int maximum = 4096)
{
    if (!value.isString()) return false;
    result = value.toString();
    return !result.isEmpty() && result.size() <= maximum && !result.contains(QChar::Null);
}
bool safeLeaf(const QString &name)
{
    static const QRegularExpression invalid(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f\\x7f]"));
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption);
    return !name.isEmpty() && name.size() <= 100 && !name.startsWith(QLatin1Char('.')) && !name.endsWith(QLatin1Char('.')) &&
           !name.endsWith(QLatin1Char(' ')) && QFileInfo(name).fileName() == name && !invalid.match(name).hasMatch() && !reserved.match(name).hasMatch();
}
QString templateHash(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 || info.size() > MaxProfileBytes) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.read(MaxProfileBytes + 1);
    if (file.error() != QFile::NoError || bytes.size() != info.size()) return {};
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
bool editableFile(const QString &path)
{
    const QFileInfo info(path);
    const auto customDirectory = directory();
    if (customDirectory.isEmpty()) return false;
    const auto writableDirectory = QDir(customDirectory).canonicalPath();
    return QDir::isAbsolutePath(path) && !writableDirectory.isEmpty() && info.absoluteDir().canonicalPath() == writableDirectory && info.isFile() &&
           !info.isSymLink() && info.isWritable() && safeLeaf(info.fileName());
}
bool protectedTemplate(const QString &path)
{
    const auto same = [&path](const QString &candidate) {
        if (path == candidate) return true;
        if (!QDir::isAbsolutePath(candidate)) return false;
        return QFileInfo(path).canonicalFilePath() == QFileInfo(candidate).canonicalFilePath();
    };
    return pCore && (same(pCore->getCurrentProfilePath()) || same(KdenliveSettings::default_profile()));
}
QJsonObject specification(const ProfileInfo &profile)
{
    return {{k("description"), profile.description()},
            {k("width"), profile.width()},
            {k("height"), profile.height()},
            {k("frameRateNumerator"), profile.frame_rate_num()},
            {k("frameRateDenominator"), profile.frame_rate_den()},
            {k("progressive"), profile.progressive()},
            {k("bottomFieldFirst"), profile.bottom_field_first()},
            {k("sampleAspectNumerator"), profile.sample_aspect_num()},
            {k("sampleAspectDenominator"), profile.sample_aspect_den()},
            {k("displayAspectNumerator"), profile.display_aspect_num()},
            {k("displayAspectDenominator"), profile.display_aspect_den()},
            {k("colorspace"), profile.colorspace()}};
}
QJsonObject describe(const QString &id)
{
    const auto &cached = ProfileRepository::get()->getProfile(id);
    const QString filePath = QDir::isAbsolutePath(id) ? id : QDir(KdenliveSettings::mltpath()).absoluteFilePath(id);
    const auto hash = templateHash(filePath);
    ProfileModel actual(id);
    const bool valid = actual.is_valid();
    QJsonObject result{{k("id"), id},
                       {k("fileName"), QFileInfo(filePath).fileName()},
                       {k("templateHash"), hash},
                       {k("editable"), editableFile(id)},
                       {k("protected"), protectedTemplate(id)},
                       {k("valid"), valid}};
    if (valid) {
        result.insert(k("specification"), specification(actual));
        result.insert(k("repositoryMatchesFile"), cached && specification(*cached) == specification(actual));
    }
    return result;
}
bool validSpec(const QJsonObject &object, ProfileParam &profile)
{
    static const QSet<QString> keys{k("description"),
                                    k("width"),
                                    k("height"),
                                    k("frameRateNumerator"),
                                    k("frameRateDenominator"),
                                    k("progressive"),
                                    k("bottomFieldFirst"),
                                    k("sampleAspectNumerator"),
                                    k("sampleAspectDenominator"),
                                    k("displayAspectNumerator"),
                                    k("displayAspectDenominator"),
                                    k("colorspace")};
    for (auto it = object.cbegin(); it != object.cend(); ++it)
        if (!keys.contains(it.key())) return false;
    QString description;
    if (!boundedText(object.value(k("description")), description, 256) ||
        QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")).match(description).hasMatch() ||
        !Mcp::integer(object, k("width"), 2, 10000, profile.m_width) || !Mcp::integer(object, k("height"), 2, 10000, profile.m_height) || profile.m_width % 2 ||
        profile.m_height % 2 || !Mcp::integer(object, k("frameRateNumerator"), 1, 500000, profile.m_frame_rate_num) ||
        !Mcp::integer(object, k("frameRateDenominator"), 1, 10000, profile.m_frame_rate_den) ||
        !Mcp::integer(object, k("sampleAspectNumerator"), 1, 1000000000, profile.m_sample_aspect_num) ||
        !Mcp::integer(object, k("sampleAspectDenominator"), 1, 1000000000, profile.m_sample_aspect_den) ||
        !Mcp::integer(object, k("displayAspectNumerator"), 1, 10000, profile.m_display_aspect_num) ||
        !Mcp::integer(object, k("displayAspectDenominator"), 1, 10000, profile.m_display_aspect_den) ||
        !Mcp::integer(object, k("colorspace"), 0, 2021, profile.m_colorspace) ||
        !QSet<int>{0, 170, 240, 470, 601, 709, 2020, 2021}.contains(profile.m_colorspace) || !object.value(k("progressive")).isBool() ||
        (object.contains(k("bottomFieldFirst")) && !object.value(k("bottomFieldFirst")).isBool()))
        return false;
    profile.m_description = description;
    profile.m_progressive = object.value(k("progressive")).toBool();
    profile.m_bottom_field_first = object.value(k("bottomFieldFirst")).toBool(false);
    profile.m_fps = double(profile.m_frame_rate_num) / profile.m_frame_rate_den;
    profile.m_sar = double(profile.m_sample_aspect_num) / profile.m_sample_aspect_den;
    profile.m_dar = double(profile.m_display_aspect_num) / profile.m_display_aspect_den;
    return profile.is_valid();
}
void refreshGui()
{
    if (pCore && pCore->window()) pCore->window()->slotRefreshProfiles();
}
} // namespace
namespace McpProfiles {
QJsonArray toolCatalog()
{
    const auto expected = text("Fresh expectedState from get_state; template files additionally use their own templateHash.");
    const auto id = text("Exact native profile ID from list_profiles/get_custom_profiles.");
    const auto hash = text("Exact templateHash from get_profile_template; required for overwrite/deletion.");
    QJsonObject fields{{k("description"), text("Single-line native display description, 1-256 characters.")},
                       {k("width"), integer("Exact even pixel width 2-10000, matching the native profile editor.")},
                       {k("height"), integer("Exact even pixel height 2-10000.")},
                       {k("frameRateNumerator"), integer("Positive rational frame-rate numerator, 1-500000.")},
                       {k("frameRateDenominator"), integer("Positive rational frame-rate denominator, 1-10000.")},
                       {k("progressive"), boolean("Progressive scanning when true; interlaced when false.")},
                       {k("bottomFieldFirst"), boolean("Native interlace field order, default false.")},
                       {k("sampleAspectNumerator"), integer("Positive pixel-aspect numerator, at most 1000000000.")},
                       {k("sampleAspectDenominator"), integer("Positive pixel-aspect denominator, at most 1000000000.")},
                       {k("displayAspectNumerator"), integer("Positive display-aspect numerator, at most 10000.")},
                       {k("displayAspectDenominator"), integer("Positive display-aspect denominator, at most 10000.")},
                       {k("colorspace"), integer("Native colorspace code: 0, 170, 240, 470, 601, 709, 2020, 2021.")}};
    QJsonArray required;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it)
        if (it.key() != k("bottomFieldFirst")) required.append(it.key());
    QJsonObject spec{{k("type"), k("object")},
                     {k("description"), k("Exact native profile specification; rational values are preserved.")},
                     {k("properties"), fields},
                     {k("required"), required},
                     {k("additionalProperties"), false}};
    return {Mcp::tool(k("get_profile_template"), k("Read native profile fields, file version and whether the user-custom template can be edited safely."),
                      {{k("profileId"), id}}, {k("profileId")}, true),
            Mcp::tool(k("get_custom_profiles"),
                      k("Page native templates stored in the writable user-custom profile directory. File changes are separate from timeline undo."),
                      {{k("offset"), integer("Page offset, default zero.")}, {k("limit"), integer("Page length 1-64, default 32.")}}, {}, true),
            Mcp::tool(k("save_custom_profile"),
                      k("Create a native custom profile template or explicitly replace an inactive user-custom one. Atomic file publication; does not change "
                        "the active project. Template changes are not timeline-undoable."),
                      {{k("expectedState"), expected},
                       {k("specification"), spec},
                       {k("fileName"), text("New template only: safe leaf filename, maximum 100 characters; generated when omitted.")},
                       {k("profileId"), id},
                       {k("overwrite"), boolean("Replacing an existing profile additionally requires acknowledgement and its exact file hash.")},
                       {k("expectedProfileHash"), hash},
                       {k("acknowledgeTemplateChange"), boolean("Explicitly acknowledge replacing this global user template, outside project undo history.")}},
                      {k("expectedState"), k("specification")}),
            Mcp::tool(k("delete_custom_profile"),
                      k("Delete an inactive user-custom template with an exact file-version check and acknowledgement. Built-in/current/default templates are "
                        "protected; not timeline-undoable."),
                      {{k("expectedState"), expected},
                       {k("profileId"), id},
                       {k("expectedProfileHash"), hash},
                       {k("acknowledgeTemplateChange"), boolean("Explicit acknowledgement of permanent template deletion outside project undo.")}},
                      {k("expectedState"), k("profileId"), k("expectedProfileHash"), k("acknowledgeTemplateChange")})};
}
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState,
                     const std::function<QJsonObject()> &stateReader)
{
    bool known = false;
    for (const auto &tool : toolCatalog())
        if (tool.toObject().value(k("name")) == name) {
            known = true;
            break;
        }
    if (!known) return {};
    if (!owner || !qApp || owner->thread() != qApp->thread() || QThread::currentThread() != qApp->thread())
        return Mcp::failure(k("GUI_THREAD_REQUIRED"), k("Native profile operations run on the owning GUI thread."), nativeState);
    auto reject = [&](const char *code, const char *message) { return Mcp::failure(k(code), k(message), nativeState); };
    auto finish = [&](QJsonObject result) {
        result.insert(k("ok"), true);
        result.insert(k("state"), stateReader ? stateReader() : nativeState);
        return Mcp::toolResult(result);
    };
    if (name == k("get_custom_profiles")) {
        int offset = 0, limit = 32;
        if ((arguments.contains(k("offset")) && !Mcp::integer(arguments, k("offset"), 0, 1000000, offset)) ||
            (arguments.contains(k("limit")) && !Mcp::integer(arguments, k("limit"), 1, 64, limit)))
            return reject("INVALID_ARGUMENT", "Use a bounded native custom-profile page.");
        QStringList ids;
        for (const auto &entry : ProfileRepository::get()->getAllProfiles())
            if (editableFile(entry.second)) ids.append(entry.second);
        std::sort(ids.begin(), ids.end());
        QJsonArray page;
        for (int index = offset; index < ids.size() && page.size() < limit; ++index)
            page.append(describe(ids[index]));
        return finish(
            {{k("profiles"), page}, {k("directory"), directory()}, {k("total"), ids.size()}, {k("hasMore"), qint64(offset) + page.size() < ids.size()}});
    }
    QString id;
    if (name == k("get_profile_template")) {
        if (!boundedText(arguments.value(k("profileId")), id) || !ProfileRepository::get()->profileExists(id))
            return reject("INVALID_PROFILE", "Choose an existing native profile ID.");
        return finish({{k("profile"), describe(id)}});
    }
    if (!nativeState.value(k("ready")).toBool() || !pCore) return reject("NOT_READY", "Wait for a ready native project before changing profile templates.");
    if (arguments.value(k("expectedState")).toString().isEmpty() || arguments.value(k("expectedState")) != nativeState.value(k("expectedState")))
        return reject("STALE_STATE", "Refresh get_state before changing native profile templates.");
    const bool replacing = name == k("delete_custom_profile") || arguments.contains(k("profileId"));
    if (replacing) {
        if (!boundedText(arguments.value(k("profileId")), id) || !ProfileRepository::get()->profileExists(id) || !editableFile(id))
            return reject("PROFILE_READ_ONLY", "Only an existing writable user-custom profile can be replaced or deleted; built-in profiles are protected.");
        if (protectedTemplate(id))
            return reject("ACTIVE_PROFILE_TEMPLATE",
                          "Create a separate template or select a different current/default project profile before changing this template.");
        const auto actualHash = templateHash(id);
        if (actualHash.isEmpty()) return reject("PROFILE_UNAVAILABLE", "The native template is missing, unreadable or exceeds 64 KiB.");
        if (!arguments.value(k("expectedProfileHash")).isString() || arguments.value(k("expectedProfileHash")).toString() != actualHash)
            return reject("STALE_PROFILE", "Refresh get_profile_template before replacing or deleting this template.");
        if (!arguments.value(k("acknowledgeTemplateChange")).isBool() || !arguments.value(k("acknowledgeTemplateChange")).toBool())
            return reject("ACKNOWLEDGEMENT_REQUIRED", "Acknowledge this user-template change outside the project's undo history.");
    }
    if (name == k("delete_custom_profile")) {
        if (!ProfileRepository::get()->deleteProfile(id)) return reject("DELETE_FAILED", "Native profile deletion failed; inspect the current template.");
        refreshGui();
        return finish({{k("profileId"), id}, {k("deleted"), true}, {k("undoable"), false}, {k("projectChanged"), false}});
    }
    if (!arguments.value(k("specification")).isObject()) return reject("INVALID_PROFILE", "Supply the exact structured native profile fields.");
    ProfileParam profile(pCore->getCurrentProfile().get());
    if (!validSpec(arguments.value(k("specification")).toObject(), profile))
        return reject("INVALID_PROFILE", "Use bounded native profile fields, even dimensions, positive rational denominators and a single-line description.");
    if (arguments.contains(k("overwrite")) && !arguments.value(k("overwrite")).isBool()) return reject("INVALID_ARGUMENT", "overwrite must be boolean.");
    if (replacing) {
        if (arguments.contains(k("fileName")) || !arguments.value(k("overwrite")).toBool(false))
            return reject("OUTPUT_EXISTS", "Replace by exact profileId with overwrite=true; fileName is only for new templates.");
    } else {
        if (arguments.value(k("overwrite")).toBool(false) || arguments.contains(k("expectedProfileHash")) || arguments.contains(k("acknowledgeTemplateChange")))
            return reject("INVALID_ARGUMENT", "For replacement supply the existing native profileId, overwrite, acknowledgement and exact template hash.");
        const QString leaf = arguments.contains(k("fileName")) ? arguments.value(k("fileName")).toString()
                                                               : QStringLiteral("mcp-profile-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!safeLeaf(leaf)) return reject("INVALID_PATH", "Use a plain native custom-profile filename without separators, controls or reserved names.");
        if (directory().isEmpty() || !QDir().mkpath(directory())) return reject("WRITE_FAILED", "Cannot create the native writable custom-profile directory.");
        id = QDir(directory()).absoluteFilePath(leaf);
        if (QFileInfo::exists(id) || QFileInfo(id).isSymLink())
            return reject("OUTPUT_EXISTS", "This template filename already exists; choose a new name or replace by native ID and hash.");
    }
    QString error;
    const QString saved = ProfileRepository::get()->saveProfile(&profile, id, &error, !replacing);
    if (saved.isEmpty()) return Mcp::failure(k("WRITE_FAILED"), error.isEmpty() ? k("Native profile publication failed.") : error, nativeState);
    const auto savedFailure = [&](const char *code, const char *message) {
        auto data = Mcp::failure(k(code), k(message), nativeState).value(k("structuredContent")).toObject();
        data.insert(k("profileId"), saved);
        data.insert(k("filePreserved"), true);
        data.insert(k("templateHash"), templateHash(saved));
        return Mcp::toolResult(data, true);
    };
    if (!ProfileRepository::get()->profileExists(saved))
        return savedFailure("PROFILE_RELOAD_FAILED", "The profile was saved but could not be registered by the native repository; its file was preserved.");
    const auto result = describe(saved);
    if (!result.value(k("valid")).toBool() || result.value(k("specification")).toObject() != specification(profile) ||
        !result.value(k("repositoryMatchesFile")).toBool())
        return savedFailure("PROFILE_VERIFY_FAILED", "Native saved-profile readback differs from the requested fields; inspect the preserved template.");
    refreshGui();
    return finish({{k("profile"), result}, {k("created"), !replacing}, {k("replaced"), replacing}, {k("undoable"), false}, {k("projectChanged"), false}});
}
} // namespace McpProfiles
