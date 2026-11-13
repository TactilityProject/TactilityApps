// SPDX-License-Identifier: GPL-3.0-or-later
#include "MapView.h"

#include "../platform/Platform.h"
#include "../render/Compose.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace maps {

namespace {

constexpr int KEY_PAN_PIXELS = 48;
constexpr int TRACKBALL_PAN_PIXELS = 16;
constexpr uint32_t LONG_PRESS_MS = 600;
/** Pointer movement while its button is held that makes the press a drag of the map instead of a click. */
constexpr int MOUSE_DRAG_SLOP = 6;

} // namespace

void MapView::markInput(uint32_t now) {
    lastInputAt = now;
    if (updatesAllowed) {
        // Idle-time updates pause at once, the fetch running now ends and the rest waits for the next quiet spell
        updatesAllowed = false;
        downloader.setUpdatesAllowed(false);
    }
    // Prefetch and other idle work stop at once, so the card and the lanes serve what is on screen
    if (idle) {
        idle = false;
        worker.cancelPrefetch();
        downloader.setIdle(false);
    }
}

void MapView::handleKey(uint32_t key) {
    dirty = true;
    if (prompt.isOpen()) {
        if (prompt.handleKey(key) == UiAction::Chosen) {
            confirmPrompt();
        }
        return;
    }
    if (workCard.isOpen()) {
        // Its only button is Close: Enter, Esc or Backspace
        if (key == keys::ENTER || key == keys::ESCAPE || key == keys::BACKSPACE || key == keys::YES || key == keys::NO) {
            workCard.close();
        }
        return;
    }
    if (dialog.isOpen()) {
        if (dialog.handleKey(key) == UiAction::Chosen) {
            confirmDialog();
        }
        return;
    }
    if (downloadCard.isOpen()) {
        handleCardButton(downloadCard.handleKey(key));
        return;
    }
    if (searchSheet.isOpen() && !menuOpen && screen == Screen::Map) {
        handleSearchEvent(searchSheet.handleKey(key));
        return;
    }
    if (sheet.isOpen() && !menuOpen && screen == Screen::Map) {
        handleSheetButton(sheet.handleKey(key));
        return;
    }
    if (routeCard.isOpen() && !menuOpen && screen == Screen::Map) {
        handleRouteCardButton(routeCard.handleKey(key));
        return;
    }
    if (pickingStart && !menuOpen && screen == Screen::Map) {
        // Enter takes the crosshair, Esc or Backspace cancels; the other keys move the map as always
        if (key == keys::ENTER || key == keys::YES) {
            endPickStart(true, worldYToLat(centerY, zoom), worldXToLon(centerX, zoom));
            return;
        }
        if (key == keys::ESCAPE || key == keys::BACKSPACE || key == keys::NO) {
            endPickStart(false, 0.0, 0.0);
            return;
        }
    }
    if (menuOpen) {
        if (key == 'm' || key == 'M') {
            closeMenu();
            return;
        }
        handleListAction(menu.handleKey(key));
        return;
    }
    switch (screen) {
        case Screen::Map:
            handleMapKey(key);
            break;
        case Screen::Download:
            handleDownloadKey(key);
            break;
        case Screen::About:
            if (key == 'w' || key == 'W' || key == keys::UP) {
                scrollAbout(-1);
            } else if (key == 's' || key == 'S' || key == keys::DOWN) {
                scrollAbout(1);
            } else if (key == keys::ESCAPE || key == keys::BACKSPACE || key == keys::ENTER || key == ' ') {
                closeScreen();
            }
            break;
        default:
            if (screen == Screen::Regions && (key == 'r' || key == 'R')) {
                askRenameRegion();
                break;
            }
            if (screen == Screen::Waypoints && (key == 'r' || key == 'R')) {
                askRenameWaypoint(listedWaypoint());
                break;
            }
            handleListAction(activeList()->handleKey(key));
            break;
    }
}

void MapView::pressControl(Control control) {
    dirty = true;
    switch (control) {
        case Control::Menu:
            openMenu();
            break;
        case Control::ZoomIn:
            zoomBy(1, width / 2, height / 2);
            break;
        case Control::ZoomOut:
            zoomBy(-1, width / 2, height / 2);
            break;
        case Control::Position:
            startFollowing();
            break;
        default:
            break;
    }
}

bool MapView::pressAt(int x, int y) {
    if ((isRotated() || turn.active) && compassRect().contains(x, y)) {
        headingUp = false;
        setBearing(0.0f, true);
        return true;
    }
    const Control control = controls.hit(x, y);
    if (control != Control::None) {
        pressControl(control);
        return true;
    }
    // A pin opens its sheet
    const int pin = waypointAt(x, y);
    if (pin >= 0) {
        openWaypointSheet(pin);
        return true;
    }
    // A POI or house number opens its sheet; a tap on empty map still shows or hides the controls
    const int place = placeAt(x, y);
    if (place >= 0) {
        placeOrderCount = 0;
        openPlaceSheet(place);
        return true;
    }
    switch (attribution.tap(x, y, attributionBar, attributionMark(), nowMs())) {
        case Attribution::Tap::Expanded:
            dirty = true;
            return true;
        case Attribution::Tap::OpenAbout:
            openAbout();
            return true;
        default:
            return false;
    }
}

void MapView::handleMapKey(uint32_t key) {
    // Boards with buttons only move a focus through the visible controls
    if (controls.isVisible() && controls.usesFocus()) {
        switch (key) {
            case keys::UP:
            case keys::LEFT:
            case keys::FOCUS_PREVIOUS:
                controls.moveFocus(-1);
                return;
            case keys::DOWN:
            case keys::RIGHT:
            case keys::FOCUS_NEXT:
            case keys::TAB:
                controls.moveFocus(1);
                return;
            case keys::ESCAPE:
            case keys::BACKSPACE:
                controls.hide();
                return;
            default:
                break;
        }
    }
    switch (key) {
        case ' ':
            controls.toggle();
            cursorShown = false;
            break;
        case keys::ESCAPE:
        case keys::BACKSPACE:
            // While navigating the first press stops it, the next one leaves the app
            if (navigating) {
                stopNavigation();
                showToast("Navigation stopped. To go on: Menu, Show route");
            } else {
                exitRequested = true;
            }
            break;
        case '/':
        case 'f':
        case 'F':
            // Search (M6): "/" as in many apps, F for find on keyboards where "/" needs a modifier
            openSearch();
            break;
        case 'p':
        case 'P':
            openNewWaypointAtPosition();
            break;
        case 'w':
        case 'W':
            pan(0, -KEY_PAN_PIXELS);
            break;
        case 's':
        case 'S':
            pan(0, KEY_PAN_PIXELS);
            break;
        case 'a':
        case 'A':
            pan(-KEY_PAN_PIXELS, 0);
            break;
        case 'd':
            pan(KEY_PAN_PIXELS, 0);
            break;
        case keys::UP:
            pan(0, -TRACKBALL_PAN_PIXELS);
            break;
        case keys::DOWN:
            pan(0, TRACKBALL_PAN_PIXELS);
            break;
        case keys::LEFT:
            pan(-TRACKBALL_PAN_PIXELS, 0);
            break;
        case keys::RIGHT:
            pan(TRACKBALL_PAN_PIXELS, 0);
            break;
        case 'i':
        case 'I':
            zoomBy(1, width / 2, height / 2);
            break;
        case 'o':
        case 'O':
            zoomBy(-1, width / 2, height / 2);
            break;
        case 'g':
        case 'G':
            startFollowing();
            break;
        case 'q':
        case 'Q':
            // Turned by hand the map stops turning with the direction of travel
            headingUp = false;
            setBearing((turn.active ? turn.to : bearing) - KEY_TURN_DEGREES, true);
            break;
        case 'e':
        case 'E':
            headingUp = false;
            setBearing((turn.active ? turn.to : bearing) + KEY_TURN_DEGREES, true);
            break;
        case 'n':
        case 'N':
            headingUp = false;
            setBearing(0.0f, true);
            break;
        case 'm':
        case 'M':
            openMenu();
            break;
        case 'D':
            // Lower case d pans, so the download frame is Shift and D
            screen = Screen::Download;
            // The download frame works north up
            following = false;
            headingUp = false;
            setBearing(0.0f, false);
            controls.hide();
            cursorShown = false;
            break;
        default:
            break;
    }
}

void MapView::pointerClick() {
    dirty = true;
    if (!cursorShown) {
        // The pointer hid itself after a while: a click brings it back where it was
        cursorShown = true;
        cursorMovedAt = nowMs();
        controls.setHover(menuOpen ? Control::None : controls.hit(cursorX, cursorY));
        return;
    }
    cursorMovedAt = nowMs();
    if (prompt.isOpen() || dialog.isOpen() || menuOpen || searchSheet.isOpen() || sheet.isOpen() || downloadCard.isOpen() || screen != Screen::Map ||
        (pillRect.width > 0 && pillRect.contains(cursorX, cursorY))) {
        handleTap(cursorX, cursorY);
        return;
    }
    // On the map: a button presses, a pin opens its sheet, a click on empty map shows or hides the controls
    if (!pressAt(cursorX, cursorY)) {
        controls.toggle();
    }
    controls.setHover(controls.hit(cursorX, cursorY));
}

void MapView::click(bool fromPointer) {
    dirty = true;
    if (fromPointer && input.hasPointer()) {
        pointerClick();
        return;
    }
    if (prompt.isOpen()) {
        if (prompt.handleKey(keys::ENTER) == UiAction::Chosen) {
            confirmPrompt();
        }
    } else if (dialog.isOpen()) {
        if (dialog.press() == UiAction::Chosen) {
            confirmDialog();
        }
    } else if (sheet.isOpen() && sheet.mode() == WaypointSheet::Mode::Place && sheet.focused() == WaypointSheet::Button::Close &&
               placeOrderCount > 0 && !menuOpen && !downloadCard.isOpen()) {
        // Enter again: the next nearest place
        selectNextPlace();
    } else if (menuOpen || searchSheet.isOpen() || sheet.isOpen() || downloadCard.isOpen() || workCard.isOpen() || screen != Screen::Map || pickingStart) {
        handleKey(keys::ENTER);
    } else if (controls.isVisible() && controls.usesFocus()) {
        pressControl(controls.focused());
    } else if (controls.isVisible() && selectNearestPlace()) {
        // With the controls (and the crosshair) shown, Enter picks the place nearest the centre
    } else {
        controls.toggle();
    }
}

void MapView::longClick() {
    // The menu opens from the map, a long click elsewhere goes back
    if (prompt.isOpen()) {
        prompt.close();
    } else if (dialog.isOpen()) {
        dialog.close();
    } else if (downloadCard.isOpen()) {
        downloadCard.close();
    } else if (sheet.isOpen() && screen == Screen::Map) {
        handleSheetButton(WaypointSheet::Button::Cancel);
    } else if (menuOpen) {
        closeMenu();
    } else if (screen == Screen::Map) {
        openMenu();
    } else {
        closeScreen();
    }
    dirty = true;
}

void MapView::handleEnter(bool pressed, uint32_t now) {
    // The trackball repeats its press while held, the keyboard only sends one press
    if (pressed) {
        if (!enterHeld) {
            enterHeld = true;
            enterLong = false;
            enterDownAt = now;
        }
        enterSeenAt = now;
        return;
    }
    if (enterHeld) {
        enterHeld = false;
        if (!enterLong) {
            click(false);
        }
    }
}

void MapView::handleMouse(uint32_t now) {
    int32_t rawX;
    int32_t rawY;
    const bool pressed = input.readMouse(rawX, rawY);
    if (pressed || rawX != 0 || rawY != 0) {
        markInput(now);
    }
    // A trackball in mouse mode is a pointer on every screen: it moves the pointer, a click is a tap where it points
    int dx;
    int dy;
    pointerAcceleration.apply(rawX, rawY, now, dx, dy);
    if (dx != 0 || dy != 0) {
        int overflowX;
        int overflowY;
        const int fromX = cursorX;
        const int fromY = cursorY;
        moveCursor(dx, dy, now, overflowX, overflowY);
        dirty = true;
        ListView* list = menuOpen ? &menu : activeList();
        const bool onMap = !menuOpen && !dialog.isOpen() && !prompt.isOpen() && (screen == Screen::Map || screen == Screen::Download);
        if (mouseHeld && onMap && !mouseLong) {
            // Rolling with the button held drags the map under the pointer, which stays where it is
            mouseDragDistance += std::abs(dx) + std::abs(dy);
            if (mouseDragged || mouseDragDistance >= MOUSE_DRAG_SLOP) {
                mouseDragged = true;
                cursorX = fromX;
                cursorY = fromY;
                pan(-dx, -dy);
            }
        } else if (onMap && (overflowX != 0 || overflowY != 0)) {
            // Pushing against a screen edge moves the map that way
            pan(overflowX, overflowY);
        } else if (list != nullptr && !dialog.isOpen() && !prompt.isOpen()) {
            // The row under the pointer is the highlighted one; pushing against the top or bottom scrolls the list
            const int row = list->rowAt(cursorX, cursorY);
            if (row >= 0) {
                list->select(row);
                list->setHighlighted(true);
            }
            mouseRowAccumulator += overflowY;
            while (std::abs(mouseRowAccumulator) >= list->rowHeight() / 2) {
                const int step = mouseRowAccumulator > 0 ? 1 : -1;
                mouseRowAccumulator -= step * list->rowHeight() / 2;
                list->move(step);
            }
        } else if (screen == Screen::About && overflowY != 0) {
            mouseRowAccumulator += overflowY;
            while (std::abs(mouseRowAccumulator) >= normalFont.lineHeight() / 2) {
                const int step = mouseRowAccumulator > 0 ? 1 : -1;
                mouseRowAccumulator -= step * normalFont.lineHeight() / 2;
                scrollAbout(step);
            }
        }
    }
    if (pressed && !mouseHeld) {
        mouseHeld = true;
        mouseLong = false;
        mouseDragged = false;
        mouseDragDistance = 0;
        mouseDownAt = now;
    } else if (!pressed && mouseHeld) {
        mouseHeld = false;
        // Holding the button does nothing by itself, the menu is on the button at the top left
        if (!mouseDragged && now - mouseDownAt < LONG_PRESS_MS) {
            click(true);
        }
        mouseDragged = false;
    }
}

void MapView::moveCursor(int dx, int dy, uint32_t now, int& overflowX, int& overflowY) {
    if (!cursorShown) {
        cursorShown = true;
        if (cursorMovedAt == 0) {
            cursorX = width / 2;
            cursorY = height / 2;
        }
    }
    const int wantedX = cursorX + dx;
    const int wantedY = cursorY + dy;
    cursorX = std::clamp(wantedX, 0, width - 1);
    cursorY = std::clamp(wantedY, 0, height - 1);
    overflowX = wantedX - cursorX;
    overflowY = wantedY - cursorY;
    cursorMovedAt = now;
    controls.setHover(menuOpen || screen != Screen::Map ? Control::None : controls.hit(cursorX, cursorY));
}

void MapView::syntheticTouchState(uint32_t now, TouchState& state) {
    // A debug script's touch: one or two fingers along a path over its duration, then lifted
    const DebugScript::Step& step = syntheticTouch;
    const uint32_t elapsed = now - syntheticTouchStart;
    state = {};
    // A slow frame must not swallow a short touch: the fingers are always seen down, and at their end position, once
    if (elapsed >= step.durationMs && syntheticTouchEnded) {
        syntheticTouchActive = false;
        return;
    }
    if (elapsed >= step.durationMs) {
        syntheticTouchEnded = true;
    }
    const float t = step.durationMs > 0 ? std::min(1.0f, static_cast<float>(elapsed) / static_cast<float>(step.durationMs)) : 1.0f;
    const float* v = step.values;
    auto put = [&](int finger, float x, float y) {
        state.x[finger] = static_cast<uint16_t>(std::clamp(x, 0.0f, static_cast<float>(width - 1)));
        state.y[finger] = static_cast<uint16_t>(std::clamp(y, 0.0f, static_cast<float>(height - 1)));
    };
    switch (step.touch) {
        case DebugScript::TouchKind::Tap:
        case DebugScript::TouchKind::Hold:
            state.count = 1;
            put(0, v[0], v[1]);
            break;
        case DebugScript::TouchKind::Drag:
            state.count = 1;
            put(0, v[0] + (v[2] - v[0]) * t, v[1] + (v[3] - v[1]) * t);
            break;
        case DebugScript::TouchKind::Pinch: {
            const float half = (v[2] + (v[3] - v[2]) * t) / 2.0f;
            state.count = 2;
            put(0, v[0] - half, v[1]);
            put(1, v[0] + half, v[1]);
            break;
        }
        case DebugScript::TouchKind::Twist: {
            const float radians = (v[3] + (v[4] - v[3]) * t) * 0.017453292f;
            const float dx = v[2] * std::cos(radians);
            const float dy = v[2] * std::sin(radians);
            state.count = 2;
            put(0, v[0] - dx, v[1] - dy);
            put(1, v[0] + dx, v[1] + dy);
            break;
        }
    }
}

void MapView::handleTouch(uint32_t now) {
    TouchState state;
    if (syntheticTouchActive) {
        syntheticTouchState(now, state);
    } else {
        input.readTouch(state);
    }
    if (state.count > 0) {
        markInput(now);
    }
    const TouchSample sample { state.count, { state.x[0], state.x[1] }, { state.y[0], state.y[1] } };
    Gesture found[Gestures::MAX_GESTURES];
    const int count = gestures.update(sample, now, found);
    for (int i = 0; i < count; i++) {
        handleGesture(found[i]);
    }
}

void MapView::handleTap(int x, int y) {
    // Taps on something answer at once, taps on the map wait to tell a single from a double tap
    dirty = true;
    // The on-screen keyboard types into the open text field as a keyboard would
    syncTouchKeyboard();
    if (touchKeyboard.isOpen() && touchKeyboard.contains(x, y)) {
        const uint32_t code = touchKeyboard.tap(x, y);
        if (code != 0) {
            handleKey(code);
        }
        gestures.consumeTap();
        return;
    }
    if (prompt.isOpen()) {
        if (prompt.handleTap(x, y) == UiAction::Chosen) {
            confirmPrompt();
        }
        gestures.consumeTap();
        return;
    }
    if (searchSheet.isOpen() && !menuOpen && screen == Screen::Map) {
        handleSearchEvent(searchSheet.handleTap(x, y));
        gestures.consumeTap();
        return;
    }
    if (dialog.isOpen()) {
        if (dialog.handleTap(x, y) == UiAction::Chosen) {
            confirmDialog();
        }
        gestures.consumeTap();
        return;
    }
    if (menuOpen) {
        handleListAction(menu.handleTap(x, y));
        gestures.consumeTap();
        return;
    }
    if (downloadCard.isOpen()) {
        handleCardButton(downloadCard.handleTap(x, y));
        gestures.consumeTap();
        return;
    }
    if (workCard.isOpen()) {
        // Close, or a tap anywhere else: the card only informs
        workCard.close();
        gestures.consumeTap();
        return;
    }
    if (screen == Screen::Map && workDotShown && workDotTarget(WORK_DOT_X, WORK_DOT_Y).contains(x, y)) {
        workCard.open();
        gestures.consumeTap();
        return;
    }
    if (screen == Screen::Map && !menuOpen && pillRect.width > 0 && pillRect.contains(x, y) &&
        !(routeCard.isOpen() && routeCard.modeBounds().contains(x, y))) {
        openDownloadCard();
        gestures.consumeTap();
        return;
    }
    if (sheet.isOpen() && screen == Screen::Map) {
        // A tap outside the sheet closes it and does nothing else
        handleSheetButton(sheet.handleTap(x, y));
        gestures.consumeTap();
        return;
    }
    if (navigating && screen == Screen::Map && !menuOpen && y < navBar.height(sheetFont())) {
        // The navigation bar: its X stops navigating, the rest of it does nothing (the controls are not under it)
        if (navBar.stopHit(x, y)) {
            stopNavigation();
            showToast("Navigation stopped. To go on: Menu, Show route");
        }
        gestures.consumeTap();
        return;
    }
    if (pickingStart && screen == Screen::Map && !menuOpen) {
        // The tapped point is the start
        double worldX;
        double worldY;
        screenToWorldOffset(x - width / 2.0, y - height / 2.0, worldX, worldY);
        endPickStart(true, worldYToLat(centerY + worldY, zoom), worldXToLon(centerX + worldX, zoom));
        gestures.consumeTap();
        return;
    }
    if (routeCard.isOpen() && screen == Screen::Map && !menuOpen) {
        // The same for the route card: a tap on the map closes it (the route stays)
        handleRouteCardButton(routeCard.handleTap(x, y));
        gestures.consumeTap();
        return;
    }
    if (screen == Screen::About) {
        // The < in the header goes back, the page itself scrolls by dragging
        if (aboutHeader.back.contains(x, y)) {
            closeScreen();
        }
        gestures.consumeTap();
        return;
    }
    if (ListView* list = activeList()) {
        handleListAction(list->handleTap(x, y));
        gestures.consumeTap();
        return;
    }
    // Buttons and the attribution answer at once, the map itself only to a single tap
    if (screen == Screen::Download) {
        if (downloadView.button.contains(x, y)) {
            startDownload();
            gestures.consumeTap();
        } else if (downloadView.back.contains(x, y)) {
            closeScreen();
            gestures.consumeTap();
        } else if (const Attribution::Tap result = attribution.tap(x, y, attributionBar, attributionMark(), nowMs()); result != Attribution::Tap::Missed) {
            if (result == Attribution::Tap::OpenAbout) {
                openAbout();
            }
            gestures.consumeTap();
        } else if (downloadView.bar.contains(x, y)) {
            gestures.consumeTap();
        }
        return;
    }
    if (screen == Screen::Map && pressAt(x, y)) {
        gestures.consumeTap();
    }
}

void MapView::handleLongPress(int x, int y) {
    dirty = true;
    if (searchSheet.isOpen()) {
        // Holding a result row opens it like a tap; nothing behind the sheet reacts
        handleTap(x, y);
        return;
    }
    // Holding a row opens its actions, like a tap
    if (screen == Screen::Regions && !dialog.isOpen() && !menuOpen) {
        const int row = regionList.rowAt(x, y);
        if (row >= 0) {
            regionList.select(row);
            openRegionActions();
        }
        return;
    }
    if (screen == Screen::Waypoints && !dialog.isOpen() && !menuOpen) {
        const int row = waypointList.rowAt(x, y);
        if (row >= 0) {
            waypointList.select(row);
            openWaypointActions();
        }
        return;
    }
    if (screen == Screen::Map && !dialog.isOpen()) {
        if (menuOpen) {
            closeMenu();
        } else if (controls.hit(x, y) == Control::Position) {
            // Holding the position button opens a waypoint sheet where I am
            openNewWaypointAtPosition();
        } else if (controls.hit(x, y) == Control::None && !sheet.contains(x, y)) {
            // Holding the map opens a waypoint sheet for that point; the menu is on the button
            double offsetX;
            double offsetY;
            screenToWorldOffset(x - width / 2.0, y - height / 2.0, offsetX, offsetY);
            openNewWaypoint(worldYToLat(centerY + offsetY, zoom), worldXToLon(centerX + offsetX, zoom));
        } else if (!sheet.isOpen()) {
            openMenu();
        }
    }
}

void MapView::handleGesture(const Gesture& gesture) {
    const bool onMap = !menuOpen && !dialog.isOpen() && !isFullScreen() && !searchSheet.isOpen();
    switch (gesture.type) {
        case GestureType::Tap:
            handleTap(gesture.x, gesture.y);
            break;
        case GestureType::SingleTap:
            // A tap on the map shows or hides the controls, dragging, pinching and double taps never do
            if (onMap && screen == Screen::Map) {
                controls.toggle();
                cursorShown = false;
                dirty = true;
            }
            break;
        case GestureType::DoubleTap:
            if (onMap) {
                zoomBy(1, gesture.x, gesture.y);
            }
            break;
        case GestureType::LongPress:
            handleLongPress(gesture.x, gesture.y);
            break;
        case GestureType::Drag:
            if (searchSheet.isOpen() && !menuOpen && !dialog.isOpen()) {
                handleSearchEvent(searchSheet.handleDrag(gesture.dy));
            } else if (onMap) {
                pan(-gesture.dx, -gesture.dy);
            } else if (!dialog.isOpen() && !prompt.isOpen() && std::abs(gesture.dy) >= std::abs(gesture.dx)) {
                // Lists and the About page scroll with the finger, a row at a time; the highlight stays where it is
                ListView* list = menuOpen ? &menu : activeList();
                const int rowHeight = list != nullptr ? list->rowHeight() : normalFont.lineHeight();
                dragRowAccumulator -= gesture.dy;
                while (std::abs(dragRowAccumulator) >= rowHeight) {
                    const int step = dragRowAccumulator > 0 ? 1 : -1;
                    dragRowAccumulator -= step * rowHeight;
                    if (list != nullptr) {
                        list->scrollBy(step);
                    } else {
                        scrollAbout(step);
                    }
                    dirty = true;
                }
            }
            break;
        case GestureType::SwipeBack:
            // One level back in lists, popups and About; on the map a swipe is only a drag
            if (!onMap && !dialog.isOpen() && !prompt.isOpen()) {
                dragRowAccumulator = 0;
                goBack();
            }
            break;
        case GestureType::PinchStart:
            if (!onMap) {
                break;
            }
            pinchStepped = false;
            if (smoothZoom()) {
                // The cached tiles scaled live while the fingers move, without drawing vectors again
                animation.active = false;
                io.hold(false);
                pinching = true;
                // Card work waits while the fingers move, so frames do not wait for the shared bus
                io.hold(true);
                double offsetX;
                double offsetY;
                screenToWorldOffset(gesture.startX - width / 2.0, gesture.startY - height / 2.0, offsetX, offsetY);
                pinchWorldX = centerX + offsetX;
                pinchWorldY = centerY + offsetY;
                if (turn.active) {
                    // A turn under way ends where it was going
                    turn.active = false;
                    applyBearing(turn.to, false);
                }
                pinchTurning = false;
                pinchStartBearing = bearing;
                headingUp = false;
                pinchScale = 1.0f;
                pinchStartX = gesture.startX;
                pinchStartY = gesture.startY;
                pinchX = gesture.x;
                pinchY = gesture.y;
                following = false;
                dirty = true;
            }
            break;
        case GestureType::PinchMove:
            if (pinching) {
                pinchScale = gesture.scale;
                pinchX = gesture.x;
                pinchY = gesture.y;
                // Turning starts once the fingers twisted far enough, and from there follows them; fingers turning
                // clockwise turn the map clockwise, so the bearing goes the other way
                if (!pinchTurning && std::fabs(gesture.rotation) >= (pinchStartBearing == 0.0f ? PINCH_TURN_START_NORTH : PINCH_TURN_START)) {
                    pinchTurning = true;
                    pinchTwistStart = gesture.rotation;
                }
                if (pinchTurning) {
                    applyBearing(pinchStartBearing - (gesture.rotation - pinchTwistStart), false);
                }
                dirty = true;
            } else if (!pinchStepped && onMap && std::fabs(gesture.rotation) >= 3.0f * PINCH_TURN_START) {
                // Without live drawing (e-paper, or switched off) a clear twist turns the map once by 45 degrees
                setBearing(bearing + (gesture.rotation > 0.0f ? -45.0f : 45.0f), false);
                pinchStepped = true;
            } else if (!pinchStepped && onMap) {
                // Without live scaling (e-paper, or switched off) one zoom step per pinch
                if (gesture.scale > 1.5f) {
                    setZoom(zoom + 1, gesture.x, gesture.y);
                    pinchStepped = true;
                } else if (gesture.scale < 0.67f) {
                    setZoom(zoom - 1, gesture.x, gesture.y);
                    pinchStepped = true;
                }
            }
            break;
        case GestureType::PinchEnd:
            if (pinching) {
                endPinch(gesture.scale, gesture.startX, gesture.startY, gesture.x, gesture.y);
            }
            break;
        default:
            break;
    }
}

} // namespace maps
