/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include <QMap>
#include <QUuid>
#include <memory>

namespace Mcp {
inline QMap<QUuid, QString> liveSequenceClips(const std::shared_ptr<ProjectItemModel> &bin)
{
    QMap<QUuid, QString> result;
    if (!bin) return result;
    // BinPlaylist's cached UUID map can retain deleted/undone sequences. Use
    // current native bin membership and cached clip identity instead; these
    // accessors never enter a waiting producer's loader lock.
    for (const QString &id : bin->getAllClipIds()) {
        const auto clip = bin->getClipByBinID(id);
        if (!clip || clip->clipType() != ClipType::Timeline) continue;
        const QUuid uuid = clip->getSequenceUuid();
        if (!uuid.isNull()) result.insert(uuid, id);
    }
    return result;
}
} // namespace Mcp
