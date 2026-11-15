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
    if (running && status.total > 0) {
        add("%s %d%%, %d of %d tiles", status.state == Downloader::State::Updating ? "Updating" : "Downloading",
            static_cast<int>(status.done * 100 / status.total), static_cast<int>(status.done), static_cast<int>(status.total));
        // The size so far against an estimate from the tiles done
        char done[16];
        char estimate[16];
        formatBytes(status.bytes, done, sizeof(done));
        const uint64_t total = status.done > 0 ? status.bytes * static_cast<uint64_t>(status.total) / static_cast<uint64_t>(status.done) : 0;
        formatBytes(total, estimate, sizeof(estimate));
        add("%s of about %s", done, status.done > 0 ? estimate : "?");
        const double rate = transferRate.bytesPerSecond();
        if (rate > 0.0) {
            char speed[16];
            formatBytes(static_cast<uint64_t>(rate), speed, sizeof(speed));
            const int64_t left = transferRate.secondsLeft(status.bytes, total);
            char remaining[24] = "";
            if (left >= 0) {
                formatDuration(left, remaining, sizeof(remaining));
            }
            add("%s/s%s%s", speed, left >= 0 ? ", " : "", left >= 0 ? remaining : "");
            if (left >= 0) {
                snprintf(lines[count - 1] + strlen(lines[count - 1]), sizeof(lines[0]) - strlen(lines[count - 1]), " left");
            }
        } else {
            add("Measuring the speed...");
        }
    } else if (status.state == Downloader::State::Connecting) {
        add("%s", status.message);
    } else if (downloader.isBrowsing() && browseTotal > 0) {
        add("Fetching detail for the screen:");
        add("%d of %d tiles", std::min(browseDone + 1, browseTotal), browseTotal);
        add("They are kept in Downloaded maps,");
        add("region \"browsed\".");
    } else if (pausedRegion[0] != '\0') {
        add("Paused. Resume continues where it stopped.");
    } else {
        add("%s", status.message[0] != '\0' ? status.message : "Nothing is downloading.");
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
    if (!resumeChecked) {
        // A download that was interrupted continues when the map opens again
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
        } else if (status.state == Downloader::State::Done && (lastStatus.state == Downloader::State::Downloading ||
            lastStatus.state == Downloader::State::Connecting)) {
            retries = 0;
        }
        // After a finished download the next unfinished region continues, but not one paused on purpose
        // (a short download can go from Connecting straight to Done between two polls)
        if (status.state == Downloader::State::Done && (lastStatus.state == Downloader::State::Downloading ||
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
