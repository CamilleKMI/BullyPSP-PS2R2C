/* main_host.c - the PC replayer: plays a capture through the software GS; frames as PPM files
   (HLE_GS_FRAMES=<dir> HLE_GS_EVERY=1). Usage: gsreplay_host CAPTURE */
#include "hle.h"
#include <stdarg.h>
void gsr_logf(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stdout, fmt, a); va_end(a); }
int gsr_load(uint8_t *data, uint32_t n);
int gsr_frame(void);
void hle_gs_lite_report(void);
void hle_vu1_report(void);
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n); if (fread(d, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
    if (gsr_load(d, (uint32_t)n)) { printf("not a capture\n"); return 1; }
    int k = 0; while (gsr_frame()) k++;
    printf("%d frames\n", k);
    hle_vu1_report(); hle_gs_lite_report();
    if (getenv("GSR_PCHIST")) { void hle_vu1_dump_pchist(const char *); hle_vu1_dump_pchist(getenv("GSR_PCHIST")); }
    if (getenv("GSR_DUMP_VUCODE")) { void hle_vu1_dump_code(const char *); hle_vu1_dump_code(getenv("GSR_DUMP_VUCODE")); }
    return 0;
}
