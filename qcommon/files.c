/*
Copyright (C) 1997-2001 Id Software, Inc.

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

#include "qcommon.h"
#include "redblack.h"

/*
=============================================================================

QUAKE FILESYSTEM

=============================================================================
*/

//
// in memory
//

typedef struct
{
	char	name[MAX_QPATH];
	int		filepos, filelen;
	unsigned int hash;
} packfile_t;

typedef struct pack_s
{
	char		filename[MAX_OSPATH];
	FILE		*handle;
	int			numfiles;
	packfile_t	*files;
} pack_t;

char	fs_gamedir[MAX_OSPATH];
cvar_t	*fs_basedir;
cvar_t	*fs_gamedirvar;
cvar_t	*fs_cache;
cvar_t	*fs_noextern;

typedef struct filelink_s
{
	struct filelink_s	*next;
	char	*from;
	int		fromlength;
	char	*to;
} filelink_t;

filelink_t	*fs_links;

typedef struct searchpath_s
{
	char	filename[MAX_OSPATH];
	pack_t	*pack;		// only one of filename / pack will be used
	struct searchpath_s *next;
} searchpath_t;

searchpath_t	*fs_searchpaths;
searchpath_t	*fs_base_searchpaths;	// without gamedirs

/*

All of Quake's data access is through a hierchal file system, but the contents of
the file system can be transparently merged from several sources.

The "base directory" is the path to the directory holding the quake.exe and all game
directories.  The sys_* files pass this to host_init in quakeparms_t->basedir.  This
can be overridden with the "-basedir" command line parm to allow code debugging in a
different directory.  The base directory is only used during filesystem initialization.

The "game directory" is the first tree on the search path and directory that all
generated files (savegames, screenshots, demos, config files) will be saved to. 
This can be overridden with the "-game" command line parameter.  The game directory
can never be changed while quake is executing.  This is a precacution against having a
malicious server instruct clients to write files over areas they shouldn't.

*/


/*
================
FS_filelength
================
*/
int FS_filelength (FILE *f)
{
	int		pos;
	int		end;

	pos = ftell (f);
	fseek (f, 0, SEEK_END);
	end = ftell (f);
	fseek (f, pos, SEEK_SET);

	return end;
}


/*
============
FS_CreatePath

Creates any directories needed to store the given filename
============
*/
void	FS_CreatePath (char *path)
{
	char	*ofs;
	
	for (ofs = path+1 ; *ofs ; ofs++)
	{
		if (*ofs == '/')
		{	// create the directory
			*ofs = 0;
			Sys_Mkdir (path);
			*ofs = '/';
		}
	}
}


/*
==============
FS_FCloseFile

For some reason, other dll's can't just cal fclose()
on files returned by FS_FOpenFile...
==============
*/
void FS_FCloseFile (FILE *f)
{
	fclose (f);
}


// RAFAEL
/*
	Developer_searchpath
*/
int	Developer_searchpath (void)
{
	searchpath_t	*search;
	

	for (search = fs_searchpaths ; search ; search = search->next)
	{
		if (strstr (search->filename, "xatrix"))
			return 1;

		if (strstr (search->filename, "rogue"))
			return 2;
/*
		start = strchr (search->filename, ch);

		if (start == NULL)
			continue;

		if (strcmp (start ,"xatrix") == 0)
			return (1);
*/
	}
	return (0);

}

unsigned int hashify (char *S)
{
  unsigned int hash_PeRlHaSh = 0;
  char c;
  while (*S) {
	  c = tolower(*S++);
	  hash_PeRlHaSh = hash_PeRlHaSh * 33 + (c);
  }
  return hash_PeRlHaSh + (hash_PeRlHaSh >> 5);
}

typedef struct fscache_s fscache_t;

struct fscache_s {
	fscache_t		*next;
	unsigned int	hash;
	char			*filepath;
	char			*filename;
	unsigned int	filelen;
	unsigned int	fileseek;
};

struct rbtree *rb;

/*static int _compare(const void *pa, const void *pb)
{
	return strcmp ((const char *)pa, (c.onst char *)pb);
}*/

void FS_InitCache (void)
{
	rb = rbinit (strcmp);

	if (!rb)
		Sys_Error (ERR_FATAL, "FS_InitCache: rbinit failed"); 
}

fscache_t fscache;

void FS_FlushCache (void)
{
	RBLIST *rblist;
	const void *val;
	fscache_t *last = NULL, *temp;

	temp = &fscache;
	temp = temp->next;

	if (temp) {
		while (temp->next) {
			last = temp->next;
			Z_Free (temp);
			temp = last;
		}

		fscache.next = NULL;

		Z_Free (temp);
	}

	memset (&fscache, 0, sizeof(fscache));

	if ((rblist=rbopenlist(rb)))
	{
		while((val=rbreadlist(rblist)))
			rbdelete (val, rb);
	}
	rbcloselist(rblist);		
}

void FS_Stats_f (void)
{
	int i;
	const void *val;
	fscache_t *temp;

	i = 0;

	for(val=rblookup(RB_LUFIRST, NULL, rb); val!=NULL; val=rblookup(RB_LUNEXT, val, rb))
		i++;

	Com_Printf ("%d entries in binary search tree cache.\n", i);

	temp = &fscache;
	temp = temp->next;

	i = 0;

	if (temp)
	{
		while (temp->next)
		{
			i++;
			temp = temp->next;
		}
	}

	Com_Printf ("%d entries in linked list hash cache.\n", i);
}

#define BTREE_SEARCH 1
//#define HASH_CACHE 1

#if BTREE_SEARCH
void FS_AddToCache (char *path, unsigned int filelen, unsigned int fileseek, char *filename)
{
	void		**newitem;
	fscache_t	*cache;

	cache = Z_TagMalloc (sizeof(fscache_t), TAGMALLOC_FSCACHE);
	cache->filelen = filelen;
	cache->fileseek = fileseek;

	if (path)
		cache->filepath = CopyString (path, TAGMALLOC_FSCACHE);
	else
		cache->filepath = NULL;

	cache->filename = CopyString (filename, TAGMALLOC_FSCACHE);

	//Com_Printf ("Adding %s: ", filename);
	newitem = rbsearch (filename, rb);
	*newitem = cache;
	//Com_Printf ("\n");
}
#else
void FS_AddToCache (unsigned int hash, char *path, unsigned int filelen, unsigned int fileseek, fscache_t *cache, char *filename)
{
	cache->next = Z_TagMalloc (sizeof(fscache_t), TAGMALLOC_FSCACHE);
	cache = cache->next;
	cache->filelen = filelen;
	cache->hash = hash;
	cache->next = NULL;
	cache->fileseek = fileseek;

	if (path)
		cache->filepath = CopyString (path, TAGMALLOC_FSCACHE);
	else
		cache->filepath = NULL;

	cache->filename = CopyString (filename, TAGMALLOC_FSCACHE);
}
#endif

/*
===========
FS_FOpenFile

Finds the file in the search path.
returns filesize and an open FILE *
Used for streaming data out of either a pak file or
a seperate file.
===========
*/

int FS_FOpenFile (char *filename, FILE **file)
{
	fscache_t		*cache = &fscache;
	searchpath_t	*search;
	char			netpath[MAX_OSPATH];
	pack_t			*pak;
	int				i;
	filelink_t		*link;
//#ifndef BTREE_SEARCH
	unsigned int	hash;
//#endif

	// check for links firstal
	if (!fs_noextern->value)
	{
		for (link = fs_links ; link ; link=link->next)
		{
			if (!strncmp (filename, link->from, link->fromlength))
			{
				Com_sprintf (netpath, sizeof(netpath), "%s%s",link->to, filename+link->fromlength);
				*file = fopen (netpath, "rb");
				if (*file)
				{		
					Com_DPrintf ("link file: %s\n",netpath);
					return FS_filelength (*file);
				}
				return -1;
			}
		}
	}

#ifdef BTREE_SEARCH

	//Com_Printf ("Begin search for %s: ", filename);
	cache = rbfind (filename, rb);
	if (cache)
	{
		//Com_Printf (" (cached)\n");
		cache = *(fscache_t **)cache;
		if (cache->filepath == NULL)
		{
			*file = NULL;
			return -1;
		}
		*file = fopen (cache->filepath, "rb");
		fseek (*file, cache->fileseek, SEEK_SET);
		return cache->filelen;
	}
	//Com_Printf (" (not cached)\n");
	hash = hashify (filename);
#else
	hash = hashify (filename);

	while (cache->next)
	{ 
		cache = cache->next;
		if (cache->hash == hash && !Q_stricmp (cache->filename, filename))
		{
			if (cache->filepath == NULL)
			{
				*file = NULL;
				return -1;
			}
			*file = fopen (cache->filepath, "rb");
			fseek (*file, cache->fileseek, SEEK_SET);
			return cache->filelen;
		}
	}
#endif

	//s_len -= 6;
	for (search = fs_searchpaths ; search ; search = search->next)
	{
	// is the element a pak file?
		if (search->pack) {
			// look through all the pak file elements
			pak = search->pack;
			for (i=0 ; i<pak->numfiles ; i++) {
//#ifndef BTREE_SEARCH
				if (pak->files[i].hash == hash)
//#endif
				if (!Q_stricmp (pak->files[i].name, filename))
				{	// found it!
					Com_DPrintf ("PackFile: %s : %s\n",pak->filename, filename);
				// open a new file on the pakfile
					*file = fopen (pak->filename, "rb");
					if (!*file)
						Com_Error (ERR_FATAL, "Couldn't reopen %s", pak->filename);	
					fseek (*file, pak->files[i].filepos, SEEK_SET);
					if (fs_cache->value)
					{
#ifdef BTREE_SEARCH
						FS_AddToCache (pak->filename, pak->files[i].filelen, pak->files[i].filepos, filename);
#else
						FS_AddToCache (hash, pak->filename, pak->files[i].filelen, pak->files[i].filepos, cache, filename);
#endif
					}
					return pak->files[i].filelen;
				}
			}
		}
		else if (!fs_noextern->value)
		{
			int filelen;
			// check a file in the directory tree
			
			Com_sprintf (netpath, sizeof(netpath), "%s/%s",search->filename, filename);
			
			*file = fopen (netpath, "rb");
			if (!*file)
				continue;
			
			Com_DPrintf ("FindFile: %s\n",netpath);

			filelen = FS_filelength (*file);
			if (fs_cache->value)
			{
#ifdef BTREE_SEARCH
				FS_AddToCache (netpath, filelen, 0, filename);
#else
				FS_AddToCache (hash, netpath, filelen, 0, cache, filename);
#endif
			}
			return filelen;
		}
		
	}
	
	Com_DPrintf ("FindFile: can't find %s\n", filename);

	if (fs_cache->value >= 2)
	{
#ifdef BTREE_SEARCH
		FS_AddToCache (NULL, 0, 0, filename);
#else
		FS_AddToCache (hash, NULL, 0, 0, cache, filename);
#endif
	}
	
	*file = NULL;
	return -1;
}

/*
=================
FS_ReadFile

Properly handles partial reads
=================
*/
#ifdef CD_AUDIO
#include "../client/cdaudio.h"
#endif
#define	MAX_READ	0x4000		// read in blocks of 64k
void FS_Read (void *buffer, int len, FILE *f)
{
	int		block, remaining;
	int		read;
	byte	*buf;
#ifdef CD_AUDIO
	int		tries = 0;
#endif

	buf = (byte *)buffer;

	// read in chunks for progress bar
	remaining = len;

	while (remaining)
	{
		block = remaining;
		if (block > MAX_READ)
			block = MAX_READ;
		read = fread (buf, 1, block, f);
		if (read == 0)
		{
			// we might have been trying to read from a CD
#ifdef CD_AUDIO
			if (!tries)
			{
				tries = 1;
				CDAudio_Stop();
			}
			else
#endif
				Com_Error (ERR_FATAL, "FS_Read: 0 bytes read");
		}

		if (read == -1)
			Com_Error (ERR_FATAL, "FS_Read: -1 bytes read");

		// do some progress bar thing here...

		remaining -= read;
		buf += read;
	}
}

#ifdef WIN32
#ifdef _DEBUG
#include <windows.h>
LARGE_INTEGER start;
#define START_PERFORMANCE_TIMER _START_PERFORMANCE_TIMER()
#define STOP_PERFORMANCE_TIMER _STOP_PERFORMANCE_TIMER()
void _START_PERFORMANCE_TIMER (void)
{
	QueryPerformanceCounter (&start);
}
void _STOP_PERFORMANCE_TIMER (void)
{
	LARGE_INTEGER stop;
	__int64 diff;
	QueryPerformanceCounter (&stop);
	diff = stop.QuadPart - start.QuadPart;
	Com_Printf ("Function executed in %I64u ticks.\n", diff);
}
#else
#define START_PERFORMANCE_TIMER
#define STOP_PERFORMANCE_TIMER
#endif
#endif

/*
============
FS_LoadFile

Filename are reletive to the quake search path
a null buffer will just return the file length without loading
============
*/
char emptyFile = 0;
int EXPORT FS_LoadFile (char *path, void /*@out@*/ /*@null@*/**buffer)
{
	FILE	*h;
	byte	*buf;
	int		len;//, i;
//	unsigned char message[21];
//	char	temp[512];
//	char	temp2[8];
	buf = NULL;	// quiet compiler warning

// look for it in the filesystem or pack files
	//START_PERFORMANCE_TIMER;
	//Com_Printf ("%s... ", path);
	len = FS_FOpenFile (path, &h);
	//STOP_PERFORMANCE_TIMER;

	if (!h)
	{
		if (buffer)
			*buffer = NULL;
		return -1;
	}
	
	if (!buffer)
	{
		fclose (h);
		return len;
	}

	if (!len)
	{
		fclose (h);
		Com_Printf ("WARNING: 0 byte file: %s\n", path);
		*buffer = &emptyFile;
		return 0;
	}

	buf = Z_TagMalloc(len, TAGMALLOC_FSLOADFILE);
	*buffer = buf;
	FS_Read (buf, len, h);

	fclose (h);

	return len;
}


/*
=============
FS_FreeFile
=============
*/
void EXPORT FS_FreeFile (void *buffer)
{
	Z_Free (buffer);
}

/*
=================
FS_LoadPackFile

Takes an explicit (not game tree related) path to a pak file.

Loads the header and directory, adding the files at the beginning
of the list so they override previous pack files.
=================
*/
pack_t /*@null@*/ *FS_LoadPackFile (char *packfile)
{
	dpackheader_t	header;
	int				i;
	packfile_t		*newfiles;
	int				numpackfiles;
	pack_t			*pack;
	FILE			*packhandle;
	dpackfile_t		info[MAX_FILES_IN_PACK];

	packhandle = fopen(packfile, "rb");
	if (!packhandle)
		return NULL;

	fread (&header, 1, sizeof(header), packhandle);
	if (LittleLong(header.ident) != IDPAKHEADER)
		Com_Error (ERR_FATAL, "%s is not a valid packfile", packfile);
	
	header.dirofs = LittleLong (header.dirofs);
	header.dirlen = LittleLong (header.dirlen);

	numpackfiles = header.dirlen / sizeof(dpackfile_t);

	if (numpackfiles > MAX_FILES_IN_PACK)
		Com_Error (ERR_FATAL, "FS_LoadPackFile: packfile %s has %i files (max allowed %d)", packfile, numpackfiles, MAX_FILES_IN_PACK);

	if (!numpackfiles)
	{
		Com_Printf ("Ignoring empty packfile %s\n", packfile);
		return NULL;
	}

	newfiles = Z_TagMalloc (numpackfiles * sizeof(packfile_t), TAGMALLOC_FSLOADPAK);

	if (fseek (packhandle, header.dirofs, SEEK_SET))
		Com_Error (ERR_FATAL, "FS_LoadPackFile: fseek() to offset %d in %s failed (corrupt packfile?)", header.dirofs, packfile);

	if (fread (info, 1, header.dirlen, packhandle) != header.dirlen)
		Com_Error (ERR_FATAL, "FS_LoadPackFile: error reading packfile directory");

	for (i=0 ; i<numpackfiles ; i++)
	{
		strcpy (newfiles[i].name, info[i].name);
		newfiles[i].filepos = LittleLong(info[i].filepos);
		newfiles[i].filelen = LittleLong(info[i].filelen);
		//strlwr (info[i].name);
		newfiles[i].hash = hashify (info[i].name);
	}

	pack = Z_TagMalloc (sizeof (pack_t), TAGMALLOC_FSLOADPAK);
	strcpy (pack->filename, packfile);

	pack->handle = packhandle;
	pack->numfiles = numpackfiles;
	pack->files = newfiles;

	Com_Printf ("Added packfile %s (%i files)\n",  packfile, numpackfiles);
	return pack;
}


/*
================
FS_AddGameDirectory

Sets fs_gamedir, adds the directory to the head of the path,
then loads and adds pak1.pak pak2.pak ... 
================
*/
void FS_AddGameDirectory (char *dir)
{
	int				i;
	searchpath_t	*search;
	pack_t			*pak;
	char			pakfile[MAX_OSPATH];

	strcpy (fs_gamedir, dir);

	//
	// add the directory to the search path
	//
	search = Z_TagMalloc (sizeof(searchpath_t), TAGMALLOC_SEARCHPATH);
	strcpy (search->filename, dir);
	search->next = fs_searchpaths;
	fs_searchpaths = search;

	//
	// add any pak files in the format pak0.pak pak1.pak, ...
	//
	for (i=0; i<64; i++)
	{
		Com_sprintf (pakfile, sizeof(pakfile), "%s/pak%i.pak", dir, i);
		pak = FS_LoadPackFile (pakfile);
		if (pak)
		{
			search = Z_TagMalloc (sizeof(searchpath_t), TAGMALLOC_SEARCHPATH);
			search->pack = pak;
			search->next = fs_searchpaths;
			fs_searchpaths = search;
		}
	}


}

/*
============
FS_Gamedir

Called to find where to write a file (demos, savegames, etc)
============
*/
char * EXPORT FS_Gamedir (void)
{
	if (*fs_gamedir)
		return fs_gamedir;
	else
		return BASEDIRNAME;
}

/*
=============
FS_ExecAutoexec
=============
*/
void FS_ExecAutoexec (void)
{
	char *dir;
	char name [MAX_QPATH];

	dir = Cvar_VariableString("gamedir");
	if (*dir)
		Com_sprintf(name, sizeof(name), "%s/%s/autoexec.cfg", fs_basedir->string, dir); 
	else
		Com_sprintf(name, sizeof(name), "%s/%s/autoexec.cfg", fs_basedir->string, BASEDIRNAME); 
	if (Sys_FindFirst(name, 0, SFF_SUBDIR | SFF_HIDDEN | SFF_SYSTEM))
		Cbuf_AddText ("exec autoexec.cfg\n");
	Sys_FindClose();
}


/*
================
FS_SetGamedir

Sets the gamedir and path to a different directory.
================
*/
void FS_SetGamedir (char *dir)
{
	searchpath_t	*next;

	if (strstr(dir, "..") || strstr(dir, "/")
		|| strstr(dir, "\\") || strstr(dir, ":") )
	{
		Com_Printf ("Gamedir should be a single filename, not a path\n");
		return;
	}

	//
	// free up any current game dir info
	//
	while (fs_searchpaths != fs_base_searchpaths)
	{
		if (fs_searchpaths->pack)
		{
			fclose (fs_searchpaths->pack->handle);
			Z_Free (fs_searchpaths->pack->files);
			Z_Free (fs_searchpaths->pack);
		}
		next = fs_searchpaths->next;
		Z_Free (fs_searchpaths);
		fs_searchpaths = next;
	}

	//
	// flush all data, so it will be forced to reload
	//

	FS_FlushCache();

#ifndef NO_SERVER
	if (dedicated && !dedicated->value)
#endif
		Cbuf_AddText ("vid_restart\nsnd_restart\n");
		//Cbuf_ExecuteText (EXEC_NOW, "vid_restart\nsnd_restart\ncl_restart\n");


	Com_sprintf (fs_gamedir, sizeof(fs_gamedir), "%s/%s", fs_basedir->string, dir);

	if (!strcmp(dir,BASEDIRNAME) || (*dir == 0))
	{
		Cvar_FullSet ("gamedir", "", CVAR_SERVERINFO|CVAR_NOSET);
		Cvar_FullSet ("game", "", CVAR_LATCH|CVAR_SERVERINFO);
	}
	else
	{
		Cvar_FullSet ("gamedir", dir, CVAR_SERVERINFO|CVAR_NOSET);
		FS_AddGameDirectory (va("%s/%s", fs_basedir->string, dir) );
	}
}


/*
================
FS_Link_f

Creates a filelink_t
================
*/
void FS_Link_f (void)
{
	filelink_t	*l, **prev;

	if (Cmd_Argc() != 3)
	{
		Com_Printf ("USAGE: link <from> <to>\n");
		return;
	}

	// see if the link already exists
	prev = &fs_links;
	for (l=fs_links ; l ; l=l->next)
	{
		if (!strcmp (l->from, Cmd_Argv(1)))
		{
			Z_Free (l->to);
			if (!strlen(Cmd_Argv(2)))
			{	// delete it
				*prev = l->next;
				Z_Free (l->from);
				Z_Free (l);
				return;
			}
			l->to = CopyString (Cmd_Argv(2), TAGMALLOC_LINK);
			return;
		}
		prev = &l->next;
	}

	// create a new link
	l = Z_TagMalloc(sizeof(*l), TAGMALLOC_LINK);
	l->next = fs_links;
	fs_links = l;
	l->from = CopyString(Cmd_Argv(1), TAGMALLOC_LINK);
	l->fromlength = strlen(l->from);
	l->to = CopyString(Cmd_Argv(2), TAGMALLOC_LINK);
}

/*
** FS_ListFiles
*/
char /*@null@*/ **FS_ListFiles( char *findname, int *numfiles, unsigned musthave, unsigned canthave )
{
	char *s;
	int nfiles = 0;
	char **list = 0;

	s = Sys_FindFirst( findname, musthave, canthave );
	while ( s )
	{
		if ( s[strlen(s)-1] != '.' )
			nfiles++;
		s = Sys_FindNext( musthave, canthave );
	}
	Sys_FindClose ();

	if ( !nfiles )
		return NULL;

	nfiles++; // add space for a guard
	*numfiles = nfiles;

	list = malloc( sizeof( char * ) * nfiles );
	memset( list, 0, sizeof( char * ) * nfiles );

	s = Sys_FindFirst( findname, musthave, canthave );
	nfiles = 0;
	while ( s )
	{
		if ( s[strlen(s)-1] != '.' )
		{
			list[nfiles] = strdup( s );
#ifdef _WIN32
			strlwr( list[nfiles] );
#endif
			nfiles++;
		}
		s = Sys_FindNext( musthave, canthave );
	}
	Sys_FindClose ();

	return list;
}

/*
** FS_Dir_f
*/
void FS_Dir_f( void )
{
	char	*path = NULL;
	char	findname[1024];
	char	wildcard[1024] = "*.*";
	char	**dirnames;
	int		ndirs;

	if ( Cmd_Argc() != 1 )
	{
		strcpy( wildcard, Cmd_Argv( 1 ) );
	}

	while ( ( path = FS_NextPath( path ) ) != NULL )
	{
		char *tmp = findname;

		Com_sprintf( findname, sizeof(findname), "%s/%s", path, wildcard );

		while ( *tmp != 0 )
		{
			if ( *tmp == '\\' ) 
				*tmp = '/';
			tmp++;
		}
		Com_Printf( "Directory of %s\n", findname );
		Com_Printf( "----\n" );

		if ( ( dirnames = FS_ListFiles( findname, &ndirs, 0, 0 ) ) != 0 )
		{
			int i;

			for ( i = 0; i < ndirs-1; i++ )
			{
				if ( strrchr( dirnames[i], '/' ) )
					Com_Printf( "%s\n", strrchr( dirnames[i], '/' ) + 1 );
				else
					Com_Printf( "%s\n", dirnames[i] );

				free( dirnames[i] );
			}
			free( dirnames );
		}
		Com_Printf( "\n" );
	};
}

/*
============
FS_Path_f

============
*/
void FS_Path_f (void)
{
	searchpath_t	*s;
	filelink_t		*l;

	Com_Printf ("Current search path:\n");
	for (s=fs_searchpaths ; s ; s=s->next)
	{
		if (s == fs_base_searchpaths)
			Com_Printf ("----------\n");
		if (s->pack)
			Com_Printf ("%s (%i files)\n", s->pack->filename, s->pack->numfiles);
		else
			Com_Printf ("%s\n", s->filename);
	}

	Com_Printf ("\nLinks:\n");
	for (l=fs_links ; l ; l=l->next)
		Com_Printf ("%s : %s\n", l->from, l->to);
}

/*
================
FS_NextPath

Allows enumerating all of the directories in the search path
================
*/
char /*@null@*/ *FS_NextPath (char *prevpath)
{
	searchpath_t	*s;
	char			*prev;

	if (!prevpath)
		return fs_gamedir;

	prev = fs_gamedir;
	for (s=fs_searchpaths ; s ; s=s->next)
	{
		if (s->pack)
			continue;
		if (prevpath == prev)
			return s->filename;
		prev = s->filename;
	}

	return NULL;
}

/*
================
FS_InitFilesystem
================
*/
void FS_InitFilesystem (void)
{
	Cmd_AddCommand ("path", FS_Path_f);
	Cmd_AddCommand ("link", FS_Link_f);
	Cmd_AddCommand ("dir", FS_Dir_f );

	//r1: allow manual cache flushing
	Cmd_AddCommand ("fsflushcache", FS_FlushCache);

	//r1: fs stats
	Cmd_AddCommand ("fsstats", FS_Stats_f);

	//r1: binary tree filesystem cache
	FS_InitCache ();

	//r1: init fs cache
	FS_FlushCache ();

	//
	// basedir <path>
	// allows the game to run from outside the data tree
	//
	fs_basedir = Cvar_Get ("basedir", ".", CVAR_NOSET);

	//
	// start up with baseq2 by default
	//
	FS_AddGameDirectory (va("%s/"BASEDIRNAME, fs_basedir->string) );

	// any set gamedirs will be freed up to here
	fs_base_searchpaths = fs_searchpaths;

	// check for game override
	fs_gamedirvar = Cvar_Get ("game", "", CVAR_LATCH|CVAR_SERVERINFO);
	if (fs_gamedirvar->string[0])
		FS_SetGamedir (fs_gamedirvar->string);

	fs_cache = Cvar_Get ("fs_cache", "2", 0);
	fs_noextern = Cvar_Get ("fs_noextern", "0", 0);
}
