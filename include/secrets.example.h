#pragma once

// Copy this file to include/secrets.h (git-ignored) and fill it in.
// Without secrets.h the build uses these placeholders and warns.

// Map centre and region. The default is central Kyiv; put your own location in secrets.h.
#define CLOCK_HOME_LAT 50.4501f
#define CLOCK_HOME_LON 30.5234f
// Oblast name exactly as NEPTUN writes it in "region" (check /api/v1/threats), or nullptr.
#define CLOCK_HOME_REGION nullptr

// Map half-width, and the distance at which tracks switch the clock to the map.
#define CLOCK_MAP_RADIUS_KM 30.0f
#define CLOCK_WATCH_RADIUS_KM 25.0f

// alerts.in.ua location UID (31 = Kyiv city; see devs.alerts.in.ua) and API token.
// An empty token disables alerts: the clock then shows "NO ALERT DATA".
#define CLOCK_ALERTS_REGION_UID "31"
#define CLOCK_ALERTS_TOKEN ""

// Password for ArduinoOTA, /update and /debug/* (user "admin").
#define CLOCK_ADMIN_PASSWORD "change-me"
