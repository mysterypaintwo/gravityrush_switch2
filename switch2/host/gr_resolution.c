/* The game's render resolution, rewritten in its code at launch (GOW3_RESOLUTION=720p, 864p,
 * 972p or 1080p; options.ini resolution=). The same rewrite as scripts/gr_resolution_patch.py,
 * which the PC port applies through its patch file: the game sizes its device, the display
 * buffers and the base render targets with 1920x1080 constants (two half-size targets with
 * 960x540 and 960x544) and derives every other target from them; some screen-space shaders hold
 * 1/1920 and 1/1080, and the 2D layer's clip rectangle a 1920/960, 1080/544 scale. Every original
 * value is checked first: with another build of the game nothing is changed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gr_crumb(const char* what) __attribute__((weak));

static const struct { uint32_t offset; char what; } sites[] = {
    {0x4aa, 'W'},     {0x4ae, 'H'},     {0x14902, 'W'},   {0x14906, 'H'},   {0x14959, 'W'},
    {0x1495f, 'H'},   {0x14b92, 'W'},   {0x14b96, 'H'},   {0x14d49, 'W'},   {0x14d54, 'W'},
    {0x14d5a, 'H'},   {0x14dde, 'W'},   {0x14de9, 'W'},   {0x14def, 'H'},   {0x14e7e, 'W'},
    {0x14e89, 'W'},   {0x14e8f, 'H'},   {0x28417, 'W'},   {0x2841d, 'H'},   {0x28423, 'W'},
    {0x2849a, 'W'},   {0x284a0, 'H'},   {0x28779, 'w'},   {0x2877f, 'h'},   {0x28785, 'w'},
    {0x6761c, 'W'},   {0x67622, 'H'},   {0x67628, 'W'},   {0x24f653, 'W'},  {0x24f659, 'H'},
    {0x24f65f, 'W'},  {0x1b621b, 'w'},  {0x1b6221, 'x'},  {0x1b6227, 'w'},
};
static const uint32_t screen_uv_sites[] = {0x161a274, 0x1625ee0, 0x16939ac, 0x16ab1b4, 0x16ac25c, 0x16ac4ec,
                                           0x16acee8, 0x16ad2a8, 0x16ae1a8, 0x16ae758, 0x16afc54, 0x16b0404,
                                           0x16b16ac, 0x16b1bac, 0x16b25b8, 0x16b2ae4};
#define CLIP_SCALE 0x103fab0

static uint32_t value_of(char what, uint32_t w, uint32_t h)
{
    switch (what) {
    case 'W': return w;
    case 'H': return h;
    case 'w': return w / 2;
    case 'h': return h / 2;
    default: return (h / 2 + 31) / 32 * 32; /* the 544-line half target */
    }
}
static void clip_scale(float out[4], uint32_t w, uint32_t h)
{
    out[0] = out[2] = (float)w / 960.0f;
    out[1] = out[3] = (float)h / 544.0f;
}

/* GOW3_SHADOW=1024: the shadow map (one 2048x2048 depth texture, four cascades) at half size.
 * Only its size is patched, as the PC port's 4096 option does (scripts/gr_resolution_patch.py):
 * the game keeps drawing 2048-based cascades and the GPU library scales those passes to the
 * map (vk_rasterizer.cpp); the shaders' 1/2048 stays, as positions stay 2048-based. */
static void shadow_install(unsigned char* image, size_t size)
{
    static const uint32_t shadow_sites[] = {0x28679, 0x2867f};
    const char* want = getenv("GOW3_SHADOW");
    if (!want || strcmp(want, "1024") || size < 0x28683) return;
    char line[120];
    for (size_t i = 0; i < 2; ++i) {
        uint32_t v;
        memcpy(&v, image + shadow_sites[i], 4);
        if (v != 2048) {
            snprintf(line, sizeof line, "shadows: 1024 not applied (not the expected game code)");
            if (gr_crumb) gr_crumb(line);
            return;
        }
    }
    const uint32_t v = 1024;
    for (size_t i = 0; i < 2; ++i) memcpy(image + shadow_sites[i], &v, 4);
    snprintf(line, sizeof line, "shadows: 1024x1024 map (the game's is 2048)");
    printf("Switch 2: %s\n", line);
    if (gr_crumb) gr_crumb(line);
}

/* GOW3_MENU_CONFIRM=circle (options.ini swap_ab=1): menus confirm with circle, which sits where
 * the Switch's A is. The game never asks the system which button confirms: its start-up code
 * (0x2990) picks the confirm and cancel buttons by region, as bit numbers of the pad's buttons
 * (13 circle, 14 cross), into two globals every menu tests (0x15e83b8 confirm, 0x15e83bc
 * cancel). Regions 0, 3 and 5 get circle; the others (the European release) cross. The cross
 * path's two immediates are swapped, so every region confirms with circle. */
static void menu_confirm_install(unsigned char* image, size_t size)
{
    const char* want = getenv("GOW3_MENU_CONFIRM");
    if (!want || strcmp(want, "circle") || size < 0x29b8) return;
    static const unsigned char cross_path[10] = {0xb9, 0x0d, 0, 0, 0, 0xba, 0x0e, 0, 0, 0}; /* ecx=13 cancel, edx=14 confirm */
    char line[120];
    if (memcmp(image + 0x29ae, cross_path, sizeof cross_path)) {
        snprintf(line, sizeof line, "menus: confirm with A not applied (not the expected game code)");
    } else {
        image[0x29af] = 0x0e; /* cancel: cross */
        image[0x29b4] = 0x0d; /* confirm: circle */
        snprintf(line, sizeof line, "menus: confirm with circle (the Switch's A), cancel with cross (B)");
    }
    printf("Switch 2: %s\n", line);
    if (gr_crumb) gr_crumb(line);
}

void gr_resolution_install(unsigned char* image, size_t size)
{
    (void)menu_confirm_install; /* not used: it moved jump to A too (gameplay reads the same globals) */
    shadow_install(image, size);
    static const struct { const char* name; uint32_t w, h; } sizes[] = {
        {"720p", 1280, 720}, {"864p", 1536, 864}, {"972p", 1728, 972}, {"1080p", 1920, 1080},
    };
    const char* want = getenv("GOW3_RESOLUTION");
    if (!want || !*want) return;
    uint32_t w = 0, h = 0;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i)
        if (!strcmp(want, sizes[i].name)) { w = sizes[i].w; h = sizes[i].h; }
    char line[160];
    if (!w || w == 1920) {
        snprintf(line, sizeof line, "resolution: %s (the game's own 1920x1080)", w ? want : "unknown name, so 1080p");
        if (gr_crumb) gr_crumb(line);
        return;
    }
    if (size < 0x16b2ae4 + 16) return;
    /* Check everything before changing anything. */
    for (size_t i = 0; i < sizeof sites / sizeof sites[0]; ++i) {
        uint32_t v;
        memcpy(&v, image + sites[i].offset, 4);
        if (v != value_of(sites[i].what, 1920, 1080)) goto other_build;
    }
    for (size_t i = 0; i < sizeof screen_uv_sites / sizeof screen_uv_sites[0]; ++i) {
        const float a = 1.0f / 1920.0f, b = 1.0f / 1080.0f;
        if (memcmp(image + screen_uv_sites[i], &a, 4) || memcmp(image + screen_uv_sites[i] + 8, &b, 4)) goto other_build;
    }
    float clip[4];
    clip_scale(clip, 1920, 1080);
    if (memcmp(image + CLIP_SCALE, clip, 16)) goto other_build;

    for (size_t i = 0; i < sizeof sites / sizeof sites[0]; ++i) {
        const uint32_t v = value_of(sites[i].what, w, h);
        memcpy(image + sites[i].offset, &v, 4);
    }
    for (size_t i = 0; i < sizeof screen_uv_sites / sizeof screen_uv_sites[0]; ++i) {
        const float a = 1.0f / (float)w, b = 1.0f / (float)h;
        memcpy(image + screen_uv_sites[i], &a, 4);
        memcpy(image + screen_uv_sites[i] + 8, &b, 4);
    }
    clip_scale(clip, w, h);
    memcpy(image + CLIP_SCALE, clip, 16);
    /* GOW3_RESOLUTION_FLOATS=1 (a test): the five (1920.0, 1080.0) float pairs in the game's
     * data too. At 864p one render target was still 1920x1080 and the comic panels were wrong;
     * these may size it, or may be the fixed 1920x1080 layout space the UI must keep. */
    const char* floats = getenv("GOW3_RESOLUTION_FLOATS");
    if (floats && floats[0] == '1') {
        static const uint32_t float_sites[] = {0x10217ec, 0x1021804, 0x1021810, 0x102181c, 0x1045600};
        const float full[2] = {1920.0f, 1080.0f}, now[2] = {(float)w, (float)h};
        int done = 0;
        for (size_t i = 0; i < sizeof float_sites / sizeof float_sites[0]; ++i)
            if (!memcmp(image + float_sites[i], full, 8)) { memcpy(image + float_sites[i], now, 8); ++done; }
        snprintf(line, sizeof line, "resolution: %d of 5 float size pairs rewritten too (test)", done);
        if (gr_crumb) gr_crumb(line);
    }
    snprintf(line, sizeof line, "resolution: the game renders at %ux%u (%s)", w, h, want);
    printf("Switch 2: %s\n", line);
    if (gr_crumb) gr_crumb(line);
    return;
other_build:
    snprintf(line, sizeof line, "resolution: %s not applied (not the expected game code: another build?)", want);
    printf("Switch 2: %s\n", line);
    if (gr_crumb) gr_crumb(line);
}
