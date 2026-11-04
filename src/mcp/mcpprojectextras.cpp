/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "mcpprojectextras.h"
#include "bin/clipcreator.hpp"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "jobs/taskmanager.h"
#include "mcphelpers.h"
#include "project/dialogs/noteswidget.h"
#include "project/notesplugin.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImageReader>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextDocumentFragment>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
namespace {
QString k(const char *value)
{
    return QString::fromLatin1(value);
}
QJsonObject str(const char *description)
{
    return Mcp::property(k("string"), k(description));
}
QJsonObject num(const char *description)
{
    return Mcp::property(k("integer"), k(description));
}
QJsonObject flag(const char *description)
{
    return Mcp::property(k("boolean"), k(description));
}
QJsonObject list(const char *description)
{
    auto value = Mcp::property(k("array"), k(description));
    value.insert(k("items"), QJsonObject{});
    return value;
}
bool text(const QJsonValue &value, QString &result, int maximum = 4096, bool empty = false)
{
    if (!value.isString()) return false;
    result = value.toString();
    return (empty || !result.isEmpty()) && result.size() <= maximum && !result.contains(QChar::Null);
}
QString hash(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString hash(const QJsonObject &value)
{
    return hash(QJsonDocument(value).toJson(QJsonDocument::Compact));
}
NotesWidget *notesWidget()
{
    const auto manager = pCore->projectManager();
    const auto plugin = manager ? manager->findChild<NotesPlugin *>() : nullptr;
    return plugin ? plugin->widget() : nullptr;
}
bool boundary(const QString &value, int offset)
{
    return offset >= 0 && offset <= value.size() &&
           !(offset > 0 && offset < value.size() && value.at(offset - 1).isHighSurrogate() && value.at(offset).isLowSurrogate());
}
QJsonObject notesReadback(NotesWidget *widget, const QString &savedHtml = {})
{
    QString html, plain;
    if (widget) {
        html = widget->toHtml();
        plain = widget->toPlainText();
    } else {
        html = savedHtml;
        QTextDocument document;
        document.setHtml(html);
        plain = document.toPlainText();
    }
    const auto context = QJsonObject{{k("documentId"), pCore->currentDoc()->uuid().toString()}, {k("html"), html}};
    QJsonArray anchors;
    if (widget)
        for (auto block = widget->document()->begin(); block.isValid(); block = block.next())
            for (auto entry = block.begin(); !entry.atEnd(); ++entry) {
                const auto fragment = entry.fragment();
                const auto format = fragment.charFormat();
                if (fragment.isValid() && format.isAnchor())
                    anchors.append(QJsonObject{
                        {k("start"), fragment.position()}, {k("length"), fragment.length()}, {k("href"), format.anchorHref()}, {k("text"), fragment.text()}});
            }
    return {{k("text"), plain},
            {k("html"), html},
            {k("anchors"), anchors},
            {k("notesToken"), hash(context)},
            {k("guiAvailable"), bool(widget)},
            {k("historyScope"), k("notes")},
            {k("timelineHistoryChanged"), false},
            {k("canUndo"), widget && widget->document()->isUndoAvailable()},
            {k("canRedo"), widget && widget->document()->isRedoAvailable()}};
}
struct Cleanup
{
    QJsonArray eligible;
    QHash<QString, std::shared_ptr<ProjectClip>> sources;
    QString token;
};
Cleanup cleanupState()
{
    Cleanup result;
    auto clips = pCore->projectItemModel()->getRootFolder()->childClips();
    std::sort(clips.begin(), clips.end(), [](const auto &a, const auto &b) { return a->clipId() < b->clipId(); });
    for (const auto &clip : clips) {
        if (!clip || clip->clipType() == ClipType::Timeline || clip->isIncludedInTimeline()) continue;
        const bool ready = clip->statusReady() || clip->clipStatus() == FileStatus::StatusMissing;
        const QJsonObject item{{k("binId"), clip->clipId()},
                               {k("name"), clip->clipName()},
                               {k("resource"), clip->getOriginalUrl()},
                               {k("readyForCleanup"), ready},
                               {k("sourceFilesPreserved"), true}};
        result.eligible.append(item);
        result.sources.insert(clip->clipId(), clip);
    }
    result.token = hash(QJsonObject{{k("documentId"), pCore->currentDoc()->uuid().toString()}, {k("sources"), result.eligible}});
    return result;
}
struct Intake
{
    QString kind, resource, token;
    int count{0};
    QJsonArray files;
};
bool imageFile(const QString &path, qint64 &total, QJsonObject &result, QString &error)
{
    QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || !info.isReadable() || info.size() <= 0 || info.size() > 64 * 1024 * 1024 ||
        (total += info.size()) > 256 * 1024 * 1024) {
        error = k("Use readable local images within the 64 MiB/file and 256 MiB intake budgets.");
        return false;
    }
    QImageReader reader(info.canonicalFilePath());
    const QSize size = reader.size();
    if (!reader.canRead() || size.width() < 8 || size.height() < 8 || size.width() > 16384 || size.height() > 16384 ||
        qint64(size.width()) * size.height() > 100000000 || reader.imageCount() > 1) {
        error = k("Use bounded still images rather than animated or undecodable resources.");
        return false;
    }
    QFile file(info.canonicalFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!digest.addData(&file)) {
        error = file.errorString();
        return false;
    }
    result = {{k("path"), info.canonicalFilePath()},
              {k("bytes"), double(info.size())},
              {k("width"), size.width()},
              {k("height"), size.height()},
              {k("sha256"), QString::fromLatin1(digest.result().toHex())}};
    return true;
}
bool inspectIntake(const QJsonObject &args, Intake &result, QString &error)
{
    QString path;
    if (!text(args.value(k("kind")), result.kind, 64) || !text(args.value(k("path")), path, 32768) || !QDir::isAbsolutePath(path)) {
        error = k("Use slideshow or title_template and an absolute local input path.");
        return false;
    }
    qint64 total = 0;
    if (result.kind == k("slideshow")) {
        QString extension;
        if (!text(args.value(k("extension")), extension, 8) || !QRegularExpression(k("^[A-Za-z0-9]{2,8}$")).match(extension).hasMatch()) {
            error = k("A folder slideshow needs an explicit image extension.");
            return false;
        }
        const QFileInfo folder(path);
        const QDir directory(folder.canonicalFilePath());
        if (!folder.isDir() || folder.isSymLink() || directory.canonicalPath().isEmpty()) {
            error = k("Use a real local slideshow directory.");
            return false;
        }
        const auto names = directory.entryList({k("*.") + extension}, QDir::Files, QDir::Name);
        if (names.isEmpty() || names.size() > 512) {
            error = k("A folder slideshow needs 1-512 matching still images.");
            return false;
        }
        for (const auto &name : names) {
            QJsonObject item;
            if (!imageFile(directory.absoluteFilePath(name), total, item, error)) return false;
            result.files.append(item);
        }
        result.count = names.size();
        result.resource = directory.absoluteFilePath(k(".all.") + extension);
    } else if (result.kind == k("title_template")) {
        const QFileInfo info(path);
        QFile file(info.canonicalFilePath());
        if (!info.isFile() || info.isSymLink() || info.suffix().compare(k("kdenlivetitle"), Qt::CaseInsensitive) || info.size() <= 0 ||
            info.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly)) {
            error = k("Use a bounded local .kdenlivetitle template.");
            return false;
        }
        const auto bytes = file.read(1024 * 1024 + 1);
        if (file.error() != QFileDevice::NoError || !file.atEnd() || bytes.size() > 1024 * 1024 || bytes.contains("<!DOCTYPE") || bytes.contains("<!ENTITY")) {
            error = k("Title templates cannot contain DTDs/entities or exceed 1 MiB.");
            return false;
        }
        QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
        const QString xml = utf8.decode(bytes);
        if (utf8.hasError() || xml.contains(QChar::Null)) {
            error = k("Use a valid UTF-8 native title template.");
            return false;
        }
        QDomDocument document;
        if (!document.setContent(bytes) || document.documentElement().tagName() != k("kdenlivetitle")) {
            error = k("The native title template XML is invalid.");
            return false;
        }
        const auto root = document.documentElement();
        bool widthOk = false, heightOk = false;
        const int width = root.attribute(k("width")).toInt(&widthOk), height = root.attribute(k("height")).toInt(&heightOk);
        if (!widthOk || !heightOk || width < 8 || height < 8 || width > 16384 || height > 16384 || qint64(width) * height > 100000000) {
            error = k("Title template canvas exceeds the native image budget.");
            return false;
        }
        const auto nodes = document.elementsByTagName(k("item"));
        if (nodes.size() < 1 || nodes.size() > 256 || bytes.size() > 1024 * 1024) {
            error = k("Use 1-256 bounded native title items.");
            return false;
        }
        const QRegularExpression resources(k("<(?:img|iframe|object|video|audio)\\b|url\\s*\\("), QRegularExpression::CaseInsensitiveOption);
        for (int index = 0; index < nodes.size(); ++index) {
            const auto item = nodes.at(index).toElement();
            const QString type = item.attribute(k("type"));
            if (type != k("QGraphicsTextItem") && type != k("QGraphicsRectItem") && type != k("QGraphicsEllipseItem") && type != k("QGraphicsPixmapItem")) {
                error = k("Template has an unsupported native object type.");
                return false;
            }
            const auto content = item.firstChildElement(k("content"));
            if (content.hasAttribute(k("base64")) || resources.match(content.text()).hasMatch()) {
                error = k("Inline resource loading needs the native title resource workflow.");
                return false;
            }
            if (content.hasAttribute(k("url"))) {
                QString resource = content.attribute(k("url"));
                if (QDir::isRelativePath(resource)) resource = info.dir().absoluteFilePath(resource);
                QJsonObject image;
                if (!imageFile(resource, total, image, error)) return false;
                result.files.append(image);
            }
            if (content.hasAttribute(k("shadow")) || !content.firstChildElement(k("shadows")).isNull() || !item.firstChildElement(k("effect")).isNull()) {
                error = k("Templates with legacy shadow rasters require native title inspection first.");
                return false;
            }
        }
        result.files.prepend(QJsonObject{{k("path"), info.canonicalFilePath()}, {k("bytes"), double(bytes.size())}, {k("sha256"), hash(bytes)}});
        result.count = nodes.size();
        result.resource = info.canonicalFilePath();
    } else {
        error = k("Supported source kinds are slideshow and title_template.");
        return false;
    }
    result.token = hash(QJsonObject{{k("kind"), result.kind}, {k("resource"), result.resource}, {k("files"), result.files}});
    return true;
}
class Extras : public QObject
{
public:
    explicit Extras(QObject *owner)
        : QObject(owner)
    {
    }
    struct SourceJob
    {
        QString document, binId, kind;
        std::weak_ptr<ProjectClip> source;
    };
    QMap<QString, SourceJob> jobs;
    QJsonObject job(const QString &id)
    {
        const auto record = jobs.value(id);
        const auto source = record.source.lock();
        const bool attached = pCore->currentDoc() && pCore->currentDoc()->uuid().toString() == record.document && source &&
                              pCore->projectItemModel()->hasClip(record.binId) && pCore->projectItemModel()->getClipByBinID(record.binId) == source;
        QString status = k("detached");
        if (attached) {
            const ObjectId owner(KdenliveObjectType::BinClip, record.binId.toInt(), QUuid());
            status = source->statusReady() ? k("ready") : pCore->taskManager.hasPendingJob(owner, AbstractTask::LOADJOB) ? k("loading") : k("failed");
        }
        QJsonObject result{{k("jobId"), id},
                           {k("status"), status},
                           {k("binId"), record.binId},
                           {k("kind"), record.kind},
                           {k("documentId"), record.document},
                           {k("ready"), status == k("ready")},
                           {k("terminal"), status != k("loading")}};
        if (attached && source->statusReady())
            result.insert(k("source"), QJsonObject{{k("name"), source->clipName()},
                                                   {k("durationFrames"), source->getFramePlaytime()},
                                                   {k("resource"), source->url()},
                                                   {k("hasVideo"), source->hasVideo()},
                                                   {k("hasAudio"), source->hasAudio()},
                                                   {k("ttl"), source->getProducerIntProperty(k("ttl"))},
                                                   {k("templateText"), source->getProducerProperty(k("templatetext"))}});
        return result;
    }
    bool room()
    {
        if (jobs.size() < 64) return true;
        for (auto entry = jobs.begin(); entry != jobs.end(); ++entry)
            if (job(entry.key()).value(k("terminal")).toBool()) {
                jobs.erase(entry);
                return true;
            }
        return false;
    }
};
Extras *extras(QObject *owner)
{
    for (auto *child : owner->children())
        if (auto *value = dynamic_cast<Extras *>(child)) return value;
    return new Extras(owner);
}
} // namespace

namespace McpProjectExtras {
QJsonArray toolCatalog()
{
    const auto expected = str("Fresh whole-project expectedState.");
    QJsonObject source{{k("kind"), str("slideshow or title_template")},
                       {k("path"), str("Absolute folder or .kdenlivetitle file.")},
                       {k("extension"), str("Folder slideshow image extension, e.g. png.")}};
    QJsonObject create = source;
    create.insert(k("expectedState"), expected);
    create.insert(k("expectedSource"), str("Exact sourceToken from inspect_source_creation."));
    create.insert(k("name"), str("Native bin source name, up to160 characters."));
    create.insert(k("parentId"), str("Native bin folder ID; default root."));
    create.insert(k("framesPerImage"), num("Folder slideshow hold in project frames."));
    create.insert(k("loop"), flag("Native slideshow loop."));
    create.insert(k("crop"), flag("Native slideshow crop."));
    create.insert(k("fadeFrames"), num("Optional native slideshow fade duration."));
    create.insert(k("text"), str("Native title template placeholder text."));
    return {Mcp::tool(k("get_project_notes"), k("Read native project notes, links and the separate notes history."), {}, {}, true),
            Mcp::tool(k("edit_project_notes"),
                      k("Edit the actual NotesWidget through native text history, separate from timeline undo. UTF16 ranges cannot split surrogate pairs."),
                      {{k("expectedState"), expected},
                       {k("expectedNotes"), str("Exact notesToken.")},
                       {k("operation"), str("replace, insert, remove, format, undo, redo, add_timecode.")},
                       {k("start"), num("UTF16 start offset.")},
                       {k("length"), num("UTF16 length.")},
                       {k("text"), str("Plain note text, up to131072 UTF16 units.")},
                       {k("bold"), flag("Selected text bold.")},
                       {k("italic"), flag("Selected text italic.")},
                       {k("underline"), flag("Selected text underline.")},
                       {k("binId"), str("Timecode source bin ID; omit for sequence.")},
                       {k("sequenceId"), str("Explicit sequence UUID for timeline timecode.")},
                       {k("frame"), num("Timecode frame.")}},
                      {k("expectedState"), k("expectedNotes"), k("operation")}),
            Mcp::tool(k("inspect_unused_sources"), k("List attached unused native sources eligible for exact source-preserving cleanup."), {}, {}, true),
            Mcp::tool(k("clean_project"), k("Remove exactly the prepared unused bin IDs in one native undo command. Original source files are preserved."),
                      {{k("expectedState"), expected},
                       {k("cleanupToken"), str("Exact cleanupToken from inspect_unused_sources.")},
                       {k("binIds"), list("Explicit unique bin IDs to remove.")}},
                      {k("expectedState"), k("cleanupToken"), k("binIds")}),
            Mcp::tool(k("inspect_source_creation"), k("Hash and validate local source inputs before native slideshow/template creation."), source,
                      {k("kind"), k("path")}, true),
            Mcp::tool(k("create_bin_source"),
                      k("Create an editable native folder slideshow or linked title template with native undo and asynchronous decode status."), create,
                      {k("expectedState"), k("expectedSource"), k("kind"), k("path"), k("name")}),
            Mcp::tool(k("bin_source_job_status"), k("Read the owned source creation decode outcome; acceptance is not readiness."),
                      {{k("jobId"), str("Owned source job ID.")}}, {k("jobId")}, true)};
}
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &args, const QJsonObject &state, const std::function<QJsonObject()> &reader)
{
    bool known = false;
    for (const auto &tool : toolCatalog())
        if (tool.toObject().value(k("name")) == name) {
            known = true;
            break;
        }
    if (!known) return {};
    auto reject = [&](const char *code, const QString &message) { return Mcp::failure(k(code), message, reader ? reader() : state); };
    auto finish = [&](QJsonObject result) {
        result.insert(k("ok"), true);
        result.insert(k("state"), reader ? reader() : state);
        return Mcp::toolResult(result);
    };
    if (!owner || !qApp || owner->thread() != qApp->thread() || QThread::currentThread() != qApp->thread())
        return reject("GUI_THREAD_REQUIRED", k("Use the owning native GUI thread."));
    if (!pCore->currentDoc() || !pCore->projectItemModel() || !pCore->projectItemModel()->getRootFolder())
        return reject("NOT_READY", k("No native document/bin is available."));
    const bool mutation = name == k("edit_project_notes") || name == k("clean_project") || name == k("create_bin_source");
    if (mutation && (args.value(k("expectedState")).toString().isEmpty() || args.value(k("expectedState")) != state.value(k("expectedState"))))
        return reject("STALE_STATE", k("Read current native state before editing."));
    auto *service = extras(owner);
    if (name == k("bin_source_job_status")) {
        QString id;
        if (!text(args.value(k("jobId")), id, 128) || !service->jobs.contains(id)) return reject("INVALID_JOB", k("Unknown owned source job."));
        return finish({{k("job"), service->job(id)}});
    }
    if (name == k("get_project_notes")) return finish(notesReadback(notesWidget(), state.value(k("documentNotes")).toString()));
    if (name == k("edit_project_notes")) {
        auto *widget = notesWidget();
        if (!widget) return reject("GUI_REQUIRED", k("Live note editing requires the native NotesWidget."));
        const auto before = notesReadback(widget);
        if (args.value(k("expectedNotes")) != before.value(k("notesToken"))) return reject("STALE_NOTES", k("Native notes changed; inspect them again."));
        const QString operation = args.value(k("operation")).toString();
        QStringList allowed{k("expectedState"), k("expectedNotes"), k("operation")};
        if (operation == k("replace"))
            allowed += {k("text")};
        else if (operation == k("insert"))
            allowed += {k("start"), k("text")};
        else if (operation == k("remove"))
            allowed += {k("start"), k("length")};
        else if (operation == k("format"))
            allowed += {k("start"), k("length"), k("bold"), k("italic"), k("underline")};
        else if (operation == k("add_timecode"))
            allowed += {k("start"), k("binId"), k("sequenceId"), k("frame")};
        else if (operation != k("undo") && operation != k("redo"))
            return reject("INVALID_ARGUMENT", k("Unknown native note operation."));
        for (auto entry = args.begin(); entry != args.end(); ++entry)
            if (!allowed.contains(entry.key())) return reject("INVALID_ARGUMENT", k("Argument does not apply to the note operation."));
        if (operation == k("undo") || operation == k("redo")) {
            if (!(operation == k("undo") ? widget->document()->isUndoAvailable() : widget->document()->isRedoAvailable()))
                return reject("NO_HISTORY", k("No native notes history in this direction."));
            if (operation == k("undo"))
                widget->undo();
            else
                widget->redo();
            pCore->currentDoc()->setModified(true);
            return finish(notesReadback(widget));
        }
        QString plain = widget->toPlainText(), value;
        if (plain.size() > 131072) return reject("LIMIT", k("Native notes exceed this editing operation's text budget."));
        int start = 0, length = 0;
        if (operation == k("replace"))
            length = plain.size();
        else if (!Mcp::integer(args, k("start"), 0, plain.size(), start) ||
                 ((operation == k("remove") || operation == k("format")) && !Mcp::integer(args, k("length"), 1, plain.size() - start, length)))
            return reject("INVALID_ARGUMENT", k("Use a valid explicit UTF16 text range."));
        if (!boundary(plain, start) || !boundary(plain, start + length)) return reject("INVALID_ARGUMENT", k("A note range cannot split a surrogate pair."));
        QTextCursor cursor(widget->document());
        cursor.setPosition(start);
        cursor.setPosition(start + length, QTextCursor::KeepAnchor);
        QTextCharFormat format;
        if (operation == k("format")) {
            bool any = false;
            for (const auto &field : {k("bold"), k("italic"), k("underline")})
                if (args.contains(field)) {
                    if (!args.value(field).isBool()) return reject("INVALID_ARGUMENT", k("Note format flags must be boolean."));
                    any = true;
                    if (field == k("bold"))
                        format.setFontWeight(args.value(field).toBool() ? QFont::Bold : QFont::Normal);
                    else if (field == k("italic"))
                        format.setFontItalic(args.value(field).toBool());
                    else
                        format.setFontUnderline(args.value(field).toBool());
                }
            if (!any) return reject("INVALID_ARGUMENT", k("Supply a note formatting flag."));
        } else if (operation == k("add_timecode")) {
            int frame = -1;
            if (!Mcp::integer(args, k("frame"), 0, 10000000, frame)) return reject("INVALID_ARGUMENT", k("Use a bounded explicit timecode frame."));
            QString target, label;
            if (args.contains(k("binId"))) {
                const QString id = args.value(k("binId")).toString();
                const auto source = pCore->projectItemModel()->hasClip(id) ? pCore->projectItemModel()->getClipByBinID(id) : nullptr;
                if (!source || !source->statusReady() || frame >= source->getFramePlaytime() || args.contains(k("sequenceId")))
                    return reject("INVALID_ARGUMENT", k("Use a ready native source frame and no sequenceId."));
                target = pCore->projectItemModel()->getBinClipUuid(id) + k("#") + QString::number(frame);
                label = source->clipName() + k(" ");
            } else {
                const QUuid uuid(args.value(k("sequenceId")).toString());
                const auto timeline = pCore->currentDoc()->getTimeline(uuid, true);
                if (uuid.isNull() || !timeline || frame > timeline->duration())
                    return reject("INVALID_ARGUMENT", k("Use an existing explicit sequence and frame."));
                target = uuid.toString() + k("!") + QString::number(frame);
            }
            value = label + pCore->timecode().getTimecodeFromFrames(frame);
            format.setAnchor(true);
            format.setAnchorHref(target);
        } else if (operation == k("replace") || operation == k("insert")) {
            if (!text(args.value(k("text")), value, 131072, true)) return reject("INVALID_ARGUMENT", k("Use bounded plain note text."));
        }
        if (plain.size() - length + value.size() > 131072) return reject("LIMIT", k("The resulting note exceeds its editing text budget."));
        cursor.beginEditBlock();
        if (operation == k("format"))
            cursor.mergeCharFormat(format);
        else if (operation == k("remove"))
            cursor.removeSelectedText();
        else
            cursor.insertText(value, format);
        cursor.endEditBlock();
        pCore->currentDoc()->setModified(true);
        return finish(notesReadback(widget));
    }
    if (name == k("inspect_unused_sources") || name == k("clean_project")) {
        const auto prepared = cleanupState();
        if (name == k("inspect_unused_sources")) return finish({{k("sources"), prepared.eligible}, {k("cleanupToken"), prepared.token}});
        const auto values = args.value(k("binIds"));
        if (args.value(k("cleanupToken")).toString() != prepared.token) return reject("STALE_CLEANUP", k("Inspect current unused sources again."));
        if (!values.isArray() || values.toArray().isEmpty() || values.toArray().size() > 512)
            return reject("INVALID_ARGUMENT", k("Provide 1-512 explicit unused source IDs."));
        QSet<QString> selected;
        QList<std::shared_ptr<ProjectClip>> clips;
        for (const auto &entry : values.toArray()) {
            const QString id = entry.toString();
            const auto clip = prepared.sources.value(id);
            if (!entry.isString() || selected.contains(id) || !clip || (!clip->statusReady() && clip->clipStatus() != FileStatus::StatusMissing))
                return reject("INVALID_BIN_ITEM", k("Every ID must be a unique attached unused source ready for cleanup."));
            selected.insert(id);
            clips.append(clip);
        }
        Fun undo = [] { return true; }, redo = [] { return true; };
        for (const auto &clip : clips)
            if (!pCore->projectItemModel()->requestBinClipDeletion(clip, undo, redo)) {
                const bool restored = undo();
                return reject(restored ? "EDIT_FAILED" : "ROLLBACK_FAILED", k("Native bin cleanup failed; inspect state before any retry."));
            }
        pCore->pushUndo(undo, redo, k("MCP remove exact unused media"));
        return finish({{k("removedBinIds"), values}, {k("sourceFilesPreserved"), true}, {k("undoable"), true}});
    }
    Intake intake;
    QString error;
    if (!inspectIntake(args, intake, error)) return reject("INVALID_SOURCE", error);
    if (name == k("inspect_source_creation"))
        return finish({{k("kind"), intake.kind},
                       {k("sourceToken"), intake.token},
                       {k("resource"), intake.resource},
                       {k("itemCount"), intake.count},
                       {k("files"), intake.files}});
    if (args.value(k("expectedSource")).toString() != intake.token) return reject("STALE_SOURCE", k("The input files changed; inspect them again."));
    QString title, parent = pCore->projectItemModel()->getRootFolder()->clipId();
    if (!text(args.value(k("name")), title, 160) || (args.contains(k("parentId")) && !text(args.value(k("parentId")), parent, 128)) ||
        !pCore->projectItemModel()->getFolderByBinId(parent))
        return reject("INVALID_ARGUMENT", k("Use a nonempty source name and an attached native parent folder."));
    const auto parentFolder = pCore->projectItemModel()->getFolderByBinId(parent);
    const auto rootFolder = pCore->projectItemModel()->getRootFolder();
    if (parentFolder != rootFolder && !parentFolder->hasAncestor(rootFolder->getId()))
        return reject("INVALID_ARGUMENT", k("The requested native folder is detached from the current bin."));
    if (pCore->projectItemModel()->getAllClipIds().size() >= 511 || !service->room())
        return reject("LIMIT", k("Native bin or source job capacity has been reached."));
    QString id;
    if (intake.kind == k("slideshow")) {
        int ttl = 0, fade = 0;
        if (!Mcp::integer(args, k("framesPerImage"), 1, 10000000, ttl) || qint64(ttl) * intake.count > 10000000 ||
            (args.contains(k("fadeFrames")) && !Mcp::integer(args, k("fadeFrames"), 0, ttl, fade)) || args.contains(k("text")))
            return reject("INVALID_ARGUMENT", k("Use bounded folder slideshow hold/fade frames and no template text."));
        for (const auto &field : {k("loop"), k("crop")})
            if (args.contains(field) && !args.value(field).isBool()) return reject("INVALID_ARGUMENT", k("Slideshow switches must be boolean."));
        const std::unordered_map<QString, QString> properties{{k("ttl"), QString::number(ttl)},
                                                              {k("loop"), args.value(k("loop")).toBool() ? k("1") : k("0")},
                                                              {k("crop"), args.value(k("crop")).toBool() ? k("1") : k("0")},
                                                              {k("fade"), fade ? k("1") : k("0")},
                                                              {k("luma_duration"), QString::number(fade)}};
        id = ClipCreator::createSlideshowClip(intake.resource, ttl * intake.count, title, parent, properties, pCore->projectItemModel());
    } else {
        QString value;
        if ((args.contains(k("text")) && !text(args.value(k("text")), value, 8192, true)) || args.contains(k("framesPerImage")) ||
            args.contains(k("fadeFrames")) || args.contains(k("loop")) || args.contains(k("crop")) || args.contains(k("extension")))
            return reject("INVALID_ARGUMENT", k("Use native template text without slideshow options."));
        id = ClipCreator::createTitleTemplate(intake.resource, value, title, parent, pCore->projectItemModel());
    }
    if (id.isEmpty() || id == k("-1") || !pCore->projectItemModel()->hasClip(id)) return reject("SOURCE_CREATION_FAILED", k("Native source creation failed."));
    const QString job = QUuid::createUuid().toString(QUuid::WithoutBraces);
    service->jobs.insert(job, {pCore->currentDoc()->uuid().toString(), id, intake.kind, pCore->projectItemModel()->getClipByBinID(id)});
    return finish({{k("accepted"), true}, {k("binId"), id}, {k("undoable"), true}, {k("job"), service->job(job)}});
}
} // namespace McpProjectExtras
