/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Native user-template files are outside project undo. Every created test ID has a unique owned prefix.
#include "mcp/mcpprofiles.h"
#include "profiles/profilemodel.hpp"
#include "profiles/profilerepository.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QUuid>

namespace {
QJsonObject customProfileSpec()
{
    return {{QStringLiteral("description"), QStringLiteral("MCP native portrait 29.97 – פרופיל")},
            {QStringLiteral("width"), 1080},
            {QStringLiteral("height"), 1920},
            {QStringLiteral("frameRateNumerator"), 30000},
            {QStringLiteral("frameRateDenominator"), 1001},
            {QStringLiteral("progressive"), true},
            {QStringLiteral("bottomFieldFirst"), false},
            {QStringLiteral("sampleAspectNumerator"), 1},
            {QStringLiteral("sampleAspectDenominator"), 1},
            {QStringLiteral("displayAspectNumerator"), 9},
            {QStringLiteral("displayAspectDenominator"), 16},
            {QStringLiteral("colorspace"), 709}};
}
QByteArray customProfileRead(const QString &path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
struct OwnedCustomProfile
{
    const QString directory{QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/profiles"))};
    const QString leaf{QStringLiteral("mcp-native-profile-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces)};
    const QString id{QDir(directory).absoluteFilePath(leaf)};
    ~OwnedCustomProfile()
    {
        // Never sweep the test/user profile directory, default template, or another test's files.
        if (QStandardPaths::isTestModeEnabled() && QFileInfo(id).absolutePath() == directory &&
            QFileInfo(id).fileName().startsWith(QStringLiteral("mcp-native-profile-test-")) && pCore->getCurrentProfilePath() != id && QFileInfo::exists(id))
            ProfileRepository::get()->deleteProfile(id);
    }
};
QJsonObject createCustomProfile(McpProject &project, const OwnedCustomProfile &owned)
{
    auto args = project.expected();
    args.insert(QStringLiteral("fileName"), owned.leaf);
    args.insert(QStringLiteral("specification"), customProfileSpec());
    return args;
}
QJsonObject replaceCustomProfile(McpProject &project, const QString &id)
{
    auto args = project.expected();
    const auto actual =
        project.success(QStringLiteral("get_profile_template"), {{QStringLiteral("profileId"), id}}).value(QStringLiteral("profile")).toObject();
    args.insert(QStringLiteral("profileId"), id);
    args.insert(QStringLiteral("specification"), actual.value(QStringLiteral("specification")));
    args.insert(QStringLiteral("expectedProfileHash"), actual.value(QStringLiteral("templateHash")));
    args.insert(QStringLiteral("overwrite"), true);
    args.insert(QStringLiteral("acknowledgeTemplateChange"), true);
    return args;
}
} // namespace

TEST_CASE("MCP custom profile templates preserve exact native rational fields and leave project timing unchanged", "[MCP][MCPProfiles]")
{
    REQUIRE(QStandardPaths::isTestModeEnabled());
    McpProject project;
    project.open();
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 3, 40));
    const auto before = project.state();
    const auto currentId = pCore->getCurrentProfilePath();
    const int undoCount = project.undoStack->count();
    const int undoIndex = project.undoStack->index();
    OwnedCustomProfile owned;
    const auto saved = project.success(QStringLiteral("save_custom_profile"), createCustomProfile(project, owned));
    CHECK(saved.value(QStringLiteral("created")).toBool());
    CHECK_FALSE(saved.value(QStringLiteral("undoable")).toBool(true));
    CHECK_FALSE(saved.value(QStringLiteral("projectChanged")).toBool(true));
    const auto item = saved.value(QStringLiteral("profile")).toObject();
    REQUIRE(item.value(QStringLiteral("id")).toString() == owned.id);
    CHECK(item.value(QStringLiteral("editable")).toBool());
    CHECK(item.value(QStringLiteral("repositoryMatchesFile")).toBool());
    CHECK(item.value(QStringLiteral("templateHash")).toString().size() == 64);
    CHECK(item.value(QStringLiteral("specification")).toObject() == customProfileSpec());
    const auto &native = ProfileRepository::get()->getProfile(owned.id);
    REQUIRE(native);
    REQUIRE(native->is_valid());
    CHECK(native->fps() == Approx(30000.0 / 1001));
    CHECK(native->frame_rate_num() == 30000);
    CHECK(native->frame_rate_den() == 1001);
    CHECK(native->width() == 1080);
    CHECK(native->height() == 1920);
    CHECK(native->display_aspect_num() == 9);
    CHECK(native->display_aspect_den() == 16);
    CHECK(native->sample_aspect_num() == 1);
    CHECK(native->sample_aspect_den() == 1);
    CHECK(customProfileRead(owned.id).contains(customProfileSpec().value(QStringLiteral("description")).toString().toUtf8()));
    ProfileRepository::get()->refresh();
    const auto reread =
        project.success(QStringLiteral("get_profile_template"), {{QStringLiteral("profileId"), owned.id}}).value(QStringLiteral("profile")).toObject();
    CHECK(reread == item);
    bool found = false;
    int offset = 0;
    while (!found) {
        const auto page = project.success(QStringLiteral("get_custom_profiles"), {{QStringLiteral("offset"), offset}, {QStringLiteral("limit"), 64}});
        const auto entries = page.value(QStringLiteral("profiles")).toArray();
        for (const auto &entry : entries)
            if (entry.toObject().value(QStringLiteral("id")) == owned.id) found = true;
        if (!page.value(QStringLiteral("hasMore")).toBool()) break;
        REQUIRE_FALSE(entries.isEmpty());
        offset += int(entries.size());
    }
    CHECK(found);
    CHECK(pCore->getCurrentProfilePath() == currentId);
    CHECK(project.undoStack->count() == undoCount);
    CHECK(project.undoStack->index() == undoIndex);
    CHECK(project.state() == before);
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP custom profile replacement refreshes the native cache and refuses stale or unacknowledged changes", "[MCP][MCPProfiles]")
{
    REQUIRE(QStandardPaths::isTestModeEnabled());
    McpProject project;
    project.open();
    OwnedCustomProfile owned;
    project.success(QStringLiteral("save_custom_profile"), createCustomProfile(project, owned));
    const auto original = customProfileRead(owned.id);
    project.rejectWithoutChange(QStringLiteral("save_custom_profile"), createCustomProfile(project, owned));
    CHECK(customProfileRead(owned.id) == original);
    auto args = replaceCustomProfile(project, owned.id);
    auto next = customProfileSpec();
    next[QStringLiteral("description")] = QStringLiteral("Native interlaced 59.94");
    next[QStringLiteral("width")] = 1920;
    next[QStringLiteral("height")] = 1080;
    next[QStringLiteral("frameRateNumerator")] = 60000;
    next[QStringLiteral("frameRateDenominator")] = 1001;
    next[QStringLiteral("displayAspectNumerator")] = 16;
    next[QStringLiteral("displayAspectDenominator")] = 9;
    next[QStringLiteral("progressive")] = false;
    next[QStringLiteral("bottomFieldFirst")] = true;
    next[QStringLiteral("colorspace")] = 2020;
    args[QStringLiteral("specification")] = next;
    args[QStringLiteral("acknowledgeTemplateChange")] = false;
    project.rejectWithoutChange(QStringLiteral("save_custom_profile"), args);
    CHECK(customProfileRead(owned.id) == original);
    args[QStringLiteral("acknowledgeTemplateChange")] = true;
    args[QStringLiteral("expectedProfileHash")] = QStringLiteral("stale");
    project.rejectWithoutChange(QStringLiteral("save_custom_profile"), args);
    CHECK(customProfileRead(owned.id) == original);
    args = replaceCustomProfile(project, owned.id);
    args[QStringLiteral("specification")] = next;
    const auto replaced = project.success(QStringLiteral("save_custom_profile"), args).value(QStringLiteral("profile")).toObject();
    CHECK(replaced.value(QStringLiteral("repositoryMatchesFile")).toBool());
    CHECK(replaced.value(QStringLiteral("specification")).toObject() == next);
    const auto &native = ProfileRepository::get()->getProfile(owned.id);
    REQUIRE(native);
    CHECK(native->width() == 1920);
    CHECK(native->height() == 1080);
    CHECK(native->fps() == Approx(60000.0 / 1001));
    CHECK_FALSE(native->progressive());
    CHECK(native->bottom_field_first());
    CHECK(native->colorspace() == 2020);
    auto stale = replaceCustomProfile(project, owned.id);
    {
        QFile concurrent(owned.id);
        REQUIRE(concurrent.open(QIODevice::WriteOnly | QIODevice::Append));
        REQUIRE(concurrent.write("# external test change\n") > 0);
        concurrent.close();
    }
    const auto changed = customProfileRead(owned.id);
    project.rejectWithoutChange(QStringLiteral("save_custom_profile"), stale);
    CHECK(customProfileRead(owned.id) == changed);
    auto remove = project.expected();
    remove.insert(QStringLiteral("profileId"), owned.id);
    remove.insert(QStringLiteral("expectedProfileHash"), project.success(QStringLiteral("get_profile_template"), {{QStringLiteral("profileId"), owned.id}})
                                                             .value(QStringLiteral("profile"))
                                                             .toObject()
                                                             .value(QStringLiteral("templateHash")));
    remove.insert(QStringLiteral("acknowledgeTemplateChange"), false);
    project.rejectWithoutChange(QStringLiteral("delete_custom_profile"), remove);
    CHECK(customProfileRead(owned.id) == changed);
    remove[QStringLiteral("acknowledgeTemplateChange")] = true;
    const auto deleted = project.success(QStringLiteral("delete_custom_profile"), remove);
    CHECK(deleted.value(QStringLiteral("deleted")).toBool());
    CHECK_FALSE(deleted.value(QStringLiteral("undoable")).toBool(true));
    CHECK_FALSE(QFileInfo::exists(owned.id));
    CHECK_FALSE(ProfileRepository::get()->profileExists(owned.id));
}

TEST_CASE("MCP custom profiles protect builtin and active templates and validate native fields before publication", "[MCP][MCPProfiles]")
{
    REQUIRE(QStandardPaths::isTestModeEnabled());
    McpProject project;
    project.open();
    OwnedCustomProfile owned;
    for (const auto &field : {QStringLiteral("width"), QStringLiteral("frameRateDenominator"), QStringLiteral("sampleAspectDenominator")}) {
        auto args = createCustomProfile(project, owned);
        auto spec = customProfileSpec();
        spec[field] = field == QStringLiteral("width") ? 1081 : 0;
        args[QStringLiteral("specification")] = spec;
        project.rejectWithoutChange(QStringLiteral("save_custom_profile"), args);
        CHECK_FALSE(QFileInfo::exists(owned.id));
    }
    auto invalid = createCustomProfile(project, owned);
    auto spec = customProfileSpec();
    spec[QStringLiteral("description")] = QStringLiteral("Injected\nwidth=2");
    invalid[QStringLiteral("specification")] = spec;
    project.rejectWithoutChange(QStringLiteral("save_custom_profile"), invalid);
    CHECK_FALSE(QFileInfo::exists(owned.id));
    invalid = createCustomProfile(project, owned);
    invalid[QStringLiteral("fileName")] = QStringLiteral("../escape");
    project.rejectWithoutChange(QStringLiteral("save_custom_profile"), invalid);
    CHECK_FALSE(QFileInfo::exists(owned.id));
    const auto builtin = project.success(QStringLiteral("get_profile_template"), {{QStringLiteral("profileId"), QStringLiteral("dv_pal")}})
                             .value(QStringLiteral("profile"))
                             .toObject();
    CHECK_FALSE(builtin.value(QStringLiteral("editable")).toBool());
    auto remove = project.expected();
    remove[QStringLiteral("profileId")] = QStringLiteral("dv_pal");
    remove[QStringLiteral("expectedProfileHash")] = builtin.value(QStringLiteral("templateHash"));
    remove[QStringLiteral("acknowledgeTemplateChange")] = true;
    project.rejectWithoutChange(QStringLiteral("delete_custom_profile"), remove);
    REQUIRE(ProfileRepository::get()->profileExists(QStringLiteral("dv_pal")));
    project.success(QStringLiteral("save_custom_profile"), createCustomProfile(project, owned));
    const QString previous = pCore->getCurrentProfilePath();
    const auto reset = qScopeGuard([previous] { pCore->setCurrentProfile(previous); });
    REQUIRE(pCore->setCurrentProfile(owned.id)); // Test fixture only; public conversion remains the lifecycle tool's responsibility.
    const auto bytes = customProfileRead(owned.id);
    project.rejectWithoutChange(QStringLiteral("save_custom_profile"), replaceCustomProfile(project, owned.id));
    remove = project.expected();
    remove[QStringLiteral("profileId")] = owned.id;
    remove[QStringLiteral("expectedProfileHash")] = project.success(QStringLiteral("get_profile_template"), {{QStringLiteral("profileId"), owned.id}})
                                                        .value(QStringLiteral("profile"))
                                                        .toObject()
                                                        .value(QStringLiteral("templateHash"));
    remove[QStringLiteral("acknowledgeTemplateChange")] = true;
    project.rejectWithoutChange(QStringLiteral("delete_custom_profile"), remove);
    CHECK(customProfileRead(owned.id) == bytes);
    CHECK(pCore->getCurrentProfilePath() == owned.id);
}

TEST_CASE("Native automated profile saves return errors and never replace an existing new-file target", "[MCP][MCPProfiles][NativeIO]")
{
    McpProject project;
    project.open();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto target = directory.filePath(QStringLiteral("sentinel-profile"));
    const QByteArray bytes("owned sentinel profile\n");
    {
        QFile file(target);
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        REQUIRE(file.write(bytes) == bytes.size());
    }
    ProfileParam profile(pCore->getCurrentProfile().get());
    QString error;
    const auto published = directory.filePath(QStringLiteral("new-profile"));
    REQUIRE(ProfileRepository::get()->saveProfile(&profile, published, &error, true) == published);
    CHECK(error.isEmpty());
    CHECK(customProfileRead(published).contains("description="));
    ProfileModel reloaded(published);
    CHECK(reloaded.is_valid());
    CHECK(reloaded.width() == profile.width());
    CHECK(reloaded.frame_rate_num() == profile.frame_rate_num());
    CHECK(QDir(directory.path()).entryList({QStringLiteral(".kdenlive-profile-*")}, QDir::Files | QDir::Hidden).isEmpty());
    REQUIRE(ProfileRepository::get()->deleteProfile(published));
    CHECK_FALSE(QFileInfo::exists(published));
    CHECK(ProfileRepository::get()->saveProfile(&profile, target, &error, true).isEmpty());
    CHECK_FALSE(error.isEmpty());
    CHECK(customProfileRead(target) == bytes);
    CHECK(QDir(directory.path()).entryList({QStringLiteral(".kdenlive-profile-*")}, QDir::Files | QDir::Hidden).isEmpty());
    CHECK_FALSE(QApplication::activeModalWidget());
    error.clear();
    CHECK(ProfileRepository::get()->saveProfile(&profile, directory.filePath(QStringLiteral("missing/profile")), &error, true).isEmpty());
    CHECK_FALSE(error.isEmpty());
    CHECK_FALSE(QApplication::activeModalWidget());
}
