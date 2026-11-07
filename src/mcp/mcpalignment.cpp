/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "bin/model/subtitlemodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "kdenlivesettings.h"
#include "mcpaudiomatch.h"
#include "mcphelpers.h"
#include "mcpserver.h"
#include "project/projectmanager.h"
#include "timeline2/model/clipmodel.hpp"
#include "timeline2/model/groupsmodel.hpp"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/model/trackmodel.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>

namespace {
constexpr int MaxFrame = 10000000;
constexpr int MaxInputs = 17;
constexpr int MaxAnalysisFrames = 30000;
constexpr qint64 MaxDecodedBytes = 512 * 1024 * 1024;
constexpr int SampleRate = 48000;
QString k(const char *value)
{
    return QString::fromLatin1(value);
}
QJsonObject text(const char *description)
{
    return Mcp::property(k("string"), k(description));
}
QJsonObject integer(const char *description)
{
    return Mcp::property(k("integer"), k(description));
}
QJsonObject boolean(const char *description)
{
    return Mcp::property(k("boolean"), k(description));
}
bool terminal(const QString &status)
{
    return status == k("ready") || status == k("failed") || status == k("cancelled") || status == k("obsolete") || status == k("applied");
}
QJsonObject fileIdentity(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable() || info.canonicalFilePath() != path) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(file.read(65536));
    if (file.size() > 65536) {
        file.seek(qMax<qint64>(0, file.size() - 65536));
        hash.addData(file.read(65536));
    }
    return {{k("path"), path},
            {k("size"), QString::number(info.size())},
            {k("modifiedUtc"), info.lastModified().toUTC().toString(Qt::ISODateWithMs)},
            {k("edgeSha256"), QString::fromLatin1(hash.result().toHex())}};
}
struct Input
{
    int clipId{-1}, root{-1}, trackId{-1}, position{0}, sourceIn{0}, duration{0}, streamId{-1}, audioOrdinal{-1}, timecode{-1};
    qint64 decodedSamples{0};
    int decodedFrames{0}, decoderExitCode{std::numeric_limits<int>::min()};
    bool decoderCrashed{false};
    QString binId, path;
    QJsonObject identity;
    std::vector<qint64> envelope;
    QJsonObject json() const
    {
        return {{k("clipId"), clipId},
                {k("groupRootId"), root},
                {k("trackId"), trackId},
                {k("positionFrame"), position},
                {k("sourceInFrame"), sourceIn},
                {k("durationFrames"), duration},
                {k("binId"), binId},
                {k("streamId"), streamId},
                {k("ffmpegAudioOrdinal"), audioOrdinal},
                {k("timecodeFrame"), timecode},
                {k("sourceIdentity"), identity},
                {k("decodedSamples"), double(decodedSamples)},
                {k("decodedFrames"), decodedFrames},
                {k("decoderExitCode"), decoderExitCode == std::numeric_limits<int>::min() ? QJsonValue(QJsonValue::Null) : QJsonValue(decoderExitCode)},
                {k("decoderCrashed"), decoderCrashed}};
    }
};
struct MatchResults
{
    std::vector<McpAudioMatch::Result> values;
};

class AlignmentJob : public QObject
{
public:
    explicit AlignmentJob(QObject *parent)
        : QObject(parent)
        , process(this)
        , deadline(this)
        , watcher(this)
    {
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, this, [this] { failProcess(k("Audio decoding exceeded its 60-second deadline.")); });
        connect(&process, &QProcess::readyReadStandardOutput, this, [this] { readAudio(); });
        connect(&process, &QProcess::readyReadStandardError, this, [this] {
            const auto bytes = process.readAllStandardError();
            diagnostic += QString::fromUtf8(bytes);
            diagnostic = diagnostic.right(8192);
        });
        connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) finish(k("failed"), k("The configured local FFmpeg decoder could not start."));
        });
        connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus exit) {
            deadline.stop();
            readAudio();
            if (index < int(inputs.size())) {
                inputs[index].decoderExitCode = code;
                inputs[index].decoderCrashed = exit != QProcess::NormalExit;
                inputs[index].decodedSamples = consumedSamples;
                inputs[index].decodedFrames = frameIndex;
            }
            if (cancelled->load()) {
                finish(k("cancelled"), k("Owned decoder stopped; no timeline edits were made."));
                return;
            }
            if (!pendingFailure.isEmpty()) {
                finish(k("failed"), pendingFailure);
                return;
            }
            if (exit != QProcess::NormalExit || code != 0) {
                finish(k("failed"), k("Audio decoder failed (exit ") + QString::number(code) + k(")."));
                return;
            }
            if (status != k("decoding")) return;
            auto &input = inputs[index];
            if (!sourcesUnchanged()) {
                finish(k("obsolete"), k("A captured source changed during decoding."));
                return;
            }
            // A single sample of end rounding is allowed, never an absent frame.
            if (frameIndex == input.duration - 1 && frameSamples > 0 && expectedSamples - consumedSamples <= 1) completeFrame();
            input.decodedSamples = consumedSamples;
            input.decodedFrames = frameIndex;
            if (!pending.isEmpty() || frameIndex != input.duration || std::abs(consumedSamples - expectedSamples) > 1) {
                finish(k("failed"), k("Decoded audio does not cover the exact visible source window."));
                return;
            }
            ++index;
            QTimer::singleShot(0, this, [this] { decodeNext(); });
        });
        connect(&watcher, &QFutureWatcher<MatchResults>::finished, this, [this] {
            if (cancelled->load()) {
                finish(k("cancelled"), k("Owned correlation worker stopped; no timeline edits were made."));
                return;
            }
            if (!sourcesUnchanged()) {
                finish(k("obsolete"), k("A captured source changed during correlation."));
                return;
            }
            {
                const auto results = watcher.result();
                for (size_t target = 0; target < results.values.size(); ++target) {
                    const auto &result = results.values[target];
                    if (!result.ok) {
                        finish(k("failed"), result.cancelled ? k("Correlation cancelled.") : result.error);
                        return;
                    }
                    const auto &input = inputs[target + 1];
                    const qint64 proposed = qint64(inputs.front().position) + result.lag;
                    proposals.append(QJsonObject{{k("clipId"), input.clipId},
                                                 {k("groupRootId"), input.root},
                                                 {k("fromFrame"), input.position},
                                                 {k("proposedFrame"), double(proposed)},
                                                 {k("deltaFrames"), double(proposed - input.position)},
                                                 {k("lagFrames"), result.lag},
                                                 {k("overlapFrames"), result.overlap},
                                                 {k("correlation"), result.correlation},
                                                 {k("runnerUpCorrelation"), result.runnerUp},
                                                 {k("confidenceMargin"), result.correlation - result.runnerUp},
                                                 {k("requiresAcknowledgement"), result.weak}});
                }
                finish(k("ready"), k("Proposals are ready. Inspect them, then explicitly apply against the captured project state."));
            }
            for (auto &input : inputs) {
                input.envelope.clear();
                input.envelope.shrink_to_fit();
            }
        });
    }
    ~AlignmentJob() override
    {
        cancelled->store(true);
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(1000);
        }
        if (watcher.isRunning()) watcher.waitForFinished();
    }
    QString id{QUuid::createUuid().toString(QUuid::WithoutBraces)}, mode, capturedState, documentId, sequenceId, executable;
    QString status{k("queued")}, message, diagnostic, pendingFailure;
    std::vector<Input> inputs;
    QJsonArray proposals;
    int numerator{25}, denominator{1}, maxShift{30000}, minimumOverlap{8}, index{0};
    bool includeGroups{false}, obsoleteRequested{false}, appliedChanges{false};
    QProcess process;
    QTimer deadline;
    QFutureWatcher<MatchResults> watcher;
    std::shared_ptr<std::atomic_bool> cancelled{std::make_shared<std::atomic_bool>(false)};
    QByteArray pending;
    qint64 consumedSamples{0}, expectedSamples{0}, decodedBytes{0}, frameSum{0};
    int frameIndex{0}, frameSamples{0};
    qint64 sampleBoundary(int frame) const { return qint64(frame) * SampleRate * denominator / numerator; }
    bool sourcesUnchanged() const
    {
        for (const auto &input : inputs)
            if (!input.identity.isEmpty() && fileIdentity(input.path) != input.identity) return false;
        return true;
    }
    void finish(const QString &value, const QString &reason = {})
    {
        status = value == k("cancelled") && obsoleteRequested ? k("obsolete") : value;
        message = obsoleteRequested ? k("Captured project or source changed; owned work has stopped.") : reason;
        deadline.stop();
    }
    void cancel()
    {
        if (status == k("applied") || status == k("failed") || status == k("cancelled") || status == k("obsolete")) return;
        cancelled->store(true);
        if (process.state() != QProcess::NotRunning) {
            status = k("cancelling");
            process.kill();
        } else if (watcher.isRunning())
            status = k("cancelling");
        else
            finish(k("cancelled"), k("No timeline edits were made."));
    }
    void failProcess(const QString &reason)
    {
        pendingFailure = reason;
        if (process.state() != QProcess::NotRunning)
            process.kill();
        else
            finish(k("failed"), reason);
    }
    void completeFrame()
    {
        inputs[index].envelope.push_back(frameSum / qMax(1, frameSamples));
        ++frameIndex;
        frameSamples = 0;
        frameSum = 0;
    }
    void readAudio()
    {
        while (process.bytesAvailable() > 0) {
            const auto bytes = process.read(32768);
            decodedBytes += bytes.size();
            if (decodedBytes > MaxDecodedBytes || consumedSamples * 2 + pending.size() + bytes.size() > expectedSamples * 2 + 2) {
                failProcess(k("Audio decoder exceeded the captured output bound."));
                return;
            }
            pending += bytes;
            int offset = 0;
            while (offset + 1 < pending.size()) {
                const qint16 sample = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(pending.constData() + offset));
                offset += 2;
                if (frameIndex >= inputs[index].duration) {
                    ++consumedSamples;
                    continue;
                }
                frameSum += std::abs(int(sample));
                ++frameSamples;
                ++consumedSamples;
                const auto end = sampleBoundary(inputs[index].sourceIn + frameIndex + 1) - sampleBoundary(inputs[index].sourceIn);
                if (consumedSamples == end) completeFrame();
            }
            pending.remove(0, offset);
        }
    }
    void decodeNext()
    {
        if (cancelled->load() || terminal(status)) return;
        if (!sourcesUnchanged()) {
            finish(k("obsolete"), k("A captured source changed before decoding."));
            return;
        }
        if (index == int(inputs.size())) {
            status = k("correlating");
            // Worker inputs contain values only, never pCore, models or QObjects.
            std::vector<std::vector<qint64>> envelopes;
            for (auto &input : inputs)
                envelopes.push_back(std::move(input.envelope));
            auto stop = cancelled;
            const int shift = maxShift, overlap = minimumOverlap;
            watcher.setFuture(QtConcurrent::run([values = std::move(envelopes), stop, shift, overlap] {
                MatchResults results;
                for (size_t target = 1; target < values.size(); ++target) {
                    if (stop->load()) break;
                    results.values.push_back(McpAudioMatch::match(values.front(), values[target], shift, overlap, stop));
                }
                return results;
            }));
            return;
        }
        status = k("decoding");
        pending.clear();
        consumedSamples = 0;
        frameIndex = 0;
        frameSamples = 0;
        frameSum = 0;
        const auto &input = inputs[index];
        const auto firstSample = sampleBoundary(input.sourceIn);
        expectedSamples = sampleBoundary(input.sourceIn + input.duration) - firstSample;
        const QString seek = QString::number(double(firstSample) / SampleRate, 'f', 9);
        const QString duration = QString::number(double(expectedSamples) / SampleRate, 'f', 9);
        process.setProgram(executable);
        process.setArguments({k("-nostdin"),
                              k("-hide_banner"),
                              k("-v"),
                              k("error"),
                              k("-protocol_whitelist"),
                              k("file,pipe"),
                              k("-ss"),
                              seek,
                              k("-i"),
                              input.path,
                              k("-map"),
                              k("0:a:") + QString::number(input.audioOrdinal),
                              k("-vn"),
                              k("-sn"),
                              k("-dn"),
                              k("-ac"),
                              k("1"),
                              k("-ar"),
                              QString::number(SampleRate),
                              k("-t"),
                              duration,
                              k("-f"),
                              k("s16le"),
                              k("pipe:1")});
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start(QIODevice::ReadOnly);
        deadline.start(60000);
    }
    void start()
    {
        if (status != k("queued") || cancelled->load()) return;
        if (mode == k("timecode")) {
            const auto &reference = inputs.front();
            for (size_t target = 1; target < inputs.size(); ++target) {
                const auto &input = inputs[target];
                const qint64 lag = qint64(input.timecode) + input.sourceIn - reference.timecode - reference.sourceIn;
                if (std::abs(lag) > maxShift) {
                    finish(k("failed"), k("A native timecode offset exceeds maxShiftFrames."));
                    return;
                }
                const qint64 proposed = qint64(reference.position) + lag;
                proposals.append(QJsonObject{{k("clipId"), input.clipId},
                                             {k("groupRootId"), input.root},
                                             {k("fromFrame"), input.position},
                                             {k("proposedFrame"), double(proposed)},
                                             {k("deltaFrames"), double(proposed - input.position)},
                                             {k("lagFrames"), double(lag)},
                                             {k("requiresAcknowledgement"), false}});
            }
            finish(k("ready"), k("Native timecode proposals are ready; no timeline edits were made."));
            return;
        }
        decodeNext();
    }
    QJsonObject json() const
    {
        QJsonArray provenance;
        for (const auto &input : inputs)
            provenance.append(input.json());
        return {{k("jobId"), id},
                {k("mode"), mode},
                {k("status"), status},
                {k("message"), message},
                {k("capturedExpectedState"), capturedState},
                {k("documentId"), documentId},
                {k("sequenceId"), sequenceId},
                {k("includeGroups"), includeGroups},
                {k("inputs"), provenance},
                {k("proposals"), proposals},
                {k("decodedBytes"), double(decodedBytes)},
                {k("decodeInputIndex"), index},
                {k("decoderRunning"), process.state() != QProcess::NotRunning},
                {k("decoderProcessId"), QString::number(process.processId())},
                {k("workerRunning"), watcher.isRunning()},
                {k("diagnosticTail"), diagnostic},
                {k("timelineEdited"), appliedChanges}};
    }
};

class AlignmentService : public QObject
{
public:
    explicit AlignmentService(QObject *owner)
        : QObject(owner)
    {
        setObjectName(k("nativeMcpAlignmentService"));
    }
    QHash<QString, AlignmentJob *> jobs;
    QList<QString> order;
    bool reserve()
    {
        for (auto *job : jobs)
            if (!terminal(job->status)) return false;
        while (order.size() >= 32) {
            const auto id = order.takeFirst();
            auto *job = jobs.take(id);
            delete job;
        }
        return true;
    }
};
AlignmentService *service(QObject *owner)
{
    for (auto *child : owner->children())
        if (child->objectName() == k("nativeMcpAlignmentService")) return static_cast<AlignmentService *>(child);
    return new AlignmentService(owner);
}
} // namespace

QJsonArray KdenliveMcpServer::alignmentToolCatalog()
{
    QJsonObject member{{k("type"), k("object")},
                       {k("additionalProperties"), false},
                       {k("properties"), QJsonObject{{k("clipId"), integer("Native timeline clip ID.")},
                                                     {k("streamId"), integer("Explicit native source audio stream ID; required for audio mode.")}}},
                       {k("required"), QJsonArray{k("clipId")}}};
    auto targets = Mcp::property(k("array"), k("1-16 explicit target clips; reference is separate."));
    targets.insert(k("items"), member);
    targets.insert(k("minItems"), 1);
    targets.insert(k("maxItems"), 16);
    return {
        Mcp::tool(k("start_alignment_job"), k("Analyze finite local source windows using audio or native timecode. Returns proposals only; never moves clips."),
                  {{k("expectedState"), text("Fresh native expectedState captured with immutable analysis inputs.")},
                   {k("mode"), text("audio or timecode.")},
                   {k("reference"), member},
                   {k("targets"), targets},
                   {k("includeGroups"), boolean("Explicitly permit moving each target's complete native group when applying.")},
                   {k("maxShiftFrames"), integer("Audio lag bound 0-30000; timecode bound 0-10000000. Defaults to the mode's maximum.")},
                   {k("minimumOverlapFrames"), integer("Audio only: minimum nonconstant overlap, 8-30000. Defaults to half the shortest window, at least 8.")}},
                  {k("expectedState"), k("mode"), k("reference"), k("targets"), k("includeGroups")}),
        Mcp::tool(k("alignment_job_status"), k("Inspect owned alignment proposals, exact source provenance and explicit worker/decoder completion."),
                  {{k("jobId"), text("Optional owned job ID; omit to list retained jobs.")}}, {}, true),
        Mcp::tool(k("cancel_alignment_job"), k("Cancel only this session's decoder/correlation worker. No timeline changes occur."),
                  {{k("jobId"), text("Owned alignment job ID.")}}, {k("jobId")}),
        Mcp::tool(k("apply_alignment"), k("Apply compatible reviewed proposals with one native undo transaction, preserving complete target groups."),
                  {{k("expectedState"), text("Must equal the captured state and the current state.")},
                   {k("jobId"), text("Owned ready alignment job ID.")},
                   {k("acknowledgeWeakEvidence"), boolean("Explicitly accept weak or ambiguous audio evidence reported in the proposal.")}},
                  {k("expectedState"), k("jobId")})};
}

QJsonObject KdenliveMcpServer::callAlignmentTool(const QString &name, const QJsonObject &args, const QJsonObject &state)
{
    if (name != k("start_alignment_job") && name != k("alignment_job_status") && name != k("cancel_alignment_job") && name != k("apply_alignment")) return {};
    auto *store = service(this);
    const auto reject = [&](const char *code, const QString &reason) { return Mcp::failure(k(code), reason, snapshot()); };
    const auto answer = [&](AlignmentJob *job) { return Mcp::toolResult({{k("ok"), true}, {k("job"), job->json()}, {k("state"), snapshot()}}); };
    const QString id = args.value(k("jobId")).toString();
    const auto inputsUnchanged = [&](AlignmentJob *job) {
        const auto model = m_timeline.lock();
        if (!model || !state.value(k("ready")).toBool() || !job->sourcesUnchanged() || !pCore || !pCore->projectItemModel()) return false;
        for (const auto &input : job->inputs) {
            if (!model->isClip(input.clipId) || model->getClipBinId(input.clipId) != input.binId || model->getClipPosition(input.clipId) != input.position ||
                model->getClipTrackId(input.clipId) != input.trackId || model->getClipIn(input.clipId) != input.sourceIn ||
                model->getClipPlaytime(input.clipId) != input.duration)
                return false;
            const auto source = pCore->projectItemModel()->getClipByBinID(input.binId);
            if (!source || !source->statusReady()) return false;
            if (job->mode == k("timecode") && source->getStartTimecode() != input.timecode) return false;
            if (job->mode == k("audio") &&
                (!source->audioStreams().contains(input.streamId) || source->getAudioStreamFfmpegIndex(input.streamId) != input.audioOrdinal))
                return false;
        }
        return true;
    };
    if (name == k("alignment_job_status")) {
        const auto refresh = [&](AlignmentJob *job) {
            if ((job->status == k("ready") || !terminal(job->status)) && (job->capturedState != state.value(k("expectedState")) || !inputsUnchanged(job))) {
                job->obsoleteRequested = true;
                job->cancel();
                // A running owned process/worker must actually finish before becoming terminal.
                if (job->process.state() == QProcess::NotRunning && !job->watcher.isRunning())
                    job->finish(k("obsolete"), k("Captured project or source changed."));
                else
                    job->message = k("Captured project changed; owned work is cancelling.");
            }
        };
        if (id.isEmpty()) {
            QJsonArray jobs;
            for (const auto &key : store->order) {
                auto *job = store->jobs[key];
                refresh(job);
                jobs.append(job->json());
            }
            return Mcp::toolResult({{k("ok"), true}, {k("jobs"), jobs}, {k("state"), state}});
        }
        if (!store->jobs.contains(id)) return reject("INVALID_JOB", k("Use an alignment job ID returned by this MCP session."));
        auto *job = store->jobs[id];
        refresh(job);
        return answer(job);
    }
    if (name == k("cancel_alignment_job")) {
        if (!store->jobs.contains(id)) return reject("INVALID_JOB", k("Use an owned alignment job ID."));
        store->jobs[id]->cancel();
        return answer(store->jobs[id]);
    }
    const auto timeline = m_timeline.lock();
    if (!timeline || !state.value(k("ready")).toBool()) return reject("NOT_READY", k("No native timeline is ready."));
    if (args.value(k("expectedState")) != state.value(k("expectedState"))) return reject("STALE_STATE", k("Read current native state first."));
    if (!timeline->normalEdit() || !timeline->getCurrentSelection().empty() || timeline->singleSelectionMode())
        return reject("UI_BUSY", k("Use normal edit mode and clear transient timeline selection before alignment."));
    const auto rootOf = [&](int clip) { return timeline->m_groups->isInGroup(clip) ? timeline->m_groups->getRootId(clip) : clip; };
    if (name == k("start_alignment_job")) {
        const QString mode = args.value(k("mode")).toString();
        const auto targetValues = args.value(k("targets")).toArray();
        if ((mode != k("audio") && mode != k("timecode")) || !args.value(k("includeGroups")).isBool() || !args.value(k("reference")).isObject() ||
            !args.value(k("targets")).isArray() || targetValues.isEmpty() || targetValues.size() >= MaxInputs)
            return reject("INVALID_ARGUMENT", k("Specify audio/timecode, a reference, 1-16 targets and includeGroups."));
        if (mode == k("timecode") && args.contains(k("minimumOverlapFrames")))
            return reject("INVALID_ARGUMENT", k("Overlap is only an audio-mode constraint."));
        const bool groups = args.value(k("includeGroups")).toBool();
        std::vector<Input> inputs;
        QString reason;
        QSet<int> ids;
        int shortest = MaxAnalysisFrames;
        qint64 totalBytes = 0;
        const int numerator = pCore->getProjectProfile().frame_rate_num(), denominator = pCore->getProjectProfile().frame_rate_den();
        if (numerator <= 0 || denominator <= 0 ||
            (mode == k("audio") &&
             (numerator > 500000 || denominator > 10000 || double(numerator) / denominator < .1 || double(numerator) / denominator > SampleRate)))
            return reject("UNSUPPORTED_PROFILE", k("Audio alignment requires a frame rate between 0.1 and 48000 fps."));
        const auto capture = [&](const QJsonObject &value) {
            Input input;
            for (auto it = value.begin(); it != value.end(); ++it)
                if (it.key() != k("clipId") && it.key() != k("streamId")) {
                    reason = k("Unknown alignment member field.");
                    return false;
                }
            if (!Mcp::integer(value, k("clipId"), 0, MaxFrame, input.clipId) || !timeline->isClip(input.clipId) || ids.contains(input.clipId)) {
                reason = k("Use unique existing native timeline clip IDs.");
                return false;
            }
            ids.insert(input.clipId);
            input.root = rootOf(input.clipId);
            input.trackId = timeline->getClipTrackId(input.clipId);
            input.position = timeline->getClipPosition(input.clipId);
            input.sourceIn = timeline->getClipIn(input.clipId);
            input.duration = timeline->getClipPlaytime(input.clipId);
            input.binId = timeline->getClipBinId(input.clipId);
            const auto source = pCore->projectItemModel()->getClipByBinID(input.binId);
            if (!source || !source->statusReady() || input.trackId < 0 || input.position < 0 || input.position > MaxFrame || input.duration <= 0 ||
                input.sourceIn < 0 || input.sourceIn > MaxFrame || qint64(input.sourceIn) + input.duration > MaxFrame ||
                timeline->getClipSpeed(input.clipId) != 1.0 || timeline->getClipPtr(input.clipId)->hasTimeRemap()) {
                reason = k("Alignment needs ready finite unretimed source windows on native tracks.");
                return false;
            }
            if (mode == k("timecode")) {
                if (value.contains(k("streamId"))) {
                    reason = k("Timecode alignment does not accept streamId.");
                    return false;
                }
                input.timecode = source->getStartTimecode();
                // getStartTimecode returns source-zero timecode, independently of visible in.
                if (input.timecode < 0) {
                    reason = k("A source has no valid native timecode.");
                    return false;
                }
            } else {
                if (!source->hasAudio() || !Mcp::integer(value, k("streamId"), 0, 1024, input.streamId) || !source->audioStreams().contains(input.streamId) ||
                    (input.audioOrdinal = source->getAudioStreamFfmpegIndex(input.streamId)) < 0 || input.duration < 8 || input.duration > MaxAnalysisFrames) {
                    reason = k("Choose actual source audio streams and windows of 8-30000 frames.");
                    return false;
                }
                input.path = QFileInfo(source->url()).canonicalFilePath();
                input.identity = fileIdentity(input.path);
                if (input.identity.isEmpty()) {
                    reason = k("Audio matching accepts readable ordinary local source files only.");
                    return false;
                }
                shortest = std::min(shortest, input.duration);
                const qint64 samples = (qint64(input.sourceIn) + input.duration) * SampleRate * denominator / numerator -
                                       qint64(input.sourceIn) * SampleRate * denominator / numerator;
                totalBytes += samples * 2;
                if (totalBytes > MaxDecodedBytes) {
                    reason = k("The captured analysis exceeds 512 MiB of decoded mono PCM.");
                    return false;
                }
            }
            inputs.push_back(std::move(input));
            return true;
        };
        if (!capture(args.value(k("reference")).toObject())) return reject("INVALID_SOURCE", reason);
        for (const auto &value : targetValues) {
            if (!value.isObject() || !capture(value.toObject()))
                return reject("INVALID_SOURCE", reason.isEmpty() ? k("Each target must be an object.") : reason);
            if (inputs.back().root == inputs.front().root) return reject("INVALID_GROUP", k("A target cannot move the reference's own group."));
            if (!groups && inputs.back().root != inputs.back().clipId)
                return reject("GROUP_ACKNOWLEDGEMENT_REQUIRED", k("Use includeGroups=true to move complete target groups."));
        }
        int shift = mode == k("audio") ? MaxAnalysisFrames : MaxFrame, overlap = std::max(8, shortest / 2);
        if ((args.contains(k("maxShiftFrames")) && !Mcp::integer(args, k("maxShiftFrames"), 0, shift, shift)) ||
            (args.contains(k("minimumOverlapFrames")) && !Mcp::integer(args, k("minimumOverlapFrames"), 8, shortest, overlap)))
            return reject("INVALID_ARGUMENT", k("Use valid bounded lag and overlap constraints."));
        QString executable = KdenliveSettings::ffmpegpath();
        if (mode == k("audio") && (executable.isEmpty() || !QFileInfo(executable).isFile() || !QFileInfo(executable).isExecutable()))
            return reject("MISSING_DEPENDENCY", k("Configure an existing local FFmpeg executable before audio analysis."));
        if (!store->reserve()) return reject("ALIGNMENT_BUSY", k("Cancel or wait for the active owned alignment job."));
        auto *job = new AlignmentJob(store);
        job->inputs = std::move(inputs);
        job->mode = mode;
        job->executable = executable;
        job->capturedState = state.value(k("expectedState")).toString();
        job->documentId = state.value(k("documentId")).toString();
        job->sequenceId = state.value(k("sequenceId")).toString();
        job->includeGroups = groups;
        job->numerator = numerator;
        job->denominator = denominator;
        job->maxShift = shift;
        job->minimumOverlap = overlap;
        store->jobs.insert(job->id, job);
        store->order.append(job->id);
        QTimer::singleShot(0, job, [job] { job->start(); });
        return answer(job);
    }
    if (args.contains(k("acknowledgeWeakEvidence")) && !args.value(k("acknowledgeWeakEvidence")).isBool())
        return reject("INVALID_ARGUMENT", k("acknowledgeWeakEvidence must be boolean."));
    if (!store->jobs.contains(id)) return reject("INVALID_JOB", k("Use an owned ready alignment job."));
    auto *job = store->jobs[id];
    if (job->status == k("obsolete")) return reject("OBSOLETE_ALIGNMENT", job->message);
    if (job->status != k("ready")) return reject("ALIGNMENT_NOT_READY", k("Inspect a ready alignment proposal before applying it."));
    if (job->capturedState != state.value(k("expectedState")) || !inputsUnchanged(job)) {
        job->finish(k("obsolete"), k("Captured project or source changed."));
        return reject("OBSOLETE_ALIGNMENT", job->message);
    }
    QMap<int, int> deltas, representatives;
    for (const auto &value : job->proposals) {
        const auto proposal = value.toObject();
        if (proposal.value(k("requiresAcknowledgement")).toBool() && args.value(k("acknowledgeWeakEvidence")) != QJsonValue(true))
            return reject("WEAK_ALIGNMENT_EVIDENCE", k("Review the weak/ambiguous audio proposal and explicitly acknowledge it before applying."));
        const int clip = proposal.value(k("clipId")).toInt(), root = rootOf(clip), delta = proposal.value(k("deltaFrames")).toInt();
        if (deltas.contains(root) && deltas[root] != delta)
            return reject("INCOMPATIBLE_GROUP_PROPOSALS", k("Members of one native group propose different moves."));
        deltas[root] = delta;
        representatives[root] = clip;
    }
    struct Placement
    {
        int id, root, track, layer, kind, position, duration;
    };
    std::vector<Placement> placements;
    const auto appendPlacement = [&](int item, int kind) {
        const auto subtitle = timeline->getSubtitleModel();
        placements.push_back({item, rootOf(item), timeline->getItemTrackId(item), kind == 2 ? subtitle->getLayerForId(item) : 0, kind,
                              timeline->getItemPosition(item), timeline->getItemPlaytime(item)});
    };
    for (const auto &entry : timeline->m_allClips)
        appendPlacement(entry.first, 0);
    for (const auto &entry : timeline->m_allCompositions)
        appendPlacement(entry.first, 1);
    if (const auto subtitles = timeline->getSubtitleModel())
        for (int item : subtitles->getAllSubIds())
            appendPlacement(item, 2);
    const auto moved = std::count_if(placements.begin(), placements.end(), [&](const Placement &item) { return deltas.contains(item.root); });
    if (moved > 256 || placements.size() > 20000)
        return reject("ALIGNMENT_BOUNDS", k("One alignment transaction supports 256 moved members in a timeline of at most 20000 items."));
    qint64 parking = timeline->duration() + qint64(1);
    for (const auto &item : placements)
        if (deltas.contains(item.root)) {
            const qint64 final = qint64(item.position) + deltas[item.root];
            if (final < 0 || final + item.duration > MaxFrame)
                return reject("INVALID_POSITION", k("A proposed group member falls outside the supported timeline frame range."));
            if ((item.kind != 2 && (item.track < 0 || timeline->getTrackById_const(item.track)->isLocked())) ||
                (item.kind == 2 && timeline->getSubtitleModel()->isLocked()))
                return reject("TRACK_LOCKED", k("A moved group member is on a locked native track."));
            if (item.kind == 0) {
                const auto mixes = timeline->getTrackById_const(item.track)->getMixInfo(item.id);
                for (const auto &mix : {mixes.first, mixes.second}) {
                    for (int partner : {mix.firstClipId, mix.secondClipId})
                        if (partner >= 0 && timeline->isClip(partner) && rootOf(partner) != item.root)
                            return reject("MIX_GROUP_REQUIRED", k("Native mix partners must belong to the same explicitly moved group."));
                }
            }
            for (const auto &other : placements) {
                if (other.root == item.root || other.kind != item.kind || other.track != item.track || other.layer != item.layer) continue;
                const qint64 otherStart = qint64(other.position) + deltas.value(other.root, 0);
                if (final < otherStart + other.duration && otherStart < final + item.duration)
                    return reject("ALIGNMENT_COLLISION", k("A proposed group member collides with another native item."));
            }
        }
    // Native moves preserve clips/effects/mixes/groups. Park each complete root
    // beyond the current duration first, so valid swaps cannot collide with a
    // different target's old location. No event loop is entered in this edit.
    QMap<int, int> parkingDeltas;
    for (auto it = deltas.begin(); it != deltas.end(); ++it) {
        if (it.value() == 0) continue;
        int first = MaxFrame, last = 0;
        for (const auto &item : placements)
            if (item.root == it.key()) {
                first = std::min(first, item.position);
                last = std::max(last, item.position + item.duration);
            }
        if (parking + last - first > std::numeric_limits<int>::max() / 2)
            return reject("ALIGNMENT_BOUNDS", k("There is insufficient bounded native staging space for an atomic multi-group move."));
        parkingDeltas[it.key()] = int(parking - first);
        parking += last - first + 1;
    }
    Fun undo = [] { return true; }, redo = [] { return true; };
    const auto move = [&](int root, int delta) {
        const int clip = representatives[root];
        if (root != clip) return timeline->requestGroupMove(clip, root, 0, delta, true, true, undo, redo);
        return timeline->requestClipMove(clip, timeline->getClipTrackId(clip), timeline->getClipPosition(clip) + delta, true, true, true, true, undo, redo) ==
               TimelineModel::MoveSuccess;
    };
    const auto rollback = [&] {
        if (undo()) return reject("NATIVE_EDIT_FAILED", k("Native alignment failed; every owned move was rolled back."));
        job->finish(k("obsolete"), k("Native rollback did not fully restore the project. Inspect current state and history before further editing."));
        return reject("ROLLBACK_FAILED", job->message);
    };
    std::vector<int> disabled;
    for (const auto &item : placements) {
        if (item.kind == 0 && deltas.value(item.root, 0) != 0 && timeline->getClipState(item.id).first == PlaylistState::Disabled) {
            disabled.push_back(item.id);
            // Native clip movement normalizes disabled producer states. Record
            // both transitions explicitly so apply, undo and redo all preserve
            // the user's disabled state, rather than silently enabling clips.
            if (!TimelineFunctions::changeClipState(timeline, item.id, timeline->getTrackById_const(item.track)->trackType(), undo, redo)) return rollback();
        }
    }
    bool changed = false;
    for (auto it = parkingDeltas.begin(); it != parkingDeltas.end(); ++it) {
        if (!move(it.key(), it.value())) {
            return rollback();
        }
        changed = true;
    }
    for (auto it = parkingDeltas.begin(); it != parkingDeltas.end(); ++it)
        if (!move(it.key(), deltas[it.key()] - it.value())) {
            return rollback();
        }
    for (int clip : disabled) {
        if (!TimelineFunctions::changeClipState(timeline, clip, PlaylistState::Disabled, undo, redo)) return rollback();
    }
    if (changed) pCore->pushUndo(undo, redo, k("Align clips"));
    job->appliedChanges = changed;
    job->finish(k("applied"),
                changed ? k("Reviewed alignment applied in one native undo command.") : k("All target groups already occupy their proposed positions."));
    return answer(job);
}
