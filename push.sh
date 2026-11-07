#!/bin/bash
# push.sh - publishes the source of this port (no game data) to a GitHub repository.
#   - copies only the source into .pushrepo/ (a git repository separate from PS2RC/.git)
#   - refuses to go on if a file looks like game data (disc images, EBOOT, generated VU1 headers, big files...)
#   - asks for the commit message and a GitHub token (pasted, not shown, never saved), then pushes.
# Usage: ./push.sh            (the repository URL is asked on the first run and kept in .pushrepo/.remote_url)
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$PWD"
STAGE="$ROOT/.pushrepo"
BRANCH="${BRANCH:-main}"

command -v git >/dev/null || { echo "git est introuvable"; exit 1; }
command -v rsync >/dev/null || { echo "rsync est introuvable"; exit 1; }

mkdir -p "$STAGE"
if [ ! -d "$STAGE/.git" ]; then
    git -C "$STAGE" init -q
    git -C "$STAGE" checkout -q -b "$BRANCH" 2>/dev/null || true
fi

# --- repository URL (asked once) -------------------------------------------------------------------------------
URLF="$STAGE/.git/remote_url"
if [ -s "$URLF" ]; then URL="$(cat "$URLF")"; else
    read -r -p "URL du dépôt GitHub (ex. https://github.com/moi/bully-psp.git) : " URL
    [ -n "$URL" ] || { echo "URL vide"; exit 1; }
    printf '%s\n' "$URL" > "$URLF"
fi
case "$URL" in https://github.com/*) ;; *) echo "URL attendue en https://github.com/... (reçu: $URL)"; exit 1;; esac

# --- copy the source --------------------------------------------------------------------------------------------
EXCLUDES=(
  --exclude '.git/' --exclude '__pycache__/' --exclude '*.pyc' --exclude '.DS_Store'
  --exclude '*.o' --exclude '*.a' --exclude '*.elf' --exclude '*.prx' --exclude '*.PBP' --exclude '*.pbp'
  --exclude '*.iso' --exclude '*.ISO' --exclude '*.bin' --exclude '*.BIN' --exclude '*.IMG' --exclude '*.img'
  --exclude '*.tgz' --exclude '*.tar' --exclude '*.zip' --exclude '*.log'
  --exclude 'build/' --exclude 'build-*/' --exclude 'obj/'
  --exclude 'local.toml'
  --exclude 'games/SLES-53561_v1.01'
)
# what the repository holds (everything else of the workspace stays on this computer)
rsync -a --delete "${EXCLUDES[@]}" "$ROOT/PS2RC/" "$STAGE/PS2RC/"
mkdir -p "$STAGE/packs"
rsync -a --delete "${EXCLUDES[@]}" "$ROOT/packs/SLES-53561_v1.01/" "$STAGE/packs/SLES-53561_v1.01/"
rsync -a --delete "${EXCLUDES[@]}" "$ROOT/tools/" "$STAGE/tools/"
for f in README.md push.sh env.sh; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" "$STAGE/$f"; done
# PS2RC's licence must travel with its code (licence section 2(c))
cp "$ROOT/PS2RC/LICENSE" "$STAGE/LICENSE"; cp "$ROOT/PS2RC/NOTICE.md" "$STAGE/NOTICE.md"
# machine settings: an example with placeholders instead of this computer's paths
cat > "$STAGE/local.toml.example" <<'EOF'
# PS2RC per-machine settings (copy to local.toml and to PS2RC/local.toml, then edit the paths).
# Everything below work/ is made from YOUR disc: never share it.
[paths]
work = "/chemin/vers/BULLYPS2/work"
analysis = "/chemin/vers/BULLYPS2/work/analysis"

[games."SLES-53561_v1.01"]
disc = "/chemin/vers/BULLYPS2/work/disc"
EOF
cat > "$STAGE/.gitignore" <<'EOF'
work/
dist/
stage/
ppsspp-memstick/
_to_delete/
downloads/
refdocs/
local.toml
PS2RC/local.toml
PS2RC/games/SLES-53561_v1.01
*.iso
*.ISO
*.PBP
*.elf
*.o
__pycache__/
.pushrepo/
EOF
# the env files carry this computer's paths: make them relative to where they are sourced from
if [ -f "$STAGE/env.sh" ]; then
    cat > "$STAGE/env.sh" <<'EOF'
# source this file from the workspace root: . ./env.sh
export UPR_WORKSPACE="$(pwd)"
export PS2RC_LOCAL="$(pwd)/local.toml"
EOF
fi

# --- safety: nothing that looks like game data -----------------------------------------------------------------
BAD=$(cd "$STAGE" && find . -path ./.git -prune -o -type f \( \
        -iname '*.iso' -o -iname '*.img' -o -iname '*.dir' -o -iname '*.pbp' -o -iname '*.elf' -o -iname '*.prx' \
        -o -iname '*.bin' -o -iname '*.code' -o -iname '*.mem' -o -iname '*.pkl' -o -iname '*.at3' -o -iname '*.pss' \
        -o -iname '*.pmf' -o -iname '*.vag' -o -iname 'SLES_*' -o -name 'hle_vu1_aot.h' -o -name 'hle_vu1_fast.h' \
        -o -name 'non_abi.json' -o -name 'PORT_STATE.md' -o -name 'discmap.txt' -o -name 'iso_meta.bin' \
        -o -name '*_gen.h' -o -size +4M \) -print)
BAD=$(printf '%s\n' "$BAD" | grep -v '^\./PS2RC/docs/logo.png$' | grep -v '^$' || true)
if [ -n "$BAD" ]; then
    echo "ARRÊT : ces fichiers ressemblent à des données du jeu ou sont trop gros, rien n'est poussé :"
    printf '  %s\n' $BAD
    exit 1
fi
for d in work dist stage ppsspp-memstick _to_delete; do
    [ -e "$STAGE/$d" ] && { echo "ARRÊT : $d/ ne doit pas être publié"; exit 1; }
done

# --- commit -----------------------------------------------------------------------------------------------------
cd "$STAGE"
git add -A
if git diff --cached --quiet; then
    echo "Rien de nouveau à publier."
else
    git diff --cached --stat | tail -5
    read -r -p "Message de commit : " MSG
    [ -n "$MSG" ] || { echo "Message vide : abandon"; exit 1; }
    git -c user.name="${GIT_NAME:-$(git config --global user.name || echo BULLYPS2)}" \
        -c user.email="${GIT_EMAIL:-$(git config --global user.email || echo bullyps2@users.noreply.github.com)}" \
        commit -q -m "$MSG"
fi

# --- push with a token (pasted, not shown, not saved: sent as an HTTP header for this one command) -------------
read -r -s -p "Token GitHub (droit d'écriture sur le dépôt, collé sans affichage) : " TOKEN; echo
[ -n "$TOKEN" ] || { echo "Token vide : abandon"; exit 1; }
AUTH=$(printf 'x-access-token:%s' "$TOKEN" | base64 | tr -d '\n')
unset TOKEN
git -c credential.helper= -c http.extraHeader="Authorization: Basic $AUTH" push "$URL" "HEAD:$BRANCH"
unset AUTH
echo "Poussé sur $URL ($BRANCH)."
