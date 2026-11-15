// SPDX-License-Identifier: GPL-3.0-or-later
#include "MapView.h"
#include "MapViewInternal.h"

#include "../platform/Platform.h"
#include "../render/Compose.h"

#include <algorithm>
#include <cmath>

namespace maps {

void MapView::centerOn(double lat, double lon, int newZoom) {
    zoom = newZoom;
    centerX = lonToWorldX(lon, zoom);
    centerY = latToWorldY(lat, zoom);
    dirty = true;
    updateWanted();
}

void MapView::centerOnPosition() {
    const PositionFix& fix = position.fix();
    zoom = std::clamp(zoom, 0, MAX_VIEW_ZOOM);
    centerX = lonToWorldX(fix.longitude, zoom);
    centerY = latToWorldY(fix.latitude, zoom);
    if (navigating) {
        // The position at NAVIGATION_POSITION_Y of the height: the screen centre is that far ahead of it, in the
        // direction that is up on the (turned) screen
        double aheadX;
        double aheadY;
        screenToWorldOffset(0.0, height / 2.0 - height * NAVIGATION_POSITION_Y, aheadX, aheadY);
        const double worldSize = std::ldexp(static_cast<double>(TILE_SIZE), zoom);
        centerX = std::fmod(centerX + aheadX + worldSize, worldSize);
        centerY = std::fmin(std::fmax(centerY + aheadY, 0.0), worldSize);
    }
    dirty = true;
    updateWanted();
}

void MapView::setZoom(int newZoom, int anchorX, int anchorY) {
    if (newZoom < 0 || newZoom > MAX_VIEW_ZOOM || newZoom == zoom) {
        return;
    }
    // The world point under the anchor stays where it is on the screen
    double offsetX;
    double offsetY;
    screenToWorldOffset(anchorX - width / 2.0, anchorY - height / 2.0, offsetX, offsetY);
    const double scale = std::ldexp(1.0, newZoom - zoom);
    centerX = (centerX + offsetX) * scale - offsetX;
    centerY = (centerY + offsetY) * scale - offsetY;
    zoom = newZoom;
    dirty = true;
    updateWanted();
}

void MapView::zoomBy(int steps, int anchorX, int anchorY) {
    const int target = std::min(std::max(zoom + steps, 0), MAX_VIEW_ZOOM);
    if (target == zoom) {
        return;
    }
    if (!smoothZoom()) {
        animation.active = false;
        io.hold(false);
        setZoom(target, anchorX, anchorY);
        return;
    }
    // Where the animation shows the map now: the current scale, or 1 when none runs. A second press during an
    // animation starts from what is on screen.
    const uint32_t now = nowMs();
    float scale = 1.0f;
    double anchorWorldX;
    double anchorWorldY;
    screenToWorldOffset(anchorX - width / 2.0, anchorY - height / 2.0, anchorWorldX, anchorWorldY);
    double worldX = centerX + anchorWorldX;
    double worldY = centerY + anchorWorldY;
    if (animation.active) {
        const float t = std::min(1.0f, static_cast<float>(now - animation.startMs) / ZOOM_ANIMATION_MS);
        const float eased = 1.0f - (1.0f - t) * (1.0f - t);
        scale = powf(animation.fromScale, 1.0f - eased);
        double movedX;
        double movedY;
        screenToWorldOffset((anchorX - animation.anchorX) / scale, (anchorY - animation.anchorY) / scale, movedX, movedY);
        worldX = animation.worldX + movedX;
        worldY = animation.worldY + movedY;
    }
    // The world point under the anchor stays there through the animation and after it
    const double factor = std::ldexp(1.0, target - zoom);
    const double worldSize = std::ldexp(static_cast<double>(TILE_SIZE), target);
    worldX *= factor;
    worldY *= factor;
    centerX = std::fmod(worldX - anchorWorldX + worldSize, worldSize);
    centerY = std::fmin(std::fmax(worldY - anchorWorldY, 0.0), worldSize);
    animation.active = true;
    io.hold(true);
    animation.startMs = now;
    animation.fromScale = scale / static_cast<float>(factor);
    animation.anchorX = static_cast<float>(anchorX);
    animation.anchorY = static_cast<float>(anchorY);
    animation.worldX = worldX;
    animation.worldY = worldY;
    animation.frames = 0;
    animation.composeMs = 0;
    animation.slowestFrameMs = 0;
    animation.slowestLockMs = 0;
    animation.slowestComposeMs = 0;
    zoom = target;
    dirty = true;
    // The tiles of the new level are asked for when the animation ends: their reads from the card hold the SPI bus
    // the T-Deck's display shares, which stalled animation frames by up to 240 ms
}

void MapView::screenToWorldOffset(double sx, double sy, double& wx, double& wy) const {
    wx = bearingCos * sx - bearingSin * sy;
    wy = bearingSin * sx + bearingCos * sy;
}

void MapView::worldToScreenOffset(double wx, double wy, double& sx, double& sy) const {
    sx = bearingCos * wx + bearingSin * wy;
    sy = -bearingSin * wx + bearingCos * wy;
}

void MapView::viewBounds(double& left, double& top, double& right, double& bottom) const {
    // The world box around the four turned screen corners
    const double halfWidth = width / 2.0;
    const double halfHeight = height / 2.0;
    const double spanX = std::fabs(bearingCos) * halfWidth + std::fabs(bearingSin) * halfHeight;
    const double spanY = std::fabs(bearingSin) * halfWidth + std::fabs(bearingCos) * halfHeight;
    left = centerX - spanX;
    right = centerX + spanX;
    top = centerY - spanY;
    bottom = centerY + spanY;
}

void MapView::applyBearing(float degrees, bool want) {
    degrees -= 360.0f * std::floor(degrees / 360.0f);
    // Within a tenth of a degree of north is north, so the fast unturned drawing comes back
    if (degrees < 0.1f || degrees > 359.9f) {
        degrees = 0.0f;
    }
    if (degrees == bearing) {
        return;
    }
    bearing = degrees;
    const float radians = degrees * 0.017453292f;
    bearingCos = degrees == 0.0f ? 1.0f : std::cos(radians);
    bearingSin = degrees == 0.0f ? 0.0f : std::sin(radians);
    dirty = true;
    if (want) {
        updateWanted();
    }
}

Rect MapView::compassRect() const {
    return { width - 6 - 30, 6, 30, 30 };
}

void MapView::drawCompass() {
    // A needle to north, red half towards it, in a round panel; only on a turned map
    const Rect r = compassRect();
    const float cx = r.x + r.width / 2.0f;
    const float cy = r.y + r.height / 2.0f;
    float shown = bearing;
    if (turn.active) {
        const float t = std::min(1.0f, static_cast<float>(nowMs() - turn.startMs) / TURN_ANIMATION_MS);
        shown = turn.from + (turn.to - turn.from) * (1.0f - (1.0f - t) * (1.0f - t));
    }
    const float radians = shown * 0.017453292f;
    // North on screen is the world's up turned back by the bearing
    const float northX = -std::sin(radians);
    const float northY = -std::cos(radians);
    canvas.fillCircle(cx, cy, 14.0f, theme.panel);
    canvas.ring(cx, cy, 14.0f, 1.5f, theme.text);
    canvas.line(cx, cy, cx + northX * 10.0f, cy + northY * 10.0f, 3.0f, monochrome ? theme.text : static_cast<uint16_t>(0xD8A3));
    canvas.line(cx, cy, cx - northX * 10.0f, cy - northY * 10.0f, 3.0f, monochrome ? theme.panel : theme.text);
    canvas.fillCircle(cx, cy, 2.0f, theme.text);
}

void MapView::setBearing(float degrees, bool animate) {
    // A turn already running continues from where it shows the map now
    float current = bearing;
    if (turn.active) {
        const float t = std::min(1.0f, static_cast<float>(nowMs() - turn.startMs) / TURN_ANIMATION_MS);
        current = turn.from + (turn.to - turn.from) * (1.0f - (1.0f - t) * (1.0f - t));
        turn.active = false;
        io.hold(false);
    }
    if (!animate || !smoothZoom()) {
        applyBearing(degrees);
        return;
    }
    // The shorter way round
    float target = degrees - 360.0f * std::floor(degrees / 360.0f);
    float delta = target - current;
    delta -= 360.0f * std::floor((delta + 180.0f) / 360.0f);
    if (std::fabs(delta) < 0.1f) {
        applyBearing(target);
        return;
    }
    turn.active = true;
    // Card work waits during the turn, its reads would hold the display's shared bus
    io.hold(true);
    turn.from = current;
    turn.to = current + delta;
    turn.startMs = nowMs();
    turn.frames = 0;
    turn.composeMs = 0;
    dirty = true;
}

void MapView::pan(double dx, double dy) {
    if (dx == 0.0 && dy == 0.0) {
        return;
    }
    following = false;
    headingUp = false;
    double worldDx;
    double worldDy;
    screenToWorldOffset(dx, dy, worldDx, worldDy);
    const double worldSize = std::ldexp(static_cast<double>(TILE_SIZE), zoom);
    centerX = std::fmod(centerX + worldDx + worldSize, worldSize);
    centerY = std::fmin(std::fmax(centerY + worldDy, 0.0), worldSize);
    dirty = true;
    updateWanted();
}

bool MapView::isSmoothView() const {
    return smooth.valid && smooth.zoom == zoom && smooth.bearing == bearing && std::fabs(smooth.centerX - centerX) < 0.01 &&
        std::fabs(smooth.centerY - centerY) < 0.01;
}

void MapView::updateSmooth(uint32_t now) {
    if (!isRotated() || isSmoothView() || animation.active || turn.active || pinching || isFullScreen() || now - lastInputAt < 250 ||
        !isViewComplete()) {
        return;
    }
    smooth = { true, centerX, centerY, zoom, bearing, false };
    dirty = true;
}

void MapView::startFollowing() {
    if (!position.hasFix()) {
        // Location is the user's to switch on, in the menu or in Tactility's settings
        following = false;
        showToast(position.unavailableMessage());
        return;
    }
    if (following && !headingUp) {
        // Second press: the direction of travel up, from the GPS course while moving
        headingUp = true;
        headingFilter.reset();
        showToast("Heading up");
    } else if (following) {
        // Third press: free again, the map keeps its turn until N or the compass
        following = false;
        headingUp = false;
        dirty = true;
        return;
    } else {
        following = true;
        headingUp = false;
        setBearing(0.0f, true);
    }
    centerOnPosition();
    dirty = true;
}

void MapView::endPinch(float scale, int fromX, int fromY, int toX, int toY) {
    pinching = false;
    io.hold(false);
    // Snaps to the nearest whole zoom level, the point under the fingers at the start stays under them
    const int newZoom = snapZoom(zoom, scale, MAX_VIEW_ZOOM);
    const double factor = std::ldexp(1.0, newZoom - zoom);
    // pinchWorldX, pinchWorldY is the world point under (fromX, fromY) at the start; the bearing may have changed since
    const double worldX = pinchWorldX;
    const double worldY = pinchWorldY;
    (void)fromX;
    (void)fromY;
    double toWorldX;
    double toWorldY;
    screenToWorldOffset(toX - width / 2.0, toY - height / 2.0, toWorldX, toWorldY);
    const double worldSize = std::ldexp(static_cast<double>(TILE_SIZE), newZoom);
    centerX = std::fmod(worldX * factor - toWorldX + worldSize, worldSize);
    centerY = std::fmin(std::fmax(worldY * factor - toWorldY, 0.0), worldSize);
    pinchTurning = false;
    zoom = newZoom;
    dirty = true;
    updateWanted();
}

} // namespace maps
