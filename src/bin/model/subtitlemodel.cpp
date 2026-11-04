/*
    SPDX-FileCopyrightText: 2020 Sashmita Raghav
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "subtitlemodel.hpp"
#include "config-kdenlive.h"
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "kdenlivesettings.h"
#include "macros.hpp"
#include "project/projectmanager.h"
#include "timeline2/model/groupsmodel.hpp"
#include "timeline2/model/snapmodel.hpp"
#include "timeline2/model/timelineitemmodel.hpp"

#include <KEncodingProber>
#include <KLocalizedString>
#include <KMessageBox>
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStringConverter>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

std::shared_ptr<TimelineItemModel> SubtitleModel::timeline() const
{
    return m_timeline;
}

SubtitleModel::SubtitleModel(std::shared_ptr<TimelineItemModel> timeline, const std::weak_ptr<SnapInterface> &snapModel, QObject *parent)
    : QAbstractListModel(parent)
    , m_timeline(timeline)
    , m_lock(QReadWriteLock::Recursive)
    , m_subtitleFilter(new Mlt::Filter(pCore->getProjectProfile(), "avfilter.subtitles"))
{
    qDebug() << "Subtitle constructor";
    // Ensure the subtitle also covers transparent zones (useful for timeline sequences)
    m_subtitleFilter->set("av.alpha", 1);
    if (m_timeline->tractor() != nullptr) {
        qDebug() << "Tractor!";
        m_subtitleFilter->set("internal_added", 237);
    }
    setup();
    connect(this, &SubtitleModel::modelChanged, [this]() { jsontoSubtitle(toJson()); });

    const QUuid timelineUuid = timeline->uuid();
    int id = pCore->currentDoc()->getSequenceProperty(timelineUuid, QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt();
    const QString subPath = pCore->currentDoc()->subTitlePath(timelineUuid, id, true);
    const QString workPath = pCore->currentDoc()->subTitlePath(timelineUuid, id, false);
    registerSnap(snapModel);

    // Load global subtitle styles
    m_globalSubtitleStyles = pCore->currentDoc()->globalSubtitleStyles(timeline->uuid());

    QFile subFile(subPath);
    if (pCore->currentDoc()->m_restoreFromBackup) {
        QString tmpWorkPath = pCore->currentDoc()->subTitlePath(timelineUuid, id, false, true);
        QFile prevSub(tmpWorkPath);
        if (prevSub.exists()) {
            if (!subFile.exists()) {
                prevSub.copy(subPath);
            }
            prevSub.copy(workPath);
        }
    } else {
        if (subFile.exists()) {
            subFile.copy(workPath);
        }
    }

    if (!QFile::exists(workPath)) {
        QFile newSub(workPath);
        if (newSub.open(QIODevice::WriteOnly)) {
            newSub.close();
            qDebug() << "MISSING SUBTITLE FILE, create tmp: " << workPath;
        } else {
            qWarning() << "Failed to create temporary subtitle file:" << workPath << newSub.errorString();
        }
    }

    // Load multiple subtitle data
    QMap<std::pair<int, QString>, QString> multiSubs = pCore->currentDoc()->multiSubtitlePath(timelineUuid);
    m_subtitlesList = multiSubs;
    if (m_subtitlesList.isEmpty()) {
        m_subtitlesList.insert({0, i18n("Subtitles")}, subPath);
    }

    parseSubtitle(workPath);
}

void SubtitleModel::setForceStyle(const QString &style)
{
    QString oldStyle = m_subtitleFilter->get("av.force_style");
    Fun redo = [this, style]() {
        m_subtitleFilter->set("av.force_style", style.toUtf8().constData());
        // Force refresh to show the new style
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    Fun undo = [this, oldStyle]() {
        m_subtitleFilter->set("av.force_style", oldStyle.toUtf8().constData());
        // Force refresh to show the new style
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    redo();
    pCore->pushUndo(undo, redo, i18n("Edit global subtitle style"));
}

const QString SubtitleModel::getForceStyle() const
{
    const QString style = m_subtitleFilter->get("av.force_style");
    return style;
}

void SubtitleModel::setup()
{
    // We connect the signals of the abstractitemmodel to a more generic one.
    connect(this, &SubtitleModel::columnsMoved, this, &SubtitleModel::modelChanged);
    connect(this, &SubtitleModel::columnsRemoved, this, &SubtitleModel::modelChanged);
    connect(this, &SubtitleModel::columnsInserted, this, &SubtitleModel::modelChanged);
    connect(this, &SubtitleModel::rowsMoved, this, &SubtitleModel::modelChanged);
    connect(this, &SubtitleModel::modelReset, this, [this] {
        if (!m_languageStateChanging) Q_EMIT modelChanged();
    });
}

void SubtitleModel::unsetModel()
{
    m_timeline.reset();
}

QByteArray SubtitleModel::guessFileEncoding(const QString &file, bool *confidence)
{
    QFile textFile{file};
    if (!textFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Could not open" << file;
        return "";
    }
    const QByteArray contents = textFile.readAll();
    if (contents.isEmpty()) {
        qWarning() << "Tried to guess the encoding of an empty file";
        return "";
    }

    QStringDecoder utf8Decoder(QStringDecoder::Utf8);
    utf8Decoder.decode(contents);
    if (!utf8Decoder.hasError()) {
        *confidence = true;
        return QByteArray("UTF-8");
    }

    KEncodingProber prober;
    const QByteArray sample = contents.first(1024);
    auto state = prober.feed(sample);
    *confidence = false;
    switch (state) {
    case KEncodingProber::ProberState::FoundIt:
        qDebug() << "Guessed subtitle file encoding to be " << prober.encoding() << ", confidence: " << prober.confidence();
        if (prober.confidence() < 0.6) {
            return QByteArray("UTF-8");
        }
        *confidence = true;
        break;
    case KEncodingProber::ProberState::NotMe:
        qWarning() << "Subtitle file encoding not recognized";
        return QByteArray("UTF-8");
    case KEncodingProber::ProberState::Probing:
        qWarning() << "Subtitle file encoding indeterminate, confidence is" << prober.confidence() << ", ENCODING: " << prober.encoding();
        if (prober.confidence() < 0.5) {
            // Encoding cannot be guessed, default to UTF-8
            return QByteArray(QByteArray("UTF-8"));
        }
        break;
    }
    return prober.encoding();
}

namespace {
bool sameImportedStyle(const SubtitleStyle &a, const SubtitleStyle &b)
{
    return a.fontName() == b.fontName() && a.fontSize() == b.fontSize() && a.primaryColour() == b.primaryColour() &&
           a.secondaryColour() == b.secondaryColour() && a.outlineColour() == b.outlineColour() && a.backColour() == b.backColour() && a.bold() == b.bold() &&
           a.italic() == b.italic() && a.underline() == b.underline() && a.strikeOut() == b.strikeOut() && a.scaleX() == b.scaleX() &&
           a.scaleY() == b.scaleY() && a.spacing() == b.spacing() && a.angle() == b.angle() && a.borderStyle() == b.borderStyle() &&
           a.outline() == b.outline() && a.shadow() == b.shadow() && a.alignment() == b.alignment() && a.marginL() == b.marginL() &&
           a.marginR() == b.marginR() && a.marginV() == b.marginV() && a.encoding() == b.encoding();
}
bool validImportedAssStyle(const QString &line)
{
    if (!line.startsWith(QStringLiteral("Style:"))) return false;
    const auto fields = line.section(QLatin1Char(':'), 1).trimmed().split(QLatin1Char(','));
    if (fields.size() != 23 || fields.at(0).trimmed().isEmpty() || fields.at(1).trimmed().isEmpty()) return false;
    static const QRegularExpression color(QStringLiteral("^&H[0-9a-fA-F]{8}&?$"), QRegularExpression::CaseInsensitiveOption);
    for (int index = 2; index < fields.size(); ++index) {
        if (index >= 3 && index <= 6) {
            if (!color.match(fields.at(index).trimmed()).hasMatch()) return false;
        } else {
            bool valid = false;
            const double number = fields.at(index).toDouble(&valid);
            if (!valid || !std::isfinite(number)) return false;
            if ((index >= 7 && index <= 10) || index == 15 || index >= 18) {
                fields.at(index).toInt(&valid);
                if (!valid) return false;
            }
        }
    }
    return true;
}
bool importedAssTime(const QString &value, double fps, double multiplier, GenTime &time)
{
    static const QRegularExpression grammar(QStringLiteral("^([0-9]{1,3}):([0-5][0-9]):([0-5][0-9])\\.([0-9]{2})$"));
    const auto match = grammar.match(value);
    if (!match.hasMatch() || match.captured(1).toInt() > 596) return false;
    const qint64 milliseconds = ((match.captured(1).toLongLong() * 60 + match.captured(2).toLongLong()) * 60 + match.captured(3).toLongLong()) * 1000 +
                                match.captured(4).toLongLong() * 10;
    const double frames = double(milliseconds) * fps / (1000.0 * multiplier);
    const double outputMilliseconds = double(milliseconds) / multiplier;
    if (!std::isfinite(frames) || frames < 0 || frames >= std::numeric_limits<int>::max() || outputMilliseconds >= std::numeric_limits<int>::max())
        return false;
    time = SubtitleEvent::stringtoTime(value, fps, multiplier);
    return true;
}
} // namespace

SubtitleModel::ImportMetadata SubtitleModel::captureImportMetadata() const
{
    return {m_subtitleStyles, m_scriptInfo, m_defaultStyles, fontSection, m_maxLayer, m_activeSubLayer};
}

void SubtitleModel::restoreImportMetadata(const ImportMetadata &metadata)
{
    // Event maps are intentionally untouched. Older commands and language-state
    // exchanges retain their original event nodes and native IDs.
    m_subtitleStyles = metadata.styles;
    m_scriptInfo = metadata.script;
    m_defaultStyles = metadata.defaultStyles;
    fontSection = metadata.fonts;
    m_maxLayer = metadata.maxLayer;
    m_activeSubLayer = metadata.activeLayer;
}

SubtitleModel::ImportResult SubtitleModel::importAssSubtitle(const QString &filePath, int offset, float startFramerate, float targetFramerate,
                                                             const QByteArray &encoding, int expectedCount)
{
    const auto rejected = [](const QString &reason) { return ImportResult{false, 0, reason}; };
    if (isLocked()) return rejected(QStringLiteral("Subtitles are locked."));
    const double fps = pCore->getCurrentFps();
    const double multiplier = targetFramerate / startFramerate;
    if (!std::isfinite(fps) || fps <= 0 || !std::isfinite(startFramerate) || startFramerate <= 0 || !std::isfinite(targetFramerate) || targetFramerate <= 0 ||
        !std::isfinite(multiplier) || multiplier <= 0 || expectedCount < -1)
        return rejected(QStringLiteral("Invalid subtitle frame rate or expected event count."));
    QFile file(filePath);
    constexpr qint64 maximumBytes = 16 * 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly)) return rejected(file.errorString());
    const QByteArray bytes = file.read(maximumBytes + 1);
    if (file.error() != QFileDevice::NoError || !file.atEnd() || bytes.size() > maximumBytes)
        return rejected(QStringLiteral("ASS import exceeds the 16 MiB input limit or cannot be read."));
    auto codec = QStringConverter::encodingForName(encoding.constData());
    if (!codec) return rejected(QStringLiteral("The subtitle codec is not recognized by this Qt build."));
    if (bytes.startsWith(QByteArray::fromHex("efbbbf")) || bytes.startsWith(QByteArray::fromHex("fffe")) || bytes.startsWith(QByteArray::fromHex("feff")) ||
        bytes.startsWith(QByteArray::fromHex("0000feff")))
        codec = QStringConverter::encodingForData(bytes);
    if (!codec) return rejected(QStringLiteral("Invalid ASS Unicode BOM."));
    QStringDecoder decoder(*codec, QStringConverter::Flag::Stateless);
    const QString contents = decoder.decode(bytes);
    if (decoder.hasError() || contents.contains(QChar::Null)) return rejected(QStringLiteral("ASS text decoding failed."));

    const auto before = std::make_shared<const ImportMetadata>(captureImportMetadata());
    ImportMetadata parsed = *before;
    struct ImportedEvent
    {
        int id;
        std::pair<int, GenTime> start;
        SubtitleEvent event;
    };
    std::vector<ImportedEvent> events;
    std::set<std::pair<int, int>> starts;
    QString section;
    bool turn = false;
    const auto growLayers = [&parsed](int layer) {
        parsed.maxLayer = qMax(parsed.maxLayer, layer);
        while (parsed.defaultStyles.size() <= parsed.maxLayer)
            parsed.defaultStyles.append(QStringLiteral("Default"));
    };
    const QString styleFormat = QStringLiteral("name,fontname,fontsize,primarycolour,secondarycolour,outlinecolour,backcolour,bold,italic,underline,"
                                               "strikeout,scalex,scaley,spacing,angle,borderstyle,outline,shadow,alignment,marginl,marginr,marginv,encoding");
    const QString eventFormat = QStringLiteral("layer,start,end,style,name,marginl,marginr,marginv,effect,text");
    for (const auto &raw : contents.split(QLatin1Char('\n'))) {
        if (raw.size() > 65536) return rejected(QStringLiteral("ASS records exceed the 65536 character limit."));
        const QString line = raw.simplified();
        if (line.isEmpty()) {
            turn = false;
            continue;
        }
        QString compact = line;
        compact.remove(QLatin1Char(' '));
        const bool sectionHeader =
            line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')) &&
            (compact.contains(QStringLiteral("ScriptInfo")) || compact.contains(QStringLiteral("KdenliveExtradata")) ||
             line.contains(QStringLiteral("Styles")) || line.contains(QStringLiteral("Fonts")) || line.contains(QStringLiteral("Events")));
        // The existing native section state changes only after a blank line.
        // Reject missing separators instead of swallowing a header (especially
        // Events inside Fonts) and committing a misleading metadata-only import.
        if (turn && sectionHeader) return rejected(QStringLiteral("ASS sections must be separated by a blank line."));
        if (!turn) {
            if (compact.contains(QStringLiteral("ScriptInfo")))
                section = QStringLiteral("script");
            else if (compact.contains(QStringLiteral("KdenliveExtradata")))
                section = QStringLiteral("extra");
            else if (line.contains(QStringLiteral("Styles")))
                section = QStringLiteral("styles");
            else if (line.contains(QStringLiteral("Fonts")))
                section = QStringLiteral("fonts");
            else if (line.contains(QStringLiteral("Events")))
                section = QStringLiteral("events");
            else
                section.clear();
            if (!section.isEmpty()) {
                turn = true;
                continue;
            }
        }
        if (section == QLatin1String("script")) {
            if (line.contains(QLatin1Char(':'))) parsed.script[line.section(QLatin1Char(':'), 0, 0).trimmed()] = line.section(QLatin1Char(':'), 1).trimmed();
        } else if (section == QLatin1String("extra")) {
            const auto fields = line.split(QLatin1Char(':'));
            if (fields.size() != 2) return rejected(QStringLiteral("Malformed ASS extra-data record."));
            if (fields.first().trimmed() == QLatin1String("MaxLayer")) {
                bool valid = false;
                const int layer = fields.last().trimmed().toInt(&valid);
                if (!valid || layer < 0 || layer > 50) return rejected(QStringLiteral("Invalid ASS maximum layer."));
                growLayers(layer);
            } else if (fields.first().trimmed() == QLatin1String("DefaultStyles")) {
                parsed.defaultStyles = fields.last().trimmed().split(QLatin1Char(','));
                if (parsed.defaultStyles.size() > 51) return rejected(QStringLiteral("Invalid ASS default-style count."));
                growLayers(parsed.maxLayer);
            }
        } else if (section == QLatin1String("styles")) {
            if (line.startsWith(QStringLiteral("Format:"))) {
                QString columns = line.section(QLatin1Char(':'), 1).toLower();
                columns.remove(QLatin1Char(' '));
                if (columns != styleFormat) return rejected(QStringLiteral("Unsupported ASS style column order."));
            } else {
                if (!validImportedAssStyle(line)) return rejected(QStringLiteral("Malformed ASS style record."));
                const QString name = line.section(QLatin1Char(':'), 1).trimmed().section(QLatin1Char(','), 0, 0);
                parsed.styles[name] = SubtitleStyle(line);
            }
        } else if (section == QLatin1String("fonts")) {
            parsed.fonts += line + QLatin1Char('\n');
            if (parsed.fonts.size() > maximumBytes) return rejected(QStringLiteral("ASS embedded font data exceeds the input limit."));
        } else if (section == QLatin1String("events")) {
            if (line.startsWith(QStringLiteral("Format:"))) {
                QString columns = line.section(QLatin1Char(':'), 1).toLower();
                columns.remove(QLatin1Char(' '));
                if (columns != eventFormat && columns != QString(eventFormat).replace(QStringLiteral(",name,"), QStringLiteral(",actor,")))
                    return rejected(QStringLiteral("Unsupported ASS event column order."));
                turn = true;
                continue;
            }
            if ((!line.startsWith(QStringLiteral("Dialogue:")) && !line.startsWith(QStringLiteral("Comment:"))) || line.contains(QStringLiteral("Format:")))
                return rejected(QStringLiteral("Malformed ASS event record."));
            const auto fields = line.section(QLatin1Char(':'), 1).split(QLatin1Char(','));
            if (fields.size() < 10) return rejected(QStringLiteral("Incomplete ASS event record."));
            bool valid = false;
            const int layer = fields.first().trimmed().toInt(&valid);
            GenTime first, last;
            if (!valid || layer < 0 || layer > 50 || !importedAssTime(fields.at(1), fps, multiplier, first) ||
                !importedAssTime(fields.at(2), fps, multiplier, last))
                return rejected(QStringLiteral("Invalid ASS event layer or timestamp."));
            for (int index = 5; index <= 7; ++index) {
                fields.at(index).toInt(&valid);
                if (!valid) return rejected(QStringLiteral("Invalid ASS event margin."));
            }
            const qint64 firstFrame = qint64(first.frames(fps)) + offset, lastFrame = qint64(last.frames(fps)) + offset;
            if (firstFrame < 0 || lastFrame <= firstFrame || lastFrame >= std::numeric_limits<int>::max() ||
                GenTime(int(lastFrame), fps).ms() >= std::numeric_limits<int>::max())
                return rejected(QStringLiteral("Invalid ASS event range after frame conversion and offset."));
            const std::pair<int, int> key(layer, int(firstFrame));
            if (!starts.insert(key).second || getIdForStartPos(layer, GenTime(int(firstFrame), fps)) >= 0)
                return rejected(QStringLiteral("ASS events collide with an existing start in the same layer."));
            QString nativeLine = line;
            nativeLine.replace(QStringLiteral("\\N"), QStringLiteral("\n"));
            std::pair<int, GenTime> start;
            SubtitleEvent event(nativeLine, fps, multiplier, &start);
            if (event.endTime() == GenTime()) return rejected(QStringLiteral("The native ASS event constructor rejected a validated record."));
            start.second += GenTime(offset, fps);
            event.setEndTime(event.endTime() + GenTime(offset, fps));
            event.setText(event.text().trimmed());
            events.push_back({-1, start, event});
            if (events.size() > 10000) return rejected(QStringLiteral("ASS imports exceed the 10000 event limit."));
            growLayers(layer);
        }
        turn = true;
    }
    if (expectedCount >= 0 && int(events.size()) != expectedCount) return rejected(QStringLiteral("The ASS event count does not match the validated file."));
    bool sameStyles = parsed.styles.size() == before->styles.size();
    if (sameStyles) {
        for (const auto &entry : parsed.styles) {
            const auto old = before->styles.find(entry.first);
            if (old == before->styles.end() || !sameImportedStyle(old->second, entry.second)) {
                sameStyles = false;
                break;
            }
        }
    }
    const bool sameMetadata = sameStyles && parsed.script == before->script && parsed.fonts == before->fonts && parsed.defaultStyles == before->defaultStyles &&
                              parsed.maxLayer == before->maxLayer && parsed.activeLayer == before->activeLayer;
    if (events.empty() && sameMetadata) return rejected(QStringLiteral("The ASS file contains no new events or metadata changes."));
    // Only fully validated files allocate IDs. These are reused by every redo.
    for (auto &entry : events)
        entry.id = TimelineModel::getNextId();
    const auto imported = std::make_shared<const std::vector<ImportedEvent>>(std::move(events));
    const auto after = std::make_shared<const ImportMetadata>(std::move(parsed));
    const int language =
        pCore->currentDoc()->getSequenceProperty(m_timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt();
    const auto context = [this, language] {
        return !isLocked() && pCore->currentDoc() &&
               pCore->currentDoc()->getSequenceProperty(m_timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt() ==
                   language;
    };
    const auto notify = [this] {
        Q_EMIT m_timeline->maxSubLayerChanged();
        Q_EMIT activeSubLayerChanged();
        Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
    };
    Fun redo = [this, imported, before, after, context, notify] {
        QWriteLocker locker(&m_lock);
        if (!context()) return false;
        for (const auto &entry : *imported)
            if (hasSubtitle(entry.id) || m_subtitleList.count(entry.start) > 0) return false;
        restoreImportMetadata(*after);
        size_t added = 0;
        for (const auto &entry : *imported) {
            if (!addSubtitle(entry.id, entry.start, entry.event, false, false)) {
                bool restored = true;
                while (added > 0)
                    restored = removeSubtitle(imported->at(--added).id, false, false) && restored;
                restoreImportMetadata(*before);
                notify();
                Q_ASSERT(restored);
                return false;
            }
            ++added;
        }
        notify();
        return true;
    };
    Fun undo = [this, imported, before, after, context, notify] {
        QWriteLocker locker(&m_lock);
        if (!context()) return false;
        for (const auto &entry : *imported)
            if (!hasSubtitle(entry.id) || m_allSubtitles.at(entry.id) != entry.start || m_timeline->m_groups->isInGroup(entry.id)) return false;
        size_t removed = 0;
        for (auto entry = imported->rbegin(); entry != imported->rend(); ++entry) {
            if (!removeSubtitle(entry->id, false, false)) {
                bool restored = true;
                for (size_t index = imported->size() - removed; index < imported->size(); ++index) {
                    const auto &old = imported->at(index);
                    restored = addSubtitle(old.id, old.start, old.event, false, false) && restored;
                }
                restoreImportMetadata(*after);
                notify();
                Q_ASSERT(restored);
                return false;
            }
            ++removed;
        }
        restoreImportMetadata(*before);
        notify();
        return true;
    };
    if (!redo()) return rejected(QStringLiteral("The native ASS transaction failed before committing to history."));
    pCore->pushUndo(undo, redo, i18n("Edit subtitle"));
    return {true, int(imported->size()), {}};
}

void SubtitleModel::importSubtitle(const QString &filePath, int offset, bool externalImport, float startFramerate, float targetFramerate,
                                   const QByteArray &encoding)
{
    const auto result = importSubtitleWithUndo(filePath, offset, externalImport, startFramerate, targetFramerate, encoding);
    if (externalImport && !result.success) {
        pCore->displayMessage(i18n("The selected subtitle file %1 is invalid: %2", filePath, result.error), ErrorMessage);
    }
}

SubtitleModel::ImportResult SubtitleModel::importSubtitleWithUndo(const QString &filePath, int offset, bool externalImport, float startFramerate,
                                                                  float targetFramerate, const QByteArray &encoding, int expectedCount)
{
    if (externalImport && filePath.endsWith(QLatin1String(".ass"), Qt::CaseInsensitive)) {
        return importAssSubtitle(filePath, offset, startFramerate, targetFramerate, encoding, expectedCount);
    }
    QString start, end, comment;
    QString timeLine;
    GenTime startPos, endPos;
    int turn = 0, r = 0, endIndex = 1, defaultTurn = 0;
    double transformMult = targetFramerate / startFramerate;
    /*
     * turn = 0 -> Parse next subtitle line [srt] (or) [vtt] (or) [sbv] (or) Parse next section [ssa]
     * turn = 1 -> Add string to timeLine
     * turn > 1 -> Add string to completeLine
     */
    // Loading a language is an internal state exchange, not a user edit. Keep
    // the real lock observable throughout, while allowing only this scoped load.
    if (filePath.isEmpty() || (isLocked() && !m_languageStateChanging)) return {false, 0, QStringLiteral("Subtitle path is empty or the model is locked.")};
    Fun redo = []() { return true; };
    Fun undo = [this]() {
        Q_EMIT modelChanged();
        return true;
    };
    ulong initialCount = m_subtitleList.size();
    GenTime subtitleOffset(offset, pCore->getCurrentFps());

    if (!externalImport) {
        // initialize the subtitle
        m_scriptInfo.clear();
        m_subtitleStyles.clear();
        fontSection.clear();
        m_defaultStyles.clear();
        setMaxLayer(0);

        QSize frameSize = pCore->getCurrentFrameDisplaySize();
        double fontSize = frameSize.height() / 18;
        double fontMargin = frameSize.height() / 27;

        m_scriptInfo["ScriptType"] = "v4.00+";
        m_scriptInfo["PlayResX"] = QString::number(frameSize.width());
        m_scriptInfo["PlayResY"] = QString::number(frameSize.height());
        m_scriptInfo["LayoutResX"] = QString::number(frameSize.width());
        m_scriptInfo["LayoutResY"] = QString::number(frameSize.height());
        m_scriptInfo["WrapStyle"] = "0";
        m_scriptInfo["ScaledBorderAndShadow"] = "yes";
        m_scriptInfo["YCbCr Matrix"] = "None";

        m_subtitleStyles["Default"] = SubtitleStyle();
        m_subtitleStyles["Default"].setFontSize(fontSize);
        m_subtitleStyles["Default"].setMarginV(fontMargin);
    }

    if (filePath.endsWith(".srt") || filePath.endsWith(".vtt") || filePath.endsWith(".sbv")) {
        // if (!filePath.endsWith(".vtt") || !filePath.endsWith(".sbv")) {defaultTurn = -10;}
        if (filePath.endsWith(".vtt") || filePath.endsWith(".sbv")) {
            defaultTurn = -10;
            turn = defaultTurn;
        }
        endIndex = filePath.endsWith(".sbv") ? 1 : 2;
        QFile srtFile(filePath);
        if (!srtFile.exists() || !srtFile.open(QIODevice::ReadOnly)) {
            qDebug() << " File not found " << filePath;
            return {false, 0, QStringLiteral("Cannot read the subtitle file.")};
        }

        qDebug() << "srt/vtt/sbv File";
        // parsing srt file
        QTextStream stream(&srtFile);
        std::optional<QStringConverter::Encoding> inputEncoding = QStringConverter::encodingForName(encoding.data());
        if (inputEncoding) {
            stream.setEncoding(inputEncoding.value());
        }
        // else: UTF8 is the default
        QString line;
        QStringList srtTime;
        static const QRegularExpression rx("([0-9]{1,2}):([0-9]{2})");
        QLatin1Char separator = filePath.endsWith(".sbv") ? QLatin1Char(',') : QLatin1Char(' ');
        while (stream.readLineInto(&line)) {
            line = line.trimmed();
            if (!line.isEmpty()) {
                if (!turn) {
                    // index=atoi(line.toStdString().c_str());
                    turn++;
                    continue;
                }
                // Check if position has already been read
                if (endPos == startPos && (line.contains(QLatin1String("-->")) || line.contains(rx))) {
                    timeLine += line;
                    srtTime = timeLine.split(separator);
                    if (srtTime.count() > endIndex) {
                        start = srtTime.at(0);
                        startPos = SubtitleEvent::stringtoTime(start, pCore->getCurrentFps(), transformMult);
                        end = srtTime.at(endIndex);
                        endPos = SubtitleEvent::stringtoTime(end, pCore->getCurrentFps(), transformMult);
                    } else {
                        continue;
                    }
                } else {
                    r++;
                    if (!comment.isEmpty()) comment += " ";
                    if (r == 1)
                        comment += line;
                    else
                        comment = comment + "\n" + line;
                }
                turn++;
            } else {
                if (endPos > startPos) {
                    addSubtitle({0, startPos + subtitleOffset}, SubtitleEvent(true, endPos + subtitleOffset, "Default", "", 0, 0, 0, "", comment), undo, redo,
                                false);
                } else {
                    qDebug() << "===== INVALID SUBTITLE FOUND: " << start << "-" << end << ", " << comment;
                }
                // reinitialize
                comment.clear();
                timeLine.clear();
                startPos = endPos;
                r = 0;
                turn = defaultTurn;
            }
        }
        // Ensure last subtitle is read
        if (endPos > startPos && !comment.isEmpty()) {
            addSubtitle({0, startPos + subtitleOffset}, SubtitleEvent(true, endPos + subtitleOffset, "Default", "", 0, 0, 0, "", comment), undo, redo, false);
        }
        srtFile.close();
    } else if (filePath.endsWith(QLatin1String(".ass"))) {
        qDebug() << "ass File";
        QString startTime, endTime;
        QString section;
        turn = 0;
        QFile assFile(filePath);
        if (!assFile.exists() || !assFile.open(QIODevice::ReadOnly)) {
            qDebug() << " Failed attempt on opening " << filePath;
            return {false, 0, QStringLiteral("Cannot read the subtitle file.")};
        }
        QTextStream stream(&assFile);
        std::optional<QStringConverter::Encoding> inputEncoding = QStringConverter::encodingForName(encoding.data());
        if (inputEncoding) {
            stream.setEncoding(inputEncoding.value());
        }
        // else: UTF8 is the default
        QString line;
        qDebug() << " correct ass file  " << filePath;
        while (stream.readLineInto(&line)) {
            line = line.simplified();
            if (!line.isEmpty()) {
                if (!turn) {
                    // check if it is script info, event,or v4+ style
                    QString linespace = line;
                    if (linespace.replace(" ", "").contains("ScriptInfo")) {
                        section = "Script Info";
                        turn++;
                        continue;
                    } else if (linespace.contains("KdenliveExtradata")) {
                        section = "Kdenlive Extradata";
                        turn++;
                        continue;
                    } else if (line.contains("Styles")) {
                        section = "V4+ Styles";
                        turn++;
                        continue;
                    } else if (line.contains("Fonts")) {
                        section = "Fonts";
                        turn++;
                        continue;
                    } else if (line.contains("Events")) {
                        turn++;
                        section = "Events";
                        continue;
                    } else {
                        // unknown section
                        section.clear();
                    }
                }
                if (section.contains("Script Info")) {
                    QStringList scriptInfo = line.split(":");
                    if (scriptInfo.count() > 1) {
                        m_scriptInfo[line.section(":", 0, 0).trimmed()] = line.section(":", 1).trimmed();
                    }
                }
                if (section.contains("Kdenlive Extradata")) {
                    QStringList extraData = line.split(":");
                    if (extraData[0].trimmed() == "MaxLayer") {
                        if (extraData[1].toInt() > m_maxLayer) setMaxLayer(extraData[1].toInt());
                    } else if (extraData[0].trimmed() == "DefaultStyles") {
                        const QStringList defaultStyles = extraData[1].trimmed().split(",");
                        m_defaultStyles.clear();
                        for (const QString &style : defaultStyles) {
                            m_defaultStyles << style;
                        }
                        // Fill the default styles with "Default" if not enough styles are defined
                        for (int i = m_defaultStyles.size(); i < m_maxLayer + 1; i++) {
                            m_defaultStyles << "Default";
                        }
                    }
                }
                if (section.contains("V4+ Styles")) {
                    if (!line.contains("Format:")) {
                        m_subtitleStyles[line.section(":", 1).trimmed().split(",").at(0)] = SubtitleStyle(line);
                    }
                }
                if (section.contains("Fonts")) {
                    fontSection += line + "\n";
                }
                if (section.contains("Events")) {
                    // if it is event
                    if (!line.contains("Format:")) {
                        start.clear();
                        end.clear();
                        comment.clear();
                        std::pair<int, GenTime> start;
                        line.replace(QStringLiteral("\\N"), QStringLiteral("\n"));
                        SubtitleEvent event(line, pCore->getCurrentFps(), transformMult, &start);
                        // check if event successfully parsed
                        if (event.endTime() != GenTime()) {
                            if (start.first > m_maxLayer) {
                                setMaxLayer(start.first);
                            }
                            start.second += subtitleOffset;
                            event.setEndTime(event.endTime() + subtitleOffset);
                            addSubtitle(start, event, undo, redo, false);
                        } else {
                            qDebug() << "==== FOUND INVALID SUBTITLE ITEM: " << line;
                        }
                    }
                }
                turn++;
            } else {
                turn = 0;
                startTime = endTime = QString();
            }
        }
        assFile.close();
    } else {
        //   reinitialize for next comment:
        comment.clear();
        timeLine.clear();
        // event.clear();
        turn = 0;
        r = 0;
    }
    if (initialCount == m_subtitleList.size() && externalImport) {
        // Nothing imported
        return {false, 0, QStringLiteral("No subtitle events were imported.")};
    }
    const int importedCount = int(m_subtitleList.size() - initialCount);
    if (externalImport && expectedCount >= 0 && importedCount != expectedCount) {
        // No command has been pushed: the prior redo branch remains owned by
        // the stack. These legacy text formats have no file-level metadata.
        const bool restored = undo() && m_subtitleList.size() == initialCount;
        return {false, 0,
                restored ? QStringLiteral("Native subtitle event count did not match the validated file.")
                         : QStringLiteral("Native subtitle rollback failed.")};
    }
    Fun update_model = [this]() {
        Q_EMIT modelChanged();
        return true;
    };
    PUSH_LAMBDA(update_model, redo);
    update_model();
    if (externalImport) {
        pCore->pushUndo(undo, redo, i18n("Edit subtitle"));
    }
    return {true, importedCount, {}};
}

void SubtitleModel::parseSubtitle(const QString &workPath)
{
    qDebug() << "Parsing started for: " << workPath;
    if (!workPath.isEmpty()) {
        m_subtitleFilter->set("av.filename", workPath.toUtf8().constData());
    }
    QString filePath = m_subtitleFilter->get("av.filename");
    importSubtitle(filePath, 0, false);
    // jsontoSubtitle(toJson());
}

const QString SubtitleModel::getUrl()
{
    return m_subtitleFilter->get("av.filename");
}

bool SubtitleModel::addSubtitle(std::pair<int, GenTime> start, const SubtitleEvent &event, Fun &undo, Fun &redo, bool updateFilter)
{
    if (isLocked() && !m_languageStateChanging) {
        return false;
    }
    int id = TimelineModel::getNextId();
    Fun local_redo = [this, id, start, event, updateFilter]() {
        addSubtitle(id, start, event, false, updateFilter);
        QPair<int, int> range = {start.second.frames(pCore->getCurrentFps()), event.endTime().frames(pCore->getCurrentFps())};
        pCore->invalidateRange(range);
        pCore->refreshProjectRange(range);
        return true;
    };
    Fun local_undo = [this, id, start, event, updateFilter]() {
        removeSubtitle(id, false, updateFilter);
        QPair<int, int> range = {start.second.frames(pCore->getCurrentFps()), event.endTime().frames(pCore->getCurrentFps())};
        pCore->invalidateRange(range);
        pCore->refreshProjectRange(range);
        return true;
    };
    local_redo();
    UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
    return true;
}

bool SubtitleModel::addSubtitle(int id, std::pair<int, GenTime> start, const SubtitleEvent &event, bool temporary, bool updateFilter)
{
    if (start.second.frames(pCore->getCurrentFps()) < 0 || event.endTime().frames(pCore->getCurrentFps()) < 0 || (isLocked() && !m_languageStateChanging)) {
        qDebug() << "Time error: is negative";
        return false;
    }
    if (start.second.frames(pCore->getCurrentFps()) > event.endTime().frames(pCore->getCurrentFps())) {
        qDebug() << "Time error: start should be less than end";
        return false;
    }
    // Don't allow 2 subtitles at same start pos
    if (m_subtitleList.count(start) > 0) {
        qDebug() << "already present in model"
                 << "string :" << m_subtitleList[start].text() << " start time " << start.second.frames(pCore->getCurrentFps())
                 << "end time : " << m_subtitleList[start].endTime().frames(pCore->getCurrentFps());
        return false;
    }
    if (start.first > m_maxLayer) {
        qDebug() << "Layer not allowed";
        return false;
    }
    if (start.first < 0) {
        qDebug() << "negative layer";
        return false;
    }
    registerSubtitle(id, start, temporary);
    int row = getSubtitleIndex(id);
    beginInsertRows(QModelIndex(), row, row);

    m_subtitleList[start] = event;
    // Strip all leading and trailing whitespaces from the text, to e.g. avoid bogus exports with
    // leading newlines to SRT.
    m_subtitleList[start].setText(event.text().trimmed());

    endInsertRows();
    addSnapPoint(start.second);
    addSnapPoint(event.endTime()); // {layer, end}
    if (!temporary && event.endTime().frames(pCore->getCurrentFps()) > m_timeline->duration()) {
        m_timeline->updateDuration();
    }
    QPair<int, int> range = {start.second.frames(pCore->getCurrentFps()), event.endTime().frames(pCore->getCurrentFps())};
    pCore->invalidateRange(range);
    pCore->refreshProjectRange(range);
    if (updateFilter) {
        Q_EMIT modelChanged();
    }
    return true;
}

QHash<int, QByteArray> SubtitleModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[SubtitleRole] = "subtitle";
    roles[IdRole] = "id";
    roles[StartPosRole] = "startposition";
    roles[EndPosRole] = "endposition";
    roles[StartFrameRole] = "startframe";
    roles[FakeStartFrameRole] = "fakeStart";
    roles[EndFrameRole] = "endframe";
    roles[LayerRole] = "layer";
    roles[StyleNameRole] = "styleName";
    roles[NameRole] = "name";
    roles[MarginLRole] = "marginL";
    roles[MarginRRole] = "marginR";
    roles[MarginVRole] = "marginV";
    roles[EffectRole] = "effect";
    roles[IsDialogueRole] = "isDialogue";
    roles[GrabRole] = "grabbed";
    roles[SelectedRole] = "selected";
    return roles;
}

QVariant SubtitleModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= static_cast<int>(m_subtitleList.size()) || !index.isValid()) {
        return QVariant();
    }
    auto subInfo = getSubtitleIdFromIndex(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
    case SubtitleRole:
        return m_subtitleList.at(subInfo.second).text();
    case IdRole:
        return subInfo.first;
    case LayerRole:
        return subInfo.second.first;
    case StartPosRole:
        return subInfo.second.second.seconds();
    case EndPosRole:
        return m_subtitleList.at(subInfo.second).endTime().seconds();
    case StartFrameRole:
        return subInfo.second.second.frames(pCore->getCurrentFps());
    case EndFrameRole:
        return m_subtitleList.at(subInfo.second).endTime().frames(pCore->getCurrentFps());
    case StyleNameRole:
        return m_subtitleList.at(subInfo.second).styleName();
    case NameRole:
        return m_subtitleList.at(subInfo.second).name();
    case MarginLRole:
        return m_subtitleList.at(subInfo.second).marginL();
    case MarginRRole:
        return m_subtitleList.at(subInfo.second).marginR();
    case MarginVRole:
        return m_subtitleList.at(subInfo.second).marginV();
    case EffectRole:
        return m_subtitleList.at(subInfo.second).effect();
    case IsDialogueRole:
        return m_subtitleList.at(subInfo.second).isDialogue();
    case SelectedRole:
        return m_selected.contains(subInfo.first);
    case FakeStartFrameRole:
        return getSubtitleFakePosFromIndex(index.row());
    case GrabRole:
        return m_grabbedIds.contains(subInfo.first);
    }
    return QVariant();
}

int SubtitleModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return static_cast<int>(m_subtitleList.size());
}

const QList<std::pair<std::pair<int, GenTime>, SubtitleEvent>> SubtitleModel::getAllSubtitles() const
{
    QList<std::pair<std::pair<int, GenTime>, SubtitleEvent>> allSubtitles;
    for (const auto &subtitles : m_subtitleList) {
        allSubtitles << subtitles;
    }
    return allSubtitles;
}

SubtitleEvent SubtitleModel::getSubtitle(int layer, GenTime startpos) const
{
    for (const auto &subtitles : m_subtitleList) {
        if (subtitles.first == std::make_pair(layer, startpos)) {
            return subtitles.second;
        }
    }
    return SubtitleEvent();
}

QString SubtitleModel::getText(int id) const
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) {
        return QString();
    }
    std::pair<int, GenTime> start = m_allSubtitles.at(id);
    return m_subtitleList.at(start).text();
}

bool SubtitleModel::setText(int id, const QString &text)
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end() || isLocked()) {
        return false;
    }
    std::pair<int, GenTime> start = m_allSubtitles.at(id);
    GenTime end = m_subtitleList.at(start).endTime();
    QString oldText = m_subtitleList.at(start).text();
    m_subtitleList[start].setText(text);
    Fun local_redo = [this, start, id, end, text]() {
        editSubtitle(id, text);
        QPair<int, int> range = {start.second.frames(pCore->getCurrentFps()), end.frames(pCore->getCurrentFps())};
        pCore->invalidateRange(range);
        pCore->refreshProjectRange(range);
        return true;
    };
    Fun local_undo = [this, start, id, end, oldText]() {
        editSubtitle(id, oldText);
        QPair<int, int> range = {start.second.frames(pCore->getCurrentFps()), end.frames(pCore->getCurrentFps())};
        pCore->invalidateRange(range);
        pCore->refreshProjectRange(range);
        return true;
    };
    local_redo();
    pCore->pushUndo(local_undo, local_redo, i18n("Edit subtitle"));
    return true;
}

std::unordered_set<int> SubtitleModel::getItemsInRange(int layer, int startFrame, int endFrame) const
{
    if (isLocked()) {
        return {};
    }
    GenTime startTime(startFrame, pCore->getCurrentFps());
    GenTime endTime(endFrame, pCore->getCurrentFps());
    std::unordered_set<int> matching;
    for (const auto &subtitles : m_subtitleList) {
        // if layer is -1, we check all layers
        if ((endFrame > -1 && subtitles.first.second > endTime) || (layer != -1 && subtitles.first.first != layer)) {
            // Outside range
            continue;
        }
        if (subtitles.first.second >= startTime || subtitles.second.endTime() > startTime) {
            int sid = getIdForStartPos(layer, subtitles.first.second);
            if (sid > -1) {
                matching.emplace(sid);
            } else {
                qDebug() << "==== FOUND INVALID SUBTITLE AT: " << subtitles.first.second.frames(pCore->getCurrentFps());
            }
        }
    }
    return matching;
}

bool SubtitleModel::cutSubtitle(int layer, int position)
{
    Fun redo = []() { return true; };
    Fun undo = []() { return true; };
    if (cutSubtitle(layer, position, undo, redo) > -1) {
        pCore->pushUndo(undo, redo, i18n("Cut clip"));
        return true;
    }
    return false;
}

int SubtitleModel::cutSubtitle(int layer, int position, Fun &undo, Fun &redo, bool ignoreCutMode)
{
    if (isLocked()) {
        return -1;
    }
    GenTime pos(position, pCore->getCurrentFps());
    GenTime start = GenTime(-1);
    for (const auto &subtitles : m_subtitleList) {
        if (subtitles.first.second <= pos && subtitles.second.endTime() > pos && subtitles.first.first == layer) {
            start = subtitles.first.second;
            break;
        }
    }
    if (start >= GenTime()) {
        const SubtitleEvent originalEvent = m_subtitleList.at({layer, start});
        QString originalText = originalEvent.text();
        QString leftText, rightText;

        if (KdenliveSettings::subtitle_razor_mode() == RAZOR_MODE_DUPLICATE || ignoreCutMode) {
            leftText = originalText;
            rightText = originalText;
        } else if (KdenliveSettings::subtitle_razor_mode() == RAZOR_MODE_AFTER_FIRST_LINE) {
            static const QRegularExpression newlineRe("\\n");
            QRegularExpressionMatch newlineMatch = newlineRe.match(originalText);
            if (!newlineMatch.hasMatch()) {
                undo();
                return -1;
            } else {
                leftText = originalText;
                leftText.truncate(newlineMatch.capturedStart());
                rightText = originalText.right(originalText.length() - newlineMatch.capturedEnd());
            }
        } else {
            undo();
            return -1;
        }

        int subId = getIdForStartPos(layer, start);
        int duration = position - start.frames(pCore->getCurrentFps());
        bool res = requestResize(subId, duration, true, undo, redo, false);
        if (res) {
            int id = TimelineModel::getNextId();
            bool refreshMonitor = leftText != rightText;
            Fun local_redo = [this, id, layer, pos, originalEvent, subId, leftText, rightText, refreshMonitor]() {
                editSubtitle(subId, leftText);
                bool result =
                    addSubtitle(id, {layer, pos},
                                SubtitleEvent(originalEvent.isDialogue(), originalEvent.endTime(), originalEvent.styleName(), originalEvent.name(),
                                              originalEvent.marginL(), originalEvent.marginR(), originalEvent.marginV(), originalEvent.effect(), rightText));
                if (refreshMonitor) {
                    ObjectId owner(KdenliveObjectType::TimelineSubtitle, id, m_timeline->uuid());
                    pCore->invalidateItem(owner);
                    pCore->refreshProjectItem(owner);
                }
                return result;
            };
            Fun local_undo = [this, id, subId, originalText, refreshMonitor]() {
                editSubtitle(subId, originalText);
                removeSubtitle(id);
                if (refreshMonitor) {
                    ObjectId owner(KdenliveObjectType::TimelineSubtitle, subId, m_timeline->uuid());
                    pCore->invalidateItem(owner);
                    pCore->refreshProjectItem(owner);
                }
                return true;
            };
            if (local_redo()) {
                PUSH_LAMBDA(local_redo, redo);
                PUSH_LAMBDA(local_undo, undo);
                return id;
            }
        }
    }
    undo();
    return -1;
}

void SubtitleModel::registerSnap(const std::weak_ptr<SnapInterface> &snapModel)
{
    if (auto ptr = snapModel.lock()) {
        // Each observer owns one reference per subtitle endpoint. Registering
        // the same model twice would leave doubled native snap refcounts.
        for (const auto &registered : m_regSnaps) {
            if (registered.lock() == ptr) return;
        }
        m_regSnaps.push_back(snapModel);
        for (const auto &subtitle : m_subtitleList) {
            ptr->addPoint(subtitle.first.second.frames(pCore->getCurrentFps()));
            ptr->addPoint(subtitle.second.endTime().frames(pCore->getCurrentFps()));
        }
    } else {
        qDebug() << "Error: added snapmodel for subtitle is null";
        Q_ASSERT(false);
    }
}

void SubtitleModel::addSnapPoint(GenTime startpos)
{
    std::vector<std::weak_ptr<SnapInterface>> validSnapModels;
    Q_ASSERT(m_regSnaps.size() > 0);
    for (const auto &snapModel : m_regSnaps) {
        if (auto ptr = snapModel.lock()) {
            validSnapModels.push_back(snapModel);
            ptr->addPoint(startpos.frames(pCore->getCurrentFps()));
        }
    }
    // Update the list of snapModel known to be valid
    std::swap(m_regSnaps, validSnapModels);
}

void SubtitleModel::removeSnapPoint(GenTime startpos)
{
    std::vector<std::weak_ptr<SnapInterface>> validSnapModels;
    for (const auto &snapModel : m_regSnaps) {
        if (auto ptr = snapModel.lock()) {
            validSnapModels.push_back(snapModel);
            ptr->removePoint(startpos.frames(pCore->getCurrentFps()));
        }
    }
    // Update the list of snapModel known to be valid
    std::swap(m_regSnaps, validSnapModels);
}

void SubtitleModel::editEndPos(int layer, GenTime startPos, GenTime newEndPos, bool refreshModel)
{
    qDebug() << "Changing the sub end timings in model";
    if (m_subtitleList.count({layer, startPos}) <= 0) {
        // is not present in model only
        return;
    }
    m_subtitleList[{layer, startPos}].setEndTime(newEndPos);
    // Trigger update of the qml view
    int id = getIdForStartPos(layer, startPos);
    int row = getSubtitleIndex(id);
    Q_EMIT dataChanged(index(row), index(row), {EndFrameRole});
    if (refreshModel) {
        Q_EMIT modelChanged();
    }
    qDebug() << startPos.frames(pCore->getCurrentFps()) << m_subtitleList[{layer, startPos}].endTime().frames(pCore->getCurrentFps());
}

void SubtitleModel::switchGrab(int sid)
{
    if (m_grabbedIds.contains(sid)) {
        m_grabbedIds.removeAll(sid);
    } else {
        m_grabbedIds << sid;
    }
    int row = getSubtitleIndex(sid);
    Q_EMIT dataChanged(index(row), index(row), {GrabRole});
}

void SubtitleModel::clearGrab()
{
    QVector<int> grabbed = m_grabbedIds;
    m_grabbedIds.clear();
    for (int sid : grabbed) {
        int row = getSubtitleIndex(sid);
        Q_EMIT dataChanged(index(row), index(row), {GrabRole});
    }
}

bool SubtitleModel::isGrabbed(int id) const
{
    return m_grabbedIds.contains(id);
}

bool SubtitleModel::requestResize(int id, int size, bool right)
{
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    bool res = requestResize(id, size, right, undo, redo, true);
    if (res) {
        pCore->pushUndo(undo, redo, i18n("Resize subtitle"));
        return true;
    } else {
        undo();
        return false;
    }
}

bool SubtitleModel::requestResize(int id, int size, bool right, Fun &undo, Fun &redo, bool logUndo)
{
    if (isLocked()) {
        return false;
    }
    Q_ASSERT(m_allSubtitles.find(id) != m_allSubtitles.end());
    std::pair<int, GenTime> startPos = m_allSubtitles.at(id);
    GenTime endPos = m_subtitleList.at(startPos).endTime();
    Fun operation = []() { return true; };
    Fun reverse = []() { return true; };
    if (right) {
        GenTime newEndPos = startPos.second + GenTime(size, pCore->getCurrentFps());
        operation = [this, id, startPos, endPos, newEndPos, logUndo]() {
            m_subtitleList[startPos].setEndTime(newEndPos);
            removeSnapPoint(endPos);
            addSnapPoint(newEndPos);
            // Trigger update of the qml view
            int row = getSubtitleIndex(id);
            Q_EMIT dataChanged(index(row), index(row), {EndFrameRole});
            if (logUndo) {
                Q_EMIT modelChanged();
                QPair<int, int> range;
                if (endPos > newEndPos) {
                    range = {newEndPos.frames(pCore->getCurrentFps()), endPos.frames(pCore->getCurrentFps())};
                } else {
                    range = {endPos.frames(pCore->getCurrentFps()), newEndPos.frames(pCore->getCurrentFps())};
                }
                pCore->invalidateRange(range);
                pCore->refreshProjectRange(range);
            }
            return true;
        };
        reverse = [this, id, startPos, endPos, newEndPos, logUndo]() {
            m_subtitleList[startPos].setEndTime(endPos);
            removeSnapPoint(newEndPos);
            addSnapPoint(endPos);
            // Trigger update of the qml view
            int row = getSubtitleIndex(id);
            Q_EMIT dataChanged(index(row), index(row), {EndFrameRole});
            if (logUndo) {
                Q_EMIT modelChanged();
                QPair<int, int> range;
                if (endPos > newEndPos) {
                    range = {newEndPos.frames(pCore->getCurrentFps()), endPos.frames(pCore->getCurrentFps())};
                } else {
                    range = {endPos.frames(pCore->getCurrentFps()), newEndPos.frames(pCore->getCurrentFps())};
                }
                pCore->invalidateRange(range);
                pCore->refreshProjectRange(range);
            }
            return true;
        };
    } else {
        std::pair<int, GenTime> newStartPos = {startPos.first, endPos - GenTime(size, pCore->getCurrentFps())};
        if (m_subtitleList.count(newStartPos) > 0) {
            // There already is another subtitle at this position, abort
            return false;
        }
        const SubtitleEvent event = m_subtitleList.at(startPos);
        operation = [this, id, startPos, newStartPos, event, logUndo]() {
            m_allSubtitles[id] = newStartPos;
            m_subtitleList.erase(startPos);
            m_subtitleList[newStartPos] = event;
            // Trigger update of the qml view
            removeSnapPoint(startPos.second);
            addSnapPoint(newStartPos.second);
            int row = getSubtitleIndex(id);
            Q_EMIT dataChanged(index(row), index(row), {StartFrameRole});
            if (logUndo) {
                Q_EMIT modelChanged();
                QPair<int, int> range;
                if (startPos > newStartPos) {
                    range = {newStartPos.second.frames(pCore->getCurrentFps()), startPos.second.frames(pCore->getCurrentFps())};
                } else {
                    range = {startPos.second.frames(pCore->getCurrentFps()), newStartPos.second.frames(pCore->getCurrentFps())};
                }
                pCore->invalidateRange(range);
                pCore->refreshProjectRange(range);
            }
            return true;
        };
        reverse = [this, id, startPos, newStartPos, event, logUndo]() {
            m_allSubtitles[id] = startPos;
            m_subtitleList.erase(newStartPos);
            m_subtitleList[startPos] = event;
            removeSnapPoint(newStartPos.second);
            addSnapPoint(startPos.second);
            // Trigger update of the qml view
            int row = getSubtitleIndex(id);
            Q_EMIT dataChanged(index(row), index(row), {StartFrameRole});
            if (logUndo) {
                Q_EMIT modelChanged();
                QPair<int, int> range;
                if (startPos > newStartPos) {
                    range = {newStartPos.second.frames(pCore->getCurrentFps()), startPos.second.frames(pCore->getCurrentFps())};
                } else {
                    range = {startPos.second.frames(pCore->getCurrentFps()), newStartPos.second.frames(pCore->getCurrentFps())};
                }
                pCore->invalidateRange(range);
                pCore->refreshProjectRange(range);
            }
            return true;
        };
    }
    operation();
    UPDATE_UNDO_REDO(operation, reverse, undo, redo);
    return true;
}

bool SubtitleModel::editSubtitle(int id, const QString &newSubtitleText)
{
    if (isLocked()) {
        return false;
    }
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) {
        qDebug() << "No Subtitle at pos in model";
        return false;
    }
    std::pair<int, GenTime> start = m_allSubtitles.at(id);
    if (m_subtitleList.count(start) <= 0) {
        qDebug() << "No Subtitle at pos in model";
        return false;
    }

    qDebug() << "Editing existing subtitle in model";
    m_subtitleList[start].setText(newSubtitleText);
    int row = getSubtitleIndex(id);
    // TODO

    Q_EMIT dataChanged(index(row), index(row), QVector<int>() << SubtitleRole);
    Q_EMIT modelChanged();
    return true;
}

bool SubtitleModel::updateSubtitleEvent(int id, std::pair<int, GenTime> start, const SubtitleEvent &event)
{
    if (isLocked() || !hasSubtitle(id) || start.first < 0 || start.first > m_maxLayer || start.second < GenTime() || event.endTime() <= start.second)
        return false;
    const auto oldStart = m_allSubtitles.at(id);
    const auto existing = m_subtitleList.find(start);
    if (start != oldStart && existing != m_subtitleList.end()) return false;
    const auto previous = m_subtitleList.at(oldStart);
    removeSnapPoint(oldStart.second);
    removeSnapPoint(previous.endTime());
    if (start == oldStart)
        m_subtitleList.at(start) = event;
    else {
        m_subtitleList.erase(oldStart);
        m_subtitleList.emplace(start, event);
        m_allSubtitles.at(id) = start;
    }
    addSnapPoint(start.second);
    addSnapPoint(event.endTime());
    const int row = getSubtitleIndex(id);
    Q_EMIT dataChanged(index(row), index(row), {});
    Q_EMIT modelChanged();
    m_timeline->updateDuration();
    const QPair<int, int> range{qMin(oldStart.second.frames(pCore->getCurrentFps()), start.second.frames(pCore->getCurrentFps())),
                                qMax(previous.endTime().frames(pCore->getCurrentFps()), event.endTime().frames(pCore->getCurrentFps()))};
    pCore->invalidateRange(range);
    pCore->refreshProjectRange(range);
    return true;
}
bool SubtitleModel::removeSubtitle(int id, bool temporary, bool updateFilter)
{
    qDebug() << "Deleting subtitle in model";
    if (isLocked()) {
        return false;
    }
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) {
        qDebug() << "No Subtitle at pos in model";
        return false;
    }
    std::pair<int, GenTime> start = m_allSubtitles.at(id);
    if (m_subtitleList.count(start) <= 0) {
        qDebug() << "No Subtitle at pos in model";
        return false;
    }
    GenTime end = m_subtitleList.at(start).endTime();
    int row = getSubtitleIndex(id);
    deregisterSubtitle(id, temporary);
    beginRemoveRows(QModelIndex(), row, row);
    bool lastSub = false;
    if (start == m_subtitleList.rbegin()->first) {
        // Check if this is the last subtitle
        lastSub = true;
    }
    m_subtitleList.erase(start);
    endRemoveRows();
    removeSnapPoint(start.second);
    removeSnapPoint(end);
    if (lastSub) {
        m_timeline->updateDuration();
    }
    QPair<int, int> range = {start.second.frames(pCore->getCurrentFps()), end.frames(pCore->getCurrentFps())};
    pCore->invalidateRange(range);
    pCore->refreshProjectRange(range);
    if (updateFilter) {
        Q_EMIT modelChanged();
    }
    return true;
}

void SubtitleModel::removeAllSubtitles()
{
    if (isLocked()) {
        return;
    }
    auto ids = m_allSubtitles;
    for (const auto &p : ids) {
        removeSubtitle(p.first);
    }
}

void SubtitleModel::requestSubtitleMove(int clipId, int newLayer, GenTime position)
{
    int oldLayer = getLayerForId(clipId);
    GenTime oldPos = getStartPosForId(clipId);
    Fun local_redo = [this, newLayer, clipId, position]() { return moveSubtitle(clipId, newLayer, position, true, true); };
    Fun local_undo = [this, oldLayer, clipId, oldPos]() { return moveSubtitle(clipId, oldLayer, oldPos, true, true); };
    bool res = local_redo();
    if (res) {
        pCore->pushUndo(local_undo, local_redo, i18n("Move subtitle"));
    }
}

bool SubtitleModel::moveSubtitle(int subId, int newLayer, GenTime newPos, bool updateModel, bool updateView)
{
    if (m_allSubtitles.count(subId) == 0 || isLocked()) {
        return false;
    }
    int oldLayer = getLayerForId(subId);
    GenTime oldPos = getStartPosForId(subId);
    if (m_subtitleList.count({oldLayer, oldPos}) <= 0 || m_subtitleList.count({newLayer, newPos}) > 0) {
        // is not present in model, or already another one at new position
        qDebug() << "==== MOVE FAILED";
        return false;
    }
    const SubtitleEvent event = m_subtitleList[{oldLayer, oldPos}];
    removeSnapPoint(oldPos);
    removeSnapPoint(event.endTime());
    GenTime duration = event.endTime() - oldPos;
    GenTime endPos = newPos + duration;
    int id = getIdForStartPos(oldLayer, oldPos);
    if (newLayer > m_maxLayer) {
        setMaxLayer(newLayer);
    }
    m_allSubtitles[id] = {newLayer, newPos};
    m_subtitleList.erase({oldLayer, oldPos});
    m_subtitleList[{newLayer, newPos}] = event;
    m_subtitleList[{newLayer, newPos}].setEndTime(endPos);
    addSnapPoint(newPos);
    addSnapPoint(endPos);
    setActiveSubLayer(newLayer);
    if (updateView) {
        updateSub(id, {StartFrameRole, EndFrameRole, LayerRole});
        QPair<int, int> range;
        if (oldPos < newPos) {
            range = {oldPos.frames(pCore->getCurrentFps()), endPos.frames(pCore->getCurrentFps())};
        } else {
            range = {newPos.frames(pCore->getCurrentFps()), (oldPos + duration).frames(pCore->getCurrentFps())};
        }
        pCore->invalidateRange(range);
        pCore->refreshProjectRange(range);
    }
    if (updateModel) {
        // Trigger update of the subtitle file
        Q_EMIT modelChanged();
        if (newPos == m_subtitleList.rbegin()->first.second) {
            // Check if this is the last subtitle
            m_timeline->updateDuration();
        }
    }
    return true;
}

int SubtitleModel::getIdForStartPos(int layer, GenTime startTime) const
{
    auto findResult = std::find_if(std::begin(m_allSubtitles), std::end(m_allSubtitles), [&](const std::pair<int, std::pair<int, GenTime>> &pair) {
        return pair.second.second == startTime && (pair.second.first == layer || layer == -1);
    });
    if (findResult != std::end(m_allSubtitles)) {
        return findResult->first;
    }
    return -1;
}

int SubtitleModel::getLayerForId(int id) const
{
    if (m_allSubtitles.count(id) == 0) {
        return -1;
    }
    return m_allSubtitles.at(id).first;
}

GenTime SubtitleModel::getStartPosForId(int id) const
{
    if (m_allSubtitles.count(id) == 0) {
        return GenTime();
    };
    return m_allSubtitles.at(id).second;
}

int SubtitleModel::getPreviousSub(int id) const
{
    GenTime start = getStartPosForId(id);
    int layer = getLayerForId(id);
    int row = static_cast<int>(std::distance(m_subtitleList.begin(), m_subtitleList.find({layer, start})));
    if (row > 0) {
        row--;
        auto it = m_subtitleList.begin();
        std::advance(it, row);
        const GenTime res = it->first.second;
        return getIdForStartPos(layer, res);
    }
    return -1;
}

int SubtitleModel::getNextSub(int id) const
{
    GenTime start = getStartPosForId(id);
    int layer = getLayerForId(id);
    int row = static_cast<int>(std::distance(m_subtitleList.begin(), m_subtitleList.find({layer, start})));
    if (row < static_cast<int>(m_subtitleList.size()) - 1) {
        row++;
        auto it = m_subtitleList.begin();
        std::advance(it, row);
        const GenTime res = it->first.second;
        return getIdForStartPos(layer, res);
    }
    return -1;
}

void SubtitleModel::subtitleFileFromZone(int in, int out, const QString &outFile)
{
    QJsonArray list;
    double fps = pCore->getCurrentFps();
    GenTime zoneIn(in, fps);
    GenTime zoneOut(out, fps);
    for (const auto &subtitle : m_subtitleList) {
        int layer = subtitle.first.first;
        GenTime inTime = subtitle.first.second;
        GenTime outTime = subtitle.second.endTime();
        if (outTime < zoneIn) {
            // Outside zone
            continue;
        }
        if (zoneOut > GenTime() && inTime > zoneOut) {
            // Outside zone
            continue;
        }
        if (inTime < zoneIn) {
            inTime = zoneIn;
        }
        if (zoneOut > GenTime() && outTime > zoneOut) {
            outTime = zoneOut;
        }
        inTime -= zoneIn;
        outTime -= zoneIn;

        QJsonObject currentSubtitle;
        currentSubtitle.insert(QLatin1String("layer"), QJsonValue(layer));
        currentSubtitle.insert(QLatin1String("startPos"), QJsonValue(inTime.seconds()));
        currentSubtitle.insert(QLatin1String("dialogue"), QJsonValue(subtitle.second.toString(layer, inTime)));
        list.push_back(currentSubtitle);
    }
    saveSubtitleData(list, outFile);
}

const QJsonArray SubtitleModel::toJson()
{
    QJsonArray list;
    for (const auto &subtitle : m_subtitleList) {
        QJsonObject currentSubtitle;
        currentSubtitle.insert(QLatin1String("layer"), QJsonValue(subtitle.first.first));
        currentSubtitle.insert(QLatin1String("startPos"), QJsonValue(subtitle.first.second.seconds()));
        currentSubtitle.insert(QLatin1String("dialogue"), QJsonValue(subtitle.second.toString(subtitle.first.first, subtitle.first.second)));
        list.push_back(currentSubtitle);
    }
    return list;
}

void SubtitleModel::copySubtitle(const QString &path, int ix, bool checkOverwrite, bool updateFilter)
{
    QFile srcFile(pCore->currentDoc()->subTitlePath(m_timeline->uuid(), ix, false));
    if (srcFile.exists()) {
        QFile prev(path);
        if (prev.exists()) {
            if (checkOverwrite || !path.endsWith(QStringLiteral(".ass"))) {
                if (KMessageBox::questionTwoActions(QApplication::activeWindow(), i18n("File %1 already exists.\nDo you want to overwrite it?", path), {},
                                                    KStandardGuiItem::overwrite(), KStandardGuiItem::cancel()) == KMessageBox::SecondaryAction) {
                    return;
                }
            }
            prev.remove();
        }
        srcFile.copy(path);
        if (updateFilter) {
            m_subtitleFilter->set("av.filename", path.toUtf8().constData());
        }
    } else {
        qDebug() << "/// SUB FILE " << srcFile.fileName() << " NOT FOUND!!!";
    }
}

void SubtitleModel::restoreTmpFile(int ix)
{

    QString outFile = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), ix, false);
    m_subtitleFilter->set("av.filename", outFile.toUtf8().constData());
}

void SubtitleModel::jsontoSubtitle(const QJsonArray &data)
{
    // A language exchange temporarily contains empty/intermediate maps. Only the
    // committed language may update its native work file and filter.
    if (m_languageStateChanging) return;
    int ix = pCore->currentDoc()->getSequenceProperty(m_timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt();
    QString outFile = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), ix, false);
    QString masterFile = m_subtitleFilter->get("av.filename");
    if (masterFile.isEmpty()) {
        m_subtitleFilter->set("av.filename", outFile.toUtf8().constData());
    }
    int line = saveSubtitleData(data, outFile);
    qDebug() << "Saving subtitle filter: " << outFile;
    if (line > 0) {
        m_subtitleFilter->set("av.filename", outFile.toUtf8().constData());
        m_timeline->tractor()->attach(*m_subtitleFilter.get());
    } else {
        m_timeline->tractor()->detach(*m_subtitleFilter.get());
    }
}

int SubtitleModel::saveSubtitleData(const QJsonArray &list, const QString &outFile)
{
    bool assFormat = outFile.endsWith(".ass");
    if (!assFormat) {
        qDebug() << "srt/vtt/sbv file import"; // if imported file isn't .ass, it is .srt format
    }
    QFile outF(outFile);

    QWriteLocker locker(&m_lock);
    if (list.isEmpty() && !m_subtitleList.empty()) {
        qDebug() << "Error : Json file should be an array";
        return 0;
    }
    int line = 0;
    if (outF.open(QIODevice::WriteOnly)) {
        QTextStream out(&outF);
        if (assFormat) {
            out << QStringLiteral("[Script Info]\n; Script generated by Kdenlive %1\n").arg(KDENLIVE_VERSION);
            for (const auto &entry : std::as_const(m_scriptInfo)) {
                out << entry.first + ": " + entry.second + '\n';
            }
            out << '\n';

            out << "[Kdenlive Extradata]\n";
            out << "MaxLayer: " + QString::number(getMaxLayer()) + '\n';
            QString defaultStyles;
            for (const auto &style : std::as_const(m_defaultStyles)) {
                defaultStyles += style + ',';
            }
            defaultStyles.chop(1);
            out << "DefaultStyles: " + defaultStyles + '\n';

            out << '\n';

            out << QStringLiteral("[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, "
                                  "Italic, Underline, StrikeOut, "
                                  "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n");
            for (const auto &entry : std::as_const(m_subtitleStyles)) {
                out << entry.second.toString(entry.first) << '\n';
            }
            out << '\n';

            if (!fontSection.isEmpty()) out << "[Fonts]\n" << fontSection << '\n';

            out << QStringLiteral("[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n");
        }
        for (const auto &entry : std::as_const(list)) {
            if (!entry.isObject()) {
                qDebug() << "Warning : Skipping invalid subtitle data";
                continue;
            }
            auto entryObj = entry.toObject();
            if (!entryObj.contains(QLatin1String("startPos"))) {
                qDebug() << "Warning : Skipping invalid subtitle data (does not contain position)";
                continue;
            }
            GenTime startPos(entryObj[QLatin1String("startPos")].toDouble());
            QString dialogue = entryObj[QLatin1String("dialogue")].toString();
            // Ensure line breaks are correctly handled
            SubtitleEvent sub(dialogue, pCore->getCurrentFps());
            GenTime endPos = sub.endTime();
            line++;
            if (assFormat) {
                dialogue.replace(QLatin1Char('\n'), QStringLiteral("\\N"));
                // Format: Layer, Start, End, Style, Actor, MarginL, MarginR, MarginV, Effect, Text
                // TODO : (after adjusting the structure of m_subtitleList)
                out << dialogue << '\n';
            } else {
                QString startTimeStringSRT = SubtitleEvent::timeToString(startPos, 1);
                QString endTimeStringSRT = SubtitleEvent::timeToString(endPos, 1);
                out << line << "\n" << startTimeStringSRT << " --> " << endTimeStringSRT << "\n" << sub.text() << "\n" << '\n';
                qDebug() << sub.text() << '\n';
            }
        }
        outF.close();
    }
    return line;
}

void SubtitleModel::updateSub(int id, const QVector<int> &roles)
{
    int row = getSubtitleIndex(id);
    Q_EMIT dataChanged(index(row), index(row), roles);
}

void SubtitleModel::updateSub(int startRow, int endRow, const QVector<int> &roles)
{
    Q_EMIT dataChanged(index(startRow), index(endRow), roles);
}

int SubtitleModel::getRowForId(int id) const
{
    return getSubtitleIndex(id);
}

int SubtitleModel::getSubtitlePlaytime(int id) const
{
    std::pair<int, GenTime> startPos = m_allSubtitles.at(id);
    return m_subtitleList.at(startPos).endTime().frames(pCore->getCurrentFps()) - startPos.second.frames(pCore->getCurrentFps());
}

GenTime SubtitleModel::getSubtitlePosition(int sid) const
{
    Q_ASSERT(m_allSubtitles.find(sid) != m_allSubtitles.end());
    return m_allSubtitles.at(sid).second;
}

int SubtitleModel::getSubtitleFakePosition(int sid) const
{
    int ix = getSubtitleIndex(sid);
    return getSubtitleFakePosFromIndex(ix);
}

int SubtitleModel::getSubtitleEnd(int id) const
{
    std::pair<int, GenTime> startPos = m_allSubtitles.at(id);
    return m_subtitleList.at(startPos).endTime().frames(pCore->getCurrentFps());
}

QPair<int, int> SubtitleModel::getInOut(int sid) const
{
    std::pair<int, GenTime> startPos = m_allSubtitles.at(sid);
    return {startPos.second.frames(pCore->getCurrentFps()), m_subtitleList.at(startPos).endTime().frames(pCore->getCurrentFps())};
}

void SubtitleModel::setSelected(int id, bool select)
{
    if (isLocked()) {
        return;
    }
    if (select) {
        m_selected << id;
    } else {
        m_selected.removeAll(id);
    }
    updateSub(id, {SelectedRole});
}

bool SubtitleModel::isSelected(int id) const
{
    return m_selected.contains(id);
}

int SubtitleModel::trackDuration() const
{
    if (m_subtitleList.empty()) {
        return 0;
    }
    return m_subtitleList.rbegin()->second.endTime().frames(pCore->getCurrentFps());
}

void SubtitleModel::switchDisabled()
{
    m_subtitleFilter->set("disable", 1 - m_subtitleFilter->get_int("disable"));
}

void SubtitleModel::switchLocked()
{
    bool isLocked = m_subtitleFilter->get_int("kdenlive:locked") == 1;
    m_subtitleFilter->set("kdenlive:locked", isLocked ? 0 : 1);

    // En/disable snapping on lock
    /*std::vector<std::weak_ptr<SnapInterface>> validSnapModels;
    for (const auto &snapModel : m_regSnaps) {
        if (auto ptr = snapModel.lock()) {
            validSnapModels.push_back(snapModel);
            if (isLocked) {
                for (const auto &subtitle : m_subtitleList) {
                    ptr->addPoint(subtitle.first.frames(pCore->getCurrentFps()));
                    ptr->addPoint(subtitle.second.second.frames(pCore->getCurrentFps()));
                }
            } else {
                for (const auto &subtitle : m_subtitleList) {
                    ptr->removePoint(subtitle.first.frames(pCore->getCurrentFps()));
                    ptr->removePoint(subtitle.second.second.frames(pCore->getCurrentFps()));
                }
            }
        }
    }
    // Update the list of snapModel known to be valid
    std::swap(m_regSnaps, validSnapModels);
    if (!isLocked) {
        // Clear selection
        while (!m_selected.isEmpty()) {
            int id = m_selected.takeFirst();
            updateSub(id, {SelectedRole});
        }
    }*/
}

bool SubtitleModel::isDisabled() const
{
    return m_subtitleFilter->get_int("disable") == 1;
}

bool SubtitleModel::isLocked() const
{
    return m_subtitleFilter->get_int("kdenlive:locked") == 1;
}

void SubtitleModel::loadProperties(const QMap<QString, QString> &subProperties)
{
    if (subProperties.isEmpty()) {
        if (m_subtitleFilter->property_exists("av.force_style")) {
            const QString style = m_subtitleFilter->get("av.force_style");
            Q_EMIT updateSubtitleStyle(style);
        } else {
            Q_EMIT updateSubtitleStyle(QString());
        }
        return;
    }
    QMap<QString, QString>::const_iterator i = subProperties.constBegin();
    while (i != subProperties.constEnd()) {
        if (!i.value().isEmpty()) {
            m_subtitleFilter->set(i.key().toUtf8().constData(), i.value().toUtf8().constData());
        }
        ++i;
    }
    if (subProperties.contains(QLatin1String("av.force_style"))) {
        Q_EMIT updateSubtitleStyle(subProperties.value(QLatin1String("av.force_style")));
    } else {
        Q_EMIT updateSubtitleStyle(QString());
    }
    qDebug() << "::::: LOADED SUB PROPS " << subProperties;
}

void SubtitleModel::allSnaps(std::vector<int> &snaps)
{
    for (const auto &subtitle : m_subtitleList) {
        snaps.push_back(subtitle.first.second.frames(pCore->getCurrentFps()));
        snaps.push_back(subtitle.second.endTime().frames(pCore->getCurrentFps()));
    }
}

QDomElement SubtitleModel::toXml(int sid, QDomDocument &document)
{
    std::pair<int, GenTime> startPos = m_allSubtitles.at(sid);
    GenTime endPos = m_subtitleList.at(startPos).endTime();
    QDomElement container = document.createElement(QStringLiteral("subtitle"));
    container.setAttribute(QStringLiteral("layer"), startPos.first);
    container.setAttribute(QStringLiteral("in"), startPos.second.frames(pCore->getCurrentFps()));
    container.setAttribute(QStringLiteral("out"), endPos.frames(pCore->getCurrentFps()));
    container.setAttribute(QStringLiteral("event_text"), m_subtitleList.at(startPos).toString(startPos.first, startPos.second));
    return container;
}

bool SubtitleModel::isBlankAt(int layer, int pos) const
{
    GenTime matchPos(pos, pCore->getCurrentFps());
    for (const auto &subtitles : m_subtitleList) {
        // if layer is -1, we check all layers
        if (layer == -1 || subtitles.first.first == layer) {
            if (subtitles.first.second > matchPos) {
                continue;
            }
            if (subtitles.second.endTime() > matchPos) {
                return false;
            }
        }
    }
    return true;
}

int SubtitleModel::getBlankEnd(int layer, int pos) const
{
    GenTime matchPos(pos, pCore->getCurrentFps());
    bool found = false;
    GenTime min;
    for (const auto &subtitles : m_subtitleList) {
        // if layer is -1, we check all layers
        if ((layer == -1 || subtitles.first.first == layer) && subtitles.first.second > matchPos && (min == GenTime() || subtitles.first.second < min)) {
            min = subtitles.first.second;
            found = true;
        }
    }
    return found ? min.frames(pCore->getCurrentFps()) : 0;
}

int SubtitleModel::getBlankSizeAtPos(int layer, int frame) const
{
    int bkStart = getBlankStart(layer, frame);
    int bkEnd = getBlankEnd(layer, frame);
    return bkEnd - bkStart;
}

int SubtitleModel::getBlankStart(int layer, int pos) const
{
    GenTime matchPos(pos, pCore->getCurrentFps());
    bool found = false;
    GenTime min;
    for (const auto &subtitles : m_subtitleList) {
        // if layer is -1, we check all layers
        if ((layer == -1 || subtitles.first.first == layer) && subtitles.second.endTime() <= matchPos &&
            (min == GenTime() || subtitles.second.endTime() > min)) {
            min = subtitles.second.endTime();
            found = true;
        }
    }
    return found ? min.frames(pCore->getCurrentFps()) : 0;
}

int SubtitleModel::getNextBlankStart(int layer, int pos) const
{
    while (!isBlankAt(layer, pos)) {
        std::unordered_set<int> matches = getItemsInRange(layer, pos, pos);
        if (matches.size() == 0) {
            if (isBlankAt(layer, pos)) {
                break;
            } else {
                // We are at the end of the track, abort
                return -1;
            }
        } else {
            for (int id : matches) {
                pos = qMax(pos, getSubtitleEnd(id));
            }
        }
    }
    return getBlankStart(layer, pos);
}

void SubtitleModel::editSubtitle(int id, const QString &newText, const QString &oldText)
{
    qDebug() << "Editing existing subtitle :" << id;
    if (oldText == newText) {
        return;
    }
    Fun local_redo = [this, id, newText]() {
        editSubtitle(id, newText);
        QPair<int, int> range = getInOut(id);
        pCore->invalidateRange(range);
        pCore->refreshProjectRange(range);
        return true;
    };
    Fun local_undo = [this, id, oldText]() {
        editSubtitle(id, oldText);
        QPair<int, int> range = getInOut(id);
        pCore->invalidateRange(range);
        pCore->refreshProjectRange(range);
        return true;
    };
    local_redo();
    pCore->pushUndo(local_undo, local_redo, i18n("Edit subtitle"));
}

void SubtitleModel::resizeSubtitle(int layer, int startFrame, int endFrame, int oldEndFrame, bool refreshModel)
{
    qDebug() << "Editing existing subtitle in controller at:" << startFrame;
    int max = qMax(endFrame, oldEndFrame);
    Fun local_redo = [this, layer, startFrame, endFrame, max, refreshModel]() {
        editEndPos(layer, GenTime(startFrame, pCore->getCurrentFps()), GenTime(endFrame, pCore->getCurrentFps()), refreshModel);
        QPair<int, int> range = {startFrame, max};
        pCore->invalidateRange(range);
        pCore->refreshProjectRange(range);
        return true;
    };
    Fun local_undo = [this, layer, startFrame, oldEndFrame, max, refreshModel]() {
        editEndPos(layer, GenTime(startFrame, pCore->getCurrentFps()), GenTime(oldEndFrame, pCore->getCurrentFps()), refreshModel);
        QPair<int, int> range = {startFrame, max};
        pCore->invalidateRange(range);
        pCore->refreshProjectRange(range);
        return true;
    };
    local_redo();
    if (refreshModel) {
        pCore->pushUndo(local_undo, local_redo, i18n("Resize subtitle"));
    }
}

void SubtitleModel::addSubtitle(int startframe, int layer, QString text)
{
    if (startframe == -1) {
        startframe = pCore->getMonitorPosition();
    }
    int endframe = startframe + pCore->getDurationFromString(KdenliveSettings::subtitle_duration());
    int id = TimelineModel::getNextId();
    if (text.isEmpty()) {
        text = i18n("Add Text");
    }
    Fun local_undo = [this, id, startframe, endframe]() {
        removeSubtitle(id);
        QPair<int, int> range = {startframe, endframe};
        pCore->invalidateRange(range);
        pCore->refreshProjectRange(range);
        return true;
    };
    Fun local_redo = [this, layer, id, startframe, endframe, text]() {
        if (addSubtitle(id, {layer, GenTime(startframe, pCore->getCurrentFps())},
                        SubtitleEvent(true, GenTime(endframe, pCore->getCurrentFps()), getLayerDefaultStyle(layer), "", 0, 0, 0, "", text))) {
            QPair<int, int> range = {startframe, endframe};
            pCore->invalidateRange(range);
            pCore->refreshProjectRange(range);

            m_timeline->requestAddToSelection(id, true);
            int index = positionForIndex(id);
            if (index > -1) {
                Q_EMIT m_timeline->highlightSub(index);
            }
        }
        return true;
    };
    local_redo();
    pCore->pushUndo(local_undo, local_redo, i18n("Add subtitle"));
}

void SubtitleModel::doCutSubtitle(int id, int cursorPos)
{
    Q_ASSERT(m_timeline->isSubTitle(id));
    // Cut subtitle at edit position
    int timelinePos = pCore->getMonitorPosition();
    GenTime position(timelinePos, pCore->getCurrentFps());
    GenTime start = getStartPosForId(id);
    int layer = getLayerForId(id);
    SubtitleEvent event = getSubtitle(layer, start);
    if (position > start && position < event.endTime()) {
        QString originalText = event.text();
        QString firstText = originalText;
        QString secondText = originalText.right(originalText.length() - cursorPos);
        firstText.truncate(cursorPos);
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        int newId = cutSubtitle(layer, timelinePos, undo, redo, true);
        if (newId > -1) {
            Fun local_redo = [this, id, newId, firstText, secondText]() {
                editSubtitle(id, firstText);
                editSubtitle(newId, secondText);
                ObjectId owner(KdenliveObjectType::TimelineSubtitle, newId, m_timeline->uuid());
                pCore->invalidateItem(owner);
                pCore->refreshProjectItem(owner);
                return true;
            };
            Fun local_undo = [this, id, originalText]() {
                editSubtitle(id, originalText);
                ObjectId owner(KdenliveObjectType::TimelineSubtitle, id, m_timeline->uuid());
                pCore->invalidateItem(owner);
                pCore->refreshProjectItem(owner);
                return true;
            };
            local_redo();
            PUSH_LAMBDA(local_redo, redo);
            PUSH_LAMBDA(local_undo, undo);
            pCore->pushUndo(undo, redo, i18n("Cut clip"));
        }
    }
}

void SubtitleModel::deleteSubtitle(int layer, int startframe, int endframe, const QString &text)
{
    int id = getIdForStartPos(layer, GenTime(startframe, pCore->getCurrentFps()));
    Fun local_redo = [this, id, startframe, endframe]() {
        removeSubtitle(id);
        QPair<int, int> range = {startframe, endframe};
        pCore->invalidateRange(range);
        pCore->refreshProjectRange(range);
        return true;
    };
    Fun local_undo = [this, layer, id, startframe, endframe, text]() {
        addSubtitle(id, {layer, GenTime(startframe, pCore->getCurrentFps())},
                    SubtitleEvent(true, GenTime(endframe, pCore->getCurrentFps()), "Default", "", 0, 0, 0, "", text));
        QPair<int, int> range = {startframe, endframe};
        pCore->invalidateRange(range);
        pCore->refreshProjectRange(range);
        return true;
    };
    local_redo();
    pCore->pushUndo(local_undo, local_redo, i18n("Delete subtitle"));
}

QMap<std::pair<int, QString>, QString> SubtitleModel::getSubtitlesList() const
{
    return m_subtitlesList;
}

void SubtitleModel::setSubtitlesList(QMap<std::pair<int, QString>, QString> list)
{
    m_subtitlesList = list;
}

void SubtitleModel::updateModelName(int ix, const QString &name)
{
    auto found = m_subtitlesList.end();
    for (auto i = m_subtitlesList.begin(); i != m_subtitlesList.end(); ++i)
        if (i.key().first == ix) {
            found = i;
            break;
        }
    if (found == m_subtitlesList.end() || name.isEmpty() || found.key().second == name) return;
    const auto before = found.key();
    const auto after = std::pair<int, QString>{ix, name};
    const QString path = found.value();
    const auto rename = [this, path](const std::pair<int, QString> &from, const std::pair<int, QString> &to) {
        if (!m_subtitlesList.contains(from)) return false;
        m_subtitlesList.remove(from);
        m_subtitlesList.insert(to, path);
        Q_EMIT m_timeline->subtitlesListChanged();
        Q_EMIT activeSubtitleChanged();
        return true;
    };
    Fun undo = [rename, before, after] { return rename(after, before); };
    Fun redo = [rename, before, after] { return rename(before, after); };
    if (redo()) pCore->pushUndo(undo, redo, i18n("Rename Subtitle Language"));
}
int SubtitleModel::getMaxLayer() const
{
    return m_maxLayer;
}

void SubtitleModel::requestDeleteLayer(int layer)
{
    if (isLocked() || layer < 0 || layer > m_maxLayer || m_maxLayer == 0) return;
    std::map<std::pair<int, GenTime>, SubtitleEvent> oldSubtitles;
    std::map<std::pair<int, GenTime>, int> oldIds;
    for (const auto &event : m_subtitleList)
        if (event.first.first == layer) {
            oldSubtitles.insert(event);
            oldIds[event.first] = getIdForStartPos(layer, event.first.second);
        }
    m_timeline->requestClearSelection(true);
    Fun groupUndo = [] { return true; }, groupRedo = [] { return true; };
    for (const auto &entry : oldIds)
        if (!m_timeline->extractItemFromGroup(entry.second, groupUndo, groupRedo, true)) {
            groupUndo();
            return;
        }
    Fun redo = [this, layer, groupRedo]() {
        if (!groupRedo()) return false;
        deleteLayer(layer);
        return true;
    };
    Fun undo = [this, layer, oldSubtitles, oldIds, groupUndo]() {
        createLayer(layer);
        for (const auto &sub : oldSubtitles)
            if (!addSubtitle(oldIds.at(sub.first), sub.first, sub.second)) return false;
        if (!groupUndo()) return false;
        Q_EMIT modelChanged();
        return true;
    };
    deleteLayer(layer);
    pCore->pushUndo(undo, redo, i18n("Delete subtitle layer"));
}
void SubtitleModel::requestCreateLayer(int copiedLayer, int position)
{
    if (position < 0) {
        position = getMaxLayer() + 1;
    }

    Fun undo = [this, position]() {
        deleteLayer(position);
        return true;
    };

    Fun redo = [this, position]() {
        createLayer(position);
        return true;
    };

    redo();
    if (copiedLayer >= 0) {
        auto subs = m_subtitleList;
        for (const auto &sub : subs) {
            if (sub.first.first == copiedLayer) {
                addSubtitle({position, sub.first.second}, sub.second, undo, redo);
            }
        }
    }
    pCore->pushUndo(undo, redo, i18n("Create subtitle layer"));
}

void SubtitleModel::deleteLayer(int layer)
{
    if (layer < 0) {
        layer = getMaxLayer();
    }
    // Delete all subtitles on this layer, and make the subtitles behind it move up
    auto subs = m_allSubtitles;
    for (const auto &sub : subs) {
        if (sub.second.first == layer) {
            removeSubtitle(sub.first);
        } else if (sub.second.first > layer) {
            moveSubtitle(sub.first, sub.second.first - 1, sub.second.second, false, true);
        }
    }

    setMaxLayer(getMaxLayer() - 1);
    Q_EMIT modelChanged();
    return;
}

void SubtitleModel::createLayer(int position)
{
    setMaxLayer(getMaxLayer() + 1);
    // Move all following subtitles the new layer
    // Prevent subtitles from failing to move when there is a subtitle at the same position on the next layer.
    for (int layer = getMaxLayer() - 1; layer >= position; layer--) {
        auto subs = m_allSubtitles;
        for (const auto &sub : subs) {
            if (sub.second.first == layer) {
                moveSubtitle(sub.first, sub.second.first + 1, sub.second.second, false, true);
            }
        }
    }
    Q_EMIT modelChanged();
    return;
}

void SubtitleModel::reload()
{
    int currentIx = pCore->currentDoc()->getSequenceProperty(m_timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt();
    activateSubtitle(currentIx, true);
}

const std::map<QString, SubtitleStyle> &SubtitleModel::getAllSubtitleStyles(bool global) const
{
    if (global) {
        return m_globalSubtitleStyles;
    } else {
        return m_subtitleStyles;
    }
}

const SubtitleStyle &SubtitleModel::getSubtitleStyle(const QString &name, bool global) const
{
    if (global) {
        return m_globalSubtitleStyles.at(name);
    } else {
        return m_subtitleStyles.at(name);
    }
}

void SubtitleModel::setSubtitleStyle(const QString &name, const SubtitleStyle &style, bool global)
{
    if (global) {
        m_globalSubtitleStyles[name] = style;
    } else {
        m_subtitleStyles[name] = style;
        Q_EMIT modelChanged();
    }
}

void SubtitleModel::deleteSubtitleStyle(const QString &name, bool global)
{
    if (global) {
        m_globalSubtitleStyles.erase(name);
    } else {
        // find all subtitles using this style and set them to default
        for (auto &subtitle : m_subtitleList) {
            if (subtitle.second.styleName() == name) {
                subtitle.second.setStyleName("Default");
            }
        }
        m_subtitleStyles.erase(name);
        Q_EMIT modelChanged();
    }
}

int SubtitleModel::createNewSubtitle(const QString subtitleName, int id)
{
    const auto keys = m_subtitlesList.keys();
    QStringList existingNames;
    int maxId = 0;
    for (const auto &key : keys) {
        existingNames << key.second;
        maxId = qMax(maxId, key.first);
    }
    ++maxId;
    QString name = subtitleName;
    int suffix = m_subtitlesList.size() + 1;
    if (name.isEmpty()) {
        do {
            name = i18nc("@item:inlistbox subtitle track name", "Subtitle %1", suffix++);
        } while (existingNames.contains(name));
    }
    QByteArray initial;
    if (id >= 0) {
        QString source = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), id, false);
        if (!QFile::exists(source)) source = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), id, true);
        QFile file(source);
        if (!file.open(QIODevice::ReadOnly)) return -1;
        initial = file.readAll();
    }
    const auto entry = std::pair<int, QString>{maxId, name};
    const auto create = [this, entry, initial] {
        const QString workPath = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), entry.first, false);
        const QString finalPath = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), entry.first, true);
        if (m_subtitlesList.contains(entry) || QFile::exists(workPath)) return false;
        QFile file(workPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
        if (file.write(initial) != initial.size() || !file.flush()) {
            file.close();
            QFile::remove(workPath);
            return false;
        }
        file.close();
        m_subtitlesList.insert(entry, finalPath);
        Q_EMIT m_timeline->subtitlesListChanged();
        return true;
    };
    Fun undo = [this, entry] {
        const int active =
            pCore->currentDoc()->getSequenceProperty(m_timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt();
        if (active == entry.first) return false;
        const QString workPath = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), entry.first, false);
        // Published ASS assets belong to saved project snapshots. Undo only edits
        // the live work file, so Save / Undo / Discard cannot damage saved media.
        if (QFile::exists(workPath) && !QFile::remove(workPath)) return false;
        m_subtitlesList.remove(entry);
        Q_EMIT m_timeline->subtitlesListChanged();
        return true;
    };
    Fun redo = create;
    if (!redo()) return -1;
    pCore->pushUndo(undo, redo, i18n("Create Subtitle Language"));
    return m_subtitlesList.size();
}

bool SubtitleModel::deleteSubtitle(int ix)
{
    if (m_subtitlesList.size() < 2) return false;
    auto found = m_subtitlesList.end();
    for (auto i = m_subtitlesList.begin(); i != m_subtitlesList.end(); ++i)
        if (i.key().first == ix) {
            found = i;
            break;
        }
    if (found == m_subtitlesList.end()) return false;
    const auto entry = found.key();
    QString source = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), ix, false);
    if (!QFile::exists(source)) source = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), ix, true);
    QFile sourceFile(source);
    if (!sourceFile.open(QIODevice::ReadOnly)) return false;
    const QByteArray initial = sourceFile.readAll();
    sourceFile.close();
    const int active =
        pCore->currentDoc()->getSequenceProperty(m_timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt();
    // The interactive manager can delete its selected language; first leave it through
    // the ordinary undoable activation path so no live event IDs outlast their language.
    if (active == ix) {
        int replacement = -1;
        for (const auto &key : m_subtitlesList.keys())
            if (key.first != ix) {
                replacement = key.first;
                break;
            }
        if (replacement < 0 || !activateSubtitle(replacement)) return false;
    }
    Fun redo = [this, entry] {
        const QString workPath = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), entry.first, false);
        if (QFile::exists(workPath) && !QFile::remove(workPath)) return false;
        m_subtitlesList.remove(entry);
        Q_EMIT m_timeline->subtitlesListChanged();
        return true;
    };
    Fun undo = [this, entry, initial] {
        const QString workPath = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), entry.first, false);
        const QString finalPath = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), entry.first, true);
        QFile file(workPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
        if (file.write(initial) != initial.size() || !file.flush()) {
            file.close();
            QFile::remove(workPath);
            return false;
        }
        file.close();
        m_subtitlesList.insert(entry, finalPath);
        Q_EMIT m_timeline->subtitlesListChanged();
        return true;
    };
    if (!redo()) return false;
    pCore->pushUndo(undo, redo, i18n("Delete Subtitle Language"));
    return true;
}
const QString SubtitleModel::subtitlesFilesToJson()
{
    QJsonArray list;
    QMapIterator<std::pair<int, QString>, QString> i(m_subtitlesList);
    std::pair<int, QString> oldSub = {-1, QString()};
    while (i.hasNext()) {
        i.next();
        QJsonObject currentSubtitle;
        currentSubtitle.insert(QLatin1String("name"), QJsonValue(i.key().second));
        currentSubtitle.insert(QLatin1String("id"), QJsonValue(i.key().first));
        currentSubtitle.insert(QLatin1String("file"), QJsonValue(i.value()));
        list.push_back(currentSubtitle);
    }
    QJsonDocument json(list);
    return QString::fromUtf8(json.toJson());
}

const QString SubtitleModel::globalStylesToJson()
{
    QJsonArray list;
    for (const auto &style : m_globalSubtitleStyles) {
        QJsonObject currentStyle;
        currentStyle.insert(QLatin1String("style"), QJsonValue(style.second.toString(style.first)));
        list.push_back(currentStyle);
    }
    QJsonDocument json(list);
    return QString::fromUtf8(json.toJson());
}

bool SubtitleModel::swapLanguageState(LanguageState &state)
{
    // Selection is transient, while group identity is restored by the activation command.
    m_timeline->requestClearSelection(true);
    for (const auto &entry : m_allSubtitles) {
        if (m_timeline->m_groups->isInGroup(entry.first)) return false;
    }
    if (!m_languageStateChanging) beginResetModel();
    for (const auto &entry : m_allSubtitles) {
        const auto &event = m_subtitleList.at(entry.second);
        removeSnapPoint(entry.second.second);
        removeSnapPoint(event.endTime());
        m_timeline->m_groups->destructGroupItem(entry.first);
    }
    m_subtitleList.swap(state.events);
    m_allSubtitles.swap(state.ids);
    m_subtitleStyles.swap(state.styles);
    m_scriptInfo.swap(state.script);
    m_subtitlesFakePos.swap(state.fakePositions);
    m_defaultStyles.swap(state.defaultStyles);
    fontSection.swap(state.fonts);
    QString file = m_subtitleFilter->get("av.filename");
    m_subtitleFilter->set("av.filename", state.file.toUtf8().constData());
    state.file = file;
    std::swap(m_maxLayer, state.maxLayer);
    std::swap(m_activeSubLayer, state.activeLayer);
    m_selected.clear();
    m_grabbedIds.clear();
    for (const auto &entry : m_allSubtitles) {
        const auto &event = m_subtitleList.at(entry.second);
        m_timeline->m_groups->createGroupItem(entry.first);
        addSnapPoint(entry.second.second);
        addSnapPoint(event.endTime());
    }
    if (!m_languageStateChanging) endResetModel();
    return true;
}

bool SubtitleModel::exchangeLanguageState(int id, const Fun &exchange)
{
    beginResetModel();
    bool success = false;
    {
        QScopedValueRollback<bool> changing(m_languageStateChanging, true);
        {
            QSignalBlocker signals(this);
            success = exchange();
            if (success) {
                pCore->currentDoc()->setSequenceProperty(m_timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), id);
                restoreTmpFile(id);
            }
        }
        // Observers receive only the committed map/index/filter combination. The
        // reset-to-modelChanged adapter suppresses intermediate file persistence.
        endResetModel();
    }
    if (success) languageStateActivated(id);
    return success;
}

void SubtitleModel::languageStateActivated(int id)
{
    pCore->currentDoc()->setSequenceProperty(m_timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), id);
    m_timeline->updateDuration();
    Q_EMIT m_timeline->maxSubLayerChanged();
    Q_EMIT activeSubLayerChanged();
    Q_EMIT activeSubtitleChanged();
    Q_EMIT modelChanged();
    pCore->refreshProjectMonitorOnce();
}

bool SubtitleModel::activateSubtitle(int ix, bool forceReload)
{
    const int currentIx =
        pCore->currentDoc()->getSequenceProperty(m_timeline->uuid(), QStringLiteral("kdenlive:activeSubtitleIndex"), QStringLiteral("0")).toInt();
    if (currentIx == ix && !forceReload) return true;
    const bool exists = std::any_of(m_subtitlesList.keyBegin(), m_subtitlesList.keyEnd(), [ix](const auto &entry) { return entry.first == ix; });
    if (!exists) return false;
    const QString workPath = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), ix, false);
    const QString finalPath = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), ix, true);
    if (!QFile::exists(workPath) && QFile::exists(finalPath) && !QFile::copy(finalPath, workPath)) return false;
    if (!QFile::exists(workPath)) {
        QFile file(workPath);
        if (!file.open(QIODevice::WriteOnly)) return false;
    }
    m_timeline->requestClearSelection(true);
    Fun groupUndo = [] { return true; }, groupRedo = [] { return true; };
    for (const auto &entry : m_allSubtitles) {
        if (!m_timeline->extractItemFromGroup(entry.first, groupUndo, groupRedo, true)) {
            groupUndo();
            return false;
        }
    }
    auto previous = std::make_shared<LanguageState>();
    auto next = std::make_shared<LanguageState>();
    if (!exchangeLanguageState(ix, [this, previous, workPath] {
            if (!swapLanguageState(*previous)) return false;
            parseSubtitle(workPath);
            return true;
        })) {
        groupUndo();
        return false;
    }
    // Undo crosses the language boundary before any commands referring to the former IDs.
    // Map swaps retain the exact event nodes and IDs instead of reparsing saved files.
    Fun undo = [this, previous, next, groupUndo, currentIx] {
        return exchangeLanguageState(currentIx, [this, previous, next, groupUndo] {
            if (!swapLanguageState(*next)) return false;
            if (!swapLanguageState(*previous)) {
                swapLanguageState(*next);
                return false;
            }
            if (!groupUndo()) {
                swapLanguageState(*previous);
                swapLanguageState(*next);
                return false;
            }
            return true;
        });
    };
    Fun redo = [this, previous, next, groupRedo, groupUndo, ix] {
        return exchangeLanguageState(ix, [this, previous, next, groupRedo, groupUndo] {
            if (!groupRedo()) return false;
            if (!swapLanguageState(*previous)) {
                groupUndo();
                return false;
            }
            if (!swapLanguageState(*next)) {
                swapLanguageState(*previous);
                groupUndo();
                return false;
            }
            return true;
        });
    };
    pCore->pushUndo(undo, redo, forceReload ? i18n("Reload Subtitles") : i18n("Switch Subtitle Language"));
    return true;
}
void SubtitleModel::setMaxLayer(int layer)
{
    if (layer > 50) {
        pCore->displayMessage(i18n("Cannot create more than %1 layers in subtitles.", layer), ErrorMessage);
        layer = 50;
    }
    m_maxLayer = layer;
    for (int i = m_defaultStyles.size(); i < m_maxLayer + 1; i++) {
        m_defaultStyles << "Default";
    }
    Q_EMIT m_timeline->maxSubLayerChanged();
}

void SubtitleModel::registerSubtitle(int id, std::pair<int, GenTime> startpos, bool temporary)
{
    Q_ASSERT(m_allSubtitles.count(id) == 0);
    m_allSubtitles.emplace(id, startpos);
    if (!temporary) {
        m_timeline->m_groups->createGroupItem(id);
    }
}

void SubtitleModel::deregisterSubtitle(int id, bool temporary)
{
    Q_ASSERT(m_allSubtitles.count(id) > 0);
    if (!temporary && isSelected(id)) {
        m_timeline->requestClearSelection(true);
    }
    m_allSubtitles.erase(id);
    if (!temporary) {
        m_timeline->m_groups->destructGroupItem(id);
    }
}

int SubtitleModel::positionForIndex(int id) const
{
    return int(std::distance(m_allSubtitles.begin(), m_allSubtitles.find(id)));
}

bool SubtitleModel::hasSubtitle(int id) const
{
    return m_allSubtitles.find(id) != m_allSubtitles.end();
}

int SubtitleModel::count() const
{
    return m_allSubtitles.size();
}

GenTime SubtitleModel::getPosition(int id) const
{
    return m_allSubtitles.at(id).second;
}

int SubtitleModel::getSubtitleIndex(int subId) const
{
    if (m_allSubtitles.count(subId) == 0) {
        return -1;
    }
    auto it = m_allSubtitles.find(subId);
    return int(std::distance(m_allSubtitles.begin(), it));
}

std::pair<int, std::pair<int, GenTime>> SubtitleModel::getSubtitleIdFromIndex(int index) const
{
    if (index >= static_cast<int>(m_allSubtitles.size())) {
        return {-1, {-1, GenTime()}};
    }
    auto it = m_allSubtitles.begin();
    std::advance(it, index);
    return {it->first, it->second};
}

int SubtitleModel::getSubtitleIdByPosition(int layer, int pos)
{
    GenTime startTime(pos, pCore->getCurrentFps());
    auto findResult = std::find_if(std::begin(m_allSubtitles), std::end(m_allSubtitles), [&](const std::pair<int, std::pair<int, GenTime>> &pair) {
        return pair.second.second == startTime && pair.second.first == layer;
    });
    if (findResult != std::end(m_allSubtitles)) {
        return findResult->first;
    }
    return -1;
}

int SubtitleModel::getSubtitleIdAtPosition(int layer, int pos)
{
    std::unordered_set<int> sids = getItemsInRange(layer, pos, pos);
    if (!sids.empty()) {
        return *sids.begin();
    }
    return -1;
}

void SubtitleModel::cleanupSubtitleFakePos()
{
    std::vector<int> itemKeys;
    for (const auto &sub : m_subtitlesFakePos) {
        itemKeys.push_back(sub.first);
    }
    m_subtitlesFakePos.clear();
    for (auto &i : itemKeys) {
        Q_EMIT dataChanged(index(i), index(i), {SubtitleModel::FakeStartFrameRole});
    }
}

int SubtitleModel::getSubtitleFakePosFromIndex(int index) const
{
    auto search = m_subtitlesFakePos.find(index);
    if (search != m_subtitlesFakePos.end()) {
        return search->second;
    }
    return -1;
}

void SubtitleModel::setSubtitleFakePosFromIndex(int index, int pos)
{
    m_subtitlesFakePos[index] = pos;
}

std::unordered_set<int> SubtitleModel::getAllSubIds() const
{
    std::unordered_set<int> ids;
    for (std::map<int, std::pair<int, GenTime>>::const_iterator it = m_allSubtitles.cbegin(); it != m_allSubtitles.cend(); ++it) {
        ids.emplace(it->first);
    }
    return ids;
}

bool SubtitleModel::getIsDialogue(int id) const
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return false;
    return m_subtitleList.at(m_allSubtitles.at(id)).isDialogue();
}

void SubtitleModel::setIsDialogue(int id, bool isDialogue, bool refreshModel)
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return;
    bool oldIsDialogue = getIsDialogue(id);
    if (oldIsDialogue == isDialogue) {
        return;
    }
    Fun local_redo = [this, id, isDialogue, refreshModel]() {
        m_subtitleList.at(m_allSubtitles.at(id)).setIsDialogue(isDialogue);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {IsDialogueRole});
        if (refreshModel) Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    Fun local_undo = [this, id, oldIsDialogue, refreshModel]() {
        m_subtitleList.at(m_allSubtitles.at(id)).setIsDialogue(oldIsDialogue);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {IsDialogueRole});
        if (refreshModel) Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    local_redo();
    pCore->pushUndo(local_undo, local_redo, i18n("Edit subtitle"));
}

QString SubtitleModel::getStyleName(int id) const
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return QString();
    return m_subtitleList.at(m_allSubtitles.at(id)).styleName();
}

void SubtitleModel::setStyleName(const int id, const QString &style, bool refreshModel)
{
    auto sub = m_subtitleList.find(m_allSubtitles.at(id));
    if (sub == m_subtitleList.end() || style == sub->second.styleName()) {
        return;
    }
    QString oldStyle = sub->second.styleName();
    if (oldStyle == style) {
        return;
    }
    Fun redo = [this, style, refreshModel, id]() {
        if (!hasSubtitle(id)) return false;
        m_subtitleList.at(m_allSubtitles.at(id)).setStyleName(style);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {StyleNameRole});
        if (refreshModel) Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    Fun undo = [this, oldStyle, refreshModel, id]() {
        if (!hasSubtitle(id)) return false;
        m_subtitleList.at(m_allSubtitles.at(id)).setStyleName(oldStyle);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {StyleNameRole});
        if (refreshModel) Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    redo();
    pCore->pushUndo(undo, redo, i18n("Assign a new style to a subtitle."));
}

QString SubtitleModel::getName(int id) const
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return QString();
    return m_subtitleList.at(m_allSubtitles.at(id)).name();
}

void SubtitleModel::setName(int id, const QString &name, bool refreshModel)
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return;
    QString oldName = getName(id);
    if (oldName == name) {
        return;
    }
    Fun local_redo = [this, id, name, refreshModel]() {
        m_subtitleList.at(m_allSubtitles.at(id)).setName(name);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {NameRole});
        if (refreshModel) Q_EMIT modelChanged();
        return true;
    };
    Fun local_undo = [this, id, oldName, refreshModel]() {
        m_subtitleList.at(m_allSubtitles.at(id)).setName(oldName);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {NameRole});
        if (refreshModel) Q_EMIT modelChanged();
        return true;
    };
    local_redo();
    pCore->pushUndo(local_undo, local_redo, i18n("Edit subtitle"));
}

int SubtitleModel::getMarginL(int id) const
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return 0;
    return m_subtitleList.at(m_allSubtitles.at(id)).marginL();
}

void SubtitleModel::setMarginL(int id, int marginL, bool refreshModel)
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return;
    int oldMarginL = getMarginL(id);
    if (oldMarginL == marginL) {
        return;
    }
    Fun local_redo = [this, id, marginL, refreshModel]() {
        m_subtitleList.at(m_allSubtitles.at(id)).setMarginL(marginL);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {MarginLRole});
        if (refreshModel) Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    Fun local_undo = [this, id, oldMarginL, refreshModel]() {
        m_subtitleList.at(m_allSubtitles.at(id)).setMarginL(oldMarginL);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {MarginLRole});
        if (refreshModel) Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    local_redo();
    pCore->pushUndo(local_undo, local_redo, i18n("Edit subtitle"));
}

int SubtitleModel::getMarginR(int id) const
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return 0;
    return m_subtitleList.at(m_allSubtitles.at(id)).marginR();
}

void SubtitleModel::setMarginR(int id, int marginR, bool refreshModel)
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return;
    int oldMarginR = getMarginR(id);
    if (oldMarginR == marginR) {
        return;
    }
    Fun local_redo = [this, id, marginR, refreshModel]() {
        m_subtitleList.at(m_allSubtitles.at(id)).setMarginR(marginR);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {MarginRRole});
        if (refreshModel) Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    Fun local_undo = [this, id, oldMarginR, refreshModel]() {
        m_subtitleList.at(m_allSubtitles.at(id)).setMarginR(oldMarginR);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {MarginRRole});
        if (refreshModel) Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    local_redo();
    pCore->pushUndo(local_undo, local_redo, i18n("Edit subtitle"));
}

int SubtitleModel::getMarginV(int id) const
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return 0;
    return m_subtitleList.at(m_allSubtitles.at(id)).marginV();
}

void SubtitleModel::setMarginV(int id, int marginV, bool refreshModel)
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return;
    int oldMarginV = getMarginV(id);
    if (oldMarginV == marginV) {
        return;
    }
    Fun local_redo = [this, id, marginV, refreshModel]() {
        m_subtitleList.at(m_allSubtitles.at(id)).setMarginV(marginV);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {MarginVRole});
        if (refreshModel) Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    Fun local_undo = [this, id, oldMarginV, refreshModel]() {
        m_subtitleList.at(m_allSubtitles.at(id)).setMarginV(oldMarginV);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {MarginVRole});
        if (refreshModel) Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    local_redo();
    pCore->pushUndo(local_undo, local_redo, i18n("Edit subtitle"));
}

QString SubtitleModel::getEffects(int id) const
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return QString();
    return m_subtitleList.at(m_allSubtitles.at(id)).effect();
}

void SubtitleModel::setEffects(int id, const QString &effects, bool refreshModel)
{
    if (m_allSubtitles.find(id) == m_allSubtitles.end()) return;
    QString oldEffects = getEffects(id);
    if (oldEffects == effects) {
        return;
    }
    Fun local_redo = [this, id, effects, refreshModel]() {
        m_subtitleList.at(m_allSubtitles.at(id)).setEffect(effects);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {EffectRole});
        if (refreshModel) Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    Fun local_undo = [this, id, oldEffects, refreshModel]() {
        m_subtitleList.at(m_allSubtitles.at(id)).setEffect(oldEffects);
        int row = getSubtitleIndex(id);
        Q_EMIT dataChanged(index(row), index(row), {EffectRole});
        if (refreshModel) Q_EMIT modelChanged();
        pCore->refreshProjectMonitorOnce();
        return true;
    };
    local_redo();
    pCore->pushUndo(local_undo, local_redo, i18n("Edit subtitle"));
}

int SubtitleModel::activeSubLayer() const
{
    return m_activeSubLayer;
}

void SubtitleModel::setActiveSubLayer(int layer)
{
    m_activeSubLayer = layer;
    Q_EMIT activeSubLayerChanged();
}

QString SubtitleModel::previewSubtitlePath() const
{
    QString path = pCore->currentDoc()->subTitlePath(m_timeline->uuid(), 0, false);
    path = path.left(path.lastIndexOf(QStringLiteral(".ass")));
    path += "-preview.ass";
    return path;
}

const std::map<QString, QString> &SubtitleModel::scriptInfo() const
{
    return m_scriptInfo;
}

void SubtitleModel::setLayerDefaultStyle(int layer, const QString styleName)
{
    if (m_defaultStyles.size() > layer) {
        m_defaultStyles[layer] = styleName;
    }
}

const QString SubtitleModel::getLayerDefaultStyle(int layer) const
{
    if (m_defaultStyles.size() > layer) {
        return m_defaultStyles.at(layer);
    } else {
        return QStringLiteral("Default");
    }
}
