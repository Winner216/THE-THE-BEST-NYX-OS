#ifndef DOOM_GENERIC
#define DOOM_GENERIC

#include <stdlib.h>
#include <stdint.h>

#ifndef DOOMGENERIC_RESX
#define DOOMGENERIC_RESX 640
#endif  // DOOMGENERIC_RESX

#ifndef DOOMGENERIC_RESY
#define DOOMGENERIC_RESY 400
#endif  // DOOMGENERIC_RESY


#ifdef CMAP256

typedef uint8_t pixel_t;

#else  // CMAP256

typedef uint32_t pixel_t;

#endif  // CMAP256


extern pixel_t* DG_ScreenBuffer;

#ifdef __cplusplus
extern "C" {
#endif

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick();


//Implement below functions for your platform
void DG_Init();
void DG_DrawFrame();
void DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs();
int DG_GetKey(int* pressed, unsigned char* key);

// Optional per-tic mouse polling (Nyx port).  When implemented,
// i_input.c posts one aggregated ev_mouse per tic — DOOM's
// G_Responder assigns (not accumulates) mouse motion, so per-packet
// events would lose deltas.  Provide a no-op if the platform has no
// mouse.
void DG_GetMouse(int* mx, int* my, unsigned* buttons);
void DG_SetWindowTitle(const char * title);

#ifdef __cplusplus
}
#endif

#endif //DOOM_GENERIC
