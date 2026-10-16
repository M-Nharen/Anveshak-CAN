#!/usr/bin/env bash
# Started by the desktop session (XDG autostart): waits for the dashboard, then shows the Bench page
# full-screen on the 7" display. Chromium is restarted if it ever exits.
url="http://localhost:8080/?kiosk"
until curl -fs http://localhost:8080/api/health >/dev/null; do sleep 1; done
browser="$(command -v chromium || command -v chromium-browser)"
while true; do
  "$browser" --kiosk --noerrdialogs --disable-infobars --no-first-run --password-store=basic \
    --disable-session-crashed-bubble --disable-features=Translate --check-for-update-interval=31536000 \
    --app="$url"
  sleep 2
done
