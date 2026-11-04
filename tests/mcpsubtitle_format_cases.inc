/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Native subtitle formats, codecs, timeline conversion and all-or-nothing intake.
#include <QStringConverter>

namespace {
QString subtitleFormatFixture(const QString &format, const QString &text)
{
    if (format == QLatin1String("ass"))
        return QStringLiteral("[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
                              "Dialogue: 0,0:00:00.20,0:00:01.20,Default,,0,0,0,,%1\n"
                              "Comment: 0,0:00:02.04,0:00:03.24,Default,Actor,12,13,14,fx,Second\n").arg(text);
    if (format == QLatin1String("srt"))
        return QStringLiteral("1\n00:00:00,200 --> 00:00:01,200\n%1\n\n2\n00:00:02,040 --> 00:00:03,240\nSecond\n").arg(text);
    if (format == QLatin1String("vtt"))
        return QStringLiteral("WEBVTT\n\n00:00.200 --> 00:01.200\n%1\n\n00:02.040 --> 00:03.240\nSecond\n").arg(text);
    return QStringLiteral("0:00:00.200,0:00:01.200\n%1\n\n0:00:02.040,0:00:03.240\nSecond\n").arg(text);
}
QString subtitleFileFixture(const QTemporaryDir &directory, const QString &name, const QByteArray &contents)
{
    const QString path = directory.filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    REQUIRE(file.write(contents) == contents.size());
    file.close();
    return path;
}
QJsonObject subtitleFileArgs(McpProject &project, const QString &operation, const QString &path)
{
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), operation);
    args.insert(QStringLiteral("path"), path);
    return args;
}
QString subtitleUnicode()
{
    return QStringLiteral("caf\u00e9 \u05e9\u05dc\u05d5\u05dd \u65e5\u672c \U0001f642");
}
QStringList subtitleSerializedStyles(const std::shared_ptr<SubtitleModel> &model)
{
    QStringList result;
    for (const auto &entry : model->getAllSubtitleStyles()) result.append(entry.second.toString(entry.first));
    return result;
}
} // namespace

TEST_CASE("Native subtitle file formats preserve Unicode metadata frame conversion and one undo", "[mcp][subtitle][formats]")
{
    for (const QString &format : {QStringLiteral("ass"), QStringLiteral("srt"), QStringLiteral("vtt"), QStringLiteral("sbv")}) {
        CAPTURE(format);
        McpProject project;
        project.open();
        QTemporaryDir directory;
        REQUIRE(directory.isValid());
        project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
        const QString path = subtitleFileFixture(directory, QStringLiteral("source.") + format.toUpper(), subtitleFormatFixture(format, subtitleUnicode()).toUtf8());
        auto args = subtitleFileArgs(project, QStringLiteral("import"), path);
        args.insert(QStringLiteral("sourceFps"), 50);
        args.insert(QStringLiteral("offsetFrames"), 7);
        const int undoIndex = project.undoStack->index();
        const auto imported = project.success(QStringLiteral("subtitle_file"), args);
        CHECK(imported.value(QStringLiteral("importedCount")).toInt() == 2);
        CHECK(imported.value(QStringLiteral("format")).toString() == format);
        const auto model = project.timeline->getSubtitleModel();
        REQUIRE(model);
        REQUIRE(model->count() == 2);
        const int first = model->getIdForStartPos(0, GenTime(17, 25));
        const int second = model->getIdForStartPos(0, GenTime(109, 25));
        REQUIRE(first >= 0);
        REQUIRE(second >= 0);
        CHECK(model->getSubtitleEnd(first) == 67);
        CHECK(model->getSubtitleEnd(second) == 169);
        CHECK(model->getText(first) == subtitleUnicode());
        CHECK(model->getText(second) == QStringLiteral("Second"));
        if (format == QLatin1String("ass")) {
            CHECK_FALSE(model->getIsDialogue(second));
            CHECK(model->getName(second) == QStringLiteral("Actor"));
            CHECK(model->getMarginL(second) == 12);
            CHECK(model->getMarginR(second) == 13);
            CHECK(model->getMarginV(second) == 14);
            CHECK(model->getEffects(second) == QStringLiteral("fx"));
        }
        CHECK(project.undoStack->index() == undoIndex + 1);
        const auto ids = model->getAllSubIds();
        project.success(QStringLiteral("undo"), project.expected());
        CHECK(model->count() == 0);
        CHECK(project.undoStack->index() == undoIndex);
        project.success(QStringLiteral("redo"), project.expected());
        CHECK(model->getAllSubIds() == ids);
        CHECK(model->getText(first) == subtitleUnicode());
        REQUIRE(project.timeline->checkConsistency());
    }
}

TEST_CASE("Native subtitle Unicode codecs and BOM precedence agree with the selected Qt codec", "[mcp][subtitle][formats][encoding]")
{
    struct EncodingCase { QStringConverter::Encoding codec; bool bom; bool selectCodec; };
    const EncodingCase cases[] = {
        {QStringConverter::Utf8, false, false}, {QStringConverter::Utf8, true, true},
        {QStringConverter::Utf16LE, true, false}, {QStringConverter::Utf16BE, true, false},
        {QStringConverter::Utf16LE, false, true}, {QStringConverter::Utf16BE, false, true},
        {QStringConverter::Utf32LE, true, false}, {QStringConverter::Utf32BE, true, false},
        {QStringConverter::Utf32LE, false, true}, {QStringConverter::Utf32BE, false, true},
        {QStringConverter::Latin1, false, true}};
    for (const auto &example : cases) {
        CAPTURE(example.codec, example.bom, example.selectCodec);
        McpProject project;
        project.open();
        QTemporaryDir directory;
        REQUIRE(directory.isValid());
        project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
        const QString expected = example.codec == QStringConverter::Latin1 ? QStringLiteral("caf\u00e9 \u00a3") : subtitleUnicode();
        QStringEncoder encoder(example.codec, example.bom ? QStringConverter::Flag::WriteBom : QStringConverter::Flag::Default);
        const QByteArray bytes = encoder.encode(subtitleFormatFixture(QStringLiteral("srt"), expected));
        REQUIRE_FALSE(encoder.hasError());
        const QString path = subtitleFileFixture(directory, QStringLiteral("codec.srt"), bytes);
        auto args = subtitleFileArgs(project, QStringLiteral("import"), path);
        if (example.selectCodec) args.insert(QStringLiteral("encoding"), QString::fromLatin1(QStringConverter::nameForEncoding(example.codec)));
        const auto imported = project.success(QStringLiteral("subtitle_file"), args);
        CHECK(imported.value(QStringLiteral("encoding")).toString() == QString::fromLatin1(QStringConverter::nameForEncoding(example.codec)));
        const auto model = project.timeline->getSubtitleModel();
        REQUIRE(model);
        const int id = model->getIdForStartPos(0, GenTime(5, 25));
        REQUIRE(id >= 0);
        CHECK(model->getText(id) == expected);
        CHECK(model->getSubtitleEnd(id) == 30);
        CHECK(project.undoStack->count() == 1);
    }
    SECTION("A Unicode BOM wins over an explicitly selected single-byte codec")
    {
        McpProject project;
        project.open();
        QTemporaryDir directory;
        REQUIRE(directory.isValid());
        QStringEncoder encoder(QStringConverter::Utf16BE, QStringConverter::Flag::WriteBom);
        const QString path = subtitleFileFixture(directory, QStringLiteral("bom.sbv"), encoder.encode(subtitleFormatFixture(QStringLiteral("sbv"), subtitleUnicode())));
        auto args = subtitleFileArgs(project, QStringLiteral("import"), path);
        args.insert(QStringLiteral("encoding"), QStringLiteral("ISO-8859-1"));
        project.success(QStringLiteral("subtitle_file"), args);
        const auto model = project.timeline->getSubtitleModel();
        REQUIRE(model);
        const int id = model->getIdForStartPos(0, GenTime(5, 25));
        REQUIRE(id >= 0);
        CHECK(model->getText(id) == subtitleUnicode());
    }
}

TEST_CASE("Native subtitle exports preserve ASS styles and reimport ASS and SRT without overwriting", "[mcp][subtitle][formats][roundtrip]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString ass = directory.filePath(QStringLiteral("native.ASS"));
    const QString srt = directory.filePath(QStringLiteral("native.SRT"));
    QString style;
    {
        McpProject project;
        project.open();
        project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
        const QString contents = QStringLiteral("[V4+ Styles]\nStyle: Accent,Arial,37,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,11,12,1\n\n") +
                                 subtitleFormatFixture(QStringLiteral("ass"), subtitleUnicode()).replace(QStringLiteral(",Default,"), QStringLiteral(",Accent,"));
        const QString source = subtitleFileFixture(directory, QStringLiteral("styled.ass"), contents.toUtf8());
        project.success(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("import"), source));
        const auto model = project.timeline->getSubtitleModel();
        REQUIRE(model);
        style = model->getSubtitleStyle(QStringLiteral("Accent")).toString(QStringLiteral("Accent"));
        const int undoIndex = project.undoStack->index();
        project.success(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("export"), ass));
        project.success(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("export"), srt));
        CHECK(project.undoStack->index() == undoIndex);
        QFile assExport(ass);
        REQUIRE(assExport.open(QIODevice::ReadOnly));
        const QByteArray assBytes = assExport.readAll();
        CHECK(assBytes.contains(style.toUtf8()));
        CHECK(assBytes.contains("Comment: 0,00:00:02.04,00:00:03.24,Accent,Actor,12,13,14,fx,Second"));
        QFile exported(srt);
        REQUIRE(exported.open(QIODevice::ReadOnly));
        const QByteArray bytes = exported.readAll();
        CHECK(bytes.contains(subtitleUnicode().toUtf8()));
        CHECK(bytes.startsWith("1\n00:00:00,200 --> 00:00:01,200\n"));
        exported.close();
        project.rejectWithoutChange(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("export"), srt));
        REQUIRE(exported.open(QIODevice::ReadOnly));
        CHECK(exported.readAll() == bytes);
    }
    for (const QString &path : {ass, srt}) {
        McpProject project;
        project.open();
        project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
        project.success(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("import"), path));
        const auto model = project.timeline->getSubtitleModel();
        REQUIRE(model);
        REQUIRE(model->count() == 2);
        const int first = model->getIdForStartPos(0, GenTime(5, 25));
        REQUIRE(first >= 0);
        CHECK(model->getSubtitleEnd(first) == 30);
        CHECK(model->getText(first) == subtitleUnicode());
        if (path == ass) {
            CHECK(model->getSubtitleStyle(QStringLiteral("Accent")).toString(QStringLiteral("Accent")) == style);
            const auto ids = model->getAllSubIds();
            project.success(QStringLiteral("undo"), project.expected());
            CHECK(model->count() == 0);
            CHECK(model->getAllSubtitleStyles().count(QStringLiteral("Accent")) == 0);
            project.success(QStringLiteral("redo"), project.expected());
            CHECK(model->getAllSubIds() == ids);
            CHECK(model->getSubtitleStyle(QStringLiteral("Accent")).toString(QStringLiteral("Accent")) == style);
        }
        REQUIRE(project.timeline->checkConsistency());
    }
}

TEST_CASE("Subtitle files reject malformed late cues and codec errors before creating native state", "[mcp][subtitle][formats][guards]")
{
    const QByteArray valid = "1\n00:00:00,200 --> 00:00:01,200\nValid\n\n";
    const QList<QPair<QString, QByteArray>> files{
        {QStringLiteral("empty.srt"), QByteArray()},
        {QStringLiteral("legacy.ssa"), "[Events]\nDialogue: 0,0:00:00.20,0:00:01.20,Default,,0,0,0,,Unsupported\n"},
        {QStringLiteral("late.srt"), valid + "2\nmalformed --> 00:00:02,000\nMust not skip\n"},
        {QStringLiteral("missing-time.srt"), valid + "2\nMissing timing\nMust not skip\n"},
        {QStringLiteral("zero.srt"), "1\n00:00:00,200 --> 00:00:00,200\nEmpty range\n"},
        {QStringLiteral("empty-text.srt"), "1\n00:00:00,200 --> 00:00:01,200\n\n"},
        {QStringLiteral("overflow.srt"), "1\n9999:00:00,200 --> 9999:00:01,200\nOverflow\n"},
        {QStringLiteral("bad-minute.srt"), "1\n00:60:00,200 --> 00:60:01,200\nMalformed\n"},
        {QStringLiteral("duplicate.srt"), valid + "2\n00:00:00,200 --> 00:00:02,000\nCollision\n"},
        {QStringLiteral("truncated-utf8.srt"), valid + QByteArray::fromHex("e282")},
        {QStringLiteral("bad-utf8.srt"), valid + QByteArray::fromHex("ff")},
        {QStringLiteral("truncated-utf16.srt"), QByteArray::fromHex("fffe31000a0000")},
        {QStringLiteral("nul.srt"), valid + QByteArray(1, '\0')},
        {QStringLiteral("identified.vtt"), "WEBVTT\n\nidentifier\n00:00.200 --> 00:01.200\nMust not show identifier\n"},
        {QStringLiteral("settings.vtt"), "WEBVTT\n\n00:00.200 --> 00:01.200 align:start\nMust not discard settings\n"},
        {QStringLiteral("late.sbv"), "0:00:00.200,0:00:01.200\nValid\n\n0:bad,0:00:02.000\nInvalid\n"},
        {QStringLiteral("late.ass"), "[Events]\nDialogue: 0,0:00:00.20,0:00:01.20,Default,,0,0,0,,Valid\nDialogue: 0,bad,0:00:02.00,Default,,0,0,0,,Invalid\n"},
        {QStringLiteral("columns.ass"), "[Events]\nFormat: Layer, End, Start, Style, Name, MarginL, MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:00.20,0:00:01.20,Default,,0,0,0,,Reordered\n"},
        {QStringLiteral("missing-between-sections.ass"), "[Events]\nDialogue: 0,0:00:00.20,0:00:01.20,Default,,0,0,0,,Valid\n[Events]\nDialogue: 0,0:00:02.20,0:00:03.20,Default,,0,0,0,,Invalid header\n"},
        {QStringLiteral("missing-section-separator.ass"), "Unknown preamble\n[Events]\nDialogue: 0,0:00:00.20,0:00:01.20,Default,,0,0,0,,Native will not enter this section\n"}};
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    for (const auto &entry : files) {
        CAPTURE(entry.first);
        McpProject project;
        project.open();
        const QString path = subtitleFileFixture(directory, entry.first, entry.second);
        project.rejectWithoutChange(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("import"), path));
        CHECK_FALSE(project.timeline->hasSubtitleModel());
    }
}

TEST_CASE("Subtitle rejection preserves existing event IDs styles and undo branches", "[mcp][subtitle][formats][atomic]")
{
    McpProject project;
    project.open();
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    auto args = project.expected();
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("text"), QStringLiteral("Existing"));
    args.insert(QStringLiteral("startFrame"), 5);
    args.insert(QStringLiteral("endFrame"), 30);
    project.success(QStringLiteral("edit_subtitle"), args);
    const auto model = project.timeline->getSubtitleModel();
    REQUIRE(model);
    const auto ids = model->getAllSubIds();
    const auto events = model->toJson();
    const auto styles = subtitleSerializedStyles(model);
    const auto script = model->scriptInfo();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = subtitleFileFixture(directory, QStringLiteral("existing.srt"), subtitleFormatFixture(QStringLiteral("srt"), subtitleUnicode()).toUtf8());
    project.rejectWithoutChange(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("import"), path));
    args = subtitleFileArgs(project, QStringLiteral("import"), path);
    args.insert(QStringLiteral("encoding"), QStringLiteral("not-an-installed-codec"));
    project.rejectWithoutChange(QStringLiteral("subtitle_file"), args);
    args.insert(QStringLiteral("encoding"), 1);
    project.rejectWithoutChange(QStringLiteral("subtitle_file"), args);
    args = subtitleFileArgs(project, QStringLiteral("import"), path);
    args.insert(QStringLiteral("offsetFrames"), -100);
    project.rejectWithoutChange(QStringLiteral("subtitle_file"), args);
    const QString late = subtitleFileFixture(directory, QStringLiteral("metadata.ass"),
        "[Script Info]\nTitle: Must not be applied\n\n[V4+ Styles]\nStyle: Accent,Arial,37,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,11,12,1\n\n"
        "[Events]\nDialogue: 0,0:00:10.20,0:00:11.20,Accent,,0,0,0,,Valid\nDialogue: 0,bad,0:00:12.00,Accent,,0,0,0,,Invalid\n");
    project.rejectWithoutChange(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("import"), late));
    CHECK(model->getAllSubIds() == ids);
    CHECK(model->toJson() == events);
    CHECK(subtitleSerializedStyles(model) == styles);
    CHECK(model->scriptInfo() == script);
    project.success(QStringLiteral("undo"), project.expected());
    REQUIRE(project.undoStack->canRedo());
    const QString malformed = subtitleFileFixture(directory, QStringLiteral("branch.srt"), "1\nMalformed\ntext\n");
    project.rejectWithoutChange(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("import"), malformed));
    REQUIRE(project.undoStack->canRedo());
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(model->getAllSubIds() == ids);
    CHECK(model->toJson() == events);
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("Subtitle import bounds reject oversized files records and event counts atomically", "[mcp][subtitle][formats][bounds]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QList<QPair<QString, QByteArray>> cases;
    cases.append({QStringLiteral("oversized.srt"), QByteArray(16 * 1024 * 1024 + 1, 'a')});
    cases.append({QStringLiteral("long-line.srt"), QByteArray("1\n00:00:00,200 --> 00:00:01,200\n") + QByteArray(65537, 'a') + "\n"});
    QByteArray many;
    for (int index = 0; index <= 10000; ++index) {
        const int seconds = index / 25, milliseconds = (index % 25) * 40;
        const QString start = QStringLiteral("00:%1:%2,%3").arg(seconds / 60, 2, 10, QLatin1Char('0'))
                                  .arg(seconds % 60, 2, 10, QLatin1Char('0')).arg(milliseconds, 3, 10, QLatin1Char('0'));
        const int endSeconds = (index + 1) / 25, endMilliseconds = ((index + 1) % 25) * 40;
        const QString end = QStringLiteral("00:%1:%2,%3").arg(endSeconds / 60, 2, 10, QLatin1Char('0'))
                                .arg(endSeconds % 60, 2, 10, QLatin1Char('0')).arg(endMilliseconds, 3, 10, QLatin1Char('0'));
        many += QStringLiteral("%1\n%2 --> %3\nBounded cue\n\n").arg(index + 1).arg(start, end).toUtf8();
    }
    cases.append({QStringLiteral("event-count.srt"), many});
    for (const auto &entry : cases) {
        CAPTURE(entry.first);
        McpProject project;
        project.open();
        const QString path = subtitleFileFixture(directory, entry.first, entry.second);
        project.rejectWithoutChange(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("import"), path));
        CHECK_FALSE(project.timeline->hasSubtitleModel());
    }
}

TEST_CASE("ASS metadata mutations are imported and restored in one native undo", "[mcp][subtitle][formats][metadata]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QByteArray event = "[Events]\nDialogue: 0,0:00:00.20,0:00:01.20,Default,,0,0,0,,Plain\n";
    const QList<QByteArray> prefixes{
        "[Script Info]\nTitle: Changed script\n\n",
        "[V4+ Styles]\nStyle: Accent,Arial,37,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,11,12,1\n\n",
        "[Kdenlive Extradata]\nMaxLayer: 3\n\n",
        "[Kdenlive Extradata]\nDefaultStyles: Default\n\n",
        "[Fonts]\nfontname: unused.ttf\n!!!!\n\n"};
    int fixture = 0;
    for (const auto &prefix : prefixes) {
        McpProject project;
        project.open();
        // SubtitleModel reopens its document work file during construction.
        // Each loop is a separate document and must not share that native path.
        project.document->setDocumentProperty(QStringLiteral("documentid"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto model = project.timeline->createSubtitleModel();
        REQUIRE(model);
        REQUIRE(model->count() == 0);
        const auto beforeScript = model->scriptInfo();
        const auto beforeStyles = subtitleSerializedStyles(model);
        const int beforeLayer = model->getMaxLayer();
        const QString path = subtitleFileFixture(directory, QString::number(++fixture) + QStringLiteral(".ass"), prefix + event);
        const auto result = project.success(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("import"), path));
        CHECK(result.value(QStringLiteral("importedCount")).toInt() == 1);
        CHECK(project.undoStack->count() == 1);
        const auto ids = model->getAllSubIds();
        const auto afterScript = model->scriptInfo();
        const auto afterStyles = subtitleSerializedStyles(model);
        const int afterLayer = model->getMaxLayer();
        project.success(QStringLiteral("undo"), project.expected());
        CHECK(model->count() == 0);
        CHECK(model->scriptInfo() == beforeScript);
        CHECK(subtitleSerializedStyles(model) == beforeStyles);
        CHECK(model->getMaxLayer() == beforeLayer);
        project.success(QStringLiteral("redo"), project.expected());
        CHECK(model->getAllSubIds() == ids);
        CHECK(model->scriptInfo() == afterScript);
        CHECK(subtitleSerializedStyles(model) == afterStyles);
        CHECK(model->getMaxLayer() == afterLayer);
        REQUIRE(project.timeline->checkConsistency());
    }
}

TEST_CASE("ASS imports restore identical metadata and exact subprecision style changes", "[mcp][subtitle][formats][metadata]")
{
    McpProject project;
    project.open();
    project.document->setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    const auto model = project.timeline->createSubtitleModel();
    REQUIRE(model);
    const QString style = QStringLiteral("Style: Accent,Arial,37,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,11,12,1");
    model->setSubtitleStyle(QStringLiteral("Accent"), SubtitleStyle(style));
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QString header = QStringLiteral("[Script Info]\nScriptType: v4.00+\n\n[V4+ Styles]\n") + style + QStringLiteral("\n\n");
    const QString path = subtitleFileFixture(directory, QStringLiteral("identical.ass"),
        (header + subtitleFormatFixture(QStringLiteral("ass"), subtitleUnicode()).replace(QStringLiteral(",Default,"), QStringLiteral(",Accent,"))).toUtf8());
    project.success(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("import"), path));
    REQUIRE(model->count() == 2);
    const auto ids = model->getAllSubIds();
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(model->count() == 0);
    CHECK(model->getSubtitleStyle(QStringLiteral("Accent")).fontSize() == 37.0);
    REQUIRE(project.undoStack->canRedo());
    const QString changed = subtitleFileFixture(directory, QStringLiteral("small-change.ass"),
        (header.replace(QStringLiteral(",37,"), QStringLiteral(",37.001,")) + subtitleFormatFixture(QStringLiteral("ass"), subtitleUnicode())).toUtf8());
    project.success(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("import"), changed));
    CHECK(model->getSubtitleStyle(QStringLiteral("Accent")).fontSize() == 37.001);
    const auto changedIds = model->getAllSubIds();
    CHECK(changedIds != ids);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(model->getSubtitleStyle(QStringLiteral("Accent")).fontSize() == 37.0);
    CHECK(model->count() == 0);
    REQUIRE(project.undoStack->canRedo());
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(model->getAllSubIds() == changedIds);
    CHECK(model->getSubtitleStyle(QStringLiteral("Accent")).fontSize() == 37.001);
}


TEST_CASE("ASS native style and millisecond limits fail before creating subtitle state", "[mcp][subtitle][formats][native-limits]")
{
    McpProject project;
    project.open();
    OwnedCustomProfile owned;
    auto profileArgs = createCustomProfile(project, owned);
    auto specification = customProfileSpec();
    specification.insert(QStringLiteral("frameRateNumerator"), 1);
    specification.insert(QStringLiteral("frameRateDenominator"), 1);
    profileArgs.insert(QStringLiteral("specification"), specification);
    project.success(QStringLiteral("save_custom_profile"), profileArgs);
    const QString previous = pCore->getCurrentProfilePath();
    const auto restoreProfile = qScopeGuard([previous] { pCore->setCurrentProfile(previous); });
    REQUIRE(pCore->setCurrentProfile(owned.id));
    REQUIRE(pCore->getCurrentFps() == 1);
    const QByteArray style = "[V4+ Styles]\nStyle: Accent,Arial,37,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,11,12,1\n\n";
    const QByteArray event = "[Events]\nDialogue: 0,0:00:00.40,0:00:01.40,Accent,,0,0,0,,Valid event\n";
    struct Example { QString name; QByteArray bytes; int sourceFps{1}; int offset{0}; };
    const QList<Example> examples{
        {QStringLiteral("six-digit-color.ass"), QByteArray(style).replace("&H00FFFFFF", "&HFFFFFF") + event},
        {QStringLiteral("seven-digit-color.ass"), QByteArray(style).replace("&H00FFFFFF", "&H0FFFFFF") + event},
        {QStringLiteral("fractional-bold.ass"), QByteArray(style).replace(",-1,0,0,0,100,100", ",0.5,0,0,0,100,100") + event},
        {QStringLiteral("fractional-border.ass"), QByteArray(style).replace(",1,2,2,2,10,11,12,1", ",1.5,2,2,2,10,11,12,1") + event},
        {QStringLiteral("fractional-alignment.ass"), QByteArray(style).replace(",1,2,2,2,10,11,12,1", ",1,2,2,2.5,10,11,12,1") + event},
        {QStringLiteral("overflow-margin.ass"), QByteArray(style).replace(",10,11,12,1", ",2147483648,11,12,1") + event},
        {QStringLiteral("milliseconds-overflow.ass"), "[Events]\nDialogue: 0,10:00:00.00,10:00:00.01,Default,,0,0,0,,Native milliseconds overflow\n", 240},
        {QStringLiteral("offset-milliseconds-overflow.ass"), event, 1, 2200000}};
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const int filters = project.timeline->tractor()->filter_count();
    for (const auto &example : examples) {
        CAPTURE(example.name);
        const QString path = subtitleFileFixture(directory, example.name, example.bytes);
        auto arguments = subtitleFileArgs(project, QStringLiteral("import"), path);
        arguments.insert(QStringLiteral("sourceFps"), example.sourceFps);
        arguments.insert(QStringLiteral("offsetFrames"), example.offset);
        project.rejectWithoutChange(QStringLiteral("subtitle_file"), arguments);
        CHECK_FALSE(project.timeline->hasSubtitleModel());
        CHECK(project.timeline->tractor()->filter_count() == filters);
        CHECK(project.undoStack->count() == 0);
    }
}


TEST_CASE("ASS normalized UTF8 size is bounded before creating subtitle state", "[mcp][subtitle][formats][normalized-bounds]")
{
    McpProject project;
    project.open();
    QString contents = QStringLiteral("[Script Info]\n");
    const QString cjk(60000, QChar(0x4e2d));
    for (int row = 0; row < 100; ++row)
        contents += QStringLiteral("Title%1: ").arg(row) + cjk + QLatin1Char('\n');
    contents += QStringLiteral("\n[Events]\nDialogue: 0,0:00:00.40,0:00:01.40,Default,,0,0,0,,Valid event\n");
    QStringEncoder encoder(QStringConverter::Utf16LE, QStringConverter::Flag::WriteBom);
    const QByteArray bytes = encoder.encode(contents);
    REQUIRE_FALSE(encoder.hasError());
    REQUIRE(bytes.size() < 16 * 1024 * 1024);
    REQUIRE(contents.toUtf8().size() > 16 * 1024 * 1024);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = subtitleFileFixture(directory, QStringLiteral("normalized-limit.ass"), bytes);
    const int filters = project.timeline->tractor()->filter_count();
    project.rejectWithoutChange(QStringLiteral("subtitle_file"), subtitleFileArgs(project, QStringLiteral("import"), path));
    CHECK_FALSE(project.timeline->hasSubtitleModel());
    CHECK(project.timeline->tractor()->filter_count() == filters);
    CHECK(project.undoStack->count() == 0);
}
