// Native Qt bridge behavior: actual setters, asynchronous job status, stale tokens, and refusal paths.
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

TEST_CASE("MCP native Qt controls expose actual state and queue mutations", "[MCP][UI]")
{
    McpProject project;
    project.open();
    QDialog dialog;
    dialog.setObjectName(QStringLiteral("mcp-test-dialog"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *line = new QLineEdit(&dialog);
    line->setObjectName(QStringLiteral("mcp-test-text"));
    layout->addWidget(line);
    auto *spin = new QSpinBox(&dialog);
    spin->setObjectName(QStringLiteral("mcp-test-spin"));
    spin->setRange(0, 20);
    layout->addWidget(spin);
    auto *button = new QPushButton(QStringLiteral("Native test"), &dialog);
    button->setObjectName(QStringLiteral("mcp-test-button"));
    layout->addWidget(button);
    auto *tree = new QTreeWidget(&dialog);
    tree->setObjectName(QStringLiteral("mcp-test-tree"));
    tree->setColumnCount(1);
    tree->addTopLevelItem(new QTreeWidgetItem(QStringList{QStringLiteral("First")}));
    tree->addTopLevelItem(new QTreeWidgetItem(QStringList{QStringLiteral("Second")}));
    layout->addWidget(tree);
    dialog.show();
    QCoreApplication::processEvents();
    auto ui = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
    auto idFor = [&ui](const QString &name) {
        for (const auto &value : ui.value(QStringLiteral("controls")).toArray()) {
            const auto control = value.toObject();
            if (control.value(QStringLiteral("objectName")).toString() == name) return control.value(QStringLiteral("id")).toString();
        }
        return QString();
    };
    const QString lineId = idFor(line->objectName());
    const QString spinId = idFor(spin->objectName());
    const QString buttonId = idFor(button->objectName());
    const QString treeId = idFor(tree->objectName());
    REQUIRE_FALSE(lineId.isEmpty());
    REQUIRE_FALSE(spinId.isEmpty());
    REQUIRE_FALSE(buttonId.isEmpty());
    REQUIRE_FALSE(treeId.isEmpty());
    const QString oldToken = ui.value(QStringLiteral("uiToken")).toString();
    auto queued = project.success(
        QStringLiteral("set_ui_value"),
        {{QStringLiteral("expectedUi"), oldToken}, {QStringLiteral("controlId"), lineId}, {QStringLiteral("value"), QStringLiteral("Native text")}});
    CHECK(queued.value(QStringLiteral("accepted")).toBool());
    QCoreApplication::processEvents();
    CHECK(line->text() == QStringLiteral("Native text"));
    const auto job = project.success(QStringLiteral("ui_job_status"), {{QStringLiteral("jobId"), queued.value(QStringLiteral("jobId"))}})
                         .value(QStringLiteral("job"))
                         .toObject();
    CHECK(job.value(QStringLiteral("status")).toString() == QStringLiteral("returned"));
    CHECK(project
              .call(QStringLiteral("set_ui_value"),
                    {{QStringLiteral("expectedUi"), oldToken}, {QStringLiteral("controlId"), lineId}, {QStringLiteral("value"), QStringLiteral("Stale")}})
              .value(QStringLiteral("isError"))
              .toBool());
    ui = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
    CHECK(project
              .call(QStringLiteral("set_ui_value"),
                    {{QStringLiteral("expectedUi"), ui.value(QStringLiteral("uiToken"))}, {QStringLiteral("controlId"), spinId}, {QStringLiteral("value"), 99}})
              .value(QStringLiteral("isError"))
              .toBool());
    CHECK(spin->value() == 0);
    project.success(
        QStringLiteral("set_ui_value"),
        {{QStringLiteral("expectedUi"), ui.value(QStringLiteral("uiToken"))}, {QStringLiteral("controlId"), spinId}, {QStringLiteral("value"), 17}});
    QCoreApplication::processEvents();
    CHECK(spin->value() == 17);
    int clicks = 0;
    QObject::connect(button, &QPushButton::clicked, [&clicks] { ++clicks; });
    ui = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
    project.success(QStringLiteral("activate_ui_control"), {{QStringLiteral("expectedUi"), ui.value(QStringLiteral("uiToken"))},
                                                            {QStringLiteral("controlId"), buttonId},
                                                            {QStringLiteral("operation"), QStringLiteral("click")}});
    QCoreApplication::processEvents();
    CHECK(clicks == 1);
    const auto rows =
        project.success(QStringLiteral("get_ui_model"), {{QStringLiteral("controlId"), treeId}, {QStringLiteral("offset"), 1}, {QStringLiteral("limit"), 1}});
    CHECK(rows.value(QStringLiteral("totalRows")).toInt() == 2);
    REQUIRE(rows.value(QStringLiteral("rows")).toArray().size() == 1);
    CHECK(rows.value(QStringLiteral("rows")).toArray().first().toObject().value(QStringLiteral("columns")).toArray().first().toString() ==
          QStringLiteral("Second"));
    ui = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
    project.success(QStringLiteral("select_ui_rows"), {{QStringLiteral("expectedUi"), ui.value(QStringLiteral("uiToken"))},
                                                       {QStringLiteral("controlId"), treeId},
                                                       {QStringLiteral("paths"), QJsonArray{QJsonValue(QJsonArray{1})}},
                                                       {QStringLiteral("expectedModel"), rows.value(QStringLiteral("modelToken"))}});
    QCoreApplication::processEvents();
    REQUIRE(tree->selectedItems().size() == 1);
    CHECK(tree->selectedItems().first()->text(0) == QStringLiteral("Second"));
    const auto capabilities = project.success(QStringLiteral("get_capabilities"));
    CHECK_FALSE(capabilities.value(QStringLiteral("completeEditorCoverageVerified")).toBool());
}

#include <QGroupBox>
#include <QIntValidator>
#include <QRadioButton>

TEST_CASE("MCP native inputs honor validators and user-edit/click handlers", "[MCP][UI]")
{
    McpProject project;
    project.open();
    QDialog dialog;
    auto *layout = new QVBoxLayout(&dialog);
    auto *input = new QLineEdit(QStringLiteral("5"), &dialog);
    input->setObjectName(QStringLiteral("validated-input"));
    input->setMaxLength(2);
    input->setValidator(new QIntValidator(1, 99, input));
    layout->addWidget(input);
    auto *first = new QRadioButton(QStringLiteral("First"), &dialog);
    first->setChecked(true);
    layout->addWidget(first);
    auto *radio = new QRadioButton(QStringLiteral("Second"), &dialog);
    radio->setObjectName(QStringLiteral("clicked-radio"));
    layout->addWidget(radio);
    auto *group = new QGroupBox(QStringLiteral("Options"), &dialog);
    group->setObjectName(QStringLiteral("checked-group"));
    group->setCheckable(true);
    group->setChecked(false);
    layout->addWidget(group);
    dialog.show();
    QCoreApplication::processEvents();
    QString edited;
    int radioClicks = 0;
    int groupClicks = 0;
    QObject::connect(input, &QLineEdit::textEdited, [&edited](const QString &text) { edited = text; });
    QObject::connect(radio, &QRadioButton::clicked, [&radioClicks] { ++radioClicks; });
    QObject::connect(group, &QGroupBox::clicked, [&groupClicks] { ++groupClicks; });
    auto control = [&project](const QString &name) {
        const auto ui = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
        for (const auto &value : ui.value(QStringLiteral("controls")).toArray()) {
            const auto item = value.toObject();
            if (item.value(QStringLiteral("objectName")).toString() == name)
                return QJsonObject{{QStringLiteral("expectedUi"), ui.value(QStringLiteral("uiToken"))},
                                   {QStringLiteral("controlId"), item.value(QStringLiteral("id"))}};
        }
        return QJsonObject{};
    };
    for (const auto &invalid : {QStringLiteral("bad"), QStringLiteral("100"), QStringLiteral("0")}) {
        auto args = control(input->objectName());
        REQUIRE_FALSE(args.isEmpty());
        args.insert(QStringLiteral("value"), invalid);
        CHECK(project.call(QStringLiteral("set_ui_value"), args).value(QStringLiteral("isError")).toBool());
        CHECK(input->text() == QStringLiteral("5"));
        CHECK(edited.isEmpty());
    }
    auto args = control(input->objectName());
    args.insert(QStringLiteral("value"), QStringLiteral("42"));
    project.success(QStringLiteral("set_ui_value"), args);
    QCoreApplication::processEvents();
    CHECK(input->text() == QStringLiteral("42"));
    CHECK(edited == QStringLiteral("42"));
    args = control(radio->objectName());
    args.insert(QStringLiteral("value"), true);
    project.success(QStringLiteral("set_ui_value"), args);
    QCoreApplication::processEvents();
    CHECK(radio->isChecked());
    CHECK_FALSE(first->isChecked());
    CHECK(radioClicks == 1);
    args = control(group->objectName());
    REQUIRE_FALSE(args.isEmpty());
    args.insert(QStringLiteral("value"), true);
    project.success(QStringLiteral("set_ui_value"), args);
    QCoreApplication::processEvents();
    CHECK(group->isChecked());
    CHECK(groupClicks == 1);
}
TEST_CASE("MCP row operations reject same-size model changes and native selection changes", "[MCP][UI]")
{
    McpProject project;
    project.open();
    const int clip =
        project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 10)).value(QStringLiteral("clipId")).toInt();
    QDialog dialog;
    auto *layout = new QVBoxLayout(&dialog);
    auto *tree = new QTreeWidget(&dialog);
    tree->setObjectName(QStringLiteral("revision-tree"));
    tree->addTopLevelItem(new QTreeWidgetItem(QStringList{QStringLiteral("Original")}));
    layout->addWidget(tree);
    auto *input = new QLineEdit(&dialog);
    input->setObjectName(QStringLiteral("selection-input"));
    layout->addWidget(input);
    dialog.show();
    QCoreApplication::processEvents();
    auto ui = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
    QString treeId, inputId;
    for (const auto &value : ui.value(QStringLiteral("controls")).toArray()) {
        const auto item = value.toObject();
        const QString name = item.value(QStringLiteral("objectName")).toString();
        if (name == tree->objectName()) treeId = item.value(QStringLiteral("id")).toString();
        if (name == input->objectName()) inputId = item.value(QStringLiteral("id")).toString();
    }
    REQUIRE_FALSE(treeId.isEmpty());
    REQUIRE_FALSE(inputId.isEmpty());
    auto page = project.success(QStringLiteral("get_ui_model"), {{QStringLiteral("controlId"), treeId}});
    tree->topLevelItem(0)->setText(0, QStringLiteral("Replaced"));
    ui = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
    const auto refused = project.call(QStringLiteral("select_ui_rows"), {{QStringLiteral("expectedUi"), ui.value(QStringLiteral("uiToken"))},
                                                                         {QStringLiteral("controlId"), treeId},
                                                                         {QStringLiteral("expectedModel"), page.value(QStringLiteral("modelToken"))},
                                                                         {QStringLiteral("paths"), QJsonArray{QJsonValue(QJsonArray{0})}}});
    CHECK(refused.value(QStringLiteral("isError")).toBool());
    CHECK(tree->selectedItems().isEmpty());
    REQUIRE(project.timeline->requestSetSelection({clip}));
    const auto stale = project.call(QStringLiteral("set_ui_value"), {{QStringLiteral("expectedUi"), ui.value(QStringLiteral("uiToken"))},
                                                                     {QStringLiteral("controlId"), inputId},
                                                                     {QStringLiteral("value"), QStringLiteral("Should not apply")}});
    CHECK(stale.value(QStringLiteral("isError")).toBool());
    CHECK(input->text().isEmpty());
    REQUIRE(project.timeline->requestClearSelection());
}

#include <QSlider>
#include <QTextEdit>

TEST_CASE("MCP UI context includes native text selection and rich formatting and sliders deliver drag signals", "[MCP][UI]")
{
    McpProject project;
    project.open();
    QDialog dialog;
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QTextEdit(&dialog);
    text->setObjectName(QStringLiteral("rich-context"));
    text->setPlainText(QStringLiteral("Native rich text"));
    layout->addWidget(text);
    auto *slider = new QSlider(Qt::Horizontal, &dialog);
    slider->setObjectName(QStringLiteral("native-slider"));
    slider->setRange(0, 100);
    layout->addWidget(slider);
    dialog.show();
    QCoreApplication::processEvents();
    auto ui = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
    auto controlId = [&ui](const QString &name) {
        for (const auto &value : ui.value(QStringLiteral("controls")).toArray()) {
            const auto item = value.toObject();
            if (item.value(QStringLiteral("objectName")).toString() == name) return item.value(QStringLiteral("id")).toString();
        }
        return QString();
    };
    const QString richId = controlId(text->objectName());
    const QString sliderId = controlId(slider->objectName());
    REQUIRE_FALSE(richId.isEmpty());
    REQUIRE_FALSE(sliderId.isEmpty());
    const auto beforeSelection = ui.value(QStringLiteral("uiToken"));
    auto cursor = text->textCursor();
    cursor.setPosition(0);
    cursor.setPosition(6, QTextCursor::KeepAnchor);
    text->setTextCursor(cursor);
    ui = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
    CHECK(ui.value(QStringLiteral("uiToken")) != beforeSelection);
    const auto beforeFormat = ui.value(QStringLiteral("uiToken"));
    QTextCharFormat format;
    format.setFontWeight(QFont::Bold);
    cursor.mergeCharFormat(format);
    const auto stale = project.call(
        QStringLiteral("set_ui_value"),
        {{QStringLiteral("expectedUi"), beforeFormat}, {QStringLiteral("controlId"), richId}, {QStringLiteral("value"), QStringLiteral("Stale overwrite")}});
    CHECK(stale.value(QStringLiteral("isError")).toBool());
    CHECK(text->toPlainText() == QStringLiteral("Native rich text"));
    CHECK(cursor.charFormat().fontWeight() == QFont::Bold);
    QStringList events;
    QObject::connect(slider, &QSlider::sliderPressed, [&events] { events << QStringLiteral("pressed"); });
    QObject::connect(slider, &QSlider::sliderMoved, [&events](int value) { events << QStringLiteral("moved:%1").arg(value); });
    QObject::connect(slider, &QSlider::sliderReleased, [&events] { events << QStringLiteral("released"); });
    ui = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
    const auto accepted = project.success(
        QStringLiteral("set_ui_value"),
        {{QStringLiteral("expectedUi"), ui.value(QStringLiteral("uiToken"))}, {QStringLiteral("controlId"), sliderId}, {QStringLiteral("value"), 37}});
    QCoreApplication::processEvents();
    const auto job = project.success(QStringLiteral("ui_job_status"), {{QStringLiteral("jobId"), accepted.value(QStringLiteral("jobId"))}})
                         .value(QStringLiteral("job"))
                         .toObject();
    CHECK(job.value(QStringLiteral("status")) == QStringLiteral("returned"));
    CHECK(slider->value() == 37);
    CHECK_FALSE(slider->isSliderDown());
    const QStringList expectedEvents{QStringLiteral("pressed"), QStringLiteral("moved:37"), QStringLiteral("released")};
    CHECK(events == expectedEvents);
}

#include "kdenlivesettings.h"
#include "widgets/dragvalue.h"
#include <QScopeGuard>

TEST_CASE("MCP edits native DragValue controls through final undo-aware parameter signals", "[MCP][UI][DragValue]")
{
    McpProject project;
    project.open();
    const bool previous = KdenliveSettings::dragvalue_directupdate();
    const auto restore = qScopeGuard([previous] { KdenliveSettings::setDragvalue_directupdate(previous); });
    for (bool direct : {false, true}) {
        KdenliveSettings::setDragvalue_directupdate(direct);
        QDialog dialog;
        auto *layout = new QVBoxLayout(&dialog);
        auto *drag = new DragValue(QStringLiteral("Native parameter"), 25, 1, 0, 100, -1, QString(), true, false, &dialog);
        drag->setObjectName(QStringLiteral("mcp-native-drag-value"));
        layout->addWidget(drag);
        auto *label = drag->findChild<CustomLabel *>();
        REQUIRE(label);
        label->setObjectName(QStringLiteral("mcp-native-drag-label"));
        double received = -1;
        bool receivedFinal = false, receivedUndo = false;
        int changes = 0;
        QObject::connect(drag, &DragValue::customValueChanged, [&received, &receivedFinal, &receivedUndo, &changes](double value, bool final, bool undo) {
            received = value;
            receivedFinal = final;
            receivedUndo = undo;
            ++changes;
        });
        dialog.show();
        QCoreApplication::processEvents();
        auto ui = project.success(QStringLiteral("get_ui")).value(QStringLiteral("ui")).toObject();
        QJsonObject parentControl, labelControl;
        for (const auto &value : ui.value(QStringLiteral("controls")).toArray()) {
            const auto item = value.toObject();
            if (item.value(QStringLiteral("objectName")) == drag->objectName()) parentControl = item;
            if (item.value(QStringLiteral("objectName")) == label->objectName()) labelControl = item;
        }
        REQUIRE_FALSE(parentControl.isEmpty());
        REQUIRE_FALSE(labelControl.isEmpty());
        CHECK(parentControl.value(QStringLiteral("writable")).toBool());
        CHECK(parentControl.value(QStringLiteral("value")).toDouble() == Approx(25));
        CHECK_FALSE(labelControl.value(QStringLiteral("writable")).toBool());
        CHECK(labelControl.value(QStringLiteral("valueControlId")) == parentControl.value(QStringLiteral("id")));
        const auto rejected = project.call(QStringLiteral("set_ui_value"), {{QStringLiteral("expectedUi"), ui.value(QStringLiteral("uiToken"))},
                                                                            {QStringLiteral("controlId"), labelControl.value(QStringLiteral("id"))},
                                                                            {QStringLiteral("value"), 50}});
        CHECK(rejected.value(QStringLiteral("isError")).toBool());
        CHECK(drag->value() == Approx(25));
        CHECK(changes == 0);
        const auto accepted = project.success(QStringLiteral("set_ui_value"), {{QStringLiteral("expectedUi"), ui.value(QStringLiteral("uiToken"))},
                                                                               {QStringLiteral("controlId"), parentControl.value(QStringLiteral("id"))},
                                                                               {QStringLiteral("value"), 67.5}});
        QCoreApplication::processEvents();
        const auto job = project.success(QStringLiteral("ui_job_status"), {{QStringLiteral("jobId"), accepted.value(QStringLiteral("jobId"))}})
                             .value(QStringLiteral("job"))
                             .toObject();
        CHECK(job.value(QStringLiteral("status")) == QStringLiteral("returned"));
        CHECK(drag->value() == Approx(67.5));
        CHECK(received == Approx(67.5));
        CHECK(receivedFinal);
        CHECK(receivedUndo);
        CHECK(changes == 1);
    }
}
