/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "bin/model/markerlistmodel.hpp"
#include "core.h"
#include "mainwindow.h"
#include "mcpserver.h"
#include "timeline2/model/clipmodel.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/view/timelinecontroller.h"
#include "timeline2/view/timelinewidget.h"
#include <algorithm>

bool KdenliveMcpServer::timelineConsistent(const std::shared_ptr<TimelineItemModel> &timeline)
{
    if (!timeline) return false;
    // The native diagnostic accepts snap points not included in its basic
    // item-boundary accounting. The real editor also registers source markers,
    // guide ranges and the controller's ruler/multicamera zones.
    std::vector<int> extra;
    if (const auto guides = timeline->getGuideModel()) extra = guides->getSnapPoints();
    for (const auto &entry : timeline->m_allClips) {
        const auto &clip = entry.second;
        if (clip->getCurrentTrackId() < 0) continue;
        std::vector<int> points;
        clip->allSnaps(points);
        const auto removeOne = [&](int point) {
            const auto found = std::find(points.begin(), points.end(), point);
            if (found != points.end()) points.erase(found);
        };
        removeOne(clip->getPosition());
        removeOne(clip->getPosition() + clip->getPlaytime());
        if (timeline->getMixDuration(entry.first) > timeline->getMixCutPos(entry.first))
            removeOne(clip->getPosition() + timeline->getMixDuration(entry.first) - timeline->getMixCutPos(entry.first));
        extra.insert(extra.end(), points.begin(), points.end());
    }
    if (pCore && pCore->window()) {
        if (const auto widget = pCore->window()->getTimeline(timeline->uuid()); widget && widget->model() == timeline) {
            const auto controller = widget->controller();
            if (controller->zoneIn() > 0) extra.push_back(controller->zoneIn());
            if (controller->zoneOut() > 0) extra.push_back(controller->zoneOut() - 1);
            if (controller->multicamIn != -1) extra.push_back(controller->multicamIn);
        }
    }
    return timeline->checkConsistency(extra);
}
