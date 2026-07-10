#include <3ds.h>
#include <3ds/gpu/gx.h>

/*
    Flurry (based on ChirunoMod) - A utility background process for the Nintendo 3DS,
    purpose-built for screen-streaming over WiFi.

    Original HorizonM (HzMod) code is Copyright (C) 2017 Sono

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


// Debug Log Levels!
// Disable all for marginally better performance.
#define DEBUG_BASIC 1 //   Only log the most important and critical errors.
#define DEBUG_VERBOSE 0 // Log many more unnecessary details, including live performance profiling data sent to ChokiStream.

// Use experimental UDP instead of TCP. (Unfinished; doesn't work)
#define DEBUG_USEUDP 0

extern "C"
{
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <malloc.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/iosupport.h>

#include <poll.h>
#include <arpa/inet.h>

#include "miscdef.h"
//#include "service/screen.h"
#include "service/mcu.h"
#include "service/gx.h"
#include "service/hid.h"
#include "misc/pattern.h"
#include "misc/setdmacfg.h"
#include "imanip/imanip.h"
#include "netfunc/nfhelp.h"

#include "tga/targa.h"
#include <turbojpeg.h>
#include <zlib.h> // crc32, for skipping unchanged strips
}

#include <exception>

#include "utils.hpp"

extern u32 kDown;
extern u32 kHeld;
//extern u32 kUp;

#define yield() svcSleepThread(1e8)

#define hangmacro()\
        {\
    memset(&pat.r[0], 0x7F, 16);\
    memset(&pat.g[0], 0x7F, 16);\
    memset(&pat.b[0], 0x00, 16);\
    memset(&pat.r[16], 0, 16);\
    memset(&pat.g[16], 0, 16);\
    memset(&pat.b[16], 0, 16);\
    pat.ani = 0x1006;\
    PatApply();\
    while(1)\
    {\
        hidScanInputDirectIO();\
        if(kHeld == (KEY_SELECT | KEY_START))\
        {\
            goto killswitch;\
        }\
        yield();\
    }\
        }

int checkwifi();
int pollsock(int,int,int);
void CPPCrashHandler();

inline int setCpuResourceLimit(u32); // Unfinished, doesn't work.
void waitforDMAtoFinish(void*);
void debugPrint(u8, const char*);
void sendDebugFrametimeStats(double,double,double*,double);
int allocateScreenbufMem(u32**);

// netfunc
void newThreadMainFunction(void*);

// Helper functions for netfunc
inline int netfuncWaitForSettings();
void makeTargaImage(double*,double*,int,u32*,u32*,int*,u32*,bool,bool);
void makeJpegImage(double*,double*,int,u32*,u32*,int*,u32*,bool,bool);
void netfuncTestFramebuffer(u32*, GSPGPU_CaptureInfo, GSPGPU_CaptureInfo*);


int main(); // So you can call main from main (:



static int haznet = 0;
int checkwifi()
{
    haznet = 0;
    u32 wifi = 0;
    hidScanInputDirectIO();
    if(kHeld == (KEY_SELECT | KEY_START)) return 0;
    if(ACU_GetWifiStatus(&wifi) >= 0 && wifi) haznet = 1;
    return haznet;
}


int pollsock(int sock, int wat, int timeout = 0)
{
    struct pollfd pd;
    pd.fd = sock;
    pd.events = wat;

    if(poll(&pd, 1, timeout) == 1)
        return pd.revents & wat;
    return 0;
}

const int bufsoc_pak_data_offset = 8; // After 8 bytes, the real data begins.

// Socket Buffer class
class bufsoc
{
public:

    typedef struct
    {
        u8 data[0];
    } packet;

    int socketid;
    u8* bufferptr;
    int bufsize;
    // recvsize is useless; is never read from.
    int recvsize;

    bufsoc(int passed_sock, int passed_bufsize)
    {
        bufsize = passed_bufsize;
        bufferptr = new u8[passed_bufsize];

        recvsize = 0;
        socketid = passed_sock;
    }

    // Destructor
    ~bufsoc()
    {
        // If this socket buffer is already null,
        // don't attempt to delete it again.
        if(!this) return;
        close(socketid);
        delete[] bufferptr;
    }

    u8 getPakType()
    {
        return bufferptr[0];
    }

    u8 getPakSubtype()
    {
        return bufferptr[1];
    }

    u8 getPakSubtypeB()
    {
        return bufferptr[2];
    }

    u32 getPakSize()
    {
        return *( (u32*)(bufferptr+4) );
    }

    void setPakSize(u32 input)
    {
        *( (u32*)(bufferptr+4) ) = input;
        return;
    }

    void setPakType(u8 input)
    {
        bufferptr[0] = input;
        return;
    }

    void setPakSubtype(u8 input)
    {
        bufferptr[1] = input;
        return;
    }

    void setPakSubtypeB(u8 input)
    {
        bufferptr[2] = input;
        return;
    }

    int avail()
    {
        return pollsock(socketid, POLLIN) == POLLIN;
    }

    int readbuf(int flags = 0)
    {
        //puts("attempting recv function call...");
        int ret = recv(socketid, bufferptr, 8, flags);
        //printf("incoming packet type = %i\nsubtype1 = %i\nsubtype2 = %i\nrecv function return value = %i\n",bufferptr[0],bufferptr[1],bufferptr[2],ret);

        if(ret < 0) return -errno;
        if(ret < 8) return -1;

        u32 reads_remaining = getPakSize();
        //printf("incoming packet size = %i\nrecv return value = %i\n",reads_remaining,ret);

        // Copy data to the buffer

        u32 offs = bufsoc_pak_data_offset;
        while(reads_remaining)
        {
            ret = recv(socketid, &(bufferptr[offs]), reads_remaining, flags);
            if(ret <= 0) return -errno;
            reads_remaining -= ret;
            offs += ret;
        }

        return offs;
    }

    int wribuf(int flags = 0)
    {
        u32 mustwri = getPakSize() + bufsoc_pak_data_offset;
        int offs = 0;
        int ret = 0;

        // Send as much as possible per call. Each send() is an IPC into the
        // soc service (~5 ms), so the old 4 KB chunking cost one IPC per
        // 4 KB — a 40 KB packet = 10 IPCs. Cap at 56 KB (< the 64 KB soc
        // service buffer); TCP returns a partial count and the loop
        // continues, so large writes are safe.
        while(mustwri)
        {
            u32 chunk = mustwri > 0xE000 ? 0xE000 : mustwri;
            ret = send(socketid, &(bufferptr[offs]), chunk, flags);
            if(ret < 0) return -errno;
            mustwri -= ret;
            offs += ret;
        }

        return offs;
    }

    // Flags don't matter with UDP.
    // Pass the address of the "server" (client; PC; we are the server)
    // (Is this variable "sai" in most cases? I'll have to check)
    int wribuf_udp(struct sockaddr_in myservaddr)
    {
        u32 mustwri = getPakSize() + 8;
        int offs = 0;
        int ret = 0;

        while(mustwri)
        {
            u32 wri_now;
            if(mustwri > 0x1000)
            {
                wri_now = 0x1000;
            }
            else
            {
                wri_now = mustwri;
            }

            // get host by name here (maybe) (unfinished)

            ret = sendto(socketid, &(bufferptr[offs]), wri_now, 0, (struct sockaddr *)&myservaddr, sizeof(myservaddr));

            if(ret < 0)
                return -errno;

            mustwri -= ret;
            offs += ret;
        }

        return offs;
    }

    packet* pack()
    {
        return (packet*)bufferptr;
    }

    int errformat(char* c, ...)
    {
        int len = 0;

        va_list args;
        va_start(args, c);

        // Is this line of code broken? I give up
        len = vsprintf((char*)(bufferptr+bufsoc_pak_data_offset), c, args);

        va_end(args);

        if(len < 0)
        {
            puts("out of memory");
            return -1;
        }

        //printf("Packet error %i: %s\n", p->packettype, p->data + 1);
        setPakType(0xFF);
        setPakSubtype(0x00);
        setPakSize(len);

        return wribuf();
    }
};

static jmp_buf __exc;
static int  __excno;

void CPPCrashHandler()
{
    puts("\e[0m\n\n- The application has crashed\n\n");

    try
    {
        throw;
    }
    catch(std::exception &e)
    {
        printf("std::exception: %s\n", e.what());
    }
    catch(Result res)
    {
        printf("Result: %08X\n", res);
        //NNERR(res);
    }
    catch(int e)
    {
        printf("(int) %i\n", e);
    }
    catch(...)
    {
        puts("<unknown exception>");
    }

    puts("\n");

    PatStay(0xFFFFFF);
    PatPulse(0x0000FF);

    svcSleepThread(1e9);

    hangmacro();

    killswitch:
    longjmp(__exc, 1);
}


extern "C" u32 __get_bytes_per_pixel(GSPGPU_FramebufferFormats format);

const int port = 6464;

// GPU 'Capture Info' object.
// Data about framebuffers from the GSP.
static GSPGPU_CaptureInfo capin;
static GSPGPU_CaptureInfo oldcapin;

// Whether or not this is running on an 'Old' 3DS
static int isold = 1;

static Result ret = 0;

// Related to screen capture. Dimensions and color format.
static u32 offs[2] = {0, 0};
static u32 limit[2] = {1, 1};
static u32 stride[2] = {80, 80};
static u32 format[2] = {0xF00FCACE, 0xF00FCACE};

// Config Block. Indices are named so a typo can't silently drive the wrong
// knob. Wire setting subtypes map here (see PROTOCOL.md A.1 / B2):
//   0x01→QUALITY 0x03→SCREEN 0x04→FORMAT 0x05→INTERLACE 0x06→SKIP
//   0x07→REFRESH 0x08→FPSCAP 0x09→CHUNKS 0x0A→SLEEP 0x0B→DOWNSCALE
//   0x0C→STATS 0x0F→V2 0x10→GRID_COLS 0x11→GRID_ROWS
enum {
    CFG_CONNECTED    = 0,  // 0 = waiting, 1 = streaming
    CFG_QUALITY      = 1,  // JPEG quality 1-100
    CFG_CPUCAP       = 2,  // dummied out
    CFG_SCREEN       = 3,  // 1 top, 2 bottom, 3 both
    CFG_FORMAT       = 4,  // 0 JPEG, 1 TGA
    CFG_INTERLACE    = 5,
    CFG_SKIP         = 6,  // skip unchanged strips/cells
    CFG_REFRESH      = 7,  // force-send every N frames (0 = never)
    CFG_FPSCAP       = 8,  // 0 = uncapped
    CFG_CHUNKS       = 9,  // strips per screen (Old 3DS: 2/4/8)
    CFG_SLEEP        = 10, // ms between strips (Old 3DS pacing floor)
    CFG_DOWNSCALE    = 11, // quarter-res
    CFG_STATS        = 12, // 1 Hz perf stats on/off
    CFG_V2           = 15, // protocol v2 (SFRAME) framing
    CFG_GRID_COLS    = 16, // dirty-grid columns per screen
    CFG_GRID_ROWS    = 17, // dirty-grid rows per screen
};

// Config Block
static u8 cfgblk[0x100];

static int sock = 0;

static struct sockaddr_in sai;
static socklen_t sizeof_sai = sizeof(sai);

static bufsoc* soc = nullptr;

static bufsoc::packet* k = nullptr;

static Thread netthread = 0;
static vu32 threadrunning = 0;

static u32* screenbuf = nullptr;

static tga_image img;
static tjhandle jencode = nullptr;

static TickCounter tick_ctr_1;
static TickCounter tick_ctr_2_dma;

// This only applies to Interlaced mode.
// false = odd rows (1-239, DMA start position is normal)
// true  = even rows (2-240, DMA start position is offset)
static bool interlacedRowSwitch = false;

static bool isDmaSetForInterlaced = false;

// Set while GSPGPU_ImportDisplayCaptureInfo keeps failing, so the error is
// reported to the client once per streak instead of every iteration.
static bool import_fail_reported = false;
// Same, for svcStartInterProcessDma failures in the capture loop.
static bool dmafail_reported = false;
// Protocol v2: SFRAME sequence number, increments per sent pass.
static u16 v2_seq = 0;

// Double-buffered capture. The strip DMA takes 10-20 ms on Old 3DS (the
// legacy async pipeline existed to hide it under the encode of the previous
// strip â€” "dma=0.3 ms/s" was overlap, not speed; a fully synchronous loop
// starves at <1 fps with mid-copy aborts wedging DMA channels). Each buffer
// carries the metadata of the strip captured INTO it, so labels can never
// desync from content â€” the failure mode of the legacy shared-state
// pipeline (flicker / shuffled slices).
typedef struct
{
    u8 scr;
    u8 strip;
    u8 phase;    // decimation sample phase / interlace field at capture
    u8 fmt;      // GSP format snapshot (low 3 bits)
    u8 valid;    // a DMA was successfully started into this buffer
    u8 hwil;     // hardware-interlaced capture (New 3DS)
    u16 strip_x; // screen-space x of the strip
    u32 dlen;    // destination byte count (for the completion sentinel)
    u64 t_start; // system tick at DMA start (for the transfer-time EMA)
} capmeta;

// EMA of measured transfer durations in system ticks (0 = unknown yet).
static u64 dma_ema_ticks = 0;
// Once-per-streak reporter for torn (sentinel-timeout) captures.
static bool torn_reported = false;
// Rate limiter for the cellwatch diagnostic (phantom top-right cell).
static u64 cellwatch_ms = 0;
// Last app-capture params, persisted across capture-thread death so the
// next client connect can report what a crashing title reported.
static u64 g_lastapp_progid = 0;
static u32 g_lastapp_fmt = 0;
static u32 g_lastapp_wbs = 0;
static u8  g_lastapp_valid = 0;

// Sweep markers: set when a screen's strip index wraps, consumed by the
// first SFRAME actually sent afterwards (pass_flags bit 0) — the client's
// honest fps unit, robust to skipped strips.
static u8 sweep_pending[2] = {1, 1};

// Stamp a one-region v2 SFRAME (PROTOCOL.md B2) over image data ALREADY
// sitting at bufferptr+26, and set the packet size. Single source of truth
// for the wire layout — the three encode paths (raw rect / rect-JPEG /
// full-strip-JPEG rewrap) all funnel through here. Consumes the sweep
// marker for `scr`.
static void sframe_wrap(bufsoc* s, int scr, u16 x, u16 y, u16 w, u16 h,
                        u8 codec, u8 rflags, u32 dlen)
{
    u8* b = s->bufferptr;
    b[0] = 0x90;               // SFRAME
    b[1] = (u8)scr;
    b[2] = (u8)(v2_seq & 0xFF);
    b[3] = (u8)(v2_seq >> 8);
    v2_seq++;
    b[8] = sweep_pending[scr]; sweep_pending[scr] = 0; // pass_flags bit0
    b[9] = 1;                  // region_count
    b[10] = 0; b[11] = 0;      // reserved
    u8* r = b + 12;
    r[0] = x & 0xFF;  r[1] = x >> 8;
    r[2] = y & 0xFF;  r[3] = y >> 8;
    r[4] = w & 0xFF;  r[5] = w >> 8;
    r[6] = h & 0xFF;  r[7] = h >> 8;
    r[8] = codec;
    r[9] = rflags;
    r[10] = dlen & 0xFF;
    r[11] = (dlen >> 8) & 0xFF;
    r[12] = (dlen >> 16) & 0xFF;
    r[13] = (dlen >> 24) & 0xFF;
    s->setPakSize(4 + 14 + dlen);
}

// Completion sentinel: svcGetDmaState is unreliable for inter-process DMA
// on Old 3DS (it can fail outright), and trusting a fixed settle stops slow
// transfers mid-copy â€” torn strips (flicker) and wedged DMA channels
// (stutter/freeze bursts). The scheduler plants this value in the last
// destination word; the transfer overwriting it is direct evidence the
// copy reached the end.
#define CAP_SENTINEL 0xF1A6F1A6u

// Stage-3 dirty-cell grid: crc32 per cell of each captured full-res strip
// decides what actually changed; dirty cells coalesce to one bounding rect
// per strip, shipped raw (RGB565, no encode) when small or as the usual
// full-strip JPEG when large. Cell size is client-tunable (setting 0x10):
// 0 = 10x60 px (default), 1 = fine 5x30, 2 = coarse 25x120 — widths divide
// both chunk strides, heights divide 240. Indexed
// [screen][strip][cellcol][cellseg]; dims sized for the finest preset.
static u32 cell_crc[2][8][20][16]; // [screen][strip][cols≤20][rows≤16]
static u32* capbuf[2] = {nullptr, nullptr};
static capmeta cmeta[2];

// Flurry extension state (PROTOCOL.md A.1).
// crc32 of the last-sent content per [interlace phase][screen][strip],
// plus a frames-since-last-sent age per strip for the refresh interval.
static u32 strip_crc[2][2][8];
static u8 strip_age[2][2][8];
// osGetTime() of the previous paced iteration (fps cap, cfgblk[CFG_FPSCAP]).
static u64 pace_last = 0;
// Set when the chunk count (cfgblk[CFG_CHUNKS]) or format changes and screenbuf must
// be reallocated at the next safe point in the capture loop.
static vu8 chunks_realloc_needed = 0;
// Nonzero while encoding a quarter-res (software downscaled) strip: the row
// count the encoders should use instead of stride[scr]. Also drives wire
// subtype bit 7 so the client knows to scale 2x.
static u32 enc_rows_override = 0;

// Based on (and slightly modified from) devkitpro/libctru source
//
// svcSetResourceLimitValues(Handle res_limit, LimitableResource* resource_type_list, s64* resource_list, u32 count)
//
// Note : These aren't in 2017-libctru. I'm outta luck until compatibility with current libctru is fixed.
//
inline int setCpuResourceLimit(u32 cpu_time_limit)
{
    int ret = 0;

    Handle reslimithandle;
    ret = svcGetResourceLimit(&reslimithandle,0xFFFF8001);

    if(ret<0)
        return ret;

    //ResourceLimitType name = RESLIMIT_CPUTIME;
    s64 value = cpu_time_limit;
    //ret = svcSetResourceLimitValues(reslimithandle, &name, &value, 1);

    if(ret < 0)
        return ret;

    //ret = svcSetProcessResourceLimits(0xFFFF8001, reslimithandle);

    return ret;
}

double timems_dmaasync = 0;
u32 dmastatusthreadrunning = 0;
u32 dmafallbehind = 0;
Handle dmahand = 0;

void waitforDMAtoFinish(void* __dummy_arg__)
{
    // Don't have more than one thread running this
    // function at a time. Don't want to accidentally
    // overload and slow the 3DS.
    dmastatusthreadrunning = 1;

    int r1 = 0;

    //while(r1 != 4)// DMASTATE_DONE)
    //{
    //svcSleepThread(1e7); // 10 ms
    //r2 = svcGetDmaState(&r1,dmahand);
    //}


    r1 = svcWaitSynchronization(dmahand,500000); // keep trying and waiting for half a second

    if(r1 >= 0)
    {
        osTickCounterUpdate(&tick_ctr_2_dma);
        timems_dmaasync = osTickCounterRead(&tick_ctr_2_dma);
    }

    dmastatusthreadrunning = 0;
    return;
}

// Essentially puts() with an extra step.
void debugPrint(u8 log_level, const char *debug_string)
{
    if(log_level == 0)
    {
        puts(debug_string);
        // TODO: If debug is disabled and SD Card can't be accessed, send this message to ChokiStream.
    }

#if DEBUG_BASIC==1
    else if(log_level == 1)
    {
        puts(debug_string);
    }
#endif

#if DEBUG_VERBOSE==1
    else if(log_level == 2)
    {
        puts(debug_string);
    }
#endif

    return;
}

void sendDebugFrametimeStats(double ms_compress, double ms_writesocbuf, double* ms_dma, double ms_convert)
{
    const u32 charbuflimit = 100 + sizeof(char);
    char str1[charbuflimit];
    char str2[charbuflimit];
    char str3[charbuflimit];
    char str4[charbuflimit];

    sprintf(str4,"Image format conversion / interlacing took %g ms\n",ms_convert);
    sprintf(str1,"Image compression took %g ms\n",ms_compress);
    sprintf(str2,"Copying to Socket Buffer (in WRAM) took %g ms\n",ms_writesocbuf);

    if(*ms_dma == 0)
    {
        sprintf(str3,"DMA not yet finished\n");
        dmafallbehind++;
    }
    else
    {
        double ms_dma_localtemp = *ms_dma;
        *ms_dma = 0;
        sprintf(str3,"DMA copy from framebuffer to Flurry WRAM took %g ms (measurement is %i frames behind)\n",ms_dma_localtemp,dmafallbehind);
        dmafallbehind = 0;
    }

    soc->setPakType(0xFF);
    soc->setPakSubtype(03);

    char finalstr[500+sizeof(char)];

    u32 strsiz = sprintf(finalstr,"%s%s%s%s",str4,str1,str2,str3);

    strsiz--;

    for(u32 i=0; i<strsiz; i++)
    {
        ((char*)soc->bufferptr + bufsoc_pak_data_offset)[i] = finalstr[i];
    }

    soc->setPakSize(strsiz);
    soc->wribuf();
    return;
}


// Returns -1 on an error, and expects the calling function to close the socket.
// Returns 1 on success.
int netfuncWaitForSettings()
{
    while(1)
    {
        if((kHeld & (KEY_SELECT | KEY_START)) == (KEY_SELECT | KEY_START))
            return -1;

        debugPrint(1, "Reading incoming packet...");

        int r = soc->readbuf();
        if(r <= 0)
        {
            debugPrint(1, "Failed to recvbuf...");
            debugPrint(1, strerror(errno));
            return -1;
        }
        else
        {
            u8 i = soc->getPakSubtype();
            u8 j = soc->bufferptr[bufsoc_pak_data_offset];
            // Only used in one of these, but want to be declared up here.
            u32 k;
            u32 l;

            switch(soc->getPakType())
            {
            case 0x02: // Init (New ChirunoMod Packet Specification)
                cfgblk[CFG_CONNECTED] = 1;
                return 1;

            case 0x03: // Disconnect (new packet spec)
                cfgblk[CFG_CONNECTED] = 0;
                debugPrint(1, "Received packet type $03, forcibly disconnecting...");
                return -1;

            case 0x04: // Settings input (new packet spec)

                switch(i)
                {
                case 0x01: // JPEG Quality (1-100%)
                    // Error-Checking
                    if(j > 100)
                        cfgblk[CFG_QUALITY] = 100;
                    else if(j < 1)
                        cfgblk[CFG_QUALITY] = 1;
                    else
                        cfgblk[CFG_QUALITY] = j;
                    return 1;

                case 0x02: // CPU Cap value / CPU Limit / App Resource Limit

                    // Redundancy check
                    if(j == cfgblk[CFG_CPUCAP])
                        return 1;

                    // Maybe this is percentage of CPU time? (https://www.3dbrew.org/wiki/APT:SetApplicationCpuTimeLimit)
                    // In which case, values can range from 5% to 89%
                    // (The respective passed values are 5 and 89, respectively)
                    // So I don't know if 0x7F (127) will work.
                    //
                    // Maybe I'm looking at two different things by accident.

                    // Also, it may be required to set the 'reslimitdesc' in exheader a certain way (in cia.rsf)

                    if(j > 0x7F)
                        j = 0x7F;
                    else if(j < 5)
                        j = 5;

                    // This code doesn't work, lol.
                    // Functionality dummied out for now.
                    //setCpuResourceLimit((u32)j);

                    cfgblk[CFG_CPUCAP] = j;

                    return 1;

                case 0x03: // Which Screen
                    if(j != 0 && j < 4)
                        cfgblk[CFG_SCREEN] = j;
                    return 1;

                case 0x04: // Image Format (JPEG or TGA?)
                    if(j < 2)
                    {
                        // Chunk sizing depends on the format (TGA forces 8);
                        // reallocate if a custom chunk count is in play.
                        if(isold && j != cfgblk[CFG_FORMAT] && cfgblk[CFG_CHUNKS])
                            chunks_realloc_needed = 1;
                        cfgblk[CFG_FORMAT] = j;
                    }
                    return 1;

                case 0x05: // Request to use Interlacing (yes or no)
                    // Historically disabled on Old 3DS, but only because it
                    // was paired with full-frame capture (144 KB buffer).
                    // Combined with chunked capture the strip buffer already
                    // fits, so it's allowed everywhere now.
                    cfgblk[CFG_INTERLACE] = j?1:0;
                    return 1;

                case 0x06: // Flurry extension: skip unchanged strips (crc32)
                    cfgblk[CFG_SKIP] = j?1:0;
                    return 1;

                case 0x07: // Flurry extension: strip refresh interval (frames; 0 = never force)
                    cfgblk[CFG_REFRESH] = j;
                    return 1;

                case 0x08: // Flurry extension: fps cap (0 = uncapped)
                    cfgblk[CFG_FPSCAP] = (j > 60) ? 60 : j;
                    return 1;

                case 0x09: // Flurry extension: chunk count (Old 3DS): 2, 4 or 8
                    if((j == 1 || j == 2 || j == 4 || j == 8) && isold && cfgblk[CFG_CHUNKS] != j)
                    {
                        cfgblk[CFG_CHUNKS] = j;
                        chunks_realloc_needed = 1;
                    }
                    return 1;

                case 0x0A: // Flurry extension: per-strip sleep ms (Old 3DS pacing floor)
                    cfgblk[CFG_SLEEP] = (j > 20) ? 20 : j;
                    return 1;

                case 0x0B: // Flurry extension: quarter-res downscale (Old 3DS, bool)
                    cfgblk[CFG_DOWNSCALE] = j?1:0;
                    return 1;

                case 0x0C: // Flurry extension: 1 Hz perf stats on/off (bool)
                    cfgblk[CFG_STATS] = j?1:0;
                    return 1;

                case 0x0F: // Flurry extension: protocol v2 (SFRAME) framing
                    cfgblk[CFG_V2] = j?1:0;
                    return 1;

                case 0x10: // Flurry extension: dirty-grid columns per screen (4/8/16)
                    if((j == 4 || j == 8 || j == 16) && cfgblk[CFG_GRID_COLS] != j)
                    {
                        cfgblk[CFG_GRID_COLS] = j;
                        // Geometry changed: all stored cell crcs are stale.
                        memset(cell_crc, 0, sizeof(cell_crc));
                    }
                    return 1;

                case 0x11: // Flurry extension: dirty-grid rows per screen (1/2/4/8)
                    if((j == 1 || j == 2 || j == 4 || j == 8 || j == 16) && cfgblk[CFG_GRID_ROWS] != j)
                    {
                        cfgblk[CFG_GRID_ROWS] = j;
                        memset(cell_crc, 0, sizeof(cell_crc));
                    }
                    return 1;

                default:
                    // Invalid subtype for "Settings" packet-type
                    return 1;
                }
                return 1; // Just in case?

                case 0xFF: // Debug info. Prints to log file, interpreting the Data as u8 char objects.
                    // Note: packet subtype is ignored, lol.
#if DEBUG_BASIC==1
                    k = soc->getPakSize();
                    // Current offset
                    l = 0;

                    if(k > 255) // Error checking; arbitrary limit on text characters.
                        k = 255;

                    while(k > 0)
                    {
                        printf((char*)(soc->bufferptr + bufsoc_pak_data_offset));
                        k--;
                        l++;
                    }
#endif
                    return 1;

                default:
                    debugPrint(1, "Invalid packet ID...");
#if DEBUG_BASIC==1
                    printf("%i", soc->getPakType());
#endif
                    return -1;
            }

            return 1;
        }
    }

    return 1;
}

int allocateScreenbufMem(u32** myscreenbuf)
{
    u32 screenbuf_siz = 0;
    debugPrint(1, "(re)allocating memory for screenbuf...");
    if(isold) // is Old-3DS
    {
        /* interlacing is disabled on o3DS
        // If Interlaced
        if(cfgblk[CFG_INTERLACE] == 1)
        {
            limit[0] = 1;
            limit[1] = 1;
            stride[0] = 400;
            stride[1] = 320;
            screenbuf_siz = 400 * 120 * 3;
        }
        else
        {
        */
        // Chunk count (Flurry extension, setting 0x09 -> cfgblk[CFG_CHUNKS]):
        // fewer, larger strips cut per-strip encode setup and socket IPC.
        // 2 is the floor: full frames would need a bigger socket buffer.
        // TGA is RLE with a large worst case, so it keeps the 8-chunk bound.
        // c=1 = full-frame capture (one DMA/screen, all pixels the same
        // instant → no capture skew, like New 3DS). Feasible now that the
        // cell grid never sends a full RAW frame (only dirty rects or a
        // full-frame JPEG, both fit the socket buffer). Flurry runs as a
        // 64MB app, so the 288 KB buffer is fine. TGA keeps 8 chunks (RLE
        // worst case must fit the socket buffer).
        u32 c = cfgblk[CFG_CHUNKS];
        if(c != 1 && c != 2 && c != 4)
            c = 8;
        if(cfgblk[CFG_FORMAT] == 1 && c == 1)
            c = 8;
        limit[0] = c; // Capture the screen in c chunks
        limit[1] = c;
        stride[0] = 400 / c; // Screen / Framebuffer width (divided by c)
        stride[1] = 320 / c;
        screenbuf_siz = (400 / c) * 240 * 3;
    }
    else
    {
        limit[0] = 1;
        limit[1] = 1;
        stride[0] = 400;
        stride[1] = 320;
        screenbuf_siz = 400 * 240 * 3;
    }

    // Double-buffered capture: two independent allocations (no contiguity
    // requirement between them).
    int i = 0;
    while(i < 5 && (!capbuf[0] || !capbuf[1]))
    {
        if(!capbuf[0]) capbuf[0] = (u32*)memalign(8, screenbuf_siz);
        if(!capbuf[1]) capbuf[1] = (u32*)memalign(8, screenbuf_siz);

        if(!capbuf[0] || !capbuf[1])
        {
            debugPrint(1, "memalign failed, retrying...");
            makerave();
            svcSleepThread(2e9);
        }
        i++;
    }

    if(!capbuf[0] || !capbuf[1])
    {
        if(capbuf[0]) { free(capbuf[0]); capbuf[0] = nullptr; }
        if(capbuf[1]) { free(capbuf[1]); capbuf[1] = nullptr; }
        debugPrint(1, "Error: out of memory (memalign failed trying to allocate memory for screenbuf)");
        return -1;
    }

    cmeta[0].valid = 0;
    cmeta[1].valid = 0;
    *myscreenbuf = capbuf[0];
    return 0;
}

void makeTargaImage(double* timems_fc, double* timems_pf, int scr, u32* scrw, u32* bits, int* imgsize, u32* myformat, bool isInterlaced, bool interlacedRowSwitch)
{
#if DEBUG_VERBOSE==1
    *timems_fc = 0;
    osTickCounterUpdate(&tick_ctr_1);
#endif

    u32 newbits = *bits;

    switch(myformat[scr] & 0b111)
    {
    case 0: // RGBA8
        newbits = 32;
        break;

    case 1: // RGB8
        newbits = 24;
        break;

    case 2: // RGB565
        newbits = 17;
        break;

    case 3: // RGB5A1
        newbits = 16;
        break;

    case 4: // RGBA4
        newbits = 18;
        break;

    default:
        // Invalid
        break;
    }

    u32 enc_rows = enc_rows_override ? enc_rows_override : stride[scr];

    init_tga_image(&img, (u8*)screenbuf, *scrw, enc_rows, newbits);
    img.image_type = TGA_IMAGE_TYPE_BGR_RLE;

    // horizontal offset. redundant in chirunomod/flurry.
    img.origin_y = (scr * 400) + (stride[scr] * offs[scr]);

    tga_write_to_FILE((soc->bufferptr+bufsoc_pak_data_offset), &img, imgsize);

#if DEBUG_VERBOSE==1
    osTickCounterUpdate(&tick_ctr_1);
    *timems_pf = osTickCounterRead(&tick_ctr_1);
#endif

    u8 subtype_aka_flags = 0b00001000 + (scr * 0b00010000) + (format[scr] & 0b111);
    if(isInterlaced)
        subtype_aka_flags += 0b00100000 + (interlacedRowSwitch?0:0b01000000);
    if(enc_rows_override)
        subtype_aka_flags |= 0b10000000; // quarter-res: client scales 2x2

    soc->setPakType(01);
    soc->setPakSubtype(subtype_aka_flags);
    soc->setPakSize(*imgsize);

    return;
}

void makeJpegImage(double* timems_fc, double* timems_pf, int scr, u32* scrw, u32* bsiz, int* imgsize, u32* myformat, bool isinterlaced, bool interlacedRowSwitch)
{
    u8 nativepixelformat = myformat[scr] & 0b111;
    u8 subtype_aka_flags = 0b00000000 + (scr * 0b00010000) + nativepixelformat;
    int tjpixelformat = 0;
    u32 enc_rows = enc_rows_override ? enc_rows_override : stride[scr];

    if(isinterlaced)
    {
        subtype_aka_flags += 0b00100000 + (interlacedRowSwitch?0:0b01000000);
    }
    if(enc_rows_override)
        subtype_aka_flags |= 0b10000000; // quarter-res: client scales 2x2

#if DEBUG_VERBOSE==1
    osTickCounterUpdate(&tick_ctr_1);
#endif

    switch(nativepixelformat)
    {
    case 0: // RGBA8
        //tjpixelformat = TJPF_RGBX;
        tjpixelformat = TJPF_RGB;
        *bsiz = 3;
        break;

    case 1: // RGB8
        tjpixelformat = TJPF_RGB;
        *bsiz = 3;
        break;

    case 2: // RGB565
        convert16to24_rgb565(enc_rows, *scrw, (u8*)screenbuf);
        tjpixelformat = TJPF_RGB;
        *bsiz = 3;
        break;

    case 3: // RGB5A1
        convert16to24_rgb5a1(enc_rows, *scrw, (u8*)screenbuf);
        tjpixelformat = TJPF_RGB;
        *bsiz = 3;
        break;

    case 4: // RGBA4
        convert16to24_rgba4(enc_rows, *scrw, (u8*)screenbuf);
        tjpixelformat = TJPF_RGB;
        *bsiz = 3;
        break;

    default:
        tjpixelformat = TJPF_RGB;
        //*bsiz = 3;
        break;
    }

#if DEBUG_VERBOSE==1
    osTickCounterUpdate(&tick_ctr_1);
    *timems_fc = osTickCounterRead(&tick_ctr_1);
#endif

    u8* destaddr = soc->bufferptr + bufsoc_pak_data_offset;

    if(!tjCompress2(jencode, (u8*)screenbuf, *scrw, (*bsiz) * (*scrw), enc_rows, tjpixelformat, &destaddr, (u32*)imgsize, TJSAMP_420, cfgblk[CFG_QUALITY], TJFLAG_NOREALLOC | TJFLAG_FASTDCT))
    {
#if DEBUG_VERBOSE==1
        osTickCounterUpdate(&tick_ctr_1);
        *timems_pf = osTickCounterRead(&tick_ctr_1);
#endif
        soc->setPakSize(*imgsize);
    }
    else
    {
#if DEBUG_VERBOSE==1
        *timems_pf = 0;
#endif
    }

    soc->setPakType(01); //Image
    soc->setPakSubtype(subtype_aka_flags);
    return;
}

u8 getFormatBpp(u32 my_format)
{
    switch(my_format & 0b111)
    {
    case 0: // 32bpp
        return 32;

    case 1: // 24bpp
        return 24;

    default: // 16bpp
        return 16;
    }
}

// Capture route of the previous framebuffer change: 0 unknown, 1 VRAM,
// 2 application. Used to rate-limit telemetry and detect route switches.
static u8 last_route = 0;

static bool netfuncIsProcidAlive(u32 procid)
{
    Handle h = 0;
    if(svcOpenProcess(&h, procid) < 0)
        return false;
    svcCloseHandle(h);
    return true;
}

// If framebuffers changed, do APT stuff (if necessary)
// NOTE: old_captureinfo must be passed by pointer â€” it used to be taken by
// value, so the caller's copy never updated and "changed" re-triggered every
// frame once an application was in the foreground, wedging the capture
// thread in the NS retry loop below (stream froze in any app).
void netfuncTestFramebuffer(u32* procid, GSPGPU_CaptureInfo new_captureinfo, GSPGPU_CaptureInfo* old_captureinfo)
{
    bool is_changed = false;

    for(int s = 0; s < 2; s++)
    {
        if(new_captureinfo.screencapture[s].framebuf0_vaddr != old_captureinfo->screencapture[s].framebuf0_vaddr)
        {
            old_captureinfo->screencapture[s].framebuf0_vaddr = new_captureinfo.screencapture[s].framebuf0_vaddr;
            is_changed = true;
        }
        if(new_captureinfo.screencapture[s].framebuf_widthbytesize != old_captureinfo->screencapture[s].framebuf_widthbytesize)
        {
            old_captureinfo->screencapture[s].framebuf_widthbytesize = new_captureinfo.screencapture[s].framebuf_widthbytesize;
            is_changed = true;
        }
        if(new_captureinfo.screencapture[s].format != old_captureinfo->screencapture[s].format)
        {
            old_captureinfo->screencapture[s].format = new_captureinfo.screencapture[s].format;
            is_changed = true;
        }
    }

    if(is_changed)
    {
        u32 fbva = (u32)new_captureinfo.screencapture[0].framebuf0_vaddr;

        // Real VRAM only: 0x1F000000-0x1F5FFFFF (retail applets render
        // there). Application linear heap lives at 0x30000000+ for
        // commercial titles and 0x14000000+ for libctru homebrew. The old
        // check (fbva < 0x1F600000) misrouted homebrew framebuffers to the
        // VRAM path, so the sysmodule DMA'd 0x14000000 of its OWN address
        // space and homebrew apps froze the stream.
        bool is_vram = (fbva >= 0x1F000000) && (fbva < 0x1F600000);

        if(is_vram)
        {
            *procid = 0;
            // If the framebuffer is in VRAM, we don't have to do anything special(...?)
            // (Such is the case for all retail applets, apparently.)
            tryStopDma(&dmahand);

            // Telemetry: report route changes, not every buffer swap.
            if(soc && last_route != 1)
                soc->errformat((char*)"capture: vram fb, vaddr=%08X", fbva);
            last_route = 1;
        }
        else if(*procid && netfuncIsProcidAlive(*procid))
        {
            // Still the same (double-buffered) application flipping between
            // its two framebuffers; keep the known procid instead of
            // re-running the APT/NS discovery every swap.
        }
        else //use APT fuckery, auto-assume this is an application
        {
            *procid = 0;
            last_route = 2;
            // Notif LED = Flashing red and green
            memset(&pat.r[0], 0xFF, 16);
            memset(&pat.r[16], 0, 16);
            memset(&pat.g[0], 0, 16);
            memset(&pat.g[16], 0xFF, 16);
            memset(&pat.b[0], 0, 32);
            pat.ani = 0x2004;
            PatApply();

            u64 progid = -1ULL;
            bool loaded = false;
            u8 mediatype = 0;
            Result nsret = 0;

            // Bounded (~0.5 s of attempts): giving up and streaming nothing
            // beats wedging the capture thread forever. With old_captureinfo
            // updated properly this runs once per framebuffer change, not
            // every frame.
            for(int tries = 0; tries < 32; tries++)
            {
                // loaded = Registration Status(?) of the specified application.
                loaded = false;
                while(1)
                {
                    NS_APPID appid;
                    if(APT_GetAppletManInfo(APTPOS_NONE, nullptr, nullptr, nullptr, &appid) < 0)
                        break;
                    appid = (NS_APPID)(appid & 0xFFFF);
                    if(APT_GetAppletInfo(appid, &progid, &mediatype, &loaded, nullptr, nullptr) < 0)
                        break;
                    if(loaded)
                        break;
                    svcSleepThread(15e6);
                }
                if(!loaded)
                    break;
                // see: https://github.com/ChainSwordCS/ChirunoMod/issues/11
                //if(mediatype == 2)
                //    progid = 0; // Game Card
                nsret = NS_LaunchTitle(progid, 0, procid);
                if(nsret >= 0)
                    break;
                svcSleepThread(15e6);
            }

            // Telemetry through the stream so the connected client can show
            // what the app-capture path decided (shows up as "3DS: ...").
            if(soc)
            {
                soc->errformat((char*)"capture: app fb, vaddr=%08X progid=%08X%08X loaded=%i media=%i ns=%08X pid=%u",
                               (u32)new_captureinfo.screencapture[0].framebuf0_vaddr,
                               (u32)(progid >> 32), (u32)progid, (int)loaded,
                               (int)mediatype, (u32)nsret, (unsigned int)*procid);
                // fb geometry — a game whose format/stride we mishandle
                // (e.g. stereo-3D OoT) faults the capture thread here.
                soc->errformat((char*)"capture: fb geom top fmt=%08X wbs=%u | bot fmt=%08X wbs=%u",
                               (u32)new_captureinfo.screencapture[0].format,
                               (unsigned int)new_captureinfo.screencapture[0].framebuf_widthbytesize,
                               (u32)new_captureinfo.screencapture[1].format,
                               (unsigned int)new_captureinfo.screencapture[1].framebuf_widthbytesize);
            }
            // Stash for post-crash forensics: if the capture thread faults
            // on this app's fb (stream send lost with the dropped TCP), the
            // next client connect re-emits these — the culprit's params
            // survive the thread death.
            g_lastapp_progid = progid;
            g_lastapp_fmt = (u32)new_captureinfo.screencapture[0].format;
            g_lastapp_wbs = (u32)new_captureinfo.screencapture[0].framebuf_widthbytesize;
            g_lastapp_valid = 1;

            if(!loaded || nsret < 0)
                format[0] = 0xF00FCACE; //invalidate
            PatStay(0x00FF00); // Notif LED = Green
        }
    }
    return;
}

// "netfunc" v2.1
void newThreadMainFunction(void* __dummy_arg__)
{
#if DEBUG_VERBOSE==1
    osTickCounterStart(&tick_ctr_1);
    osTickCounterStart(&tick_ctr_2_dma);
#endif
    double timems_processframe = 0;
    double timems_writetosocbuf = 0;
    double timems_formatconvert = 0;

    // Per-second perf accounting, reported to the client as a 1 Hz stats
    // packet (0xFF/0x03). Tick sums per stage = ms spent per wall second.
    u64 st_dma = 0, st_crc = 0, st_enc = 0, st_send = 0;
    u32 st_sent = 0, st_skip = 0, st_torn = 0;
    u64 st_epoch_ms = osGetTime();
    u64 st_t0 = 0;

    // Index of the capture buffer with a DMA in flight (-1 = none).
    int cap_pending = -1;

    // Multi-rect batching: consecutive same-screen raw dirty rects
    // accumulate into one SFRAME (region_count>1) in the socket buffer and
    // send once, instead of one packet per strip — socket sends cost
    // per-PACKET (~5 ms IPC, the current bottleneck). Payload grows in
    // soc->bufferptr+8; batch_scr = which screen it belongs to (-1 none).
    int batch_scr = -1;
    u32 batch_payload = 4; // pass header (flags,count,00,00) + regions
    u8  batch_count = 0;
    auto flush_batch = [&]()
    {
        if(batch_count == 0 || batch_scr < 0 || !soc)
        {
            batch_scr = -1; batch_payload = 4; batch_count = 0;
            return;
        }
        u8* b = soc->bufferptr;
        b[0] = 0x90;              // SFRAME
        b[1] = (u8)batch_scr;
        b[2] = (u8)(v2_seq & 0xFF);
        b[3] = (u8)(v2_seq >> 8);
        v2_seq++;
        // Sweep marker consumed the same way sframe_wrap does, so batched
        // and per-strip sends agree on the fps unit (no double count).
        b[8] = sweep_pending[batch_scr]; sweep_pending[batch_scr] = 0;
        b[9] = batch_count;       // region_count
        b[10] = 0; b[11] = 0;
        soc->setPakSize(batch_payload);
        u64 t = svcGetSystemTick();
        soc->wribuf();
        st_send += svcGetSystemTick() - t;
        st_sent++;
        batch_scr = -1; batch_payload = 4; batch_count = 0;
    };

    u32 siz = 0x80;
    u32 bsiz = 1;
    u32 scrw = 2;
    u32 bits = 8;
    int scr = 0;
    u32 procid = 0;
    bool doDMA = true;

    u8 dma_cfg_0[0x18];
    u8 dma_cfg_1[0x18];
    u8* dma_config[2];
    dma_config[0] = dma_cfg_0;
    dma_config[1] = dma_cfg_1;
    initCustomDmaCfg(dma_config[0]);
    initCustomDmaCfg(dma_config[1]);

    threadrunning = 1;

    // Fresh connection: forget strip crcs, pacing, and capture route state.
    memset(strip_crc, 0, sizeof(strip_crc));
    memset(strip_age, 0, sizeof(strip_age));
    memset(cell_crc, 0, sizeof(cell_crc));
    // Stagger refresh ages: seeded equal, every strip crosses the refresh
    // threshold on the same pass and the burst of forced full-strip resends
    // (~100-200 ms of CPU + memory bus) visibly hitches the foreground app
    // on a periodic beat. Distinct seeds phase-shift each strip's refresh
    // permanently.
    for(int p2 = 0; p2 < 2; p2++)
        for(int s2 = 0; s2 < 2; s2++)
            for(int i2 = 0; i2 < 8; i2++)
                strip_age[p2][s2][i2] = (u8)((i2 * 13 + s2 * 29 + p2 * 7) & 63);
    pace_last = 0;
    last_route = 0;
    import_fail_reported = false;
    dma_ema_ticks = 0;

    if(isold == 0){
        osSetSpeedupEnable(1);
    }

    PatStay(0x00FF00); // Notif LED = Green

    while(cfgblk[CFG_CONNECTED] == 0)
    {
        if(soc->avail())
        {
            int ret_nwfs = netfuncWaitForSettings();
            if(ret_nwfs < 0)
            {
                delete soc;
                soc = nullptr;
            }
            else if(ret == 9)
            {
                debugPrint(1, "o3DS toggled Interlaced setting, reallocating screenbuf...");
                free(screenbuf);
                screenbuf = nullptr;
                yield(); // does this help
                allocateScreenbufMem(&screenbuf);
            }
        }
        if(!soc) break;
    }

    // properly initialize oldcapin first
    while(true)
    {
        if(GSPGPU_ImportDisplayCaptureInfo(&capin) >= 0)
            if(GSPGPU_ImportDisplayCaptureInfo(&oldcapin) >= 0)
                break;
    }

    // ?
    format[0] = capin.screencapture[0].format & 0b111;
    format[1] = capin.screencapture[1].format & 0b111;

    // Hardware (DMA) interlacing: New 3DS only â€” the Old 3DS kernel
    // data-aborts on gather-stride DMA configs (kernel crash, FAR=0x20).
    // Old 3DS interlaces in software instead (see the decimation pass in
    // the capture loop). 24bpp frames are excluded either way.
    if(cfgblk[CFG_INTERLACE] && !isold && (format[scr] != 1))
        isDmaSetForInterlaced = true;
    else
        isDmaSetForInterlaced = false;

    updateDmaCfgBpp(dma_config[0], getFormatBpp(format[0]), cfgblk[CFG_INTERLACE], capin.screencapture[0].framebuf_widthbytesize);
    updateDmaCfgBpp(dma_config[1], getFormatBpp(format[1]), cfgblk[CFG_INTERLACE], capin.screencapture[1].framebuf_widthbytesize);

    // Housekeeping (settings poll, GSP capture-info import, DMA cfg) is
    // throttled: each is a service IPC and doing them per strip was a large
    // fixed cost per iteration (~30 ms/strip measured, pixel-independent).
    // 50 ms keeps app-switch/settings latency invisible. Same reason the
    // LED PatStay (mcu IPC) left the loop: set once, restored by any path
    // that changes it.
    PatStay(0x00FF00); // Notif LED = Green

    u64 last_housekeep_ms = 0;
    Result impret = -1;

    // Infinite loop unless it crashes or is halted by another application.
    while(threadrunning)
    {
        if(!soc) break;

        bool housekeep = false;
        {
            u64 now_hk = osGetTime();
            if(now_hk - last_housekeep_ms >= 50)
            {
                housekeep = true;
                last_housekeep_ms = now_hk;
            }
        }

        // Incoming settings read into soc->bufferptr — flush a pending batch
        // first so it isn't clobbered. Only when a packet is actually
        // waiting (rare), so normal batching is untouched.
        if(housekeep && soc->avail())
            flush_batch();
        while(housekeep && soc->avail())
        { // ?

            int ret_nwfs = netfuncWaitForSettings();
            if(ret_nwfs < 0)
            {
                delete soc;
                soc = nullptr;
                break;
            }
            /* interlacing is disabled on o3DS
            else if(ret_nwfs == 9)
            {
                debugPrint(1, "o3DS toggled Interlaced setting, reallocating screenbuf...");
                free(screenbuf);
                screenbuf = nullptr;
                yield(); // does this help
                allocateScreenbufMem(&screenbuf);
            }
            */
        }
        if(!soc) break;

#if DEBUG_VERBOSE==1
        sendDebugFrametimeStats(timems_processframe,timems_writetosocbuf,&timems_dmaasync,timems_formatconvert);
#endif

        // Capture info refreshes EVERY pass: framebuf0_vaddr tracks the
        // app's buffer swaps (60 Hz), and reading it on the old 50 ms
        // housekeeping cadence meant capturing the buffer being actively
        // redrawn half the time — mid-draw frames (cellwatch: version-text
        // cell genuinely alternating content; color flicker; source-side
        // tearing). The rest of the housekeeping stays throttled.
        impret = GSPGPU_ImportDisplayCaptureInfo(&capin);
        if(impret >= 0)
        {
            if(housekeep)
            {
            import_fail_reported = false;
            netfuncTestFramebuffer(&procid,capin,&oldcapin);

            format[0] = capin.screencapture[0].format & 0b111;
            format[1] = capin.screencapture[1].format & 0b111;

            // Hardware (DMA) interlacing: New 3DS only (o3DS kernel aborts
            // on the required gather-stride config; software path below).
            if(cfgblk[CFG_INTERLACE] && !isold && (getFormatBpp(format[scr]) != 24))
                isDmaSetForInterlaced = true;
            else
                isDmaSetForInterlaced = false;

            switch(cfgblk[CFG_SCREEN])
            {
            case 1:
                updateDmaCfgBpp(dma_config[0], getFormatBpp(format[0]), isDmaSetForInterlaced?1:0, capin.screencapture[0].framebuf_widthbytesize);
                break;
            case 2:
                updateDmaCfgBpp(dma_config[1], getFormatBpp(format[1]), isDmaSetForInterlaced?1:0, capin.screencapture[1].framebuf_widthbytesize);
                break;
            default:
                updateDmaCfgBpp(dma_config[0], getFormatBpp(format[0]), isDmaSetForInterlaced?1:0, capin.screencapture[0].framebuf_widthbytesize);
                updateDmaCfgBpp(dma_config[1], getFormatBpp(format[1]), isDmaSetForInterlaced?1:0, capin.screencapture[1].framebuf_widthbytesize);
                break;
            }
            }

            // ---- Synchronous capture: DMA, wait, encode and send all refer
            // to the SAME (screen, strip) in the SAME iteration. The legacy
            // pipeline captured one iteration ahead of labeling, and every
            // desync (torn DMA, failed start, timing shifts) shipped strips
            // with the wrong screen/index â€” flicker, shuffled slices. The
            // overlap it bought was worth ~0.3 ms/s (measured). ----

            // ---- Double-buffered capture ----
            // 1) Finish the transfer started last iteration (it has had a
            //    whole encode+send to complete, so this is normally instant).
            bool have_ready = false;
            int ready = -1;
            if(cap_pending >= 0 && cmeta[cap_pending].valid)
            {
                st_t0 = svcGetSystemTick();
                // Completion = the sentinel in the last destination word got
                // overwritten (direct evidence the copy reached the end);
                // svcGetDmaState is only a fast-path hint since it can fail
                // or lie for inter-process handles on Old 3DS.
                volatile u32* sentinel =
                    (volatile u32*)&capbuf[cap_pending][(cmeta[cap_pending].dlen / 4) - 1];
                // Invalidate the sentinel's cache line before every read:
                // the DMA writes RAM, not the CPU cache; a stale cached
                // sentinel reads as an eternal fake torn.
                svcFlushProcessDataCache(0xFFFF8001, (u8*)sentinel, 4);
                bool done = (*sentinel != CAP_SENTINEL);

                // Sleep most of the expected remaining transfer time in one
                // go (EMA of measured durations) instead of hundreds of
                // 50 µs polls whose real granularity is ~2-4x that.
                if(!done && dma_ema_ticks)
                {
                    u64 elapsed = svcGetSystemTick() - cmeta[cap_pending].t_start;
                    if(elapsed < dma_ema_ticks)
                    {
                        u64 remain_ns = (dma_ema_ticks - elapsed) * 1000000000ULL / 268123480ULL;
                        if(remain_ns > 300000)
                            svcSleepThread(remain_ns - 150000);
                    }
                }
                for(int i = 0; !done && i < 800; i++)
                {
                    svcFlushProcessDataCache(0xFFFF8001, (u8*)sentinel, 4);
                    if(*sentinel != CAP_SENTINEL)
                    {
                        done = true;
                        break;
                    }
                    if((i & 7) == 7)
                    {
                        int dmaState = -1;
                        svcFlushProcessDataCache(0xFFFF8001, (u8*)sentinel, 4);
                        if(svcGetDmaState(&dmaState, dmahand) >= 0 && dmaState == 4
                           && *sentinel != CAP_SENTINEL)
                        {
                            done = true;
                            break;
                        }
                    }
                    svcSleepThread(5e4);
                }
                tryStopDma(&dmahand);
                st_dma += svcGetSystemTick() - st_t0;
                if(done)
                {
                    // Track how long transfers really take (start → done).
                    u64 took = svcGetSystemTick() - cmeta[cap_pending].t_start;
                    dma_ema_ticks = dma_ema_ticks ? (dma_ema_ticks * 3 + took) / 4 : took;
                    ready = cap_pending;
                    have_ready = true;
                }
                else
                {
                    // Torn (or the strip legitimately ends in the sentinel
                    // value â€” vanishingly rare): dropped, recaptured on the
                    // next scheduler pass.
                    st_torn++;
                    // Identify the tearing pattern (screen/format tells us
                    // whether e.g. 24bpp destinations never write the last
                    // word); once per streak.
                    if(soc && !torn_reported)
                    {
                        torn_reported = true;
                        soc->errformat((char*)"capture: torn scr=%u strip=%u fmt=%u dlen=%u",
                                       (unsigned int)cmeta[cap_pending].scr,
                                       (unsigned int)cmeta[cap_pending].strip,
                                       (unsigned int)cmeta[cap_pending].fmt,
                                       (unsigned int)cmeta[cap_pending].dlen);
                    }
                }
                if(have_ready)
                    torn_reported = false;
                cap_pending = -1;
            }

            // 2) Schedule the next capture into the other buffer from the
            //    CURRENT scheduler state, then advance that state. The
            //    metadata freezes everything the processing side needs, so
            //    labels travel with the pixels.
            {
                int nxt = (ready == 0) ? 1 : 0;
                capmeta* m = &cmeta[nxt];
                m->scr = (u8)scr;
                m->strip = (u8)offs[scr];
                m->phase = interlacedRowSwitch ? 1 : 0;
                m->fmt = (u8)(format[scr] & 0b111);
                m->hwil = isDmaSetForInterlaced ? 1 : 0;
                m->strip_x = (u16)(offs[scr] * stride[scr]);
                m->valid = 0;

                if(format[scr] != 0xF00FCACE)
                {
                    u32 region_siz = capin.screencapture[scr].framebuf_widthbytesize * stride[scr];

                    Handle prochand = 0;
                    if(procid) if(svcOpenProcess(&prochand, procid) < 0) procid = 0;
                    u32 srcprochand = prochand ? prochand : 0xFFFF8001;
                    u8* srcaddr = (u8*)capin.screencapture[scr].framebuf0_vaddr + (region_siz * offs[scr]);

                    u32 siz2 = (getFormatBpp(format[scr]) / 8) * 240 * stride[scr];
                    if(m->hwil)
                    {
                        siz2 = siz2 / 2;
                        if(m->phase)
                            srcaddr += getFormatBpp(format[scr]) / 8;
                    }
                    // workaround for DMA Siz Bug (refer to docs)
                    siz2 += (getFormatBpp(format[scr]) / 8) * (16 * stride[scr] - 16);

                    // Destination length (32bpp sources gather to 24bpp) and
                    // the completion sentinel in the last destination word.
                    {
                        u32 dbpp = getFormatBpp(format[scr]);
                        if(dbpp == 32) dbpp = 24;
                        u32 dl = (dbpp / 8) * 240 * stride[scr];
                        if(m->hwil) dl /= 2;
                        m->dlen = dl;
                        capbuf[nxt][(dl / 4) - 1] = CAP_SENTINEL;
                        // CRITICAL: clean the WHOLE buffer to RAM before the
                        // DMA — not just the sentinel line. Any dirty
                        // CPU-written line (the sentinel, and especially the
                        // in-place decimation pass from when this buffer was
                        // last processed) writes back at some arbitrary later
                        // moment and clobbers the fresh capture: sentinel-
                        // colored (yellow) corruption, phantom crc changes,
                        // fake torn drops.
                        svcFlushProcessDataCache(0xFFFF8001, (u8*)capbuf[nxt],
                                                 stride[scr] * 240 * 3);
                    }

                    int ret_dma = svcStartInterProcessDma(&dmahand, 0xFFFF8001, capbuf[nxt], srcprochand, srcaddr, siz2, dma_config[scr]);
                    if(ret_dma < 0)
                    {
                        procid = 0;
                        format[scr] = 0xF00FCACE; //invalidate
                        // Force the next housekeeping pass to re-run process
                        // discovery even without a new framebuffer change:
                        // failing here usually means procid was stale/absent
                        // for app-memory capture, and discovery otherwise
                        // only retriggers on fb-change events.
                        memset(&oldcapin, 0, sizeof(oldcapin));
                        last_route = 0;
                        if(soc && !dmafail_reported)
                        {
                            dmafail_reported = true;
                            soc->errformat((char*)"capture: dma start failed, ret=%08X src=%08X siz=%u",
                                           (u32)ret_dma, (u32)srcaddr, (unsigned int)siz2);
                        }
                    }
                    else
                    {
                        dmafail_reported = false;
                        m->valid = 1;
                        m->t_start = svcGetSystemTick();
                        cap_pending = nxt;
                    }

                    if(prochand)
                    {
                        svcCloseHandle(prochand);
                        prochand = 0;
                    }
                }

                // Advance the scheduler. Decimation pairs field B of the
                // SAME strip right after field A; everything else moves to
                // the next strip / screen.
                bool willDecimate = m->hwil
                    || (isold && (cfgblk[CFG_INTERLACE] || cfgblk[CFG_DOWNSCALE]) && getFormatBpp(format[scr]) == 16);
                if(willDecimate && m->phase == 0)
                {
                    interlacedRowSwitch = true; // field B of this strip next
                }
                else
                {
                    interlacedRowSwitch = false;
                    offs[scr]++;
                    if(offs[scr] >= limit[scr])
                    {
                        offs[scr] = 0;
                        sweep_pending[scr] = 1; // a new sweep begins
                    }
                    if(cfgblk[CFG_SCREEN] == 1) // Top Screen Only
                        scr = 0;
                    else if(cfgblk[CFG_SCREEN] == 2) // Bottom Screen Only
                        scr = 1;
                    else if(cfgblk[CFG_SCREEN] == 3) // Both Screens
                        scr = !scr;
                }
            }

            if(!have_ready)
            {
                // Nothing to process (startup, torn, failed start): let the
                // in-flight transfer make progress before looping.
                svcSleepThread(1e6);
            }
            else
            {
            // ---- 3) Process the ready buffer, using ITS metadata only.
            // The captured snapshot shadows the scheduler variables so the
            // processing code below keeps its original shape.
            int scr = cmeta[ready].scr;
            u8 cap_strip = cmeta[ready].strip;
            bool interlacedRowSwitch = cmeta[ready].phase != 0;
            bool hwInterlaced = cmeta[ready].hwil != 0;
            u32 strip_x = cmeta[ready].strip_x;
            u32 format[2];
            format[0] = cmeta[ready].fmt;
            format[1] = cmeta[ready].fmt;
            screenbuf = capbuf[ready];

            int imgsize = 0;

            // Invalidate the WHOLE buffer before the CPU reads it (crc,
            // decimation, encode, raw-rect copy): the DMA wrote RAM and any
            // stale cache lines read back old pixels — on Old 3DS this was
            // never done at all (legacy only flushed on New 3DS) and caused
            // phantom cell changes. Flush = clean+invalidate; lines are
            // clean here so this is effectively an invalidate.
            svcFlushProcessDataCache(0xFFFF8001, (u8*)screenbuf,
                                     stride[scr] * 240 * 3);

            bsiz = capin.screencapture[scr].framebuf_widthbytesize / 240; // bytes per pixel (dumb)
            scrw = 240; // sure
            bits = 4 << bsiz; // bpp (dumb)

            // interlaced (hardware): the captured data is one field
            if(hwInterlaced)
            {
                scrw = scrw / 2;
            }

            bool dma_torn = false; // torn buffers never reach this point

            // Flurry extension: skip unchanged strips (PROTOCOL.md A.1).
            // screenbuf holds the strip just captured for screen `scr`,
            // strip `offs[scr]`. crc32 it (cheap; ~100Âµs) and skip the
            // expensive encode + send when nothing changed, unless the
            // refresh interval forces a resend.
            bool skipsend = dma_torn; // torn strips are dropped, not encoded

            // ---- Stage 3: dirty-cell grid (v2 + skip, 16bpp, full-res). ----
            // Replaces the strip-level crc below when active. Runs on the
            // pre-decimation capture, so static content skips regardless of
            // the interlace/quarter sample phase. Grid geometry is client
            // set per SCREEN: cfgblk[CFG_GRID_COLS] columns (4/8/16), cfgblk[CFG_GRID_ROWS] rows
            // (1/2/4/8; rows multiply crc call count, columns are free).
            u32 gcols = cfgblk[CFG_GRID_COLS];
            if(gcols != 4 && gcols != 8 && gcols != 16) gcols = 16;
            u32 grows = cfgblk[CFG_GRID_ROWS];
            if(grows != 1 && grows != 2 && grows != 4 && grows != 8 && grows != 16) grows = 1;
            u32 cw = ((scr == 0) ? 400 : 320) / gcols;
            u32 ch = 240 / grows;
            // Bytes per pixel in the CAPTURED buffer: the DMA squeezes
            // 32bpp sources to 24, so post-capture it's 2 or 3. Home Menu
            // runs 24bpp — the previous 16bpp-only guard silently disabled
            // the whole cell system there (full-column updates only).
            u32 capbpc = getFormatBpp(format[scr]);
            capbpc = (capbpc == 32) ? 3 : capbpc / 8;
            bool v2cells = cfgblk[CFG_V2] && cfgblk[CFG_SKIP] && !dma_torn && !hwInterlaced
                && (capbpc == 2 || capbpc == 3)
                && cw > 0 && (stride[scr] % cw == 0) && (stride[scr] / cw) <= 20;
            bool v2raw_built = false;
            bool raw_batched = false; // raw rect appended to the pending batch
            if(v2cells)
            {
                u32 cellcols = stride[scr] / cw;
                u32 cellsegs = 240 / ch;
                int minr = 127, maxr = -1, mins = 127, maxs = -1;
                st_t0 = svcGetSystemTick();
                for(u32 cr = 0; cr < cellcols; cr++)
                {
                    for(u32 cs = 0; cs < cellsegs; cs++)
                    {
                        u32 c = 0;
                        for(u32 rr = 0; rr < cw; rr++)
                        {
                            const u8* p = (const u8*)screenbuf
                                + (((cr * cw) + rr) * 240 + cs * ch) * capbpc;
                            c = crc32(c, (const Bytef*)p, ch * capbpc);
                        }
                        u32* slot = &cell_crc[scr][cap_strip & 7][cr][cs];
                        if(*slot != c)
                        {
                            // cellwatch: byte-level truth for the phantom
                            // top-right cell (version text). Reports which
                            // capture buffer, the new crc, and the last
                            // data word + first pad word — distinguishes
                            // sentinel leakage / per-buffer divergence /
                            // genuine content changes in one run.
                            if(soc && scr == 0 && cap_strip == limit[0] - 1
                               && cr == cellcols - 1 && cs == cellsegs - 1)
                            {
                                u64 cw_now = osGetTime();
                                if(cw_now - cellwatch_ms >= 500)
                                {
                                    cellwatch_ms = cw_now;
                                    u32* w = (u32*)screenbuf;
                                    u32 last_i = cmeta[ready].dlen / 4 - 1;
                                    soc->errformat((char*)"cellwatch: buf=%d old=%08X new=%08X last=%08X pad=%08X",
                                                   ready, (unsigned int)*slot, (unsigned int)c,
                                                   (unsigned int)w[last_i],
                                                   (unsigned int)w[last_i + 1]);
                                }
                            }
                            *slot = c;
                            if((int)cr < minr) minr = (int)cr;
                            if((int)cr > maxr) maxr = (int)cr;
                            if((int)cs < mins) mins = (int)cs;
                            if((int)cs > maxs) maxs = (int)cs;
                        }
                    }
                }
                st_crc += svcGetSystemTick() - st_t0;

                // The strip-level refresh interval still bounds staleness.
                u8* age = &strip_age[0][scr][cap_strip & 7];
                if(maxr < 0)
                {
                    if(cfgblk[CFG_REFRESH] && *age >= cfgblk[CFG_REFRESH])
                    {
                        minr = 0; maxr = (int)cellcols - 1;
                        mins = 0; maxs = (int)cellsegs - 1;
                        *age = 0;
                    }
                    else
                    {
                        if(*age < 0xFF) (*age)++;
                        skipsend = true;
                    }
                }
                else
                {
                    *age = 0;
                }

                // Small dirty rect: ship raw pixels (no encode, full res) —
                // RGB565 (codec 0) or BGR8 (codec 3) to match the capture.
                // Large: fall through to the normal full-strip JPEG.
                if(maxr >= 0)
                {
                    u32 w_cols = (u32)(maxr - minr + 1) * cw;
                    u32 h_px = (u32)(maxs - mins + 1) * ch;
                    u32 rawbytes = w_cols * h_px * capbpc;
                    // Raw cap 32 KB: socket sends cost per-PACKET (~3 ms
                    // IPC, nearly size-independent) and WiFi has headroom,
                    // so a bigger exact-pixel rect is cheaper than the JPEG
                    // encode it avoids. Bounded by the o3DS 48 KB socket
                    // buffer.
                    if(rawbytes <= 32 * 1024 && (int)(rawbytes + 26) < soc->bufsize)
                    {
                        // Append this raw rect to the pending batch (one
                        // SFRAME with region_count>1, flushed on sweep wrap
                        // or screen change). Flush first if the batch is for
                        // another screen or this rect would overflow.
                        if(batch_scr >= 0 && (batch_scr != scr ||
                            (int)(8 + batch_payload + 14 + rawbytes) > soc->bufsize - 16))
                            flush_batch();
                        if(batch_scr < 0)
                        { batch_scr = scr; batch_payload = 4; batch_count = 0; }

                        u8* rgn = soc->bufferptr + 8 + batch_payload;
                        u8* dst = rgn + 14;
                        for(u32 col = 0; col < w_cols; col++)
                        {
                            const u8* src = (const u8*)screenbuf
                                + ((((u32)minr * cw) + col) * 240 + (u32)mins * ch) * capbpc;
                            memcpy(dst, src, h_px * capbpc);
                            dst += h_px * capbpc;
                        }
                        u16 rx = (u16)(strip_x + (u32)minr * cw);
                        u16 ry = (u16)((cellsegs - 1 - (u32)maxs) * ch);
                        rgn[0] = rx & 0xFF;      rgn[1] = rx >> 8;
                        rgn[2] = ry & 0xFF;      rgn[3] = ry >> 8;
                        rgn[4] = w_cols & 0xFF;  rgn[5] = (w_cols >> 8) & 0xFF;
                        rgn[6] = h_px & 0xFF;    rgn[7] = (h_px >> 8) & 0xFF;
                        rgn[8] = (capbpc == 2) ? 0 : 3; // codec raw565 / BGR8
                        rgn[9] = 0;
                        rgn[10] = rawbytes & 0xFF;
                        rgn[11] = (rawbytes >> 8) & 0xFF;
                        rgn[12] = (rawbytes >> 16) & 0xFF;
                        rgn[13] = (rawbytes >> 24) & 0xFF;
                        batch_payload += 14 + rawbytes;
                        batch_count++;
                        v2raw_built = true;
                        raw_batched = true; // deferred — sent by flush_batch
                    }
                    else if(cfgblk[CFG_FORMAT] == 0)
                    {
                        // Large dirty area: JPEG the dirty RECT. Flush any
                        // pending raw batch first — it shares this buffer.
                        flush_batch();
                        st_t0 = svcGetSystemTick();
                        u32 ebpc = capbpc;
                        if(ebpc == 2)
                        {
                            // libjpeg-turbo has no 565 input; expand the
                            // strip in place (same cost the old full-strip
                            // path paid).
                            convert16to24_rgb565(stride[scr], 240, (u8*)screenbuf);
                            ebpc = 3;
                        }
                        u8* b = soc->bufferptr;
                        u8* dest = b + 26;
                        unsigned long jsz = (unsigned long)(soc->bufsize - 32);
                        const u8* srcp = (const u8*)screenbuf
                            + (((u32)minr * cw) * 240 + (u32)mins * ch) * ebpc;
                        if(!tjCompress2(jencode, (u8*)srcp, (int)h_px, (int)(240 * ebpc),
                                        (int)w_cols, TJPF_RGB, &dest, &jsz,
                                        TJSAMP_420, cfgblk[CFG_QUALITY],
                                        TJFLAG_NOREALLOC | TJFLAG_FASTDCT))
                        {
                            u16 rx = (u16)(strip_x + (u32)minr * cw);
                            u16 ry = (u16)((cellsegs - 1 - (u32)maxs) * ch);
                            sframe_wrap(soc, scr, rx, ry, (u16)w_cols, (u16)h_px,
                                        1, 0, (u32)jsz);
                            v2raw_built = true;
                        }
                        else
                        {
                            // Encode failed after a possible in-place 565
                            // expansion: the buffer is no longer valid for
                            // the legacy path — drop this pass.
                            skipsend = true;
                        }
                        st_enc += svcGetSystemTick() - st_t0;
                    }
                }
            }

            if(!v2cells && !dma_torn && cfgblk[CFG_SKIP] && format[scr] != 0xF00FCACE && getFormatBpp(format[scr]) >= 16)
            {
                u32 crclen = (getFormatBpp(format[scr]) / 8) * 240 * stride[scr];
                if(hwInterlaced)
                    crclen /= 2;
                // Never crc past the screenbuf allocation (sized for 24bpp).
                u32 crccap = stride[scr] * 240 * 3;
                if(crclen > crccap)
                    crclen = crccap;

                st_t0 = svcGetSystemTick();
                u32 c = crc32(0L, (const Bytef*)screenbuf, crclen);
                st_crc += svcGetSystemTick() - st_t0;
                // Slot per decimation phase: interlace fields AND quarter-res
                // sample phases produce two alternating contents for the same
                // static strip; sharing one slot made them fight (crc A vs B
                // mismatch every pass = endless resends of static screens,
                // seen as bottom fps ~5 in quarter-res benchmarks).
                int phase = ((cfgblk[CFG_INTERLACE] || cfgblk[CFG_DOWNSCALE]) && interlacedRowSwitch) ? 1 : 0;
                u32* stored = &strip_crc[phase][scr][cap_strip & 0b111];
                u8* age = &strip_age[phase][scr][cap_strip & 0b111];

                if(*stored == c && (cfgblk[CFG_REFRESH] == 0 || *age < cfgblk[CFG_REFRESH]))
                {
                    if(*age < 0xFF)
                        (*age)++;
                    skipsend = true;
                }
                else
                {
                    *stored = c;
                    *age = 0;
                }
            }

            // Software decimation (Old 3DS): the o3DS kernel data-aborts on
            // the gather-stride DMA config hardware interlace needs, so keep
            // the full-strip DMA and drop pixels here instead. Interlace
            // halves the width (~half the encode cost); quarter-res halves
            // both axes (~a quarter). Quarter-res wins when both are set.
            bool softInterlaced = false;
            bool softDownscaled = false;
            if((cfgblk[CFG_INTERLACE] || cfgblk[CFG_DOWNSCALE]) && isold && !skipsend && !v2raw_built
               && getFormatBpp(format[scr]) == 16)
            {
                u16* px16 = (u16*)screenbuf;
                u32 ph = interlacedRowSwitch ? 1 : 0;
                if(cfgblk[CFG_DOWNSCALE]) // quarter-res: every other pixel of every other row
                {
                    u32 rows = stride[scr] / 2;
                    for(u32 r = 0; r < rows; r++)
                        for(u32 i = 0; i < 120; i++)
                            px16[r * 120 + i] = px16[(r * 2) * 240 + i * 2 + ph];
                    enc_rows_override = rows;
                    softDownscaled = true;
                    // phase still alternates so detail dithers back over time
                }
                else // interlace: every other pixel, all rows
                {
                    u32 halfpx = (240 / 2) * stride[scr];
                    for(u32 i = 0; i < halfpx; i++)
                        px16[i] = px16[i * 2 + ph];
                    softInterlaced = true;
                }
                scrw = 120;
            }

            bool frameInterlaced = hwInterlaced || softInterlaced;

            if(v2raw_built)
            {
                // Raw dirty-rect SFRAME already sits in the buffer.
            }
            else if(skipsend)
            {
                // Suppress the send below; the pipeline (strip increment,
                // screen switch, next DMA) still advances.
                soc->setPakSize(0);
                st_skip++;
                // Idle throttle: skips are so cheap the loop otherwise spins
                // at 400+ passes/s, hammering the kernel (svcOpenProcess and
                // a DMA against the foreground app's memory on every pass)
                // hard enough to freeze the app. ~25 checks/s per static
                // strip is plenty.
                svcSleepThread(3e6);
            }
            else
            {
            // Full-strip encode writes soc->bufferptr — flush any pending
            // raw batch first (it shares the buffer).
            flush_batch();
            st_t0 = svcGetSystemTick();
            switch(cfgblk[CFG_FORMAT])
            {
            case 0:
                makeJpegImage(&timems_formatconvert, &timems_processframe, scr, &scrw, &bsiz, &imgsize, format, frameInterlaced, interlacedRowSwitch);
                break;
            case 1:
                makeTargaImage(&timems_formatconvert, &timems_processframe, scr, &scrw, &bits, &imgsize, format, frameInterlaced, interlacedRowSwitch);
                break;
            default:
                break; // This case shouldn't occur.
            }
            st_enc += svcGetSystemTick() - st_t0;
            }

            //printf("height (scrw) = %i\n", scrw);
            //printf("screen size in bytes = %i\n", bsiz);
            //printf("interlace_px_offset = %i\n", interlace_px_offset);

            // legacy chunk index on o3DS (strip advance now lives in the
            // capture scheduler). Never touch a built SFRAME: byte 2 is its
            // sequence number.
            if(isold && !v2raw_built){
                soc->setPakSubtypeB(0b00001000 + cap_strip);
            }

            // Protocol v2 (SFRAME): rewrap the encoded JPEG strip as a
            // one-region pass. The u32 length field shares the legacy
            // offset (bytes 4..8), so wribuf() sends it unchanged; the
            // data moves from the legacy payload offset (+8) to after the
            // pass + region headers (+26). TGA stays on legacy framing.
            if(cfgblk[CFG_V2] && !skipsend && !v2raw_built && cfgblk[CFG_FORMAT] == 0 && soc->getPakSize())
            {
                u32 dlen = soc->getPakSize();
                // Move the legacy payload (at +8) to the SFRAME data offset
                // (+26), then stamp a full-strip region over it.
                memmove(soc->bufferptr + 26, soc->bufferptr + 8, dlen);
                u8 rflags = 0;
                if(frameInterlaced)
                {
                    rflags |= 0b00000010;    // INTERLACED
                    if(interlacedRowSwitch)
                        rflags |= 0b00000001; // FIELD_B (pre-toggle phase)
                }
                if(softDownscaled)
                    rflags |= 0b00000100;    // DOWNSCALED (client paints 2x2)
                sframe_wrap(soc, scr, (u16)strip_x, 0, (u16)stride[scr], 240,
                            1, rflags, dlen);
            }

            // If size is 0, don't send the packet. Raw-batched strips defer
            // to flush_batch (their bytes are the pending batch, not a
            // finished packet).
            if(!raw_batched && soc->getPakSize())
            {
#if DEBUG_VERBOSE==1
                osTickCounterUpdate(&tick_ctr_1);
#endif

                st_t0 = svcGetSystemTick();
                soc->wribuf();
                st_send += svcGetSystemTick() - st_t0;
                st_sent++;

#if DEBUG_VERBOSE==1
                osTickCounterUpdate(&tick_ctr_1);
                timems_writetosocbuf = osTickCounterRead(&tick_ctr_1);
#endif
            }

            // Sweep complete for this screen → flush its pending batch as
            // the sweep-marked frame. Single-screen streaming collapses a
            // whole sweep of raw rects into one send.
            if(batch_scr == scr && (u32)cap_strip + 1 >= limit[scr])
                flush_batch();

            enc_rows_override = 0;
            } // end of ready-buffer processing

            // 1 Hz perf stats to the client (0xFF/0x03). Sums are ms spent
            // in each stage during the last wall second (i.e. utilization);
            // the frame packet above was already sent, so the socket buffer
            // is free to reuse.
            {
                u64 st_now = osGetTime();
                if(cfgblk[CFG_STATS] && st_now - st_epoch_ms >= 1000)
                {
                    flush_batch(); // stats sprintf reuses soc->bufferptr
                    // ARM11 system tick frequency (SYSCLOCK_ARM11 in modern
                    // libctru; not defined in the legacy 1.2.1 headers).
                    const double tick2ms = 1000.0 / 268123480.0;
                    int stlen = sprintf((char*)(soc->bufferptr + bufsoc_pak_data_offset),
                        "sent=%lu skip=%lu torn=%lu\ndma=%.1f crc=%.1f enc=%.1f send=%.1f (ms/s)\nq=%u chunks=%lu mode=%c",
                        (unsigned long)st_sent, (unsigned long)st_skip, (unsigned long)st_torn,
                        st_dma * tick2ms, st_crc * tick2ms,
                        st_enc * tick2ms, st_send * tick2ms,
                        (unsigned int)cfgblk[CFG_QUALITY], (unsigned long)limit[scr],
                        cfgblk[CFG_DOWNSCALE] ? 'd' : (cfgblk[CFG_INTERLACE] ? 'i' : 'p'));
                    if(stlen > 0)
                    {
                        soc->setPakType(0xFF);
                        soc->setPakSubtype(0x03); // legacy STATS subtype
                        soc->setPakSubtypeB(0);
                        soc->setPakSize(stlen);
                        soc->wribuf();
                    }
                    st_dma = st_crc = st_enc = st_send = 0;
                    st_sent = st_skip = st_torn = 0;
                    st_epoch_ms = st_now;
                }
                else if(!cfgblk[CFG_STATS])
                {
                    // Stats disabled: keep the window fresh so counters
                    // don't accumulate stale history until re-enabled.
                    if(st_now - st_epoch_ms >= 1000)
                    {
                        st_dma = st_crc = st_enc = st_send = 0;
                        st_sent = st_skip = st_torn = 0;
                        st_epoch_ms = st_now;
                    }
                }
            }

            // Chunk-count change (Flurry extension): reallocate screenbuf at
            // this safe point (frame already sent). A DMA into screenbuf may
            // be in flight â€” stop it before freeing. One garbage strip may
            // be sent right after; the refresh interval heals it.
            if(chunks_realloc_needed && isold)
            {
                chunks_realloc_needed = 0;
                flush_batch(); // geometry changes; send accumulated regions
                tryStopDma(&dmahand);
                cap_pending = -1;
                cmeta[0].valid = 0;
                cmeta[1].valid = 0;
                if(capbuf[0]) { free(capbuf[0]); capbuf[0] = nullptr; }
                if(capbuf[1]) { free(capbuf[1]); capbuf[1] = nullptr; }
                screenbuf = nullptr;
                if(allocateScreenbufMem(&screenbuf) < 0)
                {
                    // Not enough heap for the bigger strips: fall back to
                    // the default 8 chunks.
                    cfgblk[CFG_CHUNKS] = 0;
                    allocateScreenbufMem(&screenbuf);
                }
                offs[0] = 0;
                offs[1] = 0;
                memset(strip_crc, 0, sizeof(strip_crc));
                memset(strip_age, 0, sizeof(strip_age));
                memset(cell_crc, 0, sizeof(cell_crc));
                for(int p2 = 0; p2 < 2; p2++)
                    for(int s2 = 0; s2 < 2; s2++)
                        for(int i2 = 0; i2 < 8; i2++)
                            strip_age[p2][s2][i2] = (u8)((i2 * 13 + s2 * 29 + p2 * 7) & 63);
                if(soc)
                    soc->errformat((char*)"chunks: now %u per screen", (unsigned int)limit[0]);
            }

            // Frame pacing. With an fps cap (Flurry extension, cfgblk[CFG_FPSCAP]),
            // pace iterations so full frames hit the target rate; otherwise
            // keep the legacy fixed Old-3DS sleep.
            if(cfgblk[CFG_FPSCAP])
            {
                u32 iters_per_frame = limit[scr] * ((cfgblk[CFG_SCREEN] == 3) ? 2 : 1);
                u64 gap_ms = 1000 / ((u32)cfgblk[CFG_FPSCAP] * iters_per_frame);
                u64 now = osGetTime();
                if(pace_last && now >= pace_last && (now - pace_last) < gap_ms)
                    svcSleepThread((gap_ms - (now - pace_last)) * 1000000ULL);
                pace_last = osGetTime();
            }
            else {
                // Uncapped: the tunable Old-3DS pause (setting 0x0A), but
                // never zero — a ~0.3 ms floor yield keeps the audio system
                // thread on syscore fed. Without it, active streaming pegs
                // the core and audio buffers underrun (crackle while
                // connected). n3DS gets only the floor.
                u64 ns = isold ? (u64)cfgblk[CFG_SLEEP] * 1000000ULL : 0;
                if(ns < 15e5) ns = 15e5; // ~1.5 ms floor: DSP/audio service
                svcSleepThread(ns);      // on syscore needs real airtime
            }
        }
        else
        {
            // Capture info unavailable (suspected while some homebrew apps
            // are in the foreground) â€” report once per failure streak so a
            // connected client can see why the stream went quiet.
            if(soc && !import_fail_reported)
            {
                import_fail_reported = true;
                soc->errformat((char*)"capture: ImportDisplayCaptureInfo failed, ret=%08X", (u32)impret);
            }
            yield();
        }
    }
    flush_batch(); // send any regions accumulated before the loop exited
    // Notif LED = Flashing yellow and purple
    memset(&pat.r[0], 0xFF, 16);
    memset(&pat.g[0], 0xFF, 16);
    memset(&pat.b[0], 0x00, 16);
    memset(&pat.r[16],0x7F, 16);
    memset(&pat.g[16],0x00, 16);
    memset(&pat.b[16],0x7F, 16);
    pat.ani = 0x0406;
    PatApply();

    if(soc)
    {
        delete soc;
        soc = nullptr;
    }

    if(dmahand)
    {
        svcStopDma(dmahand);
        svcCloseHandle(dmahand);
    }

    threadrunning = 0;
}


static FILE* f = nullptr;

ssize_t stdout_write(struct _reent* r, void* fd, const char* ptr, size_t len)
{
    if(!f) return 0;
    fputs("[STDOUT] ", f);
    return fwrite(ptr, 1, len, f);
}

ssize_t stderr_write(struct _reent* r, void* fd, const char* ptr, size_t len)
{
    if(!f) return 0;
    fputs("[STDERR] ", f);
    return fwrite(ptr, 1, len, f);
}

static const devoptab_t devop_stdout = { "stdout", 0, nullptr, nullptr, stdout_write, nullptr, nullptr, nullptr };
static const devoptab_t devop_stderr = { "stderr", 0, nullptr, nullptr, stderr_write, nullptr, nullptr, nullptr };

int main()
{
    hidExit(); // for some old versions of libctru

    mcuInit();
    nsInit();

    soc = nullptr;

#if DEBUG_BASIC==1
    f = fopen("Flurry.log", "a");
    if((int)f <= 0) f = nullptr;
    else
    {
        devoptab_list[STD_OUT] = &devop_stdout;
        devoptab_list[STD_ERR] = &devop_stderr;

        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    printf("Flurry is starting...\n");
#endif

    memset(&pat, 0, sizeof(pat));
    memset(&capin, 0, sizeof(capin));
    memset(cfgblk, 0, sizeof(cfgblk));

    // cfgblk sane defaults. NOTE: must come after the memset above (they
    // used to be set before it and were silently wiped; clients that send
    // settings before Init masked that).
    cfgblk[CFG_QUALITY] = 70; // JPEG quality
    cfgblk[CFG_SCREEN] = 1;  // top screen
    cfgblk[CFG_REFRESH] = 64; // strip-skip refresh interval (frames)
    cfgblk[CFG_SLEEP] = 5; // per-strip sleep ms (legacy Old-3DS pacing floor)
    cfgblk[CFG_GRID_COLS] = 16; // dirty-grid columns per screen
    cfgblk[CFG_GRID_ROWS] = 1;  // dirty-grid rows (1 = full-height columns: row cuts
                     // multiply crc calls, column cuts are free)

    u32 soc_service_buf_siz = 0;
    u32 screenbuf_siz = 0;
    u32 bufsoc_siz = 0;
    u32 netfunc_thread_stack_siz = 0;
    int netfunc_thread_priority = 0;
    int netfunc_thread_cpu = 0;

    isold = APPMEMTYPE <= 5;

    PatStay(0x0000FF); // Notif LED = Red

    // Initialize AC service, for Wifi stuff.
    acInit();

    if(isold) // is Old-3DS
    {
        soc_service_buf_siz = 0x10000;
        bufsoc_siz = 0xC000; // Consider trying 0x10000, but that may do nothing but waste memory.
        netfunc_thread_stack_siz = 0x2000;
        netfunc_thread_priority = 0x14;
        netfunc_thread_cpu = 1;

        /* interlacing is disabled on o3DS
        // If Interlaced
        if(cfgblk[CFG_INTERLACE] == 1)
        {
            limit[0] = 1;
            limit[1] = 1;
            stride[0] = 400;
            stride[1] = 320;
            screenbuf_siz = 400 * 120 * 3;
        }
        else
        {
        */
        limit[0] = 8; // Capture the screen in 8 chunks
        limit[1] = 8;
        stride[0] = 50; // Screen / Framebuffer width (divided by 8)
        stride[1] = 40;
        screenbuf_siz = 50 * 240 * 3;
        //}
    }
    else // is New-3DS (or New-2DS)
    {
        soc_service_buf_siz = 0x10000; //0x200000
        bufsoc_siz = 0x70000;
        netfunc_thread_stack_siz = 0x4000;

        limit[0] = 1;
        limit[1] = 1;
        stride[0] = 400;
        stride[1] = 320;
        screenbuf_siz = 400 * 240 * 3;

        // Original values; priority = 0x08, CPU = 3
        //
        // Setting priority around 0x10 (16) makes it stop slowing down Home Menu and games.
        // Priority:
        // Range from 0x00 to 0x3F. Lower numbers mean higher priority.
        netfunc_thread_priority = 0x10;
        //
        // Processor ID:
        // -2 = Default (Don't bother using this)
        // -1 = All CPU cores(?)
        // 0 = Appcore and 1 = Syscore on Old-3DS
        // 2 and 3 are allowed on New-3DS (for Base processes)
        netfunc_thread_cpu = 2;
    }


    // TODO: Try experimenting with a hacky way to write to this buffer even though we're not supposed to have access.
    // I know it'll break things. But I am still interested to see what'll happen, lol.
    ret = socInit((u32*)memalign(0x1000, soc_service_buf_siz), soc_service_buf_siz);
    if(ret < 0) *(u32*)0x1000F0 = ret;

    jencode = tjInitCompress();
    if(!jencode) *(u32*)0x1000F0 = 0xDEADDEAD;

    // Initialize communication with the GSP service, for GPU stuff
    gspInit();

    // screenbuf = (u32*)memalign(8, screenbuf_siz);

    // If memalign returns null or 0
    //if(!screenbuf)
    //{
    //    makerave();
    //    svcSleepThread(2e9);
    //    printf("crashed while trying to allocate memory for screenbuf\n");
    //    hangmacro();
    //}


    if((__excno = setjmp(__exc))) goto killswitch;

#ifdef _3DS
    std::set_unexpected(CPPCrashHandler);
    std::set_terminate(CPPCrashHandler);
#endif

    if( allocateScreenbufMem(&screenbuf) == -1)
    {
        hangmacro();
    }

    netreset:

    if(sock)
    {
        close(sock);
        sock = 0;
    }

    // at boot, haznet is set to 0. so skip this on the first run through
    if(haznet && errno == EINVAL)
    {
        errno = 0;
        PatStay(0x00FFFF); // Notif LED = Yellow
        while(checkwifi()) yield();
    }

    if(checkwifi())
    {
        int r;


#if DEBUG_USEUDP==1
        // UDP (May not work!)

        // For third argument, 0 is fine as there's only one form of datagram service(?)
        // But also, if IPPROTO_UDP is fine, I may stick with that.

        r = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif

#if DEBUG_USEUDP==0
        r = socket(AF_INET, SOCK_STREAM, IPPROTO_IP); // TCP (This works; don't change it.)
#endif

        if(r <= 0)
        {
#if DEBUG_BASIC==1
            printf("socket error: (%i) %s\n", errno, strerror(errno));
#endif
            hangmacro();
        }

        sock = r;

        struct sockaddr_in sao;
        sao.sin_family = AF_INET;
        sao.sin_addr.s_addr = gethostid();
        sao.sin_port = htons(port);

        if(bind(sock, (struct sockaddr*)&sao, sizeof(sao)) < 0)
        {
#if DEBUG_BASIC==1
            printf("bind error: (%i) %s\n", errno, strerror(errno));
#endif
            hangmacro();
        }

#if DEBUG_USEUDP==0// TCP-only code block
        {
            if(listen(sock, 1) < 0)
            {
#if DEBUG_BASIC==1
                printf("listen error: (%i) %s\n", errno, strerror(errno));
#endif
                hangmacro();
            }
        }
#endif
    }

    if(!isold) // is New-3DS
    {
        osSetSpeedupEnable(1);
    }

    if(haznet) PatStay(0xCCFF00); // Notif LED = 100% Green, 75% Blue
    else PatStay(0x00FFFF); // Notif LED = Yellow

    while(1)
    {
        hidScanInputDirectIO();

        if(kHeld == (KEY_SELECT | KEY_START)) break;

        if(!soc)
        {
            if(!haznet)
            {
                if(checkwifi()) goto netreset;
            }
            else if(pollsock(sock, POLLIN) == POLLIN)
            {
                // Client
                int cli = accept(sock, (struct sockaddr*)&sai, &sizeof_sai);
                if(cli < 0)
                {
#if DEBUG_BASIC==1
                    printf("Failed to accept client: (%i) %s\n", errno, strerror(errno));
#endif
                    if(errno == EINVAL) goto netreset;
                }
                else
                {
                    soc = new bufsoc(cli, bufsoc_siz);
                    k = soc->pack();

                    // Flurry extension: announce our feature set so extended
                    // clients unlock the new settings (PROTOCOL.md A.1).
                    // Pre-extension clients log/ignore unknown 0xFF packets.
                    soc->setPakType(0xFF);
                    soc->setPakSubtype(0x04);
                    soc->setPakSubtypeB(0);
                    soc->bufferptr[bufsoc_pak_data_offset + 0] = 1; // announce revision
                    soc->bufferptr[bufsoc_pak_data_offset + 1] = 0b11111111; // strip-skip | fps-cap | o3DS-interlace | chunks | strip-sleep | downscale | stats-toggle | protocol-v2
                    soc->bufferptr[bufsoc_pak_data_offset + 2] = 0b00000001; // features2: cell-size setting
                    soc->setPakSize(3);
                    soc->wribuf();

                    // Post-crash forensics: if the prior capture thread died
                    // on an app's fb (its telemetry lost with the TCP drop),
                    // report the culprit's params now.
                    if(g_lastapp_valid)
                        soc->errformat((char*)"last app before reconnect: progid=%08X%08X fmt=%08X wbs=%u",
                                       (u32)(g_lastapp_progid >> 32), (u32)g_lastapp_progid,
                                       (unsigned int)g_lastapp_fmt, (unsigned int)g_lastapp_wbs);

                    netthread = threadCreate(newThreadMainFunction, nullptr, netfunc_thread_stack_siz, netfunc_thread_priority, netfunc_thread_cpu, true);

                    if(!netthread)
                    {
                        // Notif LED = Blinking Red
                        memset(&pat, 0, sizeof(pat));
                        memset(&pat.r[0], 0xFF, 16);
                        pat.ani = 0x102;
                        PatApply();

                        svcSleepThread(2e9);
                    }

                    // Could above and below if statements be combined? lol -H
                    // No, we wait a little while to see if netthread is still
                    // not running or if it was just slow starting up. -C

                    if(netthread)
                    {
                        // After threadrunning = 1, we continue
                        while(!threadrunning) yield();
                    }
                    else
                    {
                        delete soc;
                        soc = nullptr;
                        hangmacro();
                    }
                }
            }
            else if(pollsock(sock, POLLERR) == POLLERR)
            {
#if DEBUG_BASIC==1
                printf("POLLERR (%i) %s", errno, strerror(errno));
#endif
                goto netreset;
            }
        }

        if(netthread && !threadrunning)
        {
            netthread = nullptr;
            if(haznet) PatStay(0xCCFF00); // Notif LED = 100% Green, 75% Blue
            else PatStay(0x00FFFF); // Notif LED = Yellow
        }

        // VRAM Corruption function :)
        //if((kHeld & (KEY_ZL | KEY_ZR)) == (KEY_ZL | KEY_ZR))
        //{
        //    u32* ptr = (u32*)0x1F000000;
        //    int o = 0x00600000 >> 2;
        //    while(o--) *(ptr++) = rand();
        //}

        yield();
    }

    killswitch:

    PatStay(0xFF0000); // Notif LED = Blue

    if(netthread)
    {
        threadrunning = 0;

        volatile bufsoc** vsoc = (volatile bufsoc**)&soc;
        while(*vsoc) yield(); //pls don't optimize kthx
    }

    if(soc) delete soc;
    else close(sock);

#if DEBUG_BASIC==1
    puts("Shutting down sockets...");
#endif
    SOCU_ShutdownSockets();

    socExit();

    gspExit();

    acExit();

    if(f)
    {
        fflush(f);
        fclose(f);
    }

    PatStay(0);

    nsExit();

    mcuExit();

    // APT_PrepareToCloseApplication(false);

    return 0;
}
