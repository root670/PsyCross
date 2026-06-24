#ifndef UTIL_TIMER_H
#define UTIL_TIMER_H

#include <stdint.h>

typedef struct
{
	uint64_t clockStart;  /* SDL_GetPerformanceCounter ticks */
} timerCtx_t;

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern void Util_InitHPCTimer(timerCtx_t* timer);
extern double Util_GetHPCTime(timerCtx_t* timer, int reset /*= 0*/);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif
