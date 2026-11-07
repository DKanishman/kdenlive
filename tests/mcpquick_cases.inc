/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "mcp/mcpquick.h"
#include <QAction>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWidget>
#include <QStringListModel>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWindow>

namespace {
const bool mcpQuickTestEnvironment = [] {
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")) qputenv("QT_QUICK_BACKEND", "software");
    return true;
}();
QJsonObject quickTestControl(McpProject &project, const QString &name)
{
    const auto ui = project.success(QStringLiteral("get_quick_ui"), {{QStringLiteral("query"), name}}).value(QStringLiteral("ui")).toObject();
    for (const auto &value : ui.value(QStringLiteral("controls")).toArray()) {
        const auto control = value.toObject();
        if (control.value(QStringLiteral("objectName")).toString() == name) {
            QJsonObject result = control;
            result.insert(QStringLiteral("expectedQuick"), ui.value(QStringLiteral("quickToken")));
            return result;
        }
    }
    INFO(QJsonDocument(ui).toJson(QJsonDocument::Compact).constData());
    FAIL("Requested native Quick/menu control was not discovered");
    return {};
}
QJsonObject quickTestArguments(const QJsonObject &control)
{
    QJsonObject result{{QStringLiteral("controlId"), control.value(QStringLiteral("id"))},
                       {QStringLiteral("expectedQuick"), control.value(QStringLiteral("expectedQuick"))}};
    if (control.contains(QStringLiteral("modelToken"))) result.insert(QStringLiteral("expectedModel"), control.value(QStringLiteral("modelToken")));
    return result;
}
QJsonObject awaitQuickTestJob(McpProject &project, const QJsonObject &accepted)
{
    REQUIRE(accepted.value(QStringLiteral("accepted")).toBool());
    REQUIRE_FALSE(accepted.value(QStringLiteral("jobId")).toString().isEmpty());
    QElapsedTimer deadline;
    deadline.start();
    QJsonObject job;
    while (deadline.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        job = project.success(QStringLiteral("quick_job_status"), {{QStringLiteral("jobId"), accepted.value(QStringLiteral("jobId"))}})
                  .value(QStringLiteral("job"))
                  .toObject();
        const QString status = job.value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("returned") || status == QStringLiteral("failed")) return job;
        QThread::msleep(1);
    }
    FAIL("Queued native Quick/menu handler did not return within the test deadline");
    return job;
}
void requireQuickTestReturned(McpProject &project, const QString &tool, const QJsonObject &arguments)
{
    const auto job = awaitQuickTestJob(project, project.success(tool, arguments));
    INFO(QJsonDocument(job).toJson(QJsonDocument::Compact).constData());
    REQUIRE(job.value(QStringLiteral("status")).toString() == QStringLiteral("returned"));
}
QObject *createQuickTestScene(QQuickWidget &widget, const QByteArray &source)
{
    auto *component = new QQmlComponent(widget.engine(), &widget);
    component->setData(source, QUrl(QStringLiteral("qrc:/native-mcp-quick-test.qml")));
    INFO(component->errorString().toUtf8().constData());
    REQUIRE(component->status() == QQmlComponent::Ready);
    QObject *root = component->create();
    REQUIRE(root);
    widget.setContent(QUrl(QStringLiteral("qrc:/native-mcp-quick-test.qml")), component, root);
    widget.resize(500, 500);
    widget.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    REQUIRE(widget.status() == QQuickWidget::Ready);
    return root;
}
} // namespace

TEST_CASE("MCP native Quick setters deliver actual button input and commit handlers", "[MCP][Quick]")
{
    McpProject project;
    project.open();
    QQuickWidget widget;
    QObject *root = createQuickTestScene(widget, QByteArrayLiteral(R"qml(
import QtQuick
import QtQuick.Controls
Item {
    id: root; width: 500; height: 500
    property int clicks: 0; property int checks: 0; property int spinEdits: 0
    property int sliderMoves: 0; property int textEdits: 0; property int commits: 0
    Button { objectName: "mcp-quick-button"; x: 10; y: 10; text: "Native"; onClicked: root.clicks += 1 }
    CheckBox { objectName: "mcp-quick-check"; x: 10; y: 60; text: "Native toggle"; onClicked: root.checks += 1 }
    SpinBox { objectName: "mcp-quick-spin"; x: 10; y: 110; from: 0; to: 10; value: 1; onValueModified: root.spinEdits += 1 }
    Slider { objectName: "mcp-quick-slider"; x: 10; y: 160; from: 0; to: 1; value: 0.2; onMoved: root.sliderMoves += 1 }
    TextField {
        objectName: "mcp-quick-text"; x: 10; y: 210; text: "5"; maximumLength: 2
        validator: IntValidator { bottom: 1; top: 99 }
        onTextEdited: root.textEdits += 1
        onEditingFinished: root.commits += 1
    }
    TextField { objectName: "mcp-quick-secret"; x: 10; y: 260; text: "secret-value"; echoMode: TextInput.Password }
}
)qml"));
    auto control = quickTestControl(project, QStringLiteral("mcp-quick-button"));
    auto args = quickTestArguments(control);
    args.insert(QStringLiteral("operation"), QStringLiteral("click"));
    requireQuickTestReturned(project, QStringLiteral("activate_quick_control"), args);
    CHECK(root->property("clicks").toInt() == 1);
    control = quickTestControl(project, QStringLiteral("mcp-quick-check"));
    args = quickTestArguments(control);
    args.insert(QStringLiteral("value"), true);
    requireQuickTestReturned(project, QStringLiteral("set_quick_value"), args);
    CHECK(root->property("checks").toInt() == 1);
    CHECK(root->findChild<QObject *>(QStringLiteral("mcp-quick-check"))->property("checked").toBool());
    control = quickTestControl(project, QStringLiteral("mcp-quick-spin"));
    args = quickTestArguments(control);
    args.insert(QStringLiteral("value"), 7);
    requireQuickTestReturned(project, QStringLiteral("set_quick_value"), args);
    CHECK(root->property("spinEdits").toInt() == 1);
    control = quickTestControl(project, QStringLiteral("mcp-quick-slider"));
    args = quickTestArguments(control);
    args.insert(QStringLiteral("value"), 0.7);
    requireQuickTestReturned(project, QStringLiteral("set_quick_value"), args);
    CHECK(root->property("sliderMoves").toInt() == 1);
    control = quickTestControl(project, QStringLiteral("mcp-quick-text"));
    for (const auto &invalid : {QStringLiteral("bad"), QStringLiteral("ab"), QStringLiteral("0")}) {
        args = quickTestArguments(control);
        args.insert(QStringLiteral("value"), invalid);
        CHECK(project.call(QStringLiteral("set_quick_value"), args).value(QStringLiteral("isError")).toBool());
    }
    CHECK(root->property("textEdits").toInt() == 0);
    CHECK(root->property("commits").toInt() == 0);
    CHECK(root->findChild<QObject *>(QStringLiteral("mcp-quick-text"))->property("text").toString() == QStringLiteral("5"));
    args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-text")));
    args.insert(QStringLiteral("value"), QStringLiteral("42"));
    requireQuickTestReturned(project, QStringLiteral("set_quick_value"), args);
    CHECK(root->property("textEdits").toInt() == 1);
    CHECK(root->property("commits").toInt() == 1);
    control = quickTestControl(project, QStringLiteral("mcp-quick-secret"));
    CHECK(control.value(QStringLiteral("value")).toString() == QStringLiteral("[redacted]"));
    CHECK_FALSE(control.contains(QStringLiteral("valueHash")));
    args = quickTestArguments(control);
    args.insert(QStringLiteral("value"), QStringLiteral("changed"));
    CHECK(project.call(QStringLiteral("set_quick_value"), args).value(QStringLiteral("isError")).toBool());
    CHECK(root->findChild<QObject *>(QStringLiteral("mcp-quick-secret"))->property("text").toString() == QStringLiteral("secret-value"));
    args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-button")));
    args.insert(QStringLiteral("operation"), QStringLiteral("deleteLater"));
    CHECK(project.call(QStringLiteral("activate_quick_control"), args).value(QStringLiteral("isError")).toBool());
    args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-text")));
    args.insert(QStringLiteral("value"), QStringLiteral("12"));
    args.insert(QStringLiteral("property"), QStringLiteral("x"));
    CHECK(project.call(QStringLiteral("set_quick_value"), args).value(QStringLiteral("isError")).toBool());
}

TEST_CASE("MCP Quick model versions popup scopes and native editing context reject stale operations", "[MCP][Quick]")
{
    McpProject project;
    project.open();
    const int clip =
        project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 10)).value(QStringLiteral("clipId")).toInt();
    QStringListModel choices(QStringList{QStringLiteral("One"), QStringLiteral("Two")});
    QQuickWidget widget;
    widget.engine()->rootContext()->setContextProperty(QStringLiteral("testChoices"), &choices);
    QObject *root = createQuickTestScene(widget, QByteArrayLiteral(R"qml(
import QtQuick
import QtQuick.Controls
Item {
    id: root; width: 500; height: 500; property int clicks: 0; property int activatedIndex: -1
    Button { objectName: "mcp-quick-context-button"; x: 10; y: 10; text: "Context"; onClicked: root.clicks += 1 }
    ComboBox { objectName: "mcp-quick-combo"; x: 10; y: 70; model: testChoices; textRole: "display"; onActivated: function(index) { root.activatedIndex = index } }
    Popup { id: panel; objectName: "mcp-quick-popup"; parent: root; width: 220; height: 120; modal: true; closePolicy: Popup.NoAutoClose
        Button { objectName: "mcp-quick-popup-button"; text: "Inside"; onClicked: root.clicks += 10 }
    }
}
)qml"));
    auto *behind = new QLineEdit(&widget);
    behind->setObjectName(QStringLiteral("mcp-quick-native-behind"));
    behind->setGeometry(300, 440, 160, 30);
    behind->show();
    QCoreApplication::processEvents();
    auto control = quickTestControl(project, QStringLiteral("mcp-quick-combo"));
    auto page = project.success(QStringLiteral("get_quick_model"), {{QStringLiteral("controlId"), control.value(QStringLiteral("id"))}});
    CHECK(page.value(QStringLiteral("total")).toInt() == 2);
    REQUIRE(choices.setData(choices.index(0, 0), QStringLiteral("Changed"), Qt::EditRole));
    auto args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-combo")));
    args.insert(QStringLiteral("expectedModel"), page.value(QStringLiteral("modelToken")));
    args.insert(QStringLiteral("value"), 1);
    CHECK(project.call(QStringLiteral("set_quick_value"), args).value(QStringLiteral("isError")).toBool());
    args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-combo")));
    args.insert(QStringLiteral("value"), 1);
    requireQuickTestReturned(project, QStringLiteral("set_quick_value"), args);
    CHECK(root->property("activatedIndex").toInt() == 1);
    control = quickTestControl(project, QStringLiteral("mcp-quick-context-button"));
    REQUIRE(project.timeline->requestSetSelection({clip}));
    args = quickTestArguments(control);
    args.insert(QStringLiteral("operation"), QStringLiteral("click"));
    CHECK(project.call(QStringLiteral("activate_quick_control"), args).value(QStringLiteral("isError")).toBool());
    CHECK(root->property("clicks").toInt() == 0);
    REQUIRE(project.timeline->requestClearSelection());
    args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-popup")));
    args.insert(QStringLiteral("operation"), QStringLiteral("open"));
    requireQuickTestReturned(project, QStringLiteral("activate_quick_control"), args);
    // Read-only native and widget inspection remains available, but editing behind a Quick modal is refused.
    CHECK(McpQuick::modalPopupActive(&project.server));
    CHECK(project.success(QStringLiteral("get_state")).value(QStringLiteral("state")).toObject().value(QStringLiteral("ready")).toBool());
    CHECK_FALSE(project.success(QStringLiteral("list_profiles")).value(QStringLiteral("profiles")).toArray().isEmpty());
    auto nativeEdit = project.expected();
    nativeEdit.insert(QStringLiteral("trackId"), project.videoTracks.front());
    nativeEdit.insert(QStringLiteral("name"), QStringLiteral("Must remain unchanged"));
    project.rejectWithoutChange(QStringLiteral("rename_track"), nativeEdit);
    const auto widgetUi = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
    QString behindId;
    for (const auto &value : widgetUi.value(QStringLiteral("controls")).toArray()) {
        const auto field = value.toObject();
        if (field.value(QStringLiteral("objectName")).toString() == behind->objectName()) behindId = field.value(QStringLiteral("id")).toString();
    }
    REQUIRE_FALSE(behindId.isEmpty());
    const auto widgetEdit = project.call(QStringLiteral("set_ui_value"), {{QStringLiteral("expectedUi"), widgetUi.value(QStringLiteral("uiToken"))},
                                                                          {QStringLiteral("controlId"), behindId},
                                                                          {QStringLiteral("value"), QStringLiteral("Must not apply")}});
    CHECK(widgetEdit.value(QStringLiteral("isError")).toBool());
    CHECK(behind->text().isEmpty());
    args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-context-button")));
    args.insert(QStringLiteral("operation"), QStringLiteral("click"));
    CHECK(project.call(QStringLiteral("activate_quick_control"), args).value(QStringLiteral("isError")).toBool());
    args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-popup-button")));
    args.insert(QStringLiteral("operation"), QStringLiteral("click"));
    requireQuickTestReturned(project, QStringLiteral("activate_quick_control"), args);
    CHECK(root->property("clicks").toInt() == 10);
    args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-popup")));
    args.insert(QStringLiteral("operation"), QStringLiteral("close"));
    requireQuickTestReturned(project, QStringLiteral("activate_quick_control"), args);
    control = quickTestControl(project, QStringLiteral("mcp-quick-context-button"));
    args = quickTestArguments(control);
    args.insert(QStringLiteral("operation"), QStringLiteral("click"));
    const auto pending = project.success(QStringLiteral("activate_quick_control"), args);
    REQUIRE(project.timeline->requestSetSelection({clip}));
    const auto refused = awaitQuickTestJob(project, pending);
    CHECK(refused.value(QStringLiteral("status")).toString() == QStringLiteral("failed"));
    CHECK(root->property("clicks").toInt() == 10);
    REQUIRE(project.timeline->requestClearSelection());
}

TEST_CASE("MCP discovers stable native menu actions and opens item-view context menus", "[MCP][Quick]")
{
    McpProject project;
    project.open();
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *bar = new QMenuBar(&window);
    layout->addWidget(bar);
    auto *menu = bar->addMenu(QStringLiteral("Native"));
    auto *action = menu->addAction(QStringLiteral("First"));
    action->setObjectName(QStringLiteral("mcp-quick-menu-first"));
    auto *second = menu->addAction(QStringLiteral("Second"));
    second->setObjectName(QStringLiteral("mcp-quick-menu-second"));
    int firstCount = 0, contextCount = 0;
    QObject::connect(action, &QAction::triggered, [&firstCount] { ++firstCount; });
    auto *tree = new QTreeWidget(&window);
    tree->setObjectName(QStringLiteral("mcp-quick-native-context"));
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    tree->addTopLevelItem(new QTreeWidgetItem(QStringList{QStringLiteral("Source")}));
    tree->setCurrentItem(tree->topLevelItem(0));
    layout->addWidget(tree);
    auto *context = new QMenu(&window);
    auto *contextAction = context->addAction(QStringLiteral("Context operation"));
    contextAction->setObjectName(QStringLiteral("mcp-quick-context-action"));
    QObject::connect(contextAction, &QAction::triggered, [&contextCount] { ++contextCount; });
    QObject::connect(tree, &QTreeWidget::customContextMenuRequested,
                     [context, tree](const QPoint &point) { context->popup(tree->viewport()->mapToGlobal(point)); });
    window.resize(400, 300);
    window.show();
    QCoreApplication::processEvents();
    auto control = quickTestControl(project, QStringLiteral("mcp-quick-menu-first"));
    auto args = quickTestArguments(control);
    args.insert(QStringLiteral("operation"), QStringLiteral("trigger"));
    requireQuickTestReturned(project, QStringLiteral("activate_quick_control"), args);
    CHECK(firstCount == 1);
    control = quickTestControl(project, QStringLiteral("mcp-quick-menu-first"));
    menu->removeAction(second);
    menu->insertAction(action, second);
    args = quickTestArguments(control);
    args.insert(QStringLiteral("operation"), QStringLiteral("trigger"));
    CHECK(project.call(QStringLiteral("activate_quick_control"), args).value(QStringLiteral("isError")).toBool());
    CHECK(firstCount == 1);
    args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-native-context")));
    args.remove(QStringLiteral("expectedModel")); // Fixed context-menu operation is guarded by its complete Quick context token.
    args.insert(QStringLiteral("operation"), QStringLiteral("context_menu"));
    requireQuickTestReturned(project, QStringLiteral("activate_quick_control"), args);
    REQUIRE(context->isVisible());
    args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-context-action")));
    args.insert(QStringLiteral("operation"), QStringLiteral("trigger"));
    requireQuickTestReturned(project, QStringLiteral("activate_quick_control"), args);
    CHECK(contextCount == 1);
    CHECK_FALSE(context->isVisible());
}

TEST_CASE("MCP Quick text selections invalidate queued Quick and QWidget actions", "[MCP][Quick]")
{
    McpProject project;
    project.open();
    QQuickWidget widget;
    QObject *root = createQuickTestScene(widget, QByteArrayLiteral(R"qml(
import QtQuick
import QtQuick.Controls
Item {
    id: root; width: 500; height: 500; property int clicks: 0; property string captured: ""
    TextField { id: field; objectName: "mcp-quick-selection-field"; text: "abcdef"; x: 10; y: 10 }
    Button { objectName: "mcp-quick-selection-button"; text: "Capture selection"; x: 10; y: 70
        onClicked: { root.captured = field.selectedText; root.clicks += 1 }
    }
}
)qml"));
    auto *field = root->findChild<QObject *>(QStringLiteral("mcp-quick-selection-field"));
    REQUIRE(field);
    const auto choose = [field](int start, int end) {
        REQUIRE(QMetaObject::invokeMethod(field, "select", Qt::DirectConnection, Q_ARG(int, start), Q_ARG(int, end)));
    };
    choose(0, 2);
    auto before = quickTestControl(project, QStringLiteral("mcp-quick-selection-field"));
    CHECK(before.value(QStringLiteral("selectionStart")).toInt() == 0);
    CHECK(before.value(QStringLiteral("selectionEnd")).toInt() == 2);
    const auto button = quickTestControl(project, QStringLiteral("mcp-quick-selection-button"));
    auto args = quickTestArguments(button);
    args.insert(QStringLiteral("operation"), QStringLiteral("click"));
    const auto accepted = project.success(QStringLiteral("activate_quick_control"), args);
    choose(2, 4);
    const auto failed = awaitQuickTestJob(project, accepted);
    CHECK(failed.value(QStringLiteral("status")).toString() == QStringLiteral("failed"));
    CHECK(root->property("clicks").toInt() == 0);
    const auto after = quickTestControl(project, QStringLiteral("mcp-quick-selection-field"));
    CHECK(after.value(QStringLiteral("expectedQuick")) != before.value(QStringLiteral("expectedQuick")));
    CHECK(after.value(QStringLiteral("selectionStart")).toInt() == 2);
    CHECK(after.value(QStringLiteral("selectionEnd")).toInt() == 4);
    args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-selection-button")));
    args.insert(QStringLiteral("operation"), QStringLiteral("click"));
    requireQuickTestReturned(project, QStringLiteral("activate_quick_control"), args);
    CHECK(root->property("clicks").toInt() == 1);
    CHECK(root->property("captured").toString() == QStringLiteral("cd"));
    auto *native = new QLineEdit(&widget);
    native->setObjectName(QStringLiteral("mcp-quick-selection-native"));
    native->setGeometry(250, 350, 200, 30);
    native->show();
    QCoreApplication::processEvents();
    const auto ui = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
    QString nativeId;
    for (const auto &entry : ui.value(QStringLiteral("controls")).toArray()) {
        const auto control = entry.toObject();
        if (control.value(QStringLiteral("objectName")).toString() == native->objectName()) nativeId = control.value(QStringLiteral("id")).toString();
    }
    REQUIRE_FALSE(nativeId.isEmpty());
    const auto queued = project.success(QStringLiteral("set_ui_value"), {{QStringLiteral("expectedUi"), ui.value(QStringLiteral("uiToken"))},
                                                                         {QStringLiteral("controlId"), nativeId},
                                                                         {QStringLiteral("value"), QStringLiteral("Must not be applied")}});
    REQUIRE(field->setProperty("cursorPosition", 5));
    QCoreApplication::processEvents();
    const auto nativeJob = project.success(QStringLiteral("ui_job_status"), {{QStringLiteral("jobId"), queued.value(QStringLiteral("jobId"))}})
                               .value(QStringLiteral("job"))
                               .toObject();
    CHECK(nativeJob.value(QStringLiteral("status")).toString() == QStringLiteral("failed"));
    CHECK(native->text().isEmpty());
}

TEST_CASE("MCP native nested menu action closes its full visible ancestor chain", "[MCP][Quick]")
{
    McpProject project;
    project.open();
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *bar = new QMenuBar(&window);
    layout->addWidget(bar);
    auto *menu = bar->addMenu(QStringLiteral("Root"));
    auto *submenu = menu->addMenu(QStringLiteral("Nested"));
    auto *action = submenu->addAction(QStringLiteral("Native operation"));
    action->setObjectName(QStringLiteral("mcp-quick-nested-leaf"));
    int triggered = 0;
    QObject::connect(action, &QAction::triggered, [&triggered] { ++triggered; });
    window.resize(400, 300);
    window.show();
    menu->popup(window.mapToGlobal(QPoint(0, 40)));
    submenu->popup(window.mapToGlobal(QPoint(150, 40)));
    QCoreApplication::processEvents();
    REQUIRE(menu->isVisible());
    REQUIRE(submenu->isVisible());
    auto args = quickTestArguments(quickTestControl(project, QStringLiteral("mcp-quick-nested-leaf")));
    args.insert(QStringLiteral("operation"), QStringLiteral("trigger"));
    requireQuickTestReturned(project, QStringLiteral("activate_quick_control"), args);
    CHECK(triggered == 1);
    CHECK_FALSE(submenu->isVisible());
    CHECK_FALSE(menu->isVisible());
    CHECK(QApplication::activePopupWidget() == nullptr);
}

TEST_CASE("MCP nested context menus retain ancestor closure when the submenu is enumerated first", "[MCP][Quick][QuickMenus]")
{
    McpProject project;
    project.open();
    QWidget window;
    window.resize(400, 300);
    window.show();
    // Drain native window exposure before opening a popup. A delayed Windows
    // activation/exposure event can otherwise dismiss an already-open menu.
    QElapsedTimer exposure;
    exposure.start();
    while ((!window.windowHandle() || !window.windowHandle()->isExposed()) && exposure.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    REQUIRE(window.windowHandle());
    REQUIRE(window.windowHandle()->isExposed());
    auto *one = new QMenu(&window);
    auto *two = new QMenu(&window);
    // Warm native popup windows before selecting roles from Qt's actual unordered
    // widget inventory. No menu bar supplies an ordered alternate discovery path.
    one->addAction(QStringLiteral("Warm first"));
    two->addAction(QStringLiteral("Warm second"));
    one->popup(window.mapToGlobal(QPoint(0, 40)));
    QCoreApplication::processEvents();
    one->hide();
    QCoreApplication::processEvents();
    two->popup(window.mapToGlobal(QPoint(150, 40)));
    QCoreApplication::processEvents();
    two->hide();
    QCoreApplication::processEvents();
    one->clear();
    two->clear();
    const auto inventory = QApplication::allWidgets();
    REQUIRE(inventory.contains(one));
    REQUIRE(inventory.contains(two));
    QMenu *submenu = inventory.indexOf(one) < inventory.indexOf(two) ? one : two;
    QMenu *root = submenu == one ? two : one;
    root->setObjectName(QStringLiteral("mcp-quick-context-root-last"));
    submenu->setObjectName(QStringLiteral("mcp-quick-context-submenu-first"));
    root->setTitle(QStringLiteral("Context root"));
    submenu->setTitle(QStringLiteral("Nested context"));
    // Give the native popup an actual submenu owner after selecting the
    // deliberately reversed inventory order; addMenu(QMenu*) does not reparent.
    // QWidget::setParent without flags turns a warmed popup into a child
    // widget. Preserve QMenu's popup window type when changing ownership.
    submenu->setParent(root, submenu->windowFlags());
    root->addMenu(submenu);
    auto *action = submenu->addAction(QStringLiteral("Context operation"));
    action->setObjectName(QStringLiteral("mcp-quick-context-ordered-leaf"));
    int triggered = 0;
    QObject::connect(action, &QAction::triggered, [&triggered] { ++triggered; });
    root->popup(window.mapToGlobal(QPoint(0, 40)));
    submenu->popup(window.mapToGlobal(QPoint(150, 40)));
    QCoreApplication::processEvents();
    REQUIRE(root->isVisible());
    REQUIRE(submenu->isVisible());
    const auto activeInventory = QApplication::allWidgets();
    REQUIRE(activeInventory.indexOf(submenu) < activeInventory.indexOf(root));
    const auto rootControl = quickTestControl(project, root->objectName());
    const auto leaf = quickTestControl(project, action->objectName());
    CHECK(leaf.value(QStringLiteral("menus")).toArray().contains(rootControl.value(QStringLiteral("id"))));
    auto args = quickTestArguments(leaf);
    args.insert(QStringLiteral("operation"), QStringLiteral("trigger"));
    requireQuickTestReturned(project, QStringLiteral("activate_quick_control"), args);
    CHECK(triggered == 1);
    CHECK_FALSE(root->isVisible());
    CHECK_FALSE(submenu->isVisible());
    CHECK(QApplication::activePopupWidget() == nullptr);
}

TEST_CASE("MCP shared submenus accumulate every menu-bar ancestor path", "[MCP][Quick][QuickMenus]")
{
    McpProject project;
    project.open();
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *bar = new QMenuBar(&window);
    layout->addWidget(bar);
    auto *firstRoot = bar->addMenu(QStringLiteral("First root"));
    auto *secondRoot = bar->addMenu(QStringLiteral("Second root"));
    auto *shared = new QMenu(QStringLiteral("Shared"), &window);
    firstRoot->setObjectName(QStringLiteral("mcp-quick-shared-root-one"));
    secondRoot->setObjectName(QStringLiteral("mcp-quick-shared-root-two"));
    shared->setObjectName(QStringLiteral("mcp-quick-shared-submenu"));
    firstRoot->addMenu(shared);
    secondRoot->addMenu(shared);
    auto *action = shared->addAction(QStringLiteral("Shared operation"));
    action->setObjectName(QStringLiteral("mcp-quick-shared-leaf"));
    int triggered = 0;
    QObject::connect(action, &QAction::triggered, [&triggered] { ++triggered; });
    window.resize(400, 300);
    window.show();
    QCoreApplication::processEvents();
    const auto first = quickTestControl(project, firstRoot->objectName());
    const auto second = quickTestControl(project, secondRoot->objectName());
    auto leaf = quickTestControl(project, action->objectName());
    const auto owners = leaf.value(QStringLiteral("menus")).toArray();
    CHECK(owners.contains(first.value(QStringLiteral("id"))));
    CHECK(owners.contains(second.value(QStringLiteral("id"))));
    REQUIRE(owners.size() == 3);
    // Activate through the second root. A global visited set records only the
    // first root's path and leaves this second popup holding its native grab.
    secondRoot->popup(window.mapToGlobal(QPoint(0, 40)));
    shared->popup(window.mapToGlobal(QPoint(150, 40)));
    QCoreApplication::processEvents();
    REQUIRE(secondRoot->isVisible());
    REQUIRE(shared->isVisible());
    leaf = quickTestControl(project, action->objectName());
    auto args = quickTestArguments(leaf);
    args.insert(QStringLiteral("operation"), QStringLiteral("trigger"));
    requireQuickTestReturned(project, QStringLiteral("activate_quick_control"), args);
    CHECK(triggered == 1);
    CHECK_FALSE(firstRoot->isVisible());
    CHECK_FALSE(secondRoot->isVisible());
    CHECK_FALSE(shared->isVisible());
    CHECK(QApplication::activePopupWidget() == nullptr);
}
