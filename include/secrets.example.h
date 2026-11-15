#pragma once

// Copy this file to include/secrets.h (git-ignored) and fill it in.
// Without secrets.h the build uses these placeholders and warns.

// The gateway's hmac_key from /etc/alert-gateway.toml, as 64 hex digits; gateway/install.sh
// prints this line. The placeholder key rejects every reply from a real gateway.
#define CLOCK_GATEWAY_KEY "0000000000000000000000000000000000000000000000000000000000000000"

// Password for /wifi, /update and /debug/* (user "admin"), and the setup network's WPA2 key.
#define CLOCK_ADMIN_PASSWORD "change-me"
