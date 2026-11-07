/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcpserver.h"
#include "mcpmarkers.h"
#include "mcpmedia.h"
#include "mcpmonitor.h"
#include "mcpprofiles.h"
#include "mcpprojectextras.h"
#include "mcpquick.h"
#include "mcpsequence.h"
#include "mcpspeech.h"
#include "mcpstate.h"
#include "mcptitles.h"
#include "mcpwire.h"

#include "assets/model/assetparametermodel.hpp"
#include "bin/clipcreator.hpp"
#include "bin/model/markerlistmodel.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "config-kdenlive.h"
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "effects/effectsrepository.hpp"
#include "effects/effectstack/model/effectitemmodel.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "project/projectmanager.h"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/model/trackmodel.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMimeDatabase>
#include <QMimeType>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <mlt++/MltProducer.h>

namespace {
constexpr qsizetype MaxRequestBytes = 1024 * 1024;
constexpr qsizetype MaxResponseBytes = 1024 * 1024;
constexpr int MaxFrame = 10000000;
constexpr int MaxDuration = 1000000;
constexpr qint64 MaxMediaBytes = qint64(64) << 30;

QJsonObject rpcError(const QJsonValue &id, int code, const QString &message)
{
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
}
QJsonObject toolResult(const QJsonObject &data, bool error = false)
{
    return McpWire::toolResult(data, error);
}
QJsonObject failure(const QString &code, const QString &message, const QJsonObject &state = {}, const QJsonObject &diagnostics = {})
{
    QJsonObject data{{QStringLiteral("ok"), false},
                     {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
    if (!state.isEmpty()) data.insert(QStringLiteral("state"), state);
    if (!diagnostics.isEmpty()) data.insert(QStringLiteral("diagnostics"), diagnostics);
    return toolResult(data, true);
}
QString digest(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString effectsDigest(const std::shared_ptr<EffectStackModel> &stack)
{
    if (!stack) return QString();
    QDomDocument document;
    document.appendChild(stack->toXml(document));
    return digest(document.toByteArray(0));
}
bool integer(const QJsonObject &object, const QString &key, int minimum, int maximum, int &result)
{
    const auto value = object.value(key);
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < minimum || number > maximum) return false;
    result = static_cast<int>(number);
    return true;
}
QString mediaType(const std::shared_ptr<ProjectClip> &clip)
{
    if (!clip) return QStringLiteral("unknown");
    switch (clip->clipType()) {
    case ClipType::Image:
        return QStringLiteral("image");
    case ClipType::Text:
    case ClipType::TextTemplate:
        return QStringLiteral("title");
    case ClipType::SlideShow:
        return QStringLiteral("slideshow");
    case ClipType::Audio:
        return QStringLiteral("audio");
    case ClipType::Video:
        return QStringLiteral("video");
    case ClipType::AV:
        return QStringLiteral("av");
    case ClipType::Color:
        return QStringLiteral("color");
    case ClipType::Timeline:
        return QStringLiteral("sequence");
    case ClipType::Unknown:
        return QStringLiteral("unknown");
    default:
        return QStringLiteral("unsupported");
    }
}
QString mediaStatus(const std::shared_ptr<ProjectClip> &clip, const QString &importPath = {})
{
    if (!clip) return QStringLiteral("failed");
    if (clip->clipStatus() == FileStatus::StatusWaiting) {
        // A failed native load removes its bin item. Redo can restore that same
        // waiting placeholder without restarting LOADJOB; it must remain recoverable.
        const ObjectId owner(KdenliveObjectType::BinClip, clip->clipId().toInt(), QUuid());
        return pCore && pCore->taskManager.hasPendingJob(owner, AbstractTask::LOADJOB) ? QStringLiteral("loading") : QStringLiteral("failed");
    }
    if (!clip->statusReady()) return QStringLiteral("failed");
    if (!importPath.isEmpty()) {
        const bool image = clip->clipType() == ClipType::Image;
        if (image ? clip->clipType() != ClipType::Image
                  : (clip->clipType() != ClipType::Audio && clip->clipType() != ClipType::AV && clip->clipType() != ClipType::Video))
            return QStringLiteral("unsupported");
    }
    if ((clip->clipType() == ClipType::Audio || clip->clipType() == ClipType::AV || clip->clipType() == ClipType::Video) &&
        (clip->getProducerIntProperty(QStringLiteral("_wait_for_transcode")) != 0 ||
         clip->getProducerIntProperty(QStringLiteral("meta.media.variable_frame_rate")) != 0 || clip->getProducerIntProperty(QStringLiteral("seekable")) == 0))
        return QStringLiteral("needs_transcode");
    return QStringLiteral("ready");
}
bool realNumber(const QJsonObject &object, const QString &key, double minimum, double maximum, double &result)
{
    const auto value = object.value(key);
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < minimum || number > maximum) return false;
    result = number;
    return true;
}
std::vector<std::shared_ptr<EffectItemModel>> nativeEffects(const std::shared_ptr<EffectStackModel> &stack)
{
    std::vector<std::shared_ptr<EffectItemModel>> effects;
    for (int row = 0; row < stack->rowCount(); ++row) {
        const auto effect = std::dynamic_pointer_cast<EffectItemModel>(stack->getEffectStackRow(row));
        // Groups are outside this bounded prototype; callers check row counts.
        if (effect) effects.push_back(effect);
    }
    return effects;
}
bool effectPresent(const std::shared_ptr<EffectStackModel> &stack, const std::shared_ptr<EffectItemModel> &effect)
{
    if (!effect) return false;
    const auto effects = nativeEffects(stack);
    return std::find(effects.begin(), effects.end(), effect) != effects.end();
}
QVector<QPair<QString, QVariant>> rawEffectParameters(const std::shared_ptr<EffectItemModel> &effect)
{
    QVector<QPair<QString, QVariant>> result;
    // getAllParameters() is a save/copy representation: position parameters can
    // be source-relative. Read the actual native filter values for updates/readback.
    for (const auto &parameter : effect->getAllParameters()) {
        const char *value = effect->filter().get(parameter.first.toUtf8().constData());
        result.append({parameter.first, QString::fromUtf8(value ? value : "")});
    }
    return result;
}
QJsonObject effectReadback(const QString &kind, const std::shared_ptr<EffectItemModel> &effect)
{
    QJsonObject parameters;
    for (const auto &parameter : rawEffectParameters(effect))
        parameters.insert(parameter.first, parameter.second.toString());
    return {{QStringLiteral("kind"), kind},
            {QStringLiteral("effectId"), effect->getAssetId()},
            {QStringLiteral("effectInstanceId"), effect->getId()},
            {QStringLiteral("inFrame"), effect->filter().get_in()},
            {QStringLiteral("outFrame"), effect->filter().get_out()},
            {QStringLiteral("parameters"), parameters}};
}
QString decimal(double value)
{
    return QString::number(value, 'g', 15);
}
QJsonObject property(const QString &type, const QString &description)
{
    return {{QStringLiteral("type"), type}, {QStringLiteral("description"), description}};
}
bool validateArgument(const QJsonValue &value, const QJsonObject &schema, const QString &path, QString &error, int depth = 0)
{
    auto reject = [&](const QString &message) {
        error = path + QStringLiteral(": ") + message;
        return false;
    };
    if (depth > 32) return reject(QStringLiteral("Argument nesting exceeds 32 levels."));
    const QString type = schema.value(QStringLiteral("type")).toString();
    if ((!type.isEmpty() && type == QLatin1String("string") && !value.isString()) || (type == QLatin1String("boolean") && !value.isBool()) ||
        (type == QLatin1String("object") && !value.isObject()) || (type == QLatin1String("array") && !value.isArray()) ||
        ((type == QLatin1String("number") || type == QLatin1String("integer")) &&
         (!value.isDouble() || !std::isfinite(value.toDouble()) || (type == QLatin1String("integer") && std::floor(value.toDouble()) != value.toDouble()))))
        return reject(QStringLiteral("Expected ") + type + QLatin1Char('.'));
    if (schema.contains(QStringLiteral("enum")) && !schema.value(QStringLiteral("enum")).toArray().contains(value))
        return reject(QStringLiteral("Value is outside the declared choices."));
    if (value.isDouble()) {
        if (schema.contains(QStringLiteral("minimum")) && value.toDouble() < schema.value(QStringLiteral("minimum")).toDouble())
            return reject(QStringLiteral("Value is below the declared minimum."));
        if (schema.contains(QStringLiteral("maximum")) && value.toDouble() > schema.value(QStringLiteral("maximum")).toDouble())
            return reject(QStringLiteral("Value exceeds the declared maximum."));
    }
    if (value.isString()) {
        if (schema.contains(QStringLiteral("minLength")) && value.toString().size() < schema.value(QStringLiteral("minLength")).toInt())
            return reject(QStringLiteral("Text is shorter than the declared minimum."));
        if (schema.contains(QStringLiteral("maxLength")) && value.toString().size() > schema.value(QStringLiteral("maxLength")).toInt())
            return reject(QStringLiteral("Text exceeds the declared maximum."));
    }
    if (value.isObject()) {
        const auto object = value.toObject();
        const auto properties = schema.value(QStringLiteral("properties")).toObject();
        for (const auto &required : schema.value(QStringLiteral("required")).toArray())
            if (!object.contains(required.toString())) return reject(QStringLiteral("Missing argument: ") + required.toString());
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (schema.value(QStringLiteral("additionalProperties")) == QJsonValue(false) && !properties.contains(it.key()))
                return reject(QStringLiteral("Unexpected argument: ") + it.key());
            if (properties.contains(it.key()) &&
                !validateArgument(it.value(), properties.value(it.key()).toObject(), path + QLatin1Char('.') + it.key(), error, depth + 1))
                return false;
        }
    }
    if (value.isArray()) {
        const auto array = value.toArray();
        if (schema.contains(QStringLiteral("minItems")) && array.size() < schema.value(QStringLiteral("minItems")).toInt())
            return reject(QStringLiteral("Too few array items."));
        if (schema.contains(QStringLiteral("maxItems")) && array.size() > schema.value(QStringLiteral("maxItems")).toInt())
            return reject(QStringLiteral("Too many array items."));
        const auto items = schema.value(QStringLiteral("items")).toObject();
        if (!items.isEmpty())
            for (int i = 0; i < array.size(); ++i)
                if (!validateArgument(array.at(i), items, path + QStringLiteral("[%1]").arg(i), error, depth + 1)) return false;
    }
    return true;
}
QJsonArray baseToolCatalog()
{
    const auto text = [](const char *description) { return property(QStringLiteral("string"), QString::fromUtf8(description)); };
    const auto number = [](const char *description) {
        auto value = property(QStringLiteral("integer"), QString::fromUtf8(description));
        value.insert(QStringLiteral("minimum"), 0);
        value.insert(QStringLiteral("maximum"), MaxFrame);
        return value;
    };
    const auto expected = text("Exact state.expectedState from the latest result; bound to document, sequence and undo history.");
    QJsonArray tools;
    const auto add = [&tools](const char *name, const char *description, QJsonObject properties, QJsonArray required, bool readOnly) {
        tools.append(
            QJsonObject{{QStringLiteral("name"), QString::fromUtf8(name)},
                        {QStringLiteral("description"), QString::fromUtf8(description)},
                        {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                                                    {QStringLiteral("properties"), properties},
                                                                    {QStringLiteral("required"), required},
                                                                    {QStringLiteral("additionalProperties"), false}}},
                        {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("readOnlyHint"), readOnly}, {QStringLiteral("openWorldHint"), false}}}});
    };
    add("get_info", "Describe the native prototype and its limits.", {}, {}, true);
    add("get_state", "Read the active sequence, all tracks, clip IDs and fresh expectedState.", {}, {}, true);
    add("add_color_clip", "Add a finite native color clip to an explicit unlocked video track; one undo step. Uses explicit native track/frame addresses.",
        {{QStringLiteral("expectedState"), expected},
         {QStringLiteral("trackId"), number("Native video track ID.")},
         {QStringLiteral("startFrame"), number("Exact timeline start frame.")},
         {QStringLiteral("durationFrames"), number("Positive frame count; at most 1000000.")},
         {QStringLiteral("color"), text("RGBA in #RRGGBBAA format.")},
         {QStringLiteral("name"), text("Optional name, at most 160 characters.")}},
        {QStringLiteral("expectedState"), QStringLiteral("trackId"), QStringLiteral("startFrame"), QStringLiteral("durationFrames"), QStringLiteral("color")},
        false);
    add("import_media",
        "Import one bounded local image, GIF, audio or video file into the native bin; one undo step. Poll get_state for readiness before further edits.",
        {{QStringLiteral("expectedState"), expected},
         {QStringLiteral("path"), text("Absolute existing local decodable image, GIF, audio or video path; at most 64 GiB.")}},
        {QStringLiteral("expectedState"), QStringLiteral("path")}, false);
    add("insert_media_clip",
        "Place one ready native bin image/title/sequence/video or one audio stream on an explicit unlocked track, with exact integral source and timeline "
        "ranges; one "
        "undo "
        "step.",
        {{QStringLiteral("expectedState"), expected},
         {QStringLiteral("binId"), text("Native bin ID from get_state or an import/creation tool.")},
         {QStringLiteral("trackId"), number("Native destination track ID, matching stream.")},
         {QStringLiteral("startFrame"), number("Exact timeline start frame.")},
         {QStringLiteral("sourceInFrame"), number("First inclusive source frame.")},
         {QStringLiteral("durationFrames"), number("Positive source/timeline frame count; at most 1000000.")},
         {QStringLiteral("stream"), text("video for PNG/CFR video/AV, or audio for audio-only/AV media with one audio stream.")}},
        {QStringLiteral("expectedState"), QStringLiteral("binId"), QStringLiteral("trackId"), QStringLiteral("startFrame"), QStringLiteral("sourceInFrame"),
         QStringLiteral("durationFrames"), QStringLiteral("stream")},
        false);
    QJsonObject transformKeyframe{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("frame"), number("Clip-local frame, starting at zero.")},
                     {QStringLiteral("x"), property(QStringLiteral("integer"), QStringLiteral("Destination X in project pixels."))},
                     {QStringLiteral("y"), property(QStringLiteral("integer"), QStringLiteral("Destination Y in project pixels."))},
                     {QStringLiteral("width"), property(QStringLiteral("integer"), QStringLiteral("Destination bitmap width in project pixels."))},
                     {QStringLiteral("height"), property(QStringLiteral("integer"), QStringLiteral("Destination bitmap height in project pixels."))},
                     {QStringLiteral("opacity"), property(QStringLiteral("number"), QStringLiteral("Opacity from zero to one."))},
                     {QStringLiteral("rotation"), property(QStringLiteral("number"), QStringLiteral("Degrees about bitmap center, -360 to 360."))}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("frame"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"),
                                                QStringLiteral("height"), QStringLiteral("opacity"), QStringLiteral("rotation")}},
        {QStringLiteral("additionalProperties"), false}};
    QJsonObject keyframes{{QStringLiteral("type"), QStringLiteral("array")},
                          {QStringLiteral("minItems"), 1},
                          {QStringLiteral("maxItems"), 64},
                          {QStringLiteral("items"), transformKeyframe}};
    add("set_clip_transform", "Set a typed native bitmap transform with linear clip-local keyframes. Updates only this MCP's own transform; one undo step.",
        {{QStringLiteral("expectedState"), expected},
         {QStringLiteral("clipId"), number("Native imported video timeline clip ID.")},
         {QStringLiteral("keyframes"), keyframes}},
        {QStringLiteral("expectedState"), QStringLiteral("clipId"), QStringLiteral("keyframes")}, false);
    add("set_clip_audio", "Set gain in dB and native audio fade spans on one imported audio timeline clip; one undo step. Fade N spans N+1 inclusive frames.",
        {{QStringLiteral("expectedState"), expected},
         {QStringLiteral("clipId"), number("Native imported audio timeline clip ID.")},
         {QStringLiteral("gainDb"), property(QStringLiteral("number"), QStringLiteral("Gain in dB, -60 to +12."))},
         {QStringLiteral("fadeInFrames"), number("Native fade-in interval span; zero removes only this MCP's fade.")},
         {QStringLiteral("fadeOutFrames"), number("Native fade-out interval span; zero removes only this MCP's fade.")}},
        {QStringLiteral("expectedState"), QStringLiteral("clipId"), QStringLiteral("gainDb"), QStringLiteral("fadeInFrames"), QStringLiteral("fadeOutFrames")},
        false);
    add("move_clip", "Move one ungrouped, unmixed native color clip by ID. Does not overwrite other clips or use selection.",
        {{QStringLiteral("expectedState"), expected},
         {QStringLiteral("clipId"), number("Native timeline clip ID.")},
         {QStringLiteral("trackId"), number("Native destination video track ID.")},
         {QStringLiteral("startFrame"), number("Exact timeline start frame.")}},
        {QStringLiteral("expectedState"), QStringLiteral("clipId"), QStringLiteral("trackId"), QStringLiteral("startFrame")}, false);
    add("cut_clip", "Split one ungrouped native color clip strictly inside its duration; timeline selection must be empty.",
        {{QStringLiteral("expectedState"), expected},
         {QStringLiteral("clipId"), number("Native timeline clip ID.")},
         {QStringLiteral("frame"), number("Absolute timeline cut frame.")}},
        {QStringLiteral("expectedState"), QStringLiteral("clipId"), QStringLiteral("frame")}, false);
    add("undo", "Undo the current document's last command, including user commands. Returns fresh state.", {{QStringLiteral("expectedState"), expected}},
        {QStringLiteral("expectedState")}, false);
    add("redo", "Redo the current document's next command, including user commands. Returns fresh state.", {{QStringLiteral("expectedState"), expected}},
        {QStringLiteral("expectedState")}, false);
    add("save_project", "Save a native project copy to an absolute new local .kdenlive path. Does not replace the active document or overwrite a file.",
        {{QStringLiteral("expectedState"), expected}, {QStringLiteral("path"), text("Absolute destination file path; parent folder must exist.")}},
        {QStringLiteral("expectedState"), QStringLiteral("path")}, false);
    return tools;
}
} // namespace

QJsonArray KdenliveMcpServer::toolCatalog()
{
    QJsonArray result;
    const auto timeline = timelineToolCatalog();
    for (const auto &entry : baseToolCatalog()) {
        const auto name = entry.toObject().value(QStringLiteral("name")).toString();
        bool replaced = false;
        for (const auto &replacement : timeline)
            if (replacement.toObject().value(QStringLiteral("name")).toString() == name) replaced = true;
        if (!replaced) result.append(entry);
    }
    for (const auto &catalog :
         {timeline, effectToolCatalog(), lifecycleToolCatalog(), uiToolCatalog(), McpQuick::toolCatalog(), McpMedia::toolCatalog(), McpMonitor::toolCatalog(),
          McpProfiles::toolCatalog(), McpSpeech::toolCatalog(), McpMarkers::toolCatalog(), McpTitles::toolCatalog(), McpProjectExtras::toolCatalog()})
        for (const auto &entry : catalog)
            result.append(entry);
    for (const auto &entry : McpState::toolCatalog())
        result.append(entry);
    for (const auto &entry : McpWire::toolCatalog())
        result.append(entry);
    return result;
}
KdenliveMcpServer::KdenliveMcpServer(QObject *parent)
    : QObject(parent)
    , m_server(this)
    , m_sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.setMaxPendingConnections(1);
    connect(&m_server, &QLocalServer::newConnection, this, &KdenliveMcpServer::acceptClient);
}
KdenliveMcpServer::~KdenliveMcpServer()
{
    lifecycleCleanup();
    disconnect(m_undoConnection);
    if (m_client) m_client->abort();
    m_server.close();
}
bool KdenliveMcpServer::start(const QString &socketName, QString *error)
{
    QString reason;
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
    reason = QStringLiteral("This prototype requires Windows or Linux socket user-access enforcement.");
#endif
    if (!qApp || QThread::currentThread() != qApp->thread()) reason = QStringLiteral("Start must run on the GUI thread.");
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{7,99}$")).match(socketName).hasMatch())
        reason = QStringLiteral("Use an explicit unique socket name, 8-100 letters, digits, dots, underscores or hyphens.");
    if (reason.isEmpty() && !m_server.listen(socketName)) reason = m_server.errorString();
    if (reason.isEmpty()) qInfo().noquote() << "Kdenlive MCP endpoint:" << m_server.fullServerName();
    if (error) *error = reason;
    return reason.isEmpty();
}
void KdenliveMcpServer::acceptClient()
{
    while (auto *socket = m_server.nextPendingConnection()) {
        if (m_client) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_client = socket;
        m_initialized = false;
        m_input.clear();
        socket->setReadBufferSize(MaxRequestBytes + 1);
        connect(socket, &QLocalSocket::readyRead, this, &KdenliveMcpServer::readClient);
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            if (m_client == socket) {
                m_client.clear();
                m_input.clear();
                m_initialized = false;
            }
            socket->deleteLater();
        });
        if (socket->bytesAvailable()) readClient();
    }
}
void KdenliveMcpServer::readClient()
{
    if (!m_client || m_reading) return;
    QScopedValueRollback<bool> reading(m_reading, true);
    m_input += m_client->readAll();
    if (m_input.size() > MaxRequestBytes + 1) {
        m_client->abort();
        return;
    }
    int processed = 0;
    while (m_client && processed++ < 16) {
        const auto end = m_input.indexOf('\n');
        if (end < 0) {
            if (m_input.size() > MaxRequestBytes) {
                send(rpcError(QJsonValue::Null, -32600, QStringLiteral("Request exceeds 1 MiB.")));
                if (m_client) m_client->disconnectFromServer();
            }
            if (m_client && m_client->bytesAvailable()) QMetaObject::invokeMethod(this, &KdenliveMcpServer::readClient, Qt::QueuedConnection);
            return;
        }
        if (end > MaxRequestBytes) {
            m_client->abort();
            return;
        }
        QByteArray line = m_input.left(end).trimmed();
        m_input.remove(0, end + 1);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            send(rpcError(QJsonValue::Null, -32700, QStringLiteral("Expected one JSON object per line.")));
        } else {
            const auto response = handleRequest(document.object());
            if (!response.isEmpty()) send(response);
        }
    }
    if (m_client && (m_input.contains('\n') || m_client->bytesAvailable()))
        QMetaObject::invokeMethod(this, &KdenliveMcpServer::readClient, Qt::QueuedConnection);
}
void KdenliveMcpServer::send(const QJsonObject &response)
{
    if (!m_client) return;
    auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
    if (bytes.size() > MaxResponseBytes) {
        // Normally bounded by handleRequest. Preserve an unexpected tool
        // result at this last boundary instead of losing its native outcome.
        const bool toolData = response.value(QStringLiteral("result")).toObject().value(QStringLiteral("structuredContent")).isObject();
        bytes = QJsonDocument(McpWire::boundRpcResponse(this, response, QStringLiteral("transport_fallback"), toolData)).toJson(QJsonDocument::Compact);
    }
    if (m_client->bytesToWrite() + bytes.size() > 2 * MaxResponseBytes) {
        m_client->abort();
        return;
    }
    bytes.append('\n');
    m_client->write(bytes);
}
QJsonObject KdenliveMcpServer::handleRequest(const QJsonObject &request)
{
    const auto id = request.value(QStringLiteral("id"));
    const auto method = request.value(QStringLiteral("method"));
    if (request.contains(QStringLiteral("id")) && ((!id.isString() && !id.isDouble() && !id.isNull()) || (id.isString() && id.toString().size() > 128))) {
        return rpcError(QJsonValue::Null, -32600, QStringLiteral("Request ID must be a scalar with at most 128 characters."));
    }
    if (request.value(QStringLiteral("jsonrpc")) != QJsonValue(QStringLiteral("2.0")) || !method.isString() ||
        (request.contains(QStringLiteral("id")) && !id.isString() && !id.isDouble() && !id.isNull()) ||
        (request.contains(QStringLiteral("params")) && !request.value(QStringLiteral("params")).isObject()))
        return rpcError(id.isUndefined() ? QJsonValue(QJsonValue::Null) : id, -32600, QStringLiteral("Invalid JSON-RPC request."));
    if (id.isUndefined()) return {}; // Notifications never execute editing commands.
    if (!qApp || QThread::currentThread() != qApp->thread()) return rpcError(id, -32000, QStringLiteral("GUI thread required."));
    if (m_dispatching) return rpcError(id, -32000, QStringLiteral("Another command is still running."));
    QScopedValueRollback<bool> dispatching(m_dispatching, true);
    const QString name = method.toString();
    const auto params = request.value(QStringLiteral("params")).toObject();
    QJsonObject result;
    QString wireToolName;
    bool wireMutation = false;
    if (name == QLatin1String("initialize")) {
        if (m_initialized) return rpcError(id, -32600, QStringLiteral("Already initialized."));
        const QString version = params.value(QStringLiteral("protocolVersion")).toString();
        const QString selected = version == QLatin1String("2025-11-25") ? version : QStringLiteral("2025-06-18");
        m_initialized = true;
        result = {{QStringLiteral("protocolVersion"), selected},
                  {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
                  {QStringLiteral("serverInfo"),
                   QJsonObject{{QStringLiteral("name"), QStringLiteral("kdenlive-native-mcp")}, {QStringLiteral("version"), QStringLiteral("0.4.0")}}},
                  {QStringLiteral("instructions"),
                   QStringLiteral("Native editor MCP. Read get_state before direct edits and pass its exact expectedState. "
                                  "Discover native tools, assets and actions. UI operations use get_ui/expectedUi and return pending jobs; "
                                  "accepted or returned handlers do not prove feature completion. Inspect jobs, readback and outputs.")}};
    } else if (name == QLatin1String("ping")) {
        result = {};
    } else {
        if (!m_initialized) return rpcError(id, -32002, QStringLiteral("Initialize the MCP session first."));
        if (name == QLatin1String("tools/list")) {
            result = {{QStringLiteral("tools"), toolCatalog()}};
        } else if (name == QLatin1String("tools/call")) {
            if (!params.value(QStringLiteral("name")).isString() ||
                (params.contains(QStringLiteral("arguments")) && !params.value(QStringLiteral("arguments")).isObject()))
                return rpcError(id, -32602, QStringLiteral("Tool name and object arguments are required."));
            wireToolName = params.value(QStringLiteral("name")).toString();
            for (const auto &entry : toolCatalog()) {
                const auto definition = entry.toObject();
                if (definition.value(QStringLiteral("name")).toString() == wireToolName)
                    wireMutation = !definition.value(QStringLiteral("annotations")).toObject().value(QStringLiteral("readOnlyHint")).toBool();
            }
            result = callTool(wireToolName, params.value(QStringLiteral("arguments")).toObject());
        } else
            return rpcError(id, -32601, QStringLiteral("Unknown method."));
    }
    return McpWire::boundRpcResponse(
        this, QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {QStringLiteral("result"), result}}, wireToolName,
        wireMutation);
}
QJsonObject KdenliveMcpServer::snapshot()
{
    QJsonObject state{{QStringLiteral("ready"), false}, {QStringLiteral("sessionId"), m_sessionId}};
    if (!pCore || pCore->closing || !pCore->projectManager() || !pCore->currentDoc()) {
        state.insert(QStringLiteral("reason"), QStringLiteral("No active document."));
        return state;
    }
    auto *document = pCore->currentDoc();
    const auto timeline = pCore->projectManager()->getTimeline();
    const auto bin = pCore->projectItemModel();
    const auto stack = pCore->undoStack();
    if (!timeline || !bin || !stack || document->loading || document->closing || timeline->isClosed) {
        state.insert(QStringLiteral("reason"), QStringLiteral("Document is loading, closing or unavailable."));
        return state;
    }
    if (m_document != document || m_timeline.lock() != timeline) {
        disconnect(m_undoConnection);
        m_document = document;
        m_timeline = timeline;
        m_imports.clear();
        m_ownedEffects.clear();
        ++m_epoch;
        ++m_revision;
        m_undoConnection = connect(stack.get(), &QUndoStack::indexChanged, this, [this](int) { ++m_revision; });
        m_freshProject = document->url().isEmpty() && timeline->getClipsCount() == 0 && timeline->getCompositionsCount() == 0 && !timeline->hasSubtitles() &&
                         Mcp::liveSequenceClips(bin).size() <= 1;
        const auto sequences = Mcp::liveSequenceClips(bin).values();
        for (const auto &id : bin->getAllClipIds())
            if (!sequences.contains(id)) m_freshProject = false;
    }
    state.insert(QStringLiteral("ready"), true);
    state.insert(QStringLiteral("documentId"), document->uuid().toString(QUuid::WithoutBraces));
    state.insert(QStringLiteral("sequenceId"), timeline->uuid().toString(QUuid::WithoutBraces));
    state.insert(QStringLiteral("epoch"), QString::number(m_epoch));
    state.insert(QStringLiteral("revision"), QString::number(m_revision));
    state.insert(QStringLiteral("freshProjectEligible"), m_freshProject);
    state.insert(QStringLiteral("projectPath"), document->url().toLocalFile());
    state.insert(QStringLiteral("modified"), document->isModified());
    state.insert(QStringLiteral("profile"), pCore->getCurrentProfilePath());
    state.insert(QStringLiteral("audioChannels"), pCore->audioChannels());
    state.insert(QStringLiteral("fps"), pCore->getCurrentFps());
    state.insert(QStringLiteral("width"), pCore->getCurrentFrameSize().width());
    state.insert(QStringLiteral("height"), pCore->getCurrentFrameSize().height());
    state.insert(QStringLiteral("durationFrames"), timeline->duration());
    state.insert(QStringLiteral("editMode"), static_cast<int>(timeline->editMode()));
    state.insert(QStringLiteral("singleSelectionMode"), timeline->singleSelectionMode());
    state.insert(QStringLiteral("undoIndex"), stack->index());
    state.insert(QStringLiteral("undoCount"), stack->count());
    state.insert(QStringLiteral("canUndo"), stack->canUndo());
    state.insert(QStringLiteral("canRedo"), stack->canRedo());
    state.insert(QStringLiteral("undoText"), stack->undoText());
    state.insert(QStringLiteral("redoText"), stack->redoText());
    QJsonArray selection;
    const auto selected = timeline->getCurrentSelection();
    std::vector<int> selectedIds(selected.begin(), selected.end());
    std::sort(selectedIds.begin(), selectedIds.end());
    for (int id : selectedIds)
        selection.append(id);
    state.insert(QStringLiteral("selection"), selection);
    state.insert(QStringLiteral("groups"), timeline->groupsData());
    state.insert(QStringLiteral("masterEffectsHash"), effectsDigest(timeline->getMasterEffectStackModel()));
    QList<McpState::ImportedSource> imported;
    imported.reserve(m_imports.size());
    for (auto it = m_imports.cbegin(); it != m_imports.cend(); ++it)
        imported.append(McpState::ImportedSource{it.key(), it.value().path, it.value().clip});
    const McpState::ClipStateReader clipStateReader = [](const std::shared_ptr<TimelineItemModel> &model, int id) {
        QJsonObject result;
        if (model->getMixDuration(id) <= 0) return result;
        const auto track = model->getTrackById_const(model->getClipTrackId(id));
        const auto parameters = track->mixModel(id);
        if (!parameters || !parameters->getAsset()) return result;
        QJsonObject values;
        for (const auto &parameter : parameters->getAllParameters()) {
            const char *raw = parameters->getAsset()->get(parameter.first.toUtf8().constData());
            values.insert(parameter.first, QString::fromUtf8(raw ? raw : ""));
        }
        const auto binding = track->getMixTracks(id);
        result.insert(QStringLiteral("mix"), QJsonObject{{QStringLiteral("assetId"), parameters->getAssetId()},
                                                         {QStringLiteral("parameters"), values},
                                                         {QStringLiteral("cutFrame"), model->getMixCutPos(id)},
                                                         {QStringLiteral("aTrack"), binding.first},
                                                         {QStringLiteral("bTrack"), binding.second},
                                                         {QStringLiteral("inFrame"), parameters->getAsset()->get_int("in")},
                                                         {QStringLiteral("outFrame"), parameters->getAsset()->get_int("out")}});
        return result;
    };
    return McpState::snapshot(this, state, timeline, bin, document, imported, clipStateReader);
}
QJsonObject KdenliveMcpServer::callTool(const QString &name, const QJsonObject &arguments)
{
    QJsonObject schema;
    bool wireMutation = true;
    for (const auto &entry : toolCatalog()) {
        const auto tool = entry.toObject();
        if (tool.value(QStringLiteral("name")).toString() == name) {
            schema = tool.value(QStringLiteral("inputSchema")).toObject();
            wireMutation = !tool.value(QStringLiteral("annotations")).toObject().value(QStringLiteral("readOnlyHint")).toBool();
            break;
        }
    }
    if (schema.isEmpty()) return failure(QStringLiteral("UNKNOWN_TOOL"), QStringLiteral("Unknown tool."));
    QString argumentError;
    if (!validateArgument(arguments, schema, name, argumentError)) return failure(QStringLiteral("INVALID_ARGUMENT"), argumentError);
    const auto properties = schema.value(QStringLiteral("properties")).toObject();
    for (auto it = arguments.begin(); it != arguments.end(); ++it)
        if (!properties.contains(it.key())) return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Unexpected argument: ") + it.key());
    for (const auto &entry : schema.value(QStringLiteral("required")).toArray())
        if (!arguments.contains(entry.toString())) return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Missing argument: ") + entry.toString());
    const auto receiptControl = McpWire::callTool(this, name, arguments);
    if (!receiptControl.isEmpty()) return receiptControl;
    const auto receiptPreflight = McpWire::mutationPreflight(this, name, wireMutation);
    if (!receiptPreflight.isEmpty()) return receiptPreflight;
    if (name == QLatin1String("get_info"))
        return toolResult(
            {{QStringLiteral("ok"), true},
             {QStringLiteral("application"), QStringLiteral("Kdenlive")},
             {QStringLiteral("version"), QStringLiteral(KDENLIVE_VERSION)},
             {QStringLiteral("buildVersion"), QStringLiteral(KDENLIVE_FULL_VERSION_STRING)},
             {QStringLiteral("prototypeVersion"), QStringLiteral("0.4.0")},
             {QStringLiteral("socketName"), m_server.serverName()},
             {QStringLiteral("endpoint"), m_server.fullServerName()},
             {QStringLiteral("sessionId"), m_sessionId},
             {QStringLiteral("processId"), QString::number(QCoreApplication::applicationPid())},
             {QStringLiteral("scope"),
              QStringLiteral("Native project/timeline/effect/media/monitor/render operations and an explicit interactive Qt control bridge. "
                             "Existing project state is editable by supported native operations. Inspect get_capabilities for verified coverage limits.")}});
    auto state = snapshot();
    if (name == QLatin1String("get_state")) return toolResult({{QStringLiteral("ok"), true}, {QStringLiteral("state"), state}});
    for (const auto &entry : McpQuick::toolCatalog()) {
        if (entry.toObject().value(QStringLiteral("name")).toString() != name) continue;
        state.insert(QStringLiteral("_widgetUiToken"), uiSnapshot().value(QStringLiteral("uiToken")));
        return McpQuick::callTool(this, name, arguments, state, [this] {
            auto fresh = snapshot();
            fresh.insert(QStringLiteral("_widgetUiToken"), uiSnapshot().value(QStringLiteral("uiToken")));
            return fresh;
        });
    }
    if (name == QLatin1String("speech_job_status") || name == QLatin1String("cancel_speech_job"))
        return McpSpeech::callTool(this, name, arguments, state, [this] { return snapshot(); });
    if (name == QLatin1String("transcode_status") || name == QLatin1String("cancel_transcode"))
        return McpMedia::callTool(this, name, arguments, state, [this] { return snapshot(); });
    if (name == QLatin1String("get_monitors") || name == QLatin1String("monitor_job_status"))
        return McpMonitor::callTool(this, name, arguments, state, [this] { return snapshot(); });
    const auto uiResult = callUiTool(name, arguments, state);
    if (!uiResult.isEmpty()) return uiResult;
    if (name == QLatin1String("cancel_render")) return callLifecycleTool(name, arguments, state);
    bool readOnly = false;
    for (const auto &entry : toolCatalog())
        if (entry.toObject().value(QStringLiteral("name")).toString() == name)
            readOnly = entry.toObject().value(QStringLiteral("annotations")).toObject().value(QStringLiteral("readOnlyHint")).toBool();
    if (readOnly) {
        const auto statePage = McpState::callTool(this, name, arguments, state, [this] { return snapshot(); });
        if (!statePage.isEmpty()) return statePage;
        auto result = McpProjectExtras::callTool(this, name, arguments, state, [this] { return snapshot(); });
        if (!result.isEmpty()) return result;
        result = McpTitles::callTool(this, name, arguments, state, [this] { return snapshot(); });
        if (!result.isEmpty()) return result;
        result = McpMarkers::callTool(this, name, arguments, state, [this] { return snapshot(); });
        if (!result.isEmpty()) return result;
        result = McpSpeech::callTool(this, name, arguments, state, [this] { return snapshot(); });
        if (!result.isEmpty()) return result;
        result = McpProfiles::callTool(this, name, arguments, state, [this] { return snapshot(); });
        if (!result.isEmpty()) return result;
        result = McpMedia::callTool(this, name, arguments, state, [this] { return snapshot(); });
        if (!result.isEmpty()) return result;
        result = callEffectTool(name, arguments, state);
        if (!result.isEmpty()) return result;
        result = callLifecycleTool(name, arguments, state);
        if (!result.isEmpty()) return result;
        result = callTimelineTool(name, arguments, state);
        if (!result.isEmpty()) return result;
        return failure(QStringLiteral("UNKNOWN_TOOL"), QStringLiteral("No read-only handler registered."), state);
    }
    if (!state.value(QStringLiteral("ready")).toBool()) return failure(QStringLiteral("NOT_READY"), state.value(QStringLiteral("reason")).toString(), state);
    const auto expected = arguments.value(QStringLiteral("expectedState"));
    if (!expected.isString() || expected.toString() != state.value(QStringLiteral("expectedState")).toString())
        return failure(QStringLiteral("STALE_STATE"), QStringLiteral("Read get_state again before editing."), state);
    const auto timeline = m_timeline.lock();
    const auto bin = pCore->projectItemModel();
    auto stack = pCore->undoStack();
    if (m_lifecycleBusy)
        return failure(QStringLiteral("PROJECT_BUSY"), QStringLiteral("A project operation is still running; inspect its job and dialogs."), state);
    if (QApplication::activeModalWidget() || QApplication::activePopupWidget() || McpQuick::modalPopupActive(this) ||
        QApplication::mouseButtons() != Qt::NoButton)
        return failure(QStringLiteral("UI_BUSY"), QStringLiteral("Finish dragging or inspect get_ui to respond to the active dialog."), state);
    auto moduleResult = McpProjectExtras::callTool(this, name, arguments, state, [this] { return snapshot(); });
    if (!moduleResult.isEmpty()) return moduleResult;
    moduleResult = McpTitles::callTool(this, name, arguments, state, [this] { return snapshot(); });
    if (!moduleResult.isEmpty()) return moduleResult;
    moduleResult = McpMarkers::callTool(this, name, arguments, state, [this] { return snapshot(); });
    if (!moduleResult.isEmpty()) return moduleResult;
    moduleResult = McpSpeech::callTool(this, name, arguments, state, [this] { return snapshot(); });
    if (!moduleResult.isEmpty()) return moduleResult;
    moduleResult = McpProfiles::callTool(this, name, arguments, state, [this] { return snapshot(); });
    if (!moduleResult.isEmpty()) return moduleResult;
    moduleResult = McpMonitor::callTool(this, name, arguments, state, [this] { return snapshot(); });
    if (!moduleResult.isEmpty()) return moduleResult;
    moduleResult = McpMedia::callTool(this, name, arguments, state, [this] { return snapshot(); });
    if (!moduleResult.isEmpty()) return moduleResult;
    moduleResult = callLifecycleTool(name, arguments, state);
    if (!moduleResult.isEmpty()) return moduleResult;
    moduleResult = callEffectTool(name, arguments, state);
    if (!moduleResult.isEmpty()) return moduleResult;
    moduleResult = callTimelineTool(name, arguments, state);
    if (!moduleResult.isEmpty()) return moduleResult;
    if (!timeline->normalEdit() || !timeline->getCurrentSelection().empty() || timeline->singleSelectionMode())
        return failure(QStringLiteral("UI_BUSY"), QStringLiteral("Use normal edit mode and clear timeline selection for this legacy command."), state);
    for (const auto &entry : state.value(QStringLiteral("imports")).toArray()) {
        if (entry.toObject().value(QStringLiteral("status")).toString() == QLatin1String("loading"))
            return failure(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Wait for native media loading to finish; poll get_state before further edits."),
                           state);
    }
    QJsonObject data{{QStringLiteral("ok"), true}};
    if (name == QLatin1String("import_media")) {
        const auto requested = arguments.value(QStringLiteral("path"));
        if (!requested.isString() || requested.toString().size() > 2048 || requested.toString().contains(QChar::Null))
            return failure(QStringLiteral("INVALID_PATH"), QStringLiteral("A bounded absolute local media path is required."), state);
        const QString requestedPath = QDir::fromNativeSeparators(requested.toString());
        const QFileInfo source(requestedPath);
        const QString path = source.canonicalFilePath();
        const QFileInfo actual(path);
        const QString extension = actual.suffix().toLower();
        const QString mime = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchContent).name();
        const bool stillImage = mime.startsWith(QLatin1String("image/")) && mime != QLatin1String("image/gif");
        const bool supportedContent = stillImage || mime == QLatin1String("image/gif") || mime.startsWith(QLatin1String("audio/")) ||
                                      mime.startsWith(QLatin1String("video/")) || mime == QLatin1String("application/ogg") ||
                                      mime == QLatin1String("application/x-matroska") || mime == QLatin1String("application/mxf");
        if (!QDir::isAbsolutePath(requestedPath) || requestedPath.startsWith(QLatin1String("//")) || path.isEmpty() || path.startsWith(QLatin1String("//")) ||
            !actual.isFile() || !actual.isReadable() || source.isSymLink() || actual.size() <= 0 || actual.size() > MaxMediaBytes || !supportedContent ||
            actual.fileName().contains(QLatin1Char(':')) || QRegularExpression(QStringLiteral("[\\x00-\\x1f]")).match(path).hasMatch())
            return failure(QStringLiteral("INVALID_PATH"),
                           QStringLiteral("Use existing readable local image/audio/video media, at most 64 GiB; no playlists, scripts or links."), state);
        if (extension == QLatin1String("png")) {
            QFile file(path);
            QImageReader reader(path, "png");
            const QSize size = reader.size();
            if (!file.open(QIODevice::ReadOnly) || file.read(8) != QByteArray::fromHex("89504e470d0a1a0a") || !size.isValid() || size.width() < 8 ||
                size.height() < 8 || size.width() > 16384 || size.height() > 16384 || qint64(size.width()) * size.height() > 100000000)
                return failure(QStringLiteral("INVALID_PATH"),
                               QStringLiteral("Use a valid PNG with each side between 8 and 16384 pixels, and at most 100 million pixels."), state);
        } else if (stillImage) {
            QImageReader reader(path);
            const QSize size = reader.size();
            if (!reader.canRead() || !size.isValid() || size.width() < 8 || size.height() < 8 || size.width() > 16384 || size.height() > 16384 ||
                qint64(size.width()) * size.height() > 100000000)
                return failure(QStringLiteral("INVALID_PATH"), QStringLiteral("Use a decodable image with sides 8-16384 and at most 100 million pixels."),
                               state);
        }
        Fun undo = [] { return true; }, redo = [] { return true; };
        // The native load task owns loading and cancellation. Its default no-op ready
        // callback captures neither this service nor a document; state is read from the bin.
        const QString binId = ClipCreator::createClipFromFile(path, bin->getRootFolder()->clipId(), bin, undo, redo, [](const QString &) {}, false, true);
        const auto clip = binId != QLatin1String("-1") && bin->hasClip(binId) ? bin->getClipByBinID(binId) : nullptr;
        if (!clip) {
            const bool restored = undo();
            return failure(restored ? QStringLiteral("IMPORT_FAILED") : QStringLiteral("ROLLBACK_FAILED"),
                           QStringLiteral("Native import could not create a bin item; inspect the returned state."), snapshot());
        }
        m_imports.insert(binId, MediaImport{path, clip});
        pCore->pushUndo(undo, redo, QStringLiteral("MCP import media"));
        data.insert(QStringLiteral("binId"), binId);
        data.insert(QStringLiteral("importStatus"), mediaStatus(clip, path));
    } else if (name == QLatin1String("set_clip_transform") || name == QLatin1String("set_clip_audio")) {
        int clipId = -1;
        if (!integer(arguments, QStringLiteral("clipId"), 0, MaxFrame, clipId) || !timeline->isClip(clipId))
            return failure(QStringLiteral("INVALID_CLIP"), QStringLiteral("Use an existing native timeline clip ID."), state);
        const int trackId = timeline->getClipTrackId(clipId);
        if (trackId < 0 || timeline->trackIsLocked(trackId) || timeline->isInGroup(clipId) || timeline->getMixDuration(clipId) != 0 ||
            timeline->hasClipEndMix(clipId))
            return failure(QStringLiteral("UNSUPPORTED_CLIP"), QStringLiteral("Use an unlocked, ungrouped imported clip without mixes."), state);
        const QString binId = timeline->getClipBinId(clipId);
        const auto imported = m_imports.constFind(binId);
        const auto source = bin->hasClip(binId) ? bin->getClipByBinID(binId) : nullptr;
        const QString sourcePath = imported != m_imports.cend() && imported->clip.lock() == source ? imported->path : QString();
        if (!source || mediaStatus(source, sourcePath) != QLatin1String("ready"))
            return failure(QStringLiteral("INVALID_BIN_CLIP"), QStringLiteral("Effects require a ready current native bin source."), state);
        const bool transforming = name == QLatin1String("set_clip_transform");
        if (timeline->isAudioTrack(trackId) == transforming || (transforming && timeline->getClipState(clipId).first != PlaylistState::VideoOnly) ||
            (!transforming && timeline->getClipState(clipId).first != PlaylistState::AudioOnly))
            return failure(QStringLiteral("UNSUPPORTED_CLIP"), QStringLiteral("Transform video-only clips or adjust audio-only clips."), state);
        const auto effects = timeline->getClipEffectStackModel(clipId);
        if (!effects || effects->rowCount() > 28 || int(nativeEffects(effects).size()) != effects->rowCount())
            return failure(QStringLiteral("UNSUPPORTED_EFFECT_STACK"), QStringLiteral("Use a bounded ungrouped native effect stack."), state);
        if (!effects->isStackEnabled())
            return failure(QStringLiteral("EFFECT_STACK_DISABLED"), QStringLiteral("Enable the clip's effect stack before applying typed effects."), state);
        m_ownedEffects.erase(std::remove_if(m_ownedEffects.begin(), m_ownedEffects.end(),
                                            [](const OwnedEffect &owned) { return owned.effect.expired() || owned.stack.expired(); }),
                             m_ownedEffects.end());
        if (m_ownedEffects.size() >= 4096) return failure(QStringLiteral("LIMIT"), QStringLiteral("Prototype effect history limit reached."), state);
        const auto ownedEffect = [this, clipId, effects](const QString &kind) -> std::shared_ptr<EffectItemModel> {
            for (const auto &owned : m_ownedEffects) {
                auto effect = owned.effect.lock();
                if (owned.clipId == clipId && owned.kind == kind && owned.stack.lock() == effects && effectPresent(effects, effect)) return effect;
            }
            return {};
        };
        for (const auto &kind : {QStringLiteral("transform"), QStringLiteral("gain")}) {
            const auto owned = ownedEffect(kind);
            if (owned && (owned->hasForcedInOut() || owned->filter().get_in() != 0 || owned->filter().get_out() > 0))
                return failure(QStringLiteral("EFFECT_ZONE"),
                               QStringLiteral("An owned transform/gain has a user-defined filter zone; preserve it and choose another clip."), state);
        }
        const int duration = timeline->getClipPlaytime(clipId);
        const int sourceIn = timeline->getClipInOut(clipId).first;
        const int sourceOut = timeline->getClipInOut(clipId).second;
        QVector<QPair<QString, QVariant>> transformParams;
        double gainDb = 0;
        int fadeIn = 0, fadeOut = 0;
        if (transforming) {
            const auto value = arguments.value(QStringLiteral("keyframes"));
            const auto keys = value.toArray();
            if (!value.isArray() || keys.isEmpty() || keys.size() > 64)
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use 1-64 complete transform keyframes."), state);
            const auto previous = ownedEffect(QStringLiteral("transform"));
            for (const auto &effect : nativeEffects(effects)) {
                if (effect != previous && effect->isAssetEnabled() && !effect->isAudio())
                    return failure(QStringLiteral("EFFECT_CONFLICT"),
                                   QStringLiteral("Other active visual effects make bitmap geometry ambiguous; preserve them and choose another clip."), state);
            }
            QStringList rectangles, rotations;
            int last = -1;
            const double width = pCore->getCurrentFrameSize().width(), height = pCore->getCurrentFrameSize().height();
            const QStringList fields{QStringLiteral("frame"),  QStringLiteral("x"),       QStringLiteral("y"),       QStringLiteral("width"),
                                     QStringLiteral("height"), QStringLiteral("opacity"), QStringLiteral("rotation")};
            for (const auto &entry : keys) {
                const auto key = entry.toObject();
                int frame = -1;
                double x, y, w, h, opacity, rotation;
                if (!entry.isObject() || key.size() != fields.size() ||
                    !std::all_of(fields.begin(), fields.end(), [&key](const QString &field) { return key.contains(field); }) ||
                    !integer(key, QStringLiteral("frame"), 0, duration - 1, frame) || frame <= last || (last < 0 && frame != 0) ||
                    !realNumber(key, QStringLiteral("x"), -4 * width, 4 * width, x) || !realNumber(key, QStringLiteral("y"), -4 * height, 4 * height, y) ||
                    !realNumber(key, QStringLiteral("width"), 1, 4 * width, w) || !realNumber(key, QStringLiteral("height"), 1, 4 * height, h) ||
                    !realNumber(key, QStringLiteral("opacity"), 0, 1, opacity) || !realNumber(key, QStringLiteral("rotation"), -360, 360, rotation) ||
                    std::floor(x) != x || std::floor(y) != y || std::floor(w) != w || std::floor(h) != h)
                    return failure(
                        QStringLiteral("INVALID_ARGUMENT"),
                        QStringLiteral(
                            "Use ordered finite clip-local keyframes starting at zero, bounded whole-pixel geometry, opacity 0-1 and rotation -360..360."),
                        state);
                const QString at = QString::number(sourceIn + frame) + QLatin1Char('=');
                rectangles << at + decimal(x) + QLatin1Char(' ') + decimal(y) + QLatin1Char(' ') + decimal(w) + QLatin1Char(' ') + decimal(h) +
                                  QLatin1Char(' ') + decimal(opacity);
                rotations << at + decimal(rotation);
                last = frame;
            }
            transformParams = {{QStringLiteral("rect"), rectangles.join(QLatin1Char(';'))},
                               {QStringLiteral("rotation"), rotations.join(QLatin1Char(';'))},
                               {QStringLiteral("distort"), QStringLiteral("1")},
                               {QStringLiteral("compositing"), QStringLiteral("0")},
                               {QStringLiteral("rotate_anchor"), QString(QString::number(sourceIn) + QStringLiteral("=0.5 0.5 0 0"))}};
            if (!EffectsRepository::get()->exists(QStringLiteral("qtblend")))
                return failure(QStringLiteral("EFFECT_UNAVAILABLE"), QStringLiteral("The native qtblend effect is unavailable."), state);
        } else {
            if (!realNumber(arguments, QStringLiteral("gainDb"), -60, 12, gainDb) ||
                !integer(arguments, QStringLiteral("fadeInFrames"), 0, duration - 1, fadeIn) ||
                !integer(arguments, QStringLiteral("fadeOutFrames"), 0, duration - 1, fadeOut) || qint64(fadeIn) + fadeOut > duration - 1)
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use gain -60..12 dB and nonoverlapping fade spans within the clip."), state);
            for (const auto &pair :
                 {qMakePair(QStringLiteral("fadeIn"), QStringLiteral("fadein")), qMakePair(QStringLiteral("fadeOut"), QStringLiteral("fadeout"))}) {
                const auto previous = ownedEffect(pair.first);
                for (const auto &effect : nativeEffects(effects)) {
                    if (effect->getAssetId() == pair.second && effect != previous)
                        return failure(QStringLiteral("EFFECT_CONFLICT"), QStringLiteral("A non-MCP unique audio fade already exists; it was preserved."),
                                       state);
                }
            }
            if (!EffectsRepository::get()->exists(QStringLiteral("volume")) || (fadeIn > 0 && !EffectsRepository::get()->exists(QStringLiteral("fadein"))) ||
                (fadeOut > 0 && !EffectsRepository::get()->exists(QStringLiteral("fadeout"))))
                return failure(QStringLiteral("EFFECT_UNAVAILABLE"), QStringLiteral("A required native audio effect is unavailable."), state);
        }
        Fun undo = [] { return true; }, redo = [] { return true; };
        QJsonArray applied;
        const auto apply = [this, clipId, sourceIn, effects, &undo, &redo, &applied, &ownedEffect](const QString &kind, const QString &id,
                                                                                                   const QVector<QPair<QString, QVariant>> &parameters,
                                                                                                   int filterIn, int filterOut, bool remove) {
            auto effect = ownedEffect(kind);
            if (remove) {
                if (!effect) return true;
                QString name;
                effects->removeEffectWithUndo(effect, name, undo, redo);
                return !effectPresent(effects, effect);
            }
            if (effect) {
                const auto old = rawEffectParameters(effect);
                const int oldIn = effect->filter().get_in(), oldOut = effect->filter().get_out();
                const bool enabled = effect->isAssetEnabled();
                const auto set = [effect](const QVector<QPair<QString, QVariant>> &params, int in, int out, bool active) {
                    effect->setParameters(params);
                    if (in >= 0) effect->filter().set_in_and_out(in, out);
                    effect->setAssetEnabled(active);
                    return effect->isValid();
                };
                Fun operation = [set, parameters, filterIn, filterOut] { return set(parameters, filterIn, filterOut, true); };
                Fun reverse = [set, old, oldIn, oldOut, enabled] { return set(old, oldIn, oldOut, enabled); };
                Fun previousUndo = undo, previousRedo = redo;
                undo = [reverse, previousUndo] { return reverse() && previousUndo(); };
                redo = [operation, previousRedo] { return previousRedo() && operation(); };
                if (!operation()) return false;
            } else {
                const auto before = nativeEffects(effects);
                QDomDocument document;
                auto container = document.createElement(QStringLiteral("effects"));
                container.setAttribute(QStringLiteral("parentIn"), sourceIn);
                document.appendChild(container);
                auto element = document.createElement(QStringLiteral("effect"));
                element.setAttribute(QStringLiteral("id"), id);
                if (filterIn >= 0) {
                    element.setAttribute(QStringLiteral("in"), filterIn);
                    element.setAttribute(QStringLiteral("out"), filterOut);
                }
                for (const auto &parameter : parameters) {
                    auto property = document.createElement(QStringLiteral("property"));
                    property.setAttribute(QStringLiteral("name"), parameter.first);
                    property.appendChild(document.createTextNode(parameter.second.toString()));
                    element.appendChild(property);
                }
                container.appendChild(element);
                if (!effects->copyXmlEffectWithUndo(container, undo, redo)) return false;
                for (const auto &candidate : nativeEffects(effects)) {
                    if (std::find(before.begin(), before.end(), candidate) == before.end() && candidate->getAssetId() == id) {
                        effect = candidate;
                        break;
                    }
                }
                if (!effect || !effect->isValid()) return false;
                if (filterIn >= 0) {
                    // Native paste offsets fade bounds after model construction;
                    // synchronize the public position parameters with those bounds.
                    effect->setParameter(QStringLiteral("in"), filterIn);
                    effect->setParameter(QStringLiteral("out"), filterOut);
                }
                m_ownedEffects.push_back({clipId, kind, effects, effect});
            }
            if (filterIn >= 0 && (effect->filter().get_in() != filterIn || effect->filter().get_out() != filterOut)) return false;
            applied.append(effectReadback(kind, effect));
            return true;
        };
        bool success;
        if (transforming) {
            success = apply(QStringLiteral("transform"), QStringLiteral("qtblend"), transformParams, -1, -1, false);
        } else {
            success = apply(QStringLiteral("gain"), QStringLiteral("volume"),
                            {{QStringLiteral("level"), QString(QString::number(sourceIn) + QLatin1Char('=') + decimal(gainDb))}}, -1, -1, false) &&
                      apply(QStringLiteral("fadeIn"), QStringLiteral("fadein"),
                            {{QStringLiteral("in"), QString::number(sourceIn)},
                             {QStringLiteral("out"), QString::number(sourceIn + fadeIn)},
                             {QStringLiteral("gain"), QStringLiteral("0")},
                             {QStringLiteral("end"), QStringLiteral("1")}},
                            sourceIn, sourceIn + fadeIn, fadeIn == 0) &&
                      apply(QStringLiteral("fadeOut"), QStringLiteral("fadeout"),
                            {{QStringLiteral("in"), QString::number(sourceOut - fadeOut)},
                             {QStringLiteral("out"), QString::number(sourceOut)},
                             {QStringLiteral("gain"), QStringLiteral("1")},
                             {QStringLiteral("end"), QStringLiteral("0")}},
                            sourceOut - fadeOut, sourceOut, fadeOut == 0);
        }
        if (success && transforming) {
            const auto actual = ownedEffect(QStringLiteral("transform"));
            if (!actual) success = false;
            for (const auto &entry : arguments.value(QStringLiteral("keyframes")).toArray()) {
                if (!success) break;
                const auto key = entry.toObject();
                const int frame = sourceIn + key.value(QStringLiteral("frame")).toInt();
                const auto rectangle = actual->filter().anim_get_rect("rect", frame);
                const double rotation = actual->filter().anim_get_double("rotation", frame);
                const auto close = [&key](double value, const QString &name) { return std::abs(value - key.value(name).toDouble()) <= 0.00001; };
                success = close(rectangle.x, QStringLiteral("x")) && close(rectangle.y, QStringLiteral("y")) && close(rectangle.w, QStringLiteral("width")) &&
                          close(rectangle.h, QStringLiteral("height")) && close(rectangle.o, QStringLiteral("opacity")) &&
                          close(rotation, QStringLiteral("rotation"));
            }
        } else if (success) {
            const auto gain = ownedEffect(QStringLiteral("gain"));
            success = gain && std::abs(gain->filter().anim_get_double("level", sourceIn) - gainDb) <= 0.00001 &&
                      std::abs(gain->filter().anim_get_double("level", sourceOut) - gainDb) <= 0.00001 && effects->getFadePosition(true) == fadeIn &&
                      effects->getFadePosition(false) == fadeOut;
        }
        const bool consistent = effects->checkConsistency();
        if (!success || !consistent) {
            QJsonObject diagnostics{{QStringLiteral("matched"), success},
                                    {QStringLiteral("consistent"), consistent},
                                    {QStringLiteral("attemptedEffects"), applied},
                                    {QStringLiteral("nativeFadeInSpan"), effects->getFadePosition(true)},
                                    {QStringLiteral("nativeFadeOutSpan"), effects->getFadePosition(false)}};
            if (const auto gain = ownedEffect(QStringLiteral("gain"))) {
                diagnostics.insert(QStringLiteral("gainRaw"), gain->getParam(QStringLiteral("level")));
                diagnostics.insert(QStringLiteral("gainAtSourceIn"), gain->filter().anim_get_double("level", sourceIn));
                diagnostics.insert(QStringLiteral("gainAtSourceOut"), gain->filter().anim_get_double("level", sourceOut));
            }
            const bool restored = undo();
            return failure(restored ? QStringLiteral("EFFECT_FAILED") : QStringLiteral("ROLLBACK_FAILED"),
                           QStringLiteral("Native effect application did not match the contract; inspect the returned state."), snapshot(), diagnostics);
        }
        pCore->pushUndo(undo, redo, transforming ? QStringLiteral("MCP set clip transform") : QStringLiteral("MCP set clip audio"));
        data.insert(QStringLiteral("clipId"), clipId);
        data.insert(QStringLiteral("appliedEffects"), applied);
    } else if (name == QLatin1String("insert_media_clip")) {
        const auto requestedId = arguments.value(QStringLiteral("binId"));
        if (!requestedId.isString() || !QRegularExpression(QStringLiteral("^[0-9]{1,12}$")).match(requestedId.toString()).hasMatch())
            return failure(QStringLiteral("INVALID_BIN_CLIP"), QStringLiteral("Use the binId returned by import_media."), state);
        const QString binId = requestedId.toString();
        const auto imported = m_imports.constFind(binId);
        const auto clip = bin->hasClip(binId) ? bin->getClipByBinID(binId) : nullptr;
        if (!clip) return failure(QStringLiteral("INVALID_BIN_CLIP"), QStringLiteral("Use a current native bin clip ID."), state);
        const QString localPath = imported != m_imports.cend() && imported->clip.lock() == clip ? imported->path : QString();
        const QString status = mediaStatus(clip, localPath);
        if (status != QLatin1String("ready"))
            return failure(status == QLatin1String("loading")           ? QStringLiteral("MEDIA_NOT_READY")
                           : status == QLatin1String("unsupported")     ? QStringLiteral("MEDIA_UNSUPPORTED")
                           : status == QLatin1String("needs_transcode") ? QStringLiteral("TRANSCODE_REQUIRED")
                                                                        : QStringLiteral("MEDIA_FAILED"),
                           QStringLiteral("The imported native producer is not ready for insertion."), state);
        const bool audio = arguments.value(QStringLiteral("stream")).toString() == QLatin1String("audio");
        const bool video = arguments.value(QStringLiteral("stream")).toString() == QLatin1String("video");
        if ((!audio && !video) ||
            (video && ((clip->clipType() != ClipType::Image && clip->clipType() != ClipType::Video && clip->clipType() != ClipType::AV &&
                        clip->clipType() != ClipType::Text && clip->clipType() != ClipType::TextTemplate && clip->clipType() != ClipType::Color &&
                        clip->clipType() != ClipType::Timeline && clip->clipType() != ClipType::SlideShow) ||
                       !clip->hasVideo())) ||
            (audio && ((clip->clipType() != ClipType::Audio && clip->clipType() != ClipType::AV) || !clip->hasAudio() || clip->audioStreamsCount() > 1 ||
                       clip->activeStreams().size() > 1 || (clip->clipType() == ClipType::AV && clip->activeStreams().isEmpty()))))
            return failure(
                QStringLiteral("MEDIA_UNSUPPORTED"),
                QStringLiteral("Use insert_media_streams for explicit multistream audio selection; this command accepts one audio stream or supported video."),
                state);
        if (!localPath.isEmpty() && (!QFileInfo(localPath).isFile() || !QFileInfo(localPath).isReadable()))
            return failure(QStringLiteral("MEDIA_FAILED"), QStringLiteral("The imported local source is no longer readable."), state);
        int trackId = -1, start = -1, sourceIn = -1, duration = 0;
        if (!integer(arguments, QStringLiteral("trackId"), 0, MaxFrame, trackId) || !timeline->isTrack(trackId) || timeline->trackIsLocked(trackId) ||
            timeline->isAudioTrack(trackId) != audio)
            return failure(QStringLiteral("INVALID_TRACK"), QStringLiteral("Use an unlocked track matching the requested stream."), state);
        if (!integer(arguments, QStringLiteral("startFrame"), 0, MaxFrame, start) ||
            !integer(arguments, QStringLiteral("sourceInFrame"), 0, MaxFrame, sourceIn) ||
            !integer(arguments, QStringLiteral("durationFrames"), 1, MaxDuration, duration) || qint64(start) + duration > MaxFrame ||
            qint64(sourceIn) + duration > MaxFrame || qint64(sourceIn) + duration > clip->getFramePlaytime())
            return failure(QStringLiteral("INVALID_SOURCE_RANGE"),
                           QStringLiteral("Use bounded integral timeline/source frames within the native source duration."), state);
        if (clip->clipType() == ClipType::Image && sourceIn != 0)
            return failure(QStringLiteral("INVALID_SOURCE_RANGE"), QStringLiteral("Native still-image insertion starts at source frame zero."), state);
        for (int other : timeline->getItemsInRange(trackId, 0, -1, true)) {
            const qint64 otherStart = timeline->getItemPosition(other), otherEnd = otherStart + timeline->getItemPlaytime(other);
            if (qint64(start) < otherEnd && qint64(start) + duration > otherStart)
                return failure(QStringLiteral("COLLISION"), QStringLiteral("Destination overlaps another item."), state);
        }
        const int previousCount = timeline->getClipsCount();
        const int sourceOut = sourceIn + duration - 1;
        const QString insertion = (audio ? QStringLiteral("A") : QStringLiteral("V")) + binId + QLatin1Char('/') + QString::number(sourceIn) +
                                  QLatin1Char('/') + QString::number(sourceOut);
        Fun undo = [] { return true; }, redo = [] { return true; };
        int clipId = -1;
        const bool inserted = timeline->requestClipInsertion(insertion, trackId, start, clipId, true, true, false, undo, redo, QVector<int>{trackId}, 1);
        const bool exact = inserted && timeline->isClip(clipId) && timeline->getClipsCount() == previousCount + 1 &&
                           timeline->getClipTrackId(clipId) == trackId && timeline->getClipPosition(clipId) == start &&
                           timeline->getClipPlaytime(clipId) == duration && timeline->getClipInOut(clipId) == std::make_pair(sourceIn, sourceOut) &&
                           timeline->getClipBinId(clipId) == binId && timeline->getCurrentSelection().empty();
        if (!exact) {
            const bool restored = undo();
            return failure(restored ? QStringLiteral("EDIT_FAILED") : QStringLiteral("ROLLBACK_FAILED"),
                           restored ? QStringLiteral("Native insertion did not match the requested range; changes were rolled back.")
                                    : QStringLiteral("Insertion rollback failed; inspect the project."),
                           snapshot());
        }
        pCore->pushUndo(undo, redo, QStringLiteral("MCP insert media clip"));
        data.insert(QStringLiteral("binId"), binId);
        data.insert(QStringLiteral("clipId"), clipId);
        data.insert(QStringLiteral("stream"), audio ? QStringLiteral("audio") : QStringLiteral("video"));
    } else if (name == QLatin1String("save_project")) {
        const auto requested = arguments.value(QStringLiteral("path"));
        if (!requested.isString() || requested.toString().size() > 2048)
            return failure(QStringLiteral("INVALID_PATH"), QStringLiteral("An absolute local .kdenlive path is required."), state);
        const QString path = QDir::fromNativeSeparators(requested.toString());
        const QFileInfo target(path);
        const QString leaf = target.fileName();
        const QString parent = target.absoluteDir().canonicalPath();
        if (!QDir::isAbsolutePath(path) || path.startsWith(QLatin1String("//")) || parent.isEmpty() ||
            target.suffix().compare(QLatin1String("kdenlive"), Qt::CaseInsensitive) != 0 || leaf.isEmpty() || leaf.size() > 200 ||
            leaf.contains(QLatin1Char(':')) || leaf.contains(QLatin1Char('"')) ||
            QRegularExpression(QStringLiteral("[<>|?*\\x00-\\x1f]")).match(leaf).hasMatch() || !QFileInfo(parent).isWritable() || target.exists() ||
            target.isSymLink())
            return failure(QStringLiteral("INVALID_PATH"), QStringLiteral("Use a new local .kdenlive file in an existing writable folder."), state);
        const QString actual = QDir(parent).absoluteFilePath(leaf);
        if (QFileInfo::exists(actual) || QFileInfo(actual).isSymLink())
            return failure(QStringLiteral("FILE_EXISTS"), QStringLiteral("The output file already exists."), state);
        if (!pCore->window()) return failure(QStringLiteral("GUI_REQUIRED"), QStringLiteral("Saving requires the running GUI."), state);
        QString error;
        const bool saved = pCore->projectManager()->saveFileAs(actual, false, true, &error);
        const QFileInfo persisted(actual);
        if (!saved || !persisted.isFile() || persisted.size() <= 0)
            return failure(QStringLiteral("SAVE_FAILED"), error.isEmpty() ? QStringLiteral("The project copy was not saved.") : error, snapshot());
        data.insert(QStringLiteral("path"), persisted.absoluteFilePath());
        data.insert(QStringLiteral("saveAsCopy"), true);
        data.insert(QStringLiteral("persisted"), true);
    } else if (name == QLatin1String("undo") || name == QLatin1String("redo")) {
        const bool undoing = name == QLatin1String("undo");
        if (undoing ? !stack->canUndo() : !stack->canRedo())
            return failure(QStringLiteral("NO_HISTORY"), QStringLiteral("No matching history command."), state);
        data.insert(QStringLiteral("command"), undoing ? stack->undoText() : stack->redoText());
        if (undoing)
            stack->undo();
        else
            stack->redo();
    } else {
        int tid = -1, cid = -1, frame = -1;
        if (name != QLatin1String("cut_clip")) {
            if (!integer(arguments, QStringLiteral("trackId"), 0, MaxFrame, tid) || !timeline->isTrack(tid))
                return failure(QStringLiteral("INVALID_TRACK"), QStringLiteral("Unknown trackId."), state);
            if (timeline->isAudioTrack(tid) || timeline->trackIsLocked(tid))
                return failure(QStringLiteral("INVALID_TRACK"), QStringLiteral("An unlocked video track is required."), state);
        }
        if (name != QLatin1String("add_color_clip")) {
            if (!integer(arguments, QStringLiteral("clipId"), 0, MaxFrame, cid) || !timeline->isClip(cid))
                return failure(QStringLiteral("INVALID_CLIP"), QStringLiteral("Unknown clipId."), state);
            const int sourceTrack = timeline->getClipTrackId(cid);
            if (sourceTrack < 0 || timeline->trackIsLocked(sourceTrack) || timeline->isInGroup(cid) || timeline->getMixDuration(cid) != 0 ||
                timeline->hasClipEndMix(cid) || timeline->getClipState(cid).second != ClipType::Color)
                return failure(QStringLiteral("UNSUPPORTED_CLIP"), QStringLiteral("Use an unlocked, ungrouped color clip without mixes."), state);
        }
        const QString frameKey = name == QLatin1String("cut_clip") ? QStringLiteral("frame") : QStringLiteral("startFrame");
        if (!integer(arguments, frameKey, 0, MaxFrame, frame))
            return failure(QStringLiteral("INVALID_FRAME"), QStringLiteral("A bounded non-negative integral frame is required."), state);
        int duration = name == QLatin1String("add_color_clip") ? 0 : timeline->getClipPlaytime(cid);
        QString color, clipName;
        if (name == QLatin1String("add_color_clip")) {
            color = arguments.value(QStringLiteral("color")).toString();
            clipName = arguments.contains(QStringLiteral("name")) ? arguments.value(QStringLiteral("name")).toString() : QStringLiteral("MCP color");
            if (!integer(arguments, QStringLiteral("durationFrames"), 1, MaxDuration, duration) ||
                !QRegularExpression(QStringLiteral("^#[0-9A-Fa-f]{8}$")).match(color).hasMatch() ||
                (arguments.contains(QStringLiteral("name")) && !arguments.value(QStringLiteral("name")).isString()) || clipName.isEmpty() ||
                clipName.size() > 160 || clipName.contains(QChar::Null))
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use positive duration, #RRGGBBAA color and a 1-160 character name."), state);
        }
        if (name == QLatin1String("cut_clip")) {
            const int start = timeline->getClipPosition(cid);
            if (frame <= start || qint64(frame) >= qint64(start) + duration)
                return failure(QStringLiteral("CUT_BOUNDARY"), QStringLiteral("Cut strictly inside the clip."), state);
        } else {
            if (qint64(frame) + duration > MaxFrame)
                return failure(QStringLiteral("INVALID_FRAME"), QStringLiteral("Clip end exceeds prototype frame limit."), state);
            for (int other : timeline->getItemsInRange(tid, 0, -1, true)) {
                if (other == cid) continue;
                const qint64 start = timeline->getItemPosition(other), end = start + timeline->getItemPlaytime(other);
                if (qint64(frame) < end && qint64(frame) + duration > start)
                    return failure(QStringLiteral("COLLISION"), QStringLiteral("Destination overlaps another item."), state);
            }
        }
        Fun undo = [] { return true; }, redo = [] { return true; };
        bool success = false;
        QString undoText;
        if (name == QLatin1String("add_color_clip")) {
            // Match native color loading so RGB optimization retains the image
            // converters needed by affine/slide and other RGBA consumers.
            const QByteArray resource = (QStringLiteral("color:0x") + color.mid(1)).toLatin1();
            auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), "loader", resource.constData());
            if (!producer->is_valid()) return failure(QStringLiteral("PRODUCER"), QStringLiteral("MLT could not create the color producer."), state);
            producer->set("length", duration);
            producer->set_in_and_out(0, duration - 1);
            producer->set("kdenlive:duration", duration);
            producer->set("kdenlive:clipname", clipName.toUtf8().constData());
            QString binId;
            if (bin->requestAddBinClip(binId, producer, bin->getRootFolder()->clipId(), undo, redo)) {
                bin->getClipByBinID(binId)->forceLimitedDuration();
                success = timeline->requestClipInsertion(QStringLiteral("V") + binId, tid, frame, cid, true, true, false, undo, redo, QVector<int>{tid}, 1);
                data.insert(QStringLiteral("binId"), binId);
            }
            undoText = QStringLiteral("MCP add color clip");
        } else if (name == QLatin1String("move_clip")) {
            success = timeline->requestClipMove(cid, tid, frame, false, true, true, true, undo, redo) == TimelineModel::MoveSuccess;
            undoText = QStringLiteral("MCP move clip");
        } else {
            success = TimelineFunctions::requestClipCut(timeline, cid, frame, undo, redo);
            // The high-level cut already rolls back its accumulated changes on failure.
            if (!success) return failure(QStringLiteral("EDIT_FAILED"), QStringLiteral("Kdenlive rejected the cut."), snapshot());
            const int rightId = timeline->getClipByPosition(timeline->getClipTrackId(cid), frame);
            success = rightId >= 0 && rightId != cid && timeline->getClipPosition(rightId) == frame &&
                      timeline->getClipPosition(cid) + timeline->getClipPlaytime(cid) == frame && timeline->getCurrentSelection().empty();
            data.insert(QStringLiteral("leftClipId"), cid);
            data.insert(QStringLiteral("rightClipId"), rightId);
            undoText = QStringLiteral("MCP cut clip");
        }
        if (success && name != QLatin1String("cut_clip"))
            success = timeline->isClip(cid) && timeline->getClipTrackId(cid) == tid && timeline->getClipPosition(cid) == frame &&
                      timeline->getClipPlaytime(cid) == duration;
        if (!success) {
            const bool restored = undo();
            return failure(restored ? QStringLiteral("EDIT_FAILED") : QStringLiteral("ROLLBACK_FAILED"),
                           restored ? QStringLiteral("Kdenlive rejected the edit; changes were rolled back.")
                                    : QStringLiteral("Rollback failed; inspect the project."),
                           snapshot());
        }
        pCore->pushUndo(undo, redo, undoText);
        data.insert(QStringLiteral("clipId"), cid);
    }
    ++m_revision;
    data.insert(QStringLiteral("state"), snapshot());
    return toolResult(data);
}
