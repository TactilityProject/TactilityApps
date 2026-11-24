// SPDX-License-Identifier: GPL-3.0-or-later
#include "MapView.h"

#include "../platform/Platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

constexpr uint32_t STATUS_POLL_MS = 500;
constexpr uint32_t MESSAGE_SHOW_MS = 4000;
/** A failed download says so longer, and tries again after a while, a few times. */
constexpr uint32_t FAILED_MESSAGE_SHOW_MS = 10000;
constexpr uint32_t RETRY_AFTER_MS = 30000;
constexpr int MAX_RETRIES = 3;
constexpr uint32_t GAP_POLL_MS = 300;
/** The message about missing detail comes back at most this often. */
constexpr uint32_t GAP_HINT_MS = 30000;

} // namespace

void MapView::openDownloadCard() {
    downloadCard.open();
    fillDownloadCard();
    controls.hide();
    dirty = true;
}

void MapView::fillDownloadCard() {
    const Downloader::Status& status = lastStatus;
    const bool running = status.state == Downloader::State::Downloading || status.state == Downloader::State::Updating ||
        status.state == Downloader::State::Connecting;
    char lines[5][64];
    const char* rows[5];
    int count = 0;
    auto add = [&](const char* format, auto... values) {
        snprintf(lines[count], sizeof(lines[count]), format, values...);
        rows[count] = lines[count];
        count++;
    };
    // Line 1 says what was fetched, line 2 how big it is and how fast, line 3 the state in words.
    const char* name = statusTitle[0] != '\0' ? statusTitle : status.region;
    if (running && status.trails) {
        // A trails query: no tiles to count, the server takes up to a minute
        add("%s", status.message[0] != '\0' ? status.message : "Downloading trails");
        add("The trail server can take");
        add("up to a minute to answer.");
    } else if (running && status.total > 0) {
        formatTileLine(name, status.done, status.total, lines[count], sizeof(lines[count]));
        rows[count] = lines[count];
        count++;
        // The time left comes from an estimate of the whole size, made from the tiles done so far
        const uint64_t total = status.done > 0 ? status.bytes * static_cast<uint64_t>(status.total) / static_cast<uint64_t>(status.done) : 0;
        const double rate = transferRate.bytesPerSecond();
        formatSizeLine(status.bytes, rate, rate > 0.0 ? transferRate.secondsLeft(status.bytes, total) : -1, lines[count], sizeof(lines[count]));
        rows[count] = lines[count];
        count++;
        add("%s", status.state == Downloader::State::Updating ? "Updating" : (status.terrain ? "Downloading terrain" : (status.trails ? "Downloading trails" : "Downloading")));
    } else if (status.state == Downloader::State::Connecting) {
        add("%s", status.message);
    } else if (downloader.isBrowsing() && browseTotal > 0) {
        add("Fetching detail for the screen:");
        add("%d of %d tiles", std::min(browseDone + 1, browseTotal), browseTotal);
        add("They are kept in Downloaded maps,");
        add("region \"browsed\".");
    } else if (pausedRegion[0] != '\0') {
        add("Paused: continues when you press Resume");
    } else {
        add("%s", status.state == Downloader::State::Done && !status.paused ? "Done" : status.message[0] != '\0' ? status.message : "Nothing is downloading.");
    }
    // Tiles fetched while browsing can't be paused or cancelled from the card, only the card closed
    const bool browsing = !running && status.state != Downloader::State::Connecting && downloader.isBrowsing() && browseTotal > 0;
    const char* title = browsing ? "Map detail" : statusTitle[0] != '\0' ? statusTitle : status.region;
    downloadCard.set(title, rows, count, !running, browsing);
    downloadCard.layout(width, height, overlay.font());
}

void MapView::handleCardButton(DownloadCard::Button button) {
    dirty = true;
    switch (button) {
        case DownloadCard::Button::PauseResume: {
            const Downloader::Status status = downloader.status();
            const bool running = status.state == Downloader::State::Downloading || status.state == Downloader::State::Connecting;
            if (running) {
                snprintf(pausedRegion, sizeof(pausedRegion), "%s", status.region);
                downloader.cancel();
                showToast("Download paused");
            } else if (status.state == Downloader::State::Updating) {
                downloader.cancel();
                showToast("Update paused, it continues in idle time");
            } else if (pausedRegion[0] != '\0') {
                downloader.download(pausedRegion);
                pausedRegion[0] = '\0';
                showToast("Download continues");
            }
            downloadCard.close();
            break;
        }
        case DownloadCard::Button::Cancel: {
            // Cancelling a download deletes the part already fetched, after asking
            const Downloader::Status status = downloader.status();
            const char* region = status.region[0] != '\0' ? status.region : pausedRegion;
            downloadCard.close();
            if (region[0] == '\0' || status.state == Downloader::State::Updating) {
                downloader.cancel();
                break;
            }
            snprintf(cancelRegion, sizeof(cancelRegion), "%s", region);
            char message[96];
            snprintf(message, sizeof(message), "Stop and delete the part of %s downloaded so far?", statusTitle[0] != '\0' ? statusTitle : region);
            dialogPurpose = DialogPurpose::CancelDownload;
            dialog.open(message, "Delete", true);
            dialog.layout(width, height, overlay.font());
            break;
        }
        case DownloadCard::Button::Close:
            downloadCard.close();
            break;
        default:
            break;
    }
}

void MapView::pollDownloader(uint32_t now) {
    TileKey arrived[Downloader::MAX_ARRIVED];
    int arrivedCount = 0;
    if (downloader.takeTilesChanged(arrived, arrivedCount)) {
        worker.tilesArrived(arrived, arrivedCount);
        dirty = true;
    }
    // The arrived list again for the z12 tiles of new files (no more room on this stack)
    static_assert(Downloader::MAX_TERRAIN_ARRIVED <= Downloader::MAX_ARRIVED);
    int terrainFileCount = 0;
    if (downloader.takeTerrainChanged(arrived, terrainFileCount)) {
        // Collected while a heights run goes on: its files redraw the tiles once, not once per file (each redraw also
        // held up the next request, which waits for a complete view)
        if (terrainFileCount < 0 || pendingTerrainCount + terrainFileCount > MAX_PENDING_TERRAIN) {
            pendingTerrainAll = true;
        } else {
            for (int i = 0; i < terrainFileCount; i++) {
                pendingTerrain[pendingTerrainCount++] = arrived[i];
            }
        }
        if (pendingTerrainSince == 0) {
            pendingTerrainSince = now == 0 ? 1 : now;
        }
    }
    const bool heightsRunning = lastStatus.terrain && lastStatus.state == Downloader::State::Downloading;
    if (pendingTerrainSince != 0 && (!heightsRunning || now - pendingTerrainSince >= TERRAIN_REDRAW_MAX_MS)) {
        // The tiles they touch are drawn again with them, the old ones show until then
        worker.terrainChanged(pendingTerrain, pendingTerrainAll ? -1 : pendingTerrainCount);
        pendingTerrainCount = 0;
        pendingTerrainAll = false;
        pendingTerrainSince = 0;
        dirty = true;
    }
    // Tiles drawn without heights ask for them (the downloader does nothing while Terrain is off)
    const int missingCount = worker.takeMissingTerrain(arrived, Downloader::MAX_ARRIVED);
    for (int i = 0; i < missingCount; i++) {
        downloader.queueTerrain(arrived[i]);
    }
    // And those without trail files ask for them (nothing happens while Marked trails is off)
    const int missingTrailCount = worker.takeMissingTrails(arrived, Downloader::MAX_ARRIVED);
    for (int i = 0; i < missingTrailCount; i++) {
        downloader.queueTrails(arrived[i]);
    }
    if (!resumeChecked && !downloader.isBusy()) {
        // A download that was interrupted continues when the map opens again (never in place of a running one)
        resumeChecked = true;
        int unfinished = -1;
        RegionMeta meta {};
        auto find = [&] {
            for (int i = 0; i < store.regionCount() && unfinished < 0; i++) {
                if (strcmp(store.regionName(i), pausedRegion) == 0) {
                    continue;
                }
                char path[PATH_MAX_LENGTH];
                snprintf(path, sizeof(path), "%s/maps/%s/meta.json", rootPath, store.regionName(i));
                if (readRegionMeta(path, meta) && !meta.isComplete()) {
                    unfinished = i;
                }
            }
            return true;
        };
        runOnCard(io, find, IoPriority::Background);
        if (unfinished >= 0) {
            logInfo("Continuing the download of %s at tile %d of %d", store.regionName(unfinished), static_cast<int>(meta.done), static_cast<int>(meta.total));
            downloader.download(store.regionName(unfinished));
        }
    }
    if (retryAt != 0 && static_cast<int32_t>(now - retryAt) >= 0) {
        retryAt = 0;
        resumeChecked = false;
    }
    if (now - lastStatusPoll < STATUS_POLL_MS) {
        return;
    }
    lastStatusPoll = now;
    const Downloader::Status status = downloader.status();
    // Tile cache slots given to a large download come back once nothing downloads, and not while a search index is
    // built (its tables grow; it was started with the memory free that it had)
    if (status.state != Downloader::State::Connecting && status.state != Downloader::State::Downloading &&
        status.state != Downloader::State::Updating && !downloader.isBrowsing() && worker.cacheShrunk() && !worker.isPreparing() &&
        navigator.state() != Navigator::State::Searching && searchIndexJob.state() != SearchIndexJob::State::Busy) {
        worker.restoreMemory();
    }
    downloader.browseProgress(browseDone, browseTotal);
    if (status.region[0] != '\0' && strcmp(status.region, statusTitleOf) != 0) {
        // The title users gave the region, not its short folder name
        snprintf(statusTitleOf, sizeof(statusTitleOf), "%s", status.region);
        snprintf(statusTitle, sizeof(statusTitle), "%s", status.region);
        RegionMeta meta {};
        char path[PATH_MAX_LENGTH];
        snprintf(path, sizeof(path), "%s/maps/%s/meta.json", rootPath, status.region);
        auto read = [&] { return readRegionMeta(path, meta); };
        if (runOnCard(io, read, IoPriority::Background)) {
            snprintf(statusTitle, sizeof(statusTitle), "%s", regionTitle(meta));
        }
    }
    if (status.state == Downloader::State::Downloading || status.state == Downloader::State::Updating) {
        transferRate.add(now, status.bytes);
    } else {
        transferRate.reset();
    }
    // The pill follows browsing too, and goes when it ends
    const bool browsingNow = downloader.isBrowsing();
    if (browsingNow != lastBrowsing || browseDone != lastBrowseDone || browseTotal != lastBrowseTotal) {
        lastBrowsing = browsingNow;
        lastBrowseDone = browseDone;
        lastBrowseTotal = browseTotal;
        dirty = true;
    }
    worker.setConversionsPaused(status.state == Downloader::State::Connecting || status.state == Downloader::State::Downloading ||
        status.state == Downloader::State::Updating);
    if (status.state != lastStatus.state || status.done != lastStatus.done || strcmp(status.message, lastStatus.message) != 0) {
        if (status.state != lastStatus.state && (status.state == Downloader::State::Done || status.state == Downloader::State::Failed)) {
            messageUntil = now + (status.state == Downloader::State::Failed ? FAILED_MESSAGE_SHOW_MS : MESSAGE_SHOW_MS);
        }
        // A download that stopped on an error (the network, or memory) continues by itself a little later
        if (status.state == Downloader::State::Failed && (lastStatus.state == Downloader::State::Downloading ||
            lastStatus.state == Downloader::State::Connecting) && strstr(status.message, "limit") == nullptr) {
            if (retries < MAX_RETRIES) {
                retries++;
                retryAt = now + RETRY_AFTER_MS;
                logInfo("Download of %s failed, trying again in %u s (%d of %d)", status.region, RETRY_AFTER_MS / 1000, retries, MAX_RETRIES);
            }
        } else if (status.state == Downloader::State::Done && !status.paused && (lastStatus.state == Downloader::State::Downloading ||
            lastStatus.state == Downloader::State::Connecting)) {
            retries = 0;
        }
        // After a finished download the next unfinished region continues, but not one paused on purpose
        // (a short download can go from Connecting straight to Done between two polls). A job that ended because a
        // new one replaced it is not finished: resuming then would cancel the new job (seen 15 Nov: two regions
        // took turns cancelling each other)
        if (status.state == Downloader::State::Done && !status.paused && (lastStatus.state == Downloader::State::Downloading ||
            lastStatus.state == Downloader::State::Connecting)) {
            resumeChecked = false;
        }
        dirty = true;
        // The Regions screen shows progress as it happens
        if (screen == Screen::Regions && status.state == Downloader::State::Done) {
            lastStatus = status;
            openRegions();
        }
    }
    lastStatus = status;
    if (downloadCard.isOpen()) {
        fillDownloadCard();
        dirty = true;
    }
    if (static_cast<int32_t>(now - messageUntil) >= 0 && messageUntil != 0) {
        messageUntil = 0;
        dirty = true;
    }
}

void MapView::pollGaps(uint32_t now) {
    if (now - lastGapPoll < GAP_POLL_MS) {
        return;
    }
    lastGapPoll = now;
    TileKey gaps[8];
    const int count = worker.takeGaps(gaps, 8);
    if (count == 0) {
        return;
    }
    if (settings.browseDownload && Downloader::isOnline()) {
        downloader.browse(gaps, count);
        dirty = true;
        return;
    }
    if (lastGapHint != 0 && now - lastGapHint < GAP_HINT_MS) {
        return;
    }
    lastGapHint = now;
    if (!settings.browseDownload) {
        showToast("Less detail here. Download this area for more.");
    } else {
        showToast("Offline: no detail here. Download this area on Wi-Fi.");
    }
}

} // namespace maps
