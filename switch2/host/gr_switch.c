/* Gravity Rush Remastered on the Switch 2 (kaiHEN homebrew): the console's entry.
 *
 * Before the loader (gr_host.c) runs:
 * - the host heap is kept small (__libnx_initheap below), so the physical memory the game's
 *   5.4 GiB direct + flexible pool needs stays free for svcMapPhysicalMemory;
 * - the guest memory layout is placed in the alias region, whose address changes every launch
 *   (runtime_memory_set_layout: low memory first, then the user range with the pool);
 * - the files are found next to the NRO: sdmc:/switch/gravityrush_remastered/ holds prep/ (boot
 *   files made from your game), game/ (your game files), user/ (saves) and log.txt;
 * - the main thread becomes the applet thread (HOME menu, sleep: gr_suspend_horizon.c) and the
 *   loader runs on a game thread with a 16 MiB stack, as on the Mac.
 * Everything the game and the runtime print goes to log.txt (send it when something fails). */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "gr_suspend_horizon.h"

int gr_host_main(int argc, char** argv);                                   /* gr_host.c */
void runtime_memory_set_layout(uint64_t low_min, uint64_t user_min, uint64_t user_max); /* runtime_memory.c */
void runtime_audio_host_suspend(int suspend);                              /* gr_audio_horizon.h */
void runtime_memory_release(void);                                         /* runtime_memory.c */
void box64e_jit_release(void);                                             /* box64e_jit_horizon.c */

#define DEFAULT_DIR "sdmc:/switch/gravityrush_remastered"
/* The host heap: malloc for the runtime, Box64's data and JIT (libounce's jitCreate takes its
 * two views from here), FFmpeg, file buffers, the GPU library, and all GPU memory (OunceVK
 * allocates it with malloc). It gets everything but the guest memory the game really maps
 * (runtime_memory.c maps the pool's physical memory on demand: Gravity Rush allocates 4466 MiB
 * of direct memory and maps no flexible memory; GUEST_BUDGET leaves a little for more) and a
 * margin for the loader and the system; HOST_HEAP_SIZE is the least it takes. */
#define HOST_HEAP_SIZE ((size_t)2048 << 20)
#define GUEST_BUDGET ((size_t)(4480 + 64) << 20)
#define HOST_RESERVED (GUEST_BUDGET + ((size_t)128 << 20))
#define LOW_SPAN ((uint64_t)8 << 30) /* image, stacks, runtime structures the guest sees */
/* Without a system resource the guest lives in the heap (gr_physmem_horizon.c), its low memory
 * (the image, boot data, fiber stacks: about 50 MiB) in this much below the pool. */
#define LOW_SPAN_IN_HEAP ((uint64_t)192 << 20)

/* libnx (libounce) sets newlib's heap from the loader's heap override, which hands this process
 * almost all of its memory. Here malloc only gets the first HOST_HEAP_SIZE of it (mimalloc never
 * goes past fake_heap_end); the heap itself is not touched yet. main gives the unused top back
 * (give_back_heap), once the log is open, so the guest memory can be mapped. */
static uintptr_t heap_override_end;
void gr_crumb(const char* what);
#define crumb gr_crumb
static FILE* crumbs;
void __libnx_initheap(void)
{
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    void* addr = NULL;
    size_t size = 0;
    if (envHasHeapOverride()) {
        addr = envGetHeapOverrideAddr();
        size = envGetHeapOverrideSize();
    } else {
        /* Not expected under the homebrew loader: a heap of our own. */
        if (R_FAILED(svcSetHeapSize(&addr, HOST_HEAP_SIZE))) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
        size = HOST_HEAP_SIZE;
    }
    heap_override_end = (uintptr_t)addr + size;
    /* No system resource: the guest's low memory comes out of the heap too. */
    u64 resource = 0;
    svcGetInfo(&resource, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
    const size_t reserved = HOST_RESERVED + (resource ? 0 : (size_t)LOW_SPAN_IN_HEAP);
    const size_t cap = size > reserved + HOST_HEAP_SIZE ? size - reserved : HOST_HEAP_SIZE;
    if (size > cap) size = cap;
    fake_heap_start = (char*)addr;
    fake_heap_end = (char*)addr + size;
}

/* Shrinks the process heap to end where malloc's part ends (2 MiB granularity, counted from the
 * heap region's start). */
static bool no_system_resource;
void gr_physmem_use_heap(uintptr_t start, uintptr_t end); /* gr_physmem_horizon.c */
void gr_physmem_heap_range(uintptr_t* start, uintptr_t* end);
static void give_back_heap(void)
{
    extern char* fake_heap_end;
    u64 resource = 0;
    svcGetInfo(&resource, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
    if (!resource) {
        no_system_resource = true;
        /* The top stays: it is the guest's memory (gr_physmem_horizon.c). */
        const uintptr_t start = ((uintptr_t)fake_heap_end + 0x1fffff) & ~(uintptr_t)0x1fffff;
        gr_physmem_use_heap(start, heap_override_end);
        printf("Switch 2: heap kept, %llu MiB of it for the game's memory\n",
               (unsigned long long)((heap_override_end - start) >> 20));
        return;
    }
    u64 region = 0, used = 0;
    svcGetInfo(&region, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    const uintptr_t end = ((uintptr_t)fake_heap_end + 0x1fffff) & ~(uintptr_t)0x1fffff;
    if (!region || end <= region || end >= heap_override_end) {
        printf("Switch 2: heap left as it is (region 0x%llx, malloc end %p, loader heap end 0x%llx)\n",
               (unsigned long long)region, (void*)fake_heap_end, (unsigned long long)heap_override_end);
        return;
    }
    void* heap = NULL;
    const Result rc = svcSetHeapSize(&heap, end - region);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    printf("Switch 2: heap shrunk to %llu MiB (0x%x), process now uses %llu MiB\n",
           (unsigned long long)((end - region) >> 20), rc, (unsigned long long)(used >> 20));
}

/* At exit, everything this process took goes back, so the homebrew loader and the menu it
 * starts again have their memory: guest memory, the JIT, and the heap at its original size. */
static void release_memory(void)
{
    runtime_memory_release();
    box64e_jit_release();
    u64 region = 0, used = 0;
    svcGetInfo(&region, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    Result rc = 0;
    if (region && heap_override_end > region) {
        void* heap = NULL;
        rc = svcSetHeapSize(&heap, heap_override_end - region);
    }
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    printf("Switch 2: memory released (heap restore 0x%x), process uses %llu MiB\n", rc, (unsigned long long)(used >> 20));
    crumb("memory released; exiting");
}

/* libnx's shutdown (__appExit) calls this first; the services it closes next are closed here
 * one at a time with a breadcrumb after each (libnx counts references, so its own later calls
 * do nothing), to find the step that faults. The SD card closes after this: nothing can be
 * written past "closing the SD card". */
void userAppExit(void)
{
    crumb("libnx shutdown: start");
    hidExit();
    crumb("libnx shutdown: HID closed");
    timeExit();
    crumb("libnx shutdown: time closed");
    appletExit();
    crumb("libnx shutdown: applet closed; closing the SD card");
    if (crumbs) { fclose(crumbs); crumbs = NULL; }
}

/* Breadcrumbs: boot.txt next to the NRO, written and flushed at each early step, so a crash
 * before the log is open still shows how far the start got. */
void gr_crumb(const char* what)
{
    static Mutex lock; /* threads write crumbs too (zero-initialized: unlocked) */
    mutexLock(&lock);
    if (!crumbs) {
        /* Appended: several runs can go into one file (each starts with a "=== run" line). */
        crumbs = fopen(DEFAULT_DIR "/boot.txt", "a");
        if (crumbs) setvbuf(crumbs, NULL, _IONBF, 0);
        if (crumbs) fprintf(crumbs, "=== run (system tick %llu)\n", (unsigned long long)armGetSystemTick());
    }
    /* Milliseconds since this run's first crumb (the console's 19.2 MHz system tick). */
    static u64 first_tick;
    const u64 now = armGetSystemTick();
    if (!first_tick) first_tick = now;
    /* Kept open and unbuffered: each line goes to the file system service at once, and what it
     * has received survives a crash of this process (closing after every line took ~27 ms). */
    if (crumbs) {
        fprintf(crumbs, "%7llu ms %s\n", (unsigned long long)((now - first_tick) / 19200), what);
        fflush(crumbs);
    }
    mutexUnlock(&lock);
}

static char base_dir[512];
static char boot_file[600], content_file[600], patch_file[600], game_dir[600], user_dir[600];

static void find_files(int argc, char** argv)
{
    /* hbmenu passes the NRO's path: its folder holds everything. */
    snprintf(base_dir, sizeof base_dir, "%s", DEFAULT_DIR);
    if (argc > 0 && argv[0] && strncmp(argv[0], "sdmc:/", 6) == 0) {
        snprintf(base_dir, sizeof base_dir, "%s", argv[0]);
        char* slash = strrchr(base_dir, '/');
        if (slash) *slash = 0;
    }
    snprintf(boot_file, sizeof boot_file, "%s/prep/boot-linked.bin", base_dir);
    snprintf(content_file, sizeof content_file, "%s/prep/content.bin", base_dir);
    snprintf(patch_file, sizeof patch_file, "%s/prep/patches.bin", base_dir);
    snprintf(game_dir, sizeof game_dir, "%s/game", base_dir);
    snprintf(user_dir, sizeof user_dir, "%s/user", base_dir);
    mkdir(user_dir, 0777);
}

static int place_memory(void)
{
    u64 alias = 0, alias_size = 0;
    svcGetInfo(&alias, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&alias_size, InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0);
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    printf("Switch 2: alias region 0x%llx (%llu GiB), memory %llu of %llu MiB used\n", (unsigned long long)alias,
           (unsigned long long)(alias_size >> 30), (unsigned long long)(used >> 20), (unsigned long long)(total >> 20));
    /* svcMapPhysicalMemory (all of the game's memory) needs the process's system resource: the
     * game hbloader runs as gives it (its npdm). Without one every map fails with 0xfa01. */
    u64 resource = 0, program = 0;
    svcGetInfo(&resource, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&program, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
    {
        char line[160];
        snprintf(line, sizeof line, "running as program %016llx, system resource %llu KiB, memory %llu MiB",
                 (unsigned long long)program, (unsigned long long)(resource >> 10), (unsigned long long)(total >> 20));
        crumb(line);
        printf("Switch 2: %s\n", line);
    }
    if (!resource) {
        /* gr_physmem_horizon.c: the guest's memory comes from the heap's top (give_back_heap). */
        printf("Switch 2: no system resource (program %016llx): the game's memory comes from the heap\n",
               (unsigned long long)program);
        crumb("no system resource: the game's memory is the heap's top");
        /* Low memory, then the pool at its physical offsets, inside the heap's top. The user
         * range's end is past it (the runtime's address bookkeeping): a map beyond the heap
         * fails and is logged. */
        uintptr_t start = 0, heap_end = 0;
        gr_physmem_heap_range(&start, &heap_end);
        const uint64_t low = start, user = (start + LOW_SPAN_IN_HEAP + 0x1fffff) & ~(uint64_t)0x1fffff;
        if (!start || user >= heap_end) {
            printf("ERROR: the heap has no room for the game's memory\n");
            return -1;
        }
        runtime_memory_set_layout(low, user, user + ((uint64_t)8 << 30));
        printf("Switch 2: guest low memory 0x%llx, pool 0x%llx, %llu MiB of heap for it\n", (unsigned long long)low,
               (unsigned long long)user, (unsigned long long)((heap_end - user) >> 20));
        return 0;
    }
    if (!alias || alias_size < LOW_SPAN + ((uint64_t)16 << 30)) {
        printf("ERROR: the alias region is too small for the game's memory\n");
        return -1;
    }
    /* 1 GiB alignment keeps every range clear of the region's edges; all of it is below 512 GiB,
     * so 40-bit PS4 pointers and GPU addresses hold. */
    const uint64_t low = (alias + ((uint64_t)1 << 30)) & ~(((uint64_t)1 << 30) - 1);
    const uint64_t user = low + LOW_SPAN;
    const uint64_t end = (alias + alias_size) & ~(((uint64_t)1 << 30) - 1);
    runtime_memory_set_layout(low, user, end);
    printf("Switch 2: guest low memory 0x%llx, user range 0x%llx-0x%llx\n", (unsigned long long)low,
           (unsigned long long)user, (unsigned long long)end);
    return 0;
}

typedef struct { int argc; char** argv; int result; } GameArgs;
static void* game_thread(void* p)
{
    GameArgs* a = p;
    a->result = gr_host_main(a->argc, a->argv);
    printf("The game returned %d\n", a->result);
    gr_suspend_exit(a->result);
}

static void audio_suspend(bool suspend) { runtime_audio_host_suspend(suspend ? 1 : 0); }

/* Start menu (on screen, before anything else runs). "Without this menu" writes a flag file
 * so the next launch starts the game before the text console ever exists (it also uses the
 * default window, which Vulkan presents to). */
enum {
    RUN_GAME,          /* the game with the settings in options.ini, then back to the homebrew menu */
    TEST_ARENA_16,     /* the GPU library's buffer blocks at 16 MiB (default 32) */
    TEST_864P,         /* render size 864p in either mode */
    TEST_720P,         /* render size 720p in either mode */
    TEST_MOVEMENT,     /* the sticks (raw and as sent), motion, the game's input and the camera, 10 times a second */
    TEST_STICK_SWAP,   /* the same, with each stick's X and Y swapped (GOW3_STICK_SWAP=1) */
    TEST_TEXTURE_DUMP, /* each distinct small texture to texdump/ (open the pause menu once) */
    RUN_FRAME_LOGS,    /* with the frame-time breakdown and GPU waits (a little slower) */
    STICK_CHECK,       /* not a run: the sticks' readings on screen, then back to this menu */
    RUN_COUNT
};
static const char* const run_names[RUN_COUNT] = {
    "Run the game",
    "Test: 16 MiB GPU buffer blocks",
    "Test: render at 864p",
    "Test: render at 720p",
    "Test: movement log (walk with the left stick)",
    "Test: movement log, stick X and Y swapped",
    "Test: save textures (open the pause menu once)",
    "Run the game with frame-time logs",
    "Check the sticks (no game)",
};

/* The sticks as the console reads them, live, until B: which way each push reads, to tell a
 * swapped or turned stick from a problem in the game. */
static void stick_check(PadState* pad)
{
    static const char* const names[8] = {"right", "up-right", "up", "up-left", "left", "down-left", "down", "down-right"};
    int frames = 0;
    while (appletMainLoop()) {
        padUpdate(pad);
        if (padGetButtonsDown(pad) & HidNpadButton_B) break;
        if (frames++ % 6) { svcSleepThread(16 * 1000000ull); continue; }
        printf("\x1b[2J\x1b[1;1HStick check (B: back to the menu)\n\n"
               "Push the left stick up, then right, down and left, and check each line.\n\n");
        for (int i = 0; i < 2; ++i) {
            const HidAnalogStickState s = padGetStickPos(pad, i);
            const double r = sqrt((double)s.x * s.x + (double)s.y * s.y);
            const char* way = "centre";
            if (r > 8000) {
                const double deg = atan2((double)s.y, (double)s.x) * 57.29578;
                way = names[((int)floor((deg + 360.0 + 22.5) / 45.0)) % 8];
            }
            printf("%s stick: x %6d  y %6d  reads %s\n", i ? "Right" : "Left ", s.x, s.y, way);
        }
        printf("\nController: style 0x%llx, buttons 0x%llx\n", (unsigned long long)padGetStyleSet(pad),
               (unsigned long long)padGetButtons(pad));
        consoleUpdate(NULL);
        svcSleepThread(16 * 1000000ull);
    }
}

static int choose_run(void)
{
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    int choice = 0, shown = -1;
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (down & (HidNpadButton_AnyUp)) choice = (choice + RUN_COUNT - 1) % RUN_COUNT;
        if (down & (HidNpadButton_AnyDown)) choice = (choice + 1) % RUN_COUNT;
        if ((down & HidNpadButton_A) && choice == STICK_CHECK) {
            stick_check(&pad);
            shown = -1;
            continue;
        }
        if (down & HidNpadButton_A) break;
        if (choice != shown) {
            printf("\x1b[2J\x1b[1;1HGravity Rush Remastered: test menu\n\n");
            for (int i = 0; i < RUN_COUNT; ++i) printf(" %s %d. %s\n", i == choice ? ">" : " ", i + 1, run_names[i]);
            printf("\nUp/Down to choose, A to run. The choice is written to boot.txt and log.txt.\n"
                   "Each test changes one thing from \"Run the game\" (the settings in options.ini).\n");
            consoleUpdate(NULL);
            shown = choice;
        }
        svcSleepThread(16 * 1000000ull);
    }
    printf("\nRunning %d...\n", choice + 1);
    consoleUpdate(NULL);
    consoleExit(NULL);
    return choice;
}

int main(int argc, char** argv)
{
    crumb("main reached");
    find_files(argc, argv);
    /* The text console uses the default window too: the Vulkan probe found no GPU when the
     * console had started first. This flag file skips the menu (and the console) once. */
    char next_path[600];
    snprintf(next_path, sizeof next_path, "%s/run_without_menu", base_dir);
    int run;
    if (remove(next_path) == 0) { /* written by an older build's menu entry */
        run = RUN_GAME;
        crumb("menu skipped (run_without_menu)");
    } else {
        run = choose_run();
    }
    char line[160];
    snprintf(line, sizeof line, "menu: %s", run_names[run]);
    crumb(line);
    /* The tests: one change each, over options.ini (set before it is read, which does not
     * overwrite them). */
    if (run == TEST_ARENA_16) setenv("GOW3_ARENA_MB", "16", 1);
    if (run == TEST_864P) setenv("GOW3_RESOLUTION", "864p", 1);
    if (run == TEST_720P) setenv("GOW3_RESOLUTION", "720p", 1);
    if (run == TEST_MOVEMENT || run == TEST_STICK_SWAP) setenv("GOW3_MOVE_LOG", "fast", 1);
    if (run == TEST_STICK_SWAP) setenv("GOW3_STICK_SWAP", "1", 1);
    if (run == TEST_TEXTURE_DUMP) {
        char dir[640];
        snprintf(dir, sizeof dir, "%s/texdump", base_dir);
        mkdir(dir, 0777);
        setenv("GOW3_TEXTURE_DUMP", "1", 1);
    }
    gr_suspend_init(); /* first: every thread created from here on can be frozen */
    crumb("suspend handling ready");
    char log_path[600];
    snprintf(log_path, sizeof log_path, "%s/log.txt", base_dir);
    if (!freopen(log_path, "a", stdout)) return 1; /* appended, as boot.txt */
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* stderr into the same file: dup2 does not redirect on Horizon, so its FILE writes to the
     * log's descriptor instead (one descriptor: one file offset, the lines stay in order). */
    fflush(stderr);
    stderr->_file = fileno(stdout);
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "stderr goes to the log\n");
    crumb("log open");
    printf("\n=== run (system tick %llu)\n", (unsigned long long)armGetSystemTick());
    printf("Gravity Rush Remastered for Switch 2: files in %s\n%s\n", base_dir, line);
    give_back_heap();
    fflush(stdout);
    if (place_memory()) return 1;
    crumb("memory placed; starting the game thread");

    gr_suspend_add_listener(audio_suspend);
    gr_suspend_set_exit_hook(release_memory);
    /* x86 code relies on x86's memory order; ARM's is weaker. Box64's strongmem adds barriers
     * (1 is the default here; the menu compares 0 and 2). Threads on several cores need it. */
    const char* strongmem = "1";
    setenv("BOX64_DYNAREC_STRONGMEM", strongmem, 1);
    {
        char line[96];
        snprintf(line, sizeof line, "Box64 strongmem %s, %s", strongmem, "all cores");
        crumb(line);
    }
    /* Settings: options.ini next to the .nro (the in-game menu, R3+L2, writes it; Gow3Settings
     * writes every option there at start). Two keys are read here, once, before the game starts
     * (the menu only changes them for the next launch):
     * - language=ENG (FRA, DEU, ITA, ESP, POR, RUS, POL, NLD, JPN): the PS4 language code the
     *   game gets, and its region. The game reads the region from game/args.txt ("-region E", the
     *   European release); without it, it starts in the Japanese region and shows Japanese text
     *   whatever its language. JPN leaves the region argument out.
     * - swap_ab=1 (default): menus confirm with A and cancel with B, as on the Switch: outside
     *   gameplay A sends the PS4's cross and B its circle (GOW3_MENU_SWAP=1, src/runtime_pad.c).
     *   Gameplay is by position either way (B is cross: jump).
     *   0: menus confirm with B (the PS4's cross, as in the European release).
     * - gyro=off: no motion sensor data reaches the game (GOW3_GYRO=0).
     * - resolution_handheld=1080p, resolution_docked=1080p (1080p, 972p, 864p, 720p): the game's
     *   render size (GOW3_RESOLUTION, gr_resolution.c), by the console's mode at launch; lower is
     *   faster on the GPU, the picture is scaled to the screen.
     * GOW3_LANGUAGE (a PS4 code) overrides the language. */
    static const struct { const char* key; int code; } languages[] = {
        {"ENG", 18}, {"FRA", 2}, {"DEU", 4}, {"ITA", 5}, {"ESP", 3},
        {"POR", 7},  {"RUS", 8}, {"POL", 16}, {"NLD", 6}, {"JPN", 0},
    };
    static char options_file[600];
    snprintf(options_file, sizeof options_file, "%s/options.ini", base_dir);
    setenv("GOW3_CONFIG", options_file, 0);
    {
        const char* key = "ENG";
        int code = 18, swap_ab = 1, gyro = 1;
        static char res_handheld[16] = "1080p", res_docked[16] = "1080p";
        FILE* f = fopen(options_file, "r");
        if (f) {
            char text[256];
            while (fgets(text, sizeof text, f)) {
                char value[32];
                int number;
                if (sscanf(text, " language = %31[A-Za-z0-9]", value) == 1) {
                    for (size_t l = 0; l < sizeof languages / sizeof languages[0]; ++l)
                        if (!strcasecmp(value, languages[l].key)) { key = languages[l].key; code = languages[l].code; }
                    if (sscanf(value, "%d", &number) == 1) /* an older numeric value */
                        for (size_t l = 0; l < sizeof languages / sizeof languages[0]; ++l)
                            if (languages[l].code == number) { key = languages[l].key; code = number; }
                }
                if (sscanf(text, " swap_ab = %d", &number) == 1) swap_ab = number != 0;
                if (sscanf(text, " gyro = %31[A-Za-z0-9]", value) == 1) gyro = strcasecmp(value, "off") && strcmp(value, "0");
                if (sscanf(text, " resolution_handheld = %15[A-Za-z0-9]", value) == 1) snprintf(res_handheld, sizeof res_handheld, "%s", value);
                if (sscanf(text, " resolution_docked = %15[A-Za-z0-9]", value) == 1) snprintf(res_docked, sizeof res_docked, "%s", value);
            }
            fclose(f);
        }
        if (getenv("GOW3_LANGUAGE")) {
            code = atoi(getenv("GOW3_LANGUAGE"));
            key = "GOW3_LANGUAGE";
        }
        char text[160];
        snprintf(text, sizeof text, "%d", code);
        setenv("GOW3_LANGUAGE", text, 1);
        if (swap_ab) setenv("GOW3_MENU_SWAP", "1", 0); /* src/runtime_pad.c */
        if (!gyro) setenv("GOW3_GYRO", "0", 0);
        const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
        setenv("GOW3_RESOLUTION", docked ? res_docked : res_handheld, 0);
        snprintf(text, sizeof text, "render size: %s (%s)", docked ? res_docked : res_handheld, docked ? "docked" : "handheld");
        crumb(text);
        /* The region argument for this language. */
        char args_path[600];
        snprintf(args_path, sizeof args_path, "%s/args.txt", game_dir);
        /* GR_GAME_ARGS (a test in the start menu): more of the game's own launch arguments. */
        const char* extra = getenv("GR_GAME_ARGS");
        if (code == 0 && !extra) {
            remove(args_path);
        } else if ((f = fopen(args_path, "wb"))) {
            fprintf(f, "%s%s%s\r\n", code == 0 ? "" : "-region E", code != 0 && extra ? " " : "", extra ? extra : "");
            fclose(f);
            if (extra) {
                snprintf(text, sizeof text, "game arguments: %s%s%s", code == 0 ? "" : "-region E ", extra, "");
                crumb(text);
            }
        }
        snprintf(text, sizeof text, "language: %s (PS4 code %d, %s), A/B %s, gyro %s", key, code,
                 code == 0 ? "Japanese region" : "region E", swap_ab ? "menus confirm with A" : "menus confirm with B",
                 gyro ? "on" : "off");
        crumb(text);
    }
    /* GPU memory: the texture cache frees images not used lately past this budget (the driver
     * reports no budget of its own; images reached 1.4 GiB at the first gameplay and the heap
     * ran out). At 1000 MiB the heap still ran out after ~6 minutes, with the staging pool
     * keeping 240 MiB it no longer used (it keeps up to 512 MiB by default). */
    setenv("GOW3_GC_BUDGET_MB", "800", 0);
    setenv("GOW3_STAGING_KEEP_MB", "64", 0);
    /* Every run: the heartbeat (heap report every 20 s) and the frame rate every 5 s. */
    setenv("GR_HEARTBEAT", "1", 1);
    setenv("GOW3_FPS_LOG", "1", 1);
    /* Frame-time logs: only what explains a slow frame. The call tracer, the GPU library's info
     * log and the audio traces made the game run at ~3 FPS (2026-11-19), so they are gone. */
    if (run == RUN_FRAME_LOGS) {
        setenv("GOW3_FRAME_STATS", "1", 0); /* where the time goes in slow frames ("Stall:") */
        setenv("GOW3_PERF_DIAG", "1", 0);   /* GPU waits by call site */
    }
    static char* args[] = {"gravityrush_remastered", boot_file, "--content-profile", content_file,
                           "--patches", patch_file, "--app0", game_dir, "--user", user_dir, NULL, NULL};
    static GameArgs game = {10, args, 0};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)16 << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attr, game_thread, &game)) {
        printf("ERROR: cannot start the game thread\n");
        return 1;
    }
    gr_suspend_run_applet_loop(); /* ends the process: the system closed the game, or it exited */
}
