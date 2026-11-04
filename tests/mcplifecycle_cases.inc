/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "doc/kthumb.h"
#include "kdenlivesettings.h"
#include "renderpresets/renderpresetmodel.hpp"
#include "renderpresets/renderpresetrepository.hpp"
#include <QDir>
#include <iostream>

TEST_CASE("MCP lifecycle catalogs expose native profiles presets and bounded job queries", "[MCP][MCPLifecycle]")
{
    McpProject project;
    project.open();
    const auto profiles = project.success(QStringLiteral("list_profiles")).value(QStringLiteral("profiles")).toArray();
    REQUIRE_FALSE(profiles.isEmpty());
    bool pal = false;
    for (const auto &entry : profiles) {
        const auto profile = entry.toObject();
        CHECK(profile.value(QStringLiteral("width")).toInt() > 0);
        CHECK(profile.value(QStringLiteral("height")).toInt() > 0);
        CHECK(profile.value(QStringLiteral("frameRateDenominator")).toInt() > 0);
        if (profile.value(QStringLiteral("id")).toString() == QStringLiteral("dv_pal")) {
            pal = true;
            CHECK(profile.value(QStringLiteral("frameRateNumerator")).toInt() == 25);
        }
    }
    CHECK(pal);
    const auto presets = project.success(QStringLiteral("list_render_presets")).value(QStringLiteral("presets")).toArray();
    REQUIRE_FALSE(presets.isEmpty());
    project.rejectWithoutChange(QStringLiteral("list_render_presets"), {{QStringLiteral("presetName"), QStringLiteral("no-such-MCP-preset")}});
    project.rejectWithoutChange(QStringLiteral("render_status"), {{QStringLiteral("jobId"), QStringLiteral("another-session")}});
    project.rejectWithoutChange(QStringLiteral("project_job_status"), {{QStringLiteral("jobId"), QStringLiteral("another-session")}});
    CHECK(project.success(QStringLiteral("render_status")).value(QStringLiteral("jobs")).toArray().isEmpty());
    CHECK(project.success(QStringLiteral("project_job_status")).value(QStringLiteral("jobs")).toArray().isEmpty());
    project.rejectWithoutChange(QStringLiteral("media_job_status"), {{QStringLiteral("binId"), QStringLiteral("not-a-bin-id")}});
    QJsonObject open = project.expected();
    open.insert(QStringLiteral("path"), QStringLiteral("missing.kdenlive"));
    project.rejectWithoutChange(QStringLiteral("open_project"), open);
    QJsonObject cancel = project.expected();
    cancel.insert(QStringLiteral("jobId"), QStringLiteral("another-session"));
    project.rejectWithoutChange(QStringLiteral("cancel_render"), cancel);
}

TEST_CASE("MCP exact preview snapshots render requested frames and refuse output replacement", "[MCP][MCPLifecycle]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 10));
    auto blue = project.colorArgs(project.videoTracks.front(), 20, 10);
    blue.insert(QStringLiteral("color"), QStringLiteral("#0000ffff"));
    project.success(QStringLiteral("add_color_clip"), blue);
    const int undoIndex = project.undoStack->index();
    const auto preview = [&project, &directory](int frame, const QString &name) {
        auto args = project.expected();
        args.insert(QStringLiteral("frame"), frame);
        args.insert(QStringLiteral("path"), directory.filePath(name));
        args.insert(QStringLiteral("width"), 32);
        args.insert(QStringLiteral("height"), 32);
        return project.success(QStringLiteral("extract_preview"), args);
    };
    const auto red = preview(5, QStringLiteral("red.png"));
    const auto gap = preview(15, QStringLiteral("gap.png"));
    const auto last = preview(25, QStringLiteral("blue.png"));
    const QColor r(QImage(red.value(QStringLiteral("path")).toString()).pixel(16, 16));
    const QColor g(QImage(gap.value(QStringLiteral("path")).toString()).pixel(16, 16));
    const QColor b(QImage(last.value(QStringLiteral("path")).toString()).pixel(16, 16));
    CHECK(r.red() > 240);
    CHECK(r.green() < 16);
    CHECK(r.blue() < 16);
    CHECK(g.red() < 16);
    CHECK(g.green() < 16);
    CHECK(g.blue() < 16);
    CHECK(b.blue() > 240);
    CHECK(b.red() < 16);
    CHECK(b.green() < 16);
    CHECK(project.undoStack->index() == undoIndex);
    auto refused = project.expected();
    refused.insert(QStringLiteral("path"), directory.filePath(QStringLiteral("red.png")));
    refused.insert(QStringLiteral("frame"), 0);
    project.rejectWithoutChange(QStringLiteral("extract_preview"), refused);
    refused.insert(QStringLiteral("path"), directory.filePath(QStringLiteral("outside.png")));
    refused.insert(QStringLiteral("frame"), 30);
    project.rejectWithoutChange(QStringLiteral("extract_preview"), refused);
    CHECK_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral("outside.png"))));
}

TEST_CASE("MCP exports native snapshots and cancels only its owned renderer", "[MCP][MCPLifecycle][MCPRender]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 15));
    auto blue = project.colorArgs(project.videoTracks.front(), 15, 15);
    blue.insert(QStringLiteral("color"), QStringLiteral("#0000ffff"));
    project.success(QStringLiteral("add_color_clip"), blue);
    QString presetName;
    for (const auto &name : RenderPresetRepository::get()->getAllPresets()) {
        const auto &preset = RenderPresetRepository::get()->getPreset(name);
        if (preset && preset->isValid() && preset->renderer() == QStringLiteral("avformat") && preset->extension() == QStringLiteral("mp4") &&
            !preset->params().isImageSequence() && preset->params().value(QStringLiteral("vcodec")) == QStringLiteral("libx264")) {
            presetName = name;
            break;
        }
    }
    REQUIRE_FALSE(presetName.isEmpty());
    struct RestoreMelt
    {
        QString path{KdenliveSettings::meltpath()};
        ~RestoreMelt() { KdenliveSettings::setMeltpath(path); }
    } restore;
#ifdef Q_OS_WIN
    const QString filename = QStringLiteral("melt.exe");
#else
    const QString filename = QStringLiteral("melt");
#endif
    QString melt = QDir(QCoreApplication::applicationDirPath()).filePath(filename);
    if (!QFileInfo::exists(melt)) melt = QStandardPaths::findExecutable(filename);
    REQUIRE(QFileInfo::exists(melt));
    KdenliveSettings::setMeltpath(melt);
    const auto start = [&project, &directory, &presetName](const QString &file) {
        auto args = project.expected();
        args.insert(QStringLiteral("path"), directory.filePath(file));
        args.insert(QStringLiteral("presetName"), presetName);
        return project.success(QStringLiteral("start_render"), args).value(QStringLiteral("job")).toObject().value(QStringLiteral("jobId")).toString();
    };
    const auto awaitJob = [&project](const QString &id) {
        QElapsedTimer deadline;
        deadline.start();
        QJsonObject job;
        while (deadline.elapsed() < 30000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            job = project.success(QStringLiteral("render_status"), {{QStringLiteral("jobId"), id}}).value(QStringLiteral("job")).toObject();
            const QString status = job.value(QStringLiteral("status")).toString();
            if (status == QStringLiteral("completed") || status == QStringLiteral("failed") || status == QStringLiteral("canceled")) return job;
            QThread::msleep(5);
        }
        FAIL("Native render did not finish or cancel within its deadline");
        return job;
    };
    const QString id = start(QStringLiteral("native.mp4"));
    REQUIRE_FALSE(id.isEmpty());
    const auto job = awaitJob(id);
    INFO(QJsonDocument(job).toJson(QJsonDocument::Compact).constData());
    REQUIRE(job.value(QStringLiteral("status")).toString() == QStringLiteral("completed"));
    CHECK(job.value(QStringLiteral("progress")).toInt() == 100);
    REQUIRE(QFileInfo(directory.filePath(QStringLiteral("native.mp4"))).size() > 0);
    Mlt::Producer result(pCore->getProjectProfile(), directory.filePath(QStringLiteral("native.mp4")).toUtf8().constData());
    REQUIRE(result.is_valid());
    CHECK(result.get_length() == 30);
    const QColor red(KThumb::getFrame(result, 5, 32, 32).pixel(16, 16));
    const QColor blueFrame(KThumb::getFrame(result, 25, 32, 32).pixel(16, 16));
    CHECK(red.red() > 220);
    CHECK(red.blue() < 32);
    CHECK(blueFrame.blue() > 220);
    CHECK(blueFrame.red() < 32);
    auto existing = project.expected();
    existing.insert(QStringLiteral("path"), directory.filePath(QStringLiteral("native.mp4")));
    existing.insert(QStringLiteral("presetName"), presetName);
    project.rejectWithoutChange(QStringLiteral("start_render"), existing);
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 30, 10000));
    const QString canceledId = start(QStringLiteral("canceled.mp4"));
    auto cancel = project.expected();
    cancel.insert(QStringLiteral("jobId"), canceledId);
    project.success(QStringLiteral("cancel_render"), cancel);
    const auto canceled = awaitJob(canceledId);
    INFO(QJsonDocument(canceled).toJson(QJsonDocument::Compact).constData());
    CHECK(canceled.value(QStringLiteral("status")).toString() == QStringLiteral("canceled"));
    CHECK_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral("canceled.mp4"))));
    CHECK(QDir(directory.path()).entryList({QStringLiteral(".kdenlive-mcp-render-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}
#include <QApplication>
#include <QDialog>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <thread>

TEST_CASE("MCP normal Save As refuses a target created during native preparation", "[MCP][MCPLifecycle][MCPSave]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    const QString target = directory.filePath(QStringLiteral("concurrent.kdenlive"));
    const QString scene = QStringLiteral("<mlt><playlist id=\"main_bin\"><property name=\"race-fixture\">") + QString(8 * 1024 * 1024, QLatin1Char('x')) +
                          QStringLiteral("</property></playlist><tractor><track producer=\"main_bin\"/></tractor></mlt>");
    std::atomic<bool> started{false}, created{false};
    std::thread competingWriter([&] {
        while (!started.load(std::memory_order_acquire))
            std::this_thread::yield();
        // XML parsing/serialization runs concurrently with this independent creator.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        QFile file(target);
        if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            created.store(file.write("preserve-concurrent-file") == 24 && file.flush(), std::memory_order_release);
        }
    });
    int errorDialogs = 0;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [&] {
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
            ++errorDialogs;
            dialog->accept();
        }
    });
    dismiss.start(1);
    started.store(true, std::memory_order_release);
    // No errorMessage is supplied: this exercises the explicit normal-save policy,
    // rather than the earlier automated-copy-only error path.
    const bool saved = project.document->saveSceneList(target, scene, false, nullptr, true);
    competingWriter.join();
    dismiss.stop();
    REQUIRE(created.load(std::memory_order_acquire));
    CHECK_FALSE(saved);
    CHECK(errorDialogs == 1);
    QFile file(target);
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll() == QByteArray("preserve-concurrent-file"));
    CHECK(project.document->url().isEmpty());
    CHECK(QDir(directory.path()).entryList({QStringLiteral(".kdenlive-new-*")}, QDir::Files | QDir::Hidden).isEmpty());

    const QString fresh = directory.filePath(QStringLiteral("fresh.kdenlive"));
    const QString small = QStringLiteral("<mlt><playlist id=\"main_bin\"/><tractor><track producer=\"main_bin\"/></tractor></mlt>");
    REQUIRE(project.document->saveSceneList(fresh, small, false, nullptr, true));
    REQUIRE(QFileInfo::exists(fresh));
}

/* Advanced exports use only generated fixtures and temporary new outputs. */
#include "render/renderrequest.h"
#include <mlt++/MltProfile.h>
namespace {
struct ExportMelt
{
    QString previous{KdenliveSettings::meltpath()};
    ExportMelt()
    {
#ifdef Q_OS_WIN
        const QString name = QStringLiteral("melt.exe");
#else
        const QString name = QStringLiteral("melt");
#endif
        QString path = QDir(QCoreApplication::applicationDirPath()).filePath(name);
        if (!QFileInfo(path).isFile()) path = QStandardPaths::findExecutable(name);
        REQUIRE(QFileInfo(path).isFile());
        KdenliveSettings::setMeltpath(path);
    }
    ~ExportMelt() { KdenliveSettings::setMeltpath(previous); }
};
QString exportPreset(bool image, bool audio = false)
{
    for (const auto &name : RenderPresetRepository::get()->getAllPresets()) {
        const auto &preset = RenderPresetRepository::get()->getPreset(name);
        if (!preset || !preset->isValid() || preset->renderer() != QStringLiteral("avformat")) continue;
        const auto params = preset->params();
        if (image && preset->params().isImageSequence() && preset->extension() == QStringLiteral("png")) return name;
        if (audio && preset->extension() == QStringLiteral("wav")) return name;
        if (!image && !audio && preset->extension() == QStringLiteral("mp4") && params.value(QStringLiteral("vcodec")) == QStringLiteral("libx264"))
            return name;
    }
    FAIL("Required installed native export preset was not found");
    return {};
}
QJsonObject exportArgs(McpProject &project, const QString &path, const QString &preset, const QJsonObject &extra = {})
{
    auto args = project.expected();
    args.insert(QStringLiteral("path"), path);
    args.insert(QStringLiteral("presetName"), preset);
    for (auto it = extra.cbegin(); it != extra.cend(); ++it)
        args.insert(it.key(), it.value());
    return args;
}
QString exportStart(McpProject &project, const QString &path, const QString &preset, const QJsonObject &extra = {})
{
    return project.success(QStringLiteral("start_render"), exportArgs(project, path, preset, extra))
        .value(QStringLiteral("job"))
        .toObject()
        .value(QStringLiteral("jobId"))
        .toString();
}
QJsonObject exportTerminal(McpProject &project, const QString &id)
{
    QElapsedTimer deadline;
    deadline.start();
    int previous = 0;
    while (deadline.elapsed() < 30000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const auto job = project.success(QStringLiteral("render_status"), {{QStringLiteral("jobId"), id}}).value(QStringLiteral("job")).toObject();
        const int progress = job.value(QStringLiteral("progress")).toInt();
        REQUIRE(progress >= previous);
        REQUIRE(progress >= 0);
        REQUIRE(progress <= 100);
        previous = progress;
        const QString status = job.value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("completed") || status == QStringLiteral("failed") || status == QStringLiteral("canceled")) {
            if (status == QStringLiteral("failed"))
                std::cerr << "MCP export terminal job: " << QJsonDocument(job).toJson(QJsonDocument::Compact).constData() << '\n';
            return job;
        }
        QThread::msleep(2);
    }
    FAIL("Advanced native export did not finish before its deadline");
    return {};
}
void exportGuide(McpProject &project, int frame, const QString &label)
{
    auto args = project.expected();
    args.insert(QStringLiteral("scope"), QStringLiteral("guides"));
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("frame"), frame);
    args.insert(QStringLiteral("comment"), label);
    project.success(QStringLiteral("edit_marker"), args);
}
QByteArray exportRead(const QString &path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
} // namespace

TEST_CASE("MCP publishes exact native PNG image sequences and guide sections atomically", "[MCP][MCPLifecycle][MCPRender][MCPAdvancedExport]")
{
    ExportMelt melt;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 4));
    auto blue = project.colorArgs(project.videoTracks.front(), 4, 4);
    blue.insert(QStringLiteral("color"), QStringLiteral("#0000ffff"));
    project.success(QStringLiteral("add_color_clip"), blue);
    const QString preset = exportPreset(true);
    const QString target = directory.filePath(QStringLiteral("png-sequence"));
    const int undoIndex = project.undoStack->index();
    const QString renderId = exportStart(project, target, preset, {{QStringLiteral("outputName"), QStringLiteral("frame.png")}});
    const auto prepared = project.success(QStringLiteral("render_status"), {{QStringLiteral("jobId"), renderId}}).value(QStringLiteral("job")).toObject();
    REQUIRE(prepared.value(QStringLiteral("outputs")).toArray().first().toObject().value(QStringLiteral("kind")).toString() == QStringLiteral("imageSequence"));
    const auto completed = exportTerminal(project, renderId);
    REQUIRE(completed.value(QStringLiteral("status")).toString() == QStringLiteral("completed"));
    CHECK(completed.value(QStringLiteral("outputKind")).toString() == QStringLiteral("directory"));
    CHECK(completed.value(QStringLiteral("queueScope")).toString() == QStringLiteral("session"));
    CHECK(completed.value(QStringLiteral("progress")).toInt() == 100);
    const auto files = QDir(target).entryList({QStringLiteral("*.png")}, QDir::Files, QDir::Name);
    REQUIRE(files.size() == 8);
    for (int index = 0; index < files.size(); ++index) {
        const QImage image(QDir(target).filePath(files[index]));
        REQUIRE_FALSE(image.isNull());
        const QColor pixel(image.pixel(image.width() / 2, image.height() / 2));
        if (index < 4) {
            CHECK(pixel.red() > 240);
            CHECK(pixel.blue() < 16);
        } else {
            CHECK(pixel.blue() > 240);
            CHECK(pixel.red() < 16);
        }
    }
    CHECK(project.undoStack->index() == undoIndex);
    // Exercise both installed forms: properties=stills/PNG and an explicit
    // f=image2 profile. They must share the exact same native output contract.
    bool referencedPreset = false, explicitPreset = false;
    int variant = 0;
    for (const auto &name : RenderPresetRepository::get()->getAllPresets()) {
        const auto &candidate = RenderPresetRepository::get()->getPreset(name);
        if (!candidate || !candidate->isValid() || candidate->extension() != QStringLiteral("png") || !candidate->params().isImageSequence()) continue;
        const auto params = candidate->params();
        referencedPreset = referencedPreset || params.value(QStringLiteral("properties")).startsWith(QStringLiteral("stills/"));
        explicitPreset = explicitPreset || params.value(QStringLiteral("f")) == QStringLiteral("image2");
        if (name == preset) continue;
        const QString variantPath = directory.filePath(QStringLiteral("png-variant-%1").arg(variant++));
        const auto rendered = exportTerminal(project, exportStart(project, variantPath, name));
        REQUIRE(rendered.value(QStringLiteral("status")).toString() == QStringLiteral("completed"));
        const auto variantFiles = QDir(variantPath).entryList({QStringLiteral("*.png")}, QDir::Files, QDir::Name);
        REQUIRE(variantFiles.size() == 8);
        const QImage first(QDir(variantPath).filePath(variantFiles.first())), last(QDir(variantPath).filePath(variantFiles.last()));
        REQUIRE_FALSE(first.isNull());
        REQUIRE_FALSE(last.isNull());
        CHECK(QColor(first.pixel(first.width() / 2, first.height() / 2)).red() > 240);
        CHECK(QColor(last.pixel(last.width() / 2, last.height() / 2)).blue() > 240);
    }
    CHECK(referencedPreset);
    CHECK(explicitPreset);
    project.rejectWithoutChange(QStringLiteral("start_render"), exportArgs(project, target, preset));
    CHECK(QDir(target).entryList({QStringLiteral("*.png")}, QDir::Files).size() == 8);
    exportGuide(project, 0, QStringLiteral("A/B%05d"));
    exportGuide(project, 4, QStringLiteral("a\\b%05d"));
    const auto sourceLabels = project.timeline->getGuideModel()->toJson();
    const QString sectionsPath = directory.filePath(QStringLiteral("guide-sections"));
    const auto sections = exportTerminal(project, exportStart(project, sectionsPath, preset, {{QStringLiteral("guideSections"), true}}));
    REQUIRE(sections.value(QStringLiteral("status")).toString() == QStringLiteral("completed"));
    REQUIRE(sections.value(QStringLiteral("sections")).toArray().size() == 2);
    REQUIRE(sections.value(QStringLiteral("outputs")).toArray().size() == 2);
    for (const auto &entry : sections.value(QStringLiteral("outputs")).toArray()) {
        const auto output = entry.toObject();
        CHECK(output.value(QStringLiteral("frameCount")).toInt() == 4);
        CHECK(QFileInfo(output.value(QStringLiteral("path")).toString()).absolutePath() == sectionsPath);
    }
    CHECK(QDir(sectionsPath).entryList({QStringLiteral("*.png")}, QDir::Files).size() == 8);
    CHECK(project.timeline->getGuideModel()->toJson() == sourceLabels);
    CHECK(QDir(directory.path()).entryList({QStringLiteral(".kdenlive-mcp-render-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

TEST_CASE("MCP multioutput publication preserves concurrent files and cancels session queues", "[MCP][MCPLifecycle][MCPRender][MCPAdvancedExport]")
{
    ExportMelt melt;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 8));
    const QString preset = exportPreset(true);
    const QString target = directory.filePath(QStringLiteral("concurrent"));
    const QString id = exportStart(project, target, preset);
    REQUIRE(QDir().mkdir(target));
    const QString sentinelPath = QDir(target).filePath(QStringLiteral("sentinel.txt"));
    {
        QFile sentinel(sentinelPath);
        REQUIRE(sentinel.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        REQUIRE(sentinel.write("preserve-original") == 17);
    }
    const auto failed = exportTerminal(project, id);
    REQUIRE(failed.value(QStringLiteral("status")).toString() == QStringLiteral("failed"));
    REQUIRE(QFileInfo(failed.value(QStringLiteral("recoveryPath")).toString()).isDir());
    CHECK(exportRead(sentinelPath) == QByteArray("preserve-original"));
    CHECK(QDir(target).entryList(QDir::Files).size() == 1);
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 8, 10000));
    const QString firstPath = directory.filePath(QStringLiteral("canceled-sequence"));
    const QString secondPath = directory.filePath(QStringLiteral("dependent-sequence"));
    const QString first = exportStart(project, firstPath, preset);
    const QString second =
        exportStart(project, secondPath, preset, {{QStringLiteral("inFrame"), 0}, {QStringLiteral("outFrame"), 3}, {QStringLiteral("queueAfterJobId"), first}});
    auto cancel = project.expected();
    cancel.insert(QStringLiteral("jobId"), first);
    project.success(QStringLiteral("cancel_render"), cancel);
    CHECK(exportTerminal(project, first).value(QStringLiteral("status")).toString() == QStringLiteral("canceled"));
    CHECK(exportTerminal(project, second).value(QStringLiteral("status")).toString() == QStringLiteral("canceled"));
    CHECK_FALSE(QFileInfo::exists(firstPath));
    CHECK_FALSE(QFileInfo::exists(secondPath));
    CHECK(exportRead(sentinelPath) == QByteArray("preserve-original"));
    const QString after =
        exportStart(project, directory.filePath(QStringLiteral("after-completed")), preset, {{QStringLiteral("inFrame"), 0}, {QStringLiteral("outFrame"), 1}});
    REQUIRE(exportTerminal(project, after).value(QStringLiteral("status")).toString() == QStringLiteral("completed"));
    const QString queued = exportStart(project, directory.filePath(QStringLiteral("queued-success")), preset,
                                       {{QStringLiteral("inFrame"), 2}, {QStringLiteral("outFrame"), 3}, {QStringLiteral("queueAfterJobId"), after}});
    const auto done = exportTerminal(project, queued);
    CHECK(done.value(QStringLiteral("status")).toString() == QStringLiteral("completed"));
    CHECK(done.value(QStringLiteral("outputs")).toArray().first().toObject().value(QStringLiteral("frameCount")).toInt() == 2);
}

TEST_CASE("MCP exports native per-track WAV stems with channel settings and safe names", "[MCP][MCPLifecycle][MCPRender][MCPAdvancedExport]")
{
    ExportMelt melt;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    const QString source = wavFixture(directory);
    const QByteArray original = exportRead(source);
    const QString binId = importFixture(project, source);
    std::vector<int> audioTracks;
    for (int id : project.timeline->getAllTracksIds())
        if (project.timeline->isAudioTrack(id)) audioTracks.push_back(id);
    REQUIRE(audioTracks.size() >= 2);
    for (int index = 0; index < 2; ++index) {
        project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, audioTracks[index], 0, 0, 12, QStringLiteral("audio")));
        auto rename = project.expected();
        rename.insert(QStringLiteral("trackId"), audioTracks[index]);
        rename.insert(QStringLiteral("name"), index == 0 ? QStringLiteral("../outside%05d") : QStringLiteral("NUL:<bad>|"));
        project.success(QStringLiteral("rename_track"), rename);
    }
    const int undoIndex = project.undoStack->index();
    const QString target = directory.filePath(QStringLiteral("stems"));
    const auto result = exportTerminal(
        project, exportStart(project, target, exportPreset(false, true), {{QStringLiteral("audioFilePerTrack"), true}, {QStringLiteral("audioChannels"), 1}}));
    REQUIRE(result.value(QStringLiteral("status")).toString() == QStringLiteral("completed"));
    REQUIRE(result.value(QStringLiteral("outputs")).toArray().size() == 2);
    REQUIRE(QDir(target).entryList({QStringLiteral("*.wav")}, QDir::Files).size() == 2);
    for (const auto &entry : result.value(QStringLiteral("outputs")).toArray()) {
        const auto output = entry.toObject();
        const QString path = output.value(QStringLiteral("path")).toString();
        REQUIRE(QFileInfo(path).absolutePath() == target);
        const QByteArray wav = exportRead(path);
        REQUIRE(wav.startsWith("RIFF"));
        const int fmt = wav.indexOf("fmt ");
        REQUIRE(fmt >= 0);
        REQUIRE(wav.size() > fmt + 12);
        const int channels = static_cast<unsigned char>(wav.at(fmt + 10)) | (static_cast<unsigned char>(wav.at(fmt + 11)) << 8);
        CHECK(channels == 1);
        Mlt::Producer rendered(pCore->getProjectProfile(), path.toUtf8().constData());
        REQUIRE(rendered.is_valid());
        CHECK(std::abs(rendered.get_length() - 12) <= 1);
        rendered.seek(3);
        std::unique_ptr<Mlt::Frame> frame(rendered.get_frame());
        REQUIRE(frame);
        mlt_audio_format format = mlt_audio_f32le;
        int frequency = 48000, count = 1, samples = 1920;
        const float *values = static_cast<float *>(frame->get_audio(format, frequency, count, samples));
        REQUIRE(values);
        double energy = 0;
        for (int i = 0; i < samples * count; ++i)
            energy += values[i] * values[i];
        CHECK(energy > 0.1);
    }
    CHECK(exportRead(source) == original);
    CHECK(project.undoStack->index() == undoIndex);
    CHECK_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral("outside%05d.wav"))));
}
TEST_CASE("MCP full render queues preserve completed prerequisites during bounded eviction", "[MCP][MCPLifecycle][MCPRender][MCPAdvancedExport]")
{
    ExportMelt melt;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 2));
    const QString preset = exportPreset(true);
    const QString completed = exportStart(project, directory.filePath(QStringLiteral("prerequisite")), preset);
    REQUIRE(exportTerminal(project, completed).value(QStringLiteral("status")).toString() == QStringLiteral("completed"));
    const auto dependency = QJsonObject{{QStringLiteral("queueAfterJobId"), completed}};
    const auto cancel = [&project](const QString &id) { project.success(QStringLiteral("cancel_render"), {{QStringLiteral("jobId"), id}}); };
    // Deferred jobs are canceled before their timer runs, so this exercises the
    // real bounded job table without launching 126 unnecessary encoder children.
    for (int i = 0; i < 63; ++i) {
        const QString id = exportStart(project, directory.filePath(QStringLiteral("discard-%1").arg(i)), preset, dependency);
        cancel(id);
    }
    REQUIRE(project.success(QStringLiteral("render_status")).value(QStringLiteral("jobs")).toArray().size() == 64);
    QStringList waiting;
    for (int i = 0; i < 63; ++i)
        waiting.append(exportStart(project, directory.filePath(QStringLiteral("waiting-%1").arg(i)), preset, dependency));
    CHECK(project.success(QStringLiteral("render_status"), {{QStringLiteral("jobId"), completed}})
              .value(QStringLiteral("job"))
              .toObject()
              .value(QStringLiteral("status"))
              .toString() == QStringLiteral("completed"));
    const auto full = project.call(QStringLiteral("start_render"), exportArgs(project, directory.filePath(QStringLiteral("too-many")), preset, dependency));
    CHECK(full.value(QStringLiteral("isError")).toBool());
    for (const auto &id : waiting)
        cancel(id);
    QCoreApplication::processEvents();
    CHECK_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral("too-many"))));
    CHECK(QDir(directory.path()).entryList({QStringLiteral("waiting-*")}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    CHECK(QDir(directory.path()).entryList({QStringLiteral(".kdenlive-mcp-render-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}
