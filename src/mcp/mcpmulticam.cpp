/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "bin/model/subtitlemodel.hpp"
#include "core.h"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "mcphelpers.h"
#include "mcpserver.h"
#include "timeline2/model/clipmodel.hpp"
#include "timeline2/model/compositionmodel.hpp"
#include "timeline2/model/groupsmodel.hpp"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/model/trackmodel.hpp"
#include <QCryptographicHash>
#include <QDomDocument>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <limits>
#include <tuple>
#include <vector>

namespace {
constexpr int MaxFrame = 10000000;
QString k(const char *value)
{
    return QString::fromLatin1(value);
}
struct Zone
{
    int start, end;
};
struct Selection
{
    int track, start, end;
};
struct Fragment
{
    int kind, track, start, end, sourceIn, state;
    QString source;
    double speed;
    bool endless;
    auto key() const { return std::tie(kind, track, start, end, sourceIn, state, source, speed, endless); }
};
bool sameFragments(std::vector<Fragment> first, std::vector<Fragment> second)
{
    const auto less = [](const Fragment &a, const Fragment &b) { return a.key() < b.key(); };
    std::sort(first.begin(), first.end(), less);
    std::sort(second.begin(), second.end(), less);
    if (first.size() != second.size()) return false;
    for (size_t i = 0; i < first.size(); ++i)
        if (first[i].key() != second[i].key()) return false;
    return true;
}
QJsonArray sortedIds(const std::unordered_set<int> &items)
{
    std::vector<int> values(items.begin(), items.end());
    std::sort(values.begin(), values.end());
    QJsonArray result;
    for (int value : values)
        result.append(value);
    return result;
}
QJsonValue canonicalGroups(const QJsonValue &value)
{
    if (value.isArray()) {
        std::vector<QJsonValue> entries;
        for (const auto &entry : value.toArray())
            entries.push_back(canonicalGroups(entry));
        const auto bytes = [](const QJsonValue &entry) { return QJsonDocument(QJsonArray{entry}).toJson(QJsonDocument::Compact); };
        std::sort(entries.begin(), entries.end(), [&](const auto &a, const auto &b) { return bytes(a) < bytes(b); });
        QJsonArray result;
        for (const auto &entry : entries)
            result.append(entry);
        return result;
    }
    if (value.isObject()) {
        auto result = value.toObject();
        for (auto it = result.begin(); it != result.end(); ++it)
            it.value() = canonicalGroups(it.value());
        return result;
    }
    return value;
}
QString sha(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
} // namespace

QJsonArray KdenliveMcpServer::multicamToolCatalog()
{
    auto angles = Mcp::property(k("array"), k("2-16 unique native video track IDs used as camera angles. Other tracks are preserved."));
    angles.insert(k("items"), QJsonObject{{k("type"), k("integer")}, {k("minimum"), 0}, {k("maximum"), MaxFrame}});
    angles.insert(k("minItems"), 2);
    angles.insert(k("maxItems"), 16);
    angles.insert(k("uniqueItems"), true);
    auto selections = Mcp::property(k("array"), k("1-256 sorted, nonoverlapping half-open selections. Gaps and material outside these ranges are preserved."));
    selections.insert(k("minItems"), 1);
    selections.insert(k("maxItems"), 256);
    selections.insert(k("items"), QJsonObject{{k("type"), k("object")},
                                              {k("additionalProperties"), false},
                                              {k("properties"), QJsonObject{{k("trackId"), Mcp::property(k("integer"), k("Chosen angle track."))},
                                                                            {k("startFrame"), Mcp::property(k("integer"), k("Inclusive timeline start."))},
                                                                            {k("endFrame"), Mcp::property(k("integer"), k("Exclusive timeline end."))}}},
                                              {k("required"), QJsonArray{k("trackId"), k("startFrame"), k("endFrame")}}});
    return {Mcp::tool(
        k("multicam_cut"),
        k("Keep the chosen camera in each explicit range by lifting the other angle tracks. Preserve all existing audio and use one native undo "
          "transaction."),
        {{k("expectedState"), Mcp::property(k("string"), k("Fresh native project state token."))},
         {k("angleTrackIds"), angles},
         {k("selections"), selections},
         {k("acknowledgeGroupChanges"),
          Mcp::property(
              k("boolean"),
              k("Explicitly permit dissolving affected groups, including AV links. A refusal reports every affected group's member IDs before editing."))}},
        {k("expectedState"), k("angleTrackIds"), k("selections")})};
}

QJsonObject KdenliveMcpServer::callMulticamTool(const QString &name, const QJsonObject &args, const QJsonObject &state)
{
    if (name != k("multicam_cut")) return {};
    const auto reject = [&](const char *code, const QString &message, QJsonObject details = {}) {
        details.insert(k("ok"), false);
        details.insert(k("error"), QJsonObject{{k("code"), k(code)}, {k("message"), message}});
        details.insert(k("state"), snapshot());
        return Mcp::toolResult(details, true);
    };
    const auto timeline = m_timeline.lock();
    if (!timeline || !state.value(k("ready")).toBool()) return reject("NOT_READY", k("No native timeline is ready."));
    if (args.value(k("expectedState")) != state.value(k("expectedState"))) return reject("STALE_STATE", k("Read current native state first."));
    if (!timeline->normalEdit() || timeline->singleSelectionMode() || !timeline->getCurrentSelection().empty())
        return reject("UI_BUSY", k("Use normal edit mode and clear transient timeline selection."));
    if (args.contains(k("acknowledgeGroupChanges")) && !args.value(k("acknowledgeGroupChanges")).isBool())
        return reject("INVALID_ARGUMENT", k("acknowledgeGroupChanges must be boolean."));
    const auto angleValues = args.value(k("angleTrackIds")).toArray(), values = args.value(k("selections")).toArray();
    if (!args.value(k("angleTrackIds")).isArray() || angleValues.size() < 2 || angleValues.size() > 16 || !args.value(k("selections")).isArray() ||
        values.isEmpty() || values.size() > 256)
        return reject("INVALID_ARGUMENT", k("Specify 2-16 unique angle video tracks and 1-256 sorted half-open selections."));
    QSet<int> angles;
    for (const auto &value : angleValues) {
        int track = -1;
        if (!Mcp::integer({{k("id"), value}}, k("id"), 0, MaxFrame, track) || angles.contains(track) || !timeline->isTrack(track) ||
            timeline->isAudioTrack(track))
            return reject("INVALID_TRACK", k("Camera angles must be unique existing native video tracks."));
        angles.insert(track);
    }
    std::vector<Selection> selections;
    QMap<int, std::vector<Zone>> lifts;
    int previousEnd = 0;
    for (const auto &value : values) {
        const auto item = value.toObject();
        Selection selection;
        if (!value.isObject() || item.size() != 3 || !Mcp::integer(item, k("trackId"), 0, MaxFrame, selection.track) || !angles.contains(selection.track) ||
            !Mcp::integer(item, k("startFrame"), 0, MaxFrame - 1, selection.start) || !Mcp::integer(item, k("endFrame"), 1, MaxFrame, selection.end) ||
            selection.start < previousEnd || selection.end <= selection.start)
            return reject("INVALID_SELECTION", k("Selections must name angle tracks and sorted nonoverlapping positive half-open ranges."));
        previousEnd = selection.end;
        selections.push_back(selection);
        for (int track : angles) {
            if (track == selection.track) continue;
            auto &zones = lifts[track];
            if (!zones.empty() && zones.back().end == selection.start)
                zones.back().end = selection.end;
            else
                zones.push_back({selection.start, selection.end});
        }
    }
    const auto subtitles = timeline->getSubtitleModel();
    if (timeline->m_allClips.size() + timeline->m_allCompositions.size() + (subtitles ? subtitles->getAllSubIds().size() : 0) > 20000)
        return reject("MULTICAM_BOUNDS", k("One multicamera transaction supports a timeline of at most 20000 items."));
    if (!timelineConsistent(timeline))
        return reject("INVALID_NATIVE_STATE", k("The native timeline is inconsistent before editing; inspect its current state."));
    const auto fragments = [&] {
        std::vector<Fragment> result;
        for (const auto &entry : timeline->m_allClips) {
            const auto &clip = entry.second;
            if (!angles.contains(clip->getCurrentTrackId())) continue;
            result.push_back({0, clip->getCurrentTrackId(), clip->getPosition(),
                              int(std::min<qint64>(MaxFrame + 1, qint64(clip->getPosition()) + clip->getPlaytime())), clip->getIn(), int(clip->clipState()),
                              clip->binId(), timeline->getClipSpeed(entry.first), clip->getItemInfo().maxDuration < 0});
        }
        for (const auto &entry : timeline->m_allCompositions) {
            const auto &composition = entry.second;
            if (!angles.contains(composition->getCurrentTrackId())) continue;
            result.push_back({1, composition->getCurrentTrackId(), composition->getPosition(),
                              int(std::min<qint64>(MaxFrame + 1, qint64(composition->getPosition()) + composition->getPlaytime())), 0, 0,
                              composition->getAssetId(), 1, false});
        }
        return result;
    };
    const auto before = fragments();
    for (const auto &selection : selections) {
        if (timeline->getTrackProperty(selection.track, k("hide")).toInt() & 1)
            return reject("ANGLE_HAS_NO_VIDEO", k("A chosen camera track's video output is disabled."));
        std::vector<Zone> coverage;
        for (const auto &item : before) {
            if (item.kind == 0 && item.track == selection.track && item.state == int(PlaylistState::VideoOnly) && item.start < selection.end &&
                selection.start < item.end)
                coverage.push_back({item.start, item.end});
        }
        std::sort(coverage.begin(), coverage.end(), [](const Zone &a, const Zone &b) { return a.start < b.start; });
        int covered = selection.start;
        for (const auto &zone : coverage) {
            if (zone.start > covered) break;
            covered = std::max(covered, zone.end);
        }
        if (covered < selection.end) return reject("ANGLE_GAP", k("Every chosen camera must contain enabled video throughout its selected range."));
    }
    std::unordered_set<int> affected;
    QJsonArray plannedLifts, groupChanges;
    QMap<int, int> groupRepresentatives;
    for (auto it = lifts.begin(); it != lifts.end(); ++it) {
        std::vector<Zone> nonempty;
        for (const auto &zone : it.value()) {
            const auto items = timeline->getItemsInRange(it.key(), zone.start, zone.end);
            if (items.empty()) continue;
            if (timeline->trackIsLocked(it.key())) return reject("TRACK_LOCKED", k("A camera track requiring a lift is locked."));
            nonempty.push_back(zone);
            plannedLifts.append(QJsonObject{{k("trackId"), it.key()}, {k("startFrame"), zone.start}, {k("endFrame"), zone.end}});
            for (int item : items) {
                affected.insert(item);
                if (!timeline->isInGroup(item)) continue;
                const int root = timeline->m_groups->getRootId(item);
                if (groupRepresentatives.contains(root)) continue;
                const auto members = timeline->getGroupElements(item);
                if (members.size() > 2048) return reject("MULTICAM_BOUNDS", k("An affected group exceeds 2048 members."));
                groupRepresentatives[root] = item;
                std::unordered_set<int> audio;
                for (int member : members)
                    if (timeline->isClip(member) && timeline->isAudioTrack(timeline->getItemTrackId(member))) audio.insert(member);
                groupChanges.append(QJsonObject{{k("groupRootId"), root}, {k("memberIds"), sortedIds(members)}, {k("preservedAudioIds"), sortedIds(audio)}});
            }
        }
        it.value() = std::move(nonempty);
    }
    if (affected.size() > 2048 || plannedLifts.size() > 4096)
        return reject("MULTICAM_BOUNDS", k("One transaction supports at most 2048 affected original items and 4096 lifts."));
    if (!groupChanges.isEmpty() && args.value(k("acknowledgeGroupChanges")) != QJsonValue(true))
        return reject("GROUP_ACKNOWLEDGEMENT_REQUIRED",
                      k("Native camera cuts dissolve affected groups, including AV links. Review all member IDs and explicitly acknowledge this change."),
                      {{k("affectedGroups"), groupChanges}});
    std::vector<Fragment> expected;
    for (const auto &item : before) {
        if (item.start < 0 || item.end > MaxFrame || item.end <= item.start)
            return reject("MULTICAM_BOUNDS", k("Camera material must fit the supported finite timeline range."));
        int cursor = item.start;
        const auto append = [&](int first, int last) {
            if (first >= last) return;
            auto fragment = item;
            fragment.start = first;
            fragment.end = last;
            if (item.kind == 0) {
                // Native resizes rebase endless generators to source frame zero
                // on either edge. Finite media must retain exact source frames.
                if (item.endless && (first != item.start || last != item.end))
                    fragment.sourceIn = 0;
                else
                    fragment.sourceIn += first - item.start;
            }
            expected.push_back(std::move(fragment));
        };
        for (const auto &zone : lifts.value(item.track)) {
            if (zone.end <= cursor || zone.start >= item.end) continue;
            append(cursor, std::min(zone.start, item.end));
            cursor = std::max(cursor, zone.end);
            if (cursor >= item.end) break;
        }
        append(cursor, item.end);
    }
    if (expected.size() > 8192) return reject("MULTICAM_BOUNDS", k("The resulting camera edit exceeds 8192 fragments."));
    // Snapshot actual native audio payloads and every unlisted track. Group
    // membership alone may change; in particular mirrorTrack is an AV-link
    // annotation, not source selection or audio timing.
    const auto protectedPayload = [&] {
        QDomDocument xml;
        auto root = xml.createElement(k("protected"));
        xml.appendChild(root);
        int audioEnd = 0;
        for (const auto &entry : timeline->m_allClips) {
            const auto &clip = entry.second;
            if (clip->getCurrentTrackId() >= 0 && timeline->isAudioTrack(clip->getCurrentTrackId()))
                audioEnd =
                    int(std::max<qint64>(audioEnd, std::min<qint64>(std::numeric_limits<int>::max(), qint64(clip->getPosition()) + clip->getPlaytime())));
        }
        const auto stack = [&](const std::shared_ptr<EffectStackModel> &model, bool boundAudio) {
            auto element = model->toXml(xml);
            element.setAttribute(k("enabled"), model->isStackEnabled());
            if (boundAudio)
                for (auto effect = element.firstChildElement(); !effect.isNull(); effect = effect.nextSiblingElement())
                    for (const char *name : {"in", "out"})
                        if (effect.hasAttribute(k(name))) effect.setAttribute(k(name), std::min(effect.attribute(k(name)).toInt(), std::max(0, audioEnd - 1)));
            return element;
        };
        root.appendChild(stack(timeline->getMasterEffectStackModel(), true));
        for (int position = 0; position < timeline->getTracksCount(); ++position) {
            const int id = timeline->getTrackIndexFromPosition(position);
            if (angles.contains(id)) continue;
            auto track = xml.createElement(k("track"));
            track.setAttribute(k("id"), id);
            track.setAttribute(k("position"), position);
            track.setAttribute(k("hide"), timeline->getTrackProperty(id, k("hide")).toString());
            track.setAttribute(k("locked"), timeline->trackIsLocked(id));
            track.appendChild(stack(timeline->getTrackEffectStackModel(id), false));
            const auto model = timeline->getTrackById_const(id);
            for (const auto &value : sortedIds(timeline->getItemsInRange(id, 0))) {
                const int item = value.toInt();
                if (timeline->isClip(item)) {
                    auto element = timeline->getClipPtr(item)->toXml(xml);
                    element.removeAttribute(k("mirrorTrack"));
                    track.appendChild(element);
                    const auto mix = model->getMixInfo(item).first;
                    if (mix.firstClipId >= 0) {
                        auto element = xml.createElement(k("mix"));
                        element.setAttribute(k("first"), mix.firstClipId);
                        element.setAttribute(k("second"), mix.secondClipId);
                        element.setAttribute(k("firstIn"), mix.firstClipInOut.first);
                        element.setAttribute(k("firstOut"), mix.firstClipInOut.second);
                        element.setAttribute(k("secondIn"), mix.secondClipInOut.first);
                        element.setAttribute(k("secondOut"), mix.secondClipInOut.second);
                        element.setAttribute(k("offset"), mix.mixOffset);
                        const auto parameters = model->getMixParams(item);
                        element.setAttribute(k("asset"), parameters.first);
                        for (const auto &parameter : parameters.second) {
                            auto value = xml.createElement(k("parameter"));
                            value.setAttribute(k("name"), parameter.first);
                            value.appendChild(xml.createTextNode(parameter.second.toString()));
                            element.appendChild(value);
                        }
                        track.appendChild(element);
                    }
                } else if (timeline->isComposition(item))
                    track.appendChild(timeline->getCompositionPtr(item)->toXml(xml));
            }
            root.appendChild(track);
        }
        if (subtitles)
            for (const auto &value : sortedIds(subtitles->getAllSubIds()))
                root.appendChild(subtitles->toXml(value.toInt(), xml));
        return xml.toByteArray(-1);
    };
    const QByteArray protectedBefore = protectedPayload();
    const auto groupsBefore = canonicalGroups(QJsonDocument::fromJson(timeline->groupsData().toUtf8()).array());
    Fun undo = [] { return true; }, redo = [] { return true; };
    const auto rollback = [&](const QString &message, QJsonObject diagnostics = {}) {
        const bool restored = undo();
        const auto after = protectedPayload();
        const bool payloadMatched = after == protectedBefore, rangesMatched = sameFragments(before, fragments()),
                   groupsMatched = canonicalGroups(QJsonDocument::fromJson(timeline->groupsData().toUtf8()).array()) == groupsBefore,
                   consistent = timelineConsistent(timeline);
        const bool matched = restored && payloadMatched && rangesMatched && groupsMatched && consistent;
        diagnostics.insert(k("rollbackChecks"), QJsonObject{{k("nativeUndo"), restored},
                                                            {k("protectedPayload"), payloadMatched},
                                                            {k("retainedRanges"), rangesMatched},
                                                            {k("groups"), groupsMatched},
                                                            {k("consistency"), consistent}});
        return reject(matched ? "NATIVE_EDIT_FAILED" : "ROLLBACK_FAILED",
                      matched ? message + k(" All owned edits were rolled back.")
                              : k("Native rollback did not fully restore the project; inspect current state and history."),
                      diagnostics);
    };
    if (plannedLifts.isEmpty())
        return Mcp::toolResult(
            {{k("ok"), true}, {k("changed"), false}, {k("affectedGroups"), groupChanges}, {k("liftedZones"), plannedLifts}, {k("state"), snapshot()}});
    // Detach complete affected roots first. This prevents native group-aware
    // boundary cuts from cutting linked audio or another chosen camera.
    for (int item : groupRepresentatives)
        if (timeline->isInGroup(item) && !timeline->requestClipUngroup(item, undo, redo)) return rollback(k("Native group detachment failed."));
    for (auto it = lifts.cbegin(); it != lifts.cend(); ++it)
        for (const auto &zone : it.value())
            if (!TimelineFunctions::extractZoneWithUndo(timeline, {it.key()}, QPoint(zone.start, zone.end), true, -1, {}, undo, redo))
                return rollback(k("Native camera lift failed."));
    const auto protectedAfter = protectedPayload();
    const auto actual = fragments();
    const bool payloadMatches = protectedAfter == protectedBefore, rangesMatch = sameFragments(expected, actual), consistent = timelineConsistent(timeline);
    if (!payloadMatches || !rangesMatch || !consistent) {
        const auto records = [&](const std::vector<Fragment> &values) {
            QJsonArray result;
            for (const auto &value : values) {
                if (result.size() == 64) break;
                result.append(QJsonObject{{k("kind"), value.kind},
                                          {k("trackId"), value.track},
                                          {k("startFrame"), value.start},
                                          {k("endFrame"), value.end},
                                          {k("sourceInFrame"), value.sourceIn},
                                          {k("clipState"), value.state},
                                          {k("source"), value.source},
                                          {k("speed"), value.speed},
                                          {k("endless"), value.endless}});
            }
            return result;
        };
        QJsonObject diagnostics{
            {k("postEditChecks"), QJsonObject{{k("protectedPayload"), payloadMatches}, {k("retainedRanges"), rangesMatch}, {k("consistency"), consistent}}},
            {k("expectedFragmentPrefix"), records(expected)},
            {k("actualFragmentPrefix"), records(actual)}};
        return rollback(k("Native camera edits did not preserve exact retained ranges or protected audio/other tracks."), diagnostics);
    }
    pCore->pushUndo(undo, redo, k("Multicamera cut"));
    return Mcp::toolResult({{k("ok"), true},
                            {k("changed"), true},
                            {k("selectionCount"), int(selections.size())},
                            {k("affectedGroups"), groupChanges},
                            {k("liftedZones"), plannedLifts},
                            {k("preservedPayloadSha256"), sha(protectedBefore)},
                            {k("state"), snapshot()}});
}
