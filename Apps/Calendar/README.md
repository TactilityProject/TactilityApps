# Calendar

Read-only CalDAV calendar viewer for Tactility.

## Features

- Syncs events from a CalDAV calendar collection using an authenticated `REPORT` request
- Downloads all calendar events and caches them locally for subsequent app opens
- Shows events in the next 30 days, including recurring instances expanded by the server
- Lets you configure a calendar collection URL, username and password
- Stores account settings using Tactility preferences
- Provides Sync as the page bar's main action
- Provides a selectable month page, a full-screen agenda for the selected day, and a separate agenda page for the next 30 days

## Setup

Open Settings and enter the HTTPS URL of a calendar collection (not just the server's discovery URL), along with the account credentials. Save the settings, then use Sync to download and cache the full calendar. Cached events are loaded when the app opens; Sync downloads the calendar again and refreshes the displayed data.

The device must have Wi-Fi connectivity and a valid system clock. HTTPS certificates are checked against the certificate bundle in the firmware; servers with self-signed or otherwise untrusted certificates are not supported. Credentials are stored in the app's preferences on device storage.

Event times are displayed in the time zone supplied by the calendar; they are not converted to the device's local time zone.
