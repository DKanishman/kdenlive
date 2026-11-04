/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
// Real Qt wire encoding is the size oracle. Receipt tests do not replay edits.
#include "kdenlivesettings.h"
#include "mcp/mcpwire.h"
#include "renderpresets/renderpresetmodel.hpp"
#include "renderpresets/renderpresetrepository.hpp"
#include <QByteArrayView>
#include <QObject>
#include <QStringList>

namespace {
QJsonObject wireRpc(const QJsonObject &result, const QJsonValue &id = QJsonValue(17))
{
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {QStringLiteral("result"), result}};
}
QJsonObject wireData(const QJsonObject &result)
{
    return result.value(QStringLiteral("structuredContent")).toObject();
}
qsizetype wireActualBytes(const QJsonObject &response)
{
    return QJsonDocument(response).toJson(QJsonDocument::Compact).size();
}
QJsonObject wireUnwrapSuccess(const QJsonObject &result)
{
    REQUIRE_FALSE(result.value(QStringLiteral("isError")).toBool());
    const auto data = wireData(result);
    REQUIRE(data.value(QStringLiteral("ok")).toBool());
    return data;
}
QString wireCapture(QObject *owner, const QJsonObject &payload, const QString &tool = QStringLiteral("native_fixture"), bool mutation = true,
                    bool error = false)
{
    const auto response = McpWire::boundRpcResponse(owner, wireRpc(McpWire::toolResult(payload, error)), tool, mutation);
    REQUIRE(wireActualBytes(response) <= McpWire::MaxResponseBytes);
    REQUIRE_FALSE(response.contains(QStringLiteral("error")));
    const auto result = response.value(QStringLiteral("result")).toObject();
    CHECK(result.value(QStringLiteral("isError")).toBool() == error);
    const auto summary = wireData(result);
    CHECK(summary.value(QStringLiteral("ok")).toBool() == payload.value(QStringLiteral("ok")).toBool());
    const auto id = summary.value(QStringLiteral("receipt")).toObject().value(QStringLiteral("id")).toString();
    REQUIRE_FALSE(id.isEmpty());
    return id;
}
QJsonObject wireReceiptPage(QObject *owner, const QString &id, const QString &path = {}, int offset = 0, int limit = 64)
{
    const auto result = McpWire::callTool(
        owner, QStringLiteral("get_result_page"),
        {{QStringLiteral("receiptId"), id}, {QStringLiteral("path"), path}, {QStringLiteral("offset"), offset}, {QStringLiteral("limit"), limit}});
    CHECK(wireActualBytes(wireRpc(result, QString(128, QChar::Null))) <= McpWire::MaxResponseBytes);
    return wireUnwrapSuccess(result);
}
} // namespace

TEST_CASE("MCP wire budgets match real Qt compact JSON and preserve small replies", "[MCP][MCPWire][budget]")
{
    const auto unicode = QString::fromUtf8("שלום 中文 😀\"\\\n\t");
    const QList<QJsonValue> values{QJsonValue::Null,
                                   true,
                                   1.25,
                                   -1e300,
                                   unicode,
                                   QJsonArray{unicode, false, 19},
                                   QJsonObject{{QStringLiteral("a/b~c"), unicode}, {QStringLiteral("nested"), QJsonObject{{QStringLiteral("x"), 10}}}}};
    for (const auto &value : values) {
        const auto actual = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).size() - 2;
        CHECK(McpWire::jsonEncodedSize(value, 1024 * 1024) == actual);
        CHECK(McpWire::jsonSizeUpperBound(value) >= quint64(actual));
    }
    QJsonValue nested = unicode;
    for (int i = 0; i < 40; ++i)
        nested = QJsonObject{{QStringLiteral("child"), nested}};
    CHECK(McpWire::jsonEncodedSize(nested) == QJsonDocument(QJsonArray{nested}).toJson(QJsonDocument::Compact).size() - 2);
    const QJsonObject payload{{QStringLiteral("ok"), true}, {QStringLiteral("clipId"), 42}, {QStringLiteral("text"), unicode}};
    const auto result = McpWire::toolResult(payload);
    REQUIRE(result.value(QStringLiteral("content")).toArray().size() == 1);
    const auto text = result.value(QStringLiteral("content")).toArray().at(0).toObject().value(QStringLiteral("text")).toString();
    CHECK(QJsonDocument::fromJson(text.toUtf8()).object() == payload);
    CHECK(wireData(result) == payload);
    CHECK_FALSE(result.contains(QStringLiteral("_meta")));
    CHECK(wireActualBytes(wireRpc(result, QString(128, QChar::Null))) <= McpWire::MaxResponseBytes);
}

TEST_CASE("MCP wire omits only the duplicate text when complete structured data fits", "[MCP][MCPWire][unicode]")
{
    const QString text(220000, QChar(u'中'));
    const QJsonObject payload{{QStringLiteral("ok"), true}, {QStringLiteral("text"), text}, {QStringLiteral("clipId"), 73}};
    const auto result = McpWire::toolResult(payload);
    REQUIRE(result.value(QStringLiteral("_meta")).toObject().value(QStringLiteral("textCopyOmitted")).toBool());
    CHECK(wireData(result) == payload);
    CHECK(wireActualBytes(wireRpc(result, QString(128, QChar::Null))) <= McpWire::MaxResponseBytes);
    QObject owner;
    const auto response = McpWire::boundRpcResponse(&owner, wireRpc(result, QString(128, QChar::Null)), QStringLiteral("get_native_text"), false);
    CHECK(wireData(response.value(QStringLiteral("result")).toObject()) == payload);
    CHECK(McpWire::mutationPreflight(&owner, QStringLiteral("move_clip"), true).isEmpty());
}

TEST_CASE("MCP retained success preserves IDs and pages every captured UTF16 text unit", "[MCP][MCPWire][receipt]")
{
    QObject owner;
    const QString text = QString(400000, QChar(u'中')) + QString::fromUtf8(" 😀 done");
    QJsonObject payload{
        {QStringLiteral("ok"), true}, {QStringLiteral("clipId"), 37}, {QStringLiteral("sourceUnchanged"), true}, {QStringLiteral("text"), text}};
    const auto bounded =
        McpWire::boundRpcResponse(&owner, wireRpc(McpWire::toolResult(payload), QString(128, QChar::Null)), QStringLiteral("native_edit"), true);
    REQUIRE(wireActualBytes(bounded) <= McpWire::MaxResponseBytes);
    const auto summary = wireData(bounded.value(QStringLiteral("result")).toObject());
    REQUIRE(summary.value(QStringLiteral("ok")).toBool());
    CHECK(summary.value(QStringLiteral("clipId")).toInt() == 37);
    CHECK(summary.value(QStringLiteral("sourceUnchanged")).toBool());
    const auto id = summary.value(QStringLiteral("receipt")).toObject().value(QStringLiteral("id")).toString();
    REQUIRE_FALSE(id.isEmpty());
    QObject otherSession;
    CHECK(McpWire::callTool(&otherSession, QStringLiteral("get_result_page"), {{QStringLiteral("receiptId"), id}}).value(QStringLiteral("isError")).toBool());
    payload.insert(QStringLiteral("clipId"), 999); // Qt detaches; captured data stays immutable.
    CHECK(wireReceiptPage(&owner, id, QStringLiteral("/clipId")).value(QStringLiteral("value")).toInt() == 37);
    QString recovered;
    int offset = 0;
    do {
        const auto page = wireReceiptPage(&owner, id, QStringLiteral("/text"), offset, 8192);
        recovered += page.value(QStringLiteral("text")).toString();
        const int next = page.value(QStringLiteral("nextOffset")).toInt();
        REQUIRE(next > offset);
        offset = next;
        if (!page.value(QStringLiteral("hasMore")).toBool()) break;
    } while (offset < text.size());
    CHECK(recovered == text);
    CHECK(offset == text.size());
    REQUIRE_FALSE(McpWire::mutationPreflight(&owner, QStringLiteral("duplicate_clip"), true).isEmpty());
    CHECK(McpWire::mutationPreflight(&owner, QStringLiteral("get_state"), false).isEmpty());
    const auto wrong =
        McpWire::callTool(&owner, QStringLiteral("release_result_receipt"), {{QStringLiteral("receiptId"), id}, {QStringLiteral("acknowledgeRelease"), false}});
    CHECK(wrong.value(QStringLiteral("isError")).toBool());
    REQUIRE_FALSE(McpWire::mutationPreflight(&owner, QStringLiteral("duplicate_clip"), true).isEmpty());
    wireUnwrapSuccess(
        McpWire::callTool(&owner, QStringLiteral("release_result_receipt"), {{QStringLiteral("receiptId"), id}, {QStringLiteral("acknowledgeRelease"), true}}));
    CHECK(McpWire::mutationPreflight(&owner, QStringLiteral("duplicate_clip"), true).isEmpty());
    CHECK(McpWire::callTool(&owner, QStringLiteral("get_result_page"), {{QStringLiteral("receiptId"), id}}).value(QStringLiteral("isError")).toBool());
}

TEST_CASE("MCP retained errors preserve native outcome and cannot be displaced by another read", "[MCP][MCPWire][error]")
{
    QObject owner;
    const QString message(1200000, QLatin1Char('x'));
    const QJsonObject payload{
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("ROLLBACK_FAILED")}, {QStringLiteral("message"), message}}},
        {QStringLiteral("affectedClipId"), 92}};
    const auto response = McpWire::boundRpcResponse(&owner, wireRpc(McpWire::toolResult(payload, true)), QStringLiteral("native_edit"), true);
    REQUIRE(wireActualBytes(response) <= McpWire::MaxResponseBytes);
    const auto result = response.value(QStringLiteral("result")).toObject();
    REQUIRE(result.value(QStringLiteral("isError")).toBool());
    const auto summary = wireData(result);
    CHECK_FALSE(summary.value(QStringLiteral("ok")).toBool(true));
    CHECK(summary.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString() == QStringLiteral("ROLLBACK_FAILED"));
    CHECK(summary.value(QStringLiteral("affectedClipId")).toInt() == 92);
    const auto id = summary.value(QStringLiteral("receipt")).toObject().value(QStringLiteral("id")).toString();
    REQUIRE_FALSE(id.isEmpty());
    const auto next = McpWire::boundRpcResponse(&owner, wireRpc(McpWire::toolResult({{QStringLiteral("ok"), true}, {QStringLiteral("other"), message}})),
                                                QStringLiteral("get_other_details"), false);
    const auto refused = wireData(next.value(QStringLiteral("result")).toObject());
    CHECK(refused.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString() == QStringLiteral("RESULT_RECEIPT_PENDING"));
    CHECK(refused.value(QStringLiteral("receipt")).toObject().value(QStringLiteral("id")).toString() == id);
    CHECK(wireReceiptPage(&owner, id, QStringLiteral("/error/message"), 1199990, 64).value(QStringLiteral("text")).toString() == QString(10, QLatin1Char('x')));
}

TEST_CASE("MCP retained asynchronous acceptance noops and late cancellation retain their own meaning", "[MCP][MCPWire][async][cancel]")
{
    QObject owner;
    CHECK_FALSE(wireUnwrapSuccess(McpWire::callTool(&owner, QStringLiteral("get_result_page"), {})).value(QStringLiteral("pending")).toBool());
    const QJsonObject job{{QStringLiteral("jobId"), QStringLiteral("owned-job-17")},
                          {QStringLiteral("status"), QStringLiteral("queued")},
                          {QStringLiteral("finished"), false},
                          {QStringLiteral("operation"), QStringLiteral("open_project")}};
    const QJsonObject state{{QStringLiteral("sessionId"), QStringLiteral("wire-session")},
                            {QStringLiteral("documentId"), QStringLiteral("wire-document")},
                            {QStringLiteral("sequenceId"), QStringLiteral("wire-sequence")},
                            {QStringLiteral("expectedState"), QStringLiteral("opaque-native-token")}};
    const QJsonObject accepted{{QStringLiteral("ok"), true},       {QStringLiteral("accepted"), true},
                               {QStringLiteral("changed"), false}, {QStringLiteral("job"), job},
                               {QStringLiteral("state"), state},   {QStringLiteral("details"), QString(1200000, QLatin1Char('x'))}};
    const auto response = McpWire::boundRpcResponse(&owner, wireRpc(McpWire::toolResult(accepted)), QStringLiteral("open_project"), true);
    const auto summary = wireData(response.value(QStringLiteral("result")).toObject());
    REQUIRE(summary.value(QStringLiteral("ok")).toBool());
    CHECK(summary.value(QStringLiteral("accepted")).toBool());
    CHECK_FALSE(summary.value(QStringLiteral("changed")).toBool(true));
    CHECK(summary.value(QStringLiteral("job")).toObject() == job);
    CHECK_FALSE(summary.contains(QStringLiteral("committed")));
    CHECK_FALSE(summary.contains(QStringLiteral("completed")));
    CHECK(summary.value(QStringLiteral("state")).toObject().value(QStringLiteral("documentId")) == state.value(QStringLiteral("documentId")));
    CHECK(summary.value(QStringLiteral("state")).toObject().value(QStringLiteral("expectedState")) == state.value(QStringLiteral("expectedState")));
    const auto id = summary.value(QStringLiteral("receipt")).toObject().value(QStringLiteral("id")).toString();
    REQUIRE_FALSE(id.isEmpty());
    CHECK(wireUnwrapSuccess(McpWire::callTool(&owner, QStringLiteral("get_result_page"), {}))
              .value(QStringLiteral("receipt"))
              .toObject()
              .value(QStringLiteral("id"))
              .toString() == id);
    REQUIRE_FALSE(McpWire::mutationPreflight(&owner, QStringLiteral("move_clip"), true).isEmpty());
    for (const auto &name : {QStringLiteral("cancel_render"), QStringLiteral("cancel_transcode"), QStringLiteral("cancel_speech_job"),
                             QStringLiteral("cancel_effect_analysis"), QStringLiteral("cancel_media_job")})
        CHECK(McpWire::mutationPreflight(&owner, name, true).isEmpty());
    auto stoppingJob = job;
    stoppingJob.insert(QStringLiteral("status"), QStringLiteral("canceling"));
    stoppingJob.insert(QStringLiteral("cancelRequested"), true);
    stoppingJob.insert(QStringLiteral("outputs"), QJsonArray{QString(1200000, QLatin1Char('p'))});
    const QJsonObject stopping{{QStringLiteral("ok"), true}, {QStringLiteral("job"), stoppingJob}, {QStringLiteral("state"), state}};
    const auto late = McpWire::boundRpcResponse(&owner, wireRpc(McpWire::toolResult(stopping)), QStringLiteral("cancel_render"), true);
    CHECK(wireActualBytes(late) <= McpWire::MaxResponseBytes);
    const auto stopped = wireData(late.value(QStringLiteral("result")).toObject());
    CHECK(stopped.value(QStringLiteral("ok")).toBool());
    CHECK(stopped.value(QStringLiteral("job")).toObject().value(QStringLiteral("jobId")).toString() == QStringLiteral("owned-job-17"));
    CHECK(stopped.value(QStringLiteral("job")).toObject().value(QStringLiteral("status")).toString() == QStringLiteral("canceling"));
    CHECK_FALSE(stopped.value(QStringLiteral("job")).toObject().value(QStringLiteral("finished")).toBool(true));
    CHECK(stopped.value(QStringLiteral("detailsDeferred")).toBool());
    CHECK(stopped.value(QStringLiteral("pendingResultReceipt")).toObject().value(QStringLiteral("id")).toString() == id);
    CHECK(wireReceiptPage(&owner, id, QStringLiteral("/job/status")).value(QStringLiteral("text")).toString() == QStringLiteral("queued"));
    CHECK(wireUnwrapSuccess(McpWire::callTool(&owner, QStringLiteral("get_result_page"), {}))
              .value(QStringLiteral("receipt"))
              .toObject()
              .value(QStringLiteral("id"))
              .toString() == id);
}

TEST_CASE("MCP large native job summaries preserve identity outcomes and cancellation", "[MCP][MCPWire][async][largejob][cancel]")
{
    const QStringList tools{QStringLiteral("open_project"), QStringLiteral("start_render"), QStringLiteral("start_speech_job")};
    const QStringList cancellations{QStringLiteral("cancel_media_job"), QStringLiteral("cancel_render"), QStringLiteral("cancel_speech_job")};
    const QStringList kinds{QStringLiteral("project"), QStringLiteral("render"), QStringLiteral("speech")};
    for (int i = 0; i < tools.size(); ++i) {
        QObject owner;
        const QString jobId = QStringLiteral("owned-large-job-%1").arg(i);
        const QJsonObject job{{QStringLiteral("jobId"), jobId},
                              {QStringLiteral("kind"), kinds.at(i)},
                              {QStringLiteral("operation"), tools.at(i)},
                              {QStringLiteral("status"), QStringLiteral("queued")},
                              {QStringLiteral("phase"), QStringLiteral("waiting")},
                              {QStringLiteral("finished"), false},
                              {QStringLiteral("cancelRequested"), false},
                              {QStringLiteral("logTail"), QString(8192, QChar(u'中'))},
                              {QStringLiteral("plannedOutputs"), QJsonArray{QString(1200000, QLatin1Char('p'))}},
                              {QStringLiteral("segments"), QJsonArray{QJsonObject{{QStringLiteral("text"), QString(12000, QChar(u'中'))}}}}};
        const QJsonObject payload{{QStringLiteral("ok"), true},       {QStringLiteral("isError"), false}, {QStringLiteral("accepted"), true},
                                  {QStringLiteral("changed"), false}, {QStringLiteral("noop"), true},     {QStringLiteral("job"), job}};
        const auto bounded = McpWire::boundRpcResponse(&owner, wireRpc(McpWire::toolResult(payload), QString(128, QChar::Null)), tools.at(i), true);
        REQUIRE(wireActualBytes(bounded) <= McpWire::MaxResponseBytes);
        const auto result = bounded.value(QStringLiteral("result")).toObject();
        CHECK_FALSE(result.value(QStringLiteral("isError")).toBool());
        const auto summary = wireData(result);
        CHECK(summary.value(QStringLiteral("ok")).toBool());
        CHECK_FALSE(summary.value(QStringLiteral("isError")).toBool(true));
        CHECK(summary.value(QStringLiteral("accepted")).toBool());
        CHECK_FALSE(summary.value(QStringLiteral("changed")).toBool(true));
        CHECK(summary.value(QStringLiteral("noop")).toBool());
        CHECK_FALSE(summary.contains(QStringLiteral("committed")));
        const auto compact = summary.value(QStringLiteral("job")).toObject();
        for (const auto &key : {QStringLiteral("jobId"), QStringLiteral("kind"), QStringLiteral("operation"), QStringLiteral("status"), QStringLiteral("phase"),
                                QStringLiteral("finished"), QStringLiteral("cancelRequested")})
            CHECK(compact.value(key) == job.value(key));
        CHECK(compact.value(QStringLiteral("readbackTruncated")).toBool());
        CHECK_FALSE(compact.contains(QStringLiteral("logTail")));
        CHECK_FALSE(compact.contains(QStringLiteral("plannedOutputs")));
        CHECK_FALSE(compact.contains(QStringLiteral("segments")));
        CHECK(wireActualBytes(compact) <= 64 * 1024);
        const auto receipt = summary.value(QStringLiteral("receipt")).toObject().value(QStringLiteral("id")).toString();
        REQUIRE_FALSE(receipt.isEmpty());
        CHECK(wireReceiptPage(&owner, receipt, QStringLiteral("/job/logTail"), 8188, 64).value(QStringLiteral("text")).toString() == QString(4, QChar(u'中')));
        CHECK(McpWire::mutationPreflight(&owner, cancellations.at(i), true).isEmpty());
        auto cancellation = job;
        cancellation.insert(QStringLiteral("status"), i == 2 ? QStringLiteral("cancelled") : QStringLiteral("canceling"));
        cancellation.insert(QStringLiteral("phase"), QStringLiteral("stopping"));
        cancellation.insert(QStringLiteral("finished"), i == 2);
        cancellation.insert(QStringLiteral("cancelRequested"), true);
        const auto canceled = McpWire::boundRpcResponse(
            &owner, wireRpc(McpWire::toolResult({{QStringLiteral("ok"), true}, {QStringLiteral("changed"), false}, {QStringLiteral("job"), cancellation}})),
            cancellations.at(i), true);
        REQUIRE(wireActualBytes(canceled) <= McpWire::MaxResponseBytes);
        const auto stopped = wireData(canceled.value(QStringLiteral("result")).toObject());
        CHECK(stopped.value(QStringLiteral("ok")).toBool());
        CHECK_FALSE(stopped.value(QStringLiteral("changed")).toBool(true));
        const auto stoppedJob = stopped.value(QStringLiteral("job")).toObject();
        for (const auto &key :
             {QStringLiteral("jobId"), QStringLiteral("status"), QStringLiteral("phase"), QStringLiteral("finished"), QStringLiteral("cancelRequested")})
            CHECK(stoppedJob.value(key) == cancellation.value(key));
        CHECK(stopped.value(QStringLiteral("pendingResultReceipt")).toObject().value(QStringLiteral("id")).toString() == receipt);
        CHECK(wireReceiptPage(&owner, receipt, QStringLiteral("/job/status")).value(QStringLiteral("text")).toString() == QStringLiteral("queued"));
    }
    QObject failedOwner;
    const QString message = QString(400000, QChar(u'中')) + QString::fromUtf8("😀");
    const QJsonObject failedJob{
        {QStringLiteral("jobId"), QStringLiteral("failed-speech-job")},
        {QStringLiteral("status"), QStringLiteral("failed")},
        {QStringLiteral("finished"), true},
        {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("NATIVE_SPEECH_FAILED")}, {QStringLiteral("message"), message}}}};
    const auto failed = McpWire::boundRpcResponse(
        &failedOwner,
        wireRpc(McpWire::toolResult({{QStringLiteral("ok"), false}, {QStringLiteral("accepted"), false}, {QStringLiteral("job"), failedJob}}, true)),
        QStringLiteral("speech_job_status"), false);
    REQUIRE(wireActualBytes(failed) <= McpWire::MaxResponseBytes);
    const auto failedResult = failed.value(QStringLiteral("result")).toObject();
    CHECK(failedResult.value(QStringLiteral("isError")).toBool());
    const auto failedSummary = wireData(failedResult);
    CHECK_FALSE(failedSummary.value(QStringLiteral("ok")).toBool(true));
    CHECK_FALSE(failedSummary.value(QStringLiteral("accepted")).toBool(true));
    const auto error = failedSummary.value(QStringLiteral("job")).toObject().value(QStringLiteral("error")).toObject();
    CHECK(error.value(QStringLiteral("code")).toString() == QStringLiteral("NATIVE_SPEECH_FAILED"));
    CHECK(error.value(QStringLiteral("messageTruncated")).toBool());
    CHECK(error.value(QStringLiteral("message")).toString() == message.left(2048));
}

TEST_CASE("MCP JSON views retain escaped pointers values and surrogate boundaries", "[MCP][MCPWire][pages]")
{
    const QString text = QString::fromUtf8("A😀B😀C");
    const QJsonValue value = QJsonObject{{QStringLiteral("a/b~c"), QJsonArray{QJsonObject{{QStringLiteral("text"), text}}, true, QJsonValue::Null}}};
    auto page = McpWire::pageValue(value, QStringLiteral("/a~1b~0c/0/text"), 1, 1);
    REQUIRE(page.value(QStringLiteral("ok")).toBool());
    CHECK(page.value(QStringLiteral("text")).toString() == QString::fromUtf8("😀"));
    CHECK(page.value(QStringLiteral("nextOffset")).toInt() == 3);
    CHECK_FALSE(McpWire::pageValue(value, QStringLiteral("/a~1b~0c/0/text"), 2, 1).value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(McpWire::pageValue(value, QStringLiteral("/a~2b"), 0, 10).value(QStringLiteral("ok")).toBool());
    const auto rejectedPath = McpWire::pageValue(value, QLatin1Char('/') + QString(8193, QChar(u'中')), 0, 10);
    CHECK_FALSE(rejectedPath.value(QStringLiteral("ok")).toBool());
    CHECK(QJsonDocument(rejectedPath).toJson(QJsonDocument::Compact).size() < 1024);
    CHECK_FALSE(McpWire::pageValue(value, QStringLiteral("/a~1b~0c/01"), 0, 10).value(QStringLiteral("ok")).toBool());
    page = McpWire::pageValue(value, QStringLiteral("/a~1b~0c"), 1, 128);
    REQUIRE(page.value(QStringLiteral("ok")).toBool());
    const auto entries = page.value(QStringLiteral("entries")).toArray();
    REQUIRE(entries.size() == 2);
    CHECK(entries.at(0).toObject().value(QStringLiteral("value")) == QJsonValue(true));
    CHECK(entries.at(1).toObject().value(QStringLiteral("value")).isNull());
    QJsonArray large;
    for (int i = 0; i < 300; ++i)
        large.append(QJsonObject{{QStringLiteral("id"), i}, {QStringLiteral("text"), QString(8192, QChar(u'中'))}});
    int offset = 0, seen = 0;
    while (offset < large.size()) {
        page = McpWire::pageValue(large, {}, offset, 128, 4096);
        REQUIRE(page.value(QStringLiteral("ok")).toBool());
        CHECK(QJsonDocument(page).toJson(QJsonDocument::Compact).size() <= 4096);
        const auto rows = page.value(QStringLiteral("entries")).toArray();
        REQUIRE_FALSE(rows.isEmpty());
        for (const auto &row : rows) {
            CHECK(row.toObject().value(QStringLiteral("valueOmitted")).toBool());
            ++seen;
        }
        const int next = page.value(QStringLiteral("nextOffset")).toInt();
        REQUIRE(next > offset);
        offset = next;
    }
    CHECK(seen == large.size());
    CHECK(McpWire::pageValue(large, QStringLiteral("/299/id"), 0, 1).value(QStringLiteral("value")).toInt() == 299);
    const QString longKey(400, QLatin1Char('x'));
    const auto scalarPage = McpWire::pageValue(QJsonObject{{longKey, 42}}, QLatin1Char('/') + longKey, 0, 1, 256);
    CHECK_FALSE(scalarPage.value(QStringLiteral("ok")).toBool());
    CHECK(scalarPage.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString() == QStringLiteral("PAGE_ITEM_TOO_LARGE"));
    CHECK(wireActualBytes(scalarPage) <= 256);
}

TEST_CASE("MCP receipt preflight prevents a second real native edit before explicit release", "[MCP][MCPWire][integration]")
{
    McpProject project;
    project.open();
    const auto first = project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 5));
    // Seed only the delivery boundary with an oversized diagnostic payload. Both
    // edits and the second command's dispatch/undo outcome are real native calls.
    auto captured = first;
    captured.insert(QStringLiteral("largeReadback"), QString(1200000, QLatin1Char('x')));
    const auto id = wireCapture(&project.server, captured, QStringLiteral("add_color_clip"));
    const int beforeCount = project.timeline->getClipsCount();
    const int beforeHistory = project.undoStack->index();
    const auto blocked = project.call(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 10, 5));
    REQUIRE(blocked.value(QStringLiteral("isError")).toBool());
    CHECK(wireData(blocked).value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString() == QStringLiteral("RESULT_RECEIPT_PENDING"));
    CHECK(project.timeline->getClipsCount() == beforeCount);
    CHECK(project.undoStack->index() == beforeHistory);
    const auto original =
        project.success(QStringLiteral("get_result_page"), {{QStringLiteral("receiptId"), id}, {QStringLiteral("path"), QStringLiteral("/clipId")}});
    CHECK(original.value(QStringLiteral("value")).toInt() == first.value(QStringLiteral("clipId")).toInt());
    project.success(QStringLiteral("release_result_receipt"), {{QStringLiteral("receiptId"), id}, {QStringLiteral("acknowledgeRelease"), true}});
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 10, 5));
    CHECK(project.timeline->getClipsCount() == beforeCount + 1);
    CHECK(project.undoStack->index() == beforeHistory + 1);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getClipsCount() == beforeCount);
}

TEST_CASE("MCP retained delivery never prevents cancellation of its real owned renderer", "[MCP][MCPWire][render][cancel]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 10000));
    QString presetName;
    for (const auto &name : RenderPresetRepository::get()->getAllPresets()) {
        const auto &preset = RenderPresetRepository::get()->getPreset(name);
        if (preset && preset->isValid() && preset->renderer() == QStringLiteral("avformat") && preset->extension() == QStringLiteral("mp4") &&
            !preset->params().isImageSequence() && preset->params().value(QStringLiteral("vcodec")) == QStringLiteral("libx264")) {
            presetName = name;
            break;
        }
    }
    REQUIRE_FALSE(presetName.isEmpty());
    struct WireRestoreMelt
    {
        QString previous{KdenliveSettings::meltpath()};
        ~WireRestoreMelt() { KdenliveSettings::setMeltpath(previous); }
    } restore;
#ifdef Q_OS_WIN
    const QString filename = QStringLiteral("melt.exe");
#else
    const QString filename = QStringLiteral("melt");
#endif
    QString melt = QDir(QCoreApplication::applicationDirPath()).filePath(filename);
    if (!QFileInfo::exists(melt)) melt = QStandardPaths::findExecutable(filename);
    REQUIRE(QFileInfo::exists(melt));
    KdenliveSettings::setMeltpath(melt);
    const auto output = directory.filePath(QStringLiteral("owned-canceled.mp4"));
    auto start = project.expected();
    start.insert(QStringLiteral("path"), output);
    start.insert(QStringLiteral("presetName"), presetName);
    const auto accepted = project.success(QStringLiteral("start_render"), start);
    const auto job = accepted.value(QStringLiteral("job")).toObject();
    const auto jobId = job.value(QStringLiteral("jobId")).toString();
    REQUIRE_FALSE(jobId.isEmpty());
    auto captured = accepted;
    captured.insert(QStringLiteral("largeReadback"), QString(1200000, QLatin1Char('x')));
    const auto receipt = wireCapture(&project.server, captured, QStringLiteral("start_render"));
    const int history = project.undoStack->index();
    auto stop = project.expected();
    stop.insert(QStringLiteral("jobId"), jobId);
    const auto stopping = project.success(QStringLiteral("cancel_render"), stop).value(QStringLiteral("job")).toObject();
    CHECK(stopping.value(QStringLiteral("jobId")).toString() == jobId);
    const auto status = stopping.value(QStringLiteral("status")).toString();
    CHECK((status == QStringLiteral("canceling") || status == QStringLiteral("canceled")));
    CHECK(project.undoStack->index() == history);
    QElapsedTimer deadline;
    deadline.start();
    QJsonObject terminal;
    while (deadline.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        terminal = project.success(QStringLiteral("render_status"), {{QStringLiteral("jobId"), jobId}}).value(QStringLiteral("job")).toObject();
        if (terminal.value(QStringLiteral("status")).toString() == QStringLiteral("canceled")) break;
        REQUIRE(terminal.value(QStringLiteral("status")).toString() != QStringLiteral("completed"));
        QThread::msleep(1);
    }
    CHECK(terminal.value(QStringLiteral("status")).toString() == QStringLiteral("canceled"));
    CHECK_FALSE(QFileInfo::exists(output));
    const auto originalStatus =
        project.success(QStringLiteral("get_result_page"), {{QStringLiteral("receiptId"), receipt}, {QStringLiteral("path"), QStringLiteral("/job/status")}});
    CHECK(originalStatus.value(QStringLiteral("text")).toString() == job.value(QStringLiteral("status")).toString());
    project.success(QStringLiteral("release_result_receipt"), {{QStringLiteral("receiptId"), receipt}, {QStringLiteral("acknowledgeRelease"), true}});
}
