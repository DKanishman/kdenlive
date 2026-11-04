/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "mainwindow.h"
#include "mcphelpers.h"
#include "mcpmonitor.h"
#include "mcpquick.h"
#include "mcpserver.h"
#include "widgets/dragvalue.h"
#include <KActionCollection>
#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRadioButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <QUuid>
#include <QValidator>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace {
QString uiDigest(const QJsonObject &value)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
QList<QAction *> editorActions()
{
    if (!pCore || !pCore->window() || !pCore->window()->actionCollection()) return {};
    auto actions = pCore->window()->actionCollection()->actions();
    std::sort(actions.begin(), actions.end(), [](const QAction *a, const QAction *b) { return a->objectName() < b->objectName(); });
    return actions;
}
QJsonObject actionData(QAction *action)
{
    return {{QStringLiteral("id"), action->objectName()},
            {QStringLiteral("text"), action->text()},
            {QStringLiteral("enabled"), action->isEnabled()},
            {QStringLiteral("visible"), action->isVisible()},
            {QStringLiteral("checkable"), action->isCheckable()},
            {QStringLiteral("checked"), action->isChecked()},
            {QStringLiteral("tooltip"), action->toolTip()},
            {QStringLiteral("coverage"), QStringLiteral("interactive-action; completion depends on context and dialogs")}};
}
QJsonArray indexPath(QModelIndex index)
{
    QList<int> rows;
    while (index.isValid()) {
        rows.prepend(index.row());
        index = index.parent();
    }
    QJsonArray path;
    for (int row : rows)
        path.append(row);
    return path;
}
bool modelIndex(QAbstractItemModel *model, const QJsonValue &value, QModelIndex &index)
{
    if (!model || !value.isArray() || value.toArray().size() > 16) return false;
    index = QModelIndex();
    for (const auto &part : value.toArray()) {
        if (!part.isDouble() || part.toDouble() < 0 || std::floor(part.toDouble()) != part.toDouble() || part.toDouble() >= model->rowCount(index))
            return false;
        index = model->index(part.toInt(), 0, index);
        if (!index.isValid()) return false;
    }
    return true;
}
bool allowedLineValue(QLineEdit *input, const QJsonValue &value, bool commit)
{
    if (!input || !value.isString() || value.toString().size() > 16384 || value.toString().size() > input->maxLength() || input->isReadOnly() ||
        input->echoMode() != QLineEdit::Normal)
        return false;
    // Validate in a detached input, without touching the live control or emitting any editor signals.
    QLineEdit probe;
    probe.setMaxLength(input->maxLength());
    probe.setInputMask(input->inputMask());
    probe.setValidator(input->validator());
    probe.setText(value.toString());
    return probe.text() == value.toString() && (!commit || probe.hasAcceptableInput());
}
} // namespace
QJsonArray KdenliveMcpServer::uiToolCatalog()
{
    using namespace Mcp;
    auto text = [](const char *d) { return Mcp::property(QStringLiteral("string"), QString::fromUtf8(d)); };
    auto array = [](const char *d) {
        auto p = Mcp::property(QStringLiteral("array"), QString::fromUtf8(d));
        p.insert(QStringLiteral("items"), QJsonObject{});
        return p;
    };
    const auto ui = text("Exact uiToken from the latest get_ui. This guards the current widgets/actions, including dialogs.");
    return {
        tool(QStringLiteral("get_capabilities"),
             QStringLiteral("Report actual tool coverage and the distinction between direct native editing and interactive action reachability."), {}, {},
             true),
        tool(QStringLiteral("list_actions"),
             QStringLiteral(
                 "Discover registered native editor actions and their actual enabled/checkable state. Listing an action does not prove autonomous completion."),
             {{QStringLiteral("query"), text("Optional case-insensitive name/text filter.")}}, {}, true),
        tool(QStringLiteral("get_ui"),
             QStringLiteral("Inspect this Kdenlive instance's visible Qt dialogs and controls; obtain a token for native control operations."), {}, {}, true),
        tool(QStringLiteral("ui_job_status"), QStringLiteral("Query an accepted native UI operation; modal dialogs are explicitly reported as needsUserInput."),
             {{QStringLiteral("jobId"), text("UI job ID.")}}, {QStringLiteral("jobId")}, true),
        tool(QStringLiteral("trigger_action"),
             QStringLiteral("Queue one enabled editor QAction by its discovered ID. Returns accepted, not completed; inspect job and UI for dialogs/results."),
             {{QStringLiteral("expectedUi"), ui}, {QStringLiteral("actionId"), text("Exact ID from list_actions.")}},
             {QStringLiteral("expectedUi"), QStringLiteral("actionId")}),
        tool(QStringLiteral("set_ui_value"),
             QStringLiteral("Set a discovered enabled native Qt input using its normal typed setter. The queued result includes native readback."),
             {{QStringLiteral("expectedUi"), ui},
              {QStringLiteral("controlId"), text("ID from get_ui.")},
              {QStringLiteral("value"), QJsonObject{}},
              {QStringLiteral("commit"), Mcp::property(QStringLiteral("boolean"), QStringLiteral("Emit editingFinished for line/spin inputs, default true."))}},
             {QStringLiteral("expectedUi"), QStringLiteral("controlId"), QStringLiteral("value")}),
        tool(
            QStringLiteral("activate_ui_control"),
            QStringLiteral("Queue a discovered native button click, item-view activation, focus, or dialog rejection. Does not simulate mouse/keyboard input."),
            {{QStringLiteral("expectedUi"), ui},
             {QStringLiteral("controlId"), text("ID from get_ui.")},
             {QStringLiteral("operation"), text("click, activate, focus, or reject_dialog.")},
             {QStringLiteral("path"), array("For item-view activate: row index path.")},
             {QStringLiteral("expectedModel"), text("Required modelToken when activating an item-view row.")}},
            {QStringLiteral("expectedUi"), QStringLiteral("controlId"), QStringLiteral("operation")}),
        tool(QStringLiteral("get_ui_model"),
             QStringLiteral("Page native model rows for a discovered list/tree/table view; retrieve stable row paths for selection."),
             {{QStringLiteral("controlId"), text("Item-view ID from get_ui.")},
              {QStringLiteral("parentPath"), array("Optional parent row path.")},
              {QStringLiteral("offset"), Mcp::property(QStringLiteral("integer"), QStringLiteral("First row, default zero."))},
              {QStringLiteral("limit"), Mcp::property(QStringLiteral("integer"), QStringLiteral("Page size 1-100, default 50."))}},
             {QStringLiteral("controlId")}, true),
        tool(QStringLiteral("select_ui_rows"), QStringLiteral("Set a discovered native item-view selection from explicit model row paths."),
             {{QStringLiteral("expectedUi"), ui},
              {QStringLiteral("controlId"), text("Item-view ID.")},
              {QStringLiteral("paths"), array("Array of row paths, at most 128 selections.")},
              {QStringLiteral("expectedModel"), text("modelToken from the inspected get_ui_model page.")}},
             {QStringLiteral("expectedUi"), QStringLiteral("controlId"), QStringLiteral("paths"), QStringLiteral("expectedModel")})};
}
QString KdenliveMcpServer::uiModelToken(QAbstractItemModel *model)
{
    if (!model) return {};
    if (!m_uiModelIds.contains(model)) {
        m_uiModelIds.insert(model, QStringLiteral("model-%1").arg(++m_uiControlCounter));
        m_uiModelRevisions.insert(model, 0);
        auto changed = [this, model] { ++m_uiModelRevisions[model]; };
        connect(model, &QAbstractItemModel::dataChanged, this, changed);
        connect(model, &QAbstractItemModel::modelReset, this, changed);
        connect(model, &QAbstractItemModel::layoutChanged, this, changed);
        connect(model, &QAbstractItemModel::rowsInserted, this, changed);
        connect(model, &QAbstractItemModel::rowsRemoved, this, changed);
        connect(model, &QAbstractItemModel::rowsMoved, this, changed);
        connect(model, &QAbstractItemModel::columnsInserted, this, changed);
        connect(model, &QAbstractItemModel::columnsRemoved, this, changed);
        connect(model, &QAbstractItemModel::columnsMoved, this, changed);
        connect(model, &QObject::destroyed, this, [this, model] {
            m_uiModelIds.remove(model);
            m_uiModelRevisions.remove(model);
        });
    }
    return m_uiModelIds.value(model) + QLatin1Char('-') + QString::number(m_uiModelRevisions.value(model));
}
QJsonObject KdenliveMcpServer::uiSnapshot()
{
    for (auto it = m_uiControls.begin(); it != m_uiControls.end();) {
        if (it.value().isNull())
            it = m_uiControls.erase(it);
        else
            ++it;
    }
    auto idFor = [this](QWidget *widget) {
        for (auto it = m_uiControls.cbegin(); it != m_uiControls.cend(); ++it)
            if (it.value() == widget) return it.key();
        const QString id = QStringLiteral("ui-%1").arg(++m_uiControlCounter);
        m_uiControls.insert(id, widget);
        return id;
    };
    QJsonArray controls;
    auto widgets = QApplication::allWidgets();
    for (QWidget *widget : widgets) {
        if (!widget->isVisible() || controls.size() >= 2048) continue;
        if (!qobject_cast<QAbstractButton *>(widget) && !qobject_cast<QLineEdit *>(widget) && !qobject_cast<QTextEdit *>(widget) &&
            !qobject_cast<QPlainTextEdit *>(widget) && !qobject_cast<QSpinBox *>(widget) && !qobject_cast<QDoubleSpinBox *>(widget) &&
            !qobject_cast<QComboBox *>(widget) && !qobject_cast<QAbstractSlider *>(widget) && !qobject_cast<QAbstractItemView *>(widget) &&
            !qobject_cast<QLabel *>(widget) && !qobject_cast<QDialog *>(widget) && !qobject_cast<QGroupBox *>(widget) && !qobject_cast<DragValue *>(widget))
            continue;
        QJsonObject data{{QStringLiteral("id"), idFor(widget)},
                         {QStringLiteral("objectName"), widget->objectName()},
                         {QStringLiteral("class"), QString::fromLatin1(widget->metaObject()->className())},
                         {QStringLiteral("enabled"), widget->isEnabled()},
                         {QStringLiteral("tooltip"), widget->toolTip().left(1024)},
                         {QStringLiteral("accessibleName"), widget->accessibleName()},
                         {QStringLiteral("windowId"), idFor(widget->window())},
                         {QStringLiteral("windowTitle"), widget->window()->windowTitle()}};
        bool writable = false;
        if (auto *w = qobject_cast<DragValue *>(widget)) {
            data.insert(QStringLiteral("value"), w->value());
            data.insert(QStringLiteral("minimum"), w->minimum());
            data.insert(QStringLiteral("maximum"), w->maximum());
            data.insert(QStringLiteral("decimals"), w->precision());
            writable = true;
        } else if (auto *w = qobject_cast<QLineEdit *>(widget)) {
            const bool secret = w->echoMode() != QLineEdit::Normal;
            data.insert(QStringLiteral("value"), secret ? QStringLiteral("[redacted]") : w->text().left(16384));
            if (!secret) {
                data.insert(QStringLiteral("textHash"), uiDigest({{QStringLiteral("text"), w->text()}}));
                data.insert(QStringLiteral("cursorPosition"), w->cursorPosition());
                data.insert(QStringLiteral("selectionStart"), w->selectionStart());
                data.insert(QStringLiteral("selectionLength"), w->selectionLength());
            }
            writable = !w->isReadOnly() && !secret;
            data.insert(QStringLiteral("maxLength"), w->maxLength());
            data.insert(QStringLiteral("inputMask"), w->inputMask());
            data.insert(QStringLiteral("validator"), w->validator() ? QString::fromLatin1(w->validator()->metaObject()->className()) : QString());
        } else if (auto *w = qobject_cast<QPlainTextEdit *>(widget)) {
            data.insert(QStringLiteral("value"), w->toPlainText().left(16384));
            data.insert(QStringLiteral("textHash"), uiDigest({{QStringLiteral("text"), w->toPlainText()}}));
            data.insert(QStringLiteral("cursorPosition"), w->textCursor().position());
            data.insert(QStringLiteral("cursorAnchor"), w->textCursor().anchor());
            writable = !w->isReadOnly();
        } else if (auto *w = qobject_cast<QTextEdit *>(widget)) {
            data.insert(QStringLiteral("value"), w->toPlainText().left(16384));
            data.insert(QStringLiteral("richTextHash"), uiDigest({{QStringLiteral("html"), w->toHtml()}}));
            data.insert(QStringLiteral("cursorPosition"), w->textCursor().position());
            data.insert(QStringLiteral("cursorAnchor"), w->textCursor().anchor());
            writable = !w->isReadOnly();
        } else if (auto *w = qobject_cast<QSpinBox *>(widget)) {
            data.insert(QStringLiteral("value"), w->value());
            data.insert(QStringLiteral("minimum"), w->minimum());
            data.insert(QStringLiteral("maximum"), w->maximum());
            writable = !w->isReadOnly();
        } else if (auto *w = qobject_cast<QDoubleSpinBox *>(widget)) {
            data.insert(QStringLiteral("value"), w->value());
            data.insert(QStringLiteral("minimum"), w->minimum());
            data.insert(QStringLiteral("maximum"), w->maximum());
            writable = !w->isReadOnly();
        } else if (auto *w = qobject_cast<QComboBox *>(widget)) {
            QJsonArray entries;
            for (int i = 0; i < std::min(w->count(), 256); ++i)
                entries.append(QJsonObject{{QStringLiteral("index"), i}, {QStringLiteral("text"), w->itemText(i)}});
            data.insert(QStringLiteral("choices"), entries);
            data.insert(QStringLiteral("value"), w->currentIndex());
            data.insert(QStringLiteral("text"), w->currentText());
            data.insert(QStringLiteral("editable"), w->isEditable());
            writable = true;
        } else if (auto *w = qobject_cast<QGroupBox *>(widget)) {
            data.insert(QStringLiteral("text"), w->title());
            data.insert(QStringLiteral("value"), w->isChecked());
            data.insert(QStringLiteral("checkable"), w->isCheckable());
            writable = w->isCheckable();
        } else if (auto *w = qobject_cast<QAbstractButton *>(widget)) {
            data.insert(QStringLiteral("text"), w->text());
            data.insert(QStringLiteral("checkable"), w->isCheckable());
            data.insert(QStringLiteral("value"), w->isChecked());
            writable = w->isCheckable();
        } else if (auto *w = qobject_cast<QAbstractSlider *>(widget)) {
            data.insert(QStringLiteral("value"), w->value());
            data.insert(QStringLiteral("minimum"), w->minimum());
            data.insert(QStringLiteral("maximum"), w->maximum());
            writable = !qobject_cast<CustomLabel *>(w);
            if (!writable && qobject_cast<DragValue *>(w->parentWidget())) data.insert(QStringLiteral("valueControlId"), idFor(w->parentWidget()));
        } else if (auto *w = qobject_cast<QAbstractItemView *>(widget)) {
            data.insert(QStringLiteral("rows"), w->model() ? w->model()->rowCount() : 0);
            data.insert(QStringLiteral("modelToken"), uiModelToken(w->model()));
            QJsonArray selected;
            if (w->selectionModel())
                for (const auto &index : w->selectionModel()->selectedRows())
                    if (selected.size() < 128) selected.append(indexPath(index));
            data.insert(QStringLiteral("selectedPaths"), selected);
        } else if (auto *w = qobject_cast<QLabel *>(widget)) {
            data.insert(QStringLiteral("text"), w->text().left(16384));
            if (w->buddy()) data.insert(QStringLiteral("buddyId"), idFor(w->buddy()));
        }
        if (auto *dialog = qobject_cast<QDialog *>(widget)) {
            data.insert(QStringLiteral("modal"), dialog->isModal());
            data.insert(QStringLiteral("dialog"), true);
        }
        data.insert(QStringLiteral("writable"), writable);
        controls.append(data);
    }
    QVector<QJsonObject> orderedControls;
    orderedControls.reserve(controls.size());
    for (const auto &control : controls)
        orderedControls.append(control.toObject());
    std::sort(orderedControls.begin(), orderedControls.end(),
              [](const QJsonObject &a, const QJsonObject &b) { return a.value(QStringLiteral("id")).toString() < b.value(QStringLiteral("id")).toString(); });
    controls = QJsonArray();
    for (const auto &control : orderedControls)
        controls.append(control);
    QJsonArray actions;
    for (auto *action : editorActions())
        actions.append(actionData(action));
    auto *modal = QApplication::activeModalWidget();
    QJsonObject result{{QStringLiteral("sessionId"), m_sessionId},
                       {QStringLiteral("controls"), controls},
                       {QStringLiteral("actions"), actions},
                       {QStringLiteral("needsUserInput"), modal != nullptr},
                       {QStringLiteral("modalId"), modal ? idFor(modal) : QString()},
                       {QStringLiteral("truncated"), controls.size() >= 2048}};
    const auto native = snapshot();
    result.insert(QStringLiteral("nativeStateToken"), native.value(QStringLiteral("expectedState")));
    result.insert(QStringLiteral("nativeReady"), native.value(QStringLiteral("ready")));
    result.insert(QStringLiteral("nativeDocumentId"), pCore && pCore->currentDoc() ? pCore->currentDoc()->uuid().toString() : QString());
    result.insert(QStringLiteral("focusId"), QApplication::focusWidget() ? idFor(QApplication::focusWidget()) : QString());
    result.insert(QStringLiteral("activeWindowId"), QApplication::activeWindow() ? idFor(QApplication::activeWindow()) : QString());
    result.insert(QStringLiteral("monitorFingerprint"), McpMonitor::interactionFingerprint(this));
    result.insert(QStringLiteral("quickFingerprint"), McpQuick::interactionFingerprint(this));
    result.insert(QStringLiteral("uiToken"), uiDigest(result));
    return result;
}
QJsonObject KdenliveMcpServer::callUiTool(const QString &name, const QJsonObject &args, const QJsonObject &state)
{
    using namespace Mcp;
    if (name == QLatin1String("get_capabilities")) {
        QJsonArray direct, interactive;
        QStringList interactiveNames;
        for (const auto &catalog : {uiToolCatalog(), McpQuick::toolCatalog()})
            for (const auto &entry : catalog)
                interactiveNames.append(entry.toObject().value(QStringLiteral("name")).toString());
        for (const auto &entry : toolCatalog()) {
            const auto name = entry.toObject().value(QStringLiteral("name")).toString();
            if (interactiveNames.contains(name))
                interactive.append(name);
            else
                direct.append(name);
        }
        return toolResult(
            {{QStringLiteral("ok"), true},
             {QStringLiteral("version"), QStringLiteral("0.4.0")},
             {QStringLiteral("nativeTools"), direct},
             {QStringLiteral("interactiveTools"), interactive},
             {QStringLiteral("interactiveBridge"), QStringLiteral("Native registered actions, typed Qt widgets, QtQuick controls and menus. Actions may open "
                                                                  "dialogs or require external dependencies; query actual job outcomes.")},
             {QStringLiteral("completeEditorCoverageVerified"), false},
             {QStringLiteral("coveragePolicy"),
              QStringLiteral(
                  "Discovery, accepted jobs, completed operations, and independently verified results are distinct. No every-function claim is made.")}});
    }
    if (name == QLatin1String("list_actions")) {
        if (args.contains(QStringLiteral("query")) && !args.value(QStringLiteral("query")).isString())
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("query must be text."));
        QJsonArray entries;
        const QString query = args.value(QStringLiteral("query")).toString();
        for (auto *action : editorActions())
            if (query.isEmpty() || action->objectName().contains(query, Qt::CaseInsensitive) || action->text().contains(query, Qt::CaseInsensitive))
                entries.append(actionData(action));
        return toolResult({{QStringLiteral("ok"), true}, {QStringLiteral("actions"), entries}});
    }
    if (name == QLatin1String("get_ui")) return toolResult({{QStringLiteral("ok"), true}, {QStringLiteral("ui"), uiSnapshot()}});
    if (name == QLatin1String("ui_job_status")) {
        const QString id = args.value(QStringLiteral("jobId")).toString();
        if (!m_uiJobs.contains(id)) return failure(QStringLiteral("UNKNOWN_JOB"), QStringLiteral("Unknown UI job ID."));
        auto job = m_uiJobs.value(id);
        job.insert(QStringLiteral("needsUserInput"), QApplication::activeModalWidget() != nullptr);
        return toolResult({{QStringLiteral("ok"), true}, {QStringLiteral("job"), job}, {QStringLiteral("state"), snapshot()}});
    }
    if (name == QLatin1String("get_ui_model")) {
        const QString id = args.value(QStringLiteral("controlId")).toString();
        auto *view = qobject_cast<QAbstractItemView *>(m_uiControls.value(id).data());
        if (!view || !view->isVisible() || !view->model())
            return failure(QStringLiteral("INVALID_CONTROL"), QStringLiteral("Inspect a visible item view first."));
        int offset = 0, limit = 50;
        if ((args.contains(QStringLiteral("offset")) && !integer(args, QStringLiteral("offset"), 0, 10000000, offset)) ||
            (args.contains(QStringLiteral("limit")) && !integer(args, QStringLiteral("limit"), 1, 100, limit)))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Invalid pagination."));
        QModelIndex parent;
        if (args.contains(QStringLiteral("parentPath")) && !modelIndex(view->model(), args.value(QStringLiteral("parentPath")), parent))
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Invalid parent path."));
        QJsonArray rows;
        const int total = view->model()->rowCount(parent);
        for (int row = offset; row < std::min(total, offset + limit); ++row) {
            QJsonArray columns;
            for (int column = 0; column < std::min(view->model()->columnCount(parent), 16); ++column)
                columns.append(view->model()->data(view->model()->index(row, column, parent), Qt::DisplayRole).toString().left(4096));
            const auto index = view->model()->index(row, 0, parent);
            rows.append(QJsonObject{{QStringLiteral("path"), indexPath(index)},
                                    {QStringLiteral("columns"), columns},
                                    {QStringLiteral("children"), view->model()->rowCount(index)},
                                    {QStringLiteral("enabled"), bool(index.flags() & Qt::ItemIsEnabled)}});
        }
        return toolResult({{QStringLiteral("ok"), true},
                           {QStringLiteral("rows"), rows},
                           {QStringLiteral("totalRows"), total},
                           {QStringLiteral("nextOffset"), offset + rows.size()},
                           {QStringLiteral("modelToken"), uiModelToken(view->model())},
                           {QStringLiteral("uiToken"), uiSnapshot().value(QStringLiteral("uiToken"))}});
    }
    const bool trigger = name == QLatin1String("trigger_action"), setter = name == QLatin1String("set_ui_value"),
               activate = name == QLatin1String("activate_ui_control"), select = name == QLatin1String("select_ui_rows");
    if (!trigger && !setter && !activate && !select) return {};
    const auto ui = uiSnapshot();
    const QString expected = args.value(QStringLiteral("expectedUi")).toString();
    if (expected.isEmpty() || expected != ui.value(QStringLiteral("uiToken")).toString())
        return failure(QStringLiteral("STALE_UI"), QStringLiteral("Read get_ui again before operating controls."), state);
    if (QApplication::mouseButtons() != Qt::NoButton) return failure(QStringLiteral("UI_BUSY"), QStringLiteral("Finish dragging first."), state);
    if (McpQuick::modalPopupActive(this))
        return failure(QStringLiteral("UI_BUSY"), QStringLiteral("Respond to the active QtQuick popup through get_quick_ui first."), state);
    if (m_uiJobs.size() >= 64) {
        QStringList finished;
        for (auto it = m_uiJobs.cbegin(); it != m_uiJobs.cend(); ++it)
            if (it.value().value(QStringLiteral("status")) != QLatin1String("running") &&
                it.value().value(QStringLiteral("status")) != QLatin1String("pending"))
                finished.append(it.key());
        for (const auto &id : finished)
            m_uiJobs.remove(id);
        if (m_uiJobs.size() >= 64) return failure(QStringLiteral("LIMIT"), QStringLiteral("Too many pending UI jobs."), state);
    }
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QPointer<QWidget> control;
    QPointer<QAction> action;
    if (trigger) {
        const QString id = args.value(QStringLiteral("actionId")).toString();
        for (auto *candidate : editorActions())
            if (candidate->objectName() == id) {
                action = candidate;
                break;
            }
        if (!action || !action->isEnabled()) return failure(QStringLiteral("INVALID_ACTION"), QStringLiteral("Choose an enabled discovered action."), state);
    } else {
        control = m_uiControls.value(args.value(QStringLiteral("controlId")).toString());
        if (!control || !control->isVisible() || !control->isEnabled())
            return failure(QStringLiteral("INVALID_CONTROL"), QStringLiteral("Choose a visible enabled discovered control."), state);
        if (auto *modal = QApplication::activeModalWidget())
            if (control->window() != modal && control != modal)
                return failure(QStringLiteral("MODAL_SCOPE"), QStringLiteral("Operate the active modal dialog first."), state);
    }
    if (trigger && QApplication::activeModalWidget())
        return failure(QStringLiteral("UI_BUSY"), QStringLiteral("Respond to the active modal dialog first."), state);
    // Validate all value types/ranges before queuing; arbitrary QObject properties or method names are never accepted.
    if (setter) {
        const auto v = args.value(QStringLiteral("value"));
        bool valid = false;
        if (auto *w = qobject_cast<DragValue *>(control.data())) {
            double n;
            valid = real(args, QStringLiteral("value"), w->minimum(), w->maximum(), n) && (w->precision() != 0 || std::floor(n) == n);
        } else if (auto *w = qobject_cast<QLineEdit *>(control.data()))
            valid = allowedLineValue(w, v, args.value(QStringLiteral("commit")).toBool(true));
        else if (auto *w = qobject_cast<QPlainTextEdit *>(control.data()))
            valid = v.isString() && v.toString().size() <= 16384 && !w->isReadOnly();
        else if (auto *w = qobject_cast<QTextEdit *>(control.data()))
            valid = v.isString() && v.toString().size() <= 16384 && !w->isReadOnly();
        else if (auto *w = qobject_cast<QSpinBox *>(control.data())) {
            int n;
            valid = !w->isReadOnly() && integer(args, QStringLiteral("value"), w->minimum(), w->maximum(), n);
        } else if (auto *w = qobject_cast<QDoubleSpinBox *>(control.data())) {
            double n;
            valid = !w->isReadOnly() && real(args, QStringLiteral("value"), w->minimum(), w->maximum(), n);
        } else if (auto *w = qobject_cast<QComboBox *>(control.data())) {
            int n;
            valid = (v.isString() && v.toString().size() <= 16384 && w->isEditable()) || integer(args, QStringLiteral("value"), 0, w->count() - 1, n);
        } else if (auto *w = qobject_cast<QAbstractButton *>(control.data()))
            valid = w->isCheckable() && v.isBool();
        else if (auto *w = qobject_cast<QGroupBox *>(control.data()))
            valid = w->isCheckable() && v.isBool();
        else if (auto *w = qobject_cast<QAbstractSlider *>(control.data())) {
            int n;
            valid = !qobject_cast<CustomLabel *>(w) && integer(args, QStringLiteral("value"), w->minimum(), w->maximum(), n);
        }
        if (!valid || (args.contains(QStringLiteral("commit")) && !args.value(QStringLiteral("commit")).isBool()))
            return failure(QStringLiteral("INVALID_VALUE"), QStringLiteral("Value must match the discovered editable control and its native range."), state);
    }
    if (activate) {
        const QString op = args.value(QStringLiteral("operation")).toString();
        if (op != QLatin1String("focus") && !(op == QLatin1String("click") && qobject_cast<QAbstractButton *>(control.data())) &&
            !(op == QLatin1String("reject_dialog") && qobject_cast<QDialog *>(control.data())) &&
            !(op == QLatin1String("activate") && qobject_cast<QAbstractItemView *>(control.data())))
            return failure(QStringLiteral("INVALID_OPERATION"), QStringLiteral("Operation does not match this native control."), state);
        if (op == QLatin1String("activate")) {
            auto *view = qobject_cast<QAbstractItemView *>(control.data());
            QModelIndex index;
            if (!modelIndex(view->model(), args.value(QStringLiteral("path")), index) || !index.isValid())
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Use an existing row path."), state);
        }
    }
    if (select || (activate && args.value(QStringLiteral("operation")) == QLatin1String("activate"))) {
        auto *view = qobject_cast<QAbstractItemView *>(control.data());
        if (!view || args.value(QStringLiteral("expectedModel")).toString().isEmpty() ||
            args.value(QStringLiteral("expectedModel")).toString() != uiModelToken(view->model()))
            return failure(QStringLiteral("STALE_MODEL"), QStringLiteral("Read the current row page and modelToken before selecting/activating rows."), state);
    }
    if (select) {
        auto *view = qobject_cast<QAbstractItemView *>(control.data());
        const auto paths = args.value(QStringLiteral("paths"));
        if (!view || !view->selectionModel() || !paths.isArray() || paths.toArray().size() > 128)
            return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Select at most 128 paths in an item view."), state);
        for (const auto &path : paths.toArray()) {
            QModelIndex index;
            if (!modelIndex(view->model(), path, index) || !index.isValid() || !(index.flags() & Qt::ItemIsSelectable) || !(index.flags() & Qt::ItemIsEnabled))
                return failure(QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("Path is not selectable."), state);
        }
    }
    m_uiJobs.insert(jobId,
                    QJsonObject{{QStringLiteral("jobId"), jobId}, {QStringLiteral("status"), QStringLiteral("pending")}, {QStringLiteral("operation"), name}});
    QTimer::singleShot(0, this, [this, jobId, name, args, action, control, expected] {
        auto update = [this, jobId](const QString &status, const QString &error = QString()) {
            auto j = m_uiJobs.value(jobId);
            j.insert(QStringLiteral("status"), status);
            if (!error.isEmpty()) j.insert(QStringLiteral("error"), error);
            m_uiJobs.insert(jobId, j);
        };
        if (uiSnapshot().value(QStringLiteral("uiToken")).toString() != expected) {
            update(QStringLiteral("failed"), QStringLiteral("UI changed before the queued operation ran; inspect again."));
            return;
        }
        if (args.contains(QStringLiteral("expectedModel"))) {
            auto *view = qobject_cast<QAbstractItemView *>(control.data());
            if (!view || uiModelToken(view->model()) != args.value(QStringLiteral("expectedModel")).toString()) {
                update(QStringLiteral("failed"), QStringLiteral("Model rows changed before the queued operation ran."));
                return;
            }
        }
        update(QStringLiteral("running"));
        if (name == QLatin1String("trigger_action")) {
            if (!action || !action->isEnabled()) {
                update(QStringLiteral("failed"), QStringLiteral("Action disappeared or was disabled."));
                return;
            }
            action->trigger();
        } else if (!control || !control->isVisible() || !control->isEnabled()) {
            update(QStringLiteral("failed"), QStringLiteral("Control disappeared or was disabled."));
            return;
        } else if (name == QLatin1String("set_ui_value")) {
            const auto v = args.value(QStringLiteral("value"));
            const bool commit = args.value(QStringLiteral("commit")).toBool(true);
            if (auto *w = qobject_cast<DragValue *>(control.data())) {
                w->setValue(v.toDouble(), true, true);
            } else if (auto *w = qobject_cast<QLineEdit *>(control.data())) {
                if (!allowedLineValue(w, v, commit)) {
                    update(QStringLiteral("failed"), QStringLiteral("Input rules changed before the operation ran."));
                    return;
                }
                const bool changed = w->text() != v.toString();
                w->setText(v.toString());
                if (changed && control) Q_EMIT w->textEdited(v.toString());
                if (commit && control) Q_EMIT w->editingFinished();
            } else if (auto *w = qobject_cast<QPlainTextEdit *>(control.data()))
                w->setPlainText(v.toString());
            else if (auto *w = qobject_cast<QTextEdit *>(control.data()))
                w->setPlainText(v.toString());
            else if (auto *w = qobject_cast<QSpinBox *>(control.data())) {
                w->setValue(v.toInt());
                if (commit && control) Q_EMIT w->editingFinished();
            } else if (auto *w = qobject_cast<QDoubleSpinBox *>(control.data())) {
                w->setValue(v.toDouble());
                if (commit && control) Q_EMIT w->editingFinished();
            } else if (auto *w = qobject_cast<QComboBox *>(control.data())) {
                if (v.isString())
                    w->setEditText(v.toString());
                else {
                    w->setCurrentIndex(v.toInt());
                    if (control) Q_EMIT w->activated(v.toInt());
                }
            } else if (auto *w = qobject_cast<QAbstractButton *>(control.data())) {
                if (w->isChecked() != v.toBool()) w->click();
                if (control && w->isChecked() != v.toBool()) {
                    update(QStringLiteral("failed"), QStringLiteral("Native button rules refused the requested checked state."));
                    return;
                }
            } else if (auto *w = qobject_cast<QGroupBox *>(control.data())) {
                const bool changed = w->isChecked() != v.toBool();
                w->setChecked(v.toBool());
                if (changed && control) Q_EMIT w->clicked(v.toBool());
            } else if (auto *w = qobject_cast<QAbstractSlider *>(control.data())) {
                w->setSliderDown(true);
                if (control) w->setSliderPosition(v.toInt());
                if (control) w->setSliderDown(false);
            }
        } else if (name == QLatin1String("select_ui_rows")) {
            auto *view = qobject_cast<QAbstractItemView *>(control.data());
            view->selectionModel()->clearSelection();
            for (const auto &path : args.value(QStringLiteral("paths")).toArray()) {
                QModelIndex index;
                if (!modelIndex(view->model(), path, index) || !index.isValid()) {
                    update(QStringLiteral("failed"), QStringLiteral("Model changed before selection."));
                    return;
                }
                view->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
            }
        } else {
            const QString op = args.value(QStringLiteral("operation")).toString();
            if (op == QLatin1String("focus"))
                control->setFocus(Qt::OtherFocusReason);
            else if (op == QLatin1String("click"))
                qobject_cast<QAbstractButton *>(control.data())->click();
            else if (op == QLatin1String("reject_dialog"))
                qobject_cast<QDialog *>(control.data())->reject();
            else {
                auto *view = qobject_cast<QAbstractItemView *>(control.data());
                QModelIndex index;
                if (!modelIndex(view->model(), args.value(QStringLiteral("path")), index) || !index.isValid()) {
                    update(QStringLiteral("failed"), QStringLiteral("Model changed before activation."));
                    return;
                }
                view->setCurrentIndex(index);
                Q_EMIT view->activated(index);
            }
        }
        ++m_revision;
        update(QStringLiteral("returned"));
        auto j = m_uiJobs.value(jobId);
        j.insert(QStringLiteral("ui"), uiSnapshot());
        j.insert(QStringLiteral("completionMeaning"),
                 QStringLiteral("Native handler returned; inspect editor/job/output state to verify the requested feature completed."));
        m_uiJobs.insert(jobId, j);
    });
    return toolResult({{QStringLiteral("ok"), true},
                       {QStringLiteral("accepted"), true},
                       {QStringLiteral("jobId"), jobId},
                       {QStringLiteral("status"), QStringLiteral("pending")}});
}
