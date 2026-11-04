/* SPDX-FileCopyrightText: 2026 Kdenlive contributors
   SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "mcpspeech.h"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "dialogs/textbasededit.h"
#include "doc/kdenlivedoc.h"
#include "kdenlivesettings.h"
#include "mcphelpers.h"
#include "pythoninterfaces/speechtotextvosk.h"
#include "pythoninterfaces/speechtotextwhisper.h"
#include <KLocalizedString>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <memory>
namespace {
QString k(const char *v)
{
    return QString::fromLatin1(v);
}
constexpr int MaxWords = 50000;
constexpr qint64 MaxResultBytes = 16 * 1024 * 1024;
constexpr int MaxSeconds = 1800;
bool text(const QJsonValue &v, QString &out, int maximum = 4096)
{
    if (!v.isString()) return false;
    out = v.toString();
    return !out.isEmpty() && out.size() <= maximum && !out.contains(QChar::Null);
}
QJsonObject stringProperty(const char *description)
{
    return Mcp::property(k("string"), k(description));
}
QJsonObject intProperty(const char *description)
{
    return Mcp::property(k("integer"), k(description));
}
std::shared_ptr<ProjectClip> sourceFor(const QString &id)
{
    return pCore && pCore->projectItemModel() && pCore->projectItemModel()->hasClip(id) ? pCore->projectItemModel()->getClipByBinID(id) : nullptr;
}
QString existingFile(const QString &path)
{
    const QFileInfo info(path);
    return !path.isEmpty() && info.isAbsolute() && info.isFile() ? info.canonicalFilePath() : QString();
}
QString pythonPath()
{
    // Native venvPythonExecs/checkSetup can rebuild or install, even while checking.
    if (KdenliveSettings::speech_system_python()) return existingFile(KdenliveSettings::speech_system_python_path());
#ifdef Q_OS_WIN
    const QString suffix = k("venv/Scripts/python.exe");
#else
    const QString suffix = k("venv/bin/python3");
#endif
    return existingFile(QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(suffix));
}
std::unique_ptr<SpeechToText> engineFor(const QString &name)
{
    if (name == k("whisper")) return std::make_unique<SpeechToTextWhisper>();
    if (name == k("vosk")) return std::make_unique<SpeechToTextVosk>();
    return {};
}
QStringList dependencies(const QString &engine)
{
    return engine == k("whisper") ? QStringList{k("openai-whisper"), k("torch")} : QStringList{k("vosk")};
}
bool scriptContract(const QString &path, bool whisper)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return false;
    const auto code = file.readAll();
    return code.contains("--pcm_source") && code.contains("--result_file") && (!whisper || code.contains("--model_root"));
}
QString sourceIdentity(const std::shared_ptr<ProjectClip> &source)
{
    if (!source || !source->statusReady()) return {};
    const QFileInfo file(source->url());
    const QByteArray identity = (file.canonicalFilePath() + QLatin1Char('\n') + QString::number(file.size()) + QLatin1Char('\n') +
                                 QString::number(file.lastModified().toMSecsSinceEpoch()) + QLatin1Char('\n') + QString::number(source->frameDuration()) +
                                 QLatin1Char('\n') + source->getProducerProperty(k("audio_index")))
                                    .toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}
struct Word
{
    double start = 0;
    double end = 0;
    QString text;
    int block = 0;
    bool silence = false;
};
QString nativeHtml(const QString &id, const QList<Word> &words)
{
    QTextDocument document;
    QTextCursor cursor(&document);
    int previousBlock = -1;
    for (const auto &word : words) {
        if (previousBlock >= 0 && word.block != previousBlock) cursor.insertBlock();
        QTextCharFormat format;
        format.setAnchor(true);
        format.setAnchorHref(id + QLatin1Char('#') + QString::number(word.start, 'g', 17) + QLatin1Char(':') + QString::number(word.end, 'g', 17));
        cursor.insertText(word.text, format);
        cursor.insertText(k(" "), QTextCharFormat{});
        previousBlock = word.block;
    }
    return document.toHtml();
}
bool readNativeWords(const QString &id, const QString &html, QList<Word> &words)
{
    if (html.size() > MaxResultBytes || html.contains(QChar::Null)) return false;
    QTextDocument document;
    document.setHtml(html);
    const QRegularExpression time(k("^([0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?):([0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)$"));
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isAnchor() || fragment.text().trimmed().isEmpty()) continue;
            const auto href = fragment.charFormat().anchorHref();
            if (!href.startsWith(id + QLatin1Char('#'))) return false;
            const auto match = time.match(href.mid(id.size() + 1));
            if (!match.hasMatch()) return false;
            const double start = match.captured(1).toDouble(), end = match.captured(2).toDouble();
            if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end < start || end > 10000000 || words.size() >= MaxWords ||
                fragment.text().size() > 256)
                return false;
            words.append({start, end, fragment.text().trimmed(), block.blockNumber(), fragment.text().trimmed() == i18n("No speech")});
        }
    }
    return true;
}
QList<Word> withNativeSilences(const QList<Word> &words, double begin, double end, double fps)
{
    QList<Word> result;
    double previousEnd = begin;
    int previousBlock = -1, nextBlock = 0, currentBlock = -1;
    for (const auto &word : words) {
        if (word.block != previousBlock) {
            if (GenTime(word.start).frames(fps) > GenTime(previousEnd).frames(fps) + 1)
                result.append({previousEnd, word.start, i18n("No speech"), nextBlock++, true});
            currentBlock = nextBlock++;
        }
        auto copy = word;
        copy.block = currentBlock;
        result.append(copy);
        previousEnd = std::max(previousEnd, word.end);
        previousBlock = word.block;
    }
    if (GenTime(end).frames(fps) > GenTime(previousEnd).frames(fps) + 1) result.append({previousEnd, end, i18n("No speech"), nextBlock, true});
    return result;
}
QJsonObject wordPage(const QList<Word> &words, int offset, int limit, double fps, int endBoundary)
{
    QJsonArray page, speechRanges, editRanges;
    QHash<int, QPair<int, int>> paragraphBounds;
    for (const auto &word : words) {
        const int first = std::clamp(GenTime(word.start).frames(fps), 0, endBoundary);
        const int boundary = std::clamp(GenTime(word.end).frames(fps), first, endBoundary);
        auto found = paragraphBounds.find(word.block);
        if (found == paragraphBounds.end())
            paragraphBounds.insert(word.block, {first, boundary - 1});
        else {
            found.value().first = std::min(found.value().first, first);
            found.value().second = std::max(found.value().second, boundary - 1);
        }
    }
    int begin = -1, end = -1, paragraph = -1;
    bool silence = false;
    auto flush = [&] {
        if (begin < 0 || end < begin) return;
        const auto bounds = paragraphBounds.value(paragraph);
        if (bounds.second < bounds.first) return;
        const QJsonObject range{{k("paragraph"), paragraph},
                                {k("sourceInFrame"), bounds.first},
                                {k("sourceOutFrame"), bounds.second},
                                {k("durationFrames"), bounds.second - bounds.first + 1}};
        editRanges.append(range);
        if (!silence) speechRanges.append(range);
    };
    for (int i = offset; i < std::min(int(words.size()), offset + limit); ++i) {
        const auto &word = words.at(i);
        const int first = std::clamp(GenTime(word.start).frames(fps), 0, endBoundary);
        const int boundary = std::clamp(GenTime(word.end).frames(fps), first, endBoundary), last = boundary - 1;
        page.append(QJsonObject{{k("index"), i},
                                {k("text"), word.text},
                                {k("startSeconds"), word.start},
                                {k("endSeconds"), word.end},
                                {k("sourceInFrame"), first},
                                {k("sourceOutFrame"), last},
                                {k("endBoundaryFrame"), boundary},
                                {k("durationFrames"), boundary - first},
                                {k("insertable"), boundary > first},
                                {k("silence"), word.silence},
                                {k("paragraph"), word.block}});
        if (boundary <= first) continue;
        if (begin >= 0 && (paragraph != word.block || silence != word.silence)) {
            flush();
            begin = -1;
        }
        if (begin < 0) {
            begin = first;
            end = last;
            paragraph = word.block;
            silence = word.silence;
        } else
            end = std::max(end, last);
    }
    flush();
    return {{k("wordCount"), words.size()},
            {k("offset"), offset},
            {k("words"), page},
            {k("speechRanges"), speechRanges},
            {k("editRanges"), editRanges},
            {k("hasMore"), offset + limit < words.size()},
            {k("fps"), fps},
            {k("rangeScope"), k("Complete native paragraphs intersecting this token page; deduplicate by paragraph ID across pages. Individual word fields "
                                "retain precise selection boundaries.")},
            {k("frameConvention"), k("Native GenTime boundary rounding; sourceOutFrame=endBoundaryFrame-1, inclusive and clipped to source/range.")}};
}

struct SpeechJob
{
    QJsonObject data;
    QPointer<QProcess> process;
    std::unique_ptr<QTemporaryDir> temporary;
    std::weak_ptr<ProjectClip> source;
    QPointer<KdenliveDoc> document;
    QString identity, engine, model, language, task, python, script, modelRoot, ffmpeg, mode, binId;
    int inFrame = 0, outFrame = 0, stream = -1;
    double fps = 25;
    QList<Word> words;
    QByteArray output, errors;
    bool terminal = false, cancelled = false;
};
class SpeechBridge : public QObject
{
public:
    explicit SpeechBridge(QObject *owner)
        : QObject(owner)
    {
        setObjectName(k("kdenlive-mcp-speech"));
    }
    ~SpeechBridge() override
    {
        for (const auto &job : jobs)
            if (job->process && job->process->state() != QProcess::NotRunning) {
                job->process->disconnect(this);
                job->process->kill();
                job->process->waitForFinished(1000);
            }
    }
    QHash<QString, std::shared_ptr<SpeechJob>> jobs;
    std::function<QJsonObject()> readState;
    QJsonObject state() const { return readState ? readState() : QJsonObject{}; }
    void fail(const std::shared_ptr<SpeechJob> &job, const QString &code, const QString &message)
    {
        job->terminal = true;
        job->data.insert(k("status"), job->cancelled ? k("cancelled") : k("failed"));
        job->data.insert(k("finished"), true);
        if (!job->cancelled) job->data.insert(k("error"), QJsonObject{{k("code"), code}, {k("message"), message}});
        job->output.clear();
        job->errors.clear();
        // completed() may still have a QFile on its stack. Defer removal until
        // both the process and that callback have released native file handles.
        QTimer::singleShot(0, this, [job] {
            if (job->temporary && (!job->process || job->process->state() == QProcess::NotRunning)) job->temporary.reset();
        });
    }
    bool sameSource(const std::shared_ptr<SpeechJob> &job) const
    {
        const auto source = job->source.lock();
        return job->document && pCore->currentDoc() == job->document && source && sourceFor(job->binId) == source && sourceIdentity(source) == job->identity &&
               std::abs(pCore->getCurrentFps() - job->fps) < 1e-9;
    }
    QJsonObject jobState(const std::shared_ptr<SpeechJob> &job) const
    {
        auto data = job->data;
        if (job->mode == k("recognize")) data.insert(k("sourceCurrent"), sameSource(job));
        return data;
    }
    bool room()
    {
        int active = 0;
        for (const auto &job : jobs)
            if (!job->terminal || (job->process && job->process->state() != QProcess::NotRunning)) ++active;
        if (active >= 1) return false;
        if (jobs.size() < 32) return true;
        for (auto it = jobs.begin(); it != jobs.end(); ++it)
            if (it.value()->terminal) {
                jobs.erase(it);
                return true;
            }
        return false;
    }
    void run(const std::shared_ptr<SpeechJob> &job, const QString &phase, const QString &program, const QStringList &args, int timeout,
             const std::function<void()> &next)
    {
        if (job->cancelled) {
            fail(job, {}, {});
            return;
        }
        job->data.insert(k("status"), phase);
        job->output.clear();
        job->errors.clear();
        auto *process = new QProcess(this);
        job->process = process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(k("PYTHONDONTWRITEBYTECODE"), k("1"));
        environment.insert(k("PYTHONIOENCODING"), k("utf-8"));
        environment.insert(k("HF_HUB_OFFLINE"), k("1"));
        environment.insert(k("TRANSFORMERS_OFFLINE"), k("1"));
        environment.insert(k("HF_DATASETS_OFFLINE"), k("1"));
        process->setProcessEnvironment(environment);
        if (job->temporary) process->setWorkingDirectory(job->temporary->path());
        connect(process, &QProcess::readyReadStandardOutput, this, [this, job, process] {
            const auto bytes = process->readAllStandardOutput();
            if (job->terminal) return;
            job->output.append(bytes);
            if (job->output.size() > 1024 * 1024) {
                fail(job, k("OUTPUT_LIMIT"), k("Native speech diagnostic output exceeded its bound."));
                process->kill();
            }
        });
        connect(process, &QProcess::readyReadStandardError, this, [job, process] {
            job->errors.append(process->readAllStandardError());
            if (job->errors.size() > 65536) job->errors = job->errors.right(65536);
        });
        connect(process, &QProcess::errorOccurred, this, [this, job, process](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart && !job->terminal) {
                fail(job, k("START_FAILED"), process->errorString());
                process->deleteLater();
            }
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, job, process, next](int code, QProcess::ExitStatus status) {
            job->output.append(process->readAllStandardOutput());
            job->errors.append(process->readAllStandardError());
            process->deleteLater();
            if (job->terminal) {
                if (job->temporary) job->temporary.reset();
                return;
            }
            if (job->cancelled) {
                fail(job, {}, {});
                return;
            }
            if (status != QProcess::NormalExit || code != 0) {
                fail(job, k("NATIVE_SPEECH_FAILED"), k("Native process failed: ") + QString::fromUtf8(job->errors.right(4096)));
                return;
            }
            if (job->output.size() > 1024 * 1024) {
                fail(job, k("OUTPUT_LIMIT"), k("Native process diagnostic output exceeded its bound."));
                return;
            }
            next();
        });
        QTimer::singleShot(timeout, process, [this, job, process] {
            if (!job->terminal && job->process == process && process->state() != QProcess::NotRunning) {
                fail(job, k("DEADLINE_EXCEEDED"), k("Native speech phase exceeded its bounded deadline."));
                process->kill();
            }
        });
        process->start(program, args);
    }
    void completed(const std::shared_ptr<SpeechJob> &job)
    {
        QFile file(job->temporary->filePath(k("result.json")));
        if (!file.open(QIODevice::ReadOnly) || file.size() > MaxResultBytes) {
            fail(job, k("INVALID_RESULT"), k("The native engine did not produce a bounded transcript result."));
            return;
        }
        QJsonParseError parseError;
        const auto result = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !result.isObject()) {
            fail(job, k("INVALID_RESULT"), k("The native engine returned invalid transcript JSON."));
            return;
        }
        const auto object = result.object();
        const QString groupKey = job->engine == k("whisper") ? k("segments") : k("results");
        if (!object.value(groupKey).isArray()) {
            fail(job, k("INVALID_RESULT"), k("The native transcript segment array is missing."));
            return;
        }
        const auto groups = object.value(groupKey).toArray();
        if (job->engine == k("whisper")) job->data.insert(k("detectedLanguage"), object.value(k("language")));
        const double duration = (job->outFrame - job->inFrame + 1) / job->fps, offset = job->inFrame / job->fps;
        int block = 0;
        for (const auto &group : groups) {
            const QString wordsKey = job->engine == k("whisper") ? k("words") : k("result");
            const auto value = group.toObject().value(wordsKey);
            const bool emptyVosk = job->engine == k("vosk") && value.isUndefined() && group.toObject().value(k("text")).isString() &&
                                   group.toObject().value(k("text")).toString().isEmpty();
            if (!group.isObject() || (!value.isArray() && !emptyVosk)) {
                fail(job, k("INVALID_RESULT"), k("Native transcript contains a malformed word group."));
                return;
            }
            const auto items = value.toArray();
            for (const auto &item : items) {
                const auto word = item.toObject();
                double start = 0, end = 0;
                QString value;
                if (!Mcp::real(word, k("start"), 0, duration + 0.1, start) || !Mcp::real(word, k("end"), start, duration + 0.1, end) ||
                    !text(word.value(k("word")), value, 256) || value.trimmed().isEmpty() || job->words.size() >= MaxWords) {
                    fail(job, k("INVALID_RESULT"), k("Native transcript contains invalid word timing or exceeded its word bound."));
                    return;
                }
                job->words.append({start + offset, end + offset, value.trimmed(), block, false});
            }
            ++block;
        }
        file.close();
        QFile srt(job->temporary->filePath(k("speech.srt")));
        if (!srt.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            fail(job, k("SAVE_FAILED"), k("Could not stage the recognized subtitles."));
            return;
        }
        auto timestamp = [](double seconds) {
            const qint64 ms = qRound64(seconds * 1000);
            return QStringLiteral("%1:%2:%3,%4")
                .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
                .arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
                .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
                .arg(ms % 1000, 3, 10, QLatin1Char('0'));
        };
        QString contents;
        int cue = 0;
        for (int first = 0; first < job->words.size();) {
            auto subtitleText = [](QString value) { return value.replace(QRegularExpression(k("[\\r\\n\\x{2028}\\x{2029}]+")), k(" ")); };
            int last = first;
            QString line = subtitleText(job->words.at(first).text);
            while (last + 1 < job->words.size() && last - first < 6 && job->words.at(last + 1).block == job->words.at(first).block &&
                   line.size() + job->words.at(last + 1).text.size() < 84)
                line += QLatin1Char(' ') + subtitleText(job->words.at(++last).text);
            const double begin = job->words.at(first).start - offset, end = std::max(begin + 0.001, job->words.at(last).end - offset);
            contents += QString::number(++cue) + QLatin1Char('\n') + timestamp(begin) + k(" --> ") + timestamp(end) + QLatin1Char('\n') + line + k("\n\n");
            first = last + 1;
        }
        const auto bytes = contents.toUtf8();
        if (srt.write(bytes) != bytes.size() || !srt.flush()) {
            srt.close();
            fail(job, k("SAVE_FAILED"), k("Could not save staged speech subtitles."));
            return;
        }
        job->terminal = true;
        job->data.insert(k("status"), k("completed"));
        job->data.insert(k("finished"), true);
        job->data.insert(k("wordCount"), job->words.size());
        job->data.insert(k("subtitlePath"), srt.fileName());
        job->data.insert(k("subtitleCount"), cue);
        job->data.insert(k("subtitleTimeBase"), k("Seconds relative to the extracted range; use subtitle_file offsetFrames for placement."));
        job->data.insert(k("resultLifetime"), k("Owned by this MCP server; import before server exit or eviction after 32 jobs."));
        job->output.clear();
        job->errors.clear();
    }
    void recognize(const std::shared_ptr<SpeechJob> &job)
    {
        QStringList args{job->script,       k("--src"),         job->temporary->filePath(k("audio.wav")),  k("--model"), job->model,
                         k("--pcm_source"), k("--result_file"), job->temporary->filePath(k("result.json"))};
        if (job->engine == k("whisper")) {
            args << k("--model_root") << job->modelRoot << k("--device") << k("cpu") << k("--task") << job->task;
            if (!job->language.isEmpty()) args << k("--language") << job->language;
        } else
            args << k("--model_directory") << job->modelRoot;
        run(job, k("recognizing"), job->python, args, 60 * 60 * 1000, [this, job] { completed(job); });
    }
    void prepare(const std::shared_ptr<SpeechJob> &job)
    {
        if (!sameSource(job)) {
            fail(job, k("SOURCE_CHANGED"), k("The project or source changed before extraction."));
            return;
        }
        const auto source = job->source.lock();
        QStringList args{k("-v"),
                         k("error"),
                         k("-nostdin"),
                         k("-protocol_whitelist"),
                         k("file,pipe"),
                         k("-i"),
                         existingFile(source->url()),
                         k("-ss"),
                         QString::number(job->inFrame / job->fps, 'f', 12),
                         k("-t"),
                         QString::number((job->outFrame - job->inFrame + 1) / job->fps, 'f', 12),
                         k("-map"),
                         k("0:") + QString::number(job->stream),
                         k("-vn"),
                         k("-sn"),
                         k("-dn"),
                         k("-ac"),
                         k("1"),
                         k("-ar"),
                         k("16000"),
                         k("-c:a"),
                         k("pcm_s16le"),
                         k("-n"),
                         job->temporary->filePath(k("audio.wav"))};
        run(job, k("extracting_audio"), job->ffmpeg, args, 10 * 60 * 1000, [this, job] { recognize(job); });
    }
    void begin(const std::shared_ptr<SpeechJob> &job)
    {
        if (job->terminal) return;
        if (job->cancelled) {
            fail(job, {}, {});
            return;
        }
        job->python = pythonPath();
        if (job->python.isEmpty()) {
            fail(job, k("MISSING_PYTHON"), k("Configure an existing native speech Python environment; MCP does not install or rebuild it."));
            return;
        }
        auto engine = engineFor(job->engine);
        const QString check = existingFile(engine->getScript(k("checkpackages.py")));
        if (check.isEmpty()) {
            fail(job, k("MISSING_NATIVE_SCRIPT"), k("The installed native package-check script is missing."));
            return;
        }
        if (job->mode == k("recognize")) {
            job->script = existingFile(engine->speechScript());
            job->modelRoot = QDir(engine->modelFolder(true, false)).canonicalPath();
            if (!engine->getInstalledModels().contains(job->model)) {
                fail(job, k("MISSING_MODEL"), k("Select a locally installed model from speech_capabilities; no download is performed."));
                return;
            }
            if (!scriptContract(job->script, job->engine == k("whisper"))) {
                fail(job, k("NATIVE_SCRIPT_UPDATE_REQUIRED"),
                     k("Installed speech scripts lack the PCM/offline result contract; use this build's matching scripts."));
                return;
            }
            job->ffmpeg = existingFile(KdenliveSettings::ffmpegpath());
            if (job->ffmpeg.isEmpty()) {
                fail(job, k("MISSING_FFMPEG"), k("Configure the native FFmpeg executable before speech extraction."));
                return;
            }
            job->temporary =
                std::make_unique<QTemporaryDir>(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(k("kdenlive-mcp-speech-XXXXXX")));
            if (!job->temporary->isValid()) {
                fail(job, k("TEMPORARY_FILE_FAILED"), k("Could not allocate an owned speech workspace."));
                return;
            }
        }
        QStringList args{check, k("--check")};
        args.append(dependencies(job->engine));
        run(job, k("checking_dependencies"), job->python, args, 30000, [this, job] {
            QJsonArray missing;
            for (const auto &line : QString::fromUtf8(job->output).split(QLatin1Char('\n')))
                if (line.startsWith(k("Missing:"))) missing.append(line.mid(8).trimmed());
            if (!missing.isEmpty()) {
                job->data.insert(k("missingDependencies"), missing);
                fail(job, k("MISSING_DEPENDENCIES"), k("The native engine's Python dependencies are absent; nothing was installed."));
                return;
            }
            job->data.insert(k("dependencyMetadataPresent"), true);
            if (job->mode == k("probe")) {
                job->terminal = true;
                job->data.insert(k("status"), k("completed"));
                job->data.insert(k("finished"), true);
                job->data.insert(k("engineVerified"), false);
                job->data.insert(k("note"), k("Package metadata only; model loading and recognition are verified only by a completed recognition job."));
                job->output.clear();
                job->errors.clear();
            } else
                prepare(job);
        });
    }
};
SpeechBridge *bridge(QObject *owner)
{
    for (auto *child : owner->children())
        if (auto *existing = dynamic_cast<SpeechBridge *>(child)) return existing;
    return new SpeechBridge(owner);
}
} // namespace
namespace McpSpeech {
QJsonArray toolCatalog()
{
    const auto expected = stringProperty("Fresh get_state.expectedState before starting or editing.");
    const auto bin = stringProperty("Exact native bin clip ID."), job = stringProperty("Owned speech job ID.");
    return {Mcp::tool(k("speech_capabilities"),
                      k("Read native local speech engines, installed model names and languages without checking, installing, downloading or repairing Python."),
                      {}, {}, true),
            Mcp::tool(k("start_speech_job"),
                      k("Queue a native offline speech job. probe checks package metadata; recognize extracts one local file's explicit audio stream/range and "
                        "returns native word timestamps and staged SRT. No automatic project edits or downloads."),
                      {{k("expectedState"), expected},
                       {k("mode"), stringProperty("probe or recognize.")},
                       {k("engine"), stringProperty("whisper or vosk.")},
                       {k("binId"), bin},
                       {k("model"), stringProperty("Exact installed model name.")},
                       {k("task"), stringProperty("transcribe (default) or translate_english (Whisper only).")},
                       {k("language"), stringProperty("Native Whisper language ID from speech_capabilities; omit to autodetect.")},
                       {k("inFrame"), intProperty("Inclusive source start, required for recognize.")},
                       {k("outFrame"), intProperty("Inclusive source end; maximum 30 minutes per job.")},
                       {k("streamId"), intProperty("Actual native audio stream ID, required for recognize.")}},
                      {k("expectedState"), k("mode"), k("engine")}),
            Mcp::tool(k("speech_job_status"), k("Read bounded owned native speech job status. A successful metadata probe does not verify model readiness."),
                      {{k("jobId"), job}}, {}, true),
            Mcp::tool(k("cancel_speech_job"),
                      k("Cancel an owned queued/native speech job and its current process. Direct PCM recognition launches no FFmpeg child. Terminal results "
                        "cannot be cancelled."),
                      {{k("jobId"), job}}, {k("jobId")}),
            Mcp::tool(k("get_speech_transcript"),
                      k("Read exact native timestamp anchors from a bin transcript or completed owned job, in bounded pages. Current editRanges preserve "
                        "retained silence; speechRanges omit silence. Inclusive ranges can be used by insert_media_streams/insert_clip."),
                      {{k("binId"), bin},
                       {k("jobId"), job},
                       {k("offset"), intProperty("First transcript token index, default zero.")},
                       {k("limit"), intProperty("Maximum tokens in this page, 1..128; default 128.")}},
                      {}, true),
            Mcp::tool(k("edit_speech_transcript"),
                      k("Undoably apply an owned transcript, replace one word without retiming it, or remove/keep words/silence in native text-editor "
                        "metadata. To create an edited video, inspect the returned source ranges then insert them using timeline tools."),
                      {{k("expectedState"), expected},
                       {k("binId"), bin},
                       {k("action"), stringProperty("apply_job, replace_word, remove_words, keep_words, or remove_silence.")},
                       {k("jobId"), job},
                       {k("firstWord"), intProperty("First token index for replace/remove/keep.")},
                       {k("wordCount"), intProperty("Positive token count for remove/keep.")},
                       {k("text"), stringProperty("Replacement word text, at most 256 characters; native times are preserved.")}},
                      {k("expectedState"), k("binId"), k("action")})};
}
QJsonObject callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState,
                     const std::function<QJsonObject()> &stateReader)
{
    bool known = false;
    for (const auto &entry : toolCatalog())
        if (entry.toObject().value(k("name")) == name) known = true;
    if (!known) return {};
    auto *service = bridge(owner);
    service->readState = stateReader;
    auto reject = [&](const char *code, const char *message) { return Mcp::failure(k(code), k(message), nativeState); };
    auto finish = [&](QJsonObject data = {}) {
        data.insert(k("ok"), true);
        data.insert(k("state"), service->state());
        return Mcp::toolResult(data);
    };
    if (name == k("speech_capabilities")) {
        QJsonArray engines;
        for (const auto &id : {k("whisper"), k("vosk")}) {
            auto engine = engineFor(id);
            QJsonArray models, languages;
            const auto installed = engine->getInstalledModels();
            for (const auto &model : installed)
                models.append(model);
            const auto nativeLanguages = engine->speechLanguages();
            for (auto it = nativeLanguages.cbegin(); it != nativeLanguages.cend(); ++it)
                languages.append(QJsonObject{{k("name"), it.key()}, {k("id"), it.value()}});
            const QString script = existingFile(engine->speechScript()), python = pythonPath();
            const bool localFiles = !python.isEmpty() && !installed.isEmpty() && scriptContract(script, id == k("whisper")) &&
                                    !existingFile(KdenliveSettings::ffmpegpath()).isEmpty();
            bool packagesSeen = false, recognitionSeen = false, translationSeen = false;
            for (const auto &job : service->jobs)
                if (job->engine == id && job->python == python && job->data.value(k("status")) == k("completed")) {
                    packagesSeen = packagesSeen || job->data.value(k("dependencyMetadataPresent")).toBool();
                    if (job->mode == k("recognize") && installed.contains(job->model) &&
                        job->modelRoot == QDir(engine->modelFolder(true, false)).canonicalPath()) {
                        recognitionSeen = true;
                        translationSeen = translationSeen || job->task == k("translate");
                    }
                }
            QJsonObject modelTasks;
            QJsonArray translationModels;
            for (const auto &model : installed) {
                QJsonArray tasks{k("transcribe")};
                if (id == k("whisper") && !model.contains(k("turbo")) && !model.endsWith(k(".en"))) {
                    tasks.append(k("translate_english"));
                    translationModels.append(model);
                }
                modelTasks.insert(model, tasks);
            }
            engines.append(QJsonObject{
                {k("id"), id},
                {k("modelDirectory"), engine->modelFolder(true, false)},
                {k("installedModels"), models},
                {k("modelTasks"), modelTasks},
                {k("languages"), languages},
                {k("languageSelection"), id == k("vosk") ? k("Determined by installed model.") : k("Native language ID or autodetect.")},
                {k("nativeScriptAvailable"), !script.isEmpty()},
                {k("offlinePcmContract"), scriptContract(script, id == k("whisper"))},
                {k("pythonAvailable"), !python.isEmpty()},
                {k("dependencyStatus"), packagesSeen ? k("metadata_present") : k("not_probed")},
                {k("recognitionVerified"), recognitionSeen},
                {k("localFilesPresent"), localFiles},
                {k("computeDevice"), k("cpu")},
                {k("verificationScope"), k("Only completed owned recognition jobs verify execution; changed model files/environments require another run.")},
                {k("translation"), QJsonObject{{k("available"), localFiles && packagesSeen && !translationModels.isEmpty()},
                                               {k("target"), id == k("whisper") ? k("English") : k("none")},
                                               {k("installedSupportedModels"), translationModels},
                                               {k("verified"), translationSeen}}}});
        }
        return finish(
            {{k("engines"), engines},
             {k("configuredPython"), pythonPath()},
             {k("maxConcurrentJobs"), 1},
             {k("maxDurationSeconds"), MaxSeconds},
             {k("seamless"),
              QJsonObject{{k("available"), false},
                          {k("reason"),
                           k("Native multilingual translation can fetch models; offline support and prerequisites need separate approval/verification.")}}},
             {k("sourceSupport"), k("Existing local Audio/AV bin files with an explicit audio stream/range. Sequence/playlist mixes and live recordings "
                                    "require a separate native export/capture workflow.")}});
    }
    if (name == k("speech_job_status") || name == k("cancel_speech_job")) {
        if (name == k("speech_job_status") && !arguments.contains(k("jobId"))) {
            QJsonArray jobs;
            auto ids = service->jobs.keys();
            std::sort(ids.begin(), ids.end());
            for (const auto &id : ids)
                jobs.append(service->jobState(service->jobs.value(id)));
            return finish({{k("jobs"), jobs}});
        }
        QString id;
        if (!text(arguments.value(k("jobId")), id, 128) || !service->jobs.contains(id)) return reject("INVALID_JOB", "Unknown owned speech job ID.");
        const auto job = service->jobs.value(id);
        if (name == k("cancel_speech_job") && !job->terminal) {
            job->cancelled = true;
            if (job->process && job->process->state() != QProcess::NotRunning) {
                job->data.insert(k("status"), k("cancelling"));
                job->process->kill();
            } else
                service->fail(job, {}, {});
        }
        return finish({{k("job"), service->jobState(job)}});
    }
    if (name == k("start_speech_job")) {
        QString engine, mode;
        if (!text(arguments.value(k("engine")), engine, 32) || !engineFor(engine) || !text(arguments.value(k("mode")), mode, 32) ||
            (mode != k("probe") && mode != k("recognize")))
            return reject("INVALID_ARGUMENT", "Use native whisper/vosk and mode probe/recognize.");
        auto job = std::make_shared<SpeechJob>();
        job->engine = engine;
        job->mode = mode;
        if (mode == k("recognize")) {
            job->binId = arguments.value(k("binId")).toString();
            const auto source = sourceFor(job->binId);
            if (!source || !source->statusReady() || !source->hasAudio() || (source->clipType() != ClipType::AV && source->clipType() != ClipType::Audio) ||
                existingFile(source->url()).isEmpty())
                return reject("INVALID_SOURCE", "Select a ready native local Audio/AV bin file; playlist/sequence extraction is unavailable here.");
            if (!text(arguments.value(k("model")), job->model, 256) || !Mcp::integer(arguments, k("inFrame"), 0, 10000000, job->inFrame) ||
                !Mcp::integer(arguments, k("outFrame"), job->inFrame, 10000000, job->outFrame) || job->outFrame >= qint64(source->frameDuration()) ||
                !Mcp::integer(arguments, k("streamId"), 0, 1024, job->stream) || !source->audioStreams().contains(job->stream))
                return reject("INVALID_ARGUMENT", "Supply an installed model, valid inclusive source range and actual native audio stream ID.");
            job->fps = pCore->getCurrentFps();
            if ((job->outFrame - job->inFrame + 1) / job->fps > MaxSeconds)
                return reject("DURATION_LIMIT", "Recognize at most 30 minutes per job; split longer sources into ranges.");
            if (arguments.contains(k("task")) && !arguments.value(k("task")).isString()) return reject("INVALID_ARGUMENT", "Task must be a string.");
            const QString task = arguments.value(k("task")).toString(k("transcribe"));
            if (task != k("transcribe") && (task != k("translate_english") || engine != k("whisper")))
                return reject("INVALID_ARGUMENT", "Translation is local Whisper-to-English only.");
            if (task == k("translate_english") && (job->model.contains(k("turbo")) || job->model.endsWith(k(".en"))))
                return reject("UNSUPPORTED_MODEL_TASK", "Turbo and English-only Whisper checkpoints do not support multilingual translation-to-English.");
            job->task = task == k("translate_english") ? k("translate") : task;
            if (arguments.contains(k("language"))) {
                if (!arguments.value(k("language")).isString()) return reject("INVALID_ARGUMENT", "Language must be a native language ID.");
                job->language = arguments.value(k("language")).toString();
                if (engine != k("whisper") || !engineFor(engine)->speechLanguages().values().contains(job->language))
                    return reject("INVALID_ARGUMENT", "Use a native Whisper language ID; Vosk language belongs to its model.");
            }
            if (job->model.endsWith(k(".en")) && !job->language.isEmpty() && job->language != k("English"))
                return reject("UNSUPPORTED_MODEL_LANGUAGE", "English-only Whisper checkpoints accept English or autodetect only.");
            job->source = source;
            job->document = pCore->currentDoc();
            job->identity = sourceIdentity(source);
        } else if (arguments.contains(k("binId")) || arguments.contains(k("model")) || arguments.contains(k("task")) || arguments.contains(k("language")) ||
                   arguments.contains(k("inFrame")) || arguments.contains(k("outFrame")) || arguments.contains(k("streamId")))
            return reject("INVALID_ARGUMENT", "A probe accepts only mode, engine and expectedState.");
        if (!service->room()) return reject("SPEECH_BUSY", "Only one native speech job may run at a time.");
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        job->data = {{k("jobId"), id},         {k("mode"), mode},        {k("engine"), engine},  {k("status"), k("queued")},   {k("finished"), false},
                     {k("binId"), job->binId}, {k("model"), job->model}, {k("task"), job->task}, {k("inFrame"), job->inFrame}, {k("outFrame"), job->outFrame},
                     {k("fps"), job->fps}};
        service->jobs.insert(id, job);
        QTimer::singleShot(0, service, [service, job] { service->begin(job); });
        return finish({{k("job"), service->jobState(job)}});
    }
    QString binId = arguments.value(k("binId")).toString();
    std::shared_ptr<ProjectClip> source;
    QList<Word> words;
    double fps = pCore->getCurrentFps();
    int endBoundary = 10000001;
    if (name == k("get_speech_transcript") && arguments.contains(k("jobId"))) {
        if (arguments.contains(k("binId"))) return reject("INVALID_ARGUMENT", "Choose either binId or jobId.");
        const QString id = arguments.value(k("jobId")).toString();
        if (!service->jobs.contains(id)) return reject("INVALID_JOB", "Unknown owned speech job ID.");
        const auto job = service->jobs.value(id);
        if (job->mode != k("recognize") || job->data.value(k("status")) != k("completed"))
            return reject("RESULT_NOT_READY", "This recognition job has no completed transcript.");
        words = job->words;
        fps = job->fps;
        binId = job->binId;
        endBoundary = job->outFrame + 1;
    } else {
        source = sourceFor(binId);
        if (!source || !source->statusReady()) return reject("INVALID_BIN_CLIP", "Select a ready native bin clip ID.");
        endBoundary = int(std::min(size_t(10000001), source->frameDuration()));
        if (!readNativeWords(binId, source->getProducerProperty(k("kdenlive:speech")), words))
            return reject("INVALID_TRANSCRIPT", "Native transcript contains malformed or excessive timestamp anchors.");
    }
    if (name == k("get_speech_transcript")) {
        int offset = 0, limit = 128;
        if ((arguments.contains(k("offset")) && !Mcp::integer(arguments, k("offset"), 0, int(words.size()), offset)) ||
            (arguments.contains(k("limit")) && !Mcp::integer(arguments, k("limit"), 1, 128, limit)))
            return reject("INVALID_ARGUMENT", "Use an existing token offset and limit 1..128.");
        auto result = wordPage(words, offset, limit, fps, endBoundary);
        result.insert(k("binId"), binId);
        return finish(result);
    }
    if (!pCore->undoStack()) return reject("NOT_READY", "A native undo stack is required.");
    const QString action = arguments.value(k("action")).toString();
    const bool hasIndex = arguments.contains(k("firstWord")), hasCount = arguments.contains(k("wordCount")), hasText = arguments.contains(k("text")),
               hasJob = arguments.contains(k("jobId"));
    if ((action == k("apply_job") && (hasIndex || hasCount || hasText)) || (action == k("remove_silence") && (hasIndex || hasCount || hasText || hasJob)) ||
        (action == k("replace_word") && (hasCount || hasJob)) || ((action == k("remove_words") || action == k("keep_words")) && (hasText || hasJob)))
        return reject("INVALID_ARGUMENT", "Supply only arguments belonging to this transcript edit action.");
    if (action == k("apply_job")) {
        const QString id = arguments.value(k("jobId")).toString();
        if (!service->jobs.contains(id)) return reject("INVALID_JOB", "Unknown owned speech job ID.");
        const auto job = service->jobs.value(id);
        if (job->mode != k("recognize") || job->data.value(k("status")) != k("completed") || job->binId != binId || !service->sameSource(job))
            return reject("SOURCE_CHANGED", "Apply only a completed result to its unchanged original native project/source.");
        words = withNativeSilences(job->words, job->inFrame / job->fps, (job->outFrame + 1) / job->fps, job->fps);
    } else if (action == k("remove_silence"))
        words.erase(std::remove_if(words.begin(), words.end(), [](const Word &word) { return word.silence; }), words.end());
    else {
        int first = 0, count = 1;
        if (!Mcp::integer(arguments, k("firstWord"), 0, int(words.size()) - 1, first))
            return reject("INVALID_ARGUMENT", "Choose an existing transcript token index.");
        if (action == k("replace_word")) {
            QString value;
            if (!text(arguments.value(k("text")), value, 256) || value.trimmed().isEmpty() || value.contains(QLatin1Char('\n')) ||
                value.contains(QLatin1Char('\r')) || words.at(first).silence)
                return reject("INVALID_ARGUMENT", "Replace one speech token with bounded nonempty single-line text, preserving its times.");
            words[first].text = value;
        } else if (action == k("remove_words") || action == k("keep_words")) {
            if (!Mcp::integer(arguments, k("wordCount"), 1, int(words.size()) - first, count))
                return reject("INVALID_ARGUMENT", "Choose a positive existing token range.");
            if (action == k("remove_words")) {
                // Native fullExport spans each paragraph. Split at a deletion so
                // reopening the editor cannot reintroduce the removed audio.
                if (first > 0 && first + count < words.size() && words.at(first - 1).block == words.at(first + count).block) {
                    int maximumBlock = 0;
                    for (const auto &word : words)
                        maximumBlock = std::max(maximumBlock, word.block);
                    const int oldBlock = words.at(first + count).block;
                    for (int i = first + count; i < words.size() && words.at(i).block == oldBlock; ++i)
                        words[i].block = maximumBlock + 1;
                }
                words.erase(words.begin() + first, words.begin() + first + count);
            } else
                words = words.mid(first, count);
        } else
            return reject("INVALID_ARGUMENT", "Unknown native transcript edit action.");
    }
    if (words.size() > MaxWords)
        return reject("TRANSCRIPT_LIMIT", "Native transcript including silence exceeds its token bound; apply shorter recognition ranges.");
    const QString previous = source->getProducerProperty(k("kdenlive:speech")), updated = nativeHtml(binId, words);
    if (updated.size() > MaxResultBytes) return reject("TRANSCRIPT_LIMIT", "Generated native transcript exceeds its bounded HTML size.");
    if (updated != previous) {
        auto apply = [source](const QString &html) {
            source->setProperties({{k("kdenlive:speech"), html}});
            if (pCore->textEditWidget()) pCore->textEditWidget()->openClip(source);
            return source->getProducerProperty(k("kdenlive:speech")) == html;
        };
        if (!apply(updated)) return reject("EDIT_FAILED", "Native transcript property did not retain the edit.");
        pCore->pushUndo([apply, previous] { return apply(previous); }, [apply, updated] { return apply(updated); }, k("MCP edit speech transcript"));
    }
    return finish({{k("binId"), binId}, {k("wordCount"), words.size()}, {k("undoable"), true}, {k("nativeProperty"), k("kdenlive:speech")}});
}
} // namespace McpSpeech
