/*
    SPDX-FileCopyrightText: 2026 Nicolas Carion
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "bin/projectfolder.h"
#include "doc/kdenlivedoc.h"
#include "test_utils.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

TEST_CASE("Bin items resolve to unique source clips", "[Bin]")
{
    auto binModel = pCore->projectItemModel();
    std::shared_ptr<DocUndoStack> undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    pCore->projectManager()->testSetActiveTimeline(document.getTimeline(document.uuid()));

    const QString masterId = KdenliveTests::createAVProducer(pCore->getProjectProfile(), binModel);
    const QString otherId = KdenliveTests::createAVProducer(pCore->getProjectProfile(), binModel);
    QString zoneId;
    REQUIRE(binModel->requestAddBinSubClip(zoneId, 1, 5, {}, masterId));
    QString siblingId;
    REQUIRE(binModel->requestAddBinSubClip(siblingId, 6, 10, {}, masterId));
    QString otherZoneId;
    REQUIRE(binModel->requestAddBinSubClip(otherZoneId, 1, 5, {}, otherId));
    QString folderId;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    REQUIRE(binModel->requestAddFolder(folderId, QStringLiteral("Folder"), binModel->getRootFolder()->clipId(), undo, redo));

    const auto master = binModel->getClipByBinID(masterId);
    const auto other = binModel->getClipByBinID(otherId);
    const auto item = [&](const QString &id) { return binModel->getItemByBinId(id); };
    using Sources = QList<std::shared_ptr<ProjectClip>>;

    CHECK(ProjectItemModel::sourceClips({}).isEmpty());
    CHECK(ProjectItemModel::sourceClips({item(folderId), nullptr}).isEmpty());
    // A selected zone is not a clip itself: it must resolve to its master.
    CHECK(ProjectItemModel::sourceClips({item(zoneId)}) == Sources{master});
    // A master and its zones are one source.
    CHECK(ProjectItemModel::sourceClips({item(zoneId), item(masterId), item(siblingId), item(folderId)}) == Sources{master});
    // Distinct masters keep first-selection order.
    CHECK(ProjectItemModel::sourceClips({item(otherZoneId), item(zoneId), item(otherId)}) == Sources{other, master});
    pCore->projectManager()->closeCurrentDocument(false, false);
}

TEST_CASE("Mask removal preserves exact named entries and their persisted registry", "[Bin][Masks][MCP]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), false));
    pCore->projectManager()->testSetActiveTimeline(document.getTimeline(document.uuid()));
    const auto k = [](const char *text) { return QString::fromLatin1(text); };
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QFile maskFile(directory.filePath(k("shared.mask")));
    REQUIRE(maskFile.open(QIODevice::WriteOnly));
    const QByteArray original("owned fixture data");
    REQUIRE(maskFile.write(original) == original.size());
    maskFile.close();
    const QJsonObject first{{k("name"), k("first")}, {k("file"), maskFile.fileName()}, {k("in"), 2}, {k("out"), 10},
                            {k("includepoints"), k("4=1,2")}, {k("excludepoints"), k("5=3,4")}, {k("boxes"), k("6=1,2,3,4")}};
    auto second = first;
    second.insert(k("name"), k("second"));
    const auto createClip = [&](const QString &data) {
        auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), "color", "red");
        REQUIRE(producer->is_valid());
        producer->set("length", 20);
        producer->set_in_and_out(0, 19);
        producer->set("kdenlive:masks", data.toUtf8().constData());
        QString id;
        Fun undo = [] { return true; }, redo = [] { return true; };
        REQUIRE(binModel->requestAddBinClip(id, producer, binModel->getRootFolder()->clipId(), undo, redo));
        auto clip = binModel->getClipByBinID(id);
        REQUIRE(clip);
        return clip;
    };
    auto clip = createClip(QString::fromUtf8(QJsonDocument(QJsonArray{first, second}).toJson()));
    REQUIRE(clip->masks().size() == 2);
    int changes = 0;
    QJsonArray observedRegistry;
    QObject::connect(clip.get(), &ProjectClip::masksUpdated, &document, [&] {
        ++changes;
        observedRegistry = QJsonDocument::fromJson(clip->getProducerProperty(k("kdenlive:masks")).toUtf8()).array();
    });
    const auto before = clip->getProducerProperty(k("kdenlive:masks"));
    document.setModified(false);
    clip->removeMask(k("unknown"));
    CHECK(changes == 0);
    CHECK_FALSE(document.isModified());
    CHECK(clip->getProducerProperty(k("kdenlive:masks")) == before);

    clip->removeMask(k("second"));
    REQUIRE(clip->masks().size() == 1);
    CHECK(clip->masks().first().maskName == k("first"));
    CHECK(document.isModified());
    CHECK(changes == 1);
    CHECK(observedRegistry == QJsonArray{first});
    auto restored = createClip(clip->getProducerProperty(k("kdenlive:masks")));
    REQUIRE(restored->masks().size() == 1);
    CHECK(restored->masks().first().maskName == k("first"));
    CHECK(restored->masks().first().includepoints == k("4=1,2"));
    CHECK(restored->masks().first().boxes == k("6=1,2,3,4"));

    clip->removeMask(k("first"));
    CHECK(clip->masks().isEmpty());
    CHECK(changes == 2);
    CHECK(observedRegistry.isEmpty());
    auto empty = createClip(clip->getProducerProperty(k("kdenlive:masks")));
    CHECK(empty->masks().isEmpty());
    REQUIRE(maskFile.open(QIODevice::ReadOnly));
    CHECK(maskFile.readAll() == original);
    maskFile.close();
    clip.reset();
    restored.reset();
    empty.reset();
    pCore->projectManager()->closeCurrentDocument(false, false);
}
