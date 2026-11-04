/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Included by mcptest.cpp. No speech models are downloaded or mocked as recognition.
#include "kdenlivesettings.h"
#include "mcp/mcpspeech.h"
#include <KLocalizedString>
#include <QApplication>
#include <QTextCursor>
#include <QTextDocument>
namespace {
QJsonObject speechFields(McpProject &project, const QJsonObject &fields)
{
    auto arguments = project.expected();
    for (auto it = fields.begin(); it != fields.end(); ++it)
        arguments.insert(it.key(), it.value());
    return arguments;
}
struct SpeechPythonSettings
{
    bool system = KdenliveSettings::speech_system_python();
    QString path = KdenliveSettings::speech_system_python_path();
    ~SpeechPythonSettings()
    {
        KdenliveSettings::setSpeech_system_python(system);
        KdenliveSettings::setSpeech_system_python_path(path);
    }
};
QJsonObject speechTerminal(McpProject &project, const QString &id)
{
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const auto job = project.success(QStringLiteral("speech_job_status"), {{QStringLiteral("jobId"), id}}).value(QStringLiteral("job")).toObject();
        if (job.value(QStringLiteral("finished")).toBool()) return job;
        QThread::msleep(1);
    }
    FAIL("Native speech prerequisite job exceeded its test deadline");
    return {};
}
QString speechFixtureBin(McpProject &project, const QTemporaryDir &directory)
{
    const auto accepted = project.success(QStringLiteral("import_media"), speechFields(project, {{QStringLiteral("path"), wavFixture(directory)}}));
    const QString bin = accepted.value(QStringLiteral("binId")).toString();
    project.awaitMediaReady(bin);
    return bin;
}
QString speechFixtureHtml(const QString &id)
{
    QTextDocument document;
    QTextCursor cursor(&document);
    const QList<QString> labels{QString::fromUtf8("שלום"), QStringLiteral("two"), i18n("No speech"), QStringLiteral("three")};
    const QList<QPair<double, double>> times{{0.123456789012345, 0.2}, {0.4, 0.6}, {0.6, 0.7}, {0.72, 0.9}};
    for (int i = 0; i < labels.size(); ++i) {
        if (i > 0) cursor.insertBlock();
        QTextCharFormat format;
        format.setAnchor(true);
        format.setAnchorHref(id + QLatin1Char('#') + QString::number(times[i].first, 'g', 17) + QLatin1Char(':') + QString::number(times[i].second, 'g', 17));
        cursor.insertText(labels[i], format);
        cursor.insertText(QStringLiteral(" "), QTextCharFormat{});
    }
    return document.toHtml();
}
} // namespace
TEST_CASE("MCP native speech capabilities remain observational and report prerequisites", "[MCP][Speech]")
{
    McpProject project;
    project.open();
    SpeechPythonSettings restore;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString absent = directory.filePath(QStringLiteral("not-installed/python.exe"));
    KdenliveSettings::setSpeech_system_python(true);
    KdenliveSettings::setSpeech_system_python_path(absent);
    const int history = project.undoStack->count();
    const auto capabilities = project.success(QStringLiteral("speech_capabilities"));
    REQUIRE(capabilities.value(QStringLiteral("engines")).toArray().size() == 2);
    CHECK(capabilities.value(QStringLiteral("configuredPython")).toString().isEmpty());
    CHECK_FALSE(capabilities.value(QStringLiteral("seamless")).toObject().value(QStringLiteral("available")).toBool());
    for (const auto &entry : capabilities.value(QStringLiteral("engines")).toArray()) {
        const auto engine = entry.toObject();
        CHECK_FALSE(engine.value(QStringLiteral("pythonAvailable")).toBool());
        CHECK_FALSE(engine.value(QStringLiteral("recognitionVerified")).toBool());
        CHECK(engine.value(QStringLiteral("dependencyStatus")) == QStringLiteral("not_probed"));
        if (engine.value(QStringLiteral("id")) == QStringLiteral("whisper")) CHECK(engine.value(QStringLiteral("languages")).toArray().size() > 50);
    }
    CHECK_FALSE(QFileInfo::exists(absent));
    CHECK(project.undoStack->count() == history);
    CHECK(KdenliveSettings::speech_system_python_path() == absent);
}
TEST_CASE("MCP native speech prerequisite jobs fail and cancel without changing project", "[MCP][Speech]")
{
    McpProject project;
    project.open();
    SpeechPythonSettings restore;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    KdenliveSettings::setSpeech_system_python(true);
    KdenliveSettings::setSpeech_system_python_path(directory.filePath(QStringLiteral("missing-python.exe")));
    const auto fields = QJsonObject{{QStringLiteral("mode"), QStringLiteral("probe")}, {QStringLiteral("engine"), QStringLiteral("whisper")}};
    const auto queued = project.success(QStringLiteral("start_speech_job"), speechFields(project, fields)).value(QStringLiteral("job")).toObject();
    CHECK(queued.value(QStringLiteral("status")) == QStringLiteral("queued"));
    const QString id = queued.value(QStringLiteral("jobId")).toString();
    project.rejectWithoutChange(QStringLiteral("start_speech_job"), speechFields(project, fields));
    const auto cancelled = project.success(QStringLiteral("cancel_speech_job"), {{QStringLiteral("jobId"), id}}).value(QStringLiteral("job")).toObject();
    CHECK(cancelled.value(QStringLiteral("status")) == QStringLiteral("cancelled"));
    CHECK(speechTerminal(project, id).value(QStringLiteral("status")) == QStringLiteral("cancelled"));
    const auto accepted = project.success(QStringLiteral("start_speech_job"), speechFields(project, fields));
    const auto failed = speechTerminal(project, accepted.value(QStringLiteral("job")).toObject().value(QStringLiteral("jobId")).toString());
    CHECK(failed.value(QStringLiteral("status")) == QStringLiteral("failed"));
    CHECK(failed.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")) == QStringLiteral("MISSING_PYTHON"));
    CHECK(project.undoStack->count() == 0);
    CHECK(project.timeline->getClipsCount() == 0);
    CHECK_FALSE(QApplication::activeModalWidget());
}
TEST_CASE("MCP native text transcript preserves exact anchors and supports undoable word edits", "[MCP][Speech]")
{
    McpProject project;
    project.open();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString id = speechFixtureBin(project, directory);
    const auto source = pCore->projectItemModel()->getClipByBinID(id);
    const QString original = speechFixtureHtml(id);
    source->setProperties({{QStringLiteral("kdenlive:speech"), original}});
    const auto transcript = project.success(QStringLiteral("get_speech_transcript"), {{QStringLiteral("binId"), id}});
    const auto words = transcript.value(QStringLiteral("words")).toArray();
    REQUIRE(words.size() == 4);
    CHECK(words[0].toObject().value(QStringLiteral("text")) == QString::fromUtf8("שלום"));
    CHECK(words[0].toObject().value(QStringLiteral("startSeconds")).toDouble() == 0.123456789012345);
    CHECK(words[0].toObject().value(QStringLiteral("sourceInFrame")).toInt() == GenTime(0.123456789012345).frames(25));
    CHECK(words[2].toObject().value(QStringLiteral("silence")).toBool());
    const auto page =
        project.success(QStringLiteral("get_speech_transcript"), {{QStringLiteral("binId"), id}, {QStringLiteral("offset"), 1}, {QStringLiteral("limit"), 1}});
    CHECK(page.value(QStringLiteral("hasMore")).toBool());
    CHECK(page.value(QStringLiteral("words")).toArray().size() == 1);
    const int before = project.undoStack->index();
    project.success(QStringLiteral("edit_speech_transcript"), speechFields(project, {{QStringLiteral("binId"), id},
                                                                                     {QStringLiteral("action"), QStringLiteral("replace_word")},
                                                                                     {QStringLiteral("firstWord"), 0},
                                                                                     {QStringLiteral("text"), QString::fromUtf8("שלום & <world>")}}));
    CHECK(project.undoStack->index() == before + 1);
    const auto edited = project.success(QStringLiteral("get_speech_transcript"), {{QStringLiteral("binId"), id}}).value(QStringLiteral("words")).toArray();
    CHECK(edited[0].toObject().value(QStringLiteral("text")) == QString::fromUtf8("שלום & <world>"));
    CHECK(edited[0].toObject().value(QStringLiteral("startSeconds")) == words[0].toObject().value(QStringLiteral("startSeconds")));
    project.undoStack->undo();
    CHECK(source->getProducerProperty(QStringLiteral("kdenlive:speech")) == original);
    project.undoStack->redo();
    CHECK(source->getProducerProperty(QStringLiteral("kdenlive:speech")) != original);
    project.undoStack->undo();
    project.success(QStringLiteral("edit_speech_transcript"),
                    speechFields(project, {{QStringLiteral("binId"), id}, {QStringLiteral("action"), QStringLiteral("remove_silence")}}));
    CHECK(project.success(QStringLiteral("get_speech_transcript"), {{QStringLiteral("binId"), id}}).value(QStringLiteral("wordCount")).toInt() == 3);
    project.undoStack->undo();
    CHECK(source->getProducerProperty(QStringLiteral("kdenlive:speech")) == original);
    project.success(QStringLiteral("edit_speech_transcript"), speechFields(project, {{QStringLiteral("binId"), id},
                                                                                     {QStringLiteral("action"), QStringLiteral("remove_words")},
                                                                                     {QStringLiteral("firstWord"), 1},
                                                                                     {QStringLiteral("wordCount"), 1}}));
    const auto removed = project.success(QStringLiteral("get_speech_transcript"), {{QStringLiteral("binId"), id}});
    CHECK(removed.value(QStringLiteral("wordCount")).toInt() == 3);
    for (const auto &entry : removed.value(QStringLiteral("speechRanges")).toArray()) {
        const auto range = entry.toObject();
        CHECK_FALSE((range.value(QStringLiteral("sourceInFrame")).toInt() <= 12 && range.value(QStringLiteral("sourceOutFrame")).toInt() >= 12));
    }
    project.undoStack->undo();
    CHECK(source->getProducerProperty(QStringLiteral("kdenlive:speech")) == original);
    CHECK(project.timeline->getClipsCount() == 0);
}
TEST_CASE("MCP native text transcript rejects malformed anchors and foreign results", "[MCP][Speech]")
{
    McpProject project;
    project.open();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString id = speechFixtureBin(project, directory);
    const auto source = pCore->projectItemModel()->getClipByBinID(id);
    source->setProperties({{QStringLiteral("kdenlive:speech"), QStringLiteral("<p><a href=\"other#0.1:0.2\">foreign</a></p>")}});
    project.rejectWithoutChange(QStringLiteral("get_speech_transcript"), {{QStringLiteral("binId"), id}});
    source->setProperties({{QStringLiteral("kdenlive:speech"), speechFixtureHtml(id)}});
    project.rejectWithoutChange(QStringLiteral("edit_speech_transcript"), speechFields(project, {{QStringLiteral("binId"), id},
                                                                                                 {QStringLiteral("action"), QStringLiteral("replace_word")},
                                                                                                 {QStringLiteral("firstWord"), -1},
                                                                                                 {QStringLiteral("text"), QStringLiteral("invalid")}}));
    project.rejectWithoutChange(QStringLiteral("edit_speech_transcript"), speechFields(project, {{QStringLiteral("binId"), id},
                                                                                                 {QStringLiteral("action"), QStringLiteral("apply_job")},
                                                                                                 {QStringLiteral("jobId"), QStringLiteral("not-owned")}}));
}
TEST_CASE("MCP native text edit ranges use exclusive end boundaries without an extra frame", "[MCP][Speech]")
{
    McpProject project;
    project.open();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString id = speechFixtureBin(project, directory);
    const auto source = pCore->projectItemModel()->getClipByBinID(id);
    source->setProperties({{QStringLiteral("kdenlive:speech"), QStringLiteral("<p><a href=\"%1#0:1\">one second</a></p>").arg(id)}});
    const auto transcript = project.success(QStringLiteral("get_speech_transcript"), {{QStringLiteral("binId"), id}});
    const auto ranges = transcript.value(QStringLiteral("editRanges")).toArray();
    REQUIRE(ranges.size() == 1);
    const auto range = ranges.first().toObject();
    CHECK(range.value(QStringLiteral("sourceInFrame")).toInt() == 0);
    CHECK(range.value(QStringLiteral("sourceOutFrame")).toInt() == 24);
    CHECK(range.value(QStringLiteral("durationFrames")).toInt() == 25);
    REQUIRE_FALSE(source->audioStreams().isEmpty());
    const auto inserted =
        project.success(QStringLiteral("insert_media_streams"),
                        speechFields(project, {{QStringLiteral("binId"), id},
                                               {QStringLiteral("startFrame"), 0},
                                               {QStringLiteral("sourceInFrame"), range.value(QStringLiteral("sourceInFrame"))},
                                               {QStringLiteral("durationFrames"), range.value(QStringLiteral("durationFrames"))},
                                               {QStringLiteral("audio"), QJsonArray{QJsonObject{{QStringLiteral("streamId"), source->audioStreams().firstKey()},
                                                                                                {QStringLiteral("trackId"), project.audioTrack}}}}}));
    Q_UNUSED(inserted)
    REQUIRE(project.timeline->getClipsCount() == 1);
    const int clip = project.timeline->getClipByPosition(project.audioTrack, 0);
    REQUIRE(clip >= 0);
    CHECK(project.timeline->getClipPlaytime(clip) == 25);
    CHECK(project.timeline->getClipIn(clip) == 0);
    CHECK(project.timeline->getClipIn(clip) + project.timeline->getClipPlaytime(clip) - 1 == 24);
}
TEST_CASE("MCP native word deletion survives native transcript paragraphs", "[MCP][Speech]")
{
    McpProject project;
    project.open();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString id = speechFixtureBin(project, directory);
    const auto source = pCore->projectItemModel()->getClipByBinID(id);
    source->setProperties(
        {{QStringLiteral("kdenlive:speech"),
          QStringLiteral("<p><a href=\"%1#0:0.2\">first</a> <a href=\"%1#0.2:0.4\">second</a> <a href=\"%1#0.4:0.8\">third</a></p>").arg(id)}});
    project.success(QStringLiteral("edit_speech_transcript"), speechFields(project, {{QStringLiteral("binId"), id},
                                                                                     {QStringLiteral("action"), QStringLiteral("remove_words")},
                                                                                     {QStringLiteral("firstWord"), 1},
                                                                                     {QStringLiteral("wordCount"), 1}}));
    const auto transcript = project.success(QStringLiteral("get_speech_transcript"), {{QStringLiteral("binId"), id}});
    const auto words = transcript.value(QStringLiteral("words")).toArray();
    REQUIRE(words.size() == 2);
    CHECK(words[0].toObject().value(QStringLiteral("paragraph")) != words[1].toObject().value(QStringLiteral("paragraph")));
    const auto ranges = transcript.value(QStringLiteral("editRanges")).toArray();
    REQUIRE(ranges.size() == 2);
    CHECK(ranges[0].toObject().value(QStringLiteral("sourceOutFrame")).toInt() == 4);
    CHECK(ranges[1].toObject().value(QStringLiteral("sourceInFrame")).toInt() == 10);
    // Remaining native silence blocks are kept in editRanges until explicitly removed.
    source->setProperties({{QStringLiteral("kdenlive:speech"), speechFixtureHtml(id)}});
    const auto before = project.success(QStringLiteral("get_speech_transcript"), {{QStringLiteral("binId"), id}});
    CHECK(before.value(QStringLiteral("editRanges")).toArray().size() == 4);
    CHECK(before.value(QStringLiteral("speechRanges")).toArray().size() == 3);
    project.success(QStringLiteral("edit_speech_transcript"),
                    speechFields(project, {{QStringLiteral("binId"), id}, {QStringLiteral("action"), QStringLiteral("remove_silence")}}));
    CHECK(project.success(QStringLiteral("get_speech_transcript"), {{QStringLiteral("binId"), id}}).value(QStringLiteral("editRanges")).toArray().size() == 3);
}
TEST_CASE("MCP native speech checks actual configured Python package metadata without installation", "[.SpeechPython]")
{
    // Opt-in: a real existing interpreter must be supplied by the controlled test launcher.
    const QString python = qEnvironmentVariable("KDENLIVE_MCP_TEST_SPEECH_PYTHON");
    REQUIRE(QFileInfo(python).isAbsolute());
    REQUIRE(QFileInfo(python).isFile());
    McpProject project;
    project.open();
    SpeechPythonSettings restore;
    KdenliveSettings::setSpeech_system_python(true);
    KdenliveSettings::setSpeech_system_python_path(python);
    const auto accepted =
        project.success(QStringLiteral("start_speech_job"),
                        speechFields(project, {{QStringLiteral("mode"), QStringLiteral("probe")}, {QStringLiteral("engine"), QStringLiteral("whisper")}}));
    const auto result = speechTerminal(project, accepted.value(QStringLiteral("job")).toObject().value(QStringLiteral("jobId")).toString());
    INFO(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    if (result.value(QStringLiteral("status")) == QStringLiteral("failed")) {
        CHECK(result.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")) == QStringLiteral("MISSING_DEPENDENCIES"));
        REQUIRE_FALSE(result.value(QStringLiteral("missingDependencies")).toArray().isEmpty());
    } else {
        CHECK(result.value(QStringLiteral("status")) == QStringLiteral("completed"));
        CHECK(result.value(QStringLiteral("dependencyMetadataPresent")).toBool());
        CHECK_FALSE(result.value(QStringLiteral("engineVerified")).toBool());
    }
    CHECK(project.undoStack->count() == 0);
}

TEST_CASE("MCP native transcript pagination retains complete paragraph boundaries", "[MCP][Speech]")
{
    McpProject project;
    project.open();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString id = speechFixtureBin(project, directory);
    const auto source = pCore->projectItemModel()->getClipByBinID(id);
    source->setProperties({{QStringLiteral("kdenlive:speech"), QStringLiteral("<p><a href=\"%1#0:0.2\">first</a> <a href=\"%1#0.8:1\">last</a></p>").arg(id)}});
    const auto first = project.success(QStringLiteral("get_speech_transcript"), {{QStringLiteral("binId"), id}, {QStringLiteral("limit"), 1}});
    const auto last =
        project.success(QStringLiteral("get_speech_transcript"), {{QStringLiteral("binId"), id}, {QStringLiteral("offset"), 1}, {QStringLiteral("limit"), 1}});
    const auto a = first.value(QStringLiteral("editRanges")).toArray(), b = last.value(QStringLiteral("editRanges")).toArray();
    REQUIRE(a.size() == 1);
    REQUIRE(b.size() == 1);
    CHECK(a == b);
    CHECK(a[0].toObject().value(QStringLiteral("durationFrames")).toInt() == 25);
    CHECK(first.value(QStringLiteral("words")).toArray()[0].toObject().value(QStringLiteral("sourceOutFrame")).toInt() == 4);
    CHECK(last.value(QStringLiteral("words")).toArray()[0].toObject().value(QStringLiteral("sourceInFrame")).toInt() == 20);
}
