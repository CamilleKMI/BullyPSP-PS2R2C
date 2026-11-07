/* pack_main.c - the game pack's side of the HLE interface (hle/core/hle.h, "the game pack's interface").
 *
 * Every function here is optional: the core has empty defaults. Add only what the game needs, and list this
 * directory in pack.toml [build] sources = ["src"]. Hooks of the game's own functions are defined by name:
 *     HLE_FN(MyFunction) { ... GUEST_FN(MyFunction)(c); ... }      (MyFunction bound in hooks.def)
 */
#include "hle.h"

/* boot, after the IOP: the pack's modules (renderer boundary, tunables), its code-overlay table:
 *     rc_overlay_lookup[id] = ov<id>_rc_lookup; rc_overlay_lo / rc_overlay_hi = the overlay window */
/* hle_pack_boot_init: hle_game.c */

/* end of run: the pack's report lines */
void hle_gfx_scan_report(void);           /* hle_gfx_scan.c */
void hle_pack_report(void)
{
    hle_gfx_scan_report();
}

/* the IOP-side services: hle_iop_servers.c */
