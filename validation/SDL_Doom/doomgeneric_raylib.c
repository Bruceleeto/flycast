#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"
#include "vmu_profiler.h"
#include <fastmem/fastmem.h>


#include <dc/pvr.h>
#include <dc/video.h>
#include <dc/sq.h>
#include <kos.h>
#include <arch/timer.h>
#include <assert.h>

#define DOOM_WIDTH  320
#define DOOM_HEIGHT 200

/* PVR texture size in pixels - power of 2 aligned */
#define TEX_WIDTH  1024
#define TEX_HEIGHT 512

#define KEYQUEUE_SIZE 16

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

static pvr_ptr_t pvram;
static uint32_t *pvram_sq;
static int started = 0;
static vmu_profiler_t* profiler = NULL;
static size_t frame_vertex_count = 0;
static maple_device_t* controller = NULL;
static uint64_t timer_ms = 0;
static unsigned int frames = 0;
static uint32_t last_buttons = 0;

static inline uint16_t rgba8888_to_rgb565(uint32_t rgba) {
    uint8_t b = (rgba >> 0) & 0xff;
    uint8_t g = (rgba >> 8) & 0xff;
    uint8_t r = (rgba >> 16) & 0xff;

    return ((r & 0xf8) << 8) |    // 5 bits red
           ((g & 0xfc) << 3) |    // 6 bits green
           ((b & 0xf8) >> 3);     // 5 bits blue
}

static void addKeyToQueue(int pressed, unsigned char key) {
    unsigned short keyData = (pressed << 8) | key;
    s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
    s_KeyQueueWriteIndex++;
    s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}

static inline void copy_frame_to_texture() {
    const uint32_t *src = (const uint32_t *)DG_ScreenBuffer;
    uint32_t *dest = pvram_sq;
    int y;
    uint32_t output[8];

    for (y = 0; y < DOOM_HEIGHT; y++) {
        uint32_t *line = SQ_MASK_DEST(dest);
        
        sq_lock(dest);
        
        for (int x = 0; x < DOOM_WIDTH; x += 16) {
            // Convert 16 pixels at a time using temporary buffer
            for(int i = 0; i < 8; i++) {
                uint16_t p1 = rgba8888_to_rgb565(src[i * 2]);
                uint16_t p2 = rgba8888_to_rgb565(src[i * 2 + 1]);
                output[i] = (p2 << 16) | p1;
            }
            
            // Use optimized memory copy
            memcpy_fast(line, output, sizeof(output));
            sq_flush(line);
            
            line += 8;
            src += 16;
        }
        
        dest += TEX_WIDTH / 2;
        sq_unlock();
    }
}
void DG_Init() {
    /* Initialize the PVR */
    pvr_init_defaults();
    
    /* Set video mode to 640x480 interlaced */
    vid_set_mode(DM_640x480, PM_RGB565);
    
    /* Allocate texture memory */
    pvram = pvr_mem_malloc(TEX_WIDTH * TEX_HEIGHT * 2);
    assert(pvram != NULL);
    assert(!((unsigned int)pvram & 0x1f));
    
    /* Setup SQ access to texture memory */
    pvram_sq = (uint32_t *)(((uintptr_t)pvram & 0xffffff) | PVR_TA_TEX_MEM);
    
    /* Initialize VMU profiler */
    profiler = vmu_profiler_init();
    
    /* Cache controller reference */
    controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    
    started = 1;
}

void DG_DrawFrame() {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t vert;
    
    if (!started || !DG_ScreenBuffer) return;

    /* Copy Doom's frame buffer to PVR texture */
    copy_frame_to_texture();

    /* Wait for PVR */
    pvr_wait_ready();
    
    /* Begin scene */
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);

    /* Setup polygon context */
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                     PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     TEX_WIDTH, TEX_HEIGHT, pvram, PVR_FILTER_BILINEAR);
    
    cxt.gen.alpha = PVR_ALPHA_DISABLE;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    frame_vertex_count = 0;

    /* Draw textured quad to fill 640x480 */
    vert.argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
    vert.oargb = 0;
    vert.flags = PVR_CMD_VERTEX;
    
    /* Top-left */
    vert.x = 0.0f;
    vert.y = 0.0f;
    vert.z = 1.0f;
    vert.u = 0.0f;
    vert.v = 0.0f;
    pvr_prim(&vert, sizeof(vert));
    frame_vertex_count++;
    
    /* Top-right */
    vert.x = 640.0f;
    vert.u = (float)DOOM_WIDTH / (float)TEX_WIDTH;
    pvr_prim(&vert, sizeof(vert));
    frame_vertex_count++;
    
    /* Bottom-left */
    vert.x = 0.0f;
    vert.y = 480.0f;
    vert.u = 0.0f;
    vert.v = (float)DOOM_HEIGHT / (float)TEX_HEIGHT;
    pvr_prim(&vert, sizeof(vert));
    frame_vertex_count++;
    
    /* Bottom-right */
    vert.x = 640.0f;
    vert.u = (float)DOOM_WIDTH / (float)TEX_WIDTH;
    vert.flags = PVR_CMD_VERTEX_EOL;
    pvr_prim(&vert, sizeof(vert));
    frame_vertex_count++;

    /* End scene */
    pvr_list_finish();
    pvr_scene_finish();

    /* Update profiler */
    if (profiler) {
        vmu_profiler_update(profiler, frame_vertex_count);
    }
    
    frames++;
}

void DG_SleepMs(uint32_t ms) {
    uint64_t current_time = timer_ms_gettime64();
    
    if (timer_ms == 0) {
        timer_ms = current_time;
        frames = 0;
        return;
    }

    // Update FPS counter every second
    if (current_time > (timer_ms + 1000)) {
        if (profiler) {
            vmu_printf("\n FPS: %5.1f\n", (float)frames);
        }
        timer_ms = current_time;
        frames = 0;
    }
}

uint32_t DG_GetTicksMs() {
    return timer_ms_gettime64();
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
    maple_device_t *cont;
    cont_state_t *state;
    
    // First check if we have any queued keys
    if (s_KeyQueueReadIndex != s_KeyQueueWriteIndex) {
        unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
        s_KeyQueueReadIndex++;
        s_KeyQueueReadIndex %= KEYQUEUE_SIZE;

        *pressed = keyData >> 8;
        *doomKey = keyData & 0xFF;
        return 1;
    }
    
    // If no queued keys, check controller
    cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!cont) return 0;
        
    state = (cont_state_t *)maple_dev_status(cont);
    if (!state) return 0;

   // Get changed buttons
    uint32_t new_buttons = state->buttons;
    uint32_t changed_buttons = new_buttons ^ last_buttons;
    
    // Queue button presses and releases for each changed button
    if (changed_buttons & CONT_DPAD_UP) {
        addKeyToQueue((new_buttons & CONT_DPAD_UP) ? 1 : 0, KEY_UPARROW);
    }
    if (changed_buttons & CONT_DPAD_DOWN) {
        addKeyToQueue((new_buttons & CONT_DPAD_DOWN) ? 1 : 0, KEY_DOWNARROW);
    }
    if (changed_buttons & CONT_DPAD_LEFT) {
        addKeyToQueue((new_buttons & CONT_DPAD_LEFT) ? 1 : 0, KEY_LEFTARROW);
    }
    if (changed_buttons & CONT_DPAD_RIGHT) {
        addKeyToQueue((new_buttons & CONT_DPAD_RIGHT) ? 1 : 0, KEY_RIGHTARROW);
    }
    if (changed_buttons & CONT_A) {
        addKeyToQueue((new_buttons & CONT_A) ? 1 : 0, KEY_FIRE);
    }
    if (changed_buttons & CONT_B) {
        addKeyToQueue((new_buttons & CONT_B) ? 1 : 0, KEY_USE);
    }
    if (changed_buttons & CONT_X) {
        addKeyToQueue((new_buttons & CONT_X) ? 1 : 0, KEY_RSHIFT);
    }
    if (changed_buttons & CONT_Y) {
        addKeyToQueue((new_buttons & CONT_Y) ? 1 : 0, KEY_ENTER);
    }
    if (changed_buttons & CONT_START) {
        addKeyToQueue((new_buttons & CONT_START) ? 1 : 0, KEY_ESCAPE);
    }
    last_buttons = new_buttons;

    // Now try to read from the queue we just filled
    if (s_KeyQueueReadIndex != s_KeyQueueWriteIndex) {
        unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
        s_KeyQueueReadIndex++;
        s_KeyQueueReadIndex %= KEYQUEUE_SIZE;

        *pressed = keyData >> 8;
        *doomKey = keyData & 0xFF;
        return 1;
    }

    return 0;
}

void DG_SetWindowTitle(const char* title) {
    /* No-op on Dreamcast */
}

int main(int argc, char **argv) {
    DG_Init();
    doomgeneric_Create(argc, argv);

    timer_ms = timer_ms_gettime64();

    while (1) {
        doomgeneric_Tick();
    }

    if (profiler) {
        vmu_profiler_shutdown(profiler);
    }

    return 0;
}