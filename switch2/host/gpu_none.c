/* The GPU library's runtime-facing functions while the ARM64 host has no GPU (phase 3 brings
 * the shadPS4 video core). No window exists, so the dialogs report that, and GPU/video imports
 * stay unresolved: the game stops at the first one with the usual STOP report. */
#include <stdint.h>
#include "gpu/gow3gpu.h"

uintptr_t gow3gpu_resolve(const char *scoped_nid) { (void)scoped_nid; return 0; }
int gow3gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8) { (void)initial_utf8; (void)prompt_utf8; return 0; }
int gow3gpu_text_input_poll(char *out_utf8, uint64_t size) { (void)out_utf8; (void)size; return 2; }
int gow3gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus)
{
    (void)title; (void)items; (void)images; (void)count; (void)focus;
    return 0;
}
int gow3gpu_choice_poll(void) { return -2; }
void gow3gpu_choice_set_input(uint32_t (*buttons)(void)) { (void)buttons; }
int gow3gpu_overlay_captures_input(void) { return 0; }
