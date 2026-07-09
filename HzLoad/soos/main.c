#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <malloc.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
    FlurryLoad - launches the Flurry streaming sysmodule and stays open as a
    small status/test app: the top screen shows a moving test pattern (so a
    connected client can judge FPS/quality live) plus the build version, and
    the bottom screen shows status and controls.
*/

#ifndef FLURRY_VERSION
#define FLURRY_VERSION "dev"
#endif

#define FLURRY_TID  0x000401300CF00A02ULL
#define HZMOD_TID   0x000401300CF00F02ULL
#define CHIRUNO_TID 0x000401300CF00902ULL
#define FLURRY_PORT 6464

#ifndef _HIMEM
Handle mcuHandle = 0;

Result mcuInit()
{
    return srvGetServiceHandle(&mcuHandle, "mcu::HWC");
}

Result mcuExit()
{
    return svcCloseHandle(mcuHandle);
}

Result mcuWriteRegister(u8 reg, void* data, u32 size)
{
    u32* ipc = getThreadCommandBuffer();
    ipc[0] = 0x20082;
    ipc[1] = reg;
    ipc[2] = size;
    ipc[3] = size << 4 | 0xA;
    ipc[4] = (u32)data;
    Result ret = svcSendSyncRequest(mcuHandle);
    if(ret < 0) return ret;
    return ipc[1];
}
#endif

// ---------------------------------------------------------------------------
// Top-screen software rendering, RGB565 (16bpp). We deliberately avoid the
// default 24bpp BGR8 framebuffer: the streaming sysmodule's inter-process
// DMA capture misbehaves on 3-byte pixels (black stream), and 16bpp is what
// games typically use anyway. The framebuffer is rotated 90 degrees: screen
// pixel (x,y), x in [0,400), y in [0,240) with y=0 at the top, lives at
// byte offset ((x*240) + (239-y)) * 2.

static inline void setpix(u8* fb, int x, int y, u8 r, u8 g, u8 b)
{
    if(x < 0 || x >= 400 || y < 0 || y >= 240) return;
    u32 o = ((x * 240) + (239 - y)) * 2;
    u16 c = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    fb[o+0] = c & 0xFF;
    fb[o+1] = c >> 8;
}

static void drawLine(u8* fb, int x0, int y0, int x1, int y1, u8 r, u8 g, u8 b)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if(steps == 0) steps = 1;
    for(int i = 0; i <= steps; i++)
    {
        setpix(fb, x0 + dx * i / steps, y0 + dy * i / steps, r, g, b);
    }
}

// Minimal 3x5 font (digits, lowercase letters, '.', '-', ':'), enough for
// version strings like "v0.1.1-5-gabc123". 3 bits per row, bit 2 = left.
static const u8 font3x5[][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1}, // 01234
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,2,2}, {7,5,7,5,7}, {7,5,7,1,7}, // 56789
    {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6}, {7,4,6,4,7}, // abcde
    {7,4,6,4,4}, {7,4,5,5,7}, {5,5,7,5,5}, {7,2,2,2,7}, {7,1,1,5,2}, // fghij
    {5,6,4,6,5}, {4,4,4,4,7}, {5,7,7,5,5}, {6,5,5,5,5}, {2,5,5,5,2}, // klmno
    {7,5,7,4,4}, {7,5,5,7,1}, {6,5,6,5,5}, {3,4,2,1,6}, {7,2,2,2,2}, // pqrst
    {5,5,5,5,7}, {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5}, {5,5,2,2,2}, // uvwxy
    {7,1,2,4,7},                                                     // z
    {0,0,0,0,2}, {0,0,7,0,0}, {0,2,0,2,0},                           // . - :
};

static int glyphIndex(char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'z') return 10 + (c - 'a');
    if(c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    if(c == '.') return 36;
    if(c == '-') return 37;
    if(c == ':') return 38;
    return -1; // space / unknown: skip
}

// Draw text at pixel scale 2 (glyph cell 8x12).
static void drawText(u8* fb, int x, int y, const char* s, u8 r, u8 g, u8 b)
{
    const int sc = 2;
    for(; *s; s++, x += 4 * sc)
    {
        int gi = glyphIndex(*s);
        if(gi < 0) continue;
        for(int row = 0; row < 5; row++)
            for(int col = 0; col < 3; col++)
                if(font3x5[gi][row] & (4 >> col))
                    for(int py = 0; py < sc; py++)
                        for(int px = 0; px < sc; px++)
                            setpix(fb, x + col * sc + px, y + row * sc + py, r, g, b);
    }
}

static int textWidth(const char* s)
{
    return strlen(s) * 4 * 2;
}

// Test pattern: rotating snowflake + orbiting dot + horizontal sweep bar.
// Continuous motion on every part of the screen makes stream FPS, tearing
// and strip-refresh behavior visible at a glance.
static void drawTestPattern(u8* fb, u32 frame)
{
    memset(fb, 0x00, 400 * 240 * 2); // black background

    float t = frame * 0.03f;
    int cx = 200, cy = 120;

    // 6 rotating arms with branch ticks
    for(int k = 0; k < 6; k++)
    {
        float a = t + k * (3.14159265f / 3.0f);
        float ca = cosf(a), sa = sinf(a);
        drawLine(fb, cx, cy, cx + (int)(70 * ca), cy + (int)(70 * sa), 120, 200, 255);
        int bx = cx + (int)(45 * ca), by = cy + (int)(45 * sa);
        float a2 = a + 0.7f, a3 = a - 0.7f;
        drawLine(fb, bx, by, bx + (int)(18 * cosf(a2)), by + (int)(18 * sinf(a2)), 120, 200, 255);
        drawLine(fb, bx, by, bx + (int)(18 * cosf(a3)), by + (int)(18 * sinf(a3)), 120, 200, 255);
    }

    // orbiting dot (counter-rotation)
    int ox = cx + (int)(95 * cosf(-t * 1.7f));
    int oy = cy + (int)(95 * sinf(-t * 1.7f));
    for(int dy = -2; dy <= 2; dy++)
        for(int dx = -2; dx <= 2; dx++)
            setpix(fb, ox + dx, oy + dy, 255, 255, 255);

    // sweep bar along the bottom (linear motion -> easy fps judgment)
    int bx = (frame * 3) % 400;
    for(int dx = 0; dx < 6; dx++)
        for(int y = 228; y < 238; y++)
            setpix(fb, (bx + dx) % 400, y, 198, 236, 255);

    // version, top-right (visible in the stream itself)
    const char* v = FLURRY_VERSION;
    drawText(fb, 400 - 4 - textWidth(v), 4, v, 198, 236, 255);

    // Color-calibration swatches, bottom-left, left-to-right R, G, B by
    // setpix intent. Ground truth for every channel-order question: what
    // the CONSOLE shows reveals the real framebuffer packing; what the
    // CLIENT shows reveals each decode path. Compare one photo + one
    // screenshot.
    for(int sy = 190; sy < 214; sy++)
        for(int sx = 0; sx < 24; sx++)
        {
            setpix(fb, 8 + sx, sy, 255, 0, 0);   // intended RED
            setpix(fb, 40 + sx, sy, 0, 255, 0);  // intended GREEN
            setpix(fb, 72 + sx, sy, 0, 0, 255);  // intended BLUE
        }
}

// ---------------------------------------------------------------------------

static void redrawConsole(u64 titleid, int running, Result lastres, u32 ip)
{
    printf("\x1b[2J"); // clear
    printf("\x1b[1;1H Flurry Loader %s\n", FLURRY_VERSION);
    printf(" --------------------------------------\n");
    printf(" Module:  %s\n", (titleid == HZMOD_TID) ? "HzMod" : "Flurry");
    if(running)
        printf(" Stream:  \x1b[32mRUNNING\x1b[0m\n");
    else if(lastres < 0)
        printf(" Stream:  \x1b[31mFAILED %08lX\x1b[0m\n", (unsigned long)lastres);
    else
        printf(" Stream:  \x1b[33mSTOPPED\x1b[0m\n");
    if(ip)
    {
        struct in_addr a = { .s_addr = ip };
        printf(" Address: %s:%d\n", inet_ntoa(a), FLURRY_PORT);
    }
    else
        printf(" Address: (WiFi off):%d\n", FLURRY_PORT);
    printf("\n");
    printf(" [A]     start / stop streaming\n");
    printf(" [START] exit (stream keeps running)\n");
}

int main()
{
    hidScanInput();
    u32 r = hidKeysHeld();
    u64 titleid;
    if(r & KEY_Y)
        titleid = HZMOD_TID;
    else
        titleid = FLURRY_TID;

#if _HIMEM
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, 0);
    printf("\n Flurry Loader %s (HIMEM)\n", FLURRY_VERSION);
    printf(" --------------------------------------\n");
    printf(" HIMEM mode: close this app from the\n");
    printf(" HOME Menu to free its memory and\n");
    printf(" start streaming.\n");
    gfxFlushBuffers();

    srvPublishToSubscriber(0x204, 0);
    srvPublishToSubscriber(0x205, 0);

    while(aptMainLoop())
    {
        svcSleepThread(5e7);
    }
    gfxExit();
#endif

    nsInit();

#ifndef _HIMEM
    // Shut down HzMod and the old ChirunoMod if they happen to be running.
    // A running Flurry is left alone so opening this app doesn't break an
    // active stream; use [A] to restart it (e.g. after an update).
    NS_TerminateProcessTID(HZMOD_TID);
    NS_TerminateProcessTID(CHIRUNO_TID); // old ChirunoMod (Flurry replaces it)
#endif

#ifndef _HIMEM
    if(r & KEY_X) // Do this thing and don't boot
    {
        if(mcuInit() >= 0)
        {
            u8 blk[0x64];
            memset(blk, 0, sizeof(blk));
            blk[2] = 0xFF;
            mcuWriteRegister(0x2D, blk, sizeof(blk));
            mcuExit();
        }
        nsExit();
        return 0;
    }
#endif

    {
        u32 pid;
        Result ret;
        int running;

        // Launch the sysmodule. If it's already running the launch fails —
        // treat that as "running" (if it was actually missing, toggling with
        // [A] will surface the real launch error).
        ret = NS_LaunchTitle(titleid, 0, &pid);
        running = 1;
        if(ret >= 0)
            ret = 0;

#if _HIMEM
        // HIMEM: the app is already exiting (loop above ended); launch and go.
        (void)running;
#else
        gfxInitDefault();
        gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES); // see setpix() comment
        consoleInit(GFX_BOTTOM, 0);

        // soc only for gethostid() (to display our IP); streaming sockets
        // live in the sysmodule, which has its own soc session.
        u32* socbuf = (u32*)memalign(0x1000, 0x100000);
        int hazsoc = socbuf && socInit(socbuf, 0x100000) >= 0;

        u32 ip = hazsoc ? (u32)gethostid() : 0;
        redrawConsole(titleid, running, ret, ip);

        u32 frame = 0;
        while(aptMainLoop())
        {
            hidScanInput();
            u32 kDown = hidKeysDown();

            if(kDown & KEY_START) break;

            if(kDown & KEY_A)
            {
                if(running)
                {
                    NS_TerminateProcessTID(titleid);
                    running = 0;
                    ret = 0;
                }
                else
                {
                    ret = NS_LaunchTitle(titleid, 0, &pid);
                    running = (ret >= 0);
                }
                redrawConsole(titleid, running, ret, ip);
            }

            // WiFi can come and go; refresh the displayed IP occasionally.
            if(hazsoc && (frame % 128) == 0)
            {
                u32 newip = (u32)gethostid();
                if(newip != ip)
                {
                    ip = newip;
                    redrawConsole(titleid, running, ret, ip);
                }
            }

            drawTestPattern(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), frame);
            frame++;

            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }

        if(hazsoc) socExit();
        gfxExit();
#endif
    }

    nsExit();

    return 0;
}
