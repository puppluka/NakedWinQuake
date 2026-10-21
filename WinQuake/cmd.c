/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// cmd.c -- Quake script command processing module

#include <sys/stat.h>
#include "quakedef.h"
#include "cdaudio.h"

extern int static_registered;
byte *COM_LoadFile (char *path, int usehunk);

void Cmd_ForwardToServer (void);

#define	MAX_ALIAS_NAME	32

typedef struct cmdalias_s
{
	struct cmdalias_s	*next;
	char	name[MAX_ALIAS_NAME];
	char	*value;
} cmdalias_t;

cmdalias_t	*cmd_alias;

int trashtest;
int *trashspot;

qboolean	cmd_wait;

//=============================================================================

/*
============
Cmd_Wait_f

Causes execution of the remainder of the command buffer to be delayed until
next frame.  This allows commands like:
bind g "impulse 5 ; +attack ; wait ; -attack ; impulse 2"
============
*/
void Cmd_Wait_f (void)
{
	cmd_wait = true;
}

/*
=============================================================================

						COMMAND BUFFER

=============================================================================
*/

sizebuf_t	cmd_text;

/*
============
Cbuf_Init
============
*/
void Cbuf_Init (void)
{
	SZ_Alloc (&cmd_text, 8192);		// space for commands and script files
}


/*
============
Cbuf_AddText

Adds command text at the end of the buffer
============
*/
void Cbuf_Clear (void)
{
	SZ_Clear (&cmd_text);
}

void Cbuf_AddText (char *text)
{
	int		l;
	
	l = Q_strlen (text);

	if (cmd_text.cursize + l >= cmd_text.maxsize)
	{
		Con_Printf ("Cbuf_AddText: overflow\n");
		return;
	}

	SZ_Write (&cmd_text, text, Q_strlen (text));
}


/*
============
Cbuf_InsertText

Adds command text immediately after the current command
Adds a \n to the text
FIXME: actually change the command buffer to do less copying
============
*/
void Cbuf_InsertText (char *text)
{
	char	*temp;
	int		templen;

// copy off any commands still remaining in the exec buffer
	templen = cmd_text.cursize;
	if (templen)
	{
		temp = Z_Malloc (templen);
		Q_memcpy (temp, cmd_text.data, templen);
		SZ_Clear (&cmd_text);
	}
	else
		temp = NULL;	// shut up compiler
		
// add the entire text of the file
	Cbuf_AddText (text);
	
// add the copied off data
	if (templen)
	{
		SZ_Write (&cmd_text, temp, templen);
		Z_Free (temp);
	}
}

/*
============
Cbuf_Execute
============
*/
void Cbuf_Execute (void)
{
	int		i;
	char	*text;
	char	line[1024];
	int		quotes;
	
	while (cmd_text.cursize)
	{
// find a \n or ; line break
		text = (char *)cmd_text.data;

		quotes = 0;
		for (i=0 ; i< cmd_text.cursize ; i++)
		{
			if (text[i] == '"')
				quotes++;
			if ( !(quotes&1) &&  text[i] == ';')
				break;	// don't break if inside a quoted string
			if (text[i] == '\n')
				break;
		}
			
				
		memcpy (line, text, i);
		line[i] = 0;
		
// delete the text from the command buffer and move remaining commands down
// this is necessary because commands (exec, alias) can insert data at the
// beginning of the text buffer

		if (i == cmd_text.cursize)
			cmd_text.cursize = 0;
		else
		{
			i++;
			cmd_text.cursize -= i;
			Q_memcpy (text, text+i, cmd_text.cursize);
		}

// execute the command line
		Cmd_ExecuteString (line, src_command);
		
		if (cmd_wait)
		{	// skip out while text still remains in buffer, leaving it
			// for next frame
			cmd_wait = false;
			break;
		}
	}
}

/*
==============================================================================

						SCRIPT COMMANDS

==============================================================================
*/

/*
===============
Cmd_StuffCmds_f

Adds command line parameters as script statements
Commands lead with a +, and continue until a - or another +
quake +prog jctest.qp +cmd amlev1
quake -nosound +cmd amlev1
===============
*/
static qboolean cmd_stuffcmds_executed;
static qboolean cmd_stuffcmds_game_pending;
static qboolean cmd_stuffcmds_after_game_switch;

void Cmd_StuffCmds_f (void)
{
	int		i, j;
	int		s;
	char	*text, *build, c;
		
	if (Cmd_Argc () != 1)
	{
		Con_Printf ("stuffcmds : execute command line parameters\n");
		return;
	}
	/* Game/mod quake.rc files commonly run stuffcmds. After a game switch,
	 * remove the switch arguments to avoid relaunching, but still process the
	 * remaining startup commands (for example, +map start). */
	if (cmd_stuffcmds_executed)
	{
		if (!cmd_stuffcmds_after_game_switch)
			return;
		cmd_stuffcmds_after_game_switch = false;
		for (i = 1; i < com_argc; )
		{
			int remove = 1;
			if (com_argv[i] && !Q_strcasecmp(com_argv[i], "+game"))
			{
				remove = 2;
				if (i + 1 >= com_argc || !com_argv[i + 1] ||
					com_argv[i + 1][0] == '+' || com_argv[i + 1][0] == '-')
					remove = 1;
			}
			else if (com_argv[i] && !Q_strcasecmp(com_argv[i], "+hipnotic"))
			{
				remove = 1;
			}
			else if (com_argv[i] && !Q_strcasecmp(com_argv[i], "+rogue"))
			{
				remove = 1;
			}
			else if (com_argv[i] && !Q_strcasecmp(com_argv[i], "-game"))
			{
				remove = 2;
				if (i + 1 >= com_argc || !com_argv[i + 1] ||
					com_argv[i + 1][0] == '+' || com_argv[i + 1][0] == '-')
					remove = 1;
			}
			if (remove > 1 || (com_argv[i] &&
				(!Q_strcasecmp(com_argv[i], "+game") ||
				 !Q_strcasecmp(com_argv[i], "+hipnotic") ||
				 !Q_strcasecmp(com_argv[i], "+rogue") ||
				 !Q_strcasecmp(com_argv[i], "-game"))))
			{
				memmove (&com_argv[i], &com_argv[i + remove],
					(com_argc - i - remove) * sizeof(com_argv[0]));
				com_argc -= remove;
				com_argv[com_argc] = " ";
			}
			else
				i++;
		}
	}
	else
	{
		cmd_stuffcmds_executed = true;
		cmd_stuffcmds_game_pending = COM_CheckParm ("+game") != 0;
	}

// build the combined string to parse from
	s = 0;
	for (i=1 ; i<com_argc ; i++)
	{
		if (!com_argv[i])
			continue;		// NEXTSTEP nulls out -NXHost
		s += Q_strlen (com_argv[i]) + 1;
	}
	if (!s)
		return;
		
	text = Z_Malloc (s+1);
	text[0] = 0;
	for (i=1 ; i<com_argc ; i++)
	{
		if (!com_argv[i])
			continue;		// NEXTSTEP nulls out -NXHost
		Q_strcat (text,com_argv[i]);
		if (i != com_argc-1)
			Q_strcat (text, " ");
	}
	
// pull out the commands
	build = Z_Malloc (s+1);
	build[0] = 0;
	
	for (i=0 ; i<s-1 ; i++)
	{
		if (text[i] == '+')
		{
			i++;

			for (j=i ; (text[j] != '+') && (text[j] != '-') && (text[j] != 0) ; j++)
				;

			c = text[j];
			text[j] = 0;
			
			Q_strcat (build, text+i);
			Q_strcat (build, "\n");
			text[j] = c;
			i = j-1;
		}
	}
	
	if (build[0])
		Cbuf_InsertText (build);
	
	Z_Free (text);
	Z_Free (build);
}


/*
===============
Cmd_Exec_f
===============
*/
void Cmd_Exec_f (void)
{
	char	*f;
	int		mark;

	if (Cmd_Argc () != 2)
	{
		Con_Printf ("exec <filename> : execute a script file\n");
		return;
	}

	mark = Hunk_LowMark ();
	f = (char *)COM_LoadHunkFile (Cmd_Argv(1));
	if (!f)
	{
		Con_Printf ("couldn't exec %s\n",Cmd_Argv(1));
		return;
	}
	Con_Printf ("execing %s\n",Cmd_Argv(1));
	
	Cbuf_InsertText (f);
	Hunk_FreeToLowMark (mark);
}


/*
===============
Cmd_Echo_f

Just prints the rest of the line to the console
===============
*/
void Cmd_Echo_f (void)
{
	int		i;
	
	for (i=1 ; i<Cmd_Argc() ; i++)
		Con_Printf ("%s ",Cmd_Argv(i));
	Con_Printf ("\n");
}

/*
===============
Cmd_Alias_f

Creates a new command that executes a command string (possibly ; seperated)
===============
*/

char *CopyString (char *in)
{
	char	*out;
	
	out = Z_Malloc (strlen(in)+1);
	strcpy (out, in);
	return out;
}

void Cmd_Alias_f (void)
{
	cmdalias_t	*a;
	char		cmd[1024];
	int			i, c;
	char		*s;

	if (Cmd_Argc() == 1)
	{
		Con_Printf ("Current alias commands:\n");
		for (a = cmd_alias ; a ; a=a->next)
			Con_Printf ("%s : %s\n", a->name, a->value);
		return;
	}

	s = Cmd_Argv(1);
	if (strlen(s) >= MAX_ALIAS_NAME)
	{
		Con_Printf ("Alias name is too long\n");
		return;
	}

	// if the alias allready exists, reuse it
	for (a = cmd_alias ; a ; a=a->next)
	{
		if (!strcmp(s, a->name))
		{
			Z_Free (a->value);
			break;
		}
	}

	if (!a)
	{
		a = Z_Malloc (sizeof(cmdalias_t));
		a->next = cmd_alias;
		cmd_alias = a;
	}
	strcpy (a->name, s);	

// copy the rest of the command line
	cmd[0] = 0;		// start out with a null string
	c = Cmd_Argc();
	for (i=2 ; i< c ; i++)
	{
		strcat (cmd, Cmd_Argv(i));
		if (i != c)
			strcat (cmd, " ");
	}
	strcat (cmd, "\n");
	
	a->value = CopyString (cmd);
}

static qboolean COM_IsDirectory (char *path)
{
#ifdef _WIN32
	struct _stat info;
	if (_stat (path, &info) != 0)
		return false;
	return (info.st_mode & _S_IFMT) == _S_IFDIR;
#else
	struct stat info;
	if (stat (path, &info) != 0)
		return false;
	return S_ISDIR (info.st_mode) ? true : false;
#endif
}

/* Switch the active game directory, optionally enabling a mission pack. */
static void COM_Game_f (void)
{
	qboolean startup_switch = cmd_stuffcmds_game_pending;
	int argc = Cmd_Argc();
	char *game;
	char *mission;
	char *missiondir = NULL;
	char path[MAX_OSPATH];
	char missionpath[MAX_OSPATH];
	qboolean has_gfx_wad;
	int wad_handle;
	qboolean use_base;
	qboolean want_hipnotic = false;
	qboolean want_rogue = false;
	qboolean want_standard = true;
	viddef_t *video = NULL;
	byte *palette = NULL;
	byte *colormap = NULL;
	int palette_size = 0;
	int colormap_size = 0;

	cmd_stuffcmds_game_pending = false;
	if (cmd_source != src_command)
	{
		Con_Printf ("\"game\" can only be run from the local console\n");
		return;
	}
	if (!isDedicated)
	{
		extern viddef_t vid;
		video = &vid;
	}

	if (argc == 1)
	{
		Con_Printf ("\"game\" is \"%s\"\n", COM_SkipPath(com_gamedir));
		return;
	}
	if (argc > 3)
	{
		Con_Printf ("Usage: game <gamedir> [-hipnotic|-rogue|-quoth]\n");
		return;
	}

	game = Cmd_Argv (1);
	mission = Cmd_Argv (2);
	if (!*game || !strcmp(game, ".") || !strcmp(game, "..") ||
		strchr(game, '.') || strchr(game, '/') || strchr(game, '\\') ||
		strchr(game, ':') || strchr(game, '*') || strchr(game, '?') ||
		strchr(game, '"') || strchr(game, '<') || strchr(game, '>') ||
		strchr(game, '|') || strchr(game, ' ') || strchr(game, '\t'))
	{
		Con_Printf ("gamedir should be a single directory name, not a path\n");
		return;
	}
	if (!Q_strcasecmp(game, GAMENAME))
		game = GAMENAME;
	else if (!Q_strcasecmp(game, "hipnotic"))
		game = "hipnotic";
	else if (!Q_strcasecmp(game, "quoth"))
		game = "quoth";
	else if (!Q_strcasecmp(game, "rogue"))
		game = "rogue";

	if (*mission)
	{
		if (Q_strcasecmp(mission, "-hipnotic") &&
			Q_strcasecmp(mission, "-rogue") &&
			Q_strcasecmp(mission, "-quoth"))
		{
			Con_Printf ("invalid mission pack argument to \"game\"\n");
			return;
		}
		if (!Q_strcasecmp(game, GAMENAME))
		{
			Con_Printf ("no mission pack arguments to %s game\n", GAMENAME);
			return;
		}
		want_standard = false;
		if (!Q_strcasecmp(mission, "-rogue"))
		{
			want_rogue = true;
			missiondir = "rogue";
		}
		else
		{
			want_hipnotic = true;
			missiondir = !Q_strcasecmp(mission, "-quoth") ? "quoth" : "hipnotic";
		}
	}
	else if (!Q_strcasecmp(game, "hipnotic") ||
			!Q_strcasecmp(game, "quoth"))
	{
		want_hipnotic = true;
		want_standard = false;
	}
	else if (!Q_strcasecmp(game, "rogue"))
	{
		want_rogue = true;
		want_standard = false;
	}

	/* COM_CheckRegistered already rejects mod use in the shareware build. */
	if (snprintf (path, sizeof(path), "%s/%s", com_basedir, game) >= (int)sizeof(path))
	{
		Con_Printf ("game directory path is too long\n");
		return;
	}
	if (!COM_IsDirectory (path))
	{
			Con_Printf ("No such game directory \"%s\"\n", game);
			return;
	}
	if (missiondir)
	{
		if (snprintf (missionpath, sizeof(missionpath), "%s/%s",
				com_basedir, missiondir) >= (int)sizeof(missionpath))
		{
			Con_Printf ("mission pack path is too long\n");
			return;
		}
		if (!COM_IsDirectory (missionpath))
		{
				Con_Printf ("No such mission pack directory \"%s\"\n", missiondir);
				return;
		}
	}

	use_base = !Q_strcasecmp(game, GAMENAME);
	if (!static_registered && !use_base)
	{
		Con_Printf ("You must have the registered version to use modified games\n");
		return;
	}
	if (COM_GameDirectoriesMatch (path,
		missiondir && Q_strcasecmp(game, missiondir) && !com_base_modified ? missionpath : NULL,
		use_base) && hipnotic == want_hipnotic && rogue == want_rogue &&
		standard_quake == want_standard)
	{
		Con_Printf ("\"game\" is already \"%s\"\n", game);
		return;
	}

	if (!COM_PrepareGameDirectories (path,
		missiondir && Q_strcasecmp(game, missiondir) && !com_base_modified ? missionpath : NULL,
		use_base))
		return;

	/* Probe the target while the old client is intact; cancel cleanly on failure. */
	COM_BeginPreparedGameDirectories ();
	wad_handle = -1;
	has_gfx_wad = COM_OpenFile ("gfx.wad", &wad_handle) >= 0;
	if (wad_handle != -1)
		COM_CloseFile (wad_handle);
	if (!isDedicated)
	{
		if (!has_gfx_wad)
		{
			Con_Printf ("game switch: target game has no gfx.wad\n");
			COM_EndPreparedGameDirectories ();
			COM_CancelPreparedGameDirectories ();
			return;
		}
		palette = COM_LoadFile ("gfx/palette.lmp", 0);
		palette_size = palette ? com_filesize : -1;
		colormap = COM_LoadFile ("gfx/colormap.lmp", 0);
		colormap_size = colormap ? com_filesize : -1;
		/* Keep base graphics if the mod omits palette or colormap resources. */
		if (!palette || palette_size < 768 || !colormap || colormap_size < 256 * 64 + 1)
		{
			byte *fallback_palette = NULL;
			byte *fallback_colormap = NULL;
			if (!palette || palette_size < 768)
				fallback_palette = Z_Malloc (768);
			if (!colormap || colormap_size < 256 * 64 + 1)
				fallback_colormap = Z_Malloc (256 * 64 + 1);
			if (((!palette || palette_size < 768) && !fallback_palette) ||
				((!colormap || colormap_size < 256 * 64 + 1) && !fallback_colormap))
			{
				if (palette) Z_Free (palette);
				if (colormap) Z_Free (colormap);
				if (fallback_palette) Z_Free (fallback_palette);
				if (fallback_colormap) Z_Free (fallback_colormap);
				W_CancelValidatedWadFile ();
				COM_EndPreparedGameDirectories ();
				COM_CancelPreparedGameDirectories ();
				Con_Printf ("game switch: not enough memory for graphics resources\n");
				return;
			}
			if (fallback_palette)
			{
				memcpy (fallback_palette, host_basepal, 768);
				if (palette) Z_Free (palette);
				palette = fallback_palette;
				palette_size = 768;
			}
			if (fallback_colormap)
			{
				memcpy (fallback_colormap, host_colormap, 256 * 64 + 1);
				if (colormap) Z_Free (colormap);
				colormap = fallback_colormap;
				colormap_size = 256 * 64 + 1;
			}
		}
		if (!palette || palette_size < 768 || !colormap || colormap_size < 256 * 64 + 1)
		{
			Con_Printf ("game switch: missing or truncated palette/colormap resource\n");
			W_CancelValidatedWadFile ();
			if (palette) Z_Free (palette);
			if (colormap) Z_Free (colormap);
			COM_EndPreparedGameDirectories ();
			COM_CancelPreparedGameDirectories ();
			return;
		}
		if (!W_ValidateWadFile ("gfx.wad"))
		{
			Con_Printf ("game switch: invalid gfx.wad\n");
			Z_Free (palette);
			Z_Free (colormap);
			W_CancelValidatedWadFile ();
			COM_EndPreparedGameDirectories ();
			COM_CancelPreparedGameDirectories ();
			return;
		}
	}

	/* Commit before shutdown code can non-locally exit through host_abortserver. */
	Host_WriteConfiguration ();
	if (sv.active)
		Host_ShutdownServer (true);
	CL_Disconnect ();
	CDAudio_Stop ();
	S_StopAllSounds (true);
	COM_EndPreparedGameDirectories ();
	Host_ClearMemory ();

	COM_BeginPreparedGameDirectories ();
	hipnotic = want_hipnotic;
	rogue = want_rogue;
	standard_quake = want_standard;
	Cache_Flush ();

	if (!isDedicated)
	{
		W_CommitValidatedWadFile ();
		Draw_Init ();
		SCR_Init ();
		Sbar_Init ();
		if (palette && colormap)
		{
			memcpy (host_basepal, palette, 768);
			memcpy (host_colormap, colormap, 256 * 64 + 1);
			Z_Free (palette);
			Z_Free (colormap);
			VID_SetPalette (host_basepal);
			video->colormap = host_colormap;
			video->fullbright = 256 - LittleLong (*((int *)video->colormap + 2048));
		}
	}
	COM_CommitGameDirectories ();
	if (!use_base)
		com_modified = true;
	cls.demonum = -1;
	Cbuf_Clear ();
	cmd_stuffcmds_after_game_switch = startup_switch;

	Con_Printf ("\"game\" changed to \"%s\"\n", COM_SkipPath(com_gamedir));
	if (COM_OpenFile ("quake.rc", &wad_handle) >= 0)
	{
		COM_CloseFile (wad_handle);
		Cbuf_InsertText ("exec quake.rc\n");
	}
	else
		Cbuf_InsertText ("exec default.cfg\nexec config.cfg\nexec autoexec.cfg\n");
	if (startup_switch)
		Cbuf_AddText ("stuffcmds\n");
}

/*
=============================================================================

					COMMAND EXECUTION

=============================================================================
*/

typedef struct cmd_function_s
{
	struct cmd_function_s	*next;
	char					*name;
	xcommand_t				function;
} cmd_function_t;


#define	MAX_ARGS		80

static	int			cmd_argc;
static	char		*cmd_argv[MAX_ARGS];
static	char		*cmd_null_string = "";
static	char		*cmd_args = NULL;

cmd_source_t	cmd_source;


static	cmd_function_t	*cmd_functions;		// possible commands to execute

/*
============
Cmd_Init
============
*/
void Cmd_Init (void)
{
//
// register our commands
//
	Cmd_AddCommand ("stuffcmds",Cmd_StuffCmds_f);
	Cmd_AddCommand ("exec",Cmd_Exec_f);
	Cmd_AddCommand ("echo",Cmd_Echo_f);
	Cmd_AddCommand ("alias",Cmd_Alias_f);
	Cmd_AddCommand ("cmd", Cmd_ForwardToServer);
	Cmd_AddCommand ("wait", Cmd_Wait_f);
	Cmd_AddCommand ("game", COM_Game_f);
}

/*
============
Cmd_Argc
============
*/
int		Cmd_Argc (void)
{
	return cmd_argc;
}

/*
============
Cmd_Argv
============
*/
char	*Cmd_Argv (int arg)
{
	if ( (unsigned)arg >= cmd_argc )
		return cmd_null_string;
	return cmd_argv[arg];	
}

/*
============
Cmd_Args
============
*/
char		*Cmd_Args (void)
{
	return cmd_args;
}


/*
============
Cmd_TokenizeString

Parses the given string into command line tokens.
============
*/
void Cmd_TokenizeString (char *text)
{
	int		i;
	
// clear the args from the last string
	for (i=0 ; i<cmd_argc ; i++)
		Z_Free (cmd_argv[i]);
		
	cmd_argc = 0;
	cmd_args = NULL;
	
	while (1)
	{
// skip whitespace up to a /n
		while (*text && *text <= ' ' && *text != '\n')
		{
			text++;
		}
		
		if (*text == '\n')
		{	// a newline seperates commands in the buffer
			text++;
			break;
		}

		if (!*text)
			return;
	
		if (cmd_argc == 1)
			 cmd_args = text;
			
		text = COM_Parse (text);
		if (!text)
			return;

		if (cmd_argc < MAX_ARGS)
		{
			cmd_argv[cmd_argc] = Z_Malloc (Q_strlen(com_token)+1);
			Q_strcpy (cmd_argv[cmd_argc], com_token);
			cmd_argc++;
		}
	}
	
}


/*
============
Cmd_AddCommand
============
*/
void	Cmd_AddCommand (char *cmd_name, xcommand_t function)
{
	cmd_function_t	*cmd;
	
	if (host_initialized)	// because hunk allocation would get stomped
		Sys_Error ("Cmd_AddCommand after host_initialized");
		
// fail if the command is a variable name
	if (Cvar_VariableString(cmd_name)[0])
	{
		Con_Printf ("Cmd_AddCommand: %s already defined as a var\n", cmd_name);
		return;
	}
	
// fail if the command already exists
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
	{
		if (!Q_strcmp (cmd_name, cmd->name))
		{
			Con_Printf ("Cmd_AddCommand: %s already defined\n", cmd_name);
			return;
		}
	}

	cmd = Hunk_Alloc (sizeof(cmd_function_t));
	cmd->name = cmd_name;
	cmd->function = function;
	cmd->next = cmd_functions;
	cmd_functions = cmd;
}

/*
============
Cmd_Exists
============
*/
qboolean	Cmd_Exists (char *cmd_name)
{
	cmd_function_t	*cmd;

	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
	{
		if (!Q_strcmp (cmd_name,cmd->name))
			return true;
	}

	return false;
}



/*
============
Cmd_CompleteCommand
============
*/
char *Cmd_CompleteCommand (char *partial)
{
	cmd_function_t	*cmd;
	int				len;
	
	len = Q_strlen(partial);
	
	if (!len)
		return NULL;
		
// check functions
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
		if (!Q_strncmp (partial,cmd->name, len))
			return cmd->name;

	return NULL;
}

/*
============
Cmd_ExecuteString

A complete command line has been parsed, so try to execute it
FIXME: lookupnoadd the token to speed search?
============
*/
void	Cmd_ExecuteString (char *text, cmd_source_t src)
{	
	cmd_function_t	*cmd;
	cmdalias_t		*a;

	cmd_source = src;
	Cmd_TokenizeString (text);
			
// execute the command line
	if (!Cmd_Argc())
		return;		// no tokens

// check functions
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
	{
		if (!Q_strcasecmp (cmd_argv[0],cmd->name))
		{
			cmd->function ();
			return;
		}
	}

// check alias
	for (a=cmd_alias ; a ; a=a->next)
	{
		if (!Q_strcasecmp (cmd_argv[0], a->name))
		{
			Cbuf_InsertText (a->value);
			return;
		}
	}
	
// check cvars
	if (!Cvar_Command ())
		Con_Printf ("Unknown command \"%s\"\n", Cmd_Argv(0));
	
}


/*
===================
Cmd_ForwardToServer

Sends the entire command line over to the server
===================
*/
void Cmd_ForwardToServer (void)
{
	if (cls.state != ca_connected)
	{
		Con_Printf ("Can't \"%s\", not connected\n", Cmd_Argv(0));
		return;
	}
	
	if (cls.demoplayback)
		return;		// not really connected

	MSG_WriteByte (&cls.message, clc_stringcmd);
	if (Q_strcasecmp(Cmd_Argv(0), "cmd") != 0)
	{
		SZ_Print (&cls.message, Cmd_Argv(0));
		SZ_Print (&cls.message, " ");
	}
	if (Cmd_Argc() > 1)
		SZ_Print (&cls.message, Cmd_Args());
	else
		SZ_Print (&cls.message, "\n");
}


/*
================
Cmd_CheckParm

Returns the position (1 to argc-1) in the command's argument list
where the given parameter apears, or 0 if not present
================
*/

int Cmd_CheckParm (char *parm)
{
	int i;
	
	if (!parm)
		Sys_Error ("Cmd_CheckParm: NULL");

	for (i = 1; i < Cmd_Argc (); i++)
		if (! Q_strcasecmp (parm, Cmd_Argv (i)))
			return i;
			
	return 0;
}
