// vid_sdl.c -- SDL3 video driver with GPU-accelerated rendering

#include <SDL3/SDL.h>
#include "pl_mpeg_sdl3.h"
#include "quakedef.h"
#include "d_local.h"

extern int VID_SetMode(int mode_num, unsigned char *palette);
extern void M_DrawPic(int x, int y, qpic_t *pic);
extern void M_Print(int x, int y, char *str);
extern void M_PrintWhite(int x, int y, char *str);
extern void M_DrawCharacter(int x, int y, int num);
extern void M_Menu_Options_f(void);

extern viddef_t vid;
unsigned short d_8to16table[256];

#define BASEWIDTH             320
#define BASEHEIGHT            200
#define MAX_MODEDESCS         32
#define MAX_COLUMN_SIZE       6

unsigned screenWidth = 320;
unsigned screenHeight = 200;

int VGA_width, VGA_height, VGA_rowbytes, VGA_bufferrowbytes = 0;
byte* VGA_pagebase;

static int firstupdate = 1;
static int vid_highhunkmark;
static int lockcount;
static qboolean vid_initialized = false;
static SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
static SDL_Texture* texture = NULL;
static Uint32 SdlPalette[256];
static byte* frame_buffer;
static qboolean palette_changed;
static unsigned char vid_curpal[256 * 3];
static qboolean mouse_avail;
static float mouse_x, mouse_y;
static int mouse_oldbuttonstate = 0;

static qboolean relative_mode_available = false;
float accumulated_mouse_dx = 0.0f;
float accumulated_mouse_dy = 0.0f;

void (*vid_menudrawfn)(void) = NULL;
void (*vid_menukeyfn)(int key) = NULL;

void VID_MenuDraw(void);
void VID_MenuKey(int key);

byte vid_current_palette[768];	// save for mode changes

typedef struct {
    int modenum;
    char* desc;
    int iscur;
} modedesc_t;

typedef struct {
    int width;
    int height;
    const char* desc;
} sdl_mode_t;

static sdl_mode_t sdl_modes[] = {
    {  320, 200, "320x200" },
    {  640, 400, "640x400" },
    {  640, 480, "640x480" },
    {  800, 600, "800x600" },
    { 1024, 768, "1024x768"},
    { 1280, 720, "1280x720"},
    { 1280, 800, "1280x800"},
    { 1280, 960, "1280x960"},
    { 1280, 1024,"1280x1024"},
};

static modedesc_t modedescs[MAX_MODEDESCS];
static int vid_wmodes = 0;
static int vid_line = 0;
static int vid_modenum = 0;
static int vid_column_size = 0;
static int vid_testingmode = 0;
static float vid_testendtime = 0.0f;
static int vid_previousmode = 0;

cvar_t _vid_default_mode = { "_vid_default_mode", "0", true };

/*
VID_Playvideo_f
    command handler for playing mpeg videos via 'playvideo <filename>'
*/
void VID_Playvideo_f(void)
{
    char filename[1024];
    int was_mouse_available = mouse_avail;

    if (Cmd_Argc() != 2) {
        Con_Printf("Usage: playvideo <filename>\n");
        return;
    }
	Q_strncpy(filename, Cmd_Argv(1), sizeof(filename));
    if (was_mouse_available)
        IN_Shutdown();
    PLM_PlayVideo(filename, renderer, SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    if (was_mouse_available)
        IN_Init();
}

void VID_PlayStartupVideo(const char* filename)
{
    int was_mouse_available = mouse_avail;
    if (!renderer) return;

    if (was_mouse_available)
        IN_Shutdown();
    PLM_PlayStartupVideo(filename, renderer, SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    if (was_mouse_available)
        IN_Init();
}

/*
================
VID_Mode_f

Allows the user to change the resolution dynamically via the console.
Usage: "vid_mode 0"
================
*/
void VID_Mode_f(void)
{
    int new_mode;

    // If typed without a number, print the current mode
    if (Cmd_Argc() != 2)
    {
        Con_Printf("vid_mode is %d (%s)\n", vid_modenum, sdl_modes[vid_modenum].desc);
        Con_Printf("Valid modes are 0 to %d\n", VID_NumModes() - 1);
        return;
    }

    new_mode = atoi(Cmd_Argv(1));

    // Bounds checking based on the size of your sdl_modes array
    if (new_mode < 0 || new_mode >= VID_NumModes())
    {
        Con_Printf("Bad video mode. Valid modes are 0 to %d\n", VID_NumModes() - 1);
        return;
    }

    // Skip if the user selects the mode they are already in
    if (new_mode == vid_modenum)
    {
        return;
    }

    // Apply the new resolution
    if (VID_SetMode(new_mode, vid_current_palette))
    {
        // Update the archived cvar so the choice persists in config.cfg
        Cvar_SetValue("_vid_default_mode", new_mode);
        Con_Printf("Video mode set to %s\n", sdl_modes[new_mode].desc);
    }
    else
    {
        Con_Printf("Failed to set video mode %d\n", new_mode);
    }
}

int VID_NumModes(void)
{
    return (int)(sizeof(sdl_modes) / sizeof(sdl_modes[0]));
}

char* VID_GetModeDescription(int mode_num)
{
    if (mode_num < 0 || mode_num >= VID_NumModes())
        return NULL;
    return (char*)sdl_modes[mode_num].desc;
}

int VID_GetMode(void)
{
    return vid_modenum;
}

int VID_SetMode(int mode_num, unsigned char* palette)
{
    int chunk, cachesize, hunkmark;
    byte* cache;
    byte* new_zbuffer;
    byte* new_frame_buffer;
    byte* old_frame_buffer;
    SDL_Texture* new_texture;
    SDL_Texture* old_texture;
    SDL_RendererLogicalPresentation old_presentation;

    if (mode_num < 0 || mode_num >= VID_NumModes() || !renderer || !window)
        return false;

    new_frame_buffer = (byte*)calloc((size_t)sdl_modes[mode_num].width,
        (size_t)sdl_modes[mode_num].height);
    if (!new_frame_buffer)
        return false;
    new_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, sdl_modes[mode_num].width,
        sdl_modes[mode_num].height);
    if (!new_texture) {
        free(new_frame_buffer);
        Con_Printf("VID_SetMode: couldn't create texture: %s\n", SDL_GetError());
        return false;
    }
    if (!SDL_SetTextureScaleMode(new_texture, SDL_SCALEMODE_NEAREST)) {
        SDL_DestroyTexture(new_texture);
        free(new_frame_buffer);
        Con_Printf("VID_SetMode: couldn't configure texture: %s\n", SDL_GetError());
        return false;
    }

    SDL_GetRenderLogicalPresentation(renderer, NULL, NULL, &old_presentation);
    if (!SDL_SetRenderLogicalPresentation(renderer, sdl_modes[mode_num].width,
            sdl_modes[mode_num].height, SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        SDL_DestroyTexture(new_texture);
        free(new_frame_buffer);
        Con_Printf("VID_SetMode: couldn't configure presentation: %s\n", SDL_GetError());
        return false;
    }

    chunk = sdl_modes[mode_num].width * sdl_modes[mode_num].height * sizeof(*d_pzbuffer);
    cachesize = D_SurfaceCacheForRes(sdl_modes[mode_num].width,
        sdl_modes[mode_num].height);
    chunk += cachesize;
    hunkmark = Hunk_HighMark();
    /*
     * The zbuffer and surface cache live above the high-hunk mark.  Release
     * the block owned by the current mode first: without this every switch
     * leaks its memory for the rest of the session, and after two or three
     * changes higher modes start failing to allocate.
     */
    if (vid_highhunkmark) {
        /* Surfaces still point into the old cache block; clearing their
         * owner pointers before the block goes away avoids the use-after-free
         * that used to crash D_DrawSpans after a mode change. */
        D_FlushCaches();
        Hunk_FreeToHighMark(vid_highhunkmark);
        vid_highhunkmark = 0;
        hunkmark = Hunk_HighMark();
    }
    new_zbuffer = Hunk_HighAllocName(chunk, "video");
    if (!new_zbuffer) {
        int restore_mode = vid_modenum;
        Con_Printf("VID_SetMode: not enough memory for %s\n",
            sdl_modes[mode_num].desc);
        SDL_SetRenderLogicalPresentation(renderer, vid.width, vid.height, old_presentation);
        SDL_DestroyTexture(new_texture);
        free(new_frame_buffer);
        /* Rebuild the old mode's block so the engine keeps running. */
        if (mode_num != restore_mode)
            VID_SetMode(restore_mode, palette);
        return false;
    }
    cache = new_zbuffer + sdl_modes[mode_num].width * sdl_modes[mode_num].height * sizeof(*d_pzbuffer);
    D_InitCaches(cache, cachesize);

    old_frame_buffer = frame_buffer;
    old_texture = texture;
    frame_buffer = new_frame_buffer;
    texture = new_texture;
    d_pzbuffer = (short*)new_zbuffer;
    vid_highhunkmark = hunkmark;
    vid_modenum = mode_num;
    screenWidth = sdl_modes[mode_num].width;
    screenHeight = sdl_modes[mode_num].height;
    vid.width = sdl_modes[mode_num].width;
    vid.height = sdl_modes[mode_num].height;
    VGA_width = vid.conwidth = vid.width;
    VGA_height = vid.conheight = vid.height;
    vid.rowbytes = vid.conrowbytes = vid.width;
    vid.aspect = ((float)vid.height / (float)vid.width) * (320.0f / 200.0f);
    VGA_pagebase = vid.buffer = vid.conbuffer = vid.direct = frame_buffer;

    if (palette)
        VID_SetPalette(palette);
    /* Keep the window as-is: the renderer letterboxes the new logical
     * resolution into it, so changing mode sharpens the image instead of
     * suddenly shrinking the window to 320x200. */
    if (old_frame_buffer)
        free(old_frame_buffer);
    if (old_texture)
        SDL_DestroyTexture(old_texture);
    vid.recalc_refdef = 1;
    return true;
}

void VID_SetPalette(unsigned char* palette)
{
    int i;
    if (!palette)
        return;
    palette_changed = true;
    memcpy(vid_current_palette, palette, sizeof(vid_current_palette));
    if (palette != vid_curpal)
        memcpy(vid_curpal, palette, sizeof(vid_curpal));

    // Build 32-bit ARGB palette for GPU
    for (i = 0; i < 256; ++i) {
        SdlPalette[i] = ((Uint32)0xFF << 24) |
                        ((Uint32)palette[i*3] << 16) |
                        ((Uint32)palette[i*3+1] << 8) |
                        ((Uint32)palette[i*3+2]);
    }
}

void VID_ShiftPalette(unsigned char* palette)
{
    VID_SetPalette(palette);
}

void VID_Init(unsigned char* palette)
{
    int chunk;
    byte* cache;
    int cachesize;
    char caption[50];
    SDL_WindowFlags window_flags = 0;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
        Sys_Error("VID: Couldn't load SDL: %s", SDL_GetError());

    vid_modenum = 0;
    {
        int mode_arg = COM_CheckParm("-mode");
        int requested_mode = 0;
        if (mode_arg && mode_arg < com_argc - 1)
            requested_mode = atoi(com_argv[mode_arg + 1]);
        if (requested_mode < 0 || requested_mode >= VID_NumModes())
            requested_mode = 0;
        vid_modenum = requested_mode;
        screenWidth = (unsigned)sdl_modes[requested_mode].width;
        screenHeight = (unsigned)sdl_modes[requested_mode].height;
    }
    vid.width = screenWidth;
    vid.height = screenHeight;
    vid.maxwarpwidth = WARP_WIDTH;
    vid.maxwarpheight = WARP_HEIGHT;

	Cvar_RegisterVariable(&_vid_default_mode);
    Cmd_AddCommand("vid_mode", VID_Mode_f);
	Cmd_AddCommand("playvideo", VID_Playvideo_f);

	vid_menudrawfn = VID_MenuDraw;
	vid_menukeyfn = VID_MenuKey;

    /* Safe classic presentation by default; -fullscreen opts into exclusive mode. */
    if (COM_CheckParm("-fullscreen"))
        window_flags |= SDL_WINDOW_FULLSCREEN;

    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (mode && !COM_CheckParm("-mode")) {
        unsigned max_width = (unsigned)(mode->w * 3 / 4);
        unsigned max_height = (unsigned)(mode->h * 3 / 4);
        if (COM_CheckParm("-fullscreen")) {
            screenWidth = (unsigned)mode->w;
            screenHeight = (unsigned)mode->h;
        } else {
            screenWidth = max_width < 960 ? max_width : 960;
            screenHeight = max_height < 720 ? max_height : 720;
        }
    }
    if (screenWidth < 320) screenWidth = 320;
    if (screenHeight < 200) screenHeight = 200;

    window = SDL_CreateWindow( PROJECTID,
                               screenWidth, screenHeight,
                               window_flags);
    if (!window) {
        Sys_Error("VID: Couldn't create window: %s\n", SDL_GetError());
    }

    // Prefer acceleration, but retain classic software rendering on systems
    // where SDL cannot create an accelerated renderer (e.g. remote desktops).
    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        Con_Printf("VID: accelerated renderer unavailable (%s); trying software renderer\n", SDL_GetError());
        renderer = SDL_CreateRenderer(window, "software");
    }
    if (!renderer) {
        SDL_DestroyWindow(window);
        window = NULL;
        Sys_Error("VID: Couldn't create renderer: %s\n", SDL_GetError());
    }

    // VSync is optional; renderer backends without support must still work.
    if (!SDL_SetRenderVSync(renderer, 1))
        Con_Printf("VID: renderer does not support VSync: %s\n", SDL_GetError());
    
    // Retain a 4:3 Quake viewport with pixel-crisp nearest-neighbor scaling.
    if (!SDL_SetRenderLogicalPresentation(renderer, vid.width, vid.height,
                                      SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        Con_Printf("VID: logical presentation unavailable: %s\n", SDL_GetError());
    }

    // Create streaming texture - GPU will handle the blitting
    texture = SDL_CreateTexture( renderer,
                                 SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING,
                                 vid.width, vid.height);
    if (!texture) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        Sys_Error("VID: Couldn't create texture: %s\n", SDL_GetError());
    }

    // Nearest neighbor preserves the original indexed-palette pixel art.
    if (!SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST)) {
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        texture = NULL;
        renderer = NULL;
        window = NULL;
        Sys_Error("VID: couldn't set nearest scaling: %s\n", SDL_GetError());
    }

    frame_buffer = (byte*)calloc((size_t)vid.width, (size_t)vid.height);
    if (!frame_buffer) {
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        texture = NULL;
        renderer = NULL;
        window = NULL;
        Sys_Error("VID: Couldn't allocate frame_buffer\n");
    }

    VID_SetPalette(palette);

    sprintf(caption, "%s - Version %4.2f", PROJECTID, (float)VERSION);
    SDL_SetWindowTitle(window, caption);

    VGA_width = vid.conwidth = vid.width;
    VGA_height = vid.conheight = vid.height;
    vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 200.0);
    vid.numpages = 1;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int*)vid.colormap + 2048));

    VGA_pagebase = vid.buffer = vid.conbuffer = vid.direct = frame_buffer;
    vid.rowbytes = vid.conrowbytes = vid.width;

    chunk = vid.width * vid.height * sizeof(*d_pzbuffer);
    cachesize = D_SurfaceCacheForRes(vid.width, vid.height);
    chunk += cachesize;
    vid_highhunkmark = Hunk_HighMark();
    d_pzbuffer = Hunk_HighAllocName(chunk, "video");
    if (d_pzbuffer == NULL)
        Sys_Error("Not enough memory for video mode\n");

    cache = (byte*)d_pzbuffer + vid.width * vid.height * sizeof(*d_pzbuffer);
    D_InitCaches(cache, cachesize);

    SDL_HideCursor();

    vid_initialized = true;
    SDL_StartTextInput(window);

    Con_Printf("Video: SDL3 renderer initialized (%s)\n", SDL_GetRendererName(renderer));
}

void VID_Shutdown(void)
{
    if (vid_initialized)
    {
        IN_Shutdown();
        if (texture) {
            SDL_DestroyTexture(texture);
            texture = NULL;
        }
        if (renderer) {
            SDL_DestroyRenderer(renderer);
            renderer = NULL;
        }
        if (window) {
            SDL_StopTextInput(window);
            SDL_DestroyWindow(window);
            window = NULL;
        }
        if (frame_buffer) {
            free(frame_buffer);
            frame_buffer = NULL;
        }

        vid_initialized = false;
        SDL_Quit();
    }
}

void VID_Update(vrect_t* rects)
{
    void* pixels;
    int pitch;
    static Uint64 last_frame_time = 0;
    Uint64 current_time;

    if (!vid_initialized || !renderer || !texture || !frame_buffer)
        return;

    current_time = SDL_GetTicksNS();
    if (last_frame_time && current_time - last_frame_time < 1000000)
        return;
    last_frame_time = current_time;

    if (!SDL_LockTexture(texture, NULL, &pixels, &pitch)) {
        Con_Printf("VID_Update: couldn't lock frame texture: %s\n", SDL_GetError());
        return;
    }

    // Pitch-safe row-by-row copy loop. Correctly handles GPU memory alignment
	// padding for arbitrary resolutions. This ensures that the CPU frame buffer
    // is copied into the GPU texture correctly.
    for (int y = 0; y < (int)vid.height; y++) {
		Uint32* dest_row = (Uint32*)((byte*)pixels + y * pitch);
		byte* src_row = frame_buffer + y * vid.width;
        for(int x = 0; x < (int)vid.width; x++) {
            dest_row[x] = SdlPalette[src_row[x]];
		}
    }

    SDL_UnlockTexture(texture);

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);

    if (!SDL_RenderTexture(renderer, texture, NULL, NULL))
        Con_Printf("VID_Update: couldn't render frame: %s\n", SDL_GetError());
    SDL_RenderPresent(renderer);
}

/*
================
VID_MenuDraw
================
*/
void VID_MenuDraw(void)
{
    qpic_t* p;
    char* ptr;
    int         nummodes, i, j, column, row, dup;
    char        temp[100];

    // Handle 5-second test mode timer expiration
    if (vid_testingmode)
    {
        if (realtime >= vid_testendtime)
        {
            vid_testingmode = 0;
            if (!VID_SetMode(vid_previousmode, vid_current_palette))
                Con_Printf("VID_MenuDraw: couldn't restore previous video mode\n");
        }
    }

    vid_wmodes = 0;
    nummodes = VID_NumModes();

    p = Draw_CachePic("gfx/p_option.lmp"); // Uses default options header pic if vidmodes.lmp is missing
    M_DrawPic((320 - p->width) / 2, 4, p);

    for (i = 0; i < nummodes; i++)
    {
        if (vid_wmodes < MAX_MODEDESCS)
        {
            ptr = VID_GetModeDescription(i);

            if (ptr)
            {
                dup = 0;

                for (j = 0; j < vid_wmodes; j++)
                {
                    if (!strcmp(modedescs[j].desc, ptr))
                    {
                        if (modedescs[j].modenum != 0)
                        {
                            modedescs[j].modenum = i;
                            dup = 1;

                            if (i == vid_modenum)
                                modedescs[j].iscur = 1;
                        }
                        else
                        {
                            dup = 1;
                        }

                        break;
                    }
                }

                if (!dup)
                {
                    modedescs[vid_wmodes].modenum = i;
                    modedescs[vid_wmodes].desc = ptr;
                    modedescs[vid_wmodes].iscur = 0;

                    if (i == vid_modenum)
                        modedescs[vid_wmodes].iscur = 1;

                    vid_wmodes++;
                }
            }
        }
    }

    vid_column_size = (vid_wmodes + 2) / 3;
    if (!vid_wmodes)
        return;
    if (vid_line < 0 || vid_line >= vid_wmodes)
        vid_line = 0;

    column = 16;
    row = 36;

    for (i = 0; i < vid_wmodes; i++)
    {
        if (modedescs[i].iscur)
            M_PrintWhite(column, row, modedescs[i].desc);
        else
            M_Print(column, row, modedescs[i].desc);

        row += 8;

        if ((i % vid_column_size) == (vid_column_size - 1))
        {
            column += 13 * 8;
            row = 36;
        }
    }

    // Line cursor & text status
    if (vid_testingmode)
    {
        sprintf(temp, "TESTING %s", modedescs[vid_line].desc);
        M_Print(13 * 8, 36 + MAX_COLUMN_SIZE * 8 + 8 * 4, temp);
        M_Print(9 * 8, 36 + MAX_COLUMN_SIZE * 8 + 8 * 6, "Please wait 5 seconds...");
    }
    else
    {
        M_Print(9 * 8, 36 + MAX_COLUMN_SIZE * 8 + 8, "Press Enter to set mode");
        M_Print(6 * 8, 36 + MAX_COLUMN_SIZE * 8 + 8 * 3, "T to test mode for 5 seconds");

        ptr = VID_GetModeDescription(vid_modenum);
        if (ptr)
        {
            sprintf(temp, "D to make %s the default", ptr);
            M_Print(6 * 8, 36 + MAX_COLUMN_SIZE * 8 + 8 * 5, temp);
        }

        ptr = VID_GetModeDescription((int)_vid_default_mode.value);
        if (ptr)
        {
            sprintf(temp, "Current default is %s", ptr);
            M_Print(7 * 8, 36 + MAX_COLUMN_SIZE * 8 + 8 * 6, temp);
        }

        M_Print(15 * 8, 36 + MAX_COLUMN_SIZE * 8 + 8 * 8, "Esc to exit");

        row = 36 + (vid_line % vid_column_size) * 8;
        column = 8 + (vid_line / vid_column_size) * 13 * 8;

        M_DrawCharacter(column, row, 12 + ((int)(realtime * 4) & 1));
    }
}

/*
================
VID_MenuKey
================
*/
void VID_MenuKey(int key)
{
    if (vid_testingmode || vid_wmodes <= 0)
        return;

    switch (key)
    {
    case K_ESCAPE:
        S_LocalSound("misc/menu1.wav");
        M_Menu_Options_f();
        break;

    case K_UPARROW:
        S_LocalSound("misc/menu1.wav");
        vid_line--;
        if (vid_line < 0)
            vid_line = vid_wmodes - 1;
        break;

    case K_DOWNARROW:
        S_LocalSound("misc/menu1.wav");
        vid_line++;
        if (vid_line >= vid_wmodes)
            vid_line = 0;
        break;

    case K_LEFTARROW:
        S_LocalSound("misc/menu1.wav");
        vid_line -= vid_column_size;
        if (vid_line < 0)
        {
            vid_line += ((vid_wmodes + (vid_column_size - 1)) / vid_column_size) * vid_column_size;
            while (vid_line >= vid_wmodes)
                vid_line -= vid_column_size;
        }
        break;

    case K_RIGHTARROW:
        S_LocalSound("misc/menu1.wav");
        vid_line += vid_column_size;
        if (vid_line >= vid_wmodes)
        {
            vid_line -= ((vid_wmodes + (vid_column_size - 1)) / vid_column_size) * vid_column_size;
            while (vid_line < 0)
                vid_line += vid_column_size;
        }
        break;

    case K_ENTER:
        S_LocalSound("misc/menu1.wav");
        VID_SetMode(modedescs[vid_line].modenum, vid_current_palette);
        break;

    case 'T':
    case 't':
        S_LocalSound("misc/menu1.wav");
        vid_previousmode = vid_modenum;
        if (VID_SetMode(modedescs[vid_line].modenum, vid_current_palette))
        {
            vid_testingmode = 1;
            vid_testendtime = realtime + 5.0f;
        }
        break;

    case 'D':
    case 'd':
        S_LocalSound("misc/menu1.wav");
        firstupdate = 0;
        Cvar_SetValue("_vid_default_mode", vid_modenum);
        break;

    default:
        break;
    }
}

void D_BeginDirectRect(int x, int y, byte* pbitmap, int width, int height)
{
    Uint8* offset;

    if (!frame_buffer) return;

    if (x < 0) x = vid.width + x;

    if (x < 0 || x >= vid.width || y < 0 || y >= vid.height) return;
    if (x + width > vid.width) width = vid.width - x;
    if (y + height > vid.height) height = vid.height - y;
    if (width <= 0 || height <= 0) return;

    offset = frame_buffer + y * vid.width + x;

    while (height--)
    {
        memcpy(offset, pbitmap, width);
        offset += vid.width;
        pbitmap += width;
    }
}

void D_EndDirectRect(int x, int y, int width, int height)
{
    (void)x; (void)y; (void)width; (void)height;
}

void Sys_SendKeyEvents(void)
{
    SDL_Event event;
    int sym, state;
    SDL_Keymod modstate;
    static int center_x = 0, center_y = 0;
    int current_x, current_y;

    if (center_x == 0 || center_y == 0) {
        center_x = screenWidth / 2;
        center_y = screenHeight / 2;
    }

    while (SDL_PollEvent(&event))
    {
        switch (event.type) {

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            /* Game keys must be delivered with their real press/release state:
             * synthesising an immediate release here made +forward/-forward
             * (and every other held key) cancel out, so the player could not
             * move, jump or hold fire. */
            sym = event.key.key;
            state = (event.type == SDL_EVENT_KEY_DOWN);
            modstate = SDL_GetModState();
            
            switch (sym)
            {
            case SDLK_DELETE: sym = K_DEL; break;
            case SDLK_BACKSPACE: sym = K_BACKSPACE; break;
            case SDLK_F1: sym = K_F1; break;
            case SDLK_F2: sym = K_F2; break;
            case SDLK_F3: sym = K_F3; break;
            case SDLK_F4: sym = K_F4; break;
            case SDLK_F5: sym = K_F5; break;
            case SDLK_F6: sym = K_F6; break;
            case SDLK_F7: sym = K_F7; break;
            case SDLK_F8: sym = K_F8; break;
            case SDLK_F9: sym = K_F9; break;
            case SDLK_F10: sym = K_F10; break;
            case SDLK_F11: sym = K_F11; break;
            case SDLK_F12: sym = K_F12; break;
            case SDLK_PAUSE: sym = K_PAUSE; break;
            case SDLK_UP: sym = K_UPARROW; break;
            case SDLK_DOWN: sym = K_DOWNARROW; break;
            case SDLK_RIGHT: sym = K_RIGHTARROW; break;
            case SDLK_LEFT: sym = K_LEFTARROW; break;
            case SDLK_INSERT: sym = K_INS; break;
            case SDLK_HOME: sym = K_HOME; break;
            case SDLK_END: sym = K_END; break;
            case SDLK_PAGEUP: sym = K_PGUP; break;
            case SDLK_PAGEDOWN: sym = K_PGDN; break;
            case SDLK_RSHIFT:
            case SDLK_LSHIFT: sym = K_SHIFT; break;
            case SDLK_RCTRL:
            case SDLK_LCTRL: sym = K_CTRL; break;
            case SDLK_RALT:
            case SDLK_LALT: sym = K_ALT; break;
            case SDLK_KP_0:
                if (modstate & SDL_KMOD_NUM) sym = K_INS;
                else sym = SDLK_0;
                break;
            case SDLK_KP_1:
                if (modstate & SDL_KMOD_NUM) sym = K_END;
                else sym = SDLK_1;
                break;
            case SDLK_KP_2:
                if (modstate & SDL_KMOD_NUM) sym = K_DOWNARROW;
                else sym = SDLK_2;
                break;
            case SDLK_KP_3:
                if (modstate & SDL_KMOD_NUM) sym = K_PGDN;
                else sym = SDLK_3;
                break;
            case SDLK_KP_4:
                if (modstate & SDL_KMOD_NUM) sym = K_LEFTARROW;
                else sym = SDLK_4;
                break;
            case SDLK_KP_5: sym = SDLK_5; break;
            case SDLK_KP_6:
                if (modstate & SDL_KMOD_NUM) sym = K_RIGHTARROW;
                else sym = SDLK_6;
                break;
            case SDLK_KP_7:
                if (modstate & SDL_KMOD_NUM) sym = K_HOME;
                else sym = SDLK_7;
                break;
            case SDLK_KP_8:
                if (modstate & SDL_KMOD_NUM) sym = K_UPARROW;
                else sym = SDLK_8;
                break;
            case SDLK_KP_9:
                if (modstate & SDL_KMOD_NUM) sym = K_PGUP;
                else sym = SDLK_9;
                break;
            case SDLK_KP_PERIOD:
                if (modstate & SDL_KMOD_NUM) sym = K_DEL;
                else sym = SDLK_PERIOD;
                break;
            case SDLK_KP_DIVIDE: sym = SDLK_SLASH; break;
            case SDLK_KP_MULTIPLY: sym = SDLK_ASTERISK; break;
            case SDLK_KP_MINUS: sym = SDLK_MINUS; break;
            case SDLK_KP_PLUS: sym = SDLK_PLUS; break;
            case SDLK_KP_ENTER: sym = SDLK_RETURN; break;
            case SDLK_KP_EQUALS: sym = SDLK_EQUALS; break;
            }
            if (sym > 255) sym = 0;
            /* In the console and the chat line printable characters arrive
             * through SDL_EVENT_TEXT_INPUT; feeding the raw key here as well
             * would insert every character twice.  The console toggle key is
             * exempt so it can still close the console. */
            if (state && (key_dest == key_console || key_dest == key_message) &&
                sym >= 32 && sym < 127 && sym != '`' && sym != '~')
                break;
            Key_Event(sym, state);
            break;

        case SDL_EVENT_TEXT_INPUT:
            if (event.text.text && event.text.text[0] &&
                (key_dest == key_console || key_dest == key_message)) {
                const unsigned char *text = (const unsigned char *)event.text.text;
                while (*text) {
                    /* The console toggle is delivered as a key event, not as
                     * text, or the two paths would cancel each other out. */
                    if (*text >= 0x20 && *text < 0x7f && *text != '`' && *text != '~')
                        Key_CharEvent(*text);
                    ++text;
                }
            }
            break;

        case SDL_EVENT_MOUSE_MOTION:
            /* Relative mouse mode reports deltas even while the console or a
             * menu is up.  Ignoring them there stops the view from spinning
             * while you type, and keeps them from snapping into place when the
             * console closes. */
            if (key_dest != key_game)
                break;
            if (relative_mode_available) {
                accumulated_mouse_dx += event.motion.xrel;
                accumulated_mouse_dy += event.motion.yrel;
            } else {
                current_x = (int)event.motion.x;
                current_y = (int)event.motion.y;
                
                accumulated_mouse_dx += (current_x - center_x);
                accumulated_mouse_dy += (current_y - center_y);
            }
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            mouse_oldbuttonstate = 0;
            accumulated_mouse_dx = 0.0f;
            accumulated_mouse_dy = 0.0f;
            break;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            CL_Disconnect();
            Host_ShutdownServer(false);
            Sys_Quit();
            return;

        case SDL_EVENT_QUIT:
            CL_Disconnect();
            Host_ShutdownServer(false);
            Sys_Quit();
            return;
            
        default:
            break;
        }
    }
}

void IN_Init(void)
{
    if (COM_CheckParm("-nomouse")) {
        mouse_avail = 0;
        return;
    }

    if (!COM_CheckParm("-nomouse") && SDL_SetWindowRelativeMouseMode(window, true)) {
        relative_mode_available = true;
        Sys_Printf("Relative mouse mode enabled\n");
    } else {
        relative_mode_available = false;
        Sys_Printf("Warning: Could not enable relative mouse mode: %s\n", SDL_GetError());
        Sys_Printf("Falling back to mouse warp mode\n");
    }

    mouse_x = 0.0;
    mouse_y = 0.0;
    accumulated_mouse_dx = 0.0f;
    accumulated_mouse_dy = 0.0f;
    mouse_oldbuttonstate = 0;
    mouse_avail = 1;
}

void IN_Shutdown(void)
{
    if (mouse_avail)
    {
        SDL_SetWindowRelativeMouseMode(window, false);
    }
    mouse_avail = 0;
}

void IN_Commands(void)
{
    int i;
    Uint32 current_mouse_buttonstate_sdl;
    int ingame_mouse_buttonstate;

    if (!mouse_avail) return;

    if (!relative_mode_available && key_dest != key_game)
        return;

    current_mouse_buttonstate_sdl = SDL_GetMouseState(NULL, NULL);

    ingame_mouse_buttonstate = 0;
    if (current_mouse_buttonstate_sdl & SDL_BUTTON_LMASK)
        ingame_mouse_buttonstate |= (1 << 0);
    if (current_mouse_buttonstate_sdl & SDL_BUTTON_RMASK)
        ingame_mouse_buttonstate |= (1 << 1);
    if (current_mouse_buttonstate_sdl & SDL_BUTTON_MMASK)
        ingame_mouse_buttonstate |= (1 << 2);
    if (current_mouse_buttonstate_sdl & SDL_BUTTON_X1MASK)
        ingame_mouse_buttonstate |= (1 << 3);
    if (current_mouse_buttonstate_sdl & SDL_BUTTON_X2MASK)
        ingame_mouse_buttonstate |= (1 << 4);

    for (i = 0; i < 5; i++) {
        if ((ingame_mouse_buttonstate & (1 << i)) && !(mouse_oldbuttonstate & (1 << i)))
            Key_Event(K_MOUSE1 + i, true);

        if (!(ingame_mouse_buttonstate & (1 << i)) && (mouse_oldbuttonstate & (1 << i)))
            Key_Event(K_MOUSE1 + i, false);
    }
    mouse_oldbuttonstate = ingame_mouse_buttonstate;
}

void IN_Move(usercmd_t* cmd)
{
    static Uint64 last_warp_time = 0;
    
    if (!mouse_avail)
        return;

    mouse_x = accumulated_mouse_dx;
    mouse_y = accumulated_mouse_dy;

    accumulated_mouse_dx = 0.0f;
    accumulated_mouse_dy = 0.0f;

    // If not in relative mode, warp mouse periodically (not every frame!)
    if (!relative_mode_available) {
        Uint64 now = SDL_GetTicks();
        if (now - last_warp_time > 100) { // Only warp every 100ms
            int center_x = screenWidth / 2;
            int center_y = screenHeight / 2;
            SDL_WarpMouseInWindow(window, center_x, center_y);
            last_warp_time = now;
        }
    }

    mouse_x *= sensitivity.value;
    mouse_y *= sensitivity.value;

    if ((in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1))) {
        cmd->sidemove += m_side.value * mouse_x;
    }
    else {
        cl.viewangles[YAW] -= m_yaw.value * mouse_x;
        while (cl.viewangles[YAW] < 0)
            cl.viewangles[YAW] += 360;
        while (cl.viewangles[YAW] >= 360)
            cl.viewangles[YAW] -= 360;
    }

    if (in_mlook.state & 1) {
        V_StopPitchDrift();
    }

    if ((in_mlook.state & 1) && !(in_strafe.state & 1)) {
        cl.viewangles[PITCH] += m_pitch.value * mouse_y;
        if (cl.viewangles[PITCH] > 80)
            cl.viewangles[PITCH] = 80;
        if (cl.viewangles[PITCH] < -70)
            cl.viewangles[PITCH] = -70;
    }
    else {
        if ((in_strafe.state & 1) && noclip_anglehack) {
            cmd->upmove -= m_forward.value * mouse_y;
        }
        else {
            cmd->forwardmove -= m_forward.value * mouse_y;
        }
    }
}

char* Sys_ConsoleInput(void)
{
    return 0;
}

