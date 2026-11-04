/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "mcpstate.h"
#include "assets/model/assetparametermodel.hpp"
#include "bin/model/markerlistmodel.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "jobs/taskmanager.h"
#include "mcphelpers.h"
#include "mcpmedia.h"
#include "mcpsequence.h"
#include "mcpwire.h"
#include "project/notesplugin.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include <QCryptographicHash>
#include <QDomDocument>
#include <QHash>
#include <QJsonDocument>
#include <QObject>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <framework/mlt_chain.h>
#include <framework/mlt_filter.h>
#include <framework/mlt_link.h>
#include <framework/mlt_multitrack.h>
#include <framework/mlt_playlist.h>
#include <framework/mlt_producer.h>
#include <framework/mlt_service.h>
#include <limits>
#include <mlt++/MltChain.h>
#include <mlt++/MltLink.h>
#include <mlt++/MltProfile.h>
#include <mlt++/MltTractor.h>
#include <vector>

namespace {
QString sk(const char *s)
{
    return QString::fromLatin1(s);
}
constexpr quint64 PublicStateBudget = 96 * 1024;
constexpr quint64 PageBudget = 96 * 1024;

// Versioned, typed, length-framed canonical input to SHA-256. QString values are
// fed in fixed-size UTF-16BE chunks; neither a complete JSON document nor one
// UTF-8 copy of a large native comment/group string is needed for fingerprinting.
class RecordHash
{
public:
    RecordHash() { m_hash.addData(QByteArrayView("kdenlive-native-state-v2", 24)); }
    void number(quint64 n)
    {
        std::array<char, 8> bytes{};
        qToBigEndian(n, reinterpret_cast<uchar *>(bytes.data()));
        m_hash.addData(QByteArrayView(bytes.data(), bytes.size()));
    }
    void text(const QString &s)
    {
        number(quint64(s.size()));
        std::array<char, 8192> bytes; // Every byte passed below is filled before hashing.
        for (qsizetype offset = 0; offset < s.size();) {
            const qsizetype count = qMin<qsizetype>(4096, s.size() - offset);
            for (qsizetype i = 0; i < count; ++i)
                qToBigEndian<quint16>(s.at(offset + i).unicode(), reinterpret_cast<uchar *>(bytes.data() + 2 * i));
            m_hash.addData(QByteArrayView(bytes.data(), 2 * count));
            offset += count;
        }
    }
    void value(const QJsonValue &v)
    {
        number(quint64(v.type()));
        if (v.isString())
            text(v.toString());
        else if (v.isDouble()) {
            const double n = v.toDouble();
            quint64 bits = 0;
            static_assert(sizeof(bits) == sizeof(n));
            std::memcpy(&bits, &n, sizeof(bits));
            number(bits);
        } else if (v.isBool())
            number(v.toBool() ? 1 : 0);
        else if (v.isArray()) {
            const auto a = v.toArray();
            number(quint64(a.size()));
            for (const auto &entry : a)
                value(entry);
        } else if (v.isObject()) {
            const auto o = v.toObject();
            number(quint64(o.size()));
            for (auto it = o.begin(); it != o.end(); ++it) {
                text(it.key());
                value(it.value());
            }
        }
    }
    void record(const QString &section, const QString &id, const QJsonValue &v)
    {
        text(section);
        text(id);
        value(v);
    }
    QString result() const { return QString::fromLatin1(m_hash.result().toHex()); }

private:
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
};
// Native transport uses _position/_speed. Plain position/speed can be editable
// effect parameters (for example wave and timer); only known service-local
// telemetry is excluded. Readonly metadata alone is insufficient: some readonly
// analysis results and producer inputs still determine rendered content.
bool persistedSequenceProperty(const QString &key)
{
    return key != sk("position") && key != sk("activeTrack") && key != sk("scrollPos") && key != sk("zoom") && key != sk("verticalzoom");
}
bool persistedSourceProperty(const QString &key)
{
    const QString sequencePrefix = sk("kdenlive:sequenceproperties.");
    return key != sk("kdenlive:monitorPosition") && key != sk("kdenlive:thumbZoomFactor") && key != sk("kdenlive:thumbZoomStart") &&
           (!key.startsWith(sequencePrefix) || persistedSequenceProperty(key.mid(sequencePrefix.size())));
}
bool persistedGraphProperty(const QString &key, mlt_service_type type, const QString &service)
{
    if (key.isEmpty() || key.startsWith(QLatin1Char('_')) || key.startsWith(sk("meta.")) || key.startsWith(sk("xml_retain")) || !persistedSourceProperty(key))
        return false;
    if (type == mlt_service_link_type && service == sk("timeremap") && key == sk("speed")) return false;
    if (type == mlt_service_transition_type && service == sk("mix") && key == sk("duck_level")) return false;
    return true;
}
QString nativeDocumentNotes(KdenliveDoc *document, const std::shared_ptr<ProjectItemModel> &bin)
{
    const auto manager = pCore->projectManager();
    // Headless ProjectManager has no notes plugin. The live widget belongs only
    // to the current document; querying its native getter otherwise can crash.
    if (pCore->window() && pCore->currentDoc() == document && manager && manager->findChild<NotesPlugin *>()) return manager->documentNotes();
    const auto tractor = bin->projectTractor();
    if (!tractor || !tractor->is_valid()) return {};
    const auto retained = reinterpret_cast<mlt_service>(tractor->get_data("xml_retain main_bin"));
    if (retained) return QString::fromUtf8(mlt_properties_get(mlt_service_properties(retained), "kdenlive:documentnotes"));
    // XML-loaded projects can hold the retained bin inside this property map
    // before native project setup moves it to the direct retained-service key.
    const auto retainedProperties = reinterpret_cast<mlt_properties>(tractor->get_data("xml_retain"));
    const auto playlist = retainedProperties ? reinterpret_cast<mlt_playlist>(mlt_properties_get_data(retainedProperties, "main_bin", nullptr)) : nullptr;
    return playlist ? QString::fromUtf8(mlt_properties_get(MLT_PLAYLIST_PROPERTIES(playlist), "kdenlive:documentnotes")) : QString();
}
// A live sequence can temporarily have no TimelineItemModel (native sequence
// closure/deletion staging). Fingerprint its actual upstream MLT graph without
// pulling frames, moving producer positions, or invoking the XML consumer (which
// mutates xml_retain flags and can serialize the retained whole project).
struct GraphFingerprint
{
    QString hash;
    QString reason;
};
GraphFingerprint sequenceGraphHash(const std::shared_ptr<ProjectClip> &source)
{
    RecordHash hash;
    if (!source || !source->statusReady()) {
        hash.text(sk("unavailable"));
        return {hash.result(), {}};
    }
    const auto original = source->originalProducer();
    if (!original || !original->is_valid()) {
        hash.text(sk("missing"));
        return {hash.result(), {}};
    }
    Mlt::Producer root(original->parent());
    struct Edge
    {
        QString name;
        mlt_service service;
    };
    std::vector<Edge> pending{{sk("root"), root.get_service()}};
    QHash<quintptr, quint64> seen;
    while (!pending.empty()) {
        const auto edge = pending.back();
        pending.pop_back();
        hash.text(edge.name);
        if (!edge.service) {
            hash.text(sk("null"));
            continue;
        }
        const auto identity = reinterpret_cast<quintptr>(edge.service);
        if (seen.contains(identity)) {
            hash.text(sk("reference"));
            hash.number(seen.value(identity));
            continue;
        }
        const quint64 ordinal = quint64(seen.size());
        seen.insert(identity, ordinal);
        hash.text(sk("service"));
        hash.number(ordinal);
        const auto type = mlt_service_identify(edge.service);
        hash.number(quint64(type));
        QJsonObject values;
        {
            struct ReadLock
            {
                mlt_service service;
                explicit ReadLock(mlt_service value)
                    : service(value)
                {
                    mlt_service_lock(service);
                }
                ~ReadLock() { mlt_service_unlock(service); }
            } lock(edge.service);
            const auto properties = mlt_service_properties(edge.service);
            const QString service = QString::fromUtf8(mlt_properties_get(properties, "mlt_service"));
            const int propertyCount = mlt_properties_count(properties);
            for (int row = 0; row < propertyCount; ++row) {
                const char *rawKey = mlt_properties_get_name(properties, row);
                if (!rawKey) continue;
                const QString key = QString::fromUtf8(rawKey);
                if (!persistedGraphProperty(key, type, service)) continue;
                if (const char *rawValue = mlt_properties_get(properties, rawKey)) values.insert(key, QString::fromUtf8(rawValue));
            }
        }
        hash.value(values);
        std::vector<Edge> children;
        const int filters = mlt_service_filter_count(edge.service);
        for (int row = 0; row < filters; ++row) {
            const auto filter = mlt_service_filter(edge.service, row);
            children.push_back({sk("filter/") + QString::number(row), filter ? MLT_FILTER_SERVICE(filter) : nullptr});
        }
        if (type == mlt_service_multitrack_type) {
            const auto multitrack = reinterpret_cast<mlt_multitrack>(edge.service);
            const int count = mlt_multitrack_count(multitrack);
            hash.number(quint64(count));
            for (int row = 0; row < count; ++row) {
                const auto track = mlt_multitrack_track(multitrack, row);
                children.push_back({sk("track/") + QString::number(row), track ? MLT_PRODUCER_SERVICE(track) : nullptr});
            }
        } else if (type == mlt_service_playlist_type) {
            const auto playlist = reinterpret_cast<mlt_playlist>(edge.service);
            const int count = mlt_playlist_count(playlist);
            hash.number(quint64(count));
            qint64 start = 0;
            auto entry = [&](mlt_position in, mlt_position out, mlt_position duration, int repeat, mlt_producer cut, int row) {
                if (duration < 0 || start > std::numeric_limits<mlt_position>::max() - qint64(duration)) return false;
                hash.record(sk("playlistEntry"), QString::number(row),
                            QJsonObject{{sk("start"), start}, {sk("in"), in}, {sk("out"), out}, {sk("duration"), duration}, {sk("repeat"), repeat}});
                children.push_back({sk("clip/") + QString::number(row), cut ? MLT_PRODUCER_SERVICE(cut) : nullptr});
                start += duration;
                return true;
            };
#if defined(MLT_PLAYLIST_ENTRY_INFO_API) && MLT_PLAYLIST_ENTRY_INFO_API >= 1
            // Optional additive read-only MLT API supplied in packaging/patches.
            // Fixed scratch and checked local starts
            // preserve repeats without repeatedly summing preceding entries.
            std::array<mlt_playlist_entry_info, 256> entries;
            for (int first = 0; first < count;) {
                const int length = qMin(256, count - first);
                if (mlt_playlist_get_entries_info(playlist, first, length, entries.data()) != 0)
                    return {{}, sk("Native playlist bulk-entry read failed; no verified editing token was issued.")};
                for (int row = 0; row < length; ++row) {
                    const auto &info = entries[size_t(row)];
                    if (!entry(info.frame_in, info.frame_out, info.frame_count, info.repeat, info.cut, first + row))
                        return {{}, sk("Native playlist durations exceed their position range; no verified editing token was issued.")};
                }
                first += length;
            }
#else
            // Temporary compatibility boundary, confined to unbuilt raw graphs.
            // Current MLT get_clip_info recomputes prefix starts per entry.
            // Never truncate repeats or issue an incomplete freshness token.
            if (count > 512)
                return {{},
                        sk("An unbuilt native sequence contains a raw playlist above512 entries. Exact scalable repeat/range fingerprinting "
                           "requires MLT built with the supplied bulk-entry read patch.")};
            for (int row = 0; row < count; ++row) {
                mlt_playlist_clip_info info{};
                if (mlt_playlist_get_clip_info(playlist, &info, row) != 0 ||
                    !entry(info.frame_in, info.frame_out, info.frame_count, info.repeat, info.cut, row))
                    return {{}, sk("Native playlist entry read failed or exceeded its position range; no verified editing token was issued.")};
            }
#endif
        } else if (type == mlt_service_chain_type) {
            const auto chain = reinterpret_cast<mlt_chain>(edge.service);
            const auto sourceProducer = mlt_chain_get_source(chain);
            children.push_back({sk("source"), sourceProducer ? MLT_PRODUCER_SERVICE(sourceProducer) : nullptr});
            const int count = mlt_chain_link_count(chain);
            hash.number(quint64(count));
            for (int row = 0; row < count; ++row) {
                const auto link = mlt_chain_link(chain, row);
                children.push_back({sk("link/") + QString::number(row), link ? MLT_LINK_SERVICE(link) : nullptr});
            }
        } else {
            children.push_back({sk("upstream"), mlt_service_producer(edge.service)});
        }
        if ((type == mlt_service_producer_type || type == mlt_service_tractor_type || type == mlt_service_playlist_type || type == mlt_service_chain_type ||
             type == mlt_service_link_type) &&
            mlt_producer_is_cut(reinterpret_cast<mlt_producer>(edge.service))) {
            const auto parent = mlt_producer_cut_parent(reinterpret_cast<mlt_producer>(edge.service));
            children.push_back({sk("cutParent"), parent ? MLT_PRODUCER_SERVICE(parent) : nullptr});
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            pending.push_back(*it);
    }
    return {hash.result(), {}};
}
QString stateValueHash(const QJsonValue &value)
{
    RecordHash hash;
    hash.value(value);
    return hash.result();
}
QString stateEffectsHash(const std::shared_ptr<EffectStackModel> &stack)
{
    if (!stack) return {};
    QDomDocument xml;
    xml.appendChild(stack->toXml(xml));
    return QString::fromLatin1(QCryptographicHash::hash(xml.toByteArray(0), QCryptographicHash::Sha256).toHex());
}
QString markerHash(const std::shared_ptr<MarkerListModel> &model)
{
    RecordHash hash;
    if (model) {
        const auto markers = model->getAllMarkers();
        hash.number(quint64(markers.size()));
        for (const auto &marker : markers)
            hash.value(QJsonObject{{sk("id"), model->markerIdAtFrame(marker.time().frames(pCore->getCurrentFps()))},
                                   {sk("seconds"), marker.time().seconds()},
                                   {sk("durationSeconds"), marker.duration().seconds()},
                                   {sk("comment"), marker.comment()},
                                   {sk("category"), marker.markerType()}});
    }
    return hash.result();
}
class StateBridge final : public QObject
{
public:
    explicit StateBridge(QObject *parent)
        : QObject(parent)
    {
    }
    QList<McpState::ImportedSource> imports;
    QHash<QString, McpState::ImportedSource> importById;
    QJsonObject header;
    McpState::ClipStateReader clipStateReader;
};
StateBridge *stateBridge(QObject *owner)
{
    for (QObject *child : owner->children())
        if (auto *bridge = dynamic_cast<StateBridge *>(child)) return bridge;
    return new StateBridge(owner);
}
QString mediaTypeName(const std::shared_ptr<ProjectClip> &clip)
{
    if (!clip) return sk("unknown");
    switch (clip->clipType()) {
    case ClipType::Image:
        return sk("image");
    case ClipType::Text:
    case ClipType::TextTemplate:
        return sk("title");
    case ClipType::SlideShow:
        return sk("slideshow");
    case ClipType::Audio:
        return sk("audio");
    case ClipType::Video:
        return sk("video");
    case ClipType::AV:
        return sk("av");
    case ClipType::Color:
        return sk("color");
    case ClipType::Timeline:
        return sk("sequence");
    case ClipType::Unknown:
        return sk("unknown");
    default:
        return sk("unsupported");
    }
}
QString sourceStatus(const std::shared_ptr<ProjectClip> &clip, const QString &importPath = {})
{
    if (!clip) return sk("failed");
    if (clip->clipStatus() == FileStatus::StatusWaiting) {
        const ObjectId owner(KdenliveObjectType::BinClip, clip->clipId().toInt(), QUuid());
        return pCore->taskManager.hasPendingJob(owner, AbstractTask::LOADJOB) ? sk("loading") : sk("failed");
    }
    if (!clip->statusReady()) return sk("failed");
    const auto type = clip->clipType();
    if (!importPath.isEmpty() && type != ClipType::Image && type != ClipType::Audio && type != ClipType::AV && type != ClipType::Video)
        return sk("unsupported");
    if ((type == ClipType::Audio || type == ClipType::AV || type == ClipType::Video) &&
        (clip->getProducerIntProperty(sk("_wait_for_transcode")) != 0 || clip->getProducerIntProperty(sk("meta.media.variable_frame_rate")) != 0 ||
         clip->getProducerIntProperty(sk("seekable")) == 0))
        return sk("needs_transcode");
    return sk("ready");
}
QJsonObject trackRecord(const std::shared_ptr<TimelineItemModel> &timeline, int id)
{
    return {{sk("id"), id},
            {sk("position"), timeline->getTrackPosition(id)},
            {sk("name"), timeline->getTrackFullName(id)},
            {sk("audio"), timeline->isAudioTrack(id)},
            {sk("locked"), timeline->trackIsLocked(id)},
            {sk("effectsHash"), stateEffectsHash(timeline->getTrackEffectStackModel(id))},
            {sk("hide"), timeline->getTrackProperty(id, sk("hide")).toString()},
            {sk("trackName"), timeline->getTrackProperty(id, sk("kdenlive:track_name")).toString()}};
}
QJsonObject timelineRecord(const std::shared_ptr<TimelineItemModel> &timeline, int id, const McpState::ClipStateReader &clipStateReader)
{
    QJsonObject item{{sk("id"), id},
                     {sk("trackId"), timeline->getItemTrackId(id)},
                     {sk("startFrame"), timeline->getItemPosition(id)},
                     {sk("durationFrames"), timeline->getItemPlaytime(id)}};
    if (timeline->isClip(id)) {
        const auto range = timeline->getClipInOut(id);
        item.insert(sk("binId"), timeline->getClipBinId(id));
        item.insert(sk("name"), timeline->getClipName(id));
        item.insert(sk("inFrame"), range.first);
        item.insert(sk("outFrame"), range.second);
        item.insert(sk("grouped"), timeline->isInGroup(id));
        item.insert(sk("mixDuration"), timeline->getMixDuration(id));
        item.insert(sk("endMix"), timeline->hasClipEndMix(id));
        item.insert(sk("speed"), timeline->getClipSpeed(id));
        item.insert(sk("clipState"), static_cast<int>(timeline->getClipState(id).first));
        item.insert(sk("effectsHash"), stateEffectsHash(timeline->getClipEffectStackModel(id)));
        const bool remapped = timeline->data(timeline->makeClipIndexFromID(id), TimelineModel::TimeRemapRole).toBool();
        item.insert(sk("timeRemap"), remapped);
        if (remapped) {
            const auto producer = timeline->getClipProducer(id);
            if (producer && producer->parent().type() == mlt_service_chain_type) {
                Mlt::Chain chain(producer->parent());
                for (int row = 0; row < chain.link_count(); ++row) {
                    std::unique_ptr<Mlt::Link> link(chain.link(row));
                    if (!link || QString::fromUtf8(link->get("mlt_service")) != sk("timeremap")) continue;
                    const char *map = link->property_exists("time_map") ? link->get("time_map") : link->get("map");
                    item.insert(sk("timeMap"), QJsonObject{{sk("time_map"), QString::fromUtf8(map)},
                                                           {sk("pitch"), QString::fromUtf8(link->get("pitch"))},
                                                           {sk("image_mode"), QString::fromUtf8(link->get("image_mode"))}});
                    break;
                }
            }
        }
        if (clipStateReader) {
            const auto extra = clipStateReader(timeline, id);
            for (auto it = extra.begin(); it != extra.end(); ++it)
                item.insert(it.key(), it.value());
        }
    } else {
        QJsonObject values;
        if (const auto model = timeline->getCompositionParameterModel(id)) {
            item.insert(sk("assetId"), model->getAssetId());
            for (const auto &p : model->getAllParameters())
                values.insert(p.first, p.second.toString());
        }
        item.insert(sk("parameters"), values);
        const auto model = timeline->getCompositionParameterModel(id);
        item.insert(sk("aTrack"), model && model->getAsset() ? model->getAsset()->get_int("a_track") : -1);
        item.insert(sk("forcedTrack"), timeline->data(timeline->makeCompositionIndexFromID(id), TimelineModel::ItemATrack).toInt());
    }
    return item;
}
QJsonObject sourceProperties(const std::shared_ptr<ProjectClip> &clip)
{
    QJsonObject properties;
    if (!clip || !clip->statusReady()) return properties;
    const auto stored = clip->getPropertiesFromPrefix(sk("kdenlive:"), true);
    for (auto it = stored.begin(); it != stored.end(); ++it) {
        // Saved monitor/timeline view state is separate from editing state.
        if (!persistedSourceProperty(it.key())) continue;
        properties.insert(it.key(), it.value());
    }
    // Use the editor's own producer-reload preservation list, including native
    // title XML, aspect/color overrides, proxies and masks. Runtime frame/cache
    // positions are not part of this persisted editing state.
    const char *allocated = ClipController::getPassPropertiesList();
    const QByteArray names(allocated ? allocated : "");
    std::free(const_cast<char *>(allocated));
    for (const auto &name : names.split(','))
        if (!name.isEmpty()) {
            const QString key = QString::fromLatin1(name);
            properties.insert(key, clip->getProducerProperty(key));
        }
    return properties;
}
QJsonObject sourceRecord(const std::shared_ptr<ProjectClip> &clip, const QString &ownedPath)
{
    const bool ready = clip->statusReady();
    return {{sk("id"), clip->clipId()},
            {sk("name"), clip->getData(AbstractProjectItem::DataName).toString()},
            {sk("ready"), ready},
            {sk("status"), sourceStatus(clip, ownedPath)},
            {sk("mediaType"), mediaTypeName(clip)},
            {sk("hasAudio"), ready && clip->hasAudio()},
            {sk("hasVideo"), ready && clip->hasVideo()},
            {sk("imported"), !ownedPath.isEmpty()},
            {sk("resource"), ready ? clip->getProducerProperty(sk("resource")) : ownedPath},
            {sk("durationFrames"), ready ? clip->getFramePlaytime() : 0},
            {sk("effectsHash"), ready ? stateEffectsHash(clip->getEffectStack()) : QString()},
            {sk("streams"), ready ? McpMedia::sourceState(clip) : QJsonObject{}},
            {sk("editablePropertiesHash"), ready ? stateValueHash(sourceProperties(clip)) : QString()}};
}
QJsonObject binRecord(const std::shared_ptr<AbstractProjectItem> &item, int siblingRow)
{
    const auto parent = item->parent();
    QJsonObject result{{sk("id"), item->clipId()},
                       {sk("kind"), static_cast<int>(item->itemType())},
                       {sk("parentId"), parent ? parent->clipId() : QString()},
                       {sk("row"), siblingRow},
                       {sk("childCount"), item->childCount()},
                       {sk("name"), item->name()},
                       {sk("description"), item->description()},
                       {sk("rating"), static_cast<int>(item->rating())},
                       {sk("tags"), item->tags()}};
    if (item->itemType() == AbstractProjectItem::SubClipItem || item->itemType() == AbstractProjectItem::SubSequenceItem) {
        result.insert(sk("inFrame"), item->zone().x());
        result.insert(sk("outFrame"), item->zone().y());
    }
    if (item->itemType() == AbstractProjectItem::ClipItem) {
        const auto source = std::static_pointer_cast<ProjectClip>(item);
        result.insert(sk("ready"), source->statusReady());
        if (source->statusReady()) {
            result.insert(sk("sourceZones"), source->getProducerProperty(sk("kdenlive:clipzones")));
            result.insert(sk("markersHash"), markerHash(source->getMarkerModel()));
        }
    }
    return result;
}
// Iterative native preorder traversal has no artificial node/depth cutoff. A
// corrupt cycle/alias is rejected instead of recursing forever.
bool walkBin(const std::shared_ptr<AbstractProjectItem> &root, const std::function<void(const std::shared_ptr<AbstractProjectItem> &, int)> &visit)
{
    if (!root) return false;
    struct Frame
    {
        std::shared_ptr<AbstractProjectItem> item;
        int siblingRow;
        std::vector<std::shared_ptr<TreeItem>> children;
        qsizetype nextChild;
    };
    std::vector<Frame> stack{{root, -1, {}, -1}};
    QSet<const AbstractProjectItem *> seen;
    while (!stack.empty()) {
        auto &frame = stack.back();
        if (frame.nextChild < 0) {
            if (!frame.item || seen.contains(frame.item.get())) return false;
            seen.insert(frame.item.get());
            visit(frame.item, frame.siblingRow);
            frame.children = frame.item->directChildren();
            frame.nextChild = 0;
        }
        if (frame.nextChild >= qsizetype(frame.children.size())) {
            stack.pop_back();
            continue;
        }
        const int row = int(frame.nextChild++);
        const auto child = std::static_pointer_cast<AbstractProjectItem>(frame.children.at(row));
        stack.push_back({child, row, {}, -1});
    }
    return true;
}

std::vector<int> trackOrder(const std::shared_ptr<TimelineItemModel> &timeline)
{
    const auto all = timeline->getAllTracksIds();
    std::vector<int> ids(all.begin(), all.end());
    std::sort(ids.begin(), ids.end(), [timeline](int a, int b) { return timeline->getTrackPosition(a) < timeline->getTrackPosition(b); });
    return ids;
}
QJsonObject subtitleRecord(const std::shared_ptr<SubtitleModel> &model, int id)
{
    const int layer = model->getLayerForId(id);
    const auto start = model->getStartPosForId(id);
    const auto event = model->getSubtitle(layer, start);
    return {{sk("id"), id}, {sk("layer"), layer}, {sk("startPos"), start.seconds()}, {sk("dialogue"), event.toString(layer, start)}};
}
std::vector<int> subtitleOrder(const std::shared_ptr<SubtitleModel> &model)
{
    const auto all = model->getAllSubIds();
    std::vector<int> ids(all.begin(), all.end());
    std::sort(ids.begin(), ids.end(), [model](int a, int b) {
        const auto layerA = model->getLayerForId(a), layerB = model->getLayerForId(b);
        return layerA == layerB ? model->getStartPosForId(a) < model->getStartPosForId(b) : layerA < layerB;
    });
    return ids;
}
QJsonObject importRecord(const McpState::ImportedSource &item, const std::shared_ptr<ProjectItemModel> &bin)
{
    const auto clip = bin->hasClip(item.binId) ? bin->getClipByBinID(item.binId) : nullptr;
    const bool present = clip && clip == item.clip.lock();
    const auto status = sourceStatus(present ? clip : nullptr, item.path);
    QJsonObject result{{sk("binId"), item.binId}, {sk("path"), item.path}, {sk("present"), present}, {sk("status"), status}};
    if (!present)
        result.insert(sk("reason"), sk("Imported clip is absent (failed load, undo or deletion)."));
    else if (status == sk("needs_transcode"))
        result.insert(sk("reason"), sk("Native source requires conversion; inspect list_transcode_profiles and start_transcode."));
    else if (status == sk("unsupported"))
        result.insert(sk("reason"), sk("Decoded media type is outside this prototype."));
    else if (status == sk("failed"))
        result.insert(sk("reason"), sk("Native media loading failed, stopped without a producer, or the source is missing."));
    return result;
}
QJsonObject subtitleInfo(const std::shared_ptr<SubtitleModel> &model, KdenliveDoc *document)
{
    QJsonObject script;
    for (const auto &entry : model->scriptInfo())
        script.insert(entry.first, entry.second);
    return {{sk("scriptInfo"), script},
            {sk("forceStyle"), model->getForceStyle()},
            {sk("locked"), model->isLocked()},
            {sk("disabled"), model->isDisabled()},
            {sk("maxLayer"), model->getMaxLayer()},
            {sk("activeLayer"), model->activeSubLayer()},
            {sk("activeLanguageId"), document->getSequenceProperty(model->timeline()->uuid(), sk("kdenlive:activeSubtitleIndex"), sk("0"))}};
}
using LazyValue = std::function<QJsonValue()>;
using RecordVisitor = std::function<void(const QString &, const LazyValue &)>;
// Page enumeration constructs full native values only for the requested rows.
bool records(const QString &section, StateBridge *bridge, const std::shared_ptr<TimelineItemModel> &timeline, const std::shared_ptr<ProjectItemModel> &bin,
             KdenliveDoc *document, const RecordVisitor &visit)
{
    if (section == sk("header")) {
        visit(sk("all"), [bridge] { return QJsonValue(bridge->header); });
    } else if (section == sk("tracks")) {
        for (int id : trackOrder(timeline))
            visit(QString::number(id), [timeline, id] { return trackRecord(timeline, id); });
    } else if (section == sk("clips") || section == sk("compositions")) {
        for (int track : trackOrder(timeline)) {
            const auto found = timeline->getItemsInRange(track, 0, -1, true);
            std::vector<int> ids(found.begin(), found.end());
            std::sort(ids.begin(), ids.end());
            for (int id : ids)
                if ((section == sk("clips")) == timeline->isClip(id))
                    visit(QString::number(id), [timeline, id, bridge] { return timelineRecord(timeline, id, bridge->clipStateReader); });
        }
    } else if (section == sk("sources") || section == sk("source_properties")) {
        auto ids = bin->getAllClipIds();
        std::sort(ids.begin(), ids.end());
        for (const auto &id : ids)
            if (const auto clip = bin->getClipByBinID(id)) {
                const auto imported = bridge->importById.constFind(id);
                const QString owned = imported != bridge->importById.cend() && imported->clip.lock() == clip ? imported->path : QString();
                visit(id, [clip, owned, section] { return section == sk("sources") ? sourceRecord(clip, owned) : sourceProperties(clip); });
            }
    } else if (section == sk("bin")) {
        return walkBin(bin->getRootFolder(), [&visit](const auto &item, int row) { visit(item->clipId(), [item, row] { return binRecord(item, row); }); });
    } else if (section == sk("guides")) {
        const auto sequences = Mcp::liveSequenceClips(bin);
        for (auto it = sequences.begin(); it != sequences.end(); ++it) {
            const auto uuid = it.key();
            const auto source = bin->getClipByBinID(it.value());
            visit(uuid.toString(QUuid::WithoutBraces), [document, uuid, source] {
                QJsonObject value{{sk("sequenceId"), uuid.toString(QUuid::WithoutBraces)}};
                const auto model = document->getTimeline(uuid, true);
                const auto guides = model ? model->getGuideModel() : nullptr;
                value.insert(sk("loaded"), bool(guides));
                if (guides)
                    value.insert(sk("markersJson"), guides->toJson());
                else {
                    value.insert(sk("documentGuides"), document->getSequenceProperty(uuid, sk("guides")));
                    if (source && source->statusReady()) {
                        const auto markers = source->markerModel();
                        if (markers) value.insert(sk("markersJson"), markers->toJson());
                        value.insert(sk("producerGuides"), source->getProducerProperty(sk("kdenlive:sequenceproperties.guides")));
                    }
                }
                return value;
            });
        }
    } else if (section == sk("source_markers")) {
        auto ids = bin->getAllClipIds();
        std::sort(ids.begin(), ids.end());
        for (const auto &id : ids) {
            const auto source = bin->getClipByBinID(id);
            if (!source || !source->statusReady()) continue;
            const auto model = source->markerModel();
            if (!model) continue;
            for (const auto &marker : model->getAllMarkers()) {
                const int frame = marker.time().frames(pCore->getCurrentFps());
                const int markerId = model->markerIdAtFrame(frame);
                visit(id + QLatin1Char(':') + QString::number(markerId), [id, markerId, marker, frame] {
                    return QJsonObject{{sk("binId"), id},
                                       {sk("markerId"), markerId},
                                       {sk("frame"), frame},
                                       {sk("seconds"), marker.time().seconds()},
                                       {sk("durationSeconds"), marker.duration().seconds()},
                                       {sk("durationFrames"), marker.duration().frames(pCore->getCurrentFps())},
                                       {sk("categoryId"), marker.markerType()},
                                       {sk("comment"), marker.comment()}};
                });
            }
        }
    } else if (section == sk("imports")) {
        for (const auto &item : bridge->imports)
            visit(item.binId, [item, bin] { return importRecord(item, bin); });
    } else if (section == sk("groups")) {
        visit(sk("all"), [timeline] { return QJsonValue(timeline->groupsData()); });
    } else if (section == sk("selection")) {
        const auto selected = timeline->getCurrentSelection();
        std::vector<int> ids(selected.begin(), selected.end());
        std::sort(ids.begin(), ids.end());
        for (int id : ids)
            visit(QString::number(id), [id] { return QJsonValue(id); });
    } else if (section.startsWith(sk("subtitle"))) {
        const auto model = timeline->getSubtitleModel();
        if (!model) return true;
        if (section == sk("subtitles"))
            for (int id : subtitleOrder(model))
                visit(QString::number(id), [model, id] { return subtitleRecord(model, id); });
        else if (section == sk("subtitle_styles"))
            for (const auto &style : model->getAllSubtitleStyles())
                visit(style.first, [style] { return QJsonObject{{sk("name"), style.first}, {sk("assStyle"), style.second.toString(style.first)}}; });
        else if (section == sk("subtitle_languages")) {
            const auto languages = model->getSubtitlesList();
            for (auto it = languages.begin(); it != languages.end(); ++it) {
                const auto id = it.key().first;
                const auto name = it.key().second;
                const auto path = it.value();
                visit(QString::number(id), [id, name, path] { return QJsonObject{{sk("id"), id}, {sk("name"), name}, {sk("path"), path}}; });
            }
        } else if (section == sk("subtitle_info"))
            visit(sk("all"), [model, document] { return subtitleInfo(model, document); });
        else if (section == sk("subtitle_layers"))
            for (int layer = 0; layer <= model->getMaxLayer(); ++layer)
                visit(QString::number(layer), [model, layer] { return QJsonValue(model->getLayerDefaultStyle(layer)); });
        else
            return false;
    } else
        return false;
    return true;
}
QJsonObject compactHeader(QJsonObject state)
{
    if (state.contains(sk("selection"))) {
        state.insert(sk("selectionCount"), state.value(sk("selection")).toArray().size());
        state.insert(sk("selectionHash"), stateValueHash(state.take(sk("selection"))));
    }
    if (state.contains(sk("groups"))) state.insert(sk("groupsHash"), stateValueHash(state.take(sk("groups"))));
    if (state.contains(sk("documentMetadata"))) state.insert(sk("documentMetadataHash"), stateValueHash(state.take(sk("documentMetadata"))));
    for (const auto &field : {sk("tracks"), sk("bin"), sk("imports"), sk("subtitles"), sk("subtitleMetadata")})
        state.remove(field);
    QJsonObject omitted;
    const auto keys = state.keys();
    for (const auto &key : keys)
        if (state.value(key).isString() && state.value(key).toString().size() > 1024) {
            const auto value = state.value(key).toString();
            omitted.insert(key, QJsonObject{{sk("lengthUtf16"), value.size()}, {sk("hash"), stateValueHash(value)}});
            state.insert(key, value.left(1024));
        }
    if (!omitted.isEmpty()) state.insert(sk("truncatedFields"), omitted);
    state.insert(sk("compact"), true);
    state.insert(sk("projectionComplete"), false);
    state.insert(sk("pagingTool"), sk("get_state_page"));
    return state;
}
} // namespace

namespace McpState {
QJsonObject snapshot(QObject *owner, QJsonObject header, const std::shared_ptr<TimelineItemModel> &timeline, const std::shared_ptr<ProjectItemModel> &bin,
                     KdenliveDoc *document, const QList<ImportedSource> &imports, const ClipStateReader &clipStateReader)
{
    auto *bridge = stateBridge(owner);
    bridge->imports = imports;
    bridge->importById.clear();
    bridge->clipStateReader = clipStateReader;
    for (const auto &item : imports)
        bridge->importById.insert(item.binId, item);
    std::sort(bridge->imports.begin(), bridge->imports.end(), [](const auto &a, const auto &b) { return a.binId < b.binId; });
    QJsonObject documentMetadata;
    const auto metadata = document->metadata();
    for (auto it = metadata.cbegin(); it != metadata.cend(); ++it)
        documentMetadata.insert(it.key(), it.value());
    header.insert(sk("documentMetadata"), documentMetadata);
    header.insert(sk("documentNotes"), nativeDocumentNotes(document, bin));
    const auto &profile = pCore->getProjectProfile();
    header.insert(sk("profileData"), QJsonObject{{sk("width"), profile.width()},
                                                 {sk("height"), profile.height()},
                                                 {sk("frameRateNumerator"), profile.frame_rate_num()},
                                                 {sk("frameRateDenominator"), profile.frame_rate_den()},
                                                 {sk("sampleAspectNumerator"), profile.sample_aspect_num()},
                                                 {sk("sampleAspectDenominator"), profile.sample_aspect_den()},
                                                 {sk("displayAspectNumerator"), profile.display_aspect_num()},
                                                 {sk("displayAspectDenominator"), profile.display_aspect_den()},
                                                 {sk("progressive"), profile.progressive()},
                                                 {sk("colorspace"), profile.colorspace()}});
    RecordHash full, treeHash, guideHash;
    full.record(sk("header"), sk("all"), header);
    bool complete =
        timeline->getClipsCount() + timeline->getCompositionsCount() <= 512 && timeline->getTracksCount() <= 64 && bin->getAllClipIds().size() <= 512;
    quint64 used = quint64(McpWire::jsonEncodedSize(header, qsizetype(PublicStateBudget)));
    complete = complete && used <= PublicStateBudget;
    auto keep = [&](const QJsonValue &value) {
        if (!complete) return false;
        const auto cost = quint64(McpWire::jsonEncodedSize(value, qsizetype(PublicStateBudget)));
        if (cost > PublicStateBudget - qMin(used, PublicStateBudget)) {
            complete = false;
            return false;
        }
        used += cost;
        return true;
    };
    QJsonArray tracks, sources, imported, subtitles, styles, languages, layers;
    QMap<int, QJsonObject> trackViews;
    QMap<int, QJsonArray> trackClips, trackCompositions;
    QJsonObject counts;
    const QStringList sections{sk("tracks"),    sk("clips"),           sk("compositions"),       sk("sources"),         sk("bin"),          sk("imports"),
                               sk("subtitles"), sk("subtitle_styles"), sk("subtitle_languages"), sk("subtitle_layers"), sk("subtitle_info")};
    QJsonObject subtitleMetadata;
    for (const auto &section : sections) {
        int count = 0;
        const bool valid = records(section, bridge, timeline, bin, document, [&](const QString &id, const LazyValue &read) {
            const auto value = read();
            ++count;
            full.record(section, id, value);
            if (section == sk("bin")) {
                treeHash.record(section, id, value);
                return;
            }
            if (!keep(value)) return;
            auto item = value.toObject();
            if (section == sk("tracks"))
                trackViews.insert(item.value(sk("id")).toInt(), item);
            else if (section == sk("clips") || section == sk("compositions")) {
                const int track = item.take(sk("trackId")).toInt();
                if (section == sk("clips"))
                    trackClips[track].append(item);
                else
                    trackCompositions[track].append(item);
            } else if (section == sk("sources"))
                sources.append(value);
            else if (section == sk("imports"))
                imported.append(value);
            else if (section == sk("subtitles")) {
                item.remove(sk("id"));
                subtitles.append(item);
            } else if (section == sk("subtitle_styles"))
                styles.append(value);
            else if (section == sk("subtitle_languages"))
                languages.append(value);
            else if (section == sk("subtitle_layers"))
                layers.append(value);
            else if (section == sk("subtitle_info"))
                subtitleMetadata = item;
        });
        if (!valid) {
            return {{sk("ready"), false},
                    {sk("sessionId"), header.value(sk("sessionId"))},
                    {sk("reason"), sk("The native bin hierarchy contains a cycle or duplicated child; state was not issued.")}};
        }
        counts.insert(section, count);
        full.record(sk("sectionCount"), section, count);
    }
    const auto sequences = Mcp::liveSequenceClips(bin);
    const QStringList sequenceSections{sk("tracks"),          sk("clips"),           sk("compositions"),
                                       sk("subtitles"),       sk("subtitle_styles"), sk("subtitle_languages"),
                                       sk("subtitle_layers"), sk("subtitle_info")};
    int inactiveModels = 0;
    for (auto it = sequences.begin(); it != sequences.end(); ++it) {
        const auto properties = document->getSequenceProperties(it.key());
        for (auto property = properties.begin(); property != properties.end(); ++property)
            if (persistedSequenceProperty(property.key())) full.record(sk("sequenceProperties/") + it.key().toString(), property.key(), property.value());
        if (it.key() == timeline->uuid()) continue;
        const auto sequence = document->getTimeline(it.key(), true);
        const auto identity = it.key().toString(QUuid::WithoutBraces);
        if (!sequence) {
            const auto graph = sequenceGraphHash(bin->getClipByBinID(it.value()));
            if (graph.hash.isEmpty())
                return {{sk("ready"), false},
                        {sk("sessionId"), header.value(sk("sessionId"))},
                        {sk("reason"), graph.reason},
                        {sk("sequenceId"), identity},
                        {sk("binId"), it.value()},
                        {sk("limitation"), sk("UNBUILT_SEQUENCE_GRAPH_UNVERIFIED")},
                        {sk("recovery"), sk("Inspect list_sequences/get_bin, then open this sequence from the native bin using the UI bridge to build its "
                                            "timeline model. Read get_state again. Native read-only and UI inspection remain available.")}};
            full.record(sk("unbuiltSequence"), identity, graph.hash);
            continue;
        }
        ++inactiveModels;
        full.record(sk("sequenceHeader"), identity,
                    QJsonObject{{sk("uuid"), identity},
                                {sk("durationFrames"), sequence->duration()},
                                {sk("groups"), sequence->groupsData()},
                                {sk("masterEffectsHash"), stateEffectsHash(sequence->getMasterEffectStackModel())}});
        for (const auto &section : sequenceSections) {
            int count = 0;
            records(section, bridge, sequence, bin, document, [&](const QString &id, const LazyValue &read) {
                ++count;
                full.record(sk("sequence/") + identity + QLatin1Char('/') + section, id, read());
            });
            full.record(sk("sequenceCount/") + identity, section, count);
        }
    }
    counts.insert(sk("sequences"), sequences.size());
    counts.insert(sk("inactiveLoadedSequences"), inactiveModels);
    for (auto it = sequences.begin(); it != sequences.end(); ++it) {
        const auto sequence = document->getTimeline(it.key(), true);
        const auto guides = sequence ? sequence->getGuideModel() : nullptr;
        QJsonObject value;
        if (guides)
            value.insert(sk("markersHash"), markerHash(guides));
        else {
            value.insert(sk("loaded"), false);
            value.insert(sk("documentGuides"), document->getSequenceProperty(it.key(), sk("guides")));
            const auto source = bin->getClipByBinID(it.value());
            if (source && source->statusReady()) {
                value.insert(sk("binMarkersHash"), markerHash(source->markerModel()));
                value.insert(sk("producerGuides"), source->getProducerProperty(sk("kdenlive:sequenceproperties.guides")));
            }
        }
        guideHash.record(sk("guide"), it.key().toString(), value);
    }
    guideHash.record(sk("activeGuide"), timeline->uuid().toString(), markerHash(timeline->getGuideModel()));
    full.record(sk("guides"), sk("all"), guideHash.result());
    QJsonArray categories;
    for (auto it = pCore->markerTypes.cbegin(); it != pCore->markerTypes.cend(); ++it) {
        const QJsonObject value{{sk("id"), it.key()}, {sk("name"), it.value().displayName}, {sk("color"), it.value().color.name(QColor::HexArgb)}};
        full.record(sk("markerCategory"), QString::number(it.key()), value);
        categories.append(value);
    }
    header.insert(sk("binTreeHash"), treeHash.result());
    header.insert(sk("guidesHash"), guideHash.result());
    header.insert(sk("markerCategoriesHash"), stateValueHash(categories));
    header.insert(sk("counts"), counts);
    header.insert(sk("fingerprintScope"), sk("whole_project"));
    header.insert(sk("viewScope"), sk("active_sequence_and_project_bin"));
    header.insert(sk("nativeStateHash"), full.result());
    header.insert(sk("expectedState"), full.result());
    bridge->header = header;
    // New summary fields are projection metadata. The fingerprint above has
    // already consumed every complete native value, not their shortened views.
    if (complete) {
        for (int track : trackOrder(timeline)) {
            auto value = trackViews.value(track);
            value.insert(sk("clips"), trackClips.value(track));
            value.insert(sk("compositions"), trackCompositions.value(track));
            tracks.append(value);
        }
        header.insert(sk("tracks"), tracks);
        header.insert(sk("bin"), sources);
        header.insert(sk("imports"), imported);
        if (timeline->hasSubtitleModel()) {
            subtitleMetadata.insert(sk("styles"), styles);
            subtitleMetadata.insert(sk("languageTracks"), languages);
            subtitleMetadata.insert(sk("defaultStyles"), layers);
            header.insert(sk("subtitles"), subtitles);
            header.insert(sk("subtitleMetadata"), subtitleMetadata);
        }
        header.insert(sk("compact"), false);
        header.insert(sk("projectionComplete"), true);
    } else
        header = compactHeader(header);
    return header;
}
bool binItems(const std::shared_ptr<AbstractProjectItem> &parent, bool recursive, std::vector<std::shared_ptr<AbstractProjectItem>> &items)
{
    if (!parent) return false;
    if (!recursive) {
        QSet<const AbstractProjectItem *> seen;
        for (const auto &entry : parent->directChildren()) {
            const auto child = std::static_pointer_cast<AbstractProjectItem>(entry);
            if (!child || child == parent || seen.contains(child.get())) return false;
            seen.insert(child.get());
            items.push_back(child);
        }
        return true;
    }
    return walkBin(parent, [&items, parent](const auto &item, int) {
        if (item != parent) items.push_back(item);
    });
}
QJsonArray toolCatalog()
{
    const auto text = [](const char *s) { return Mcp::property(sk("string"), sk(s)); };
    const auto integer = [](const char *s) { return Mcp::property(sk("integer"), sk(s)); };
    return {Mcp::tool(
        sk("get_state_page"),
        sk("Read a bounded, token-bound page of exact native records. Track/clip/composition/subtitle/group views belong to the explicitly returned active "
           "sequence; bin/source/guide views belong to the project. Large fields are explicit descriptors; select recordId and a JSON-pointer path "
           "to page their full values. This never constructs another timeline or changes native state."),
        {{sk("expectedState"), text("Exact token from get_state; every page is bound to that document/sequence/native state.")},
         {sk("section"), text("header, tracks, clips, compositions, sources, source_properties, bin, imports, guides, source_markers, groups, selection, "
                              "subtitles, subtitle_styles, subtitle_languages, subtitle_layers, or subtitle_info.")},
         {sk("recordId"),
          text("Optional exact record ID. header/groups/subtitle_info use all; source_markers use binId:markerId. Select this before paging a nested path.")},
         {sk("path"), text("Optional RFC6901 JSON pointer within the selected record; no methods or property setters are evaluated.")},
         {sk("offset"), integer("First row or UTF-16 text offset, default zero.")},
         {sk("limit"), integer("Directory/collection rows1..128 (default64); selected text fields accept1..8192 UTF-16 units.")}},
        {sk("expectedState"), sk("section")}, true)};
}
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState,
                     const std::function<QJsonObject()> &stateReader)
{
    Q_UNUSED(stateReader)
    if (name != sk("get_state_page")) return {};
    auto fail = [&](const char *code, const char *message) { return Mcp::failure(sk(code), sk(message), nativeState); };
    if (!nativeState.value(sk("ready")).toBool()) return fail("NOT_READY", "Wait for the native project to become ready.");
    if (!arguments.value(sk("expectedState")).isString() || arguments.value(sk("expectedState")) != nativeState.value(sk("expectedState")))
        return fail("STALE_STATE", "Read get_state again; pages cannot be combined across native changes.");
    const QString section = arguments.value(sk("section")).toString(), recordId = arguments.value(sk("recordId")).toString();
    const QString path = arguments.value(sk("path")).toString();
    int offset = 0, limit = 64;
    const QStringList allowed{sk("header"),
                              sk("tracks"),
                              sk("clips"),
                              sk("compositions"),
                              sk("sources"),
                              sk("source_properties"),
                              sk("bin"),
                              sk("imports"),
                              sk("guides"),
                              sk("source_markers"),
                              sk("groups"),
                              sk("selection"),
                              sk("subtitles"),
                              sk("subtitle_styles"),
                              sk("subtitle_languages"),
                              sk("subtitle_layers"),
                              sk("subtitle_info")};
    if (!allowed.contains(section)) return fail("INVALID_SECTION", "Use one of the advertised native sections.");
    if (recordId.size() > 256 || path.size() > 4096 || (!arguments.value(sk("recordId")).isUndefined() && !arguments.value(sk("recordId")).isString()) ||
        (!arguments.value(sk("path")).isUndefined() && !arguments.value(sk("path")).isString()) || (!path.isEmpty() && recordId.isEmpty()) ||
        (arguments.contains(sk("offset")) && !Mcp::integer(arguments, sk("offset"), 0, std::numeric_limits<int>::max(), offset)) ||
        (arguments.contains(sk("limit")) && !Mcp::integer(arguments, sk("limit"), 1, recordId.isEmpty() ? 128 : 8192, limit)))
        return fail("INVALID_ARGUMENT", "Use bounded page arguments; nested paths require an exact recordId.");
    if (!pCore || !pCore->currentDoc() || !pCore->projectManager()) return fail("NOT_READY", "No native document is available.");
    const auto timeline = pCore->projectManager()->getTimeline();
    const auto bin = pCore->projectItemModel();
    if (!timeline || !bin) return fail("NOT_READY", "No native timeline or bin is available.");
    auto *bridge = stateBridge(owner);
    QJsonArray page;
    int total = 0;
    QJsonValue selected;
    bool found = false;
    bool pageFull = false;
    quint64 used = 0;
    const bool valid = records(section, bridge, timeline, bin, pCore->currentDoc(), [&](const QString &id, const LazyValue &read) {
        const int row = total++;
        if (!recordId.isEmpty()) {
            if (id == recordId) {
                selected = read();
                found = true;
            }
            return;
        }
        if (pageFull || row < offset || page.size() >= limit) return;
        const auto value = read();
        QJsonObject entry{{sk("recordId"), id}};
        if (McpWire::jsonEncodedSize(value, 8192) <= 8192)
            entry.insert(sk("value"), value);
        else {
            entry.insert(sk("valueOmitted"), true);
            entry.insert(sk("preview"), McpWire::pageValue(value, {}, 0, 16, 8192));
            entry.insert(sk("readUsingRecordId"), id);
        }
        const auto cost = quint64(McpWire::jsonEncodedSize(entry, qsizetype(PageBudget)));
        if (cost > PageBudget - qMin(used, PageBudget)) {
            pageFull = true;
            return;
        }
        page.append(entry);
        used += cost;
    });
    if (!valid) return fail("INVALID_SECTION", "Use an advertised native section; the bin hierarchy must be valid.");
    QJsonObject result;
    if (!recordId.isEmpty()) {
        if (!found) return fail("INVALID_RECORD", "This exact native record ID is absent in the selected state.");
        result = McpWire::pageValue(selected, path, offset, limit, McpWire::MaxPageBytes);
        if (!result.value(sk("ok")).toBool()) return Mcp::toolResult(result, true);
        result.insert(sk("recordId"), recordId);
    } else {
        result = {{sk("ok"), true},
                  {sk("items"), page},
                  {sk("total"), total},
                  {sk("offset"), offset},
                  {sk("nextOffset"), offset + page.size()},
                  {sk("hasMore"), qint64(offset) + page.size() < total}};
    }
    result.insert(sk("section"), section);
    result.insert(sk("expectedState"), nativeState.value(sk("expectedState")));
    result.insert(sk("documentId"), nativeState.value(sk("documentId")));
    result.insert(sk("sequenceId"), nativeState.value(sk("sequenceId")));
    result.insert(sk("state"), compactHeader(nativeState));
    return Mcp::toolResult(result);
}
} // namespace McpState
