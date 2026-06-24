#ifndef EMULATOR_SETUP_H
#define EMULATOR_SETUP_H

#include "platform.h"
#include <SDL.h>

/* Lightweight gap tracker — measures time between PsyCross function calls.
 * Accumulated in segments; printed every 5s via PerfGap_Print(). */
enum PerfSeg {
	PSEG_VSYNC_TO_BEGIN  = 0,  /* WaitForTimestep return → BeginScene entry  */
	PSEG_BEGIN_TO_OTAG,        /* BeginScene exit → DrawOTag entry            */
	PSEG_OTAG_INNER,           /* inside DrawOTag (parse + draw)              */
	PSEG_OTAG_TO_SYNC,         /* DrawOTag exit → DrawSync entry              */
	PSEG_SYNC_INNER,           /* inside DrawSync                             */
	PSEG_SYNC_TO_END,          /* DrawSync exit → EndScene entry              */
	PSEG_END_INNER,            /* inside EndScene                             */
	PSEG_END_TO_VSYNC,         /* EndScene exit → WaitForTimestep entry       */
	PSEG_VSYNC_INNER,          /* inside WaitForTimestep (semaphore wait)     */
	PSEG_COUNT
};

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern unsigned int g_perfGapMs[PSEG_COUNT];
extern unsigned int g_perfGapCalls[PSEG_COUNT];
extern unsigned int g_perfPrevTick;
extern unsigned int g_perfStatsTick;

/* Call at every transition point. segDone = segment just completed. */
static inline void PerfGap_Mark(int segDone)
{
	unsigned int now = SDL_GetTicks();
	g_perfGapMs[segDone]    += now - g_perfPrevTick;
	g_perfGapCalls[segDone] += 1;
	g_perfPrevTick = now;
}

void PerfGap_Print(void);   /* prints + resets every 5s; call from any hot path */

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

//Disc image filename to load for disc image builds
#define DISC_CUE_FILENAME "IMAGE.CUE"

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern unsigned int g_swapTime;
extern int g_vmode;
extern int g_activeKeyboardControllers;

extern int PsyX_Sys_GetVBlankCount();
extern int PsyX_Sys_SetVMode(int mode);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif