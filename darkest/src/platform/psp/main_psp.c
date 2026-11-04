/*
 * main_psp.c - the PSP program entry point and the platform.h functions.
 *
 * A PSP program must (1) declare module info, (2) register an EXIT CALLBACK
 * so the HOME button can quit cleanly, (3) run a main loop. The loop here is:
 *   read pad -> game_update(dt, buttons) -> game_draw()   (draw waits for vblank)
 * dt is the real time since the previous frame, clamped to 0.1 s so a hitch
 * can't make the game jump.
 */
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <psprtc.h>
#include <stdarg.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>

#include "game.h"
#include "gfx.h"
#include "platform.h"

/* Module metadata and options: user-mode thread with VFPU (the PSP's vector
 * math unit) enabled, and a heap using all free RAM minus 1 MB for the OS. */
PSP_MODULE_INFO("Darkest Dungeon Demake", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024); /* all free RAM minus 1 MB for the system */

/* Cleared by the exit callback when the user quits from the XMB. */
static volatile int running = 1;

/* Called by the OS when the user chooses to exit the game. */
static int exit_callback(int a, int b, void *c) {
    (void)a; (void)b; (void)c;
    running = 0;
    return 0;
}

/* A helper thread whose only job is to wait for system callbacks. */
static int callback_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

/* Start that thread (priority 0x11, 4 KB stack). */
static void setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) sceKernelStartThread(thid, 0, 0);
}

/* 16-byte aligned allocation (required for texture memory). */
void *plat_alloc(int n) { return memalign(16, n > 0 ? n : 1); }

/* Read a whole file into an aligned buffer. Caller frees. */
void *plat_read_file(const char *path, int *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = memalign(16, n > 0 ? n : 1); /* aligned so textures can be used in place */
    if (buf && fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf && size) *size = (int)n;
    return buf;
}

/* Write a whole file (used for save.bin). */
int plat_write_file(const char *path, const void *data, int size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == (size_t)size;
    fclose(f);
    return ok ? 0 : -1;
}

/* Append a line + heap statistics to log.txt. The file is opened and closed
 * on every call so the log survives a crash - invaluable when debugging on
 * real hardware where there is no console. */
void plat_log(const char *fmt, ...) {
    FILE *f = fopen("log.txt", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    struct mallinfo mi = mallinfo();
    fprintf(f, "   [heap used %d KB of %d KB]\n", (int)(mi.uordblks / 1024), (int)(mi.arena / 1024));
    fclose(f); /* close every line so the log survives a crash */
}

/* Milliseconds from the real-time clock (RTC ticks are microseconds). */
uint32_t plat_ticks_ms(void) {
    u64 t;
    sceRtcGetCurrentTick(&t);
    return (uint32_t)(t / 1000);
}

/* Translate the PSP's PSP_CTRL_* bits into the game's own BTN_* bits. */
static uint32_t map_buttons(unsigned int b) {
    uint32_t o = 0;
    if (b & PSP_CTRL_UP) o |= BTN_UP;
    if (b & PSP_CTRL_DOWN) o |= BTN_DOWN;
    if (b & PSP_CTRL_LEFT) o |= BTN_LEFT;
    if (b & PSP_CTRL_RIGHT) o |= BTN_RIGHT;
    if (b & PSP_CTRL_CROSS) o |= BTN_CROSS;
    if (b & PSP_CTRL_CIRCLE) o |= BTN_CIRCLE;
    if (b & PSP_CTRL_SQUARE) o |= BTN_SQUARE;
    if (b & PSP_CTRL_TRIANGLE) o |= BTN_TRIANGLE;
    if (b & PSP_CTRL_LTRIGGER) o |= BTN_L;
    if (b & PSP_CTRL_RTRIGGER) o |= BTN_R;
    if (b & PSP_CTRL_START) o |= BTN_START;
    if (b & PSP_CTRL_SELECT) o |= BTN_SELECT;
    return o;
}

/* Startup: boot log, callbacks, CPU at the maximum 333 MHz, controller in
 * analog mode, graphics, game. Then the main loop described above. */
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    remove("log.txt");
    plat_log("boot: free %d KB", (int)(sceKernelMaxFreeMemSize() / 1024));
    setup_callbacks();
    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    gfx_init();
    plat_log("gfx ok");
    if (game_init(plat_ticks_ms()) != 0) {
        plat_log("game_init failed");
        gfx_shutdown();
        pspDebugScreenInit();
        pspDebugScreenPrintf("Darkest Dungeon demake\n\n"
                             "The game data was not found in assets.pak.\n"
                             "Build assets.pak with the extractor tool from your own\n"
                             "copy of the game and put it next to EBOOT.PBP.\n\n"
                             "(see log.txt)  Press any button to exit.\n");
        for (;;) {
            SceCtrlData pad;
            sceCtrlReadBufferPositive(&pad, 1);
            if (pad.Buttons || !running) break;
            sceDisplayWaitVblankStart();
        }
        sceKernelExitGame();
        return 1;
    }
    uint32_t last = plat_ticks_ms();
    while (running && !game_quit_requested()) {
        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        uint32_t now = plat_ticks_ms();
        float dt = (now - last) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;
        game_update(dt, map_buttons(pad.Buttons));
        game_draw();
        static int frames;
        if (++frames == 1 || frames == 60) plat_log("frame %d ok, free %d KB", frames, (int)(sceKernelMaxFreeMemSize() / 1024));
    }
    game_shutdown();
    gfx_shutdown();
    sceKernelExitGame();
    return 0;
}
