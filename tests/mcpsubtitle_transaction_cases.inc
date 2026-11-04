/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Native ASS import transactions: exact metadata/events, history and native persistence.
#include <QUndoGroup>
namespace {
QByteArray assTransactionFixture()
{
    return "[Script Info]\nTitle: R28 imported metadata\nScriptType: v4.00+\n\n"
           "[V4+ Styles]\nStyle: Accent,Arial,37.001,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,11,12,1\n\n"
           "[Kdenlive Extradata]\nMaxLayer: 3\nDefaultStyles: Accent,Default,Accent,Accent\n\n"
           "[Fonts]\nfontname: r28-probe-font.ttf\n!!!!\n\n"
           "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
           "Dialogue: 0,0:00:00.40,0:00:01.40,Accent,Actor,12,13,14,fx,Imported\n"
           "Comment: 3,0:00:02.04,0:00:03.24,Default,,0,0,0,,Second\n";
}
QByteArray nativeAssSnapshot(const std::shared_ptr<SubtitleModel> &model, const QTemporaryDir &directory)
{
    const QString path = directory.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".ass"));
    REQUIRE(model->saveSubtitleData(model->toJson(), path) == model->count());
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
QString assTransactionFile(const QTemporaryDir &directory, const QByteArray &bytes)
{
    const QString path = directory.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".ass"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
    return path;
}
void checkImportedAssMetadata(const std::shared_ptr<SubtitleModel> &model, const QTemporaryDir &directory)
{
    CHECK(model->scriptInfo().at(QStringLiteral("Title")) == QStringLiteral("R28 imported metadata"));
    CHECK(model->getMaxLayer() == 3);
    CHECK(model->getLayerDefaultStyle(0) == QStringLiteral("Accent"));
    CHECK(model->getLayerDefaultStyle(1) == QStringLiteral("Default"));
    CHECK(model->getLayerDefaultStyle(2) == QStringLiteral("Accent"));
    CHECK(model->getLayerDefaultStyle(3) == QStringLiteral("Accent"));
    CHECK(model->getSubtitleStyle(QStringLiteral("Accent")).bold());
    const QByteArray bytes = nativeAssSnapshot(model, directory);
    CHECK(bytes.contains("[Fonts]\nfontname: r28-probe-font.ttf\n!!!!\n"));
}
void saveAssNativeProject(McpProject &project, const std::shared_ptr<SubtitleModel> &model, const QString &path)
{
    project.document->setSequenceProperty(project.timeline->uuid(), QStringLiteral("subtitlesList"), model->subtitlesFilesToJson());
    for (const auto &entry : model->getSubtitlesList().keys()) {
        QString finalName = path;
        if (project.timeline->uuid() != project.document->uuid()) finalName += project.timeline->uuid().toString();
        if (entry.first > 0) finalName += QStringLiteral("-%1").arg(entry.first);
        const QFileInfo info(finalName);
        model->copySubtitle(info.dir().absoluteFilePath(info.fileName() + QStringLiteral(".ass")), entry.first, false, true);
    }
    REQUIRE(pCore->projectManager()->testSaveFileAs(path));
    const int active = project.document->getSequenceProperty(project.timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt();
    model->restoreTmpFile(active);
    project.document->setUrl(QUrl::fromLocalFile(path));
}
} // namespace

TEST_CASE("Native ASS import owns complete metadata and stable event IDs in one undo", "[mcp][subtitle][ass-transaction]")
{
    for (const bool preexisting : {false, true}) {
        CAPTURE(preexisting);
        McpProject project;
        project.open();
        project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
        QTemporaryDir directory;
        REQUIRE(directory.isValid());
        int originalId = -1, videoId = -1;
        std::unordered_set<int> group;
        if (preexisting) {
            videoId = project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 75)).value(QStringLiteral("clipId")).toInt();
            auto args = project.expected();
            args.insert(QStringLiteral("operation"), QStringLiteral("add"));
            args.insert(QStringLiteral("startFrame"), 2);
            args.insert(QStringLiteral("endFrame"), 8);
            args.insert(QStringLiteral("text"), QStringLiteral("Existing grouped event"));
            originalId = project.success(QStringLiteral("edit_subtitle"), args).value(QStringLiteral("id")).toInt();
            REQUIRE(project.timeline->requestClipsGroup({videoId, originalId}) >= 0);
            REQUIRE(project.timeline->requestClearSelection());
            group = project.timeline->getGroupElements(originalId);
        }
        const auto model = project.timeline->hasSubtitleModel() ? project.timeline->getSubtitleModel() : project.timeline->createSubtitleModel();
        REQUIRE(model);
        const auto originalIds = model->getAllSubIds();
        const auto beforeScript = model->scriptInfo();
        const int beforeMax = model->getMaxLayer(), beforeActive = model->activeSubLayer();
        const auto beforeBytes = nativeAssSnapshot(model, directory);
        const int beforeUndo = project.undoStack->index();
        const QString path = assTransactionFile(directory, assTransactionFixture());
        QObject observer;
        int changes = 0;
        QObject::connect(model.get(), &SubtitleModel::modelChanged, &observer, [&] { ++changes; });
        const auto result = model->importSubtitleWithUndo(path, 0, true, 25, 25, QByteArrayLiteral("UTF-8"), 2);
        REQUIRE(result.success);
        CHECK(result.importedCount == 2);
        CHECK(project.undoStack->index() == beforeUndo + 1);
        CHECK(changes == 1);
        REQUIRE(model->count() == int(originalIds.size()) + 2);
        const auto importedIds = model->getAllSubIds();
        const int imported = model->getIdForStartPos(0, GenTime(10, 25));
        REQUIRE(imported >= 0);
        CHECK(model->getSubtitleEnd(imported) == 35);
        CHECK(model->getName(imported) == QStringLiteral("Actor"));
        CHECK(model->getMarginL(imported) == 12);
        CHECK(model->getMarginR(imported) == 13);
        CHECK(model->getMarginV(imported) == 14);
        CHECK(model->getEffects(imported) == QStringLiteral("fx"));
        CHECK(model->getSubtitleStyle(QStringLiteral("Accent")).fontSize() == 37.001);
        checkImportedAssMetadata(model, directory);
        const auto afterBytes = nativeAssSnapshot(model, directory);
        model->setActiveSubLayer(3);
        project.undoStack->undo();
        CHECK(changes == 2);
        CHECK(model->getAllSubIds() == originalIds);
        CHECK(model->scriptInfo() == beforeScript);
        CHECK(model->getMaxLayer() == beforeMax);
        CHECK(model->activeSubLayer() == beforeActive);
        CHECK(nativeAssSnapshot(model, directory) == beforeBytes);
        if (preexisting) CHECK(project.timeline->getGroupElements(originalId) == group);
        REQUIRE(project.timeline->checkConsistency());
        project.undoStack->redo();
        CHECK(changes == 3);
        CHECK(model->getAllSubIds() == importedIds);
        CHECK(model->getSubtitleStyle(QStringLiteral("Accent")).fontSize() == 37.001);
        CHECK(nativeAssSnapshot(model, directory) == afterBytes);
        if (preexisting) CHECK(project.timeline->getGroupElements(originalId) == group);
        REQUIRE(project.timeline->checkConsistency());
        model->switchLocked();
        const auto locked = model->importSubtitleWithUndo(path, 0, true, 25, 25);
        CHECK_FALSE(locked.success);
        CHECK(changes == 3);
        CHECK(project.undoStack->index() == beforeUndo + 1);
        CHECK(nativeAssSnapshot(model, directory) == afterBytes);
        model->switchLocked();
    }
}

TEST_CASE("Native ASS parser failures conflicts and count mismatches preserve metadata and prior redo", "[mcp][subtitle][ass-transaction][atomic]")
{
    McpProject project;
    project.open();
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("startFrame"), 2);
    args.insert(QStringLiteral("endFrame"), 8);
    args.insert(QStringLiteral("text"), QStringLiteral("Redo identity"));
    const int original = project.success(QStringLiteral("edit_subtitle"), args).value(QStringLiteral("id")).toInt();
    const auto model = project.timeline->getSubtitleModel();
    REQUIRE(model);
    project.undoStack->undo();
    REQUIRE(project.undoStack->canRedo());
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QByteArray before = nativeAssSnapshot(model, directory);
    const auto ids = model->getAllSubIds();
    const int index = project.undoStack->index(), count = project.undoStack->count();
    const QList<QByteArray> invalid{
        assTransactionFixture() + "Dialogue: 0,bad,0:00:09.00,Accent,,0,0,0,,Late malformed\n",
        assTransactionFixture() + "Dialogue: 0,0:00:00.40,0:00:09.00,Accent,,0,0,0,,Duplicate start\n",
        QByteArray(assTransactionFixture()).replace("0:00:00.40", "999:00:00.40"),
        QByteArray(assTransactionFixture()).replace("Actor,12,13,14", "Actor,bad,13,14"),
        QByteArray(assTransactionFixture()).replace("Style: Accent,Arial,37.001", "Style: Accent,Arial,NaN"),
        // These must fail even with expectedCount left at its GUI default -1.
        QByteArray(assTransactionFixture()).replace("\n\n[Events]", "\n[Events]"),
        QByteArray(assTransactionFixture()).replace("\n\n[Kdenlive Extradata]", "\n[Kdenlive Extradata]"),
        assTransactionFixture() + QByteArray::fromHex("e282")};
    for (const auto &bytes : invalid) {
        const auto result = model->importSubtitleWithUndo(assTransactionFile(directory, bytes), 0, true, 25, 25);
        CHECK_FALSE(result.success);
        CHECK(result.importedCount == 0);
        CHECK(nativeAssSnapshot(model, directory) == before);
        CHECK(model->getAllSubIds() == ids);
        CHECK(project.undoStack->index() == index);
        CHECK(project.undoStack->count() == count);
        CHECK(project.undoStack->canRedo());
        REQUIRE(project.timeline->checkConsistency());
    }
    const QString valid = assTransactionFile(directory, assTransactionFixture());
    const auto mismatch = model->importSubtitleWithUndo(valid, 0, true, 25, 25, QByteArrayLiteral("UTF-8"), 3);
    CHECK_FALSE(mismatch.success);
    CHECK(nativeAssSnapshot(model, directory) == before);
    CHECK(project.undoStack->index() == index);
    CHECK(project.undoStack->count() == count);
    REQUIRE(project.undoStack->canRedo());
    project.undoStack->redo();
    REQUIRE(model->hasSubtitle(original));
    CHECK(model->getText(original) == QStringLiteral("Redo identity"));
    // Existing-event collisions are also rejected before metadata is applied.
    const QByteArray collision = QByteArray(assTransactionFixture()).replace("0:00:00.40", "0:00:00.08");
    const QByteArray existing = nativeAssSnapshot(model, directory);
    const auto conflict = model->importSubtitleWithUndo(assTransactionFile(directory, collision), 0, true, 25, 25);
    CHECK_FALSE(conflict.success);
    CHECK(nativeAssSnapshot(model, directory) == existing);
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("Native metadata-only ASS imports undo without event identity changes", "[mcp][subtitle][ass-transaction][metadata]")
{
    McpProject project;
    project.open();
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    const auto model = project.timeline->createSubtitleModel();
    REQUIRE(model);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto before = nativeAssSnapshot(model, directory);
    const QByteArray metadata = assTransactionFixture().left(assTransactionFixture().indexOf("[Events]"));
    const int undo = project.undoStack->index();
    const auto result = model->importSubtitleWithUndo(assTransactionFile(directory, metadata), 0, true, 25, 25, QByteArrayLiteral("UTF-8"), 0);
    REQUIRE(result.success);
    CHECK(result.importedCount == 0);
    CHECK(model->count() == 0);
    CHECK(project.undoStack->index() == undo + 1);
    checkImportedAssMetadata(model, directory);
    const auto after = nativeAssSnapshot(model, directory);
    project.undoStack->undo();
    CHECK(nativeAssSnapshot(model, directory) == before);
    project.undoStack->redo();
    CHECK(nativeAssSnapshot(model, directory) == after);
    CHECK(model->count() == 0);
}

TEST_CASE("Native checked text subtitle counts roll back before replacing redo history", "[mcp][subtitle][ass-transaction][counts]")
{
    McpProject project;
    project.open();
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("startFrame"), 2);
    args.insert(QStringLiteral("endFrame"), 8);
    args.insert(QStringLiteral("text"), QStringLiteral("Original redo"));
    const int id = project.success(QStringLiteral("edit_subtitle"), args).value(QStringLiteral("id")).toInt();
    const auto model = project.timeline->getSubtitleModel();
    project.undoStack->undo();
    REQUIRE(project.undoStack->canRedo());
    const int index = project.undoStack->index(), count = project.undoStack->count();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("count.srt"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write("1\n00:00:00,400 --> 00:00:01,400\nOne\n\n2\n00:00:02,040 --> 00:00:03,240\nTwo\n") > 0);
    file.close();
    const auto result = model->importSubtitleWithUndo(path, 0, true, 25, 25, QByteArrayLiteral("UTF-8"), 3);
    CHECK_FALSE(result.success);
    CHECK(model->count() == 0);
    CHECK(project.undoStack->index() == index);
    CHECK(project.undoStack->count() == count);
    REQUIRE(project.undoStack->canRedo());
    project.undoStack->redo();
    CHECK(model->hasSubtitle(id));
    CHECK(model->getText(id) == QStringLiteral("Original redo"));
    REQUIRE(project.timeline->checkConsistency());
}
TEST_CASE("ASS import metadata survives language history Save As and native reopen", "[mcp][subtitle][ass-transaction][language][save]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    const int video = project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 100)).value(QStringLiteral("clipId")).toInt();
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("startFrame"), 2);
    args.insert(QStringLiteral("endFrame"), 8);
    args.insert(QStringLiteral("text"), QStringLiteral("Retained before import"));
    const int original = project.success(QStringLiteral("edit_subtitle"), args).value(QStringLiteral("id")).toInt();
    auto model = project.timeline->getSubtitleModel();
    REQUIRE(model);
    REQUIRE(project.timeline->requestClipsGroup({video, original}) >= 0);
    REQUIRE(project.timeline->requestClearSelection());
    const auto group = project.timeline->getGroupElements(original);
    const int beforeImport = project.undoStack->index();
    const auto beforeBytes = nativeAssSnapshot(model, directory);
    const QByteArray input = QByteArray(assTransactionFixture()).replace("37.001", "37.25");
    const QString source = assTransactionFile(directory, input);
    // Exercise the unchanged native GUI-facing signature as well as the checked API.
    model->importSubtitle(source, 0, true, 25, 25, QByteArrayLiteral("UTF-8"));
    REQUIRE(project.undoStack->index() == beforeImport + 1);
    REQUIRE(model->count() == 3);
    const auto originalImportIds = model->getAllSubIds();
    checkImportedAssMetadata(model, directory);
    CHECK(model->getSubtitleStyle(QStringLiteral("Accent")).fontSize() == 37.25);
    const int afterImport = project.undoStack->index();
    REQUIRE(model->createNewSubtitle(QStringLiteral("Imported language B"), 0) > 0);
    int languageB = -1;
    for (const auto &key : model->getSubtitlesList().keys()) if (key.first > 0) languageB = key.first;
    REQUIRE(languageB > 0);
    REQUIRE(model->activateSubtitle(languageB));
    checkImportedAssMetadata(model, directory);
    const int b = model->getIdForStartPos(0, GenTime(10, 25));
    REQUIRE(b >= 0);
    REQUIRE(model->setText(b, QStringLiteral("Imported language B edited")));
    REQUIRE(model->activateSubtitle(0));
    REQUIRE(model->count() == 3);
    checkImportedAssMetadata(model, directory);
    const auto savedIds = model->getAllSubIds();
    const QString saveA = directory.filePath(QStringLiteral("ass-save-A.kdenlive"));
    const QString saveB = directory.filePath(QStringLiteral("ass-save-B.kdenlive"));
    saveAssNativeProject(project, model, saveA);
    saveAssNativeProject(project, model, saveA);
    const QString a0 = project.document->subTitlePath(project.timeline->uuid(), 0, true);
    const QString a1 = project.document->subTitlePath(project.timeline->uuid(), languageB, true);
    const auto read = [](const QString &path) {
        QFile file(path);
        REQUIRE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    };
    const auto savedA0 = read(a0), savedA1 = read(a1);
    CHECK(savedA0.contains("[Fonts]\nfontname: r28-probe-font.ttf\n!!!!\n"));
    CHECK(savedA1.contains("Imported language B edited"));
    saveAssNativeProject(project, model, saveB);
    const QString b0 = project.document->subTitlePath(project.timeline->uuid(), 0, true);
    const QString b1 = project.document->subTitlePath(project.timeline->uuid(), languageB, true);
    const auto savedB0 = read(b0), savedB1 = read(b1);
    const QString work0 = project.document->subTitlePath(project.timeline->uuid(), 0, false);
    const QString work1 = project.document->subTitlePath(project.timeline->uuid(), languageB, false);
    const int savedIndex = project.undoStack->index();
    while (project.undoStack->index() > afterImport) project.undoStack->undo();
    CHECK(model->getAllSubIds() == originalImportIds);
    CHECK(project.timeline->getGroupElements(original) == group);
    project.undoStack->undo();
    REQUIRE(project.undoStack->index() == beforeImport);
    CHECK(model->getAllSubIds() == std::unordered_set<int>{original});
    CHECK(project.timeline->getGroupElements(original) == group);
    CHECK(nativeAssSnapshot(model, directory) == beforeBytes);
    CHECK(read(a0) == savedA0);
    CHECK(read(a1) == savedA1);
    CHECK(read(b0) == savedB0);
    CHECK(read(b1) == savedB1);
    REQUIRE(project.timeline->checkConsistency());
    while (project.undoStack->index() < savedIndex) project.undoStack->redo();
    CHECK(model->getAllSubIds() == savedIds);
    checkImportedAssMetadata(model, directory);
    CHECK(model->getSubtitleStyle(QStringLiteral("Accent")).fontSize() == 37.25);
    REQUIRE(project.timeline->checkConsistency());
    const QUuid sequence = project.timeline->uuid();
    model.reset();
    project.timeline.reset();
    REQUIRE(pCore->projectManager()->closeCurrentDocument(false, false));
    for (const QString &path : {saveA, saveB}) {
        // Force native reconstruction from the published file, not the live cache.
        QFile::remove(work0);
        QFile::remove(work1);
        QUndoGroup undoGroup;
        auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(path), directory.path(), &undoGroup, false, nullptr);
        REQUIRE(opened.isSuccessful());
        auto document = opened.getDocument();
        pCore->projectManager()->testSetDocument(document.get());
        REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(path).lastModified(), false));
        const auto sequences = pCore->projectItemModel()->getAllSequenceClips();
        REQUIRE(sequences.contains(sequence));
        REQUIRE(pCore->projectManager()->openTimeline(sequences.value(sequence), -1, sequence));
        auto timeline = document->getTimeline(sequence);
        REQUIRE(timeline);
        pCore->projectManager()->testSetActiveTimeline(timeline);
        document->loading = false;
        auto subtitles = timeline->getSubtitleModel();
        REQUIRE(subtitles);
        REQUIRE(subtitles->count() == 3);
        checkImportedAssMetadata(subtitles, directory);
        CHECK(subtitles->getSubtitleStyle(QStringLiteral("Accent")).fontSize() == 37.25);
        const int a = subtitles->getIdForStartPos(0, GenTime(10, 25));
        REQUIRE(a >= 0);
        CHECK(subtitles->getText(a) == QStringLiteral("Imported"));
        REQUIRE(subtitles->activateSubtitle(languageB));
        REQUIRE(subtitles->count() == 3);
        checkImportedAssMetadata(subtitles, directory);
        const int restoredB = subtitles->getIdForStartPos(0, GenTime(10, 25));
        REQUIRE(restoredB >= 0);
        CHECK(subtitles->getText(restoredB) == QStringLiteral("Imported language B edited"));
        CHECK(subtitles->getSubtitleStyle(QStringLiteral("Accent")).fontSize() == 37.25);
        REQUIRE(timeline->checkConsistency());
        subtitles.reset();
        timeline.reset();
        REQUIRE(pCore->projectManager()->closeCurrentDocument(false, false));
    }
}


TEST_CASE("Native ASS transactions reject each invalid individual frame rate before changing history", "[mcp][subtitle][ass-transaction][fps]")
{
    McpProject project;
    project.open();
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("startFrame"), 2);
    args.insert(QStringLiteral("endFrame"), 8);
    args.insert(QStringLiteral("text"), QStringLiteral("FPS rejection redo"));
    const int id = project.success(QStringLiteral("edit_subtitle"), args).value(QStringLiteral("id")).toInt();
    const auto model = project.timeline->getSubtitleModel();
    REQUIRE(model);
    project.undoStack->undo();
    REQUIRE(project.undoStack->canRedo());
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = assTransactionFile(directory, assTransactionFixture());
    const QByteArray before = nativeAssSnapshot(model, directory);
    const auto script = model->scriptInfo();
    const int index = project.undoStack->index(), count = project.undoStack->count();
    struct Rates { float source; float target; };
    const Rates invalid[] = {{0, 25}, {25, 0}, {-25, 25}, {25, -25}, {-25, -25},
                             {std::numeric_limits<float>::quiet_NaN(), 25}, {25, std::numeric_limits<float>::quiet_NaN()},
                             {std::numeric_limits<float>::infinity(), 25}, {25, std::numeric_limits<float>::infinity()}};
    for (const auto &rates : invalid) {
        CAPTURE(rates.source, rates.target);
        const auto result = model->importSubtitleWithUndo(path, 0, true, rates.source, rates.target);
        CHECK_FALSE(result.success);
        CHECK(result.importedCount == 0);
        CHECK(model->count() == 0);
        CHECK(model->scriptInfo() == script);
        CHECK(nativeAssSnapshot(model, directory) == before);
        CHECK(project.undoStack->index() == index);
        CHECK(project.undoStack->count() == count);
        CHECK(project.undoStack->canRedo());
        REQUIRE(project.timeline->checkConsistency());
    }
    project.undoStack->redo();
    CHECK(model->hasSubtitle(id));
    CHECK(model->getText(id) == QStringLiteral("FPS rejection redo"));
}
