// SPDX-License-Identifier: GPL-3.0-or-later
// Navigation on the map (M5c-3, M5c-4): asking for a route, its preview, drawing it and following it.
#include "MapView.h"

#include "../platform/Platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

/** Route colours: a clear blue with a white casing; passed parts grey while following. */
constexpr uint16_t ROUTE_COLOR = 0x1B3F;
constexpr uint16_t ROUTE_CASING = 0xFFFF;
constexpr uint16_t ROUTE_PASSED = 0x9CF3;
constexpr float ROUTE_WIDTH = 5.0f;
constexpr float ROUTE_CASING_WIDTH = 8.0f;
/** At least this long between two recalculations after leaving the route. */
constexpr uint32_t RECALCULATE_MS = 20000;
constexpr uint32_t ARRIVED_BAR_MS = 8000;
constexpr char CAR_NOTE[] = "Car routes follow road types only, without turn restrictions or access rules.";

double worldOfUnits(int32_t units, int zoom) {
    // Graph units: 4096 per z14 tile; world pixels: 256 per tile at the view zoom
    return std::ldexp(static_cast<double>(units), zoom - 14 - 4);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// GPX replay: positions from a GPX file, one point a second, for testing navigation indoors

bool ReplaySource::load(const char* text, size_t size) {
    count = 0;
    next = 0;
    const char* cursor = text;
    const char* end = text + size;
    while (cursor < end && count < MAX_POINTS) {
        const char* at = strstr(cursor, "<trkpt");
        if (at == nullptr || at >= end) {
            break;
        }
        const char* close = strchr(at, '>');
        if (close == nullptr) {
            break;
        }
        const char* lat = strstr(at, "lat=\"");
        const char* lon = strstr(at, "lon=\"");
        if (lat != nullptr && lon != nullptr && lat < close && lon < close) {
            points[count][0] = strtod(lat + 5, nullptr);
            points[count][1] = strtod(lon + 5, nullptr);
            count++;
        }
        cursor = close + 1;
    }
    return count > 1;
}

PositionRead ReplaySource::read(PositionFix& fix) {
    if (count == 0) {
        return PositionRead::NoFix;
    }
    // The point for the time since the replay started, however often the position is read
    const uint32_t now = nowMs();
    if (next == 0) {
        next = 1;
        lastStep = now;
    }
    const int index = std::min(count - 1, static_cast<int>((now - lastStep) / STEP_MS));
    const int previous = std::max(0, index - 1);
    fix = {};
    fix.latitude = points[index][0];
    fix.longitude = points[index][1];
    fix.accuracy = 5.0f;
    // Course and speed from the step since the last point
    const double dy = (points[index][0] - points[previous][0]) * 111320.0;
    const double dx = (points[index][1] - points[previous][1]) * 111320.0 * std::cos(points[index][0] * 3.14159265358979 / 180.0);
    fix.speed = static_cast<float>(std::sqrt(dx * dx + dy * dy) * 1000.0 / STEP_MS);
    fix.course = fix.speed > 0.1f ? static_cast<float>(std::fmod(std::atan2(dx, dy) * 180.0 / 3.14159265358979 + 360.0, 360.0)) : NAN;
    fix.timeMs = static_cast<int64_t>(now);
    return PositionRead::Fix;
}

void MapView::startReplay(const char* name) {
    if (strcmp(name, "stop") == 0) {
        replay.clear();
        replaying = false;
        showToast("Replay stopped");
        return;
    }
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/%s", rootPath, name);
    FileBuffer file;
    auto read = [&]() { return io.readFile(path, file); };
    if (!runOnCard(io, read) || !replay.load(reinterpret_cast<const char*>(file.bytes()), file.size())) {
        showToast("Replay: no track points in that file");
        return;
    }
    replaying = true;
    logInfo("Replay: %d points from %s", replay.size(), path);
}

// ---------------------------------------------------------------------------------------------------------------

void MapView::navigateTo(double lat, double lon, RouteProfile profile, bool ask) {
    goalLat = lat;
    goalLon = lon;
    routeProfile = profile;
    sheet.close();
    if (position.hasFix() && position.state() == PositionState::Precise) {
        requestRoute(position.fix().latitude, position.fix().longitude, true, nullptr);
    } else if (!ask) {
        requestRoute(worldYToLat(centerY, zoom), worldXToLon(centerX, zoom), false, "No position: the route starts at the screen centre");
    } else {
        openRouteStartChoice();
    }
}

void MapView::requestRoute(double fromLat, double fromLon, bool fromPosition, const char* toast) {
    if (!navigator.request(fromLat, fromLon, goalLat, goalLon, routeProfile)) {
        showToast("A route is being searched already");
        return;
    }
    startLat = fromLat;
    startLon = fromLon;
    startFromPosition = fromPosition;
    recalculating = false;
    routeRequested = true;
    restartRoute = false;
    routeCard.open("", nullptr);
    routeCard.setMode(static_cast<int>(routeProfile));
    routeCard.setBusy(true);
    routeCard.layout(width, height, sheetFont());
    if (toast != nullptr) {
        showToast(toast);
    }
    dirty = true;
}

namespace {

constexpr int START_ON_MAP = 0;
constexpr int START_CENTRE = 1;
constexpr int START_POSITION = 2;

} // namespace

void MapView::openRouteStartChoice() {
    // No GPS fix: where does the route start
    beginPopup(Popup::RouteStart, "Route from");
    menu.add("Choose on the map", input.hasTouch() ? "tap it" : "crosshair");
    popupActions[popupActionCount++] = START_ON_MAP;
    menu.add("Screen centre");
    popupActions[popupActionCount++] = START_CENTRE;
    if (position.hasFix()) {
        // An approximate position (Wi-Fi or IP based): offered with how far off it may be
        char accuracy[24];
        formatDistance(position.fix().accuracy, accuracy, sizeof(accuracy));
        char detail[32];
        snprintf(detail, sizeof(detail), "\xC2\xB1%s", accuracy);
        menu.add("My position", detail);
        popupActions[popupActionCount++] = START_POSITION;
    }
    layoutPopup();
}

void MapView::chooseRouteStart(int choice) {
    switch (choice) {
        case START_ON_MAP:
            beginPickStart();
            break;
        case START_CENTRE:
            requestRoute(worldYToLat(centerY, zoom), worldXToLon(centerX, zoom), false, nullptr);
            break;
        case START_POSITION:
            if (position.hasFix()) {
                requestRoute(position.fix().latitude, position.fix().longitude, true, "Route from your approximate position");
            }
            break;
        default:
            break;
    }
}

void MapView::beginPickStart() {
    pickingStart = true;
    routeCard.close();
    following = false;
    controls.hide();
    // Short: the bar has room for about 35 characters a line on the T-Deck
    pickBar.set(Turn::Arrive, "Where does the route start?",
        input.hasTouch() && input.hasKeyboard() ? "Tap it, or Enter for the +" :
        input.hasTouch()                       ? "Tap it on the map" :
                                                 "Move the map, then Enter");
    dirty = true;
}

void MapView::endPickStart(bool chosen, double lat, double lon) {
    pickingStart = false;
    dirty = true;
    if (chosen) {
        requestRoute(lat, lon, false, nullptr);
    }
}

void MapView::drawPickStart() {
    // The crosshair the start is taken from, and the bar saying what to do
    const auto x = static_cast<float>(width / 2);
    const auto y = static_cast<float>(height / 2);
    canvas.line(x - 9.0f, y, x + 9.0f, y, 4.0f, theme.panel);
    canvas.line(x, y - 9.0f, x, y + 9.0f, 4.0f, theme.panel);
    canvas.line(x - 8.0f, y, x + 8.0f, y, 2.0f, theme.text);
    canvas.line(x, y - 8.0f, x, y + 8.0f, 2.0f, theme.text);
    pickBar.draw(canvas, sheetFont(), theme, width);
}

void MapView::fillRouteCard() {
    navigator.lock();
    const Route& route = navigator.route();
    char distance[16];
    char time[24];
    formatDistance(route.lengthM, distance, sizeof(distance));
    formatDuration(route.seconds, time, sizeof(time));
    const RouteProfile profile = route.profile;
    navigator.unlock();
    char summary[64];
    snprintf(summary, sizeof(summary), "%s \xC2\xB7 %s \xC2\xB7 %s", distance, time, routeProfileName(profile));
    routeCard.open(summary, profile == RouteProfile::Car ? CAR_NOTE : nullptr);
    routeCard.setMode(static_cast<int>(profile));
    routeCard.layout(width, height, sheetFont());
}

void MapView::chooseRouteMode(RouteProfile next) {
    routeProfile = next;
    routeCard.setMode(static_cast<int>(next));
    dirty = true;
    if (navigator.state() == Navigator::State::Searching) {
        // The running search stops; the new one starts when it has (updateNavigation)
        navigator.cancel();
        restartRoute = true;
        routeCard.setBusy(true);
        return;
    }
    searchRouteAgain();
}

void MapView::searchRouteAgain() {
    // The same start again: the position when it was the position, else the point chosen
    if (startFromPosition && position.hasFix()) {
        requestRoute(position.fix().latitude, position.fix().longitude, true, nullptr);
    } else {
        requestRoute(startLat, startLon, false, nullptr);
    }
}

void MapView::fitRoute() {
    navigator.lock();
    const Route& route = navigator.route();
    if (route.isEmpty()) {
        navigator.unlock();
        return;
    }
    int32_t minX = INT32_MAX;
    int32_t minY = INT32_MAX;
    int32_t maxX = INT32_MIN;
    int32_t maxY = INT32_MIN;
    for (const RoutePoint& point : route.points) {
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x);
        maxY = std::max(maxY, point.y);
    }
    navigator.unlock();
    // The largest zoom at which the route's box fits between the mode bar and the card, north up
    const int cardTop = routeCard.isOpen() ? routeCard.bounds().y : height;
    const int barBottom = routeCard.isOpen() ? routeCard.modeBounds().y + routeCard.modeBounds().height : 0;
    const double availableWidth = std::max(40, width - 32);
    const double availableHeight = std::max(40, cardTop - barBottom - 24);
    int fit = 3;
    for (int z = 18; z >= 3; z--) {
        if (worldOfUnits(maxX - minX, z) <= availableWidth && worldOfUnits(maxY - minY, z) <= availableHeight) {
            fit = z;
            break;
        }
    }
    following = false;
    headingUp = false;
    setBearing(0.0f, false);
    double lat;
    double lon;
    routePointTo({ (minX + maxX) / 2, (minY + maxY) / 2 }, lat, lon);
    centerOn(lat, lon, fit);
    // Moved so the route is in the middle of the space between the bar and the card
    centerY += (height - cardTop - barBottom) / 2.0;
    updateWanted();
    dirty = true;
}

void MapView::handleRouteCardButton(RouteCard::Button button) {
    dirty = true;
    switch (button) {
        case RouteCard::Button::Start:
            startNavigation();
            break;
        case RouteCard::Button::Profile:
            // Walk, bike, car in turn: the same destination again (the default stays as Settings has it)
            chooseRouteMode(static_cast<RouteProfile>((static_cast<int>(routeProfile) + 1) % static_cast<int>(RouteProfile::Count)));
            break;
        case RouteCard::Button::Walk:
        case RouteCard::Button::Bike:
        case RouteCard::Button::Car: {
            const auto next = static_cast<RouteProfile>(static_cast<int>(button) - static_cast<int>(RouteCard::Button::Walk));
            // The mode shown already: searched again only when it has no route (failed, or the first search was cancelled)
            if (next != routeProfile || (!routeCard.isBusy() && !routeCard.canStartRoute())) {
                chooseRouteMode(next);
            }
            break;
        }
        case RouteCard::Button::Close:
            routeCard.close();
            break;
        default:
            break;
    }
}

void MapView::startNavigation() {
    routeCard.close();
    navigator.lock();
    follower.begin(navigator.route());
    navigator.unlock();
    navigating = true;
    arrivedAt = 0;
    lastRecalculation = 0;
    navBar.showStop(true);
    layoutControls();
    char instruction[NavigationBar::INSTRUCTION_LENGTH];
    char remaining[NavigationBar::REMAINING_LENGTH];
    navigator.lock();
    follower.instruction(navigator.route(), instruction, sizeof(instruction));
    navigator.unlock();
    follower.remaining(remaining, sizeof(remaining));
    navBar.set(follower.status().nextTurn, instruction, remaining);
    if (!position.hasFix()) {
        navBar.setMessage("Waiting for a GPS position");
    }
    // Heading up while moving, like the second press of the position button
    if (position.hasFix()) {
        following = true;
        headingUp = true;
        zoom = std::max(zoom, 16);
        centerOnPosition();
    }
    dirty = true;
}

void MapView::stopNavigation() {
    navigating = false;
    follower.end();
    headingUp = false;
    navBar.showStop(false);
    layoutControls();
    // The position back in the middle
    if (following && position.hasFix()) {
        centerOnPosition();
    }
    dirty = true;
}

void MapView::clearRoute() {
    stopNavigation();
    routeCard.close();
    navigator.clearRoute();
    showToast("Route cleared");
}

void MapView::updateNavigation(uint32_t now) {
    const Navigator::State state = navigator.state();
    if (state == Navigator::State::Done || state == Navigator::State::Failed) {
        const RouteResult result = navigator.takeResult();
        const bool asked = routeRequested;
        routeRequested = false;
        if (restartRoute) {
            // Another mode was chosen while it searched
            restartRoute = false;
            searchRouteAgain();
        } else if (!asked) {
            // A bench run from a debug script: nothing to show
        } else if (result == RouteResult::Found) {
            if (recalculating && navigating) {
                navigator.lock();
                follower.begin(navigator.route());
                navigator.unlock();
                navBar.setMessage("New route");
            } else {
                fillRouteCard();
                fitRoute();
            }
        } else {
            if (recalculating && navigating) {
                navBar.setMessage(routeResultText(result));
            } else {
                // The card stays with the reason, so another mode can be tried
                routeCard.setBusy(false);
                routeCard.setCanStart(false);
                routeCard.setSummary(routeResultText(result));
                routeCard.setNote(nullptr);
                routeCard.layout(width, height, sheetFont());
                if (!routeCard.isOpen()) {
                    showToast(routeResultText(result));
                }
            }
        }
        recalculating = false;
        dirty = true;
    }
    // Also without a new position: standing still off the route still counts its 10 s
    if (navigating && now - lastFollowAt >= 1000) {
        followRoute(now);
    }
    if (navigating && arrivedAt != 0 && now - arrivedAt >= ARRIVED_BAR_MS) {
        // Arrived: following stops, the line stays until it is cleared
        stopNavigation();
        arrivedAt = 0;
    }
}

void MapView::followRoute(uint32_t now) {
    lastFollowAt = now;
    if (!navigating || !position.hasFix() || arrivedAt != 0) {
        return;
    }
    const PositionFix& fix = position.fix();
    navigator.lock();
    const Route& route = navigator.route();
    if (route.isEmpty()) {
        navigator.unlock();
        stopNavigation();
        return;
    }
    const RouteFollower::Status status = follower.update(route, fix.latitude, fix.longitude, fix.accuracy, now);
    char instruction[NavigationBar::INSTRUCTION_LENGTH];
    follower.instruction(route, instruction, sizeof(instruction));
    navigator.unlock();
    char remaining[NavigationBar::REMAINING_LENGTH];
    follower.remaining(remaining, sizeof(remaining));
    switch (status.state) {
        case RouteFollower::State::Arrived:
            navBar.set(Turn::Arrive, "You have arrived", "");
            following = false;
            headingUp = false;
            arrivedAt = now;
            logInfo("Navigation: arrived");
            break;
        case RouteFollower::State::OffRoute:
            if (!recalculating && (lastRecalculation == 0 || now - lastRecalculation >= RECALCULATE_MS) &&
                navigator.state() == Navigator::State::Idle) {
                lastRecalculation = now;
                recalculating = navigator.request(fix.latitude, fix.longitude, goalLat, goalLon, routeProfile);
                routeRequested = recalculating;
                logInfo("Navigation: off route by %.0f m, recalculating", status.distanceFromRouteM);
            }
            navBar.setMessage("Off route \xE2\x80\x94 recalculating");
            break;
        default:
            if (!recalculating) {
                navBar.set(status.nextTurn, instruction, remaining);
            }
            break;
    }
    // Redrawn when the bar or the state changed, not every second (e-paper)
    const uint32_t signature = (static_cast<uint32_t>(status.state) << 24) ^ static_cast<uint32_t>(status.toTurnM) ^
        (static_cast<uint32_t>(status.remainingM) << 8) ^ static_cast<uint32_t>(status.progressM * 10.0f);
    if (signature != lastFollowSignature) {
        lastFollowSignature = signature;
        dirty = true;
    }
}

void MapView::drawRoute() {
    if (!navigator.hasRoute()) {
        return;
    }
    navigator.lock();
    const Route& route = navigator.route();
    if (route.isEmpty()) {
        navigator.unlock();
        return;
    }
    const float passed = navigating ? follower.status().progressM : -1.0f;
    const bool binary = palette->binary;
    // A dark, thick line on monochrome displays; colour with a casing otherwise
    const uint16_t casing = monochrome ? palette->halo : ROUTE_CASING;
    const uint16_t fill = monochrome ? palette->text : ROUTE_COLOR;
    const uint16_t done = monochrome ? palette->halo : ROUTE_PASSED;
    const float fillWidth = binary ? 6.0f : ROUTE_WIDTH;
    const float casingWidth = binary ? 9.0f : ROUTE_CASING_WIDTH;
    auto project = [&](const RoutePoint& point, float& x, float& y) {
        double sx;
        double sy;
        worldToScreenOffset(worldOfUnits(point.x, zoom) - centerX, worldOfUnits(point.y, zoom) - centerY, sx, sy);
        x = static_cast<float>(sx + width / 2.0);
        y = static_cast<float>(sy + height / 2.0);
    };
    // Two passes: the casing under the whole line, then the line, passed parts grey
    for (int pass = 0; pass < 2; pass++) {
        float lastX;
        float lastY;
        project(route.points[0], lastX, lastY);
        for (size_t i = 1; i < route.points.size(); i++) {
            float x;
            float y;
            project(route.points[i], x, y);
            const float dx = x - lastX;
            const float dy = y - lastY;
            if (dx * dx + dy * dy < 4.0f && i + 1 < route.points.size()) {
                continue;
            }
            const bool visible = !((lastX < -10 && x < -10) || (lastY < -10 && y < -10) || (lastX > width + 10 && x > width + 10) ||
                (lastY > height + 10 && y > height + 10));
            if (visible) {
                if (pass == 0) {
                    canvas.line(lastX, lastY, x, y, casingWidth, casing);
                } else {
                    const bool isPassed = passed >= 0.0f && route.distances[i] <= passed;
                    canvas.line(lastX, lastY, x, y, isPassed && !monochrome ? fillWidth - 1.0f : fillWidth, isPassed ? done : fill);
                }
            }
            lastX = x;
            lastY = y;
        }
    }
    // The destination: a ring at the end
    float goalX;
    float goalY;
    project(route.points[route.points.size() - 1], goalX, goalY);
    navigator.unlock();
    canvas.fillCircle(goalX, goalY, 6.0f, casing);
    canvas.fillCircle(goalX, goalY, 4.0f, fill);
    canvas.fillCircle(goalX, goalY, 1.5f, casing);
}

} // namespace maps
