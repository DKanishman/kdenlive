/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "mcpprojectextras.h"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "mcphelpers.h"
#include "project/dialogs/noteswidget.h"
#include "project/notesplugin.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QFont>
#include <QSet>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QThread>
#include <QUuid>
#include <algorithm>
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
    if ((!empty && result.isEmpty()) || result.size() > maximum || result.contains(QChar::Null)) return false;
    for (qsizetype index = 0; index < result.size(); ++index) {
        if (result.at(index).isHighSurrogate()) {
            if (++index >= result.size() || !result.at(index).isLowSurrogate()) return false;
        } else if (result.at(index).isLowSurrogate()) {
            return false;
        }
    }
    return true;
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
// Plain-text inspection of saved notes must not load linked local images.
class SavedNotesDocument : public QTextDocument
{
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};
bool boundary(const QString &value, int offset)
{
    return offset >= 0 && offset <= value.size() &&
           !(offset > 0 && offset < value.size() && value.at(offset - 1).isHighSurrogate() && value.at(offset).isLowSurrogate());
}
QJsonObject notesReadback(NotesWidget *widget, const QString &savedHtml = {})
{
    QString html, plain;
    SavedNotesDocument savedDocument;
    QTextDocument *document = widget ? widget->document() : &savedDocument;
    if (widget) {
        html = widget->toHtml();
        plain = widget->toPlainText();
    } else {
        html = savedHtml;
        document->setHtml(html);
        plain = document->toPlainText();
    }
    const auto context = QJsonObject{{k("documentId"), pCore->currentDoc()->uuid().toString()}, {k("html"), html}};
    QJsonArray anchors;
    for (auto block = document->begin(); block.isValid(); block = block.next())
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
} // namespace

namespace McpProjectExtras {
QJsonArray toolCatalog()
{
    const auto expected = str("Fresh whole-project expectedState.");
    return {Mcp::tool(k("get_project_notes"), k("Read native project notes, links and the separate notes history."), {}, {}, true),
            Mcp::tool(k("edit_project_notes"),
                      k("Edit the actual NotesWidget through native text history, separate from timeline undo. UTF16 ranges cannot split surrogate pairs."),
                      {{k("expectedState"), expected},
                       {k("expectedNotes"), str("Exact notesToken.")},
                       {k("operation"), str("replace, insert, remove, format, undo, redo, add_timecode.")},
                       {k("start"), num("UTF16 start offset.")},
                       {k("length"), num("UTF16 length.")},
                       {k("text"), str("Plain note text, up to 131072 UTF16 units.")},
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
                      {k("expectedState"), k("cleanupToken"), k("binIds")})};
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
    if (!pCore || !pCore->currentDoc() || pCore->currentDoc()->loading || !pCore->projectItemModel() || !pCore->projectItemModel()->getRootFolder())
        return reject("NOT_READY", k("No native document/bin is available."));
    const bool mutation = name == k("edit_project_notes") || name == k("clean_project");
    if (mutation && (args.value(k("expectedState")).toString().isEmpty() || args.value(k("expectedState")) != state.value(k("expectedState"))))
        return reject("STALE_STATE", k("Read current native state before editing."));
    if (name == k("get_project_notes")) {
        if (!args.isEmpty()) return reject("INVALID_ARGUMENT", k("Project notes inspection takes no arguments."));
        return finish(notesReadback(notesWidget(), state.value(k("documentNotes")).toString()));
    }
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
        if (widget->document()->characterCount() - 1 != plain.size())
            return reject("UNSUPPORTED_NOTES", k("Native rich-note structure does not have a one-to-one plain-text range mapping."));
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
                QString id;
                if (!text(args.value(k("binId")), id, 128)) return reject("INVALID_ARGUMENT", k("Use an explicit native source ID."));
                const auto source = pCore->projectItemModel()->hasClip(id) ? pCore->projectItemModel()->getClipByBinID(id) : nullptr;
                if (!source || !source->statusReady() || frame >= source->getFramePlaytime() || args.contains(k("sequenceId")))
                    return reject("INVALID_ARGUMENT", k("Use a ready native source frame and no sequenceId."));
                target = pCore->projectItemModel()->getBinClipUuid(id) + k("#") + QString::number(frame);
                label = source->clipName() + k(" ");
            } else {
                QString sequenceId;
                if (!text(args.value(k("sequenceId")), sequenceId, 128))
                    return reject("INVALID_ARGUMENT", k("Use an explicit sequence UUID."));
                const QUuid uuid(sequenceId);
                if (uuid.isNull()) return reject("INVALID_ARGUMENT", k("Use an existing explicit sequence and frame."));
                const auto timeline = pCore->currentDoc()->getTimeline(uuid, true);
                if (!timeline || frame > timeline->duration())
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
        const QStringList allowed = name == k("inspect_unused_sources")
                                        ? QStringList{}
                                        : QStringList{k("expectedState"), k("cleanupToken"), k("binIds")};
        for (auto entry = args.begin(); entry != args.end(); ++entry)
            if (!allowed.contains(entry.key())) return reject("INVALID_ARGUMENT", k("Argument does not apply to source cleanup."));
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
    return {};
}
} // namespace McpProjectExtras
