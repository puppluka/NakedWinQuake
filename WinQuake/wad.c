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
// wad.c

#include "quakedef.h"

int			wad_numlumps;
lumpinfo_t	*wad_lumps;
byte		*wad_base;
static byte *wad_staged_base;
static int wad_staged_numlumps;
static int wad_staged_infotableofs;

#define QPIC_HEADER_SIZE (sizeof(int) * 2)

void SwapPic (qpic_t *pic);

static void W_InstallWadFile (byte *base, int numlumps, int infotableofs)
{
	lumpinfo_t *lump_p;
	unsigned i;

	if (wad_base)
		Z_Free (wad_base);
	wad_base = base;
	wad_numlumps = numlumps;
	wad_lumps = (lumpinfo_t *)(wad_base + infotableofs);
	for (i = 0, lump_p = wad_lumps; i < wad_numlumps; i++, lump_p++)
	{
		lump_p->filepos = LittleLong (lump_p->filepos);
		lump_p->disksize = LittleLong (lump_p->disksize);
		lump_p->size = LittleLong (lump_p->size);
		W_CleanupName (lump_p->name, lump_p->name);
		if (lump_p->type == TYP_QPIC)
			SwapPic ((qpic_t *)(wad_base + lump_p->filepos));
	}
}

/*
==================
W_CleanupName

Lowercases name and pads with spaces and a terminating 0 to the length of
lumpinfo_t->name.
Used so lumpname lookups can proceed rapidly by comparing 4 chars at a time
Space padding is so names can be printed nicely in tables.
Can safely be performed in place.
==================
*/
void W_CleanupName (char *in, char *out)
{
	int		i;
	int		c;
	
	for (i=0 ; i<16 ; i++ )
	{
		c = in[i];
		if (!c)
			break;
			
		if (c >= 'A' && c <= 'Z')
			c += ('a' - 'A');
		out[i] = c;
	}
	
	for ( ; i< 16 ; i++ )
		out[i] = 0;
}



/*
====================
W_LoadWadFile
====================
*/
static qboolean W_LoadWadFileInternal (char *filename, qboolean install)
{
	lumpinfo_t		*lump_p;
	wadinfo_t		*header;
	byte			*new_base;
	unsigned		i;
	int				infotableofs;
	int				filelength;
	int				lumpcount;

	/* WAD data must survive Host_ClearMemory, which runs when a demo or
	 * server sends new level state. COM_LoadFile usehunk 0 returns zone
	 * memory, so WAD ownership is released with Z_Free. */
	new_base = COM_LoadFile (filename, 0);
	filelength = new_base ? com_filesize : -1;
	if (!new_base)
		return false;
	if (filelength < (int)sizeof(wadinfo_t))
	{
		Z_Free (new_base);
		return false;
	}

	header = (wadinfo_t *)new_base;
	if (header->identification[0] != 'W' || header->identification[1] != 'A' ||
		header->identification[2] != 'D' || header->identification[3] != '2')
	{
		Z_Free (new_base);
		return false;
	}

	lumpcount = LittleLong (header->numlumps);
	infotableofs = LittleLong (header->infotableofs);
	if (lumpcount < 0 || infotableofs < (int)sizeof(*header) ||
		infotableofs > filelength ||
		lumpcount > (filelength - infotableofs) / (int)sizeof(lumpinfo_t))
	{
		Z_Free (new_base);
		return false;
	}

	/* Validate every lump while the old WAD remains live. */
	for (i = 0, lump_p = (lumpinfo_t *)(new_base + infotableofs);
		i < (unsigned)lumpcount; i++, lump_p++)
	{
		int filepos = LittleLong (lump_p->filepos);
		int disksize = LittleLong (lump_p->disksize);
		int size = LittleLong (lump_p->size);
		if (filepos < (int)sizeof(*header) || disksize < 0 || size < 0 ||
			filepos > filelength || disksize > filelength - filepos ||
			size > disksize || lump_p->compression != 0)
		{			Z_Free (new_base);
			return false;
		}
	}

	if (!install)

	{
		if (wad_staged_base)
			Z_Free (wad_staged_base);
		wad_staged_base = new_base;
		wad_staged_numlumps = lumpcount;
		wad_staged_infotableofs = infotableofs;
		return true;
	}

	W_InstallWadFile (new_base, lumpcount, infotableofs);
	return true;
}

void W_LoadWadFile (char *filename)
{
	W_CancelValidatedWadFile ();
	if (!W_LoadWadFileInternal (filename, true))
		Sys_Error ("W_LoadWadFile: invalid or unavailable WAD2 file %s", filename);
}

qboolean W_ValidateWadFile (char *filename)
{
	W_CancelValidatedWadFile ();
	return W_LoadWadFileInternal (filename, false);
}

void W_CancelValidatedWadFile (void)
{
	if (wad_staged_base)
		Z_Free (wad_staged_base);
	wad_staged_base = NULL;
	wad_staged_numlumps = 0;
	wad_staged_infotableofs = 0;
}

void W_CommitValidatedWadFile (void)
{
	if (!wad_staged_base)
		Sys_Error ("W_CommitValidatedWadFile: no validated WAD file");
	W_InstallWadFile (wad_staged_base, wad_staged_numlumps, wad_staged_infotableofs);
	wad_staged_base = NULL;
	wad_staged_numlumps = 0;
	wad_staged_infotableofs = 0;
}


/*
=============
W_GetLumpinfo
=============
*/
lumpinfo_t	*W_GetLumpinfo (char *name)
{
	int		i;
	lumpinfo_t	*lump_p;
	char	clean[16];
	
	W_CleanupName (name, clean);
	
	for (lump_p=wad_lumps, i=0 ; i<wad_numlumps ; i++,lump_p++)
	{
		if (!strcmp(clean, lump_p->name))
			return lump_p;
	}
	
	Sys_Error ("W_GetLumpinfo: %s not found", name);
	return NULL;
}

void *W_GetLumpName (char *name)
{
	lumpinfo_t	*lump;
	
	lump = W_GetLumpinfo (name);
	if (lump->compression)
		Sys_Error ("W_GetLumpName: compressed lump %s is unsupported", name);
	if (lump->type == TYP_QPIC && lump->size < (int)QPIC_HEADER_SIZE)
		Sys_Error ("W_GetLumpName: corrupt picture lump %s", name);
	
	return (void *)(wad_base + lump->filepos);
}

void *W_GetLumpNum (int num)
{
	lumpinfo_t	*lump;
	
	if (num < 0 || num >= wad_numlumps)
		Sys_Error ("W_GetLumpNum: bad number: %i", num);
		
	lump = wad_lumps + num;
	
	return (void *)(wad_base + lump->filepos);
}

/*
=============================================================================

automatic byte swapping

=============================================================================
*/

void SwapPic (qpic_t *pic)
{
	pic->width = LittleLong(pic->width);
	pic->height = LittleLong(pic->height);	
}
