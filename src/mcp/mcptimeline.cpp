/* SPDX-FileCopyrightText: 2026 Kdenlive contributors
   SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "assets/model/assetparametermodel.hpp"
#include "bin/clipcreator.hpp"
#include "bin/model/markerlistmodel.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "bin/projectsubclip.h"
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "kdenlivesettings.h"
#include "mcphelpers.h"
#include "mcpsequence.h"
#include "mcpserver.h"
#include "mcpstate.h"
#include "project/projectmanager.h"
#include "timeline2/model/clipmodel.hpp"
#include "timeline2/model/compositionmodel.hpp"
#include "timeline2/model/groupsmodel.hpp"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/model/trackmodel.hpp"
#include <QColor>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <functional>
#include <mlt++/MltAnimation.h>
#include <mlt++/MltChain.h>
#include <mlt++/MltLink.h>
#include <mlt++/MltProfile.h>
#include <mlt++/MltProperties.h>
#include <mlt++/MltTractor.h>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr int MaxFrame = 10000000;
constexpr int MaxDuration = 1000000;
constexpr int MaxBatchItems = 8192;
QJsonObject typed(const char *type, const char *description)
{
    return Mcp::property(QString::fromUtf8(type), QString::fromUtf8(description));
}
QJsonObject choice(const char *description, const QJsonArray &values)
{
    auto p = typed("string", description);
    p.insert(QStringLiteral("enum"), values);
    return p;
}
QJsonObject idsProperty(const char *description, int maximum)
{
    auto p = typed("array", description);
    p.insert(QStringLiteral("items"),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 0}, {QStringLiteral("maximum"), MaxFrame}});
    p.insert(QStringLiteral("maxItems"), maximum);
    p.insert(QStringLiteral("uniqueItems"), true);
    return p;
}
bool boolean(const QJsonObject &a, const QString &key, bool fallback, bool &result)
{
    if (!a.contains(key)) {
        result = fallback;
        return true;
    }
    if (!a.value(key).isBool()) return false;
    result = a.value(key).toBool();
    return true;
}
bool ids(const QJsonValue &value, std::unordered_set<int> &result, int maximum, bool emptyAllowed = false)
{
    if (!value.isArray()) return false;
    const auto array = value.toArray();
    if (array.size() > maximum || (!emptyAllowed && array.isEmpty())) return false;
    for (const auto &entry : array) {
        int id = -1;
        if (!Mcp::integer(QJsonObject{{QStringLiteral("id"), entry}}, QStringLiteral("id"), 0, MaxFrame, id) || !result.insert(id).second) return false;
    }
    return true;
}
QJsonArray sortedIds(const std::unordered_set<int> &items)
{
    std::vector<int> values(items.begin(), items.end());
    std::sort(values.begin(), values.end());
    QJsonArray result;
    for (int id : values)
        result.append(id);
    return result;
}
QJsonObject binReadback(const std::shared_ptr<AbstractProjectItem> &item, std::optional<int> siblingRow = std::nullopt)
{
    const auto parent = item->parent();
    const auto kind = item->itemType();
    QJsonObject result{{QStringLiteral("binId"), item->clipId()},
                       {QStringLiteral("parentId"), parent ? parent->clipId() : QString()},
                       {QStringLiteral("kind"), kind == AbstractProjectItem::FolderItem    ? QStringLiteral("folder")
                                                : kind == AbstractProjectItem::ClipItem    ? QStringLiteral("clip")
                                                : kind == AbstractProjectItem::SubClipItem ? QStringLiteral("subclip")
                                                                                           : QStringLiteral("subsequence")},
                       {QStringLiteral("name"), item->name().left(512)},
                       {QStringLiteral("description"), item->description().left(2048)},
                       {QStringLiteral("descriptionTruncated"), item->description().size() > 2048},
                       {QStringLiteral("row"), siblingRow ? *siblingRow : item->row()},
                       {QStringLiteral("childCount"), item->childCount()},
                       {QStringLiteral("rating"), int(item->rating())},
                       {QStringLiteral("tags"), item->tags().left(512)}};
    if (kind == AbstractProjectItem::ClipItem) {
        const auto clip = std::static_pointer_cast<ProjectClip>(item);
        const bool ready = clip->statusReady();
        result.insert(QStringLiteral("ready"), ready);
        result.insert(QStringLiteral("used"), clip->isIncludedInTimeline());
        result.insert(QStringLiteral("durationFrames"), ready ? clip->getFramePlaytime() : 0);
        result.insert(QStringLiteral("sequence"), clip->clipType() == ClipType::Timeline);
    } else if (kind == AbstractProjectItem::SubClipItem || kind == AbstractProjectItem::SubSequenceItem) {
        const auto zone = item->zone();
        result.insert(QStringLiteral("inFrame"), zone.x());
        result.insert(QStringLiteral("outFrame"), zone.y());
    }
    return result;
}
bool binTree(const std::shared_ptr<AbstractProjectItem> &parent, bool recursive, std::vector<std::shared_ptr<AbstractProjectItem>> &items)
{
    return McpState::binItems(parent, recursive, items);
}

QJsonObject markerReadback(const std::shared_ptr<MarkerListModel> &model, int offset, int limit)
{
    const auto all = model->getAllMarkers();
    QJsonArray markers;
    for (int row = offset; row < all.size() && row - offset < limit; ++row) {
        const auto &marker = all.at(row);
        const int frame = marker.time().frames(pCore->getCurrentFps());
        markers.append(QJsonObject{{QStringLiteral("markerId"), model->markerIdAtFrame(frame)},
                                   {QStringLiteral("frame"), frame},
                                   {QStringLiteral("durationFrames"), marker.duration().frames(pCore->getCurrentFps())},
                                   {QStringLiteral("comment"), marker.comment().left(2048)},
                                   {QStringLiteral("commentTruncated"), marker.comment().size() > 2048},
                                   {QStringLiteral("categoryId"), marker.markerType()}});
    }
    return {{QStringLiteral("markers"), markers},
            {QStringLiteral("total"), int(all.size())},
            {QStringLiteral("offset"), offset},
            {QStringLiteral("hasMore"), qint64(offset) + markers.size() < all.size()},
            {QStringLiteral("categories"), QJsonDocument::fromJson(model->categoriesToJSon().toUtf8()).array()}};
}
QJsonObject itemReadback(const std::shared_ptr<TimelineItemModel> &timeline, int id)
{
    QJsonObject item{{QStringLiteral("id"), id}, {QStringLiteral("present"), timeline->isItem(id)}};
    if (!timeline->isItem(id)) return item;
    item.insert(QStringLiteral("trackId"), timeline->getItemTrackId(id));
    item.insert(QStringLiteral("startFrame"), timeline->getItemPosition(id));
    item.insert(QStringLiteral("durationFrames"), timeline->getItemPlaytime(id));
    item.insert(QStringLiteral("grouped"), timeline->isInGroup(id));
    if (timeline->isClip(id)) {
        const auto range = timeline->getClipInOut(id);
        item.insert(QStringLiteral("inFrame"), range.first);
        item.insert(QStringLiteral("outFrame"), range.second);
        item.insert(QStringLiteral("binId"), timeline->getClipBinId(id));
        item.insert(QStringLiteral("speed"), timeline->getClipSpeed(id));
        item.insert(QStringLiteral("timeRemap"), timeline->data(timeline->makeClipIndexFromID(id), TimelineModel::TimeRemapRole).toBool());
    }
    return item;
}
QJsonArray itemsReadback(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &items)
{
    QJsonArray result;
    for (const auto &value : sortedIds(items))
        result.append(itemReadback(timeline, value.toInt()));
    return result;
}
bool unlockedItems(const std::shared_ptr<TimelineItemModel> &timeline, const std::unordered_set<int> &items)
{
    for (int id : items) {
        if (!timeline->isItem(id)) return false;
        if (timeline->isSubTitle(id)) {
            const auto subtitles = timeline->getSubtitleModel();
            if (!subtitles || subtitles->isLocked()) return false;
        } else {
            const int tid = timeline->getItemTrackId(id);
            if (!timeline->isTrack(tid) || timeline->trackIsLocked(tid)) return false;
        }
    }
    return true;
}
std::unordered_set<int> trackItems(const std::shared_ptr<TimelineItemModel> &timeline, const QVector<int> &tracks)
{
    std::unordered_set<int> result;
    for (int track : tracks) {
        const auto found = timeline->getItemsInRange(track, 0, -1, true);
        result.insert(found.begin(), found.end());
    }
    return result;
}
void appendStep(Fun &undo, Fun &redo, const Fun &stepUndo, const Fun &stepRedo)
{
    const Fun previousUndo = undo, previousRedo = redo;
    undo = [stepUndo, previousUndo] {
        const bool current = stepUndo();
        const bool previous = previousUndo();
        return current && previous;
    };
    redo = [previousRedo, stepRedo] { return previousRedo() && stepRedo(); };
}
} // namespace

QJsonArray KdenliveMcpServer::timelineToolCatalog()
{
    QJsonArray catalog;
    const auto expected = typed("string", "Exact expectedState from latest native state.");
    const auto id = typed("integer", "Native item or track ID from get_state.");
    const auto frame = typed("integer", "Integral project frame, 0 through 10000000.");
    const auto duration = typed("integer", "Positive frame count, at most 1000000.");
    const auto group = typed("boolean", "True explicitly applies to native group. Grouped targets otherwise rejected.");
    auto add = [&](const char *name, const char *description, QJsonObject p, QJsonArray required) {
        p.insert(QStringLiteral("expectedState"), expected);
        required.prepend(QStringLiteral("expectedState"));
        catalog.append(Mcp::tool(QString::fromUtf8(name), QString::fromUtf8(description), p, required));
    };
    add("move_clip", "Move ordinary image/video/audio. includeGroup moves its native group and AV partners.",
        {{QStringLiteral("clipId"), id}, {QStringLiteral("trackId"), id}, {QStringLiteral("startFrame"), frame}, {QStringLiteral("includeGroup"), group}},
        {QStringLiteral("clipId"), QStringLiteral("trackId"), QStringLiteral("startFrame")});
    add("cut_clip", "Split ordinary media at exact timeline frame; optionally split its group and preserve grouping.",
        {{QStringLiteral("clipId"), id}, {QStringLiteral("frame"), frame}, {QStringLiteral("includeGroup"), group}},
        {QStringLiteral("clipId"), QStringLiteral("frame")});
    add("trim_clip", "Trim left/right with native resizing, optionally ripple. Returns applied duration if source bounds clamp it.",
        {{QStringLiteral("clipId"), id},
         {QStringLiteral("durationFrames"), duration},
         {QStringLiteral("edge"), choice("Edge.", {QStringLiteral("left"), QStringLiteral("right")})},
         {QStringLiteral("ripple"), typed("boolean", "Shift following items.")},
         {QStringLiteral("includeGroup"), group}},
        {QStringLiteral("clipId"), QStringLiteral("durationFrames"), QStringLiteral("edge")});
    add("slip_clip", "Change source range without moving clip. Positive offsetFrames moves source later; exact range required.",
        {{QStringLiteral("clipId"), id},
         {QStringLiteral("offsetFrames"), typed("integer", "Signed source offset within +/-1000000.")},
         {QStringLiteral("includeGroup"), group}},
        {QStringLiteral("clipId"), QStringLiteral("offsetFrames")});
    add("delete_item", "Delete clip/composition/subtitle. includeGroup explicitly deletes entire group. Undo restores it.",
        {{QStringLiteral("itemId"), id}, {QStringLiteral("includeGroup"), group}}, {QStringLiteral("itemId")});
    add("duplicate_clip",
        "Duplicate exact source ranges, effects, time maps and native groups, including subtitles. Missing mix partners require includeMixPartners=true; their "
        "groups also require includeGroup=true.",
        {{QStringLiteral("clipId"), id},
         {QStringLiteral("trackId"), id},
         {QStringLiteral("startFrame"), frame},
         {QStringLiteral("includeGroup"), group},
         {QStringLiteral("includeMixPartners"), typed("boolean", "Explicitly include the complete same-track mix chain and, with includeGroup, its groups.")}},
        {QStringLiteral("clipId"), QStringLiteral("trackId"), QStringLiteral("startFrame")});
    add("group_items", "Create undoable group from clip/composition IDs; retain existing group hierarchies.",
        {{QStringLiteral("itemIds"), idsProperty("Two or more IDs.", MaxBatchItems)}}, {QStringLiteral("itemIds")});
    add("ungroup_items", "Ungroup topmost group containing item; undoable.", {{QStringLiteral("itemId"), id}}, {QStringLiteral("itemId")});
    add("set_selection", "Set/clear timeline selection. Selection changes do not enter undo history.",
        {{QStringLiteral("itemIds"), idsProperty("Item IDs; empty clears.", MaxBatchItems)}}, {QStringLiteral("itemIds")});
    add("add_track", "Add audio/video track. Position zero is model bottom; -1 appends at top.",
        {{QStringLiteral("position"), typed("integer", "Position -1 through track count.")},
         {QStringLiteral("audio"), typed("boolean", "True for audio.")},
         {QStringLiteral("name"), typed("string", "Name up to 160 characters.")}},
        {QStringLiteral("position"), QStringLiteral("audio")});
    add("delete_track", "Delete track and contents in one undo step. Nonempty requires deleteContents=true.",
        {{QStringLiteral("trackId"), id}, {QStringLiteral("deleteContents"), typed("boolean", "Allow deleting contents.")}}, {QStringLiteral("trackId")});
    add("move_track", "Move track one place up/down among same type; undoable.",
        {{QStringLiteral("trackId"), id}, {QStringLiteral("direction"), choice("Direction.", {QStringLiteral("up"), QStringLiteral("down")})}},
        {QStringLiteral("trackId"), QStringLiteral("direction")});
    add("rename_track", "Rename track; undoable.",
        {{QStringLiteral("trackId"), id}, {QStringLiteral("name"), typed("string", "Name up to 160 characters; empty resets.")}},
        {QStringLiteral("trackId"), QStringLiteral("name")});
    add("set_track_locked", "Lock/unlock track; undoable.", {{QStringLiteral("trackId"), id}, {QStringLiteral("locked"), typed("boolean", "Requested state.")}},
        {QStringLiteral("trackId"), QStringLiteral("locked")});
    add("set_track_muted", "Mute audio/hide video using native track output; undoable.",
        {{QStringLiteral("trackId"), id}, {QStringLiteral("muted"), typed("boolean", "Disable rendered output.")}},
        {QStringLiteral("trackId"), QStringLiteral("muted")});
    add("set_clip_speed", "Set constant speed including reverse. Returns applied range/duration. includeGroup changes every group clip.",
        {{QStringLiteral("clipId"), id},
         {QStringLiteral("speedPercent"), typed("number", "Percent -1600..1600; magnitude at least 1.")},
         {QStringLiteral("pitchCompensate"), typed("boolean", "Compensate pitch.")},
         {QStringLiteral("changeDuration"), typed("boolean", "Change duration with speed.")},
         {QStringLiteral("includeGroup"), group}},
        {QStringLiteral("clipId"), QStringLiteral("speedPercent"), QStringLiteral("pitchCompensate"), QStringLiteral("changeDuration")});
    add("set_time_remap_enabled", "Enable/disable native time map. Arbitrary map editing is separate.",
        {{QStringLiteral("clipId"), id}, {QStringLiteral("enabled"), typed("boolean", "Requested state.")}, {QStringLiteral("includeGroup"), group}},
        {QStringLiteral("clipId"), QStringLiteral("enabled")});
    add("set_clips_enabled", "Enable/disable explicit clips without changing selection/groups.",
        {{QStringLiteral("clipIds"), idsProperty("Clip IDs.", MaxBatchItems)}, {QStringLiteral("enabled"), typed("boolean", "Requested state.")}},
        {QStringLiteral("clipIds"), QStringLiteral("enabled")});
    add("edit_timeline_zone", "Lift/extract half-open zone on explicit tracks, or insert space. Include all affected group tracks.",
        {{QStringLiteral("trackIds"), idsProperty("Unlocked tracks.", 64)},
         {QStringLiteral("action"), choice("Operation.", {QStringLiteral("lift"), QStringLiteral("extract"), QStringLiteral("insert_space")})},
         {QStringLiteral("startFrame"), frame},
         {QStringLiteral("durationFrames"), duration}},
        {QStringLiteral("trackIds"), QStringLiteral("action"), QStringLiteral("startFrame"), QStringLiteral("durationFrames")});
    add("remove_gap", "Remove gap on explicit tracks. Each track must be empty throughout gap.",
        {{QStringLiteral("trackIds"), idsProperty("Unlocked tracks.", 64)},
         {QStringLiteral("startFrame"), frame},
         {QStringLiteral("durationFrames"), duration}},
        {QStringLiteral("trackIds"), QStringLiteral("startFrame"), QStringLiteral("durationFrames")});
    add("insert_zone", "Insert/overwrite exact source range on one track; no audio mirroring/dialogs. Ungroup affected material first.",
        {{QStringLiteral("binId"), typed("string", "Ready bin ID, including existing assets.")},
         {QStringLiteral("trackId"), id},
         {QStringLiteral("startFrame"), frame},
         {QStringLiteral("sourceInFrame"), frame},
         {QStringLiteral("durationFrames"), duration},
         {QStringLiteral("stream"), choice("Stream.", {QStringLiteral("video"), QStringLiteral("audio")})},
         {QStringLiteral("mode"), choice("Behavior.", {QStringLiteral("insert"), QStringLiteral("overwrite")})}},
        {QStringLiteral("binId"), QStringLiteral("trackId"), QStringLiteral("startFrame"), QStringLiteral("sourceInFrame"), QStringLiteral("durationFrames"),
         QStringLiteral("stream"), QStringLiteral("mode")});
    catalog.append(Mcp::tool(QStringLiteral("list_sequences"), QStringLiteral("List native sequences, bin IDs, names and active sequence."), {}, {}, true));
    add("create_sequence", "Create/open a native sequence with explicit track counts; GUI session required. Undoable.",
        {{QStringLiteral("name"), typed("string", "Nonempty name up to 160 characters.")},
         {QStringLiteral("audioTracks"), typed("integer", "Audio tracks 0..32.")},
         {QStringLiteral("videoTracks"), typed("integer", "Video tracks 0..32.")}},
        {QStringLiteral("name"), QStringLiteral("audioTracks"), QStringLiteral("videoTracks")});
    add("switch_sequence", "Activate an existing native sequence; GUI session required. No undo entry.",
        {{QStringLiteral("sequenceId"), typed("string", "Sequence UUID from list_sequences.")}}, {QStringLiteral("sequenceId")});
    add("rename_sequence", "Rename a native sequence, including its bin and tab label; GUI session required. Undoable.",
        {{QStringLiteral("sequenceId"), typed("string", "Sequence UUID.")}, {QStringLiteral("name"), typed("string", "Nonempty name up to 160 characters.")}},
        {QStringLiteral("sequenceId"), QStringLiteral("name")});
    add("delete_sequence",
        "Permanently delete a nonactive, unreferenced sequence. Native Kdenlive clears ALL project undo history. Explicit acknowledgeHistoryReset=true "
        "required.",
        {{QStringLiteral("sequenceId"), typed("string", "Sequence UUID.")},
         {QStringLiteral("acknowledgeHistoryReset"), typed("boolean", "Must explicitly be true; deletion clears the entire undo stack and is not undoable.")}},
        {QStringLiteral("sequenceId"), QStringLiteral("acknowledgeHistoryReset")});
    auto keys = typed(
        "array",
        "Ordered output-to-source frame mapping, clip-local output frames. Source frames use project fps. First output frame is zero, last is duration-1.");
    keys.insert(QStringLiteral("minItems"), 2);
    keys.insert(QStringLiteral("maxItems"), 256);
    keys.insert(
        QStringLiteral("items"),
        QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("properties"),
                     QJsonObject{{QStringLiteral("frame"), frame},
                                 {QStringLiteral("sourceFrame"), frame},
                                 {QStringLiteral("interpolation"),
                                  choice("Interpolation to next key.", {QStringLiteral("linear"), QStringLiteral("discrete"), QStringLiteral("smooth")})}}},
                    {QStringLiteral("required"), QJsonArray{QStringLiteral("frame"), QStringLiteral("sourceFrame")}},
                    {QStringLiteral("additionalProperties"), false}});
    add("set_clip_time_map", "Set native speed-ramp/freeze/reverse frame map over the current clip duration; enables time remapping. Undoable.",
        {{QStringLiteral("clipId"), id},
         {QStringLiteral("keyframes"), keys},
         {QStringLiteral("pitchCompensate"), typed("boolean", "Compensate audio pitch.")},
         {QStringLiteral("frameBlending"), typed("boolean", "Blend remapped video frames.")},
         {QStringLiteral("includeGroup"), group}},
        {QStringLiteral("clipId"), QStringLiteral("keyframes"), QStringLiteral("pitchCompensate"), QStringLiteral("frameBlending")});
    catalog.append(Mcp::tool(QStringLiteral("get_bin"), QStringLiteral("Read paged native bin folders, media and source zones; no filesystem writes."),
                             {{QStringLiteral("parentId"), typed("string", "Parent native bin ID; omitted means root.")},
                              {QStringLiteral("recursive"), typed("boolean", "Include descendants; default true.")},
                              {QStringLiteral("offset"), typed("integer", "Page offset, default zero.")},
                              {QStringLiteral("limit"), typed("integer", "Page size 1..128; default 64.")}},
                             {}, true));
    add("edit_bin",
        "Native undoable folder creation, move, rename, metadata, unused deletion, and source zones. Deletes only project entries, preserving source files. "
        "Sequences require sequence tools.",
        {{QStringLiteral("operation"),
          choice("Operation.", {QStringLiteral("create_folder"), QStringLiteral("move"), QStringLiteral("rename"), QStringLiteral("description"),
                                QStringLiteral("rating"), QStringLiteral("tags"), QStringLiteral("delete"), QStringLiteral("create_subclip")})},
         {QStringLiteral("itemId"), typed("string", "Target native bin ID; parent clip for create_subclip.")},
         {QStringLiteral("parentId"), typed("string", "Destination folder; omitted root for create_folder.")},
         {QStringLiteral("name"), typed("string", "Nonempty name, up to 160 characters.")},
         {QStringLiteral("description"), typed("string", "Source description, up to 8192 characters.")},
         {QStringLiteral("rating"), typed("integer", "Rating 0..5.")},
         {QStringLiteral("tags"), typed("string", "Native tag string, up to 1024 characters.")},
         {QStringLiteral("inFrame"), frame},
         {QStringLiteral("outFrame"), frame},
         {QStringLiteral("deleteContents"), typed("boolean", "Explicitly delete a nonempty folder's unused contents.")}},
        {QStringLiteral("operation")});
    const auto markerScope = choice("Marker owner.", {QStringLiteral("guides"), QStringLiteral("clip")});
    catalog.append(Mcp::tool(QStringLiteral("get_markers"),
                             QStringLiteral("Read paged sequence guides or source markers, range lengths and global categories."),
                             {{QStringLiteral("scope"), markerScope},
                              {QStringLiteral("binId"), typed("string", "Required only for clip scope.")},
                              {QStringLiteral("offset"), typed("integer", "Page offset, default zero.")},
                              {QStringLiteral("limit"), typed("integer", "Page size 1..128; default 64.")}},
                             {QStringLiteral("scope")}, true));
    add("edit_marker",
        "Add/update/delete/clear point or range markers using native undo. Position addressed; positive ranges use an exclusive end. Existing positions cannot "
        "be overwritten by add or move.",
        {{QStringLiteral("scope"), markerScope},
         {QStringLiteral("binId"), typed("string", "Required only for clip scope.")},
         {QStringLiteral("operation"),
          choice("Operation.", {QStringLiteral("add"), QStringLiteral("update"), QStringLiteral("delete"), QStringLiteral("clear")})},
         {QStringLiteral("frame"), frame},
         {QStringLiteral("newFrame"), frame},
         {QStringLiteral("durationFrames"), typed("integer", "Zero for point marker; positive range length.")},
         {QStringLiteral("comment"), typed("string", "Comment up to 8192 characters; empty input uses the native localized Marker placeholder.")},
         {QStringLiteral("categoryId"), typed("integer", "Existing global category ID.")}},
        {QStringLiteral("scope"), QStringLiteral("operation")});
    add("edit_marker_category",
        "Add/update/delete a global project marker category in one native project undo. Deletion defaults to rejecting used categories. Explicit remap "
        "preserves point/range markers and IDs; explicit marker deletion requires acknowledgement. Application defaults are unchanged.",
        {{QStringLiteral("operation"), choice("Operation.", {QStringLiteral("add"), QStringLiteral("update"), QStringLiteral("delete")})},
         {QStringLiteral("categoryId"), typed("integer", "Category ID 0..1000000.")},
         {QStringLiteral("name"), typed("string", "Nonempty name up to 160 characters.")},
         {QStringLiteral("color"), typed("string", "Color #RRGGBB.")},
         {QStringLiteral("markerPolicy"), choice("Delete only: reject used categories (default), remap markers, or delete markers.",
                                                 {QStringLiteral("reject"), QStringLiteral("remap"), QStringLiteral("delete")})},
         {QStringLiteral("replacementCategoryId"), typed("integer", "Delete/remap only: another existing category ID.")},
         {QStringLiteral("acknowledgeMarkerDeletion"), typed("boolean", "Required true when markerPolicy is delete.")}},
        {QStringLiteral("operation"), QStringLiteral("categoryId")});
    return catalog;
}

QJsonObject KdenliveMcpServer::callTimelineTool(const QString &name, const QJsonObject &arguments, const QJsonObject &state)
{
    bool known = false;
    for (const auto &entry : timelineToolCatalog())
        if (entry.toObject().value(QStringLiteral("name")).toString() == name) {
            known = true;
            break;
        }
    if (!known) return {};
    const auto timeline = pCore->projectManager()->getTimeline();
    const auto stack = pCore->undoStack();
    auto reject = [&](const QString &code, const QString &message) { return Mcp::failure(code, message, state); };
    auto finish = [&](QJsonObject data = {}) {
        ++m_revision;
        data.insert(QStringLiteral("ok"), true);
        data.insert(QStringLiteral("state"), snapshot());
        return Mcp::toolResult(data);
    };
    auto rollback = [&](const Fun &undo, const QString &message) {
        const bool restored = undo();
        return Mcp::failure(restored ? QStringLiteral("EDIT_FAILED") : QStringLiteral("ROLLBACK_FAILED"),
                            restored ? message + QStringLiteral(" Changes were rolled back.") : QStringLiteral("Native rollback failed; inspect the project."),
                            snapshot());
    };
    auto commit = [&](const Fun &undo, const Fun &redo, const QString &label, QJsonObject data = {}) {
        pCore->pushUndo(undo, redo, label);
        return finish(data);
    };
    if (!timeline || !stack) return reject(QStringLiteral("NOT_READY"), QStringLiteral("No active native timeline."));
    if (name == QLatin1String("get_bin") || name == QLatin1String("edit_bin")) {
        const auto bin = pCore->projectItemModel();
        const auto root = bin->getRootFolder();
        auto readText = [&](const QString &key, int maximum, bool empty, QString &value) {
            const auto supplied = arguments.value(key);
            if (!supplied.isString() || supplied.toString().size() > maximum || supplied.toString().contains(QChar::Null) ||
                (!empty && supplied.toString().isEmpty()))
                return false;
            value = supplied.toString();
            return true;
        };
        auto resolve = [&](const QString &id) -> std::shared_ptr<AbstractProjectItem> { return id == root->clipId() ? root : bin->getItemByBinId(id); };
        if (name == QLatin1String("get_bin")) {
            int offset = 0, limit = 64;
            bool recursive = true;
            QString parentId = root->clipId();
            if ((arguments.contains(QStringLiteral("offset")) && !Mcp::integer(arguments, QStringLiteral("offset"), 0, 1000000, offset)) ||
                (arguments.contains(QStringLiteral("limit")) && !Mcp::integer(arguments, QStringLiteral("limit"), 1, 128, limit)) ||
                !boolean(arguments, QStringLiteral("recursive"), true, recursive) ||
                (arguments.contains(QStringLiteral("parentId")) && !readText(QStringLiteral("parentId"), 128, false, parentId)))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use a valid parent, boolean recursive flag and bounded page parameters."));
            const auto parent = resolve(parentId);
            if (!parent) return reject(QStringLiteral("INVALID_BIN_ITEM"), QStringLiteral("Unknown parent bin ID."));
            std::vector<std::shared_ptr<AbstractProjectItem>> items;
            if (!binTree(parent, recursive, items))
                return reject(QStringLiteral("LIMIT"), QStringLiteral("The native bin hierarchy contains a cycle or duplicate child."));
            QJsonArray result;
            QHash<const AbstractProjectItem *, int> nextSibling;
            for (int row = 0; row < int(items.size()) && row - offset < limit; ++row) {
                const auto &item = items.at(row);
                const auto parent = item->parent();
                const int siblingRow = nextSibling[parent.get()]++;
                if (row >= offset) result.append(binReadback(item, siblingRow));
            }
            return Mcp::toolResult({{QStringLiteral("ok"), true},
                                    {QStringLiteral("rootId"), root->clipId()},
                                    {QStringLiteral("parent"), binReadback(parent)},
                                    {QStringLiteral("items"), result},
                                    {QStringLiteral("total"), int(items.size())},
                                    {QStringLiteral("offset"), offset},
                                    {QStringLiteral("hasMore"), qint64(offset) + result.size() < int(items.size())},
                                    {QStringLiteral("state"), state}});
        }
        const auto operation = arguments.value(QStringLiteral("operation")).toString();
        QStringList allowed{QStringLiteral("operation"), QStringLiteral("expectedState")};
        if (operation == QLatin1String("create_folder"))
            allowed += {QStringLiteral("name"), QStringLiteral("parentId")};
        else if (operation == QLatin1String("move"))
            allowed += {QStringLiteral("itemId"), QStringLiteral("parentId")};
        else if (operation == QLatin1String("rename"))
            allowed += {QStringLiteral("itemId"), QStringLiteral("name")};
        else if (operation == QLatin1String("description"))
            allowed += {QStringLiteral("itemId"), QStringLiteral("description")};
        else if (operation == QLatin1String("rating"))
            allowed += {QStringLiteral("itemId"), QStringLiteral("rating")};
        else if (operation == QLatin1String("tags"))
            allowed += {QStringLiteral("itemId"), QStringLiteral("tags")};
        else if (operation == QLatin1String("delete"))
            allowed += {QStringLiteral("itemId"), QStringLiteral("deleteContents")};
        else if (operation == QLatin1String("create_subclip"))
            allowed += {QStringLiteral("itemId"),   QStringLiteral("name"),   QStringLiteral("inFrame"),
                        QStringLiteral("outFrame"), QStringLiteral("rating"), QStringLiteral("tags")};
        else
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Unknown native bin operation."));
        for (auto i = arguments.begin(); i != arguments.end(); ++i)
            if (!allowed.contains(i.key()))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Argument does not apply to this bin operation: ") + i.key());
        if (operation == QLatin1String("create_folder")) {
            QString title, parentId = root->clipId();
            if (!readText(QStringLiteral("name"), 160, false, title) ||
                (arguments.contains(QStringLiteral("parentId")) && !readText(QStringLiteral("parentId"), 128, false, parentId)))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use nonempty folder name and valid parent ID."));
            const auto parent = resolve(parentId);
            if (!parent || parent->itemType() != AbstractProjectItem::FolderItem)
                return reject(QStringLiteral("INVALID_BIN_ITEM"), QStringLiteral("Folder parent must be a native folder."));
            QString id;
            Fun undo = [] { return true; }, redo = [] { return true; };
            if (!bin->requestAddFolder(id, title, parentId, undo, redo)) return rollback(undo, QStringLiteral("Native folder creation failed."));
            return commit(undo, redo, QStringLiteral("MCP create bin folder"), {{QStringLiteral("item"), binReadback(resolve(id))}});
        }
        QString itemId;
        if (!readText(QStringLiteral("itemId"), 128, false, itemId))
            return reject(QStringLiteral("INVALID_BIN_ITEM"), QStringLiteral("Supply target itemId from get_bin."));
        const auto item = resolve(itemId);
        if (!item || item == root) return reject(QStringLiteral("INVALID_BIN_ITEM"), QStringLiteral("Target must be an existing non-root bin item."));
        if (item->itemType() == AbstractProjectItem::ClipItem && item->clipType() == ClipType::Timeline)
            return reject(QStringLiteral("SEQUENCE_SCOPE"), QStringLiteral("Use sequence tools for sequence assets."));
        if (operation == QLatin1String("move")) {
            QString parentId;
            if (!readText(QStringLiteral("parentId"), 128, false, parentId))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Supply destination parentId from get_bin."));
            const auto parent = resolve(parentId), previousParent = item->parent();
            if (!parent || parent->itemType() != AbstractProjectItem::FolderItem || !previousParent || item->itemType() == AbstractProjectItem::SubClipItem ||
                item->itemType() == AbstractProjectItem::SubSequenceItem || parent->hasAncestor(item->getId()))
                return reject(QStringLiteral("INVALID_BIN_ITEM"),
                              QStringLiteral("Move a clip/folder to a folder outside its own subtree; source zones remain under their source."));
            if (parent == previousParent) return finish({{QStringLiteral("item"), binReadback(item)}});
            const int oldRow = item->row();
            Fun undo = [item, previousParent, oldRow] {
                if (!item->changeParent(previousParent)) return false;
                if (oldRow >= 0 && oldRow < previousParent->childCount() && item->row() != oldRow) previousParent->moveChild(oldRow, item);
                return true;
            };
            Fun redo = [item, parent] { return item->changeParent(parent); };
            if (!redo()) return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native bin move failed."));
            return commit(undo, redo, QStringLiteral("MCP move bin item"), {{QStringLiteral("item"), binReadback(item)}});
        }
        if (operation == QLatin1String("delete")) {
            bool contents = false;
            if (!boolean(arguments, QStringLiteral("deleteContents"), false, contents))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("deleteContents must be boolean."));
            if (item->childCount() > 0 && !contents)
                return reject(QStringLiteral("BIN_NOT_EMPTY"), QStringLiteral("Explicit deleteContents=true is required for a nonempty item."));
            std::vector<std::shared_ptr<AbstractProjectItem>> affected{item};
            if (!binTree(item, true, affected))
                return reject(QStringLiteral("LIMIT"), QStringLiteral("The native bin subtree contains a cycle or duplicate child."));
            for (const auto &candidate : affected) {
                if (candidate->itemType() != AbstractProjectItem::ClipItem) continue;
                const auto clip = std::static_pointer_cast<ProjectClip>(candidate);
                if (clip->clipType() == ClipType::Timeline)
                    return reject(QStringLiteral("SEQUENCE_SCOPE"),
                                  QStringLiteral("A folder containing sequences cannot bypass explicit sequence-deletion acknowledgement."));
                if (clip->isIncludedInTimeline())
                    return reject(QStringLiteral("BIN_ITEM_USED"), QStringLiteral("Remove timeline instances before deleting this source asset."));
                if (!clip->statusReady() && clip->clipStatus() != FileStatus::StatusMissing)
                    return reject(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Wait for native source loading before bin deletion."));
            }
            Fun undo = [] { return true; }, redo = [] { return true; };
            if (!bin->requestBinClipDeletion(item, undo, redo)) return rollback(undo, QStringLiteral("Native bin deletion failed."));
            return commit(undo, redo, QStringLiteral("MCP delete unused bin item"),
                          {{QStringLiteral("deletedBinId"), itemId}, {QStringLiteral("sourceFilesPreserved"), true}});
        }
        if (operation == QLatin1String("create_subclip")) {
            if (item->itemType() != AbstractProjectItem::ClipItem)
                return reject(QStringLiteral("INVALID_BIN_ITEM"), QStringLiteral("Source zones require an ordinary source clip."));
            const auto clip = std::static_pointer_cast<ProjectClip>(item);
            if (!clip->statusReady()) return reject(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Wait for source loading."));
            int in = 0, out = 0, rating = 0;
            QString title, tags;
            if (!readText(QStringLiteral("name"), 160, false, title) || !Mcp::integer(arguments, QStringLiteral("inFrame"), 0, MaxFrame, in) ||
                !Mcp::integer(arguments, QStringLiteral("outFrame"), 0, MaxFrame, out) || out <= in || out >= clip->getFramePlaytime() ||
                (arguments.contains(QStringLiteral("rating")) && !Mcp::integer(arguments, QStringLiteral("rating"), 0, 5, rating)) ||
                (arguments.contains(QStringLiteral("tags")) && !readText(QStringLiteral("tags"), 1024, true, tags)))
                return reject(QStringLiteral("INVALID_SOURCE_RANGE"),
                              QStringLiteral("Use a named inclusive source zone of at least two frames within the source, rating 0..5 and bounded tags."));
            if (clip->getSubClip(in, out))
                return reject(QStringLiteral("ZONE_EXISTS"), QStringLiteral("This exact source zone already exists; rename it instead."));
            QString id;
            Fun nativeUndo = [] { return true; }, nativeRedo = [] { return true; };
            if (!bin->requestAddBinSubClip(
                    id, in, out, {{QStringLiteral("name"), title}, {QStringLiteral("rating"), QString::number(rating)}, {QStringLiteral("tags"), tags}}, itemId,
                    nativeUndo, nativeRedo))
                return rollback(nativeUndo, QStringLiteral("Native source-zone creation failed."));
            clip->updateZones();
            Fun undo = [nativeUndo, clip] {
                const bool ok = nativeUndo();
                clip->updateZones();
                return ok;
            };
            Fun redo = [nativeRedo, clip] {
                const bool ok = nativeRedo();
                clip->updateZones();
                return ok;
            };
            return commit(undo, redo, QStringLiteral("MCP create source zone"), {{QStringLiteral("item"), binReadback(resolve(id))}});
        }
        const auto kind = item->itemType();
        QString value;
        int rating = 0;
        if (operation == QLatin1String("rating")) {
            if (!Mcp::integer(arguments, QStringLiteral("rating"), 0, 5, rating))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use rating 0..5."));
            value = QString::number(rating);
        } else {
            const QString key = operation == QLatin1String("rename") ? QStringLiteral("name") : operation;
            if (!readText(key,
                          operation == QLatin1String("description") ? 8192
                          : operation == QLatin1String("tags")      ? 1024
                                                                    : 160,
                          operation != QLatin1String("rename"), value))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Supply bounded text for the requested metadata field."));
        }
        if (kind == AbstractProjectItem::FolderItem) {
            if (operation != QLatin1String("rename"))
                return reject(QStringLiteral("INVALID_BIN_ITEM"), QStringLiteral("Folder metadata exposes its native name only."));
            Fun undo = [] { return true; }, redo = [] { return true; };
            if (!bin->requestRenameFolder(item, value, undo, redo)) return rollback(undo, QStringLiteral("Native folder rename failed."));
            return commit(undo, redo, QStringLiteral("MCP rename bin folder"), {{QStringLiteral("item"), binReadback(item)}});
        }
        if (kind == AbstractProjectItem::ClipItem) {
            const auto clip = std::static_pointer_cast<ProjectClip>(item);
            if (!clip->statusReady()) return reject(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Wait for source loading before editing metadata."));
            if (operation == QLatin1String("rating")) {
                const uint previousRating = clip->rating();
                const QString previousValue = clip->getProducerProperty(QStringLiteral("kdenlive:rating"));
                Fun undo = [clip, bin, previousRating, previousValue] {
                    clip->setRating(previousRating);
                    clip->setProducerProperty(QStringLiteral("kdenlive:rating"), previousValue);
                    bin->onItemUpdated(clip, {AbstractProjectItem::DataRating});
                    return clip->rating() == previousRating && clip->getProducerProperty(QStringLiteral("kdenlive:rating")) == previousValue;
                };
                Fun redo = [clip, bin, rating] {
                    // The native rating setter updates both the cached bin value
                    // and the producer; generic setProperties only updates MLT.
                    clip->setRating(static_cast<uint>(rating));
                    bin->onItemUpdated(clip, {AbstractProjectItem::DataRating});
                    return clip->rating() == static_cast<uint>(rating) && clip->getProducerIntProperty(QStringLiteral("kdenlive:rating")) == rating;
                };
                if (!redo()) return rollback(undo, QStringLiteral("Native source rating did not match the requested value."));
                return commit(undo, redo, QStringLiteral("MCP edit source rating"), {{QStringLiteral("item"), binReadback(item)}});
            }
            const QString key = operation == QLatin1String("rename") ? QStringLiteral("kdenlive:clipname") : QStringLiteral("kdenlive:") + operation;
            const QString previous = clip->getProducerProperty(key);
            Fun undo = [clip, key, previous] {
                clip->setProperties({{key, previous}});
                return true;
            };
            Fun redo = [clip, key, value] {
                clip->setProperties({{key, value}});
                return true;
            };
            redo();
            return commit(undo, redo, QStringLiteral("MCP edit source metadata"), {{QStringLiteral("item"), binReadback(item)}});
        }
        if (kind != AbstractProjectItem::SubClipItem || operation == QLatin1String("description"))
            return reject(QStringLiteral("INVALID_BIN_ITEM"), QStringLiteral("This native bin item does not expose that metadata field."));
        const auto subclip = std::static_pointer_cast<ProjectSubClip>(item);
        const auto master = subclip->getMasterClip();
        const QString previous = operation == QLatin1String("rename")   ? subclip->name()
                                 : operation == QLatin1String("rating") ? QString::number(subclip->rating())
                                                                        : subclip->tags();
        auto apply = [bin, subclip, master, operation](const QString &v) {
            if (operation == QLatin1String("rename"))
                subclip->setName(v);
            else
                subclip->setProperties({{QStringLiteral("kdenlive:") + operation, v}});
            master->updateZones();
            bin->onItemUpdated(subclip, {AbstractProjectItem::DataName, AbstractProjectItem::DataRating, AbstractProjectItem::DataTag});
            return true;
        };
        Fun undo = [apply, previous] { return apply(previous); }, redo = [apply, value] { return apply(value); };
        redo();
        return commit(undo, redo, QStringLiteral("MCP edit source-zone metadata"), {{QStringLiteral("item"), binReadback(item)}});
    }
    if (name == QLatin1String("get_markers") || name == QLatin1String("edit_marker")) {
        const QString scope = arguments.value(QStringLiteral("scope")).toString();
        std::shared_ptr<MarkerListModel> model;
        int sourceLength = MaxFrame + 1;
        if (scope == QLatin1String("guides")) {
            if (arguments.contains(QStringLiteral("binId")))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("binId is only valid for clip marker scope."));
            model = timeline->getGuideModel();
        } else if (scope == QLatin1String("clip")) {
            const auto supplied = arguments.value(QStringLiteral("binId"));
            if (!supplied.isString() || supplied.toString().size() > 128 || !pCore->projectItemModel()->hasClip(supplied.toString()))
                return reject(QStringLiteral("INVALID_BIN_ITEM"), QStringLiteral("Clip marker scope requires a valid binId."));
            const auto clip = pCore->projectItemModel()->getClipByBinID(supplied.toString());
            if (!clip->statusReady()) return reject(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Wait for marker source loading."));
            model = clip->markerModel();
            sourceLength = clip->getFramePlaytime();
        } else
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Marker scope must be guides or clip."));
        if (!model) return reject(QStringLiteral("NOT_READY"), QStringLiteral("Native marker model is unavailable."));
        if (name == QLatin1String("get_markers")) {
            int offset = 0, limit = 64;
            if ((arguments.contains(QStringLiteral("offset")) && !Mcp::integer(arguments, QStringLiteral("offset"), 0, 1000000, offset)) ||
                (arguments.contains(QStringLiteral("limit")) && !Mcp::integer(arguments, QStringLiteral("limit"), 1, 128, limit)))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use bounded marker page parameters."));
            auto data = markerReadback(model, offset, limit);
            data.insert(QStringLiteral("ok"), true);
            data.insert(QStringLiteral("scope"), scope);
            data.insert(QStringLiteral("state"), state);
            return Mcp::toolResult(data);
        }
        const auto operation = arguments.value(QStringLiteral("operation")).toString();
        QStringList allowed{QStringLiteral("scope"), QStringLiteral("binId"), QStringLiteral("operation"), QStringLiteral("expectedState")};
        if (operation == QLatin1String("add"))
            allowed += {QStringLiteral("frame"), QStringLiteral("durationFrames"), QStringLiteral("comment"), QStringLiteral("categoryId")};
        else if (operation == QLatin1String("update"))
            allowed += {QStringLiteral("frame"), QStringLiteral("newFrame"), QStringLiteral("durationFrames"), QStringLiteral("comment"),
                        QStringLiteral("categoryId")};
        else if (operation == QLatin1String("delete"))
            allowed += {QStringLiteral("frame")};
        else if (operation != QLatin1String("clear"))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Unknown native marker operation."));
        for (auto i = arguments.begin(); i != arguments.end(); ++i)
            if (!allowed.contains(i.key()))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Argument does not apply to marker operation: ") + i.key());
        if (operation == QLatin1String("clear")) {
            if (!model->removeAllMarkers()) return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native marker clearing failed."));
            return finish(markerReadback(model, 0, 64));
        }
        int frame = -1;
        if (!Mcp::integer(arguments, QStringLiteral("frame"), 0, MaxFrame, frame))
            return reject(QStringLiteral("INVALID_FRAME"), QStringLiteral("Use bounded native marker frame."));
        const bool exists = model->hasMarker(frame);
        if (operation == QLatin1String("add") ? exists : !exists)
            return reject(exists ? QStringLiteral("MARKER_EXISTS") : QStringLiteral("INVALID_MARKER"),
                          QStringLiteral("Add requires a free position; update/delete requires an existing marker."));
        if (operation == QLatin1String("delete")) {
            if (!model->removeMarker(GenTime(frame, pCore->getCurrentFps())))
                return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native marker deletion failed."));
            return finish(markerReadback(model, 0, 64));
        }
        if (!exists && model->rowCount() >= 4096) return reject(QStringLiteral("LIMIT"), QStringLiteral("Native MCP marker limit reached."));
        const auto before = exists ? model->marker(frame) : CommentedTime();
        int destination = frame, duration = exists ? before.duration().frames(pCore->getCurrentFps()) : 0;
        int category = exists ? before.markerType() : KdenliveSettings::default_marker_type();
        QString comment = exists ? before.comment() : QStringLiteral("Marker");
        if ((arguments.contains(QStringLiteral("newFrame")) && !Mcp::integer(arguments, QStringLiteral("newFrame"), 0, MaxFrame, destination)) ||
            (arguments.contains(QStringLiteral("durationFrames")) && !Mcp::integer(arguments, QStringLiteral("durationFrames"), 0, MaxDuration, duration)) ||
            (arguments.contains(QStringLiteral("categoryId")) && !Mcp::integer(arguments, QStringLiteral("categoryId"), 0, MaxFrame, category)) ||
            !pCore->markerTypes.contains(category) || qint64(destination) + duration > MaxFrame || destination >= sourceLength ||
            qint64(destination) + duration > sourceLength)
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use an existing category and marker/range within source/frame bounds."));
        if (arguments.contains(QStringLiteral("comment"))) {
            const auto value = arguments.value(QStringLiteral("comment"));
            if (!value.isString() || value.toString().size() > 8192 || value.toString().contains(QChar::Null))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Comment must be text up to 8192 characters."));
            comment = value.toString();
        }
        if (destination != frame && model->hasMarker(destination))
            return reject(QStringLiteral("MARKER_EXISTS"), QStringLiteral("Destination already has a marker."));
        Fun undo = [] { return true; }, redo = [] { return true; };
        if (exists && !model->removeMarker(GenTime(frame, pCore->getCurrentFps()), undo, redo))
            return rollback(undo, QStringLiteral("Native marker replacement failed."));
        const QJsonArray marker{QJsonObject{{QStringLiteral("pos"), destination},
                                            {QStringLiteral("duration"), duration},
                                            {QStringLiteral("comment"), comment},
                                            {QStringLiteral("type"), category}}};
        Fun addUndo = [] { return true; }, addRedo = [] { return true; };
        // The native import supports empty comments and point/range conversion.
        // Its failed transaction already undoes itself; keep earlier removal separate.
        if (!model->importFromJson(QString::fromUtf8(QJsonDocument(marker).toJson(QJsonDocument::Compact)), false, addUndo, addRedo))
            return rollback(undo, QStringLiteral("Native marker creation failed."));
        appendStep(undo, redo, addUndo, addRedo);
        const auto actual = model->marker(destination);
        // Kdenlive intentionally presents an empty stored comment as its localized
        // "Marker" placeholder. Compare the native representation, not raw input.
        const CommentedTime requested(GenTime(destination, pCore->getCurrentFps()), comment, category, GenTime(duration, pCore->getCurrentFps()));
        if (actual.comment() != requested.comment() || actual.markerType() != category || actual.duration().frames(pCore->getCurrentFps()) != duration)
            return rollback(undo, QStringLiteral("Native marker readback differed from requested values."));
        auto data = markerReadback(model, 0, 64);
        data.insert(QStringLiteral("commentNormalized"), actual.comment() != comment);
        return commit(undo, redo, QStringLiteral("MCP edit marker"), data);
    }
    if (name == QLatin1String("edit_marker_category")) {
        const auto operation = arguments.value(QStringLiteral("operation")).toString();
        int category = -1;
        if (!Mcp::integer(arguments, QStringLiteral("categoryId"), 0, 1000000, category) ||
            (operation != QLatin1String("add") && operation != QLatin1String("update") && operation != QLatin1String("delete")))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use add/update/delete and bounded categoryId."));
        const bool exists = pCore->markerTypes.contains(category);
        if (operation == QLatin1String("add") ? exists : !exists)
            return reject(QStringLiteral("INVALID_CATEGORY"), QStringLiteral("Add requires new category ID; update/delete requires existing ID."));
        const auto model = timeline->getGuideModel();
        const auto document = pCore->currentDoc();
        if (!model || !document) return reject(QStringLiteral("NOT_READY"), QStringLiteral("Native guide/category model unavailable."));
        auto categories = pCore->markerTypes;
        QMap<int, int> remap;
        QString policy;
        int affectedMarkers = 0;
        if (operation == QLatin1String("delete")) {
            if (arguments.contains(QStringLiteral("name")) || arguments.contains(QStringLiteral("color")))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Category deletion does not take name/color."));
            if (categories.size() <= 1 || category == KdenliveSettings::default_marker_type())
                return reject(QStringLiteral("DEFAULT_CATEGORY"),
                              QStringLiteral("Preserve at least one category and the current application default category."));
            policy = arguments.value(QStringLiteral("markerPolicy")).toString(QStringLiteral("reject"));
            if ((arguments.contains(QStringLiteral("markerPolicy")) && !arguments.value(QStringLiteral("markerPolicy")).isString()) ||
                (policy != QLatin1String("reject") && policy != QLatin1String("remap") && policy != QLatin1String("delete")))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("markerPolicy must be reject, remap, or delete."));
            if (policy == QLatin1String("remap")) {
                int replacement = -1;
                if (!Mcp::integer(arguments, QStringLiteral("replacementCategoryId"), 0, 1000000, replacement) || replacement == category ||
                    !categories.contains(replacement) || arguments.contains(QStringLiteral("acknowledgeMarkerDeletion")))
                    return reject(QStringLiteral("INVALID_ARGUMENT"),
                                  QStringLiteral("Remap requires a different existing replacementCategoryId and no deletion acknowledgement."));
                remap.insert(category, replacement);
            } else if (arguments.contains(QStringLiteral("replacementCategoryId"))) {
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("replacementCategoryId requires markerPolicy=remap."));
            }
            if (policy == QLatin1String("delete")) {
                const auto acknowledged = arguments.value(QStringLiteral("acknowledgeMarkerDeletion"));
                if (!acknowledged.isBool() || !acknowledged.toBool())
                    return reject(QStringLiteral("ACKNOWLEDGEMENT_REQUIRED"),
                                  QStringLiteral("Deleting markers/guides requires acknowledgeMarkerDeletion=true. This remains undoable."));
            } else if (arguments.contains(QStringLiteral("acknowledgeMarkerDeletion"))) {
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("acknowledgeMarkerDeletion requires markerPolicy=delete."));
            }
            std::unordered_set<MarkerListModel *> seen;
            auto count = [&](const std::shared_ptr<MarkerListModel> &markers) {
                if (!markers) return false;
                if (seen.insert(markers.get()).second) affectedMarkers += markers->getAllMarkers(category).size();
                return true;
            };
            // Walk attached bin children; cached UUID/ID maps can retain deleted sequence entries.
            for (const auto &clip : pCore->projectItemModel()->getRootFolder()->childClips()) {
                if (!clip || !clip->statusReady() || !count(clip->getMarkerModel()))
                    return reject(QStringLiteral("MEDIA_NOT_READY"),
                                  QStringLiteral("Wait for all source/sequence marker models before deleting a global category."));
            }
            for (const auto &uuid : document->getTimelinesUuids()) {
                const auto sequence = document->getTimeline(uuid, true);
                if (!sequence || !count(sequence->getGuideModel()))
                    return reject(QStringLiteral("NOT_READY"), QStringLiteral("A native sequence guide model is unavailable."));
            }
            if (affectedMarkers > 0 && policy == QLatin1String("reject"))
                return reject(QStringLiteral("CATEGORY_USED"),
                              QStringLiteral("Choose markerPolicy=remap, or explicitly acknowledge markerPolicy=delete, for used categories."));
            categories.remove(category);
        } else {
            if (arguments.contains(QStringLiteral("markerPolicy")) || arguments.contains(QStringLiteral("replacementCategoryId")) ||
                arguments.contains(QStringLiteral("acknowledgeMarkerDeletion")))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Marker deletion policy fields apply only to category deletion."));
            const auto title = arguments.value(QStringLiteral("name")), color = arguments.value(QStringLiteral("color"));
            if (!title.isString() || title.toString().isEmpty() || title.toString().size() > 160 || title.toString().contains(QChar::Null) ||
                !color.isString() || !QRegularExpression(QStringLiteral("^#[0-9A-Fa-f]{6}$")).match(color.toString()).hasMatch())
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use nonempty name up to 160 characters and #RRGGBB color."));
            categories[category] = {QColor(color.toString()), title.toString()};
        }
        QStringList next;
        for (auto i = categories.begin(); i != categories.end(); ++i)
            next.append(QStringLiteral("%1:%2:%3").arg(i.value().displayName, QString::number(i.key()), i.value().color.name()));
        if (!document->updateGuideCategories(next, remap))
            return reject(
                QStringLiteral("EDIT_FAILED"),
                QStringLiteral(
                    "Native category transaction was rejected without changes. Check loading, pending legacy guides, category mappings, or busy models."));
        return finish({{QStringLiteral("categories"), QJsonDocument::fromJson(model->categoriesToJSon().toUtf8()).array()},
                       {QStringLiteral("affectedMarkerCount"), affectedMarkers},
                       {QStringLiteral("markerPolicy"), policy},
                       {QStringLiteral("undoable"), true},
                       {QStringLiteral("historyReset"), false}});
    }
    if (name == QLatin1String("list_sequences")) {
        QJsonArray sequences;
        const auto all = Mcp::liveSequenceClips(pCore->projectItemModel());
        for (auto it = all.begin(); it != all.end(); ++it) {
            const auto clip = pCore->projectItemModel()->getClipByBinID(it.value());
            const auto model = pCore->currentDoc()->getTimeline(it.key(), true);
            QJsonObject item{{QStringLiteral("sequenceId"), it.key().toString(QUuid::WithoutBraces)},
                             {QStringLiteral("binId"), it.value()},
                             {QStringLiteral("name"), clip ? clip->clipName() : QString()},
                             {QStringLiteral("active"), it.key() == timeline->uuid()},
                             {QStringLiteral("loaded"), bool(model)},
                             {QStringLiteral("ready"), clip && clip->statusReady()}};
            if (model) {
                item.insert(QStringLiteral("durationFrames"), model->duration());
                item.insert(QStringLiteral("trackCount"), model->getTracksCount());
            }
            sequences.append(item);
        }
        return Mcp::toolResult({{QStringLiteral("ok"), true}, {QStringLiteral("sequences"), sequences}, {QStringLiteral("state"), state}});
    }
    if (name == QLatin1String("delete_sequence")) {
        if (!arguments.value(QStringLiteral("acknowledgeHistoryReset")).isBool() || !arguments.value(QStringLiteral("acknowledgeHistoryReset")).toBool())
            return reject(QStringLiteral("ACKNOWLEDGEMENT_REQUIRED"),
                          QStringLiteral("Sequence deletion permanently clears ALL project undo history. Explicitly acknowledgeHistoryReset=true to proceed."));
        const auto supplied = arguments.value(QStringLiteral("sequenceId"));
        const QUuid uuid(supplied.toString());
        const auto all = Mcp::liveSequenceClips(pCore->projectItemModel());
        if (!supplied.isString() || uuid.isNull() || !all.contains(uuid))
            return reject(QStringLiteral("INVALID_SEQUENCE"), QStringLiteral("Use native sequence UUID from list_sequences."));
        if (all.size() <= 1 || uuid == timeline->uuid())
            return reject(QStringLiteral("SEQUENCE_IN_USE"), QStringLiteral("Keep at least one sequence and switch away from the sequence being deleted."));
        const auto clip = pCore->projectItemModel()->getClipByBinID(all.value(uuid));
        if (!clip || !clip->statusReady()) return reject(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Sequence producer is still loading."));
        if (clip->isIncludedInTimeline())
            return reject(QStringLiteral("SEQUENCE_REFERENCED"), QStringLiteral("Remove every nested instance of this sequence before deleting it."));
        if (!pCore->window()) return reject(QStringLiteral("GUI_REQUIRED"), QStringLiteral("Native sequence deletion requires running editor GUI."));
        if (!timeline->getCurrentSelection().empty() || timeline->singleSelectionMode())
            return reject(QStringLiteral("UI_BUSY"), QStringLiteral("Clear timeline selection first."));
        auto model = pCore->currentDoc()->getTimeline(uuid, true);
        if (!model) pCore->projectManager()->openTimeline(all.value(uuid), -1, uuid, -1, false, nullptr, false);
        if (!pCore->currentDoc()->getTimeline(uuid, true))
            return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native sequence could not be loaded for deletion."));
        if (!pCore->projectManager()->closeTimeline(uuid, true, true, true))
            return Mcp::failure(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native sequence closure failed; inspect current undo history."), snapshot());
        Fun undo = [] { return true; }, redo = [] { return true; };
        if (!pCore->projectItemModel()->requestBinClipDeletion(clip, undo, redo)) {
            undo();
            return Mcp::toolResult(
                {{QStringLiteral("ok"), false},
                 {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("EDIT_FAILED")},
                                                       {QStringLiteral("message"),
                                                        QStringLiteral("Native sequence bin deletion failed after undo history reset. Inspect the project.")}}},
                 {QStringLiteral("historyReset"), true},
                 {QStringLiteral("state"), snapshot()}},
                true);
        }
        stack->clear();
        return finish({{QStringLiteral("deletedSequenceId"), uuid.toString(QUuid::WithoutBraces)},
                       {QStringLiteral("historyReset"), true},
                       {QStringLiteral("undoable"), false}});
    }
    if (name == QLatin1String("create_sequence") || name == QLatin1String("switch_sequence") || name == QLatin1String("rename_sequence")) {
        if (!pCore->window()) return reject(QStringLiteral("GUI_REQUIRED"), QStringLiteral("Native sequence management requires the running editor GUI."));
        if (!timeline->getCurrentSelection().empty() || timeline->singleSelectionMode())
            return reject(QStringLiteral("UI_BUSY"), QStringLiteral("Clear timeline selection before sequence management."));
        if (name == QLatin1String("create_sequence")) {
            int audio = 0, video = 0;
            const auto title = arguments.value(QStringLiteral("name"));
            if (!title.isString() || title.toString().isEmpty() || title.toString().size() > 160 || title.toString().contains(QChar::Null) ||
                !Mcp::integer(arguments, QStringLiteral("audioTracks"), 0, 32, audio) ||
                !Mcp::integer(arguments, QStringLiteral("videoTracks"), 0, 32, video) || audio + video < 1)
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use nonempty name, 0..32 audio/video tracks and at least one track."));
            Fun undo = [] { return true; }, redo = [] { return true; };
            const QString binId = ClipCreator::createPlaylistClipWithUndo(
                title.toString(), {audio, video}, pCore->projectItemModel()->getRootFolder()->clipId(), pCore->projectItemModel(), undo, redo);
            const auto clip = pCore->projectItemModel()->hasClip(binId) ? pCore->projectItemModel()->getClipByBinID(binId) : nullptr;
            if (binId.isEmpty() || !clip) return rollback(undo, QStringLiteral("Native sequence creation failed."));
            const QUuid uuid = clip->getSequenceUuid();
            return commit(undo, redo, QStringLiteral("MCP create sequence"),
                          {{QStringLiteral("binId"), binId}, {QStringLiteral("sequenceId"), uuid.toString(QUuid::WithoutBraces)}});
        }
        const auto supplied = arguments.value(QStringLiteral("sequenceId"));
        const QUuid uuid(supplied.toString());
        const auto all = Mcp::liveSequenceClips(pCore->projectItemModel());
        if (!supplied.isString() || uuid.isNull() || !all.contains(uuid))
            return reject(QStringLiteral("INVALID_SEQUENCE"), QStringLiteral("Use a native sequence UUID from list_sequences."));
        const auto clip = pCore->projectItemModel()->getClipByBinID(all.value(uuid));
        if (!clip || !clip->statusReady()) return reject(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Sequence producer is still loading."));
        if (name == QLatin1String("switch_sequence")) {
            const auto model = pCore->currentDoc()->getTimeline(uuid, true);
            if (!model || model->isClosed) pCore->projectManager()->openTimeline(all.value(uuid), -1, uuid);
            if (!pCore->currentDoc()->getTimeline(uuid, true)) return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native sequence opening failed."));
            pCore->projectManager()->activateDocument(uuid);
            return finish({{QStringLiteral("sequenceId"), uuid.toString(QUuid::WithoutBraces)}});
        }
        const auto title = arguments.value(QStringLiteral("name"));
        if (!title.isString() || title.toString().isEmpty() || title.toString().size() > 160 || title.toString().contains(QChar::Null))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use nonempty sequence name up to 160 characters."));
        if (clip->clipName() != title.toString() && !clip->rename(title.toString(), 0))
            return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native sequence rename failed."));
        return finish({{QStringLiteral("sequenceId"), uuid.toString(QUuid::WithoutBraces)}, {QStringLiteral("name"), clip->clipName()}});
    }
    if (name == QLatin1String("set_selection")) {
        std::unordered_set<int> selected;
        if (!ids(arguments.value(QStringLiteral("itemIds")), selected, MaxBatchItems, true))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use unique native IDs, or an empty array to clear selection."));
        for (int id : selected)
            if (!timeline->isItem(id)) return reject(QStringLiteral("INVALID_ITEM"), QStringLiteral("Unknown selected item."));
        const bool success = selected.empty() ? timeline->requestClearSelection() : timeline->requestSetSelection(selected);
        if (!success) return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native selection change failed."));
        return finish({{QStringLiteral("selection"), sortedIds(timeline->getCurrentSelection())}});
    }
    if (timeline->editMode() != TimelineMode::NormalEdit || timeline->singleSelectionMode() || !timeline->getCurrentSelection().empty())
        return reject(QStringLiteral("UI_BUSY"), QStringLiteral("Use normal mode and clear timeline selection with set_selection before editing."));

    if (name == QLatin1String("add_track")) {
        int position = -1;
        bool audio = false;
        const QString trackName = arguments.value(QStringLiteral("name")).toString();
        if (!Mcp::integer(arguments, QStringLiteral("position"), -1, timeline->getTracksCount(), position) ||
            !boolean(arguments, QStringLiteral("audio"), false, audio) ||
            (arguments.contains(QStringLiteral("name")) && !arguments.value(QStringLiteral("name")).isString()) || trackName.size() > 160 ||
            trackName.contains(QChar::Null))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use valid position, boolean audio flag and name up to 160 characters."));
        int track = -1;
        if (!timeline->requestTrackInsertion(position, track, trackName, audio))
            return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native track creation failed."));
        return finish({{QStringLiteral("trackId"), track}, {QStringLiteral("position"), timeline->getTrackPosition(track)}});
    }
    if (name == QLatin1String("delete_track") || name == QLatin1String("move_track") || name == QLatin1String("rename_track") ||
        name == QLatin1String("set_track_locked") || name == QLatin1String("set_track_muted")) {
        int track = -1;
        if (!Mcp::integer(arguments, QStringLiteral("trackId"), 0, MaxFrame, track) || !timeline->isTrack(track))
            return reject(QStringLiteral("INVALID_TRACK"), QStringLiteral("Unknown native track ID."));
        if (name != QLatin1String("set_track_locked") && timeline->trackIsLocked(track))
            return reject(QStringLiteral("INVALID_TRACK"), QStringLiteral("Unlock target track first."));
        if (name == QLatin1String("delete_track")) {
            bool contents = false;
            if (!boolean(arguments, QStringLiteral("deleteContents"), false, contents))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("deleteContents must be boolean."));
            if (timeline->getTracksCount() <= 1) return reject(QStringLiteral("LAST_TRACK"), QStringLiteral("Cannot delete final native track."));
            const auto affected = timeline->getItemsInRange(track, 0, -1, true);
            if (!affected.empty() && !contents)
                return reject(QStringLiteral("TRACK_NOT_EMPTY"), QStringLiteral("Set deleteContents=true to remove track material."));
            for (int id : affected)
                if (!unlockedItems(timeline, timeline->getGroupElements(id)))
                    return reject(QStringLiteral("LOCKED_GROUP"), QStringLiteral("A grouped item on another track is locked."));
            if (!timeline->requestTrackDeletion(track)) return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native track deletion failed."));
            return finish({{QStringLiteral("deletedTrackId"), track}});
        }
        if (name == QLatin1String("move_track")) {
            const QString direction = arguments.value(QStringLiteral("direction")).toString();
            if (direction != QLatin1String("up") && direction != QLatin1String("down"))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use direction up or down."));
            const int neighborPosition = timeline->getTrackPosition(track) + (direction == QLatin1String("up") ? 1 : -1);
            if (neighborPosition < 0 || neighborPosition >= timeline->getTracksCount())
                return reject(QStringLiteral("TRACK_BOUNDARY"), QStringLiteral("Track already at boundary."));
            const int neighbor = timeline->getTrackIndexFromPosition(neighborPosition);
            if (timeline->trackIsLocked(neighbor) || timeline->isAudioTrack(track) != timeline->isAudioTrack(neighbor))
                return reject(QStringLiteral("INVALID_TRACK"), QStringLiteral("Adjacent track must be unlocked and of same type."));
            if (!timeline->requestTrackMove(timeline, track, direction == QLatin1String("up")))
                return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native track move failed."));
            return finish({{QStringLiteral("trackId"), track}, {QStringLiteral("position"), timeline->getTrackPosition(track)}});
        }
        if (name == QLatin1String("rename_track")) {
            const auto value = arguments.value(QStringLiteral("name"));
            if (!value.isString() || value.toString().size() > 160 || value.toString().contains(QChar::Null))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use name up to 160 characters."));
            timeline->setTrackName(track, value.toString());
            return finish({{QStringLiteral("trackId"), track},
                           {QStringLiteral("name"), timeline->getTrackProperty(track, QStringLiteral("kdenlive:track_name")).toString()}});
        }
        if (name == QLatin1String("set_track_locked")) {
            bool locked = false;
            if (!boolean(arguments, QStringLiteral("locked"), false, locked))
                return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("locked must be boolean."));
            if (timeline->trackIsLocked(track) != locked) timeline->setTrackLockedState(track, locked);
            return finish({{QStringLiteral("trackId"), track}, {QStringLiteral("locked"), timeline->trackIsLocked(track)}});
        }
        bool muted = false;
        if (!boolean(arguments, QStringLiteral("muted"), false, muted))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("muted must be boolean."));
        const QString previous = timeline->getTrackProperty(track, QStringLiteral("hide")).toString();
        const auto nativeTrack = timeline->getTrackById(track);
        auto *service = nativeTrack->getTrackService();
        const bool previousPresent = service->property_exists("hide");
        std::vector<std::pair<bool, QString>> previousPlaylistHides;
        for (int row = 0; row < service->count(); ++row) {
            std::unique_ptr<Mlt::Producer> playlist(service->track(row));
            if (!playlist || !playlist->is_valid()) return reject(QStringLiteral("NOT_READY"), QStringLiteral("The native track playlists are unavailable."));
            previousPlaylistHides.emplace_back(playlist->property_exists("hide"), QString::fromUtf8(playlist->get("hide")));
        }
        const QString requested = muted ? QStringLiteral("3") : timeline->isAudioTrack(track) ? QStringLiteral("1") : QStringLiteral("2");
        if (previous != requested) {
            Fun undo = [timeline, track, nativeTrack, previous, previousPresent, previousPlaylistHides] {
                auto *service = nativeTrack->getTrackService();
                if (service->count() != static_cast<int>(previousPlaylistHides.size())) return false;
                timeline->setTrackProperty(track, QStringLiteral("hide"), previous);
                if (!previousPresent) static_cast<Mlt::Properties *>(service)->clear("hide");
                // A newly created track can have outer hide=0 and inner hide=2
                // (video) or 1 (audio). Restore every original stream mask.
                for (int row = 0; row < service->count(); ++row) {
                    std::unique_ptr<Mlt::Producer> playlist(service->track(row));
                    const auto &saved = previousPlaylistHides.at(row);
                    if (saved.first)
                        playlist->set("hide", saved.second.toUtf8().constData());
                    else
                        static_cast<Mlt::Properties *>(playlist.get())->clear("hide");
                }
                timeline->updateDuration();
                return true;
            };
            Fun redo = [timeline, track, requested] {
                timeline->setTrackProperty(track, QStringLiteral("hide"), requested);
                timeline->updateDuration();
                return true;
            };
            redo();
            pCore->pushUndo(undo, redo, QStringLiteral("MCP set track output"));
        }
        return finish({{QStringLiteral("trackId"), track}, {QStringLiteral("muted"), timeline->getTrackProperty(track, QStringLiteral("hide")).toInt() == 3}});
    }
    if (name == QLatin1String("group_items") || name == QLatin1String("set_clips_enabled")) {
        std::unordered_set<int> targets;
        const bool grouping = name == QLatin1String("group_items");
        if (!ids(arguments.value(grouping ? QStringLiteral("itemIds") : QStringLiteral("clipIds")), targets, MaxBatchItems) || (grouping && targets.size() < 2))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use unique IDs; grouping requires at least two."));
        std::unordered_set<int> all = targets;
        for (int id : targets) {
            if (!timeline->isClip(id) && (!grouping || !timeline->isComposition(id)))
                return reject(QStringLiteral("INVALID_ITEM"), QStringLiteral("Unknown/incompatible item."));
            const auto siblings = timeline->getGroupElements(id);
            all.insert(siblings.begin(), siblings.end());
        }
        if (!unlockedItems(timeline, all)) return reject(QStringLiteral("LOCKED_GROUP"), QStringLiteral("Every affected group item must be unlocked."));
        if (grouping) {
            const int groupId = timeline->requestClipsGroup(targets);
            if (groupId < 0) return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native grouping failed."));
            return finish({{QStringLiteral("groupId"), groupId}, {QStringLiteral("items"), itemsReadback(timeline, all)}});
        }
        bool enabled = false;
        if (!boolean(arguments, QStringLiteral("enabled"), false, enabled))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("enabled must be boolean."));
        if (!TimelineFunctions::setClipsEnabled(timeline, targets, enabled))
            return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native enable/disable failed."));
        return finish({{QStringLiteral("items"), itemsReadback(timeline, targets)}, {QStringLiteral("enabled"), enabled}});
    }
    if (name == QLatin1String("edit_timeline_zone") || name == QLatin1String("remove_gap") || name == QLatin1String("insert_zone")) {
        const bool insertion = name == QLatin1String("insert_zone"), gap = name == QLatin1String("remove_gap");
        int start = -1, duration = -1;
        if (!Mcp::integer(arguments, QStringLiteral("startFrame"), 0, MaxFrame, start) ||
            !Mcp::integer(arguments, QStringLiteral("durationFrames"), 1, MaxDuration, duration) || qint64(start) + duration > MaxFrame)
            return reject(QStringLiteral("INVALID_FRAME"), QStringLiteral("Use bounded positive half-open range."));
        std::unordered_set<int> trackSet;
        if (insertion) {
            int track = -1;
            if (!Mcp::integer(arguments, QStringLiteral("trackId"), 0, MaxFrame, track))
                return reject(QStringLiteral("INVALID_TRACK"), QStringLiteral("Use explicit track ID."));
            trackSet.insert(track);
        } else if (!ids(arguments.value(QStringLiteral("trackIds")), trackSet, 64))
            return reject(QStringLiteral("INVALID_TRACK"), QStringLiteral("Use one or more unique track IDs."));
        QVector<int> tracks;
        for (int track : trackSet) {
            if (!timeline->isTrack(track) || timeline->trackIsLocked(track))
                return reject(QStringLiteral("INVALID_TRACK"), QStringLiteral("All target tracks must exist and be unlocked."));
            tracks.push_back(track);
        }
        const QString action = gap         ? QStringLiteral("remove_gap")
                               : insertion ? arguments.value(QStringLiteral("mode")).toString()
                                           : arguments.value(QStringLiteral("action")).toString();
        if (insertion ? action != QLatin1String("insert") && action != QLatin1String("overwrite")
                      : !gap && action != QLatin1String("lift") && action != QLatin1String("extract") && action != QLatin1String("insert_space"))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Unknown zone operation."));
        const bool shifting = gap || action == QLatin1String("extract") || action == QLatin1String("insert_space") || action == QLatin1String("insert");
        const auto allOnTracks = trackItems(timeline, tracks);
        for (int id : allOnTracks) {
            if (timeline->getItemPosition(id) + timeline->getItemPlaytime(id) <= start) continue;
            const auto siblings = timeline->getGroupElements(id);
            if (!unlockedItems(timeline, siblings)) return reject(QStringLiteral("LOCKED_GROUP"), QStringLiteral("Affected group contains locked material."));
            for (int sibling : siblings)
                if (timeline->isSubTitle(sibling) || !trackSet.contains(timeline->getItemTrackId(sibling)))
                    return reject(QStringLiteral("GROUP_SCOPE"), QStringLiteral("Include every track of affected group or ungroup first."));
            if (insertion && timeline->isInGroup(id))
                return reject(QStringLiteral("GROUP_SCOPE"), QStringLiteral("Ungroup material affected by insert/overwrite first."));
            if (gap && timeline->getItemPosition(id) < start + duration && timeline->getItemPosition(id) + timeline->getItemPlaytime(id) > start)
                return reject(QStringLiteral("NOT_A_GAP"), QStringLiteral("Every requested track must be empty throughout gap."));
            if (shifting && (action == QLatin1String("insert_space") || action == QLatin1String("insert")) &&
                qint64(timeline->getItemPosition(id)) + timeline->getItemPlaytime(id) + duration > MaxFrame)
                return reject(QStringLiteral("LIMIT"), QStringLiteral("Shifted material would exceed frame limit."));
        }
        QString binId, stream;
        int sourceIn = 0;
        std::shared_ptr<ProjectClip> source;
        if (insertion) {
            const auto bid = arguments.value(QStringLiteral("binId"));
            binId = bid.toString();
            if (!bid.isString() || !QRegularExpression(QStringLiteral("^[0-9]{1,12}$")).match(binId).hasMatch() || !pCore->projectItemModel()->hasClip(binId))
                return reject(QStringLiteral("INVALID_BIN_CLIP"), QStringLiteral("Unknown bin clip ID."));
            source = pCore->projectItemModel()->getClipByBinID(binId);
            stream = arguments.value(QStringLiteral("stream")).toString();
            const bool audio = stream == QLatin1String("audio");
            if (!source || !source->statusReady()) return reject(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Wait for native bin clip loading."));
            if ((stream != QLatin1String("video") && !audio) || timeline->isAudioTrack(tracks.first()) != audio ||
                (audio ? !source->hasAudio() : !source->hasVideo()))
                return reject(QStringLiteral("STREAM_MISMATCH"), QStringLiteral("Source stream and destination track must match."));
            if (audio && (source->audioStreamsCount() > 1 || source->activeStreams().size() > 1))
                return reject(QStringLiteral("MEDIA_UNSUPPORTED"),
                              QStringLiteral("Use insert_media_streams with explicit stream IDs and insert/overwrite placement for multistream audio."));
            if (!Mcp::integer(arguments, QStringLiteral("sourceInFrame"), 0, MaxFrame, sourceIn) || qint64(sourceIn) + duration > source->getFramePlaytime() ||
                (source->clipType() == ClipType::Image && sourceIn != 0))
                return reject(QStringLiteral("INVALID_SOURCE_RANGE"), QStringLiteral("Exact source range must fit ready producer; images start at zero."));
        }
        Fun undo = [] { return true; }, redo = [] { return true; };
        auto step = [&](const std::function<bool(Fun &, Fun &)> &operation, bool failureAlreadyUndone) {
            Fun localUndo = [] { return true; }, localRedo = [] { return true; };
            const bool success = operation(localUndo, localRedo);
            if (success)
                appendStep(undo, redo, localUndo, localRedo);
            else if (!failureAlreadyUndone)
                localUndo();
            return success;
        };
        if (action == QLatin1String("insert") || action == QLatin1String("insert_space")) {
            for (int track : tracks) {
                const int crossing = timeline->getClipByPosition(track, start);
                if (crossing >= 0 && timeline->getClipPosition(crossing) < start &&
                    !step([&](Fun &u, Fun &r) { return TimelineFunctions::requestClipCut(timeline, crossing, start, u, r); }, true))
                    return rollback(undo, QStringLiteral("Native boundary split failed."));
            }
            if (!step([&](Fun &u, Fun &r) { return TimelineFunctions::requestInsertSpace(timeline, QPoint(start, start + duration), u, r, tracks); }, true))
                return rollback(undo, QStringLiteral("Native space insertion failed."));
        } else if (!gap) {
            for (int track : tracks)
                if (!step([&](Fun &u, Fun &r) { return TimelineFunctions::liftZone(timeline, track, QPoint(start, start + duration), u, r); }, false))
                    return rollback(undo, QStringLiteral("Native zone lift failed."));
        }
        if (gap || action == QLatin1String("extract")) {
            if (!step([&](Fun &u, Fun &r) { return TimelineFunctions::removeSpace(timeline, QPoint(start, start + duration), u, r, tracks, false); }, true))
                return rollback(undo, QStringLiteral("Native zone extraction failed."));
        }
        QJsonObject result{{QStringLiteral("action"), action},
                           {QStringLiteral("trackIds"), sortedIds(trackSet)},
                           {QStringLiteral("startFrame"), start},
                           {QStringLiteral("durationFrames"), duration}};
        if (insertion) {
            int created = -1;
            const QString prefixed = (stream == QLatin1String("audio") ? QStringLiteral("A") : QStringLiteral("V")) + binId + QLatin1Char('/') +
                                     QString::number(sourceIn) + QLatin1Char('/') + QString::number(sourceIn + duration - 1);
            if (!step([&](Fun &u,
                          Fun &r) { return timeline->requestClipInsertion(prefixed, tracks.first(), start, created, true, true, false, u, r, tracks, 1); },
                      false))
                return rollback(undo, QStringLiteral("Native source insertion failed."));
            if (!timeline->isClip(created) || timeline->getClipPosition(created) != start || timeline->getClipPlaytime(created) != duration ||
                timeline->getClipInOut(created) != std::make_pair(sourceIn, sourceIn + duration - 1))
                return rollback(undo, QStringLiteral("Native insertion did not preserve source range."));
            result.insert(QStringLiteral("clipId"), created);
            result.insert(QStringLiteral("item"), itemReadback(timeline, created));
        }
        timeline->requestClearSelection();
        return commit(undo, redo, QStringLiteral("MCP timeline zone edit"), result);
    }

    int target = -1;
    const QString targetKey =
        name == QLatin1String("delete_item") || name == QLatin1String("ungroup_items") ? QStringLiteral("itemId") : QStringLiteral("clipId");
    if (!Mcp::integer(arguments, targetKey, 0, MaxFrame, target) || !timeline->isItem(target) ||
        (targetKey == QLatin1String("clipId") && !timeline->isClip(target)))
        return reject(QStringLiteral("INVALID_CLIP"), QStringLiteral("Unknown timeline item."));
    bool includeGroup = false;
    if (!boolean(arguments, QStringLiteral("includeGroup"), false, includeGroup))
        return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("includeGroup must be boolean."));
    auto affected = timeline->getGroupElements(target);
    if (!unlockedItems(timeline, affected)) return reject(QStringLiteral("LOCKED_GROUP"), QStringLiteral("Every affected item must be unlocked."));
    if (name == QLatin1String("ungroup_items")) {
        if (!timeline->isInGroup(target)) return reject(QStringLiteral("NOT_GROUPED"), QStringLiteral("Target is not grouped."));
        if (!timeline->requestClipUngroup(target)) return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native ungrouping failed."));
        return finish({{QStringLiteral("items"), itemsReadback(timeline, affected)}});
    }
    if (timeline->isInGroup(target) && !includeGroup) return reject(QStringLiteral("GROUP_SCOPE"), QStringLiteral("Set includeGroup=true or ungroup first."));
    if (name == QLatin1String("delete_item")) {
        Fun undo = [] { return true; }, redo = [] { return true; };
        if (!timeline->requestItemDeletion(target, undo, redo, true)) return rollback(undo, QStringLiteral("Native deletion failed."));
        for (int id : affected)
            if (timeline->isItem(id)) return rollback(undo, QStringLiteral("Native deletion left an item present."));
        return commit(undo, redo, QStringLiteral("MCP delete item"), {{QStringLiteral("deletedItemIds"), sortedIds(affected)}});
    }
    if (name == QLatin1String("move_clip") || name == QLatin1String("duplicate_clip")) {
        int track = -1, start = -1;
        if (!Mcp::integer(arguments, QStringLiteral("trackId"), 0, MaxFrame, track) || !timeline->isTrack(track) || timeline->trackIsLocked(track) ||
            timeline->isAudioTrack(track) != timeline->isAudioTrack(timeline->getClipTrackId(target)))
            return reject(QStringLiteral("INVALID_TRACK"), QStringLiteral("Use unlocked destination track of source type."));
        const int duration = timeline->getClipPlaytime(target);
        if (!Mcp::integer(arguments, QStringLiteral("startFrame"), 0, MaxFrame, start) || qint64(start) + duration > MaxFrame)
            return reject(QStringLiteral("INVALID_FRAME"), QStringLiteral("Use bounded timeline position."));
        if (!includeGroup) {
            for (int other : timeline->getItemsInRange(track, 0, -1, true)) {
                if ((name == QLatin1String("move_clip") && other == target) || !timeline->isClip(other)) continue;
                const qint64 otherStart = timeline->getItemPosition(other), otherEnd = otherStart + timeline->getItemPlaytime(other);
                if (start < otherEnd && qint64(start) + duration > otherStart)
                    return reject(QStringLiteral("COLLISION"), QStringLiteral("Destination overlaps another clip."));
            }
        }
        if (name == QLatin1String("move_clip")) {
            if (!timeline->requestClipMove(target, track, start, includeGroup, true, true, true))
                return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native move failed; inspect destinations and group tracks."));
            return finish({{QStringLiteral("clipId"), target}, {QStringLiteral("items"), itemsReadback(timeline, affected)}});
        }
        bool includeMixPartners = false;
        if (!boolean(arguments, QStringLiteral("includeMixPartners"), false, includeMixPartners))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("includeMixPartners must be boolean."));
        const auto originalScope = affected;
        // Native mixes extend both clips' ranges. Copying one participant alone
        // would retain hidden handles as ordinary footage, so never drop a mix.
        bool expanded = true;
        while (expanded) {
            expanded = false;
            const auto current = affected;
            for (int id : current) {
                if (!timeline->isClip(id)) continue;
                const auto sourceTrack = timeline->getTrackById_const(timeline->getClipTrackId(id));
                const auto mixes = sourceTrack->getMixInfo(id);
                for (int partner : {mixes.first.firstClipId, mixes.second.secondClipId}) {
                    if (partner < 0 || affected.count(partner)) continue;
                    if (!includeMixPartners)
                        return reject(QStringLiteral("MIX_SCOPE"), QStringLiteral("Set includeMixPartners=true to duplicate every connected mix participant."));
                    if (timeline->isInGroup(partner) && !includeGroup)
                        return reject(QStringLiteral("GROUP_SCOPE"),
                                      QStringLiteral("A mix partner is grouped; set includeGroup=true to include its complete group."));
                    const auto partners = timeline->getGroupElements(partner);
                    affected.insert(partners.begin(), partners.end());
                    if (affected.size() > MaxBatchItems) return reject(QStringLiteral("LIMIT"), QStringLiteral("Expanded mix/group scope exceeds item limit."));
                    expanded = true;
                }
            }
        }
        if (!unlockedItems(timeline, affected))
            return reject(QStringLiteral("LOCKED_GROUP"), QStringLiteral("Every included mix participant and group member must be unlocked."));
        const auto subtitles = timeline->getSubtitleModel();
        if (affected.size() > MaxBatchItems)
            return reject(QStringLiteral("LIMIT"), QStringLiteral("Duplicate at most8192 group/mix members in one explicit transaction."));
        const int deltaTrack = timeline->getTrackPosition(track) - timeline->getTrackPosition(timeline->getClipTrackId(target));
        const int deltaFrame = start - timeline->getClipPosition(target);
        const double fps = pCore->getCurrentFps();
        struct SubtitleCopy
        {
            int layer;
            GenTime start;
            SubtitleEvent event;
        };
        struct MixCopy
        {
            MixInfo info;
            int destination;
            std::pair<QString, QVector<QPair<QString, QVariant>>> parameters;
            std::pair<int, int> tracks;
            QMap<QString, QString> properties;
        };
        std::unordered_map<int, int> destinations;
        std::unordered_map<int, int> compositionTargets;
        std::unordered_map<int, SubtitleCopy> subtitleCopies;
        std::vector<MixCopy> mixes;
        std::unordered_set<int> groupRoots;
        for (const auto &entry : sortedIds(affected)) {
            const int id = entry.toInt();
            groupRoots.insert(timeline->m_groups->getRootId(id));
            const qint64 frame = qint64(timeline->getItemPosition(id)) + deltaFrame;
            if (!timeline->isSubTitle(id) && (frame < 0 || frame + timeline->getItemPlaytime(id) > MaxFrame))
                return reject(QStringLiteral("INVALID_FRAME"), QStringLiteral("Duplicated group would leave frame bounds."));
            if (timeline->isSubTitle(id)) {
                if (!subtitles || subtitles->isLocked())
                    return reject(QStringLiteral("LOCKED_GROUP"), QStringLiteral("The subtitle model must be available and unlocked."));
                const int layer = timeline->getSubtitleLayer(id);
                const GenTime offset(deltaFrame, fps);
                const GenTime subtitleStart = subtitles->getSubtitlePosition(id) + offset;
                auto event = subtitles->getSubtitle(layer, subtitles->getSubtitlePosition(id));
                event.setEndTime(event.endTime() + offset);
                if (subtitleStart < GenTime() || event.endTime() > GenTime(MaxFrame, fps))
                    return reject(QStringLiteral("INVALID_FRAME"), QStringLiteral("Duplicated subtitle would leave frame bounds."));
                // Keep imported subframe times exact. Native ASS permits overlap
                // but not identical starts in one layer; A/V track shifts do not
                // alter the independent subtitle layer plane.
                for (int other : subtitles->getAllSubIds())
                    if (timeline->getSubtitleLayer(other) == layer && subtitles->getSubtitlePosition(other) == subtitleStart)
                        return reject(QStringLiteral("COLLISION"), QStringLiteral("A subtitle already starts at the destination in the same layer."));
                subtitleCopies.emplace(id, SubtitleCopy{layer, subtitleStart, event});
                continue;
            }
            if (!timeline->isClip(id) && !timeline->isComposition(id))
                return reject(QStringLiteral("INVALID_ITEM"), QStringLiteral("Unknown group member type."));
            const int position = timeline->getTrackPosition(timeline->getItemTrackId(id)) + deltaTrack;
            if (position < 0 || position >= timeline->getTracksCount())
                return reject(QStringLiteral("INVALID_TRACK"), QStringLiteral("Duplicated group would leave available tracks."));
            const int destination = timeline->getTrackIndexFromPosition(position);
            if (timeline->trackIsLocked(destination) || timeline->isAudioTrack(destination) != timeline->isAudioTrack(timeline->getItemTrackId(id)))
                return reject(QStringLiteral("INVALID_TRACK"), QStringLiteral("Every duplicate destination must be unlocked and of the source track type."));
            for (int other : timeline->getItemsInRange(destination, 0, -1, true)) {
                if (timeline->isClip(id) != timeline->isClip(other)) continue;
                const qint64 otherStart = timeline->getItemPosition(other), otherEnd = otherStart + timeline->getItemPlaytime(other);
                if (frame < otherEnd && frame + timeline->getItemPlaytime(id) > otherStart)
                    return reject(QStringLiteral("COLLISION"), QStringLiteral("A duplicated group destination overlaps existing material."));
            }
            if (timeline->isComposition(id)) {
                const auto composition = timeline->getCompositionPtr(id);
                const int forced = composition->getForcedTrack();
                // Automatic keeps following the previous VIDEO track. A forced
                // black background stays black. Explicit real-track targets
                // follow the same relative track translation as the group.
                int lower = forced < 0 ? timeline->getPreviousVideoTrackPos(destination) : forced;
                if (forced > 0) {
                    lower += deltaTrack;
                    if (lower < 1 || lower > timeline->getTracksCount() || lower >= timeline->getTrackMltIndex(destination) ||
                        timeline->isAudioTrack(timeline->getTrackIndexFromPosition(lower - 1)))
                        return reject(QStringLiteral("INVALID_TRACK"),
                                      QStringLiteral("A duplicated composition's explicit source track would leave the lower video-track range."));
                }
                compositionTargets[id] = lower;
            } else if (timeline->getMixDuration(id) > 0) {
                const auto sourceTrack = timeline->getTrackById_const(timeline->getClipTrackId(id));
                const MixInfo info = sourceTrack->getMixInfo(id).first;
                if (info.firstClipId < 0 || info.secondClipId != id || !affected.count(info.firstClipId))
                    return reject(QStringLiteral("MIX_CONFLICT"), QStringLiteral("Native mix membership is incomplete or inconsistent."));
                MixCopy copy{info, destination, sourceTrack->getMixParams(id), sourceTrack->getMixTracks(id), {}};
                const auto asset = sourceTrack->mixModel(id)->getAsset();
                // Preserve native transition settings beyond exposed parameters
                // (e.g. audio start=-1 / accepts_blanks). Runtime identities and
                // translated structural fields belong to the new native service.
                for (int i = 0; i < asset->count(); ++i) {
                    const QString key = QString::fromUtf8(asset->get_name(i));
                    if (key.isEmpty() || key.startsWith(QLatin1Char('_')) || key == QLatin1String("in") || key == QLatin1String("out") ||
                        key == QLatin1String("a_track") || key == QLatin1String("b_track") || key == QLatin1String("kdenlive:mixcut") ||
                        key == QLatin1String("mlt_service") || key == QLatin1String("mlt_type") || key == QLatin1String("id"))
                        continue;
                    if (const char *value = asset->get(key.toUtf8().constData())) copy.properties.insert(key, QString::fromUtf8(value));
                }
                mixes.push_back(std::move(copy));
            }
            destinations[id] = destination;
        }
        Fun undo = [] { return true; }, redo = [] { return true; };
        std::unordered_map<int, int> mapping;
        std::unordered_set<int> createdItems;
        QJsonArray itemMapping;
        for (const auto &value : sortedIds(affected)) {
            const int id = value.toInt();
            int created = -1;
            if (timeline->isClip(id)) {
                if (!TimelineFunctions::cloneClip(timeline, id, created, timeline->getClipState(id).first, -1, undo, redo))
                    return rollback(undo, QStringLiteral("Native clip clone failed."));
                const auto source = timeline->getClipPtr(id);
                const auto copy = timeline->getClipPtr(created);
                if (source->hasTimeRemap()) {
                    const auto nativeMap = [timeline](const std::shared_ptr<ClipModel> &clip) {
                        QMap<QString, QString> values;
                        const auto producer = timeline->getClipProducer(clip->getId());
                        Mlt::Chain chain(producer->parent());
                        for (int row = 0; row < chain.link_count(); ++row) {
                            std::unique_ptr<Mlt::Link> link(chain.link(row));
                            if (!link || QString::fromUtf8(link->get("mlt_service")) != QLatin1String("timeremap")) continue;
                            const char *map = link->property_exists("time_map") ? link->get("time_map") : link->get("map");
                            values.insert(QStringLiteral("time_map"), QString::fromUtf8(map));
                            values.insert(QStringLiteral("pitch"), QString::fromUtf8(link->get("pitch")));
                            values.insert(QStringLiteral("image_mode"), QString::fromUtf8(link->get("image_mode")));
                            break;
                        }
                        return values;
                    };
                    if (nativeMap(source) != nativeMap(copy))
                        return rollback(undo, QStringLiteral("Native duplicate did not preserve the complete time-map configuration."));
                }
                // Native paste uses finalMove on an unplanted clip to preserve
                // alternating playlists. Mix registration follows all insertions.
                copy->setSubPlaylistIndex(source->getSubPlaylistIndex(), -1);
                if (timeline->requestClipMove(created, destinations.at(id), timeline->getClipPosition(id) + deltaFrame, false, true, true, true, undo, redo) !=
                        TimelineModel::MoveSuccess ||
                    !timeline->isClip(created) || timeline->getClipInOut(created) != timeline->getClipInOut(id) ||
                    timeline->getClipPlaytime(created) != timeline->getClipPlaytime(id) || source->hasTimeRemap() != copy->hasTimeRemap())
                    return rollback(undo, QStringLiteral("Native duplicate clip did not preserve the source."));
            } else if (timeline->isComposition(id)) {
                const auto composition = timeline->getCompositionPtr(id);
                std::unique_ptr<Mlt::Properties> properties(composition->properties());
                const int lower = compositionTargets.at(id);
                if (!timeline->requestCompositionInsertion(composition->getAssetId(), destinations.at(id), lower, timeline->getItemPosition(id) + deltaFrame,
                                                           timeline->getItemPlaytime(id), std::move(properties), created, undo, redo))
                    return rollback(undo, QStringLiteral("Native duplicate composition failed."));
                const auto copied = timeline->getCompositionPtr(created);
                const int expectedForced = composition->getForcedTrack() < 0 ? -1 : lower;
                if (copied->getATrack() != lower || copied->getForcedTrack() != expectedForced)
                    return rollback(undo, QStringLiteral("Native duplicate composition did not preserve its automatic/forced source-track binding."));
            } else {
                created = TimelineModel::getNextId();
                const auto subtitle = subtitleCopies.at(id);
                Fun subtitleUndo = [subtitles, created] { return subtitles->removeSubtitle(created); };
                Fun subtitleRedo = [subtitles, created, subtitle] {
                    if (!subtitles->addSubtitle(created, {subtitle.layer, subtitle.start}, subtitle.event)) return false;
                    // addSubtitle normalizes whitespace. The direct native edit
                    // overload restores the exact source text without an undo
                    // command; this duplicate owns one outer transaction.
                    if (subtitles->editSubtitle(created, subtitle.event.text())) return true;
                    subtitles->removeSubtitle(created);
                    return false;
                };
                if (!subtitleRedo()) return rollback(undo, QStringLiteral("Native duplicate subtitle failed."));
                appendStep(undo, redo, subtitleUndo, subtitleRedo);
                if (subtitles->getSubtitle(subtitle.layer, subtitle.start) != subtitle.event)
                    return rollback(undo, QStringLiteral("Native duplicate subtitle did not preserve the complete event."));
            }
            mapping[id] = created;
            createdItems.insert(created);
            itemMapping.append(QJsonObject{{QStringLiteral("sourceId"), id}, {QStringLiteral("createdId"), created}});
        }
        QJsonArray copiedMixes;
        for (const auto &mix : mixes) {
            MixInfo info = mix.info;
            info.firstClipId = mapping.at(info.firstClipId);
            info.secondClipId = mapping.at(info.secondClipId);
            info.firstClipInOut.first += deltaFrame;
            info.firstClipInOut.second += deltaFrame;
            info.secondClipInOut.first += deltaFrame;
            info.secondClipInOut.second += deltaFrame;
            const auto destination = timeline->getTrackById_const(mix.destination);
            const auto second = timeline->getClipPtr(info.secondClipId);
            Fun mixUndo = [destination, info] { return destination->deleteMix(info.secondClipId, true); };
            Fun mixRedo = [destination, second, info, mix] {
                if (!destination->createMix(info, mix.parameters, mix.tracks, true)) {
                    second->setMixDuration(0);
                    return false;
                }
                const auto asset = destination->mixModel(info.secondClipId)->getAsset();
                for (auto i = mix.properties.cbegin(); i != mix.properties.cend(); ++i)
                    asset->set(i.key().toUtf8().constData(), i.value().toUtf8().constData());
                return true;
            };
            if (!mixRedo()) return rollback(undo, QStringLiteral("Native duplicate mix failed."));
            appendStep(undo, redo, mixUndo, mixRedo);
            if (timeline->getMixDuration(info.secondClipId) != mix.info.firstClipInOut.second - mix.info.secondClipInOut.first ||
                timeline->getMixCutPos(info.secondClipId) != info.mixOffset || destination->getMixTracks(info.secondClipId) != mix.tracks)
                return rollback(undo, QStringLiteral("Native duplicate mix did not preserve its overlap, cut or playlist binding."));
            copiedMixes.append(QJsonObject{{QStringLiteral("firstClipId"), info.firstClipId},
                                           {QStringLiteral("secondClipId"), info.secondClipId},
                                           {QStringLiteral("trackId"), mix.destination},
                                           {QStringLiteral("transitionId"), mix.parameters.first},
                                           {QStringLiteral("durationFrames"), timeline->getMixDuration(info.secondClipId)},
                                           {QStringLiteral("cutFrames"), info.mixOffset}});
        }
        // Native copyGroups currently rebuilds every group as Normal. Retain
        // each original group type here so AV-split synchronization survives.
        std::function<int(int)> copyGroup = [&](int node) -> int {
            if (timeline->m_groups->isLeaf(node)) {
                const auto found = mapping.find(node);
                return found == mapping.end() ? -1 : found->second;
            }
            const auto type = timeline->m_groups->getType(node);
            std::unordered_set<int> children;
            for (const auto &value : sortedIds(timeline->m_groups->getDirectChildren(node))) {
                const int child = copyGroup(value.toInt());
                if (child < 0) return -1;
                children.insert(child);
            }
            // Selection is a transient UI wrapper, never a persistent group.
            if (type == GroupType::Selection) return children.empty() ? -1 : *children.begin();
            return timeline->m_groups->groupItems(children, undo, redo, type, true);
        };
        for (const auto &root : sortedIds(groupRoots))
            if (copyGroup(root.toInt()) < 0) return rollback(undo, QStringLiteral("Native duplicated group hierarchy could not be restored."));
        const int created = mapping.at(target);
        std::unordered_set<int> includedPartners;
        for (int id : affected)
            if (!originalScope.count(id)) includedPartners.insert(id);
        return commit(undo, redo, QStringLiteral("MCP duplicate clips"),
                      {{QStringLiteral("clipId"), created},
                       {QStringLiteral("item"), itemReadback(timeline, created)},
                       {QStringLiteral("items"), itemsReadback(timeline, createdItems)},
                       {QStringLiteral("itemMapping"), itemMapping},
                       {QStringLiteral("includedMixPartnerIds"), sortedIds(includedPartners)},
                       {QStringLiteral("mixes"), copiedMixes}});
    }
    if (name == QLatin1String("cut_clip")) {
        int frame = -1;
        const int start = timeline->getClipPosition(target), duration = timeline->getClipPlaytime(target), track = timeline->getClipTrackId(target);
        if (!Mcp::integer(arguments, QStringLiteral("frame"), 0, MaxFrame, frame) || frame <= start || frame >= start + duration)
            return reject(QStringLiteral("CUT_BOUNDARY"), QStringLiteral("Cut strictly inside target clip."));
        if (affected.size() > MaxBatchItems)
            return reject(QStringLiteral("LIMIT"), QStringLiteral("Cut at most8192 group members in one explicit transaction."));
        Fun undo = [] { return true; }, redo = [] { return true; };
        if (!TimelineFunctions::requestClipCut(timeline, target, frame, undo, redo))
            return Mcp::failure(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native cut failed and rolled back."), snapshot());
        const int right = timeline->getClipByPosition(track, frame);
        if (right < 0 || right == target || timeline->getClipPosition(right) != frame ||
            timeline->getClipPosition(target) + timeline->getClipPlaytime(target) != frame)
            return rollback(undo, QStringLiteral("Native split boundaries not exact."));
        return commit(undo, redo, QStringLiteral("MCP cut clip"),
                      {{QStringLiteral("clipId"), target}, {QStringLiteral("leftClipId"), target}, {QStringLiteral("rightClipId"), right}});
    }
    if (name == QLatin1String("trim_clip")) {
        int duration = -1;
        bool ripple = false;
        const QString edge = arguments.value(QStringLiteral("edge")).toString();
        if (!Mcp::integer(arguments, QStringLiteral("durationFrames"), 1, MaxDuration, duration) ||
            !boolean(arguments, QStringLiteral("ripple"), false, ripple) || (edge != QLatin1String("left") && edge != QLatin1String("right")))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use positive duration, left/right edge and boolean ripple."));
        if (qint64(timeline->getClipPosition(target)) + duration > MaxFrame || (ripple && qint64(timeline->duration()) + duration > MaxFrame))
            return reject(QStringLiteral("LIMIT"), QStringLiteral("Resize could exceed frame limit."));
        if (ripple) {
            for (int id : affected) {
                if (!timeline->isClip(id)) return reject(QStringLiteral("INVALID_ITEM"), QStringLiteral("Ripple supports clip groups."));
                for (int following : timeline->getItemsInRange(timeline->getItemTrackId(id), timeline->getItemPosition(id), -1, true))
                    if (!unlockedItems(timeline, timeline->getGroupElements(following)))
                        return reject(QStringLiteral("LOCKED_GROUP"), QStringLiteral("Ripple affects a group with locked item."));
            }
        }
        const int actual = ripple
                               ? timeline->requestItemRippleResize(timeline, target, duration, edge == QLatin1String("right"), true, false, -1, !includeGroup)
                               : timeline->requestItemResize(target, duration, edge == QLatin1String("right"), true, -1, !includeGroup);
        if (actual < 1) return reject(QStringLiteral("EDIT_FAILED"), QStringLiteral("Native resize failed."));
        return finish({{QStringLiteral("clipId"), target},
                       {QStringLiteral("requestedDurationFrames"), duration},
                       {QStringLiteral("appliedDurationFrames"), timeline->getClipPlaytime(target)},
                       {QStringLiteral("items"), itemsReadback(timeline, affected)}});
    }
    if (name == QLatin1String("slip_clip")) {
        int offset = 0;
        if (!Mcp::integer(arguments, QStringLiteral("offsetFrames"), -MaxDuration, MaxDuration, offset))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use bounded signed integral offset."));
        for (int id : affected) {
            if (!timeline->isClip(id)) return reject(QStringLiteral("INVALID_ITEM"), QStringLiteral("Slip applies to clips only."));
            const auto range = timeline->getClipInOut(id);
            const int maximum = timeline->data(timeline->makeClipIndexFromID(id), TimelineModel::MaxDurationRole).toInt();
            if (maximum < 1 || qint64(range.first) + offset < 0 || qint64(range.second) + offset >= maximum)
                return reject(QStringLiteral("INVALID_SOURCE_RANGE"), QStringLiteral("Every source range must fit finite native producer."));
        }
        Fun undo = [] { return true; }, redo = [] { return true; };
        for (int id : affected) {
            const auto before = timeline->getClipInOut(id);
            if (!timeline->requestClipSlip(id, -offset, true, undo, redo) ||
                timeline->getClipInOut(id) != std::make_pair(before.first + offset, before.second + offset))
                return rollback(undo, QStringLiteral("Native slip did not apply exact offset."));
        }
        if (offset == 0) return finish({{QStringLiteral("items"), itemsReadback(timeline, affected)}});
        return commit(undo, redo, QStringLiteral("MCP slip clips"), {{QStringLiteral("items"), itemsReadback(timeline, affected)}});
    }
    if (name == QLatin1String("set_clip_speed")) {
        double speed = 0;
        bool pitch = false, changeDuration = false;
        if (!Mcp::real(arguments, QStringLiteral("speedPercent"), -1600, 1600, speed) || std::abs(speed) < 1 ||
            !boolean(arguments, QStringLiteral("pitchCompensate"), false, pitch) ||
            !boolean(arguments, QStringLiteral("changeDuration"), false, changeDuration))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use speed magnitude 1..1600 percent and boolean pitch/duration flags."));
        for (int id : affected) {
            if (!timeline->isClip(id)) return reject(QStringLiteral("INVALID_ITEM"), QStringLiteral("Speed requires clip groups."));
            const auto type = timeline->getClipState(id).second;
            if (type != ClipType::Video && type != ClipType::Audio && type != ClipType::AV)
                return reject(QStringLiteral("MEDIA_UNSUPPORTED"), QStringLiteral("Speed requires finite video/audio source."));
        }
        Fun undo = [] { return true; }, redo = [] { return true; };
        for (int id : affected) {
            if (!timeline->requestClipTimeWarp(id, speed / 100., pitch, changeDuration, undo, redo))
                return rollback(undo, QStringLiteral("Native speed change failed."));
            if (!qFuzzyCompare(timeline->getClipSpeed(id), speed / 100.) || qint64(timeline->getClipPosition(id)) + timeline->getClipPlaytime(id) > MaxFrame)
                return rollback(undo, QStringLiteral("Native speed readback inconsistent."));
        }
        return commit(undo, redo, QStringLiteral("MCP change clip speed"), {{QStringLiteral("items"), itemsReadback(timeline, affected)}});
    }
    if (name == QLatin1String("set_clip_time_map")) {
        bool pitch = false, blending = false;
        const auto requested = arguments.value(QStringLiteral("keyframes"));
        if (!requested.isArray() || requested.toArray().size() < 2 || requested.toArray().size() > 256 ||
            !boolean(arguments, QStringLiteral("pitchCompensate"), false, pitch) || !boolean(arguments, QStringLiteral("frameBlending"), false, blending))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use 2..256 ordered mapping keys and boolean pitch/blending flags."));
        const int duration = timeline->getClipPlaytime(target);
        const double fps = pCore->getCurrentFps();
        struct MappingKey
        {
            int frame;
            int source;
            mlt_keyframe_type interpolation;
        };
        std::vector<MappingKey> keys;
        int previousFrame = -1;
        for (const auto &value : requested.toArray()) {
            if (!value.isObject()) return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Each mapping key must be an object."));
            const auto key = value.toObject();
            int frame = -1, sourceFrame = -1;
            for (auto i = key.begin(); i != key.end(); ++i)
                if (i.key() != QLatin1String("frame") && i.key() != QLatin1String("sourceFrame") && i.key() != QLatin1String("interpolation"))
                    return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Unexpected time-map key field."));
            const auto interpolationValue = key.value(QStringLiteral("interpolation"));
            const QString interpolation = key.contains(QStringLiteral("interpolation")) ? interpolationValue.toString() : QStringLiteral("linear");
            if (!Mcp::integer(key, QStringLiteral("frame"), 0, duration - 1, frame) ||
                !Mcp::integer(key, QStringLiteral("sourceFrame"), 0, MaxFrame, sourceFrame) || frame <= previousFrame ||
                (key.contains(QStringLiteral("interpolation")) && !interpolationValue.isString()) ||
                (interpolation != QLatin1String("linear") && interpolation != QLatin1String("discrete") && interpolation != QLatin1String("smooth")))
                return reject(QStringLiteral("INVALID_ARGUMENT"),
                              QStringLiteral("Use increasing output frames and bounded source frames with linear/discrete/smooth interpolation."));
            keys.push_back({frame, sourceFrame,
                            interpolation == QLatin1String("discrete") ? mlt_keyframe_discrete
                            : interpolation == QLatin1String("smooth") ? mlt_keyframe_smooth_natural
                                                                       : mlt_keyframe_linear});
            previousFrame = frame;
        }
        if (keys.front().frame != 0 || keys.back().frame != duration - 1)
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Map endpoints must cover frames zero and duration-1."));
        std::unordered_map<int, QString> maps;
        for (int id : affected) {
            if (!timeline->isClip(id) || timeline->getClipPlaytime(id) != duration)
                return reject(QStringLiteral("GROUP_SCOPE"), QStringLiteral("Time-mapped group members must all be clips of equal duration."));
            const auto type = timeline->getClipState(id).second;
            if (type != ClipType::Video && type != ClipType::AV && type != ClipType::Audio)
                return reject(QStringLiteral("MEDIA_UNSUPPORTED"), QStringLiteral("Frame mapping requires finite native audio/video media."));
            const auto source = pCore->projectItemModel()->getClipByBinID(timeline->getClipBinId(id));
            if (!source || !source->statusReady()) return reject(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Mapped source is not ready."));
            const int sourceLength = source->getFramePlaytime();
            Mlt::Properties properties;
            properties.set("_profile", pCore->getProjectProfile().get_profile(), 0);
            const int offset = timeline->getClipInOut(id).first;
            for (const auto &key : keys) {
                if (key.source >= sourceLength)
                    return reject(QStringLiteral("INVALID_SOURCE_RANGE"), QStringLiteral("Mapped source frame exceeds native source length."));
                properties.anim_set("time_map", double(key.source) / fps, offset + key.frame, 0, key.interpolation);
            }
            // Smooth interpolation may overshoot valid source boundaries.
            for (int frame = 0; frame < duration; ++frame) {
                const double sample = properties.anim_get_double("time_map", offset + frame) * fps;
                if (!std::isfinite(sample) || sample < -0.0001 || sample > sourceLength - 1 + 0.0001)
                    return reject(QStringLiteral("INVALID_SOURCE_RANGE"), QStringLiteral("Interpolated map leaves source bounds; adjust the smooth keys."));
            }
            maps[id] = QString::fromUtf8(properties.get("time_map"));
        }
        Fun undo = [] { return true; }, redo = [] { return true; };
        QJsonArray readback;
        for (int id : affected) {
            if (!timeline->data(timeline->makeClipIndexFromID(id), TimelineModel::TimeRemapRole).toBool() &&
                !timeline->requestClipTimeRemap(id, true, undo, redo))
                return rollback(undo, QStringLiteral("Native time-map enabling failed."));
            const auto clip = timeline->getClipPtr(id);
            const auto previous = clip->getRemapValues();
            const QMap<QString, QString> next{{QStringLiteral("time_map"), maps.at(id)},
                                              {QStringLiteral("pitch"), pitch ? QStringLiteral("1") : QStringLiteral("0")},
                                              {QStringLiteral("image_mode"), blending ? QStringLiteral("blend") : QStringLiteral("nearest")}};
            Fun localUndo = [clip, previous] {
                for (auto i = previous.begin(); i != previous.end(); ++i)
                    clip->setRemapValue(i.key(), i.value());
                pCore->refreshProjectMonitorOnce();
                return true;
            };
            Fun localRedo = [clip, next] {
                for (auto i = next.begin(); i != next.end(); ++i)
                    clip->setRemapValue(i.key(), i.value());
                pCore->refreshProjectMonitorOnce();
                return true;
            };
            localRedo();
            appendStep(undo, redo, localUndo, localRedo);
            const auto native = clip->getRemapValues();
            if (!native.contains(QStringLiteral("time_map"))) return rollback(undo, QStringLiteral("Native time map was not available after update."));
            Mlt::Properties verify;
            verify.set("_profile", pCore->getProjectProfile().get_profile(), 0);
            verify.set("map", native.value(QStringLiteral("time_map")).toUtf8().constData());
            QJsonArray samples;
            for (const auto &key : keys) {
                const double actual = verify.anim_get_double("map", timeline->getClipInOut(id).first + key.frame) * fps;
                if (std::abs(actual - key.source) > 0.001) return rollback(undo, QStringLiteral("Native time-map samples did not match requested keyframes."));
                samples.append(QJsonObject{{QStringLiteral("frame"), key.frame}, {QStringLiteral("sourceFrame"), actual}});
            }
            readback.append(QJsonObject{{QStringLiteral("clipId"), id},
                                        {QStringLiteral("keyframes"), samples},
                                        {QStringLiteral("nativeMap"), native.value(QStringLiteral("time_map"))},
                                        {QStringLiteral("pitchCompensate"), native.value(QStringLiteral("pitch")).toInt() != 0},
                                        {QStringLiteral("frameBlending"), native.value(QStringLiteral("image_mode")) != QLatin1String("nearest")}});
        }
        return commit(undo, redo, QStringLiteral("MCP edit time-map keys"),
                      {{QStringLiteral("maps"), readback}, {QStringLiteral("items"), itemsReadback(timeline, affected)}});
    }
    if (name == QLatin1String("set_time_remap_enabled")) {
        bool enabled = false;
        if (!boolean(arguments, QStringLiteral("enabled"), false, enabled))
            return reject(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("enabled must be boolean."));
        for (int id : affected)
            if (!timeline->isClip(id) || (timeline->getClipState(id).second != ClipType::Video && timeline->getClipState(id).second != ClipType::AV))
                return reject(QStringLiteral("MEDIA_UNSUPPORTED"), QStringLiteral("Time remap requires video/AV without grouped audio."));
        Fun undo = [] { return true; }, redo = [] { return true; };
        bool changed = false;
        for (int id : affected) {
            const bool before = timeline->data(timeline->makeClipIndexFromID(id), TimelineModel::TimeRemapRole).toBool();
            if (before == enabled) continue;
            if (!timeline->requestClipTimeRemap(id, enabled, undo, redo) ||
                timeline->data(timeline->makeClipIndexFromID(id), TimelineModel::TimeRemapRole).toBool() != enabled)
                return rollback(undo, QStringLiteral("Native time remap change failed."));
            changed = true;
        }
        if (!changed) return finish({{QStringLiteral("items"), itemsReadback(timeline, affected)}});
        return commit(undo, redo, QStringLiteral("MCP set time remapping"), {{QStringLiteral("items"), itemsReadback(timeline, affected)}});
    }
    return reject(QStringLiteral("UNKNOWN_TOOL"), QStringLiteral("Unknown timeline tool."));
}
