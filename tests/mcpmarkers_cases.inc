/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Actual native marker models, files, undo and native preferences; no parser mocks.
#include "kdenlivesettings.h"
#include "mcp/mcpmarkers.h"
#include <QCryptographicHash>
#include <QProcess>
#include <QScopeGuard>

namespace {
QJsonObject markerFileFields(McpProject &project, const QJsonObject &fields)
{
    auto arguments = project.expected();
    for (auto it = fields.begin(); it != fields.end(); ++it)
        arguments.insert(it.key(), it.value());
    return arguments;
}
QByteArray markerFileBytes(const QString &path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
void writeMarkerFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    REQUIRE(file.write(bytes) == bytes.size());
}
QString markerFileSha(const QString &path)
{
    return QString::fromLatin1(QCryptographicHash::hash(markerFileBytes(path), QCryptographicHash::Sha256).toHex());
}
struct RestoreMarkerPreferences
{
    const QString pattern = KdenliveSettings::exportGuidesFormat();
    const QStringList categories = KdenliveSettings::guidesCategories();
    const int category = KdenliveSettings::default_marker_type();
    ~RestoreMarkerPreferences()
    {
        KdenliveSettings::setExportGuidesFormat(pattern);
        KdenliveSettings::setGuidesCategories(categories);
        KdenliveSettings::setDefault_marker_type(category);
        Q_EMIT pCore->updateDefaultMarkerCategory();
    }
};
QJsonObject markerFileImport(McpProject &project, const QString &path, const QString &format, const QString &scope = QStringLiteral("guides"),
                             const QString &bin = {})
{
    auto args = markerFileFields(project, {{QStringLiteral("operation"), QStringLiteral("import")},
                                           {QStringLiteral("scope"), scope},
                                           {QStringLiteral("path"), path},
                                           {QStringLiteral("format"), format},
                                           {QStringLiteral("expectedFileHash"), markerFileSha(path)}});
    if (!bin.isEmpty()) args.insert(QStringLiteral("binId"), bin);
    if (format == QStringLiteral("text"))
        args.insert(
            QStringLiteral("expectedSettings"),
            project.success(QStringLiteral("get_marker_settings")).value(QStringLiteral("settings")).toObject().value(QStringLiteral("expectedSettings")));
    return args;
}
} // namespace

TEST_CASE("MCP native JSON marker files retain Unicode point and range frames through one undo", "[MCP][MCPMarkers][files]")
{
    McpProject project;
    project.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto model = project.timeline->getGuideModel();
    REQUIRE(model);
    const int category = pCore->markerTypes.firstKey();
    const QString text = QString::fromUtf8("פרק ראשון – marker");
    REQUIRE(model->addMarker(GenTime(7, 25), text, category));
    REQUIRE(model->addRangeMarker(GenTime(37, 25), GenTime(6, 25), QStringLiteral("range"), category));
    const auto original = model->toJson();
    const int history = project.undoStack->index();
    const auto token = project.token();
    const auto path = files.filePath(QStringLiteral("guide-roundtrip.json"));
    const auto exported = project.success(QStringLiteral("marker_file"), markerFileFields(project, {{QStringLiteral("operation"), QStringLiteral("export")},
                                                                                                    {QStringLiteral("scope"), QStringLiteral("guides")},
                                                                                                    {QStringLiteral("path"), path},
                                                                                                    {QStringLiteral("format"), QStringLiteral("json")}}));
    CHECK(project.undoStack->index() == history);
    CHECK(project.token() == token);
    CHECK(exported.value(QStringLiteral("fileHash")).toString() == markerFileSha(path));
    const auto inspected = project.success(QStringLiteral("inspect_marker_file"), {{QStringLiteral("scope"), QStringLiteral("guides")},
                                                                                   {QStringLiteral("path"), path},
                                                                                   {QStringLiteral("format"), QStringLiteral("json")},
                                                                                   {QStringLiteral("offset"), 1},
                                                                                   {QStringLiteral("limit"), 1}});
    CHECK(inspected.value(QStringLiteral("fileHash")) == exported.value(QStringLiteral("fileHash")));
    CHECK(inspected.value(QStringLiteral("markerCount")).toInt() == 2);
    REQUIRE(inspected.value(QStringLiteral("markers")).toArray().size() == 1);
    CHECK(inspected.value(QStringLiteral("markers")).toArray().at(0).toObject().value(QStringLiteral("pos")).toInt() == 37);
    CHECK_FALSE(inspected.value(QStringLiteral("hasMore")).toBool(true));
    const auto disk = QJsonDocument::fromJson(markerFileBytes(path)).array();
    REQUIRE(disk.size() == 2);
    CHECK(disk.at(0).toObject().value(QStringLiteral("pos")).toInt() == 7);
    CHECK(disk.at(0).toObject().value(QStringLiteral("comment")).toString() == text);
    CHECK(disk.at(1).toObject().value(QStringLiteral("duration")).toInt() == 6);
    REQUIRE(model->removeAllMarkers());
    REQUIRE(model->rowCount() == 0);
    const int before = project.undoStack->index();
    const auto imported = project.success(QStringLiteral("marker_file"), markerFileImport(project, path, QStringLiteral("json")));
    CHECK(imported.value(QStringLiteral("importedCount")).toInt() == 2);
    CHECK(project.undoStack->index() == before + 1);
    CHECK(model->toJson() == original);
    CHECK(model->hasMarker(7));
    CHECK(model->marker(37).duration().frames(25) == 6);
    CHECK(markerFileBytes(path) == QJsonDocument(disk).toJson());
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(model->rowCount() == 0);
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(model->toJson() == original);
    const int after = project.undoStack->index();
    const auto unchanged = project.success(QStringLiteral("marker_file"), markerFileImport(project, path, QStringLiteral("json")));
    CHECK_FALSE(unchanged.value(QStringLiteral("changed")).toBool());
    CHECK(project.undoStack->index() == after);
    REQUIRE(project.timeline->checkConsistency(model->getSnapPoints()));
}

TEST_CASE("MCP native marker text intake preserves fractional timing and range duration", "[MCP][MCPMarkers][text]")
{
    REQUIRE(QStandardPaths::isTestModeEnabled());
    McpProject project;
    project.open();
    RestoreMarkerPreferences restore;
    QTemporaryDir files;
    REQUIRE(files.isValid());
    KdenliveSettings::setDefault_marker_type(pCore->markerTypes.firstKey());
    const auto path = files.filePath(QStringLiteral("chapters.txt"));
    writeMarkerFile(path, QString::fromUtf8("0.28 התחלה\r\n00:01.480 exact range [0.24]\r\n").toUtf8());
    const int before = project.undoStack->index();
    auto stale = markerFileImport(project, path, QStringLiteral("text"));
    KdenliveSettings::setExportGuidesFormat(QStringLiteral("changed native preference {{frame}}"));
    project.rejectWithoutChange(QStringLiteral("marker_file"), stale);
    project.success(QStringLiteral("marker_file"), markerFileImport(project, path, QStringLiteral("text")));
    const auto model = project.timeline->getGuideModel();
    CHECK(project.undoStack->index() == before + 1);
    REQUIRE(model->rowCount() == 2);
    CHECK(model->marker(7).comment() == QString::fromUtf8("התחלה"));
    CHECK(model->marker(37).duration().frames(25) == 6);
    CHECK(model->marker(37).comment() == QStringLiteral("exact range"));
    const auto output = files.filePath(QStringLiteral("native-template.txt"));
    const QString pattern = QStringLiteral("{{index}}|{{frame}}|{{nextframe}}|{{duration}}|{{endframe}}|{{hasrange}}|{{comment}}|{{category}}|{{timecode}}|{{"
                                           "realtimecode}}|{{durationtimecode}}|{{endtimecode}}");
    project.success(QStringLiteral("marker_file"), markerFileFields(project, {{QStringLiteral("operation"), QStringLiteral("export")},
                                                                              {QStringLiteral("scope"), QStringLiteral("guides")},
                                                                              {QStringLiteral("path"), output},
                                                                              {QStringLiteral("format"), QStringLiteral("text")},
                                                                              {QStringLiteral("offsetFrames"), 3},
                                                                              {QStringLiteral("endFrame"), 100},
                                                                              {QStringLiteral("textTemplate"), pattern}}));
    const auto content = QString::fromUtf8(markerFileBytes(output));
    CHECK(content.startsWith(QString::fromUtf8("1|10|40|0|10|false|התחלה|")));
    CHECK(content.contains(QStringLiteral("2|40|103|6|46|true|exact range|")));
    CHECK_FALSE(content.contains(QStringLiteral("{{")));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(model->rowCount() == 0);
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(model->marker(37).duration().frames(25) == 6);
}

TEST_CASE("MCP marker file conflicts require exact content hash and explicit replacement acknowledgement", "[MCP][MCPMarkers][conflict]")
{
    McpProject project;
    project.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto model = project.timeline->getGuideModel();
    const int category = pCore->markerTypes.firstKey();
    REQUIRE(model->addRangeMarker(GenTime(1, 25), GenTime(3, 25), QStringLiteral("original"), category));
    const auto original = model->toJson();
    const int markerId = model->markerIdAtFrame(1);
    const QJsonArray incoming{QJsonObject{{QStringLiteral("pos"), 9}, {QStringLiteral("comment"), QStringLiteral("new")}, {QStringLiteral("type"), category}},
                              QJsonObject{{QStringLiteral("pos"), 1},
                                          {QStringLiteral("comment"), QStringLiteral("replacement")},
                                          {QStringLiteral("type"), category},
                                          {QStringLiteral("duration"), 7}}};
    const auto path = files.filePath(QStringLiteral("replace.json"));
    writeMarkerFile(path, QJsonDocument(incoming).toJson());
    auto args = markerFileImport(project, path, QStringLiteral("json"));
    project.rejectWithoutChange(QStringLiteral("marker_file"), args);
    CHECK(model->toJson() == original);
    args.insert(QStringLiteral("conflictPolicy"), QStringLiteral("replace"));
    project.rejectWithoutChange(QStringLiteral("marker_file"), args);
    args.insert(QStringLiteral("acknowledgeMarkerReplacement"), true);
    const auto oldHash = markerFileSha(path);
    writeMarkerFile(path, QJsonDocument(incoming).toJson() + " ");
    project.rejectWithoutChange(QStringLiteral("marker_file"), args);
    CHECK(model->toJson() == original);
    CHECK(markerFileSha(path) != oldHash);
    args.insert(QStringLiteral("expectedFileHash"), markerFileSha(path));
    args.insert(QStringLiteral("expectedState"), project.token());
    const int before = project.undoStack->index();
    const auto applied = project.success(QStringLiteral("marker_file"), args);
    CHECK(applied.value(QStringLiteral("replacedCount")).toInt() == 1);
    CHECK(project.undoStack->index() == before + 1);
    CHECK(model->marker(1).comment() == QStringLiteral("replacement"));
    CHECK(model->marker(1).duration().frames(25) == 7);
    CHECK(model->markerIdAtFrame(1) == markerId);
    CHECK(model->hasMarker(9));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(model->toJson() == original);
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(model->marker(1).duration().frames(25) == 7);
}

TEST_CASE("MCP marker file preflight rejects every malformed row before model or category edits", "[MCP][MCPMarkers][guards]")
{
    REQUIRE(QStandardPaths::isTestModeEnabled());
    McpProject project;
    project.open();
    RestoreMarkerPreferences restore;
    QTemporaryDir files;
    REQUIRE(files.isValid());
    KdenliveSettings::setDefault_marker_type(pCore->markerTypes.firstKey());
    const auto model = project.timeline->getGuideModel();
    const auto categories = pCore->markerTypes;
    const auto original = model->toJson();
    const auto path = files.filePath(QStringLiteral("invalid.json"));
    const QList<QByteArray> invalid{"[{\"pos\":7},{\"pos\":\"bad\"}]",
                                    "[{\"pos\":7},{\"pos\":7}]",
                                    "[{\"pos\":-1}]",
                                    "[{\"pos\":1.5}]",
                                    "[{\"pos\":7,\"duration\":-1}]",
                                    "[{\"pos\":9999999,\"duration\":5}]",
                                    "[{\"pos\":7,\"type\":999999}]",
                                    "[{\"pos\":7,\"comment\":{}}]",
                                    "[{\"pos\":7,\"other\":true}]",
                                    "[{\"pos\":7},false]"};
    for (const auto &bytes : invalid) {
        writeMarkerFile(path, bytes);
        project.rejectWithoutChange(QStringLiteral("marker_file"), markerFileImport(project, path, QStringLiteral("json")));
        CHECK(model->toJson() == original);
        CHECK(pCore->markerTypes.keys() == categories.keys());
    }
    const auto text = files.filePath(QStringLiteral("invalid.txt"));
    for (const auto &bytes : QList<QByteArray>{"0.28 good\nnot-a-time bad", "0.28 good\n00:01.4 bad [40001]", "0.28 good\n0.280 duplicate", "-1 negative",
                                               QByteArray("0.28 bad \xff")}) {
        writeMarkerFile(text, bytes);
        project.rejectWithoutChange(QStringLiteral("marker_file"), markerFileImport(project, text, QStringLiteral("text")));
        CHECK(model->toJson() == original);
    }
    writeMarkerFile(text, "0.28 literal [note]");
    project.success(QStringLiteral("marker_file"), markerFileImport(project, text, QStringLiteral("text")));
    CHECK(model->marker(7).comment() == QStringLiteral("literal [note]"));
    CHECK(model->marker(7).duration().frames(25) == 0);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(model->toJson() == original);
    writeMarkerFile(path, "[]");
    const int before = project.undoStack->index();
    CHECK_FALSE(
        project.success(QStringLiteral("marker_file"), markerFileImport(project, path, QStringLiteral("json"))).value(QStringLiteral("changed")).toBool());
    CHECK(project.undoStack->index() == before);
}

TEST_CASE("MCP clip marker imports enforce decoded source bounds and preserve guide models", "[MCP][MCPMarkers][clip]")
{
    McpProject project;
    project.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto bin = importTimelineFixture(project, wavFixture(files));
    const auto source = pCore->projectItemModel()->getClipByBinID(bin);
    REQUIRE(source);
    const int length = source->getFramePlaytime();
    REQUIRE(length > 2);
    const int category = pCore->markerTypes.firstKey();
    const auto path = files.filePath(QStringLiteral("clip-markers.json"));
    auto writeEntry = [&](int frame, int duration) {
        writeMarkerFile(path, QJsonDocument(QJsonArray{QJsonObject{{QStringLiteral("pos"), frame},
                                                                   {QStringLiteral("duration"), duration},
                                                                   {QStringLiteral("type"), category},
                                                                   {QStringLiteral("comment"), QStringLiteral("clip end")}}})
                                  .toJson());
    };
    writeEntry(length - 1, 1);
    project.success(QStringLiteral("marker_file"), markerFileImport(project, path, QStringLiteral("json"), QStringLiteral("clip"), bin));
    REQUIRE(source->markerModel()->rowCount() == 1);
    CHECK(source->markerModel()->marker(length - 1).duration().frames(25) == 1);
    CHECK(project.timeline->getGuideModel()->rowCount() == 0);
    writeEntry(length, 0);
    project.rejectWithoutChange(QStringLiteral("marker_file"), markerFileImport(project, path, QStringLiteral("json"), QStringLiteral("clip"), bin));
    writeEntry(length - 1, 2);
    project.rejectWithoutChange(QStringLiteral("marker_file"), markerFileImport(project, path, QStringLiteral("json"), QStringLiteral("clip"), bin));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(source->markerModel()->rowCount() == 0);
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(source->markerModel()->marker(length - 1).duration().frames(25) == 1);
}

TEST_CASE("MCP marker exports publish new files and FFmetadata keeps chapter frame ranges", "[MCP][MCPMarkers][export]")
{
    McpProject project;
    project.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto model = project.timeline->getGuideModel();
    const int category = pCore->markerTypes.firstKey();
    const QString comment = QStringLiteral("title=one;#two\\three\nnext");
    REQUIRE(model->addRangeMarker(GenTime(7, 25), GenTime(6, 25), comment, category));
    REQUIRE(model->addMarker(GenTime(37, 25), QStringLiteral("second"), category));
    const auto path = files.filePath(QStringLiteral("chapters.ffmeta"));
    auto args = markerFileFields(project, {{QStringLiteral("operation"), QStringLiteral("export")},
                                           {QStringLiteral("scope"), QStringLiteral("guides")},
                                           {QStringLiteral("path"), path},
                                           {QStringLiteral("format"), QStringLiteral("ffmetadata")},
                                           {QStringLiteral("offsetFrames"), 3},
                                           {QStringLiteral("endFrame"), 80}});
    const int history = project.undoStack->index();
    const auto token = project.token();
    project.success(QStringLiteral("marker_file"), args);
    const auto bytes = markerFileBytes(path);
    CHECK(bytes.startsWith(";FFMETADATA1\n"));
    CHECK(bytes.contains("TIMEBASE=1/25\nSTART=10\nEND=16\n"));
    CHECK(bytes.contains("START=40\nEND=83\n"));
    CHECK(bytes.contains("title=title\\=one\\;\\#two\\\\three\\\nnext\n"));
    MediaFfmpeg nativeFfmpeg;
    const QFileInfo ffmpegInfo(nativeFfmpeg.path);
    const auto probe = QDir(ffmpegInfo.absolutePath())
                           .filePath(QStringLiteral("ffprobe") + (ffmpegInfo.suffix().isEmpty() ? QString() : QLatin1Char('.') + ffmpegInfo.suffix()));
    REQUIRE(QFileInfo(probe).isFile());
    QProcess reader;
    reader.start(probe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("ffmetadata"), QStringLiteral("-show_chapters"),
                         QStringLiteral("-of"), QStringLiteral("json"), path});
    REQUIRE(reader.waitForFinished(5000));
    REQUIRE(reader.exitCode() == 0);
    const auto chapters = QJsonDocument::fromJson(reader.readAllStandardOutput()).object().value(QStringLiteral("chapters")).toArray();
    REQUIRE(chapters.size() == 2);
    CHECK(chapters.at(0).toObject().value(QStringLiteral("time_base")).toString() == QStringLiteral("1/25"));
    CHECK(chapters.at(0).toObject().value(QStringLiteral("start")).toInt() == 10);
    CHECK(chapters.at(0).toObject().value(QStringLiteral("end")).toInt() == 16);
    CHECK(chapters.at(0).toObject().value(QStringLiteral("tags")).toObject().value(QStringLiteral("title")).toString() == comment);
    CHECK(chapters.at(1).toObject().value(QStringLiteral("start")).toInt() == 40);
    CHECK(chapters.at(1).toObject().value(QStringLiteral("end")).toInt() == 83);
    CHECK(project.undoStack->index() == history);
    CHECK(project.token() == token);
    args.insert(QStringLiteral("expectedState"), project.token());
    project.rejectWithoutChange(QStringLiteral("marker_file"), args);
    CHECK(markerFileBytes(path) == bytes);
    CHECK(QDir(files.path()).entryList({QStringLiteral(".kdenlive-mcp-markers-*")}, QDir::Files | QDir::Hidden).isEmpty());
}

TEST_CASE("MCP marker preferences require settings acknowledgement and remain separate from native project undo", "[MCP][MCPMarkers][settings]")
{
    REQUIRE(QStandardPaths::isTestModeEnabled());
    McpProject project;
    project.open();
    RestoreMarkerPreferences restore;
    const auto model = project.timeline->getGuideModel();
    const auto currentCategories = model->categoriesToStringList();
    const auto beforeState = project.state();
    const int history = project.undoStack->index();
    const int defaultId = pCore->markerTypes.firstKey();
    // A supplied category definition replaces existing future defaults instead
    // of appending them, and repeating it must remain a true no-op.
    KdenliveSettings::setGuidesCategories(QStringList{QStringLiteral("Old future default:%1:#abcdef").arg(defaultId)});
    const auto first = project.success(QStringLiteral("get_marker_settings")).value(QStringLiteral("settings")).toObject();
    auto args = markerFileFields(project, {{QStringLiteral("expectedSettings"), first.value(QStringLiteral("expectedSettings"))},
                                           {QStringLiteral("exportTextTemplate"), QString::fromUtf8("{{frame}} – {{comment}}")}});
    project.rejectWithoutChange(QStringLiteral("edit_marker_settings"), args);
    args.insert(QStringLiteral("acknowledgeSettingsChange"), true);
    args.insert(QStringLiteral("defaultCategoryId"), defaultId);
    const QJsonArray defaults{QJsonObject{{QStringLiteral("index"), defaultId},
                                          {QStringLiteral("comment"), QString::fromUtf8("ברירת מחדל")},
                                          {QStringLiteral("color"), QStringLiteral("#123456")}}};
    args.insert(QStringLiteral("defaultCategories"), defaults);
    const auto changed = project.success(QStringLiteral("edit_marker_settings"), args);
    CHECK(changed.value(QStringLiteral("changed")).toBool());
    CHECK_FALSE(changed.value(QStringLiteral("undoable")).toBool(true));
    CHECK(KdenliveSettings::exportGuidesFormat() == QString::fromUtf8("{{frame}} – {{comment}}"));
    CHECK(KdenliveSettings::guidesCategories() == QStringList{QString::fromUtf8("ברירת מחדל:%1:#123456").arg(defaultId)});
    CHECK(model->categoriesToStringList() == currentCategories);
    CHECK(project.undoStack->index() == history);
    CHECK(project.state().value(QStringLiteral("modified")) == beforeState.value(QStringLiteral("modified")));
    args.insert(QStringLiteral("expectedState"), project.token());
    project.rejectWithoutChange(QStringLiteral("edit_marker_settings"), args);
    const auto fresh = project.success(QStringLiteral("get_marker_settings")).value(QStringLiteral("settings")).toObject();
    REQUIRE(fresh.value(QStringLiteral("expectedSettings")) != first.value(QStringLiteral("expectedSettings")));
    args.insert(QStringLiteral("expectedSettings"), fresh.value(QStringLiteral("expectedSettings")));
    CHECK_FALSE(project.success(QStringLiteral("edit_marker_settings"), args).value(QStringLiteral("changed")).toBool(true));
    args.insert(QStringLiteral("defaultCategories"), QJsonArray{});
    project.rejectWithoutChange(QStringLiteral("edit_marker_settings"), args);
    CHECK(model->categoriesToStringList() == currentCategories);
    CHECK(project.undoStack->index() == history);
}

TEST_CASE("MCP marker preview pages fit the encoded protocol budget with worst-case Unicode and large state", "[MCP][MCPMarkers][bounds]")
{
    McpProject project;
    project.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto path = files.filePath(QStringLiteral("wide-comments.json"));
    const QString comment(8192, QChar(0x4e2d));
    QJsonArray input;
    for (int i = 0; i < 32; ++i)
        input.append(QJsonObject{{QStringLiteral("pos"), i}, {QStringLiteral("comment"), comment}, {QStringLiteral("type"), pCore->markerTypes.firstKey()}});
    writeMarkerFile(path, QJsonDocument(input).toJson(QJsonDocument::Compact));
    const auto originalBytes = markerFileBytes(path);
    auto state = project.state();
    // This valid serialized state allowance consumes about 870 KiB after its
    // UTF-8 content appears in both text and structuredContent. It leaves room
    // for a few full markers, rather than assuming a small empty-project state.
    state.insert(QStringLiteral("boundaryStatePadding"), QString(145000, QChar(0x4e2d)));
    auto wire = [](const QJsonObject &result) {
        return QJsonDocument(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                         {QStringLiteral("id"), QString(128, QChar(1))},
                                         {QStringLiteral("result"), result}})
            .toJson(QJsonDocument::Compact);
    };
    int offset = 0, calls = 0, reads = 0;
    QJsonArray seen;
    do {
        const auto result = McpMarkers::callTool(&project.server, QStringLiteral("inspect_marker_file"),
                                                 {{QStringLiteral("scope"), QStringLiteral("guides")},
                                                  {QStringLiteral("path"), path},
                                                  {QStringLiteral("format"), QStringLiteral("json")},
                                                  {QStringLiteral("offset"), offset},
                                                  {QStringLiteral("limit"), 32}},
                                                 state, [&] {
                                                     ++reads;
                                                     return state;
                                                 });
        ++calls;
        REQUIRE_FALSE(result.value(QStringLiteral("isError")).toBool());
        CHECK(wire(result).size() <= 1024 * 1024);
        const auto data = result.value(QStringLiteral("structuredContent")).toObject();
        const auto page = data.value(QStringLiteral("markers")).toArray();
        REQUIRE_FALSE(page.isEmpty());
        CHECK(page.size() < 32);
        CHECK(data.value(QStringLiteral("nextOffset")).toInt() == offset + page.size());
        for (const auto &entry : page) {
            CHECK(entry.toObject().value(QStringLiteral("pos")).toInt() == seen.size());
            CHECK(entry.toObject().value(QStringLiteral("comment")).toString() == comment);
            seen.append(entry);
        }
        offset = data.value(QStringLiteral("nextOffset")).toInt();
        CHECK(data.value(QStringLiteral("hasMore")).toBool() == (offset < input.size()));
    } while (offset < input.size());
    CHECK(seen.size() == input.size());
    CHECK(reads == calls);
    CHECK(markerFileBytes(path) == originalBytes);
    CHECK(project.timeline->getGuideModel()->rowCount() == 0);

    // Complete structured data can fit even when its duplicate text would
    // exceed the protocol budget. Omitting that text must lose no native data.
    const QString structuredPadding(180000, QChar(0x4e2d));
    state.insert(QStringLiteral("boundaryStatePadding"), structuredPadding);
    const auto structuredOnly = McpMarkers::callTool(
        &project.server, QStringLiteral("inspect_marker_file"),
        {{QStringLiteral("scope"), QStringLiteral("guides")}, {QStringLiteral("path"), path}, {QStringLiteral("format"), QStringLiteral("json")}}, state,
        [&] { return state; });
    REQUIRE_FALSE(structuredOnly.value(QStringLiteral("isError")).toBool());
    CHECK(structuredOnly.value(QStringLiteral("_meta")).toObject().value(QStringLiteral("textCopyOmitted")).toBool());
    CHECK(wire(structuredOnly).size() <= 1024 * 1024);
    const auto structuredData = structuredOnly.value(QStringLiteral("structuredContent")).toObject();
    CHECK(structuredData.value(QStringLiteral("state")).toObject().value(QStringLiteral("boundaryStatePadding")).toString() == structuredPadding);
    const auto structuredPage = structuredData.value(QStringLiteral("markers")).toArray();
    REQUIRE_FALSE(structuredPage.isEmpty());
    for (int row = 0; row < structuredPage.size(); ++row) {
        CHECK(structuredPage.at(row).toObject().value(QStringLiteral("pos")).toInt() == row);
        CHECK(structuredPage.at(row).toObject().value(QStringLiteral("comment")).toString() == comment);
    }
    CHECK(structuredData.value(QStringLiteral("nextOffset")).toInt() == structuredPage.size());
    CHECK(structuredData.value(QStringLiteral("hasMore")).toBool() == (structuredPage.size() < input.size()));
    CHECK(markerFileBytes(path) == originalBytes);
    CHECK(project.timeline->getGuideModel()->rowCount() == 0);

    // A base state that cannot fit even once still leaves no room for a marker
    // preview; return the bounded explicit error without dropping marker rows.
    state.insert(QStringLiteral("boundaryStatePadding"), QString(400000, QChar(0x4e2d)));
    const auto excessive = McpMarkers::callTool(
        &project.server, QStringLiteral("inspect_marker_file"),
        {{QStringLiteral("scope"), QStringLiteral("guides")}, {QStringLiteral("path"), path}, {QStringLiteral("format"), QStringLiteral("json")}}, state,
        [&] { return state; });
    REQUIRE(excessive.value(QStringLiteral("isError")).toBool());
    CHECK(excessive.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")) ==
          QStringLiteral("RESPONSE_LIMIT"));
    CHECK(wire(excessive).size() <= 1024 * 1024);
}

TEST_CASE("MCP native text template expansion is bounded before direct and later-placeholder allocations", "[MCP][MCPMarkers][bounds]")
{
    McpProject project;
    project.open();
    QTemporaryDir files;
    REQUIRE(files.isValid());
    const auto model = project.timeline->getGuideModel();
    const auto originalCategories = pCore->markerTypes;
    const auto restore = qScopeGuard([&] { pCore->markerTypes = originalCategories; });
    const int category = pCore->markerTypes.firstKey();
    const auto path = files.filePath(QStringLiteral("bounded-template.txt"));
    const QString largePattern = QStringLiteral("{{comment}}").repeated(372);
    QString text, pattern = largePattern, expected;
    bool rejected = true;
    SECTION("A four-MiB native comment cannot allocate three GiB through a short template")
    {
        text = QString(4 * 1024 * 1024, QLatin1Char('x'));
    }
    SECTION("Category placeholders introduced by comment expansion are checked in their later pass")
    {
        project.success(QStringLiteral("edit_marker_category"),
                        markerFileFields(project, {{QStringLiteral("operation"), QStringLiteral("update")},
                                                   {QStringLiteral("categoryId"), category},
                                                   {QStringLiteral("name"), QString(160, QLatin1Char('c'))},
                                                   {QStringLiteral("color"), pCore->markerTypes.value(category).color.name()}}));
        text = QStringLiteral("{{category}}").repeated(681);
    }
    SECTION("Bounded replacement preserves native order without rescanning its own inserted placeholder")
    {
        project.success(QStringLiteral("edit_marker_category"),
                        markerFileFields(project, {{QStringLiteral("operation"), QStringLiteral("update")},
                                                   {QStringLiteral("categoryId"), category},
                                                   {QStringLiteral("name"), QStringLiteral("{{duration}}C")},
                                                   {QStringLiteral("color"), pCore->markerTypes.value(category).color.name()}}));
        text = QStringLiteral("{{category}}/{{duration}}/{{comment}}");
        pattern = QStringLiteral("{{comment}}{{comment}}|{{duration}}");
        expected = QStringLiteral("0C/0/{{comment}}0C/0/{{comment}}|0");
        rejected = false;
    }
    REQUIRE(model->addMarker(GenTime(7, 25), text, category));
    const int history = project.undoStack->index();
    auto args = markerFileFields(project, {{QStringLiteral("operation"), QStringLiteral("export")},
                                           {QStringLiteral("scope"), QStringLiteral("guides")},
                                           {QStringLiteral("path"), path},
                                           {QStringLiteral("format"), QStringLiteral("text")},
                                           {QStringLiteral("textTemplate"), pattern}});
    const auto result = project.call(QStringLiteral("marker_file"), args);
    const auto data = result.value(QStringLiteral("structuredContent")).toObject();
    if (rejected) {
        REQUIRE(result.value(QStringLiteral("isError")).toBool());
        CHECK(data.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")) == QStringLiteral("EXPORT_LIMIT"));
        CHECK_FALSE(QFileInfo::exists(path));
    } else {
        REQUIRE_FALSE(result.value(QStringLiteral("isError")).toBool());
        CHECK(QString::fromUtf8(markerFileBytes(path)) == expected);
    }
    CHECK(model->marker(7).comment() == text);
    CHECK(project.undoStack->index() == history);
    CHECK(QDir(files.path()).entryList({QStringLiteral(".kdenlive-mcp-markers-*")}, QDir::Files | QDir::Hidden).isEmpty());
}
