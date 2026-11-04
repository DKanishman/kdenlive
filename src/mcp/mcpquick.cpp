/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcpquick.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "mcphelpers.h"
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QCursor>
#include <QHash>
#include <QItemSelectionModel>
#include <QJSValue>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QValidator>
#include <QVariant>
#include <QVector>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <memory>

namespace {
constexpr int MaxObjects = 30000;
constexpr int MaxControls = 4096;
constexpr int MaxText = 16384;
constexpr int MaxJobs = 64;
QString k(const char *text)
{
    return QString::fromLatin1(text);
}
QString hash(const QJsonValue &value)
{
    QJsonDocument doc;
    if (value.isObject())
        doc.setObject(value.toObject());
    else if (value.isArray())
        doc.setArray(value.toArray());
    else
        doc.setArray(QJsonArray{value});
    return QString::fromLatin1(QCryptographicHash::hash(doc.toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
QObject *asObject(const QVariant &value)
{
    if (value.canConvert<QObject *>()) return value.value<QObject *>();
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        const auto script = value.value<QJSValue>();
        if (script.isQObject()) return script.toQObject();
    }
    return nullptr;
}
QVariant modelVariant(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QJSValue>()) return value.value<QJSValue>().toVariant();
    return value;
}
QJsonValue boundedModelValue(const QVariant &input, bool &ok, int &remaining, int depth = 0)
{
    if (!ok || depth > 4 || remaining <= 0) {
        ok = false;
        return {};
    }
    const auto value = modelVariant(input);
    const auto type = value.metaType().id();
    if (type == QMetaType::QString) {
        const auto text = value.toString();
        remaining -= text.size();
        if (text.size() > MaxText || remaining < 0) {
            ok = false;
            return {};
        }
        return text;
    }
    if (type == QMetaType::Bool) return value.toBool();
    if (type == QMetaType::LongLong || type == QMetaType::ULongLong)
        return QJsonObject{{k("integerType"), type == QMetaType::LongLong ? k("signed64") : k("unsigned64")}, {k("decimal"), value.toString()}};
    if (type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::Double) {
        const double number = value.toDouble();
        if (!std::isfinite(number)) {
            ok = false;
            return {};
        }
        return number;
    }
    if (type == QMetaType::QVariantMap) {
        const auto map = value.toMap();
        if (map.size() > 32) {
            ok = false;
            return {};
        }
        QJsonObject result;
        for (auto it = map.begin(); it != map.end(); ++it) {
            remaining -= it.key().size();
            result.insert(it.key(), boundedModelValue(it.value(), ok, remaining, depth + 1));
        }
        return result;
    }
    if (type == QMetaType::QVariantList || type == QMetaType::QStringList) {
        const auto list = value.toList();
        if (list.size() > 32) {
            ok = false;
            return {};
        }
        QJsonArray result;
        for (const auto &part : list)
            result.append(boundedModelValue(part, ok, remaining, depth + 1));
        return result;
    }
    if (!value.isValid() || value.isNull()) return QJsonValue(QJsonValue::Null);
    ok = false;
    return {};
}
QJsonArray indexPath(QModelIndex index)
{
    QList<int> rows;
    while (index.isValid()) {
        rows.prepend(index.row());
        index = index.parent();
    }
    QJsonArray result;
    for (int row : rows)
        result.append(row);
    return result;
}
bool isWithin(QQuickItem *item, QQuickItem *ancestor)
{
    for (auto *current = item; current; current = current->parentItem())
        if (current == ancestor) return true;
    return false;
}
bool invoke0(QObject *object, const char *signature, QMetaMethod::MethodType type)
{
    if (!object) return false;
    const int index = object->metaObject()->indexOfMethod(signature);
    if (index < 0) return false;
    const auto method = object->metaObject()->method(index);
    return method.methodType() == type && method.parameterCount() == 0 && method.invoke(object, Qt::DirectConnection);
}
bool signal0(QObject *object, const char *signature)
{
    return invoke0(object, signature, QMetaMethod::Signal);
}
bool signalInt(QObject *object, const char *signature, int value)
{
    if (!object) return false;
    const int index = object->metaObject()->indexOfMethod(signature);
    if (index < 0) return false;
    const auto method = object->metaObject()->method(index);
    return method.methodType() == QMetaMethod::Signal && method.parameterCount() == 1 && method.invoke(object, Qt::DirectConnection, Q_ARG(int, value));
}
bool writable(QObject *object, const char *property)
{
    const int index = object->metaObject()->indexOfProperty(property);
    return index >= 0 && object->metaObject()->property(index).isWritable();
}
enum class Kind { Unknown, Button, Spin, Slider, Combo, Input, TextEdit, View, Label, Popup, Menu, MenuBar, MenuAction, NativeView };
QString kindName(Kind kind)
{
    switch (kind) {
    case Kind::Button:
        return k("button");
    case Kind::Spin:
        return k("spin_box");
    case Kind::Slider:
        return k("slider");
    case Kind::Combo:
        return k("combo_box");
    case Kind::Input:
        return k("text_input");
    case Kind::TextEdit:
        return k("text_edit");
    case Kind::View:
        return k("item_view");
    case Kind::Label:
        return k("label");
    case Kind::Popup:
        return k("popup");
    case Kind::Menu:
        return k("menu");
    case Kind::MenuBar:
        return k("menu_bar");
    case Kind::MenuAction:
        return k("menu_action");
    case Kind::NativeView:
        return k("native_item_view");
    default:
        return k("unknown");
    }
}
Kind classify(QObject *object)
{
    if (qobject_cast<QAction *>(object)) return Kind::MenuAction;
    if (qobject_cast<QMenu *>(object)) return Kind::Menu;
    if (qobject_cast<QMenuBar *>(object)) return Kind::MenuBar;
    if (qobject_cast<QAbstractItemView *>(object)) return Kind::NativeView;
    // Runtime Qt class identities; neither private C++ headers nor client-provided class names are used.
    if (object->inherits("QQuickPopup")) return Kind::Popup;
    if (object->inherits("QQuickAbstractButton")) return Kind::Button;
    if (object->inherits("QQuickSpinBox")) return Kind::Spin;
    if (object->inherits("QQuickSlider")) return Kind::Slider;
    if (object->inherits("QQuickComboBox")) return Kind::Combo;
    if (object->inherits("QQuickTextInput")) return Kind::Input;
    if (object->inherits("QQuickTextEdit")) return Kind::TextEdit;
    if (object->inherits("QQuickItemView")) return Kind::View;
    if (object->inherits("QQuickText")) return Kind::Label;
    return Kind::Unknown;
}
struct Control
{
    QPointer<QObject> object;
    QPointer<QWidget> host;
    Kind kind{Kind::Unknown};
    QVector<QPointer<QMenu>> menuOwners;
    bool listed{false};
};
struct Model
{
    QPointer<QAbstractItemModel> object;
    QString id;
    quint64 revision{0};
};
struct ModelInfo
{
    QString token;
    QPointer<QAbstractItemModel> object;
    QVariantList values;
    int count{0};
    bool supported{false};
};
class Bridge final : public QObject
{
public:
    explicit Bridge(QObject *owner)
        : QObject(owner)
    {
        setObjectName(k("kdenlive-native-mcp-quick-controls"));
    }
    std::function<QJsonObject()> stateReader;
    QJsonObject snapshot(const QJsonObject &nativeState, bool interactionOnly = false);
    QJsonObject call(const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState);

private:
    QHash<QString, Control> controls;
    QHash<QObject *, QString> ids;
    QHash<QAbstractItemModel *, Model> models;
    QHash<QString, QJsonObject> jobs;
    quint64 counter{0}, revision{0};
    bool truncated{false};
    QJsonArray modalPopups;
    QString identify(QObject *object, QWidget *host = nullptr);
    QString registerModel(QAbstractItemModel *model);
    ModelInfo modelInfo(QObject *object);
    bool eligible(Control &control) const;
    bool modalScope(const Control &control) const;
    QJsonObject describe(const QString &id, Control &control);
    bool validateValue(Control &control, const QJsonObject &arguments, QString &error);
    bool applyValue(Control &control, const QJsonObject &arguments, QString &error);
    bool activate(Control &control, const QString &operation, QString &error);
    void closeActionMenus(Control &control);
};
QString Bridge::identify(QObject *object, QWidget *host)
{
    if (!object) return {};
    auto found = ids.constFind(object);
    if (found != ids.cend()) {
        auto &control = controls[*found];
        if (host) control.host = host;
        return *found;
    }
    if (controls.size() >= MaxControls) {
        truncated = true;
        return {};
    }
    const QString id = k("quick-") + QString::number(++counter);
    Control control;
    control.object = object;
    control.host = host;
    control.kind = classify(object);
    controls.insert(id, control);
    ids.insert(object, id);
    connect(object, &QObject::destroyed, this, [this, object, id] {
        controls.remove(id);
        ids.remove(object);
    });
    return id;
}
QString Bridge::registerModel(QAbstractItemModel *model)
{
    if (!model) return {};
    if (!models.contains(model)) {
        Model value;
        value.object = model;
        value.id = k("quick-model-") + QString::number(++counter);
        models.insert(model, value);
        auto changed = [this, model] {
            auto found = models.find(model);
            if (found != models.end()) ++found->revision;
        };
        connect(model, &QAbstractItemModel::dataChanged, this, changed);
        connect(model, &QAbstractItemModel::modelReset, this, changed);
        connect(model, &QAbstractItemModel::layoutChanged, this, changed);
        connect(model, &QAbstractItemModel::rowsInserted, this, changed);
        connect(model, &QAbstractItemModel::rowsRemoved, this, changed);
        connect(model, &QAbstractItemModel::rowsMoved, this, changed);
        connect(model, &QAbstractItemModel::columnsInserted, this, changed);
        connect(model, &QAbstractItemModel::columnsRemoved, this, changed);
        connect(model, &QAbstractItemModel::columnsMoved, this, changed);
        connect(model, &QObject::destroyed, this, [this, model] { models.remove(model); });
    }
    const auto value = models.value(model);
    return value.id + QLatin1Char('-') + QString::number(value.revision);
}
ModelInfo Bridge::modelInfo(QObject *object)
{
    ModelInfo info;
    if (auto *view = qobject_cast<QAbstractItemView *>(object)) {
        info.object = view->model();
        info.count = view->model() ? view->model()->rowCount() : 0;
    } else {
        const QVariant source = object->property("model");
        if (auto *model = qobject_cast<QAbstractItemModel *>(asObject(source)))
            info.object = model;
        else {
            const auto value = modelVariant(source);
            if (value.metaType() == QMetaType::fromType<QStringList>()) {
                for (const auto &text : value.toStringList())
                    info.values.append(text);
            } else if (value.metaType() == QMetaType::fromType<QVariantList>())
                info.values = value.toList();
            else if (value.metaType().id() == QMetaType::Int && value.toInt() >= 0 && value.toInt() <= 1000000) {
                info.count = value.toInt();
                info.supported = true;
                info.token = k("count-model-") + QString::number(info.count);
                return info;
            } else
                return info;
        }
        info.count = object->property("count").toInt();
    }
    if (info.object) {
        if (info.object->thread() != qApp->thread()) return info;
        info.supported = true;
        info.token = registerModel(info.object) + QLatin1Char('-') + QString::number(info.count);
    } else if (info.values.size() <= 10000) {
        bool safe = true;
        int remaining = 256 * 1024;
        QJsonArray identity;
        for (const auto &value : info.values)
            identity.append(boundedModelValue(value, safe, remaining));
        info.supported = safe;
        info.count = info.values.size();
        if (safe) info.token = k("list-model-") + hash(identity);
    }
    return info;
}

bool Bridge::eligible(Control &control) const
{
    if (!control.object || !control.listed) return false;
    if (control.kind == Kind::MenuAction) {
        auto *action = qobject_cast<QAction *>(control.object.data());
        if (!action || !action->isEnabled() || !action->isVisible() || action->isSeparator()) return false;
        for (const auto &menu : control.menuOwners)
            if (menu && (menu->isVisible() || (control.host && control.host->isVisible()))) return true;
        return false;
    }
    if (auto *widget = qobject_cast<QWidget *>(control.object.data()))
        return widget->isEnabled() && (widget->isVisible() || (control.kind == Kind::Menu && control.host && control.host->isVisible()));
    if (control.host && (!control.host->isVisible() || !control.host->isEnabled())) return false;
    if (auto *item = qobject_cast<QQuickItem *>(control.object.data()))
        return item->isVisible() && item->isEnabled() && (control.host || (item->window() && item->window()->isVisible()));
    if (control.kind == Kind::Popup) {
        auto *parentItem = qobject_cast<QQuickItem *>(asObject(control.object->property("parent")));
        return control.object->property("enabled").toBool() && parentItem && parentItem->isEnabled() &&
               (control.host || (parentItem->window() && parentItem->window()->isVisible()));
    }
    return false;
}
bool Bridge::modalScope(const Control &control) const
{
    if (auto *modal = QApplication::activeModalWidget()) {
        auto *widget = qobject_cast<QWidget *>(control.object.data());
        if (control.host ? control.host->window() != modal : !widget || (widget != modal && !modal->isAncestorOf(widget))) return false;
    }
    for (const auto &value : modalPopups) {
        const auto popup = controls.value(value.toString()).object;
        if (!popup || popup == control.object) continue;
        auto *content = qobject_cast<QQuickItem *>(asObject(popup->property("contentItem")));
        auto *item = qobject_cast<QQuickItem *>(control.object.data());
        if (!item && control.kind == Kind::Popup) item = qobject_cast<QQuickItem *>(asObject(control.object->property("parent")));
        if (!content || !isWithin(item, content)) return false;
    }
    return true;
}
QJsonObject Bridge::describe(const QString &id, Control &control)
{
    auto *object = control.object.data();
    QJsonObject data{{k("id"), id},
                     {k("objectName"), object->objectName()},
                     {k("class"), QString::fromLatin1(object->metaObject()->className())},
                     {k("kind"), kindName(control.kind)},
                     {k("enabled"), eligible(control)},
                     {k("withinModalScope"), modalScope(control)}};
    if (control.host) data.insert(k("hostName"), control.host->objectName());
    QJsonArray operations;
    if (control.kind == Kind::MenuAction) {
        auto *action = qobject_cast<QAction *>(object);
        data.insert(k("text"), action->text().left(4096));
        data.insert(k("checked"), action->isChecked());
        data.insert(k("checkable"), action->isCheckable());
        data.insert(k("separator"), action->isSeparator());
        QStringList parentIds;
        for (const auto &menu : control.menuOwners)
            if (menu) parentIds.append(identify(menu));
        std::sort(parentIds.begin(), parentIds.end());
        QJsonArray parents;
        for (const auto &parentId : parentIds)
            parents.append(parentId);
        data.insert(k("menus"), parents);
        if (action->menu()) {
            operations.append(k("open"));
            data.insert(k("submenuId"), identify(action->menu(), control.host));
        } else
            operations.append(k("trigger"));
        data.insert(k("writable"), action->isCheckable());
        data.insert(k("value"), action->isChecked());
    } else if (control.kind == Kind::Menu || control.kind == Kind::MenuBar) {
        auto *widget = qobject_cast<QWidget *>(object);
        QJsonArray actions;
        for (auto *action : widget->actions()) {
            const auto aid = identify(action, control.host);
            if (!aid.isEmpty()) actions.append(aid);
        }
        data.insert(k("actions"), actions);
        data.insert(k("visible"), widget->isVisible());
        data.insert(k("writable"), false);
        if (control.kind == Kind::Menu) {
            operations.append(k("open"));
            operations.append(k("close"));
        }
    } else if (control.kind == Kind::NativeView) {
        auto *view = qobject_cast<QAbstractItemView *>(object);
        const auto model = modelInfo(object);
        data.insert(k("modelToken"), model.token);
        data.insert(k("count"), model.count);
        data.insert(k("currentPath"), indexPath(view->currentIndex()));
        QJsonArray selection;
        if (view->selectionModel())
            for (const auto &index : view->selectionModel()->selectedRows())
                selection.append(indexPath(index));
        data.insert(k("selectedPaths"), selection);
        data.insert(k("writable"), false);
        if (view->contextMenuPolicy() == Qt::CustomContextMenu) operations.append(k("context_menu"));
    } else if (control.kind == Kind::Popup) {
        const bool visible = object->property("visible").toBool();
        data.insert(k("visible"), visible);
        data.insert(k("modal"), object->property("modal").toBool());
        data.insert(k("writable"), false);
        operations.append(visible ? k("close") : k("open"));
    } else {
        data.insert(k("visible"), object->property("visible").toBool());
        data.insert(k("activeFocus"), object->property("activeFocus").toBool());
        operations.append(k("focus"));
        bool canWrite = false;
        if (control.kind == Kind::Button) {
            data.insert(k("text"), object->property("text").toString().left(4096));
            canWrite = object->property("checkable").toBool();
            data.insert(k("checkable"), canWrite);
            data.insert(k("value"), object->property("checked").toBool());
            if (object->metaObject()->indexOfMethod("click()") >= 0) operations.append(k("click"));
        } else if (control.kind == Kind::Input || control.kind == Kind::TextEdit || control.kind == Kind::Label) {
            const bool secret = control.kind == Kind::Input && object->property("echoMode").toInt() != 0;
            const auto text = object->property("text").toString();
            data.insert(k("value"), secret ? k("[redacted]") : text.left(4096));
            data.insert(k("secret"), secret);
            if (!secret) {
                data.insert(k("valueHash"), hash(text));
                data.insert(k("truncated"), text.size() > 4096);
                if (control.kind == Kind::Input || control.kind == Kind::TextEdit) {
                    data.insert(k("cursorPosition"), object->property("cursorPosition").toInt());
                    data.insert(k("selectionStart"), object->property("selectionStart").toInt());
                    data.insert(k("selectionEnd"), object->property("selectionEnd").toInt());
                }
            }
            canWrite = control.kind != Kind::Label && !secret && !object->property("readOnly").toBool() && writable(object, "text");
            data.insert(k("readOnly"), !canWrite);
            if (control.kind == Kind::Input) {
                data.insert(k("maximumLength"), object->property("maximumLength").toInt());
                data.insert(k("inputMask"), object->property("inputMask").toString());
                const auto validator = asObject(object->property("validator"));
                data.insert(k("validator"), validator ? QString::fromLatin1(validator->metaObject()->className()) : QString());
            }
        } else if (control.kind == Kind::Spin || control.kind == Kind::Slider) {
            const double from = object->property("from").toDouble(), to = object->property("to").toDouble();
            data.insert(k("minimum"), std::min(from, to));
            data.insert(k("maximum"), std::max(from, to));
            data.insert(k("stepSize"), object->property("stepSize").toDouble());
            data.insert(k("value"),
                        control.kind == Kind::Spin ? QJsonValue(object->property("value").toInt()) : QJsonValue(object->property("value").toDouble()));
            canWrite = writable(object, "value");
        } else if (control.kind == Kind::Combo || control.kind == Kind::View) {
            const auto model = modelInfo(object);
            data.insert(k("modelToken"), model.token);
            data.insert(k("count"), model.count);
            data.insert(k("modelSupported"), model.supported);
            data.insert(k("value"), object->property("currentIndex").toInt());
            if (control.kind == Kind::Combo) {
                data.insert(k("text"), object->property("currentText").toString().left(4096));
                operations.append(k("open"));
            }
            canWrite = model.supported && writable(object, "currentIndex");
        }
        data.insert(k("writable"), canWrite);
    }
    data.insert(k("operations"), operations);
    return data;
}
QJsonObject Bridge::snapshot(const QJsonObject &nativeState, bool interactionOnly)
{
    truncated = false;
    modalPopups = QJsonArray();
    for (auto it = controls.begin(); it != controls.end(); ++it) {
        it->listed = false;
        it->menuOwners.clear();
    }
    struct Visit
    {
        QPointer<QObject> object;
        QPointer<QWidget> host;
        int depth;
    };
    QVector<Visit> queue;
    QSet<QQuickWindow *> embedded;
    for (auto *widget : QApplication::allWidgets()) {
        if (auto *quick = qobject_cast<QQuickWidget *>(widget)) {
            embedded.insert(quick->quickWindow());
            if (quick->rootObject()) queue.append({quick->rootObject(), quick, 0});
        }
    }
    for (auto *window : QApplication::allWindows())
        if (auto *quick = qobject_cast<QQuickWindow *>(window))
            if (!embedded.contains(quick)) queue.append({quick->contentItem(), nullptr, 0});
    QHash<QObject *, bool> visited;
    int cursor = 0;
    while (cursor < queue.size() && visited.size() < MaxObjects) {
        const auto entry = queue.at(cursor++);
        auto *object = entry.object.data();
        if (!object || entry.depth > 64) continue;
        if (visited.contains(object) && (visited.value(object) || !entry.host)) continue;
        visited.insert(object, bool(entry.host));
        const Kind kind = classify(object);
        auto *item = qobject_cast<QQuickItem *>(object);
        if (kind != Kind::Unknown && (kind == Kind::Popup || (item && item->isVisible() && (entry.host || (item->window() && item->window()->isVisible()))))) {
            const auto id = identify(object, entry.host);
            if (!id.isEmpty()) {
                auto &control = controls[id];
                control.listed = true;
                if (kind == Kind::Popup && object->property("visible").toBool() && object->property("modal").toBool() && eligible(control))
                    modalPopups.append(id);
            }
        }
        // Closed popup content may be deferred QML: observing it must not instantiate its editor handlers.
        if (kind == Kind::Popup && object->property("visible").toBool())
            if (auto *content = asObject(object->property("contentItem"))) queue.append({content, entry.host, entry.depth + 1});
        if (item)
            for (auto *child : item->childItems())
                queue.append({child, entry.host, entry.depth + 1});
        for (auto *child : object->children())
            queue.append({child, entry.host, entry.depth + 1});
    }
    if (cursor < queue.size()) truncated = true;
    int menuSteps = 0;
    std::function<void(QMenu *, QWidget *, int, const QVector<QPointer<QMenu>> &)> addMenu = [&](QMenu *menu, QWidget *host, int depth,
                                                                                                 const QVector<QPointer<QMenu>> &ancestors) {
        // Shared submenus must accumulate every root-to-leaf path. A global
        // visited set loses ancestors when a submenu is enumerated first.
        if (!menu || ancestors.contains(menu)) return;
        if (depth > 32 || ++menuSteps > MaxObjects) {
            truncated = true;
            return;
        }
        const auto id = identify(menu, host);
        if (id.isEmpty()) return;
        controls[id].listed = true;
        QVector<QPointer<QMenu>> lineage{menu};
        lineage += ancestors;
        for (auto *action : menu->actions()) {
            if (++menuSteps > MaxObjects) {
                truncated = true;
                break;
            }
            const auto aid = identify(action, host);
            if (aid.isEmpty()) break;
            auto &control = controls[aid];
            control.listed = true;
            for (const auto &owner : lineage)
                if (!control.menuOwners.contains(owner)) control.menuOwners.append(owner);
            if (action->menu()) addMenu(action->menu(), host, depth + 1, lineage);
        }
    };
    for (auto *widget : QApplication::allWidgets()) {
        if (auto *bar = qobject_cast<QMenuBar *>(widget); bar && bar->isVisible()) {
            const auto id = identify(bar, bar);
            if (!id.isEmpty()) controls[id].listed = true;
            for (auto *action : bar->actions()) {
                if (action->menu()) addMenu(action->menu(), bar, 0, {});
                const auto aid = identify(action, bar);
                if (!aid.isEmpty()) {
                    controls[aid].listed = true;
                    if (action->menu()) controls[aid].menuOwners.append(action->menu());
                }
            }
        }
    }
    // Revisit visible contexts too: native popup enumeration has no ordering
    // guarantee, and each visit merges its complete ancestor path.
    for (auto *widget : QApplication::allWidgets()) {
        if (auto *menu = qobject_cast<QMenu *>(widget); menu && menu->isVisible()) addMenu(menu, menu->parentWidget(), 0, {});
        if (auto *view = qobject_cast<QAbstractItemView *>(widget); view && view->isVisible() && view->contextMenuPolicy() == Qt::CustomContextMenu) {
            const auto id = identify(view, view);
            if (!id.isEmpty()) controls[id].listed = true;
        }
    }
    QStringList keys;
    for (auto it = controls.cbegin(); it != controls.cend(); ++it)
        if (it->listed && it->object) keys.append(it.key());
    std::sort(keys.begin(), keys.end());
    QJsonArray values;
    for (const auto &id : keys) {
        auto found = controls.find(id);
        if (found != controls.end() && found->object) {
            Control copy = *found;
            values.append(describe(id, copy));
        }
    }
    QJsonObject result{{k("controls"), values},
                       {k("revision"), QString::number(revision)},
                       {k("truncated"), truncated},
                       {k("nativeStateToken"), nativeState.value(k("expectedState"))},
                       {k("nativeReady"), nativeState.value(k("ready"))},
                       {k("widgetUiToken"), nativeState.value(k("_widgetUiToken"))},
                       {k("nativeDocumentId"), !interactionOnly && pCore && pCore->currentDoc() ? pCore->currentDoc()->uuid().toString() : QString()},
                       {k("modalPopupIds"), modalPopups},
                       {k("needsUserInput"), QApplication::activeModalWidget() || QApplication::activePopupWidget() || !modalPopups.isEmpty()}};
    if (interactionOnly) {
        result.remove(k("nativeStateToken"));
        result.remove(k("nativeReady"));
        result.remove(k("nativeDocumentId"));
        result.remove(k("widgetUiToken"));
        result.insert(k("needsUserInput"), !modalPopups.isEmpty());
    } else {
        auto *focus = QApplication::focusWidget();
        result.insert(k("widgetFocus"), focus ? identify(focus, focus) : QString());
    }
    auto *window = qobject_cast<QQuickWindow *>(QApplication::focusWindow());
    result.insert(k("quickFocus"), window && window->activeFocusItem() ? identify(window->activeFocusItem()) : QString());
    if (!interactionOnly) {
        auto *modal = QApplication::activeModalWidget();
        result.insert(k("modalWidget"), modal ? identify(modal, modal) : QString());
    }
    result.insert(k("truncated"), truncated);
    result.insert(k("quickToken"), hash(result));
    return result;
}

bool Bridge::validateValue(Control &control, const QJsonObject &arguments, QString &error)
{
    const auto value = arguments.value(k("value"));
    auto *object = control.object.data();
    const bool commit = arguments.value(k("commit")).toBool(true);
    if (arguments.contains(k("commit")) && !arguments.value(k("commit")).isBool()) {
        error = k("commit must be boolean.");
        return false;
    }
    if (control.kind == Kind::MenuAction) {
        auto *action = qobject_cast<QAction *>(object);
        if (action && action->isCheckable() && !action->menu() && value.isBool()) return true;
    } else if (control.kind == Kind::Button) {
        if (object->property("checkable").toBool() && value.isBool() && object->metaObject()->indexOfMethod("click()") >= 0) return true;
    } else if (control.kind == Kind::Input || control.kind == Kind::TextEdit) {
        if (!value.isString() || value.toString().size() > MaxText || object->property("readOnly").toBool() || !writable(object, "text")) {
            error = k("Text must fit an enabled editable field and the 16384-character bound.");
            return false;
        }
        if (object->metaObject()->indexOfMethod("textEdited()") < 0 || (commit && object->metaObject()->indexOfMethod("editingFinished()") < 0)) {
            error = k("This Qt control does not expose its normal edit/commit signals.");
            return false;
        }
        if (control.kind == Kind::Input) {
            if (object->property("echoMode").toInt() != 0) {
                error = k("Secret fields cannot be read or changed by this bridge.");
                return false;
            }
            const int maximum = object->property("maximumLength").toInt();
            if (maximum < 0 || value.toString().size() > maximum) {
                error = k("Text exceeds the native input maximum length.");
                return false;
            }
            QLineEdit probe;
            probe.setMaxLength(maximum);
            probe.setInputMask(object->property("inputMask").toString());
            auto *validator = qobject_cast<QValidator *>(asObject(object->property("validator")));
            probe.setValidator(validator);
            probe.setText(value.toString());
            if (probe.text() != value.toString() || (commit && !probe.hasAcceptableInput())) {
                error = k("Text does not satisfy the native validator or input mask.");
                return false;
            }
        }
        return true;
    } else if (control.kind == Kind::Spin || control.kind == Kind::Slider) {
        const double from = object->property("from").toDouble(), to = object->property("to").toDouble();
        double parsed = 0;
        if (writable(object, "value") && std::isfinite(from) && std::isfinite(to) &&
            Mcp::real(arguments, k("value"), std::min(from, to), std::max(from, to), parsed) && (control.kind != Kind::Spin || std::floor(parsed) == parsed))
            return true;
    } else if (control.kind == Kind::Combo || control.kind == Kind::View) {
        const auto model = modelInfo(object);
        int index = 0;
        if (!model.supported) {
            error = k("This model cannot provide a reliable row identity/version through this adapter.");
            return false;
        }
        if (arguments.value(k("expectedModel")).toString() != model.token) {
            error = k("Read the current modelToken before choosing a row.");
            return false;
        }
        if (writable(object, "currentIndex") && Mcp::integer(arguments, k("value"), 0, model.count - 1, index)) return true;
    }
    error = k("Value type or range does not match this control's permitted setter.");
    return false;
}
void Bridge::closeActionMenus(Control &control)
{
    const auto menus = control.menuOwners;
    for (const auto &menu : menus)
        if (menu && menu->isVisible()) menu->hide();
}
bool Bridge::applyValue(Control &control, const QJsonObject &arguments, QString &error)
{
    if (!validateValue(control, arguments, error)) return false;
    QPointer<QObject> object = control.object;
    const auto value = arguments.value(k("value"));
    if (control.kind == Kind::MenuAction) {
        auto *action = qobject_cast<QAction *>(object.data());
        if (action->isChecked() != value.toBool()) {
            closeActionMenus(control);
            if (object) action->trigger();
        }
        if (!object || action->isChecked() != value.toBool()) {
            error = k("Native action rules refused the requested checked state.");
            return false;
        }
        return true;
    }
    if (control.kind == Kind::Button) {
        if (object->property("checked").toBool() != value.toBool() && !invoke0(object, "click()", QMetaMethod::Slot)) {
            error = k("Native click method is unavailable.");
            return false;
        }
        if (!object || object->property("checked").toBool() != value.toBool()) {
            error = k("Native button rules refused the requested checked state.");
            return false;
        }
        return true;
    }
    if (control.kind == Kind::Input || control.kind == Kind::TextEdit) {
        const bool changed = object->property("text").toString() != value.toString();
        if (!object->setProperty("text", value.toString())) {
            error = k("Native text setter refused the value.");
            return false;
        }
        if (changed && object && !signal0(object, "textEdited()")) {
            error = k("This Qt version does not expose the normal text-edit signal.");
            return false;
        }
        if (object && arguments.value(k("commit")).toBool(true) && !signal0(object, "editingFinished()")) {
            error = k("The normal commit signal is unavailable.");
            return false;
        }
        return true;
    }
    if (control.kind == Kind::Spin || control.kind == Kind::Slider) {
        const QVariant native = control.kind == Kind::Spin ? QVariant(value.toInt()) : QVariant(value.toDouble());
        if (!object->setProperty("value", native)) {
            error = k("Native numeric setter refused the value.");
            return false;
        }
        if (object && !signal0(object, control.kind == Kind::Spin ? "valueModified()" : "moved()")) {
            error = k("The normal user-change signal is unavailable.");
            return false;
        }
        return true;
    }
    const int index = value.toInt();
    if (!object->setProperty("currentIndex", index)) {
        error = k("Native row selection refused the value.");
        return false;
    }
    if (control.kind == Kind::Combo && object && !signalInt(object, "activated(int)", object->property("currentIndex").toInt())) {
        error = k("The normal combo activation signal is unavailable.");
        return false;
    }
    return true;
}
bool Bridge::activate(Control &control, const QString &operation, QString &error)
{
    QPointer<QObject> object = control.object;
    if (operation == k("focus")) {
        if (auto *item = qobject_cast<QQuickItem *>(object.data())) {
            item->forceActiveFocus(Qt::OtherFocusReason);
            return true;
        }
    } else if (operation == k("click") && control.kind == Kind::Button) {
        if (invoke0(object, "click()", QMetaMethod::Slot)) return true;
    } else if ((operation == k("open") || operation == k("close")) && control.kind == Kind::Popup) {
        if (invoke0(object, operation == k("open") ? "open()" : "close()", QMetaMethod::Slot)) return true;
    } else if (operation == k("open") && control.kind == Kind::Combo) {
        auto *popup = asObject(object->property("popup"));
        if (popup && popup->inherits("QQuickPopup") && invoke0(popup, "open()", QMetaMethod::Slot)) return true;
    } else if (control.kind == Kind::Menu && (operation == k("open") || operation == k("close"))) {
        auto *menu = qobject_cast<QMenu *>(object.data());
        if (operation == k("close"))
            menu->close();
        else
            menu->popup(control.host ? control.host->mapToGlobal(QPoint(0, control.host->height())) : QCursor::pos());
        return true;
    } else if (control.kind == Kind::MenuAction) {
        auto *action = qobject_cast<QAction *>(object.data());
        if (operation == k("open") && action->menu()) {
            auto *menu = action->menu();
            menu->popup(control.host ? control.host->mapToGlobal(QPoint(0, control.host->height())) : QCursor::pos());
            return true;
        }
        if (operation == k("trigger") && !action->menu()) {
            closeActionMenus(control);
            if (!object) {
                error = k("The action disappeared while its menu closed.");
                return false;
            }
            action->trigger();
            return true;
        }
    } else if (operation == k("context_menu") && control.kind == Kind::NativeView) {
        auto *view = qobject_cast<QAbstractItemView *>(object.data());
        if (view->contextMenuPolicy() == Qt::CustomContextMenu) {
            const auto index = view->currentIndex();
            const QPoint point = index.isValid() ? view->visualRect(index).center() : QPoint(0, 0);
            Q_EMIT view->customContextMenuRequested(point);
            return true;
        }
    }
    error = k("This control does not provide the requested fixed native operation.");
    return false;
}

QJsonObject Bridge::call(const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState)
{
    using Mcp::failure;
    using Mcp::toolResult;
    if (name == k("quick_job_status")) {
        const QString id = arguments.value(k("jobId")).toString();
        if (!jobs.contains(id)) return failure(k("UNKNOWN_JOB"), k("Unknown Quick-control job."));
        auto job = jobs.value(id);
        const auto ui = snapshot(nativeState);
        job.insert(k("needsUserInput"), ui.value(k("needsUserInput")));
        return toolResult({{k("ok"), true}, {k("job"), job}});
    }
    auto ui = snapshot(nativeState);
    if (name == k("get_quick_ui")) {
        int offset = 0, limit = 64;
        if ((arguments.contains(k("offset")) && !Mcp::integer(arguments, k("offset"), 0, MaxControls, offset)) ||
            (arguments.contains(k("limit")) && !Mcp::integer(arguments, k("limit"), 1, 64, limit)) ||
            (arguments.contains(k("query")) && !arguments.value(k("query")).isString()))
            return failure(k("INVALID_ARGUMENT"), k("Use a text filter and bounded page offset/limit."));
        const QString query = arguments.value(k("query")).toString();
        QJsonArray filtered;
        for (const auto &value : ui.value(k("controls")).toArray()) {
            const auto c = value.toObject();
            if (query.isEmpty() || c.value(k("objectName")).toString().contains(query, Qt::CaseInsensitive) ||
                c.value(k("class")).toString().contains(query, Qt::CaseInsensitive) || c.value(k("text")).toString().contains(query, Qt::CaseInsensitive))
                filtered.append(c);
        }
        QJsonArray page;
        for (int i = offset; i < std::min(int(filtered.size()), offset + limit); ++i)
            page.append(filtered.at(i));
        ui.insert(k("controls"), page);
        ui.insert(k("total"), filtered.size());
        ui.insert(k("offset"), offset);
        ui.insert(k("hasMore"), qint64(offset) + page.size() < filtered.size());
        return toolResult({{k("ok"), true}, {k("ui"), ui}});
    }
    const QString id = arguments.value(k("controlId")).toString();
    auto found = controls.find(id);
    if (found == controls.end() || !found->object || !found->listed)
        return failure(k("INVALID_CONTROL"), k("Inspect a currently reachable control first."), nativeState);
    if (name == k("get_quick_model")) {
        const auto info = modelInfo(found->object);
        if (!info.supported) return failure(k("UNSUPPORTED_MODEL"), k("This control's model does not expose stable native rows."), nativeState);
        int offset = 0, limit = 32;
        if ((arguments.contains(k("offset")) && !Mcp::integer(arguments, k("offset"), 0, 1000000, offset)) ||
            (arguments.contains(k("limit")) && !Mcp::integer(arguments, k("limit"), 1, 32, limit)))
            return failure(k("INVALID_ARGUMENT"), k("Use a bounded row page."), nativeState);
        QJsonArray rows;
        for (int row = offset; row < std::min(info.count, offset + limit); ++row) {
            QJsonObject entry{{k("index"), row}};
            if (info.object) {
                const auto index = info.object->index(row, 0);
                entry.insert(k("display"), info.object->data(index, Qt::DisplayRole).toString().left(4096));
                QJsonObject roles;
                const auto names = info.object->roleNames();
                int n = 0;
                for (auto it = names.begin(); it != names.end() && n < 8; ++it, ++n) {
                    const auto value = info.object->data(index, it.key());
                    if (value.canConvert<QString>()) roles.insert(QString::fromLatin1(it.value()), value.toString().left(1024));
                }
                entry.insert(k("roles"), roles);
            } else if (row < info.values.size()) {
                bool safe = true;
                int remaining = 16384;
                const auto value = boundedModelValue(info.values.at(row), safe, remaining);
                if (safe)
                    entry.insert(k("value"), value);
                else
                    entry.insert(k("truncated"), true);
            } else
                entry.insert(k("value"), row);
            rows.append(entry);
        }
        return toolResult({{k("ok"), true},
                           {k("rows"), rows},
                           {k("modelToken"), info.token},
                           {k("quickToken"), ui.value(k("quickToken"))},
                           {k("total"), info.count},
                           {k("hasMore"), qint64(offset) + rows.size() < info.count}});
    }
    if (ui.value(k("truncated")).toBool())
        return failure(k("LIMIT"), k("Discovery was truncated; this adapter cannot safely mutate the incomplete context."), nativeState);
    const QString expected = arguments.value(k("expectedQuick")).toString();
    if (expected.isEmpty() || expected != ui.value(k("quickToken")).toString())
        return failure(k("STALE_QUICK_UI"), k("Read the current Quick UI token before operating a control."), nativeState);
    if (QApplication::mouseButtons() != Qt::NoButton || !eligible(*found) || !modalScope(*found))
        return failure(k("UI_BUSY"), k("Use an enabled control in the active modal context and finish dragging first."), nativeState);
    QString error;
    const bool setter = name == k("set_quick_value");
    const QString operation = arguments.value(k("operation")).toString();
    Control preflight = *found;
    if (setter) {
        if (!validateValue(preflight, arguments, error)) return failure(k("INVALID_VALUE"), error, nativeState);
    } else if (name != k("activate_quick_control"))
        return {};
    else if (!describe(id, preflight).value(k("operations")).toArray().contains(operation))
        return failure(k("INVALID_OPERATION"), k("Use one of this control's discovered fixed operations."), nativeState);
    if (jobs.size() >= MaxJobs) {
        for (auto it = jobs.begin(); it != jobs.end();) {
            const auto status = it->value(k("status")).toString();
            if (status == k("returned") || status == k("failed"))
                it = jobs.erase(it);
            else
                ++it;
        }
        if (jobs.size() >= MaxJobs) return failure(k("LIMIT"), k("Too many pending native UI jobs."), nativeState);
    }
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    jobs.insert(jobId, QJsonObject{{k("jobId"), jobId}, {k("status"), k("pending")}, {k("operation"), name}, {k("controlId"), id}});
    QTimer::singleShot(0, this, [this, jobId, id, arguments, expected, setter, operation] {
        auto update = [this, jobId](const QString &status, const QString &message = QString()) {
            auto job = jobs.value(jobId);
            job.insert(k("status"), status);
            if (!message.isEmpty()) job.insert(k("message"), message);
            jobs.insert(jobId, job);
        };
        if (!stateReader) {
            update(k("failed"), k("The native context reader is unavailable."));
            return;
        }
        auto ui = snapshot(stateReader());
        if (ui.value(k("truncated")).toBool() || ui.value(k("quickToken")).toString() != expected) {
            update(k("failed"), k("The native UI/document context changed before execution."));
            return;
        }
        auto target = controls.find(id);
        if (target == controls.end() || !target->object || !eligible(*target) || !modalScope(*target) || QApplication::mouseButtons() != Qt::NoButton) {
            update(k("failed"), k("The control disappeared, was disabled, or left the modal context."));
            return;
        }
        update(k("running"));
        QString error;
        // Copy QPointers before handlers can create/delete controls and invalidate hash iterators.
        Control control = *target;
        const bool applied = setter ? applyValue(control, arguments, error) : activate(control, operation, error);
        ++revision;
        snapshot(stateReader());
        auto job = jobs.value(jobId);
        job.insert(k("status"), applied ? k("returned") : k("failed"));
        if (!error.isEmpty()) job.insert(k("message"), error);
        if (control.object) {
            control.listed = controls.value(id).listed;
            job.insert(k("readback"), describe(id, control));
        } else
            job.insert(k("controlDestroyed"), true);
        job.insert(k("completionMeaning"), k("The native handler returned. Inspect editor state, background jobs and outputs to verify feature completion."));
        jobs.insert(jobId, job);
    });
    return toolResult({{k("ok"), true}, {k("accepted"), true}, {k("jobId"), jobId}, {k("status"), k("pending")}});
}
Bridge *bridge(QObject *owner)
{
    for (auto *child : owner->children())
        if (auto *result = dynamic_cast<Bridge *>(child)) return result;
    return new Bridge(owner);
}
} // namespace
QString McpQuick::interactionFingerprint(QObject *owner)
{
    if (!owner || !qApp || QThread::currentThread() != qApp->thread() || owner->thread() != qApp->thread()) return {};
    return bridge(owner)->snapshot({}, true).value(k("quickToken")).toString();
}
bool McpQuick::modalPopupActive(QObject *owner)
{
    if (!owner || !qApp || QThread::currentThread() != qApp->thread() || owner->thread() != qApp->thread()) return false;
    const auto ui = bridge(owner)->snapshot({}, true);
    return !ui.value(k("modalPopupIds")).toArray().isEmpty() || ui.value(k("truncated")).toBool();
}
QJsonArray McpQuick::toolCatalog()
{
    const auto text = [](const char *d) { return Mcp::property(k("string"), k(d)); };
    const auto integer = [](const char *d) { return Mcp::property(k("integer"), k(d)); };
    const auto expected = text("Exact quickToken from get_quick_ui; bound to native document, selection, focus, controls and model versions.");
    QJsonArray result{Mcp::tool(k("get_quick_ui"),
                                k("Inspect bounded native QtQuick inputs, popups, menus and context-menu targets. Discovery is interactive reachability, not "
                                  "completed feature coverage."),
                                {{k("query"), text("Optional case-insensitive control name/class/text filter.")},
                                 {k("offset"), integer("Page offset, default 0.")},
                                 {k("limit"), integer("Page size 1-64, default 64.")}},
                                {}, true),
                      Mcp::tool(k("get_quick_model"), k("Read a bounded page of a discovered Quick model and obtain its version token for row selection."),
                                {{k("controlId"), text("Discovered native control ID.")},
                                 {k("offset"), integer("First row, default 0.")},
                                 {k("limit"), integer("Row count 1-32, default 32.")}},
                                {k("controlId")}, true),
                      Mcp::tool(k("set_quick_value"),
                                k("Queue a typed native Quick control edit: text, numeric value, checked state or version-bound current row. Normal user-edit "
                                  "signals are delivered; inspect the returned job/readback."),
                                {{k("expectedQuick"), expected},
                                 {k("controlId"), text("Discovered native control ID.")},
                                 {k("value"), QJsonObject{}},
                                 {k("expectedModel"), text("Required for ComboBox/item-view row selection, from get_quick_ui/get_quick_model.")},
                                 {k("commit"), Mcp::property(k("boolean"), k("Commit editable text using its normal editingFinished signal; default true."))}},
                                {k("expectedQuick"), k("controlId"), k("value")}),
                      Mcp::tool(k("activate_quick_control"),
                                k("Queue one discovered fixed native operation: click/focus/open/close/menu trigger/custom context menu. Does not expose "
                                  "arbitrary QObject properties or methods."),
                                {{k("expectedQuick"), expected},
                                 {k("controlId"), text("Discovered native control ID.")},
                                 {k("operation"), text("One of the control's discovered operations.")}},
                                {k("expectedQuick"), k("controlId"), k("operation")}),
                      Mcp::tool(k("quick_job_status"), k("Inspect an accepted native Quick/menu operation and whether a dialog or popup still needs input."),
                                {{k("jobId"), text("Owned Quick-control job ID.")}}, {k("jobId")}, true)};
    for (int i = 0; i < result.size(); ++i) {
        auto tool = result.at(i).toObject();
        auto annotations = tool.value(k("annotations")).toObject();
        if (!annotations.value(k("readOnlyHint")).toBool()) annotations.insert(k("openWorldHint"), true);
        tool.insert(k("annotations"), annotations);
        result[i] = tool;
    }
    return result;
}
QJsonObject McpQuick::callTool(QObject *owner, const QString &name, const QJsonObject &arguments, const QJsonObject &nativeState,
                               const std::function<QJsonObject()> &stateReader)
{
    if (name != k("get_quick_ui") && name != k("get_quick_model") && name != k("set_quick_value") && name != k("activate_quick_control") &&
        name != k("quick_job_status"))
        return {};
    if (!owner || !qApp || QThread::currentThread() != qApp->thread() || owner->thread() != qApp->thread())
        return Mcp::failure(k("GUI_THREAD_REQUIRED"), k("Native interactive control must execute on the owning GUI thread."));
    auto *instance = bridge(owner);
    instance->stateReader = stateReader;
    return instance->call(name, arguments, nativeState);
}
