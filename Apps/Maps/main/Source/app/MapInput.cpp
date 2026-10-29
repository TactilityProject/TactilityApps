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
constexpr int MOUSE_PAN_SCALE = 4;
/** Trackball movement in mouse mode that moves a list by one row. */
constexpr int MOUSE_ROW_STEP = 6;
constexpr uint32_t LONG_PRESS_MS = 600;
constexpr int CURSOR_SCALE = 3;

} // namespace

void MapView::markInput(uint32_t now) {
    lastInputAt = now;
    // Prefetch and other idle work stop at once, so the card and the lanes serve what is on screen
    if (idle) {
        idle = false;
        worker.cancelPrefetch();
        downloader.setIdle(false);
    }
}

void MapView::handleKey(uint32_t key) {
    dirty = true;
    if (dialog.isOpen()) {
        if (dialog.handleKey(key) == UiAction::Chosen) {
            deleteRegion();
        }
        return;
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
            setZoom(zoom + 1, width / 2, height / 2);
            break;
        case Control::ZoomOut:
            setZoom(zoom - 1, width / 2, height / 2);
            break;
        case Control::Position:
            startFollowing();
            break;
        default:
            break;
    }
}

bool MapView::pressAt(int x, int y) {
    const Control control = controls.hit(x, y);
    if (control != Control::None) {
        pressControl(control);
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
            exitRequested = true;
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
            setZoom(zoom + 1, width / 2, height / 2);
            break;
        case 'o':
        case 'O':
            setZoom(zoom - 1, width / 2, height / 2);
            break;
        case 'g':
        case 'G':
            startFollowing();
            break;
        case 'm':
        case 'M':
            openMenu();
            break;
        case 'D':
            // Lower case d pans, so the download frame is Shift and D
            screen = Screen::Download;
            controls.hide();
            cursorShown = false;
            break;
        default:
            break;
    }
}

void MapView::click(bool fromPointer) {
    dirty = true;
    if (dialog.isOpen()) {
        if (dialog.press() == UiAction::Chosen) {
            deleteRegion();
        }
    } else if (menuOpen || screen != Screen::Map) {
        handleKey(keys::ENTER);
    } else if (fromPointer && cursorShown) {
        // A click on a button presses it, a click on empty map hides the controls
        if (!pressAt(cursorX, cursorY)) {
            controls.hide();
            cursorShown = false;
        }
    } else if (controls.isVisible() && controls.usesFocus()) {
        pressControl(controls.focused());
    } else {
        controls.toggle();
        // With a pointer the controls are reached with a cursor, it starts in the middle of the screen
        cursorShown = fromPointer && controls.isVisible();
        cursorX = width / 2;
        cursorY = height / 2;
    }
}

void MapView::longClick() {
    // The menu opens from the map, a long click elsewhere goes back
    if (dialog.isOpen()) {
        dialog.close();
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
    int32_t dx;
    int32_t dy;
    const bool pressed = input.readMouse(dx, dy);
    if (pressed || dx != 0 || dy != 0) {
        markInput(now);
    }
    ListView* list = menuOpen ? &menu : activeList();
    if (dialog.isOpen()) {
        if (dx != 0 || dy != 0) {
            mouseRowAccumulator += dx + dy;
            if (std::abs(mouseRowAccumulator) >= MOUSE_ROW_STEP) {
                dialog.handleKey(keys::TAB);
                mouseRowAccumulator = 0;
                dirty = true;
            }
        }
    } else if (list != nullptr || screen == Screen::About) {
        // In lists the trackball steps through the rows
        mouseRowAccumulator += dy;
        while (std::abs(mouseRowAccumulator) >= MOUSE_ROW_STEP) {
            const int step = mouseRowAccumulator > 0 ? 1 : -1;
            mouseRowAccumulator -= step * MOUSE_ROW_STEP;
            if (list != nullptr) {
                list->move(step);
            } else {
                scrollAbout(step);
            }
            dirty = true;
        }
    } else if (controls.isVisible() && screen == Screen::Map) {
        // While the controls show, the trackball moves the pointer instead of the map
        if (dx != 0 || dy != 0) {
            if (!cursorShown) {
                cursorShown = true;
                cursorX = width / 2;
                cursorY = height / 2;
            }
            cursorX = std::clamp(cursorX + static_cast<int>(dx) * CURSOR_SCALE, 0, width - 1);
            cursorY = std::clamp(cursorY + static_cast<int>(dy) * CURSOR_SCALE, 0, height - 1);
            dirty = true;
        }
    } else {
        pan(dx * MOUSE_PAN_SCALE, dy * MOUSE_PAN_SCALE);
    }
    if (pressed && !mouseHeld) {
        mouseHeld = true;
        mouseLong = false;
        mouseDownAt = now;
    } else if (pressed && mouseHeld && !mouseLong && now - mouseDownAt >= LONG_PRESS_MS) {
        mouseLong = true;
        longClick();
    } else if (!pressed && mouseHeld) {
        mouseHeld = false;
        if (!mouseLong) {
            click(true);
        }
    }
}

void MapView::handleTouch(uint32_t now) {
    TouchState state;
    input.readTouch(state);
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
    if (dialog.isOpen()) {
        if (dialog.handleTap(x, y) == UiAction::Chosen) {
            deleteRegion();
        }
        gestures.consumeTap();
        return;
    }
    if (menuOpen) {
        handleListAction(menu.handleTap(x, y));
        gestures.consumeTap();
        return;
    }
    if (screen == Screen::About) {
        // The title bar goes back, the page itself scrolls by dragging
        if (y < normalFont.lineHeight() + 8) {
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
    if (screen == Screen::Regions && !dialog.isOpen()) {
        const int row = regionList.rowAt(x, y);
        if (row >= 0) {
            regionList.select(row);
            askDeleteRegion();
        }
        return;
    }
    if (screen == Screen::Map && !dialog.isOpen()) {
        if (menuOpen) {
            closeMenu();
        } else {
            openMenu();
        }
    }
}

void MapView::handleGesture(const Gesture& gesture) {
    const bool onMap = !menuOpen && !dialog.isOpen() && !isFullScreen();
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
                setZoom(zoom + 1, gesture.x, gesture.y);
            }
            break;
        case GestureType::LongPress:
            handleLongPress(gesture.x, gesture.y);
            break;
        case GestureType::Drag:
            if (onMap) {
                pan(-gesture.dx, -gesture.dy);
            } else if (!dialog.isOpen()) {
                // Lists and the About page scroll with the finger, a row at a time
                ListView* list = menuOpen ? &menu : activeList();
                const int rowHeight = list != nullptr ? list->rowHeight() : normalFont.lineHeight();
                dragRowAccumulator -= gesture.dy;
                while (std::abs(dragRowAccumulator) >= rowHeight) {
                    const int step = dragRowAccumulator > 0 ? 1 : -1;
                    dragRowAccumulator -= step * rowHeight;
                    if (list != nullptr) {
                        list->move(step);
                    } else {
                        scrollAbout(step);
                    }
                    dirty = true;
                }
            }
            break;
        case GestureType::PinchStart:
            if (!onMap) {
                break;
            }
            pinchStepped = false;
            if (pinchBase != nullptr) {
                // The map as it is now, scaled live while the fingers move, without drawing vectors again
                worker.lock();
                composeView(pinchBase, width, height, centerX, centerY, zoom, lookupTile, this, palette->placeholder);
                worker.unlock();
                pinching = true;
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
                dirty = true;
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
