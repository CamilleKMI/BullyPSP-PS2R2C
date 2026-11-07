# Bully / Canis Canem Edit (PS2, SLES-53561) → PSP — portage PS2RC

Based on PS2RC by SandstoneDev
https://github.com/SandstoneDev/PS2RC
Community: https://discord.gg/wV8x46ZTDu

Portage expérimental, non affilié à Rockstar Games, Take-Two ou Sony, et non approuvé par eux ni par SandstoneDev.
**Ce dépôt ne contient aucune donnée du jeu** : pas de fichiers du disque, pas de code du jeu traduit, pas
d'EBOOT. Chaque utilisateur part de **sa propre copie** du disque PAL SLES-53561 (VER 1.01). La traduction et la
compilation se font sur sa machine, pour son usage personnel. Voir `PS2RC/LICENSE` (licence PS2RC 1.2) et
`PS2RC/NOTICE.md`.

## Où en est le portage

Le jeu démarre sur PSP (testé dans PPSSPP) et va jusqu'en jeu : écran titre, chargement, Jimmy devant l'école,
déplacements, HUD, textes.

Le chiffre de référence se mesure dans PPSSPP en mode headless (PSP à 333 MHz), en jeu devant l'école, avec la
distance d'affichage à 0,75 :

| Moment | ms par frame | fps |
|---|---|---|
| Début du travail sur la vitesse | ~680 | ~1,5 |
| Session 12 | 116 | 8,6 |
| Session 13 (état actuel) | **80-81** | **~12,4** |

Le jeu PS2 vise 30 fps. Le temps virtuel du jeu avance donc plus lentement que le temps réel : le jeu est
ralenti, il ne saute pas d'images.

| Domaine | État |
|---|---|
| Code du jeu (EE) | traduit en C par PS2RC (mode ABI, registres en variables locales, VU0 macro en local) |
| Graphismes VU1 | microprogrammes réécrits en natif (maillages, skinning, feuillage) et envoyés directement au GE de la PSP ; les autres passent par une traduction rapide générée (`tools/vu1_fast.py`) |
| GS | émulation légère, avec un cache de textures et des envois différés écrits bloc par bloc |
| Distance d'affichage | ×0,75 sur les objets proches, appliquée par le convertisseur (`[[stage.transform]]`) |
| Son | les modules du jeu (SCREAM, AUDVAG) tournent, mais sur un SPU **muet** : aucune sortie audio |
| Cinématiques | sautées |
| Passes plein écran (flou, halo) | sautées sur PSP (il faudrait du rendu dans une texture sur le GE) |
| Sauvegardes | carte mémoire émulée dans `memcard/` (non testée en profondeur) |

## Ce qui reste à faire

1. **Vitesse : il manque ~14 ms pour atteindre 15 fps (~66 ms par frame).** Il n'y a plus de gros poste isolé.
   Voici la répartition mesurée (part du CPU occupé, PPSSPP) :
   - code du jeu traduit : ~30 %, profil plat (la plus grosse fonction fait ~2 %) ;
   - skinning (`skin_batch`) : ~9 %, presque tout dans la boucle VFPU par sommet ;
   - lecture des commandes VIF (`vif1_chunk_`) : ~7 % ;
   - préparation et envoi des lots de maillage (`nat_batch`, `mc_draw`, `nat_emit`, `set_3d_state`…) : ~15 % pour ~1 100 lots par frame ;
   - paquets GS (`gs_packets_`) : ~4 %.

   Pistes déjà essayées sans gain (ne pas refaire à l'aveugle) :
   - remplacer en natif le test sphère/boîte `0x1313D0` : plus lent, car l'appelant doit sauvegarder ses registres autour de l'appel ;
   - un cache par site d'appel indirect (`PS2RC_IC=1`) : plus lent ;
   - gérer la saturation des flottants dans l'unité flottante (`fresf_`) : plus lent sous PPSSPP ;
   - réutiliser les sommets skinnés d'une frame à l'autre : seuls 4 lots sur 257 gardent les mêmes os.

   Pistes ouvertes :
   - rejouer les blocs DMA identiques d'une frame à l'autre (~400 sur 1 120), gain estimé à 2-4 ms ;
   - remplacer en natif d'autres fonctions du jeu qui font des appels indirects, comme la recherche dichotomique `0x146970`, déjà faite (−1,6 ms) ;
   - **mesurer sur une vraie PSP** : PPSSPP émule le VFPU avec un surcoût, donc les chiffres réels peuvent être meilleurs.
2. **Son** : brancher un vrai modèle SPU2 (voix ADPCM et mixage) sur la sortie audio PSP. Coût CPU estimé à
   5-15 %, à mettre derrière un réglage.
3. **Cinématiques** : convertir les vidéos PSS en PMF avec le convertisseur (`--movies convert`) et les lire avec
   `sceMpeg`.
4. **Passes plein écran** : masque de profondeur et halo, en faisant du rendu dans une texture sur le GE.
5. **Vraie PSP** : vérifier la mémoire (PSP-2000/3000 avec la mémoire étendue, `mem_volatile=1`) et la stabilité
   sur une longue partie.

## Organisation

```
README.md, push.sh       ce fichier ; script de publication (voir plus bas)
PS2RC/                   le moteur PS2RC v0.2.1 (commit 8cd56bb), AVEC les modifications de ce portage
packs/SLES-53561_v1.01/  le pack du jeu : hooks, code natif (src/), modules IOP, scripts de conversion
tools/                   outils de travail (profilage PSP, laboratoire VU1, générateurs)
env.sh, env.cmd, env.ps1 variables d'environnement du workspace
local.toml.example       modèle de réglages machine (à copier en local.toml)
work/                    (absent du dépôt) tout ce qui est fabriqué à partir de TON disque : privé
```

Fichiers modifiés dans PS2RC par rapport à la v0.2.1 (`git -C PS2RC diff` sur une copie propre) :
- `ps2rc/backend_c.py` et `ps2rc/vu/macro.py` : sorties supplémentaires (XOUT), fonctions qui ne touchent pas au
  VU0 (VFREE), DI/MFC0 en local, VU0 macro et MMI en variables locales ;
- `runtime/r5900_rt.h` et `hle/core/*` : `rt_di`, `rt_mfc0`, `vu0_clipw_l`, `RC_MAX_OVERLAYS` passé à 32 ;
- `ps2rc/convert/{recipe,stage,pipeline}.py` : `[[stage.transform]]`, qui permet au pack de réécrire un fichier
  du disque à la conversion, à taille égale ;
- `ps2rc/pipeline/{translate,parallel}.py`, `ps2rc/analysis/abi.py`, `ps2rc/iop/rt/iop_rt_int.h`.

## Reprendre le travail

Il faut :
- Python 3.11+ avec `pip install pyelftools pillow pytest` ;
- la toolchain pspdev (`~/pspdev`) ;
- PPSSPP, de préférence compilé en headless (`PPSSPPHeadless`) pour les mesures.

```sh
cp local.toml.example local.toml          # puis y mettre tes chemins (work, disque extrait)
ln -s ../../packs/SLES-53561_v1.01 PS2RC/games/SLES-53561_v1.01
. ./env.sh
```

**A. Juste jouer / produire le dossier PSP** (le convertisseur fait tout) :
```sh
cd PS2RC && python3 -m ps2rc.convert "/chemin/vers/ton_bully.iso" --out ~/sortie
# -> ~/sortie/PS2RC_SLES53561 à copier dans PSP/GAME/ (≈3,8 Go sur une Memory Stick)
```

**B. Développer** : il faut un `work/` rempli à partir de ton disque. Le disque est extrait dans `work/disc`, avec
`work/discmap.txt` et `work/iso_meta.bin` (le convertisseur, ou la conversion ci-dessus avec `--work`). Ensuite :
```sh
cd PS2RC
python3 games/SLES-53561_v1.01/convert/discover.py
python3 games/SLES-53561_v1.01/convert/non_abi.py
python3 games/SLES-53561_v1.01/convert/exact_extra.py
python3 games/SLES-53561_v1.01/convert/extra_outputs.py
python3 ps2rc/pipeline/translate.py --pack SLES-53561_v1.01
python3 games/SLES-53561_v1.01/convert/trapset.py
bash mk/build.sh --pack SLES-53561_v1.01 host 4                 # version PC (comparaison d'images)
SPLASH=0 bash mk/build.sh --pack SLES-53561_v1.01 psp 4         # -> work/psp/EBOOT.PBP
python3 ps2rc/pipeline/stage.py --pack SLES-53561_v1.01 --out <dossier>/PS2RC_SLES53561
```

**Fichiers privés à régénérer** (ils sont issus du jeu, donc jamais dans le dépôt). Le build fonctionne sans eux,
mais plus lentement :
- `work/vu1aot/hle_vu1_fast.h` : microprogrammes VU1 en traduction rapide. Lance une version PC avec
  `HLE_VU1_PROF=/chemin/absolu` pour récupérer `prog_*.code` et les histogrammes, puis
  `FAST_CLAMP=1 python3 tools/vu1_fast.py prog_000_c3651f62.code PCHIST work/vu1aot/hle_vu1_fast.h`.
- `work/vu1aot/hle_vu1_aot.h` : `tools/vu1_aot.py` (même principe).
- `work/iopgen/` : modules son traduits (`convert/iopgen.py`).

**Mesurer** : `ps2rc.cfg` du dossier de test avec `max_seconds=330`, `shot_frame=1250` et `zprof=1` (profil par
zones). Les lignes `PERF` du `session.log` donnent les ms par frame. Lance une seule instance de PPSSPP à la fois :
son horloge suit la charge de la machine. Pour le profil par adresse, utilise les variables d'environnement
`PPSSPP_PCPROF` / `PPSSPP_PCPROF_FROM`, puis `tools/psp_pcprof_report.py`, `psp_pcprof_lines.py` et
`psp_pcprof_callers.py`.

Réglages utiles dans `ps2rc.cfg` :
- `x_folnat=0` : feuillage par le programme VU ;
- `x_cellput=0` : textures écrites pixel par pixel ;
- `x_meshcache=0` : sans cache de maillages ;
- `x_vu1fast=0` : sans traduction rapide.

