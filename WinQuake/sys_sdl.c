#include <SDL3/SDL.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define stat _stat
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#else
#include <unistd.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <io.h>
#include <process.h>
#endif

#include <signal.h>
#include <stdlib.h>
#include <limits.h>
#ifndef _WIN32
#include <sys/types.h>
#endif
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "quakedef.h"

#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#define mkdir(path, mode) _mkdir(path)
#endif

extern void VID_PlayStartupVideo(const char* filename);
qboolean			isDedicated;

int noconinput = 0;

char* basedir = ".";
#ifdef _WIN32
char* cachedir = NULL;
#else
char* cachedir = "/tmp";
#endif

cvar_t  sys_linerefresh = { "sys_linerefresh","0" };// set for entity display
cvar_t  sys_nostdout = { "sys_nostdout","0" };

// =======================================================================
// General routines
// =======================================================================

void Sys_DebugNumber(int y, int val)
{
}

void Sys_Printf(char* fmt, ...)
{
	va_list argptr;
	char text[1024];

	va_start(argptr, fmt);
	vsnprintf(text, sizeof(text), fmt, argptr);
	va_end(argptr);
	fputs(text, stderr);
	fflush(stderr);
}

static void Sys_ShowFatalError(const char *message)
{
#ifdef _WIN32
	MessageBoxA(NULL, message, "Quake startup error", MB_OK | MB_ICONERROR | MB_TASKMODAL);
#else
	fprintf(stderr, "%s\n", message);
#endif
}

void Sys_Quit(void)
{
	Host_Shutdown();
	// Ensure all SDL subsystems are shut down before exiting.
	SDL_Quit(); // Add this line
	exit(0);
}

void Sys_Init(void)
{
#if id386
	Sys_SetFPCW();
#endif
}

#if !id386

/*
================
Sys_LowFPPrecision
================
*/
void Sys_LowFPPrecision(void)
{
	// causes weird problems on Nextstep
}


/*
================
Sys_HighFPPrecision
================
*/
void Sys_HighFPPrecision(void)
{
	// causes weird problems on Nextstep
}

#endif	// !id386


void Sys_Error(char* error, ...)
{
	va_list argptr;
	char string[1024];

	va_start(argptr, error);
	vsnprintf(string, sizeof(string), error, argptr);
	va_end(argptr);
	fprintf(stderr, "Error: %s\n", string);
	fflush(stderr);
	Sys_ShowFatalError(string);

	Host_Shutdown();
	exit(1);
}

void Sys_Warn(char* warning, ...)
{
	va_list     argptr;
	char        string[1024];

	va_start(argptr, warning);
	vsprintf(string, warning, argptr);
	va_end(argptr);
	fprintf(stderr, "Warning: %s", string);
}

/*
===============================================================================

FILE IO

===============================================================================
*/

#define	MAX_HANDLES		64
FILE* sys_handles[MAX_HANDLES];

int		findhandle(void)
{
	int		i;

	for (i = 1; i < MAX_HANDLES; i++)
		if (!sys_handles[i])
			return i;
	Sys_Error("out of handles");
	return -1;
}

/*
================
Qfilelength
================
*/
static int Qfilelength(FILE* f)
{
	int		pos;
	int		end;

	pos = ftell(f);
	fseek(f, 0, SEEK_END);
	end = ftell(f);
	fseek(f, pos, SEEK_SET);

	return end;
}

int Sys_FileOpenRead(char* path, int* hndl)
{
	FILE* f;
	int		i;

	f = fopen(path, "rb");
	if (!f) {
		*hndl = -1;
		return -1;
	}
	i = findhandle();
	sys_handles[i] = f;
	*hndl = i;

	return Qfilelength(f);
}

int Sys_FileOpenWrite(char* path)
{
	FILE* f;
	int		i;

	f = fopen(path, "wb");
	if (!f)
		Sys_Error("Error opening %s: %s", path, strerror(errno));
	i = findhandle();
	sys_handles[i] = f;

	return i;
}

void Sys_FileClose(int handle)
{
	if (handle > 0 && handle < MAX_HANDLES && sys_handles[handle]) {
		fclose(sys_handles[handle]);
		sys_handles[handle] = NULL;
	}
}

void Sys_FileSeek(int handle, int position)
{
	if (handle > 0 && handle < MAX_HANDLES && sys_handles[handle]) {
		fseek(sys_handles[handle], position, SEEK_SET);
	}
}

int Sys_FileRead(int handle, void* dst, int count)
{
	char* data;
	int size, done;

	size = 0;
	if (handle > 0 && handle < MAX_HANDLES && sys_handles[handle]) {
		data = dst;
		while (count > 0) {
			done = fread(data, 1, count, sys_handles[handle]);
			if (done == 0) {
				break;
			}
			data += done;
			count -= done;
			size += done;
		}
	}		return size;

}

int Sys_FileWrite(int handle, void* src, int count)
{
	char* data;
	int size, done;

	size = 0;
	if (handle > 0 && handle < MAX_HANDLES && sys_handles[handle]) {
		data = src;
		while (count > 0) {
			done = (int)fwrite(data, 1, count, sys_handles[handle]);
			if (done == 0) {
				break;
			}
			data += done;
			count -= done;
			size += done;
		}
	}
	return size;
}

int	Sys_FileTime(char* path)
{
	FILE* f;

	f = fopen(path, "rb");
	if (f)
	{
		fclose(f);
		return 1;
	}

#ifdef _WIN32
	{
		struct _stat info;
		if (_stat(path, &info) == 0)
			return 1;
	}
#endif
	return -1;
}

void Sys_mkdir(char* path)
{
#if defined(_WIN32) || defined(__WIN32__)
	_mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

void Sys_DebugLog(char* file, char* fmt, ...)
{
	va_list argptr;
	static char data[1024];
	FILE* fp;

	va_start(argptr, fmt);
	vsprintf(data, fmt, argptr);
	va_end(argptr);
	fp = fopen(file, "a");
	fwrite(data, strlen(data), 1, fp);
	fclose(fp);
}

double Sys_FloatTime(void)
{
	static Uint64 start_ticks = 0;
	if (start_ticks == 0) {
		start_ticks = SDL_GetTicksNS();
	}
	Uint64 current_ticks = SDL_GetTicksNS();
	return (double)(current_ticks - start_ticks) / 1000000000.0;
}

// =======================================================================
// Sleeps for microseconds
// =======================================================================

static volatile int oktogo;

void alarm_handler(int x)
{
	oktogo = 1;
}

byte* Sys_ZoneBase(int* size)
{

	char* QUAKEOPT = getenv("QUAKEOPT");

	*size = 0xc00000;
	if (QUAKEOPT)
	{
		while (*QUAKEOPT)
			if (tolower(*QUAKEOPT++) == 'm')
			{
				*size = atof(QUAKEOPT) * 1024 * 1024;
				break;
			}
	}
	return malloc(*size);

}

void Sys_LineRefresh(void)
{
}

void Sys_Sleep(void)
{
	SDL_Delay(1);
}

void floating_point_exception_handler(int whatever)
{
	//	Sys_Warn("floating point exception\n");
	signal(SIGFPE, floating_point_exception_handler);
}

void moncontrol(int x)
{
}

int main(int c, char** v)
{

	double		time, oldtime, newtime;
	quakeparms_t parms;
	extern int vcrFile;
	extern int recording;
	static int frame;

	moncontrol(0);

	signal(SIGFPE, SIG_IGN);

	parms.memsize = 16 * 1024 * 1024;
	parms.membase = malloc(parms.memsize);
	if (!parms.membase) {
		Sys_Error("Could not allocate %d bytes for the Quake hunk", parms.memsize);
	}
	parms.basedir = basedir;
	// Disable cache, else it looks in the cache for config.cfg.
	parms.cachedir = NULL;

	COM_InitArgv(c, v);
	parms.argc = com_argc;
	parms.argv = com_argv;

	Sys_Printf("NakedWinQuake startup\n");
	Sys_Printf("Working directory: %s\n", basedir);
	Sys_Init();

	Host_Init(&parms);

	Cvar_RegisterVariable(&sys_nostdout);

	if (!COM_CheckParm("-nointro"))
		VID_PlayStartupVideo("intro.mpg");

	oldtime = Sys_FloatTime() - 0.1;
	while (1)
	{
		// find time spent rendering last frame
		newtime = Sys_FloatTime();
		time = newtime - oldtime;

		if (cls.state == ca_dedicated)
		{   // play vcrfiles at max speed
			if (time < sys_ticrate.value && (vcrFile == -1 || recording))
			{
				SDL_Delay(1);
				continue;       // not time to run a server only tic yet
			}
			time = sys_ticrate.value;
		}

			if (time > sys_ticrate.value * 2)
			oldtime = newtime;
		else
			oldtime += time;

		if (++frame > 10)
			moncontrol(1);      // profile only while we do each Quake frame
		Host_Frame(time);
		moncontrol(0);

		// graphic debugging aids
		if (sys_linerefresh.value)
			Sys_LineRefresh();
	}

}


/*
================
Sys_MakeCodeWriteable
================
*/
void Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length)
{
#ifdef _WIN32
	unsigned long addr;
	DWORD oldProtect;
	addr = startaddr;
	if (!VirtualProtect((LPVOID)addr, length, PAGE_EXECUTE_READWRITE, &oldProtect)) {
		Sys_Error("Protection change failed\n");
	}
#else
	int r;
	unsigned long addr;
	int psize = getpagesize();

	fprintf(stderr, "writable code %lx-%lx\n", startaddr, startaddr + length);
	if (r < 0)
		Sys_Error("Protection change failed\n");

#endif
}
