#include "quakedef.h"

void CDAudio_Play(byte track, qboolean looping) {}

void CDAudio_Stop(void) {}

void CDAudio_Pause(void) {}

void CDAudio_Resume(void) {}

void CDAudio_Update(void) {}

int CDAudio_Init(void) {
	Con_Printf("CDAudio disabled at compile time\n");
	return -1;
}

void CDAudio_Shutdown(void) {}