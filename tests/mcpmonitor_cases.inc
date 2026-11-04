/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Included after mcpmedia_cases.inc by mcptest.cpp.
#include "mcp/mcpmonitor.h"
namespace {
QString monitorColorFixture(const QTemporaryDir &directory, const QString &ffmpeg)
{
    const auto path = directory.filePath(QStringLiteral("red-blue.mkv"));
    mediaProcess(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                          QStringLiteral("color=c=red:s=32x32:r=25:d=1"), QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                          QStringLiteral("color=c=blue:s=32x32:r=25:d=1"), QStringLiteral("-filter_complex"), QStringLiteral("[0:v][1:v]concat=n=2:v=1:a=0"),
                          QStringLiteral("-c:v"), QStringLiteral("ffv1"), QStringLiteral("-an"), QStringLiteral("-n"), path});
    return path;
}
QJsonObject monitorPreviewArguments(McpProject &project, const QString &bin, int frame, const QString &path)
{
    return mediaFields(project, {{QStringLiteral("binId"), bin},
                                 {QStringLiteral("frame"), frame},
                                 {QStringLiteral("path"), path},
                                 {QStringLiteral("width"), 64},
                                 {QStringLiteral("height"), 64}});
}
} // namespace
TEST_CASE("MCP detached source preview decodes exact red and blue frames without changing the native source", "[MCP][MCPMonitors]")
{
    MediaFfmpeg ffmpeg;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    const auto bin = importFixture(project, monitorColorFixture(directory, ffmpeg.path));
    const auto source = pCore->projectItemModel()->getClipByBinID(bin);
    REQUIRE(source);
    REQUIRE(source->statusReady());
    REQUIRE(source->getFramePlaytime() >= 50);
    const auto producer = source->originalProducer();
    REQUIRE(producer);
    producer->seek(7);
    const auto beforePosition = producer->position();
    const auto state = project.token();
    const int history = project.undoStack->index();
    const auto redPath = directory.filePath(QStringLiteral("red.png")), bluePath = directory.filePath(QStringLiteral("blue.png"));
    const auto red = project.success(QStringLiteral("preview_source_frame"), monitorPreviewArguments(project, bin, 5, redPath));
    REQUIRE(red.value(QStringLiteral("nativeVerified")).toBool());
    CHECK(red.value(QStringLiteral("detached")).toBool());
    CHECK(red.value(QStringLiteral("frame")).toInt() == 5);
    const auto hash = red.value(QStringLiteral("sourceHash")).toString();
    REQUIRE(hash.size() == 64);
    auto blueArgs = monitorPreviewArguments(project, bin, 30, bluePath);
    blueArgs[QStringLiteral("expectedSourceHash")] = hash;
    const auto blue = project.success(QStringLiteral("preview_source_frame"), blueArgs);
    CHECK(blue.value(QStringLiteral("sourceHash")) == hash);
    CHECK(blue.value(QStringLiteral("frame")).toInt() == 30);
    const QImage redImage(redPath), blueImage(bluePath);
    REQUIRE(redImage.size() == QSize(64, 64));
    REQUIRE(blueImage.size() == QSize(64, 64));
    const auto r = redImage.pixelColor(32, 32), b = blueImage.pixelColor(32, 32);
    CHECK(r.red() > 220);
    CHECK(r.blue() < 30);
    CHECK(b.blue() > 220);
    CHECK(b.red() < 30);
    CHECK(producer->position() == beforePosition);
    CHECK(project.token() == state);
    CHECK(project.undoStack->index() == history);
    REQUIRE(project.timeline->checkConsistency());
}
TEST_CASE("MCP source preview validates ranges source hashes and new output ownership", "[MCP][MCPMonitors]")
{
    MediaFfmpeg ffmpeg;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    const auto bin = importFixture(project, monitorColorFixture(directory, ffmpeg.path));
    const QString output = directory.filePath(QStringLiteral("must-stay-new.png"));
    auto arguments = monitorPreviewArguments(project, bin, 0, output);
    arguments[QStringLiteral("frame")] = -1;
    project.rejectWithoutChange(QStringLiteral("preview_source_frame"), arguments);
    arguments[QStringLiteral("frame")] = 999999;
    project.rejectWithoutChange(QStringLiteral("preview_source_frame"), arguments);
    arguments[QStringLiteral("frame")] = 0;
    arguments[QStringLiteral("width")] = 0;
    project.rejectWithoutChange(QStringLiteral("preview_source_frame"), arguments);
    arguments[QStringLiteral("width")] = 64;
    arguments[QStringLiteral("expectedSourceHash")] = QStringLiteral("stale");
    project.rejectWithoutChange(QStringLiteral("preview_source_frame"), arguments);
    CHECK_FALSE(QFileInfo(output).exists());
    arguments.remove(QStringLiteral("expectedSourceHash"));
    project.success(QStringLiteral("preview_source_frame"), arguments);
    const auto bytes = mediaHash(output);
    arguments[QStringLiteral("expectedState")] = project.token();
    project.rejectWithoutChange(QStringLiteral("preview_source_frame"), arguments);
    CHECK(mediaHash(output) == bytes);
}
TEST_CASE("MCP monitor controls report absent native GUI and owned unknown jobs truthfully", "[MCP][MCPMonitors]")
{
    McpProject project;
    project.open();
    const auto result = project.call(QStringLiteral("get_monitors"));
    REQUIRE(result.value(QStringLiteral("isError")).toBool());
    CHECK(result.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")) ==
          QStringLiteral("GUI_REQUIRED"));
    project.rejectWithoutChange(QStringLiteral("control_monitor"), mediaFields(project, {{QStringLiteral("expectedMonitor"), QStringLiteral("absent")},
                                                                                         {QStringLiteral("monitor"), QStringLiteral("source")},
                                                                                         {QStringLiteral("operation"), QStringLiteral("activate")}}));
    const auto unknown = project.call(QStringLiteral("monitor_job_status"), {{QStringLiteral("jobId"), QStringLiteral("not-owned")}});
    CHECK(unknown.value(QStringLiteral("isError")).toBool());
    CHECK(unknown.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")) ==
          QStringLiteral("INVALID_JOB"));
    const auto jobs = project.success(QStringLiteral("monitor_job_status")).value(QStringLiteral("jobs")).toArray();
    CHECK(jobs.isEmpty());
}
