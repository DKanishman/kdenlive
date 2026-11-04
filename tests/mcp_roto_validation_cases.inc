/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "mcp/mcprotospline.h"
#include "assets/keyframes/model/keyframemodellist.hpp"
#include <mlt++/MltFrame.h>
#include <limits>

namespace {
QJsonArray rotoNativePoint(double x, double y)
{
    QJsonArray point;
    for (int handle = 0; handle < 3; ++handle) point.append(QJsonArray{x, y});
    return point;
}
QJsonArray rotoNativeBox(double first, double last)
{
    QJsonArray points;
    points.append(rotoNativePoint(first, .2));
    points.append(rotoNativePoint(last, .2));
    points.append(rotoNativePoint(last, .8));
    points.append(rotoNativePoint(first, .8));
    return points;
}
std::shared_ptr<EffectItemModel> rotoNativeEffect(McpProject &project, int &clip)
{
    clip = project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.front(), 0, 11))
               .value(QStringLiteral("clipId")).toInt();
    auto args = project.expected();
    args.insert(QStringLiteral("scope"), QStringLiteral("clip"));
    args.insert(QStringLiteral("targetId"), QString::number(clip));
    args.insert(QStringLiteral("operation"), QStringLiteral("add"));
    args.insert(QStringLiteral("effectId"), QStringLiteral("rotoscoping"));
    project.success(QStringLiteral("edit_effect"), args);
    const auto stack = project.timeline->getClipEffectStackModel(clip);
    const auto effect = std::dynamic_pointer_cast<EffectItemModel>(stack->getEffectStackRow(stack->rowCount() - 1));
    REQUIRE(effect);
    REQUIRE(effect->getAssetId() == QStringLiteral("rotoscoping"));
    return effect;
}
QJsonObject rotoParameterArgs(McpProject &project, int clip, int instance, const QJsonValue &value)
{
    auto args = project.expected();
    args.insert(QStringLiteral("scope"), QStringLiteral("clip"));
    args.insert(QStringLiteral("targetId"), QString::number(clip));
    args.insert(QStringLiteral("instanceId"), instance);
    args.insert(QStringLiteral("operation"), QStringLiteral("parameters"));
    args.insert(QStringLiteral("parameters"), QJsonObject{{QStringLiteral("spline"), value}});
    return args;
}
QJsonObject rotoAnimatedArgs(McpProject &project, int clip, int instance, const QJsonArray &keys)
{
    auto args = project.expected();
    args.insert(QStringLiteral("scope"), QStringLiteral("clip"));
    args.insert(QStringLiteral("targetId"), QString::number(clip));
    args.insert(QStringLiteral("instanceId"), instance);
    args.insert(QStringLiteral("keyframes"), keys);
    return args;
}
QJsonObject rotoNativeKey(int frame, const QJsonValue &points)
{
    return {{QStringLiteral("frame"), frame}, {QStringLiteral("values"), QJsonObject{{QStringLiteral("spline"), points}}}};
}
} // namespace

TEST_CASE("MCP refuses malformed native roto properties before effect or history changes", "[MCP][MCPRoto][validation]")
{
    McpProject project;
    project.open();
    int clip = -1;
    const auto effect = rotoNativeEffect(project, clip);
    const auto valid = rotoNativeBox(.1, .4);
    project.success(QStringLiteral("edit_effect"), rotoParameterArgs(project, clip, effect->getId(), valid));
    const auto original = effect->getParam(QStringLiteral("spline"));
    QList<QJsonValue> invalid;
    invalid << QJsonValue(42) << QJsonValue(true) << QJsonValue(QJsonValue::Null);
    QJsonArray wrongHandles;
    wrongHandles.append(QJsonArray{.1, .2});
    invalid.append(wrongHandles);
    QJsonArray wrongCoordinateCount;
    QJsonArray wrongPoint;
    wrongPoint.append(QJsonArray{.1});
    wrongPoint.append(QJsonArray{.1, .2});
    wrongPoint.append(QJsonArray{.1, .2});
    wrongCoordinateCount.append(wrongPoint);
    invalid.append(wrongCoordinateCount);
    for (const QJsonValue coordinate : {QJsonValue(QStringLiteral("0.2")), QJsonValue(false), QJsonValue(QJsonValue::Null),
                                         QJsonValue(std::numeric_limits<double>::infinity()), QJsonValue(4.01), QJsonValue(1e100)}) {
        QJsonArray malformed;
        QJsonArray point;
        point.append(QJsonArray{coordinate, .2});
        point.append(QJsonArray{.1, .2});
        point.append(QJsonArray{.1, .2});
        malformed.append(point);
        invalid.append(malformed);
    }
    QJsonArray misplacedFlags;
    misplacedFlags.append(QStringLiteral("tracked"));
    misplacedFlags.append(QStringLiteral("another flag"));
    invalid.append(misplacedFlags);
    for (const auto &key : {QStringLiteral("-1"), QStringLiteral("1.5"), QStringLiteral("frame2"), QStringLiteral("2147483648"),
                            QString::fromUtf8("٢")})
        invalid.append(QJsonObject{{key, valid}});
    invalid.append(QJsonObject{{QStringLiteral("1"), valid}, {QStringLiteral("01"), valid}});
    invalid.append(QJsonObject{{QStringLiteral("0"), QJsonObject{}}});
    invalid.append(QStringLiteral("{\"0\":[[[NaN,0],[0,0],[0,0]]]}"));
    invalid.append(QStringLiteral("{\"0\":[[[1e999,0],[0,0],[0,0]]]}"));
    QJsonArray tooManyPoints;
    for (int row = 0; row <= McpRoto::MaxPoints; ++row) tooManyPoints.append(rotoNativePoint(.2, .2));
    invalid.append(tooManyPoints);
    QJsonObject tooManyKeys;
    for (int row = 0; row <= McpRoto::MaxKeys; ++row) tooManyKeys.insert(QString::number(row), QJsonArray{});
    invalid.append(tooManyKeys);
    QJsonArray oversizedFlag;
    oversizedFlag.append(QString(McpRoto::MaxPropertyBytes, QLatin1Char('x')));
    invalid.append(oversizedFlag);
    for (int index = 0; index < invalid.size(); ++index) {
        INFO("Malformed native roto property " << index);
        project.rejectWithoutChange(QStringLiteral("edit_effect"), rotoParameterArgs(project, clip, effect->getId(), invalid.at(index)));
        CHECK(effect->getParam(QStringLiteral("spline")) == original);
    }
    const QList<QJsonValue> badKeys{wrongHandles, wrongCoordinateCount, QJsonObject{{QStringLiteral("0"), valid}}, misplacedFlags};
    for (const auto &points : badKeys) {
        QJsonArray keys;
        keys.append(rotoNativeKey(0, valid));
        keys.append(rotoNativeKey(2, points));
        project.rejectWithoutChange(QStringLiteral("set_effect_keyframes"), rotoAnimatedArgs(project, clip, effect->getId(), keys));
        CHECK(effect->getParam(QStringLiteral("spline")) == original);
    }
    for (const auto &interpolation : {QStringLiteral("Discrete"), QStringLiteral("Smooth"), QStringLiteral("CubicIn")}) {
        QJsonArray keys;
        keys.append(rotoNativeKey(0, valid));
        auto eased = rotoNativeKey(2, valid);
        eased.insert(QStringLiteral("interpolation"), interpolation);
        keys.append(eased);
        project.rejectWithoutChange(QStringLiteral("set_effect_keyframes"), rotoAnimatedArgs(project, clip, effect->getId(), keys));
        CHECK(effect->getParam(QStringLiteral("spline")) == original);
    }
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP native roto clears flags topology and chronological keys preserve rendering and undo", "[MCP][MCPRoto][pixels]")
{
    McpProject project;
    project.open();
    int clip = -1;
    const auto effect = rotoNativeEffect(project, clip);
    const auto left = rotoNativeBox(.1, .4), right = rotoNativeBox(.6, .9);
    QJsonArray tracked;
    tracked.append(QStringLiteral("tracked"));
    for (const auto &point : left) tracked.append(point);
    const QList<QJsonValue> valid{QString(), QStringLiteral("   "), QJsonArray{}, QJsonObject{},
                                  QJsonObject{{QStringLiteral("000"), QJsonArray{}}}, left, tracked};
    for (const auto &value : valid) {
        const auto before = effect->getParam(QStringLiteral("spline"));
        project.success(QStringLiteral("edit_effect"), rotoParameterArgs(project, clip, effect->getId(), value));
        project.success(QStringLiteral("undo"), project.expected());
        CHECK(effect->getParam(QStringLiteral("spline")) == before);
    }
    QJsonArray extraPoint = right;
    extraPoint.append(rotoNativePoint(.6, .8));
    QJsonArray keys;
    keys.append(rotoNativeKey(0, left));
    keys.append(rotoNativeKey(2, extraPoint));
    keys.append(rotoNativeKey(10, left));
    const auto before = effect->getParam(QStringLiteral("spline"));
    project.success(QStringLiteral("set_effect_keyframes"), rotoAnimatedArgs(project, clip, effect->getId(), keys));
    const auto property = effect->getParam(QStringLiteral("spline"));
    const auto parsed = QJsonDocument::fromJson(property.toUtf8()).object();
    const QStringList expectedKeys{QStringLiteral("00"), QStringLiteral("02"), QStringLiteral("10")};
    CHECK(parsed.keys() == expectedKeys);
    CHECK(parsed.value(QStringLiteral("02")).toArray().size() == 5);
    effect->prepareKeyframes();
    const auto keyframes = effect->getKeyframeModel();
    REQUIRE(keyframes);
    CHECK(keyframes->getInterpolatedValue(1, effect->getParamIndexFromName(QStringLiteral("spline"))).toList().size() == 4);
    Mlt::Producer producer(pCore->getProjectProfile(), "color", "0xffffffff");
    REQUIRE(producer.is_valid());
    producer.set("length", 11);
    producer.set_in_and_out(0, 10);
    Mlt::Filter native(pCore->getProjectProfile(), "rotoscoping");
    REQUIRE(native.is_valid());
    native.set("spline", property.toUtf8().constData());
    REQUIRE(producer.attach(native) == 0);
    const auto alphaAt = [&](int frameNumber, double x) {
        producer.seek(frameNumber);
        std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
        REQUIRE(frame);
        mlt_image_format format = mlt_image_rgba;
        int width = 64, height = 64;
        const auto pixels = frame->get_image(format, width, height, 1);
        REQUIRE(pixels);
        REQUIRE(format == mlt_image_rgba);
        REQUIRE(width > 0);
        REQUIRE(height > 0);
        return int(pixels[(int(.5 * height) * width + int(x * width)) * 4 + 3]);
    };
    CHECK(alphaAt(0, .2) == 255);
    CHECK(alphaAt(0, .8) == 0);
    CHECK(alphaAt(2, .2) == 0);
    CHECK(alphaAt(2, .8) == 255);
    CHECK(alphaAt(10, .2) == 255);
    CHECK(alphaAt(10, .8) == 0);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(effect->getParam(QStringLiteral("spline")) == before);
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(effect->getParam(QStringLiteral("spline")) == property);
    REQUIRE(project.timeline->checkConsistency());
}