#!/bin/bash
# ppsspp_linux.sh STAGE_DIR APP SECONDS [SHOT_PREFIX] - boolean PPSSPP check on Linux (Xvfb), with screenshots.
# Copies STAGE_DIR to the PPSSPP memstick as PSP/GAME/APP, runs PPSSPPSDL for SECONDS under Xvfb, takes a
# screenshot every 5 s (SHOT_PREFIX_NN.png), then prints session.log. PPSSPP gives booleans, never speed numbers.
set -u
STAGE=$1; APP=$2; SECS=$3; SHOT=${4:-}
PP=${PPSSPP_ROOT:-/home/claude/dl/squashfs-root}
MS=$HOME/.config/ppsspp
mkdir -p "$MS/PSP/GAME" "$MS/PSP/SYSTEM"
rm -rf "$MS/PSP/GAME/$APP"; cp -al "$STAGE" "$MS/PSP/GAME/$APP" 2>/dev/null || cp -r "$STAGE" "$MS/PSP/GAME/$APP"
# EBOOT, cfg and logs must be real copies (the emulator writes next to them)
for f in EBOOT.PBP ps2rc.cfg; do [ -f "$STAGE/$f" ] && { rm -f "$MS/PSP/GAME/$APP/$f"; cp "$STAGE/$f" "$MS/PSP/GAME/$APP/$f"; }; done
cat > "$MS/PSP/SYSTEM/ppsspp.ini" <<INI
[General]
FirstRun = False
CheckForNewVersion = False
ReportingHost = default
[Graphics]
GraphicsBackend = 0 (OPENGL)
InternalResolution = 1
FrameSkip = 0
[SystemParam]
PSPModel = 1
[CPU]
CPUSpeed = 333
INI
export DISPLAY=:93 SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy
Xvfb :93 -screen 0 960x544x24 >/dev/null 2>&1 & XV=$!
sleep 2
"$PP/AppRun" "$MS/PSP/GAME/$APP/EBOOT.PBP" > /tmp/ppsspp_$APP.out 2>&1 & PID=$!
t=0; n=0
while [ $t -lt $SECS ]; do
  sleep 5; t=$((t+5))
  kill -0 $PID 2>/dev/null || { echo "PPSSPP exited at ${t}s"; break; }
  if [ -n "$SHOT" ]; then n=$((n+1)); import -window root "$(printf '%s_%02d.png' "$SHOT" $n)" 2>/dev/null; fi
done
kill $PID 2>/dev/null; sleep 1; kill -9 $PID 2>/dev/null; kill $XV 2>/dev/null
echo "--- session.log"; cat "$MS/PSP/GAME/$APP/session.log" 2>/dev/null | tail -40
echo "--- ppsspp stdout tail"; tail -5 /tmp/ppsspp_$APP.out
