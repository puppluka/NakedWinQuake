/* Null screen implementation for headless builds. */
#include "quakedef.h"

void SCR_Init(void) {}
void SCR_UpdateScreen(void) {}
void SCR_UpdateWholeScreen(void) {}
void SCR_BeginLoadingPlaque(void) {}
void SCR_EndLoadingPlaque(void) {}
int SCR_ModalMessage(char *text)
{
	(void)text;
	return true;
}
void SCR_ScreenShot_f(void)
{
	Con_Printf("Screenshots are unavailable in a NULL build.\n");
}
