// SPDX-License-Identifier: GPL-3.0-or-later
// Waypoints, track recording and GPX files on the map.
#include "MapView.h"

#include "../gpx/Track.h"
#include "../ui/Pins.h"
#include "../platform/Platform.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

namespace maps {

namespace {

constexpr int PIN_HIT = 14;
constexpr uint16_t TRACK_COLOR = 0xD8A4;
constexpr uint16_t GPX_TRACK_COLOR = 0x3A7F;
constexpr uint16_t GPX_ROUTE_COLOR = 0xA81F;
constexpr uint16_t PIN_COLOR = 0xFB20;
/** Points of a line closer than this on screen are skipped when drawing. */
constexpr float MIN_STEP_PIXELS = 2.0f;

const char* compass(double bearing) {
    static const char* const NAMES[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return NAMES[static_cast<int>(std::floor((bearing + 22.5) / 45.0)) % 8];
}

void formatCoordinates(double lat, double lon, char* out, size_t size) {
    snprintf(out, size, "%.5f %c  %.5f %c", std::fabs(lat), lat >= 0.0 ? 'N' : 'S', std::fabs(lon), lon >= 0.0 ? 'E' : 'W');
}

void formatDistance(double metres, char* out, size_t size) {
    if (metres < 1000.0) {
        snprintf(out, size, "%d m", static_cast<int>(metres + 0.5));
    } else if (metres < 10000.0) {
        snprintf(out, size, "%.1f km", metres / 1000.0);
    } else {
        snprintf(out, size, "%d km", static_cast<int>(metres / 1000.0 + 0.5));
    }
}

} // namespace

bool MapView::worldToScreen(double lat, double lon, float& x, float& y) const {
    const double worldSize = std::ldexp(static_cast<double>(TILE_SIZE), zoom);
    double dx = lonToWorldX(lon, zoom) - centerX;
    // The shorter way round the antimeridian
    if (dx > worldSize / 2.0) {
        dx -= worldSize;
    } else if (dx < -worldSize / 2.0) {
        dx += worldSize;
    }
    double sx;
    double sy;
    worldToScreenOffset(dx, latToWorldY(lat, zoom) - centerY, sx, sy);
    x = static_cast<float>(sx + width / 2.0);
    y = static_cast<float>(sy + height / 2.0);
    return x > -20.0f && y > -20.0f && x < width + 20.0f && y < height + 20.0f;
}

void MapView::describeDistance(double lat, double lon, char* out, size_t size) const {
    const bool fromHere = position.hasFix();
    const double fromLat = fromHere ? position.fix().latitude : worldYToLat(centerY, zoom);
    const double fromLon = fromHere ? position.fix().longitude : worldXToLon(centerX, zoom);
    char distance[24];
    formatDistance(distanceMetres(fromLat, fromLon, lat, lon), distance, sizeof(distance));
    snprintf(out, size, "%s %s%s", distance, compass(bearingDegrees(fromLat, fromLon, lat, lon)), fromHere ? "" : " of the centre");
}

// ---------------------------------------------------------------------------------------------------------------
// Waypoints

void MapView::loadWaypoints() {
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/waypoints.gpx", rootPath);
    FileBuffer file;
    auto read = [&] { return file.load(path); };
    if (runOnCard(io, read, IoPriority::Background) &&
        !waypoints.load(reinterpret_cast<const char*>(file.bytes()), file.size())) {
        logError("waypoints.gpx is damaged, %d waypoints read", waypoints.count());
    }
}

void MapView::saveWaypoints() {
    ByteWriter text;
    if (!waypoints.save(text, APP_VERSION)) {
        showToast("Could not save the waypoints");
        return;
    }
    char path[PATH_MAX_LENGTH];
    char temporary[PATH_MAX_LENGTH + 8];
    snprintf(path, sizeof(path), "%s/waypoints.gpx", rootPath);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    auto write = [&] { return io.writeFile(path, temporary, text.bytes(), text.size(), false); };
    if (!runOnCard(io, write, IoPriority::Background)) {
        showToast("Could not save the waypoints");
    }
}

void MapView::openNewWaypoint(double lat, double lon) {
    if (waypoints.count() >= Waypoints::MAX_WAYPOINTS) {
        showToast("200 waypoints is the most, delete some first");
        return;
    }
    sheetLat = lat;
    sheetLon = lon;
    selectedWaypoint = -1;
    char name[WaypointSheet::NAME_LENGTH];
    waypoints.nextName(name, sizeof(name));
    char coordinates[48];
    formatCoordinates(lat, lon, coordinates, sizeof(coordinates));
    char distance[64];
    describeDistance(lat, lon, distance, sizeof(distance));
    sheet.openNew(name, coordinates, distance, input.hasKeyboard());
    sheet.layout(width, height, overlay.font());
    controls.hide();
    dirty = true;
}

void MapView::openNewWaypointAtPosition() {
    if (!position.hasFix() || position.state() == PositionState::Approximate) {
        showToast(position.hasFix() ? "The position is only approximate" : position.unavailableMessage());
        return;
    }
    openNewWaypoint(position.fix().latitude, position.fix().longitude);
}

void MapView::openWaypointSheet(int index) {
    if (index < 0 || index >= waypoints.count()) {
        return;
    }
    const Waypoint& waypoint = waypoints.at(index);
    selectedWaypoint = index;
    sheetLat = waypoint.lat;
    sheetLon = waypoint.lon;
    char coordinates[48];
    formatCoordinates(waypoint.lat, waypoint.lon, coordinates, sizeof(coordinates));
    char distance[64];
    describeDistance(waypoint.lat, waypoint.lon, distance, sizeof(distance));
    sheet.openExisting(waypoint.name, coordinates, distance);
    sheet.layout(width, height, overlay.font());
    dirty = true;
}

void MapView::saveNewWaypoint() {
    const int index = waypoints.add(sheetLat, sheetLon, static_cast<int64_t>(time(nullptr)), NAN, sheet.name());
    if (index < 0) {
        showToast("200 waypoints is the most, delete some first");
        return;
    }
    saveWaypoints();
    char text[64];
    snprintf(text, sizeof(text), "%s saved", waypoints.at(index).name);
    showToast(text);
}

void MapView::handleSheetButton(WaypointSheet::Button button) {
    dirty = true;
    switch (button) {
        case WaypointSheet::Button::Save:
            sheet.close();
            saveNewWaypoint();
            break;
        case WaypointSheet::Button::Cancel:
        case WaypointSheet::Button::Close:
            sheet.close();
            selectedWaypoint = -1;
            break;
        case WaypointSheet::Button::Rename:
            if (!input.hasKeyboard()) {
                showToast("Renaming needs a keyboard");
                break;
            }
            askRenameWaypoint(selectedWaypoint);
            break;
        case WaypointSheet::Button::Delete:
            askDeleteWaypoint(selectedWaypoint);
            break;
        default:
            break;
    }
}

int MapView::waypointAt(int x, int y) const {
    int best = -1;
    float bestDistance = static_cast<float>(PIN_HIT * PIN_HIT);
    for (int i = 0; i < waypoints.count(); i++) {
        float px;
        float py;
        if (!worldToScreen(waypoints.at(i).lat, waypoints.at(i).lon, px, py)) {
            continue;
        }
        // The pin's head is above its point
        const float dx = px - static_cast<float>(x);
        const float dy = py - PIN_HEAD_OFFSET - static_cast<float>(y);
        if (dx * dx + dy * dy < bestDistance) {
            bestDistance = dx * dx + dy * dy;
            best = i;
        }
    }
    return best;
}

void MapView::openWaypoints() {
    if (waypoints.count() == 0) {
        showToast(input.hasTouch() ? "No waypoints yet: hold the map, or Menu, Add waypoint" : "No waypoints yet: P, or Menu, Add waypoint");
        return;
    }
    fillWaypointList();
    waypointList.select(0);
    waypointList.setHighlighted(false);
    screen = Screen::Waypoints;
    dirty = true;
}

void MapView::fillWaypointList() {
    waypointList.clear();
    waypointList.setTitle("Waypoints");
    const double lat = position.hasFix() ? position.fix().latitude : worldYToLat(centerY, zoom);
    const double lon = position.hasFix() ? position.fix().longitude : worldXToLon(centerX, zoom);
    const int count = waypoints.sortedByDistance(lat, lon, waypointRows, ListView::MAX_ITEMS);
    for (int row = 0; row < count; row++) {
        const Waypoint& waypoint = waypoints.at(waypointRows[row]);
        char detail[ListView::TEXT_LENGTH];
        describeDistance(waypoint.lat, waypoint.lon, detail, sizeof(detail));
        waypointList.add(waypoint.name, detail);
    }
    setListHint(waypointList, "Enter: actions  R: rename  X: delete", "Tap: actions  Swipe right: back");
    waypointList.layoutFullScreen(width, height, overlay.font());
}

void MapView::goToWaypoint() {
    const int row = waypointList.selected();
    if (row < 0 || row >= waypointList.count()) {
        return;
    }
    const int index = waypointRows[row];
    following = false;
    centerOn(waypoints.at(index).lat, waypoints.at(index).lon, std::max(zoom, 14));
    returnToMenu = false;
    screen = Screen::Map;
    openWaypointSheet(index);
    dirty = true;
}

int MapView::listedWaypoint() const {
    const int row = waypointList.selected();
    return row >= 0 && row < waypointList.count() ? waypointRows[row] : -1;
}

void MapView::openWaypointActions() {
    const int row = waypointList.selected();
    if (row < 0 || row >= waypointList.count()) {
        return;
    }
    beginPopup(Popup::WaypointActions, waypoints.at(waypointRows[row]).name);
    // 0 go to, 1 rename, 2 delete; renaming needs a keyboard
    menu.add("Go to");
    popupActions[popupActionCount++] = 0;
    if (input.hasKeyboard()) {
        menu.add("Rename");
        popupActions[popupActionCount++] = 1;
    }
    menu.add("Delete");
    popupActions[popupActionCount++] = 2;
    layoutPopup();
}

void MapView::chooseWaypointAction(int action) {
    switch (action) {
        case 0:
            goToWaypoint();
            break;
        case 1:
            askRenameWaypoint(listedWaypoint());
            break;
        default:
            askDeleteWaypoint(listedWaypoint());
            break;
    }
}

void MapView::askDeleteWaypoint(int index) {
    if (index < 0 || index >= waypoints.count()) {
        return;
    }
    editingWaypoint = index;
    char message[96];
    snprintf(message, sizeof(message), "Delete %s?", waypoints.at(index).name);
    dialogPurpose = DialogPurpose::DeleteWaypoint;
    dialog.open(message, "Delete", true);
    dialog.layout(width, height, overlay.font());
    dirty = true;
}

void MapView::deleteWaypoint() {
    if (editingWaypoint < 0 || editingWaypoint >= waypoints.count()) {
        return;
    }
    waypoints.remove(editingWaypoint);
    editingWaypoint = -1;
    // Indices after the deleted one moved, so nothing stays selected
    selectedWaypoint = -1;
    sheet.close();
    saveWaypoints();
    if (screen != Screen::Waypoints) {
        return;
    }
    if (waypoints.count() == 0) {
        closeScreen();
        return;
    }
    const int row = waypointList.selected();
    fillWaypointList();
    waypointList.select(std::min(row, waypointList.count() - 1));
}

void MapView::askRenameWaypoint(int index) {
    if (!input.hasKeyboard() || index < 0 || index >= waypoints.count()) {
        return;
    }
    editingWaypoint = index;
    promptPurpose = PromptPurpose::RenameWaypoint;
    prompt.open("Name of this waypoint", waypoints.at(index).name);
    prompt.layout(width, height, overlay.font());
    dirty = true;
}

void MapView::renameWaypoint(const char* name) {
    if (editingWaypoint < 0 || editingWaypoint >= waypoints.count() || name[0] == '\0') {
        return;
    }
    waypoints.rename(editingWaypoint, name);
    saveWaypoints();
    if (sheet.isOpen() && selectedWaypoint == editingWaypoint) {
        openWaypointSheet(editingWaypoint);
    }
    if (screen == Screen::Waypoints) {
        const int row = waypointList.selected();
        fillWaypointList();
        waypointList.select(row);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing: GPX lines, the recorded track, waypoint pins

void MapView::drawPlaces() {
    double left;
    double top;
    double right;
    double bottom;
    viewBounds(left, top, right, bottom);
    // GPX files under everything else of the user's
    for (int f = 0; f < gpxFileCount; f++) {
        const GpxLayer* layer = gpxFiles[f].shown ? gpxFiles[f].layer : nullptr;
        if (layer == nullptr) {
            continue;
        }
        const int shift = GpxLayer::CACHE_ZOOM - zoom;
        auto toWorld = [&](uint32_t value) {
            return shift >= 0 ? static_cast<double>(value >> shift) + 0.5 : std::ldexp(static_cast<double>(value), -shift);
        };
        // World to screen, turned like the map
        auto project = [&](uint32_t valueX, uint32_t valueY, float& x, float& y) {
            double sx;
            double sy;
            worldToScreenOffset(toWorld(valueX) - centerX, toWorld(valueY) - centerY, sx, sy);
            x = static_cast<float>(sx + width / 2.0);
            y = static_cast<float>(sy + height / 2.0);
        };
        const uint16_t pinColor = monochrome ? palette->text : GPX_ROUTE_COLOR;
        for (int l = 0; l < layer->lineCount(); l++) {
            const GpxLayer::Line& line = layer->line(l);
            // Lines that cannot reach the screen are skipped by their box
            if (toWorld(line.maxX) < left - 4.0 || toWorld(line.minX) > right + 4.0 || toWorld(line.maxY) < top - 4.0 ||
                toWorld(line.minY) > bottom + 4.0) {
                continue;
            }
            const uint16_t color = monochrome ? palette->text : (line.kind == GpxKind::RoutePoint ? GPX_ROUTE_COLOR : GPX_TRACK_COLOR);
            const uint32_t* points = layer->points() + 2 * line.first;
            float lastX;
            float lastY;
            project(points[0], points[1], lastX, lastY);
            for (uint32_t i = 1; i < line.count; i++) {
                float x;
                float y;
                project(points[2 * i], points[2 * i + 1], x, y);
                const float dx = x - lastX;
                const float dy = y - lastY;
                if (dx * dx + dy * dy < MIN_STEP_PIXELS * MIN_STEP_PIXELS && i + 1 < line.count) {
                    continue;
                }
                const bool visible = !((lastX < 0 && x < 0) || (lastY < 0 && y < 0) || (lastX > width && x > width) || (lastY > height && y > height));
                if (visible) {
                    canvas.line(lastX, lastY, x, y, 3.0f, color);
                }
                lastX = x;
                lastY = y;
            }
        }
        for (int p = 0; p < layer->pinCount(); p++) {
            float x;
            float y;
            project(layer->pin(p).x, layer->pin(p).y, x, y);
            if (x > -8 && y > -8 && x < width + 8 && y < height + 8) {
                canvas.fillCircle(x, y, 4.0f, palette->halo);
                canvas.fillCircle(x, y, 2.5f, pinColor);
            }
        }
    }
    // The track being recorded, above the map and the GPX lines
    if (recorder.pointCount() > 1) {
        const uint16_t color = monochrome ? palette->text : TRACK_COLOR;
        const TrackRecorder::Point* points = recorder.points();
        float lastX = 0.0f;
        float lastY = 0.0f;
        worldToScreen(points[0].lat, points[0].lon, lastX, lastY);
        for (int i = 1; i < recorder.pointCount(); i++) {
            float x;
            float y;
            worldToScreen(points[i].lat, points[i].lon, x, y);
            const float dx = x - lastX;
            const float dy = y - lastY;
            if (dx * dx + dy * dy < MIN_STEP_PIXELS * MIN_STEP_PIXELS && i + 1 < recorder.pointCount()) {
                continue;
            }
            if (!recorder.segmentStartsAt(i)) {
                canvas.line(lastX, lastY, x, y, 3.0f, color);
            }
            lastX = x;
            lastY = y;
        }
    }
    // Waypoint pins at every zoom level, all the same size; the one whose sheet is open has the accent colour
    const PinStyle style { monochrome ? palette->text : PIN_COLOR, theme.accent, palette->halo, palette->text };
    for (int i = 0; i < waypoints.count(); i++) {
        float x;
        float y;
        if (!worldToScreen(waypoints.at(i).lat, waypoints.at(i).lon, x, y)) {
            continue;
        }
        drawPin(canvas, x, y, style, sheet.isOpen() && i == selectedWaypoint);
    }
    // The point a new waypoint is being made for
    float x;
    float y;
    if (sheet.isOpen() && sheet.mode() == WaypointSheet::Mode::New && worldToScreen(sheetLat, sheetLon, x, y)) {
        drawPin(canvas, x, y, style, true);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Track recording

void MapView::drawRecording() {
    if (!recorder.isActive()) {
        return;
    }
    const uint32_t seconds = recorder.elapsedMs(nowMs()) / 1000;
    char text[48];
    const int used = seconds >= 3600 ? snprintf(text, sizeof(text), "%u:%02u:%02u", static_cast<unsigned>(seconds / 3600),
                                                static_cast<unsigned>(seconds / 60 % 60), static_cast<unsigned>(seconds % 60))
                                     : snprintf(text, sizeof(text), "%u:%02u", static_cast<unsigned>(seconds / 60), static_cast<unsigned>(seconds % 60));
    if (controls.isVisible() && used > 0) {
        char distance[24];
        formatDistance(recorder.distanceMetres(), distance, sizeof(distance));
        snprintf(text + used, sizeof(text) - static_cast<size_t>(used), "  %s", distance);
    }
    if (recorder.state() == TrackRecorder::State::Paused) {
        const size_t length = strlen(text);
        snprintf(text + length, sizeof(text) - length, "  paused");
    }
    Font& font = overlay.font();
    const int textWidth = font.textWidth(text);
    const int boxWidth = textWidth + 24;
    const int boxHeight = font.lineHeight() + 6;
    const int x = width - boxWidth - 4;
    const int y = 4;
    canvas.fillRoundRect(x, y, boxWidth, boxHeight, boxHeight / 2, theme.panel, theme.panelOpacity);
    canvas.fillCircle(static_cast<float>(x + 10), static_cast<float>(y + boxHeight / 2), 4.0f, monochrome ? palette->text : 0xF800);
    font.drawText(canvas, x + 18, y + 3, text, theme.text);
}

void MapView::openTrackMenu() {
    beginPopup(Popup::Track, "Track");
    // What can happen next, then the list of tracks: 0 start, 1 pause, 2 resume, 3 stop, 4 recorded tracks
    if (recorder.state() == TrackRecorder::State::Off) {
        menu.add("Start recording");
        popupActions[popupActionCount++] = 0;
    } else {
        menu.add(recorder.state() == TrackRecorder::State::Recording ? "Pause" : "Resume");
        popupActions[popupActionCount++] = recorder.state() == TrackRecorder::State::Recording ? 1 : 2;
        menu.add("Stop and save");
        popupActions[popupActionCount++] = 3;
    }
    menu.add("Recorded tracks");
    popupActions[popupActionCount++] = 4;
    layoutPopup();
}

void MapView::chooseTrackMenu(int action) {
    const uint32_t now = nowMs();
    switch (action) {
        case 0:
            if (!recorder.start(io, rootPath, now)) {
                showToast("Could not start the track file");
            } else {
                showToast(position.hasFix() ? "Recording" : "Recording, waiting for a GPS position");
            }
            break;
        case 1:
            recorder.pause(now);
            break;
        case 2:
            recorder.resume(now);
            break;
        case 3: {
            recorder.stop(io, now);
            char text[64];
            snprintf(text, sizeof(text), "Saved tracks/%s.gpx", recorder.fileName());
            showToast(text);
            break;
        }
        case 4:
            returnToMenu = true;
            openTracks();
            break;
        default:
            break;
    }
    dirty = true;
}

void MapView::openTracks() {
    trackList.clear();
    char title[ListView::TEXT_LENGTH];
    snprintf(title, sizeof(title), "Tracks");
    trackList.setTitle(title);
    char folder[PATH_MAX_LENGTH];
    snprintf(folder, sizeof(folder), "%s/tracks", rootPath);
    struct Context {
        ListView* list;
        const char* folder;
    } context { &trackList, folder };
    auto list = [&] {
        listDirectory(folder, [](const char* fileName, bool isDirectory, void* data) {
            auto* context = static_cast<Context*>(data);
            const size_t length = strlen(fileName);
            if (isDirectory || length < 5 || strcmp(fileName + length - 4, ".gpx") != 0 || context->list->count() >= ListView::MAX_ITEMS) {
                return true;
            }
            char path[PATH_MAX_LENGTH];
            snprintf(path, sizeof(path), "%s/%s", context->folder, fileName);
            struct stat info {};
            char detail[ListView::TEXT_LENGTH];
            snprintf(detail, sizeof(detail), "%.1f KB", stat(path, &info) == 0 ? info.st_size / 1024.0 : 0.0);
            context->list->add(fileName, detail);
            return true;
        }, &context);
        return true;
    };
    runOnCard(io, list, IoPriority::Background);
    if (trackList.count() == 0) {
        showToast("No tracks yet");
        return;
    }
    // Where the files are, so they can be copied off the card: the path on two lines, split at a folder
    char hint[160];
    const char* split = strstr(folder, "/user/app/");
    if (split != nullptr) {
        split += strlen("/user/app/");
        snprintf(hint, sizeof(hint), "Files in %.*s\n%s", static_cast<int>(split - folder), folder, split);
    } else {
        snprintf(hint, sizeof(hint), "Files in %s", folder);
    }
    trackList.setHint(hint);
    trackList.layoutFullScreen(width, height, overlay.font());
    trackList.select(0);
    trackList.setHighlighted(false);
    screen = Screen::Tracks;
    dirty = true;
}

// ---------------------------------------------------------------------------------------------------------------
// GPX files

void MapView::openGpxFiles() {
    gpxList.clear();
    gpxList.setTitle("GPX files");
    // Files dropped into gpx/ on the card; the list keeps the files already known with their state
    struct Context {
        MapView* view;
    } context { this };
    auto list = [&] {
        listDirectory(gpxFolder, [](const char* fileName, bool isDirectory, void* data) {
            MapView* view = static_cast<Context*>(data)->view;
            const size_t length = strlen(fileName);
            if (isDirectory || length < 5 || (strcmp(fileName + length - 4, ".gpx") != 0 && strcmp(fileName + length - 4, ".GPX") != 0)) {
                return true;
            }
            for (int i = 0; i < view->gpxFileCount; i++) {
                if (strcmp(view->gpxFiles[i].name, fileName) == 0) {
                    return true;
                }
            }
            if (view->gpxFileCount < MAX_GPX_FILES) {
                GpxFile& file = view->gpxFiles[view->gpxFileCount++];
                file = {};
                snprintf(file.name, sizeof(file.name), "%s", fileName);
            }
            return true;
        }, &context);
        return true;
    };
    runOnCard(io, list, IoPriority::Background);
    for (int i = 0; i < gpxFileCount; i++) {
        const GpxFile& file = gpxFiles[i];
        gpxList.add(file.name, i == gpxImporting ? "reading" : (file.failed ? "not readable" : (file.shown ? "shown" : "hidden")));
    }
    if (gpxFileCount == 0) {
        char text[96];
        snprintf(text, sizeof(text), "No GPX files: copy them into %s", gpxFolder);
        showToast(text);
        return;
    }
    setListHint(gpxList, "Enter: show/hide  Esc: back", "Tap: show/hide  Swipe right: back");
    gpxList.layoutFullScreen(width, height, overlay.font());
    gpxList.select(0);
    gpxList.setHighlighted(false);
    screen = Screen::GpxFiles;
    dirty = true;
}

void MapView::toggleGpxFile() {
    const int index = gpxList.selected();
    if (index < 0 || index >= gpxFileCount) {
        return;
    }
    GpxFile& file = gpxFiles[index];
    if (file.shown) {
        file.shown = false;
        // The memory goes, the cache stays for the next time
        if (file.layer != nullptr) {
            file.layer->~GpxLayer();
            largeFree(file.layer);
            file.layer = nullptr;
        }
    } else if (file.layer != nullptr) {
        file.shown = true;
    } else {
        char path[PATH_MAX_LENGTH];
        snprintf(path, sizeof(path), "%s/%s", gpxFolder, file.name);
        if (!gpxImport.start(io, path)) {
            showToast("Another file is still being read");
            return;
        }
        gpxImporting = index;
        file.failed = false;
    }
    saveShownGpx();
    gpxList.setDetail(index, index == gpxImporting ? "reading" : (file.shown ? "shown" : "hidden"));
    dirty = true;
}

void MapView::pollGpxImport() {
    const GpxImport::State state = gpxImport.state();
    if (gpxImporting < 0 || (state != GpxImport::State::Done && state != GpxImport::State::Failed)) {
        return;
    }
    GpxFile& file = gpxFiles[gpxImporting];
    const int index = gpxImporting;
    gpxImporting = -1;
    if (state == GpxImport::State::Done) {
        file.layer = gpxImport.takeLayer();
        file.shown = file.layer != nullptr;
    } else {
        char text[96];
        snprintf(text, sizeof(text), "%s: %s", file.name, gpxImport.error());
        gpxImport.acknowledge();
        file.failed = true;
        file.shown = false;
        showToast(text);
    }
    if (screen == Screen::GpxFiles) {
        gpxList.setDetail(index, file.failed ? "not readable" : (file.shown ? "shown" : "hidden"));
    }
    saveShownGpx();
    // The next file that was shown last time
    for (int i = 0; i < gpxFileCount; i++) {
        if (gpxFiles[i].shown && gpxFiles[i].layer == nullptr && !gpxFiles[i].failed) {
            char path[PATH_MAX_LENGTH];
            snprintf(path, sizeof(path), "%s/%s", gpxFolder, gpxFiles[i].name);
            if (gpxImport.start(io, path)) {
                gpxImporting = i;
            }
            break;
        }
    }
    dirty = true;
}

void MapView::saveShownGpx() {
    char text[MAX_GPX_FILES * 50 + 1] = "";
    size_t used = 0;
    for (int i = 0; i < gpxFileCount; i++) {
        if (gpxFiles[i].shown || i == gpxImporting) {
            used += static_cast<size_t>(snprintf(text + used, sizeof(text) - used, "%s\n", gpxFiles[i].name));
        }
    }
    char path[PATH_MAX_LENGTH];
    char temporary[PATH_MAX_LENGTH + 8];
    snprintf(path, sizeof(path), "%s/gpxshown.txt", rootPath);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    auto write = [&] { return io.writeFile(path, temporary, reinterpret_cast<const uint8_t*>(text), used, false); };
    runOnCard(io, write, IoPriority::Background);
}

void MapView::loadShownGpx() {
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/gpxshown.txt", rootPath);
    FileBuffer file;
    auto read = [&] { return file.load(path); };
    if (!runOnCard(io, read, IoPriority::Background) || file.size() == 0) {
        return;
    }
    // One file name per line; they are read one after another in the background
    const char* text = reinterpret_cast<const char*>(file.bytes());
    size_t start = 0;
    for (size_t i = 0; i <= file.size() && gpxFileCount < MAX_GPX_FILES; i++) {
        if (i == file.size() || text[i] == '\n') {
            if (i > start && i - start < sizeof(GpxFile::name)) {
                GpxFile& entry = gpxFiles[gpxFileCount++];
                entry = {};
                memcpy(entry.name, text + start, i - start);
                entry.name[i - start] = '\0';
                entry.shown = true;
            }
            start = i + 1;
        }
    }
    for (int i = 0; i < gpxFileCount; i++) {
        char gpxPath[PATH_MAX_LENGTH];
        snprintf(gpxPath, sizeof(gpxPath), "%s/%s", gpxFolder, gpxFiles[i].name);
        if (gpxImport.start(io, gpxPath)) {
            gpxImporting = i;
            break;
        }
    }
}

} // namespace maps
