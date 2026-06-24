#include "PsyX/util/timer.h"
#include <SDL.h>

void Util_InitHPCTimer(timerCtx_t* timer)
{
	timer->clockStart = SDL_GetPerformanceCounter();
}

double Util_GetHPCTime(timerCtx_t* timer, int reset)
{
	uint64_t curr = SDL_GetPerformanceCounter();
	uint64_t freq = SDL_GetPerformanceFrequency();

	double value = (double)(curr - timer->clockStart) / (double)freq;

	if (reset)
		timer->clockStart = curr;

	return value;
}
