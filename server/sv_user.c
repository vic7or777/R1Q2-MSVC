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
// sv_user.c -- server code for moving users

#include "server.h"

edict_t	*sv_player;

cvar_t	*sv_max_download_size;

char	svConnectStuffString[1100];
char	svBeginStuffString[1100];

int		stringCmdCount;

/*
============================================================

USER STRINGCMD EXECUTION

sv_client and sv_player will be valid.
============================================================
*/

static void SV_BaselinesMessage (qboolean userCmd);

/*
==================
SV_BeginDemoServer
==================
*/
static void SV_BeginDemoserver (void)
{
	char		name[MAX_OSPATH];
	qboolean	dummy;

	Com_sprintf (name, sizeof(name), "demos/%s", sv.name);
	FS_FOpenFile (name, &sv.demofile, HANDLE_DUPE, &dummy);

	if (!sv.demofile)
		Com_Error (ERR_HARD, "Couldn't open demo %s", name);
}

/*
================
SV_CreateBaseline

Entity baselines are used to compress the update messages
to the clients -- only the fields that differ from the
baseline will be transmitted
================
*/
static void SV_CreateBaseline (client_t *cl)
{
	edict_t			*svent;
	int				entnum;

	memset (cl->lastlines, 0, sizeof(entity_state_t) * MAX_EDICTS);

	for (entnum = 1; entnum < ge->num_edicts ; entnum++)
	{
		svent = EDICT_NUM(entnum);

		if (!svent->inuse)
			continue;

		if (!svent->s.modelindex && !svent->s.sound && !svent->s.effects)
			continue;

		svent->s.number = entnum;

		//
		// take current state as baseline
		//
		//VectorCopy (svent->s.origin, svent->s.old_origin);
		cl->lastlines[entnum] = svent->s;
		VectorCopy (cl->lastlines[entnum].origin, cl->lastlines[entnum].old_origin);
	}
}

static void SV_New_f (void);
static void SV_AddConfigstrings (void)
{
	int		start;
	int		wrote;
	int		len;

	if (sv_client->state != cs_spawning)
	{
		//r1: dprintf to avoid console spam from idiot client
		Com_Printf ("configstrings for %s not valid -- not spawning\n", LOG_SERVER|LOG_WARNING, sv_client->name);
		return;
	}

	start = 0;
	wrote = 0;

	// write a packet full of data
#ifndef NO_ZLIB
	if (sv_client->protocol == ORIGINAL_PROTOCOL_VERSION)
#endif
	{
#ifndef NO_ZLIB
plainStrings:
#endif
		while (start < MAX_CONFIGSTRINGS)
		{
			if (sv.configstrings[start][0])
			{
				len = (int)strlen(sv.configstrings[start]);

				len = len > MAX_QPATH ? MAX_QPATH : len;

				MSG_BeginWriting (svc_configstring);
				MSG_WriteShort (start);
				MSG_Write (sv.configstrings[start], len);
				MSG_Write ("\0", 1);
				SV_AddMessage (sv_client, true);

				//we add in a stuffcmd every 500 bytes to ensure that old clients will transmit a
				//netchan ack asap. uuuuuuugly...
				wrote += len;
				if (wrote >= 500)
				{
					MSG_BeginWriting (svc_stufftext);
					MSG_WriteString ("cmd \177n\n");
					SV_AddMessage (sv_client, true);
					wrote = 0;
				}
			}
			start++;
		}
	}
#ifndef NO_ZLIB
	else
	{
		int			index;
		uint32		realBytes;
		int			result;
		z_stream	z;
		sizebuf_t	zBuff;
		byte		tempConfigStringPacket[MAX_USABLEMSG-5];
		byte		compressedStringStream[MAX_USABLEMSG-5];

		while (start < MAX_CONFIGSTRINGS)
		{
			memset (&z, 0, sizeof(z));
			realBytes = 0;

			z.next_out = compressedStringStream;
			z.avail_out = sizeof(compressedStringStream);

			if (deflateInit2 (&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY) != Z_OK)
			{
				SV_ClientPrintf (sv_client, PRINT_HIGH, "deflateInit2() failed.\n");
				goto plainStrings;
			}

			SZ_Init (&zBuff, tempConfigStringPacket, sizeof (tempConfigStringPacket));

			index = start;

			while ( z.total_out < sv_client->netchan.message.buffsize - 200)
			{
				SZ_Clear (&zBuff);

				while (index < MAX_CONFIGSTRINGS)
				{
					if (sv.configstrings[index][0])
					{
						len = (int)strlen(sv.configstrings[index]);

						MSG_BeginWriting (svc_configstring);
						MSG_WriteShort (index);
						MSG_Write (sv.configstrings[index], len > MAX_QPATH ? MAX_QPATH : len);
						MSG_Write ("\0", 1);
						MSG_EndWriting (&zBuff);

						if (zBuff.cursize >= 300 || z.total_out > sv_client->netchan.message.buffsize - 300)
						{
							index++;
							break;
						}
					}
					index++;
				}

				if (!zBuff.cursize)
					break;

				z.avail_in = zBuff.cursize;
				z.next_in = zBuff.data;

				realBytes += zBuff.cursize;

				result = deflate(&z, Z_SYNC_FLUSH);
				if (result != Z_OK)
				{
					SV_ClientPrintf (sv_client, PRINT_HIGH, "deflate() Z_SYNC_FLUSH failed.\n");
					goto plainStrings;
				}
				if (z.avail_out == 0)
				{
					SV_ClientPrintf (sv_client, PRINT_HIGH, "deflate() ran out of buffer space.\n");
					goto plainStrings;
				}
			}

			result = deflate(&z, Z_FINISH);
			if (result != Z_STREAM_END) {
				SV_ClientPrintf (sv_client, PRINT_HIGH, "deflate() Z_FINISH failed.\n");
				goto plainStrings;
			}

			result = deflateEnd(&z);
			if (result != Z_OK) {
				SV_ClientPrintf (sv_client, PRINT_HIGH, "deflateEnd() failed.\n");
				goto plainStrings;
			}

			if (z.total_out > realBytes)
			{
				Com_DPrintf ("SV_Configstrings_f: %d bytes would be a %lu byte zPacket\n", realBytes, z.total_out);
				goto plainStrings;
			}

			start = index;

			Com_DPrintf ("SV_Configstrings_f: wrote %d bytes in a %lu byte zPacket\n", realBytes, z.total_out);

			MSG_BeginWriting (svc_zpacket);
			MSG_WriteShort (z.total_out);
			MSG_WriteShort (realBytes);
			MSG_Write (compressedStringStream, z.total_out);
			SV_AddMessage (sv_client, true);
#ifndef NPROFILE
			svs.proto35CompressionBytes += realBytes - z.total_out;
#endif
		}
	}
#endif

	// send next command

	SV_BaselinesMessage (false);
}

/*
================
SV_New_f

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each server load.
================
*/
static void SV_New_f (void)
{
	const char			*gamedir;
	int					playernum;
	edict_t				*ent;
	varban_t			*bans;

	//cvar chars that we can use without causing problems on clients.
	//nocheat uses %var.
	//r1q2 uses ${var}.
	//others???. note we like `/~ since it prevents user from using console :)
	static const char junkChars[] = "!#.-0123456789@<=>?:&ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz~~~~``````";

	Com_DPrintf ("New() from %s\n", sv_client->name);

	if (sv_client->state != cs_connected)
	{
		if (sv_client->state == cs_spawning)
		{
			//client typed 'reconnect/new' while connecting.
			MSG_BeginWriting (svc_stufftext);
			MSG_WriteString ("\ndisconnect\nreconnect\n");
			SV_AddMessage (sv_client, true);
			SV_DropClient (sv_client, true);
			//SV_WriteReliableMessages (sv_client, sv_client->netchan.message.buffsize);
		}
		else
		{
			//shouldn't be here!
			Com_DPrintf ("WARNING: Illegal 'new' from %s, client state %d. This shouldn't happen...\n", sv_client->name, sv_client->state);
		}
		return;
	}

	// demo servers just dump the file message
	if (sv.state == ss_demo)
	{
		SV_BeginDemoserver ();
		return;
	}

	//r1: new client state now to prevent multiple new from causing high cpu / overflows.
	sv_client->state = cs_spawning;

	//r1: fix for old clients that don't respond to stufftext due to pending cmd buffer
	MSG_BeginWriting (svc_stufftext);
	MSG_WriteString ("\n");
	SV_AddMessage (sv_client, true);

	if (SV_UserInfoBanned (sv_client))
		return;

	if (sv_force_reconnect->string[0] && !sv_client->reconnect_done && !NET_IsLANAddress (&sv_client->netchan.remote_address))
	{
		if (sv_client->reconnect_var[0] == 0)
		{
			int		i;
			int		j;

			int		varindex;
			int		conindex;

			int		serverIndex;

			char	aliasConnect[32];

			char	aliasJunk[16][32];
			char	randomIP[16][32];

			for (i = 0; i < sizeof(sv_client->reconnect_var)-1; i++)
			{
				sv_client->reconnect_var[i] = junkChars[(int)(random() * (sizeof(junkChars)-1))];
			}

			for (i = 0; i < sizeof(sv_client->reconnect_var)-1; i++)
				sv_client->reconnect_value[i] = junkChars[(int)(random() * (sizeof(junkChars)-1))];

			for (i = 0; i < sizeof(aliasConnect)-1; i++)
				aliasConnect[i] = junkChars[(int)(random() * (sizeof(junkChars)-1))];
			aliasConnect[i] = 0;

			for (i = 0; i < 16; i++)
			{
				for (j = 0; j < sizeof(aliasJunk[0])-1; j++)
					aliasJunk[i][j] =junkChars[(int)(random() * (sizeof(junkChars)-1))];

				aliasJunk[i][j] = 0;
				Com_sprintf (randomIP[i], sizeof(randomIP[0]), "%d.%d.%d.%d:%d", (int)(random() * 255),  (int)(random() * 255), (int)(random() * 255), (int)(random() * 255), server_port);
			}

			serverIndex = (int)(random() * 15);

			Q_strncpy (randomIP[serverIndex], sv_force_reconnect->string, sizeof(randomIP[0])-1);

			conindex = (int)(random() * 15);
			varindex = (int)(random() * 15);

			for (i = 0; i < 16; i++)
			{
				if (i == conindex)
				{
					MSG_BeginWriting (svc_stufftext);
					MSG_WriteString (va ("set %s connect\n", aliasConnect));
					SV_AddMessage (sv_client, true);
				}
				if (i == varindex)
				{
					MSG_BeginWriting (svc_stufftext);
					MSG_WriteString (va ("set %s \"%s\"\n", sv_client->reconnect_var, sv_client->reconnect_value));
					SV_AddMessage (sv_client, true);
				}
				MSG_BeginWriting (svc_stufftext);
				MSG_WriteString (va ("set %s \"%s\"\n", aliasJunk[i], randomIP[i]));
				SV_AddMessage (sv_client, true);
			}

			MSG_BeginWriting (svc_stufftext);
			MSG_WriteString (va ("$%s $%s\n",  aliasConnect, aliasJunk[serverIndex]));
			SV_AddMessage (sv_client, true);

			if (sv_client->netchan.reliable_length)
			{
				//FIXME: why does this happen?
				Com_Printf ("WARNING: Calling SV_WriteReliableMessages for %s but netchan already has %d bytes of data! This shouldn't happen.\n", LOG_SERVER|LOG_WARNING, sv_client->name, sv_client->netchan.reliable_length);
				Com_Printf ("Netchan data: %s\n", LOG_GENERAL, MakePrintable (sv_client->netchan.message.data, sv_client->netchan.message.cursize));
			}
			else
			{
				//add to netchan immediately since we destroy it next line
				SV_WriteReliableMessages (sv_client, sv_client->netchan.message.buffsize);
			}

			//give them 5 seconds to reconnect
			//sv_client->lastmessage = svs.realtime - ((timeout->intvalue - 5) * 1000);
			SV_DropClient (sv_client, false);
			return;
		}
	}

	//
	// serverdata needs to go over for all types of servers
	// to make sure the protocol is right, and to set the gamedir
	//
	gamedir = Cvar_VariableString ("gamedir");

	// send the serverdata
	MSG_BeginWriting (svc_serverdata);

	//r1: report back the same protocol they used in their connection
	MSG_WriteLong (sv_client->protocol);
	MSG_WriteLong (svs.spawncount);
	MSG_WriteByte (sv.attractloop);
	MSG_WriteString (gamedir);

	if (sv.state == ss_cinematic || sv.state == ss_pic)
		playernum = -1;
	else
		playernum = (int)(sv_client - svs.clients);
	MSG_WriteShort (playernum);

	// send full levelname
	MSG_WriteString (sv.configstrings[CS_NAME]);

	if (sv_client->protocol == ENHANCED_PROTOCOL_VERSION)
	{
		//are we enhanced?
#ifdef ENHANCED_SERVER
		MSG_WriteByte (1);
#else
		MSG_WriteByte (0);
#endif

		//forced protocol breakage for 34 fallback
		MSG_WriteShort (CURRENT_ENHANCED_COMPATIBILITY_NUMBER);

		MSG_WriteByte (sv_advanced_deltas->intvalue);
		MSG_WriteByte (sv_strafejump_hack->intvalue);
	}

	SV_AddMessage (sv_client, true);

	//r1: we have to send another \n in case serverdata caused game switch -> autoexec without \n
	//this will still cause failure if the last line of autoexec exec's another config for example.
	MSG_BeginWriting (svc_stufftext);
	MSG_WriteString ("\n");
	SV_AddMessage (sv_client, true);

	if (sv_force_reconnect->string[0] && !sv_client->reconnect_done && !NET_IsLANAddress (&sv_client->netchan.remote_address))
	{
		if (sv_client->reconnect_var[0])
		{
			MSG_BeginWriting (svc_stufftext);
			MSG_WriteString (va ("cmd \177c %s $%s\n", sv_client->reconnect_var, sv_client->reconnect_var));
			SV_AddMessage (sv_client, true);
		}
	}

	if (!sv_client->versionString)
	{
		MSG_BeginWriting (svc_stufftext);
		MSG_WriteString ("cmd \177c version $version\n");
		SV_AddMessage (sv_client, true);
	}

	bans = &cvarbans;

	while (bans->next)
	{
		bans = bans->next;
		if (!(!sv_client->versionString && !strcmp (bans->varname, "version")))
		{
			MSG_BeginWriting (svc_stufftext);
			MSG_WriteString (va("cmd \177c %s $%s\n", bans->varname, bans->varname));
			SV_AddMessage (sv_client, true);
		}
	}

	if (svConnectStuffString[0])
	{
		MSG_BeginWriting (svc_stufftext);
		MSG_WriteString (svConnectStuffString);
		SV_AddMessage (sv_client, true);
	}

	//
	// game server
	// 
	switch (sv.state)
	{
		case ss_game:
			// set up the entity for the client
			ent = EDICT_NUM(playernum+1);
			ent->s.number = playernum+1;
			sv_client->edict = ent;

			memset (&sv_client->lastcmd, 0, sizeof(sv_client->lastcmd));

			//r1: per-client baselines
			SV_CreateBaseline (sv_client);

			// begin fetching configstrings
			//MSG_BeginWriting (svc_stufftext);
			//MSG_WriteString (va("cmd configstrings %i 0\n", svs.spawncount) );
			//SV_AddMessage (sv_client, true);

			SV_AddConfigstrings ();
			break;

		case ss_pic:
		case ss_cinematic:
			MSG_BeginWriting (svc_stufftext);
			MSG_WriteString (va("cmd begin %i\n", svs.spawncount));
			SV_AddMessage (sv_client, true);
			break;

		default:
			break;
	}
}

/*
==================
SV_Baselines_f
==================
*/
static void SV_BaselinesMessage (qboolean userCmd)
{
	int				startPos;
	int				start;
	int				wrote;

	entity_state_t	*base;

	Com_DPrintf ("Baselines() from %s\n", sv_client->name);

	if (sv_client->state != cs_spawning)
	{
		Com_DPrintf ("%s: baselines not valid -- not spawning\n", sv_client->name);
		return;
	}
	
	// handle the case of a level changing while a client was connecting
	if (userCmd)
	{
		if ( atoi(Cmd_Argv(1)) != svs.spawncount)
		{
			Com_Printf ("SV_Baselines_f from %s from a different level\n", LOG_SERVER|LOG_NOTICE, sv_client->name);
			SV_New_f ();
			return;
		}

		startPos = atoi(Cmd_Argv(2));
	}
	else
	{
		startPos = 0;
	}

	//r1: huge security fix !! remote DoS by negative here.
	if (startPos < 0)
	{
		Com_Printf ("Illegal baseline offset from %s[%s], client dropped\n", LOG_SERVER|LOG_EXPLOIT, sv_client->name, NET_AdrToString (&sv_client->netchan.remote_address));
		Blackhole (&sv_client->netchan.remote_address, true, sv_blackhole_mask->intvalue, BLACKHOLE_SILENT, "attempted DoS (negative baselines)");
		SV_DropClient (sv_client, false);
		return;
	}

	start = startPos;
	wrote = 0;

	// write a packet full of data
	//r1: use new per-client baselines
#ifndef NO_ZLIB
	if (sv_client->protocol == ORIGINAL_PROTOCOL_VERSION)
#endif
	{
#ifndef NO_ZLIB
plainLines:
#endif
		start = startPos;
		while (start < MAX_EDICTS)
		{
			base = &sv_client->lastlines[start];
			if (base->number)
			{
				MSG_BeginWriting (svc_spawnbaseline);
				MSG_WriteDeltaEntity (&null_entity_state, base, true, true, sv_client->protocol, sv_advanced_deltas->intvalue);
				wrote += MSG_GetLength();
				SV_AddMessage (sv_client, true);

				//we add in a stuffcmd every 500 bytes to ensure that old clients will transmit a
				//netchan ack asap. uuuuuuugly...
				if (wrote >= 500)
				{
					MSG_BeginWriting (svc_stufftext);
					MSG_WriteString ("cmd \177n\n");
					SV_AddMessage (sv_client, true);
					wrote = 0;
				}

			}
			start++;
		}
	}
#ifndef NO_ZLIB
	else
	{
		uint32		realBytes;
		int			result;
		z_stream	z;
		sizebuf_t	zBuff;
		byte		tempBaseLinePacket[MAX_USABLEMSG];
		byte		compressedLineStream[MAX_USABLEMSG];

		while (start < MAX_EDICTS)
		{
			memset (&z, 0, sizeof(z));
			z.next_out = compressedLineStream;
			z.avail_out = sizeof(compressedLineStream);

			realBytes = 0;

			if (deflateInit2 (&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY) != Z_OK)
			{
				SV_ClientPrintf (sv_client, PRINT_HIGH, "deflateInit2() failed.\n");
				goto plainLines;
			}

			SZ_Init (&zBuff, tempBaseLinePacket, sizeof (tempBaseLinePacket));

			while ( z.total_out < sv_client->netchan.message.buffsize - 200 )
			{
				SZ_Clear (&zBuff);
				while (start < MAX_EDICTS)
				{
					base = &sv_client->lastlines[start];
					if (base->number)
					{
						MSG_BeginWriting (svc_spawnbaseline);
						MSG_SetBit (8);
						MSG_WriteDeltaEntity (&null_entity_state, base, true, true, sv_client->protocol, sv_advanced_deltas->intvalue);
						MSG_EndWriting (&zBuff);

						if (zBuff.cursize >= 300 || z.total_out > sv_client->netchan.message.buffsize - 300)
						{
							start++;
							break;
						}
					}
					start++;
				}

				if (!zBuff.cursize)
					break;

				z.avail_in = zBuff.cursize;
				z.next_in = zBuff.data;

				realBytes += zBuff.cursize;

				result = deflate(&z, Z_SYNC_FLUSH);
				if (result != Z_OK)
				{
					SV_ClientPrintf (sv_client, PRINT_HIGH, "deflate() Z_SYNC_FLUSH failed.\n");
					goto plainLines;
				}
				if (z.avail_out == 0)
				{
					SV_ClientPrintf (sv_client, PRINT_HIGH, "deflate() ran out of buffer space.\n");
					goto plainLines;
				}
			}

			result = deflate(&z, Z_FINISH);
			if (result != Z_STREAM_END) {
				SV_ClientPrintf (sv_client, PRINT_HIGH, "deflate() Z_FINISH failed.\n");
				goto plainLines;
			}

			result = deflateEnd(&z);
			if (result != Z_OK) {
				SV_ClientPrintf (sv_client, PRINT_HIGH, "deflateEnd() failed.\n");
				goto plainLines;
			}

			if (z.total_out > realBytes)
			{
				Com_DPrintf ("SV_Baselines_f: %d bytes would be a %lu byte zPacket\n", realBytes, z.total_out);
				goto plainLines;
			}

			startPos = start;

			Com_DPrintf ("SV_Baselines_f: wrote %d bytes in a %lu byte zPacket\n", realBytes, z.total_out);

			MSG_BeginWriting (svc_zpacket);
			MSG_WriteShort (z.total_out);
			MSG_WriteShort (realBytes);
			MSG_Write (compressedLineStream, z.total_out);
			SV_AddMessage (sv_client, true);
#ifndef NPROFILE
			svs.proto35CompressionBytes += realBytes - z.total_out;
#endif
		}
	}
#endif

	// send next command
	MSG_BeginWriting (svc_stufftext);
	MSG_WriteString (va("precache %i\n", svs.spawncount));
	SV_AddMessage (sv_client, true);
}

int SV_CountPlayers (void)
{
	int i;
	int count = 0;
	client_t *cl;

	if (!svs.initialized)
		return 0;

	for (i=0,cl=svs.clients; i < maxclients->intvalue ; i++,cl++)
	{
		if (cl->state != cs_spawned)
			continue;

		count++;
	}

	return count;
}

static void SV_BadCommand_f (void)
{
	Com_Printf ("WARNING: Illegal '%s' from %s, client dropped.\n", LOG_SERVER|LOG_WARNING, Cmd_Argv(0), sv_client->name);
	SV_DropClient (sv_client, false);
	return;
}

/*
==================
SV_Begin_f
==================
*/
static void SV_Begin_f (void)
{
	Com_DPrintf ("Begin() from %s\n", sv_client->name);

	//r1: could be abused to respawn or cause spam/other mod-specific problems
	if (sv_client->state != cs_spawning)
	{
		Com_Printf ("EXPLOIT: Illegal 'begin' from %s[%s] (already spawned), client dropped.\n", LOG_SERVER|LOG_WARNING, sv_client->name, NET_AdrToString (&sv_client->netchan.remote_address));
		SV_DropClient (sv_client, false);
		return;
	}

	// handle the case of a level changing while a client was connecting
	if ( atoi(Cmd_Argv(1)) != svs.spawncount )
	{
		Com_Printf ("SV_Begin_f from %s for a different level\n", LOG_SERVER|LOG_NOTICE, sv_client->name);
		sv_client->state = cs_connected;
		SV_New_f ();
		return;
	}

	if (!sv_client->versionString)
	{
		//r1: they didn't respond to version probe
		Com_Printf ("WARNING: Didn't receive 'version' string from %s[%s], hacked/broken client? Client dropped.\n", LOG_SERVER|LOG_WARNING, sv_client->name, NET_AdrToString (&sv_client->netchan.remote_address));
		SV_DropClient (sv_client, false);
		return;
	}
	else if (sv_client->reconnect_var[0])
	{
		//r1: or the reconnect cvar...
		Com_Printf ("WARNING: Client %s[%s] didn't respond to reconnect check, hacked/broken client? Client dropped.\n", LOG_SERVER|LOG_WARNING, sv_client->name, NET_AdrToString (&sv_client->netchan.remote_address));
		SV_DropClient (sv_client, false);
		return;
	}
	else if (sv_client->download)
	{
		//r1: they're still downloading? shouldn't be...
		Com_Printf ("WARNING: Begin from %s[%s] while still downloading. Client dropped.\n", LOG_SERVER|LOG_WARNING, sv_client->name, NET_AdrToString (&sv_client->netchan.remote_address));
		SV_DropClient (sv_client, false);
		return;
	}

	sv_client->downloadsize = 0;

	sv_client->state = cs_spawned;

	//r1: check dll versions for struct mismatch
	if (sv_client->edict->client == NULL)
		Com_Error (ERR_HARD, "Tried to run API V4 game on a V3 server!!");

	if (sv_deny_q2ace->intvalue)
	{
		SV_ClientPrintf (sv_client, PRINT_CHAT, "console: p_auth q2acedetect\r                                         \rWelcome to %s! [%d/%d players, %d minutes into game]\n", hostname->string, SV_CountPlayers(), maxclients->intvalue, (int)((float)sv.time / 1000 / 60));
		//SV_ClientPrintf (sv_client, PRINT_CHAT, "p_auth                                                                                                                                                                                                                                                                                                                                \r                 \r");
	}

	if (svBeginStuffString[0])
	{
		MSG_BeginWriting (svc_stufftext);
		MSG_WriteString (svBeginStuffString);
		SV_AddMessage (sv_client, true);
	}

	// call the game begin function
	ge->ClientBegin (sv_player);

	//give them some movement
	//FIXME: make this msec value correct based on server framenum
	sv_client->commandMsec = sv_msecs->intvalue;
	sv_client->commandMsecOverflowCount = 0;


	//r1: this is in bad place
	//Cbuf_InsertFromDefer ();
}

//=============================================================================

/*void SV_Protocol_Test_f (void)
{
	SV_BroadcastPrintf (PRINT_HIGH, "%s is protocol %d\n", sv_client->name, sv_client->netchan.protocol);
}*/

/*
==================
SV_NextDownload_f
==================
*/
static void SV_NextDownload_f (void)
{
	uint32		r;
	int			percent;
	int			size;
	int			remaining;

//	sizebuf_t	*queue;

	if (!sv_client->download)
		return;

	remaining = sv_client->downloadsize - sv_client->downloadcount;
	
#ifndef NO_ZLIB
	if (sv_client->downloadCompressed)
	{
		byte		zOut[0xFFFF];
		byte		*buff;
		z_stream	z = {0};
		int			i, j;
		uint32		realBytes;
		int			result;

		z.next_out = zOut;
		z.avail_out = sizeof(zOut);

		realBytes = 0;

		if (deflateInit2 (&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY) != Z_OK)
		{
			SV_ClientPrintf (sv_client, PRINT_HIGH, "deflateInit2() failed.\n");
			SV_DropClient (sv_client, true);
			return;
		}

		j = 0;

		//r = sv_client->downloadsize - sv_client->downloadcount;

		if (remaining > sv_client->netchan.message.buffsize - 300)
			r = sv_client->netchan.message.buffsize - 300;
		else
			r = remaining;

		//if (r + sv_client->datagram.cursize >= MAX_USABLEMSG)
		//	r = MAX_USABLEMSG - sv_client->datagram.cursize - 400;

		//if (sv_client->downloadcount >= 871224)
		//	Sys_DebugBreak ();

		while ( z.total_out < r )
		{
			i = 300;

			if (sv_client->downloadcount + j + i > sv_client->downloadsize)
				i = sv_client->downloadsize - (sv_client->downloadcount + j);

			//in case of really good compression...
			if (realBytes + i > 0xFFFF)
				break;

			buff = sv_client->download + sv_client->downloadcount + j;

			z.avail_in = i;
			z.next_in = buff;

			realBytes += i;

			j += i;

			result = deflate(&z, Z_SYNC_FLUSH);
			if (result != Z_OK)
			{
				SV_ClientPrintf (sv_client, PRINT_HIGH, "deflate() Z_SYNC_FLUSH failed.\n");
				SV_DropClient (sv_client, true);
				return;
			}

			if (z.avail_out == 0)
			{
				SV_ClientPrintf (sv_client, PRINT_HIGH, "deflate() ran out of buffer space.\n");
				SV_DropClient (sv_client, true);
				return;
			}

			if (sv_client->downloadcount + j == sv_client->downloadsize)
				break;
		}

		result = deflate(&z, Z_FINISH);
		if (result != Z_STREAM_END)
		{
			SV_ClientPrintf (sv_client, PRINT_HIGH, "deflate() Z_FINISH failed.\n");
			SV_DropClient (sv_client, true);
			return;
		}

		result = deflateEnd(&z);
		if (result != Z_OK)
		{
			SV_ClientPrintf (sv_client, PRINT_HIGH, "deflateEnd() failed.\n");
			SV_DropClient (sv_client, true);
			return;
		}

		if (z.total_out >= realBytes || z.total_out >= (sv_client->netchan.message.buffsize - 6) || realBytes < sv_client->netchan.message.buffsize - 100)
			goto olddownload;

		//r1: use message queue so other reliable messages put in the stream perhaps by game won't cause overflow
		//queue = MSGQueueAlloc (sv_client, 6 + z.total_out, svc_zdownload);

		MSG_BeginWriting (svc_zdownload);
		MSG_WriteShort (z.total_out);

		size = sv_client->downloadsize;

		if (!size)
			size = 1;

		sv_client->downloadcount += realBytes;
		percent = sv_client->downloadcount*100/size;
		
		MSG_WriteByte (percent);

		MSG_WriteShort (realBytes);
		MSG_Write (zOut, z.total_out);
		SV_AddMessage (sv_client, true);
#ifndef NPROFILE
		svs.proto35CompressionBytes += realBytes - z.total_out;
#endif
	}
	else
#endif
	{
#ifndef NO_ZLIB
olddownload:
#endif
		//r1: use message queue so other reliable messages put in the stream perhaps by game won't cause overflow
		//queue = MSGQueueAlloc (sv_client, 4 + r, svc_zdownload);

		if (remaining > sv_client->netchan.message.buffsize - 100)
			r = sv_client->netchan.message.buffsize - 100;
		else
			r = remaining;

		MSG_BeginWriting (svc_download);
		MSG_WriteShort (r);

		sv_client->downloadcount += r;
		size = sv_client->downloadsize;

		if (!size)
			size = 1;

		percent = sv_client->downloadcount*100/size;
		MSG_WriteByte (percent);

		MSG_Write (sv_client->download + sv_client->downloadcount - r, r);
		SV_AddMessage (sv_client, true);
	}

	if (sv_client->downloadcount != sv_client->downloadsize)
		return;

	FS_FreeFile (sv_client->download);
	sv_client->download = NULL;
	sv_client->downloadsize = 0;

	Z_Free (sv_client->downloadFileName);
	sv_client->downloadFileName = NULL;
}

#define	DL_UDP	0x00000001
#define	DL_TCP	0x00000002

/*
==================
SV_BeginDownload_f
==================
*/
static void SV_BeginDownload_f(void)
{
	char		*name, *p;
	int			offset = 0;
	size_t		length;
	qboolean	valid;

	name = Cmd_Argv(1);

	if (Cmd_Argc() > 2)
		offset = atoi(Cmd_Argv(2)); // downloaded offset

	//name is always filtered for security reasons
	StripHighBits (name, 1);

	//ugly hack to allow server to see clients who are using http dl.
	if (!strcmp (name, "http"))
	{
		if (sv_client->download)
			SV_DropClient (sv_client, false);
		else
			sv_client->downloadsize = 1;
		return;
	}

	// hacked by zoid to allow more conrol over download
	// first off, no .. or global allow check

	if (sv_download_drop_file->string[0] && !Q_stricmp (name, sv_download_drop_file->string))
	{
		if (sv_download_drop_message->modified)
		{
			ExpandNewLines (sv_download_drop_message->string);
			
			if (strlen (sv_download_drop_message->string) >= MAX_USABLEMSG - 16)
			{
				Com_Printf ("WARNING: sv_download_drop_message string is too long!\n", LOG_SERVER|LOG_WARNING);
				Cvar_Set ("sv_download_drop_message", "");
			}
			sv_download_drop_message->modified = false;
		}
		Com_Printf ("Dropping %s for trying to download %s.\n", LOG_SERVER|LOG_DOWNLOAD, sv_client->name, name);
		SV_ClientPrintf (sv_client, PRINT_HIGH, "%s\n", sv_download_drop_message->string);
		SV_DropClient (sv_client, true);
		return;
	}

	if (sv_download_refuselimit->intvalue && SV_CountPlayers() >= sv_download_refuselimit->intvalue)
	{
		SV_ClientPrintf (sv_client, PRINT_HIGH, "Too many players connected, refusing download: ");
		MSG_BeginWriting (svc_download);
		MSG_WriteShort (-1);
		MSG_WriteByte (0);
		SV_AddMessage (sv_client, true);
		return;
	}

	length = strlen(name);

	//fix some ./ references in maps, eg ./textures/map/file
	p = name;
	while ((p = strstr (p, "./")))
	{
		memmove (p, p+2, length - (p - name) - 1);
		length -= 2;
	}

	//block the really nasty ones - \server.cfg will download from mod root on win32, .. is obvious
	if (name[0] == '\\' || strstr (name, ".."))
	{
		Com_Printf ("Refusing illegal download path %s to %s\n", LOG_SERVER|LOG_DOWNLOAD|LOG_EXPLOIT, name, sv_client->name);
		MSG_BeginWriting (svc_download);
		MSG_WriteShort (-1);
		MSG_WriteByte (0);
		SV_AddMessage (sv_client, true);
		Com_Printf ("EXPLOIT: Client %s[%s] tried to download illegal path: %s\n", LOG_EXPLOIT|LOG_SERVER, sv_client->name, NET_AdrToString (&sv_client->netchan.remote_address), name);
		Blackhole (&sv_client->netchan.remote_address, true, sv_blackhole_mask->intvalue, BLACKHOLE_SILENT, "download exploit (path %s)", name);
		SV_DropClient (sv_client, false);
		return;
	}
	//negative offset will crash on read
	else if (offset < 0)
	{
		Com_Printf ("Refusing illegal download offset %d to %s\n", LOG_SERVER|LOG_DOWNLOAD|LOG_EXPLOIT, offset, sv_client->name);
		MSG_BeginWriting (svc_download);
		MSG_WriteShort (-1);
		MSG_WriteByte (0);
		SV_AddMessage (sv_client, true);
		Com_Printf ("EXPLOIT: Client %s[%s] supplied illegal download offset for %s: %d\n", LOG_EXPLOIT|LOG_SERVER, sv_client->name, NET_AdrToString (&sv_client->netchan.remote_address), name, offset);
		Blackhole (&sv_client->netchan.remote_address, true, sv_blackhole_mask->intvalue, BLACKHOLE_SILENT, "download exploit (offset %d)", offset);
		SV_DropClient (sv_client, false);
		return;
	}
	else if (!length || name[0] == 0 //empty name, maybe as result of ./ normalize
		//|| name[0] == '.' 
		// leading slash bad as well, must be in subdir
		//|| name[0] == '/'
		|| !isvalidchar (name[0])
		// r1: \ is bad in general, client won't even write properly if we do sent it
		|| strchr (name, '\\')
		// MUST be in a subdirectory	
		|| !strchr (name, '/')
		//fix for / at eof causing dir open -> crash (note, we don't blackhole this one because original q2 client
		//with allow_download_players 1 will scan entire CS_PLAYERSKINS. since some mods overload it, this may result
		//in example "download players/\nsomething/\n".
		//|| name[length-1] == '/'
		// r1: another bug, maps/. will fopen(".") -> crash
		//|| name[length-1] == '.'
		|| !isvalidchar (name[length-1])
		)
	{
		Com_Printf ("Refusing bad download path %s to %s\n", LOG_SERVER|LOG_DOWNLOAD|LOG_WARNING, name, sv_client->name);
		MSG_BeginWriting (svc_download);
		MSG_WriteShort (-1);
		MSG_WriteByte (0);
		SV_AddMessage (sv_client, true);
		return;
	}
	//r1: non-enhanced clients don't auto download a sprite's skins. this results in crash when trying to render it.
	else if (sv_client->protocol == ORIGINAL_PROTOCOL_VERSION && length >= 4 && !Q_stricmp (name + length - 4, ".sp2"))
	{
		Com_Printf ("Refusing download of sprite %s to %s\n", LOG_SERVER|LOG_DOWNLOAD|LOG_WARNING, name, sv_client->name);
		SV_ClientPrintf (sv_client, PRINT_HIGH, "\nRefusing download of '%s' as your client may not fetch any linked skins.\n"
												"Please download the '%s' mod to get this file.\n\n", name, Cvar_VariableString ("gamename"));
		MSG_BeginWriting (svc_download);
		MSG_WriteShort (-1);
		MSG_WriteByte (0);
		SV_AddMessage (sv_client, true);
		return;		
	}

	valid = true;

	if	(!allow_download->intvalue)
	{
		valid = false;
	}
	else if (strncmp(name, "players/", 8) == 0) 
	{
		if (!(allow_download_players->intvalue & DL_UDP))
			valid = false;
	}
	else if (strncmp(name, "models/", 7) == 0 || strncmp(name, "sprites/", 8) == 0)
	{
		if (!(allow_download_models->intvalue & DL_UDP))
			valid = false;
	}
	else if (strncmp(name, "sound/", 6) == 0)
	{
		if (!(allow_download_sounds->intvalue & DL_UDP))
			valid = false;
	}
	else if (strncmp(name, "maps/", 5) == 0)
	{
		if (!(allow_download_maps->intvalue & DL_UDP))
			valid = false;
	}
	else if (strncmp(name, "pics/", 5) == 0)
	{
		if (!(allow_download_pics->intvalue & DL_UDP))
			valid = false;
	}
	else if ((strncmp(name, "env/", 4) == 0 || strncmp(name, "textures/", 9) == 0))
	{
		if (!(allow_download_textures->intvalue & DL_UDP))
			valid = false;
	}
	else if (!allow_download_others->intvalue)
	{
		valid = false;
	}

	if (!valid)
	{
		Com_DPrintf ("Refusing to download %s to %s\n", name, sv_client->name);
		if (strncmp(name, "maps/", 5) == 0 && !(allow_download_maps->intvalue & DL_UDP))
		{
			if (sv_mapdownload_denied_message->modified)
			{
				ExpandNewLines (sv_mapdownload_denied_message->string);
				
				if (strlen (sv_mapdownload_denied_message->string) >= MAX_USABLEMSG - 16)
				{
					Com_Printf ("WARNING: sv_mapdownload_denied_message string is too long!\n", LOG_SERVER|LOG_WARNING);
					Cvar_Set ("sv_mapdownload_denied_message", "");
				}
				sv_mapdownload_denied_message->modified = false;
			}

			if (sv_mapdownload_denied_message->string[0])
				SV_ClientPrintf (sv_client, PRINT_HIGH, "%s\n", sv_mapdownload_denied_message->string);
		}
		MSG_BeginWriting (svc_download);
		MSG_WriteShort (-1);
		MSG_WriteByte (0);
		SV_AddMessage (sv_client, true);
		return;
	}

	//r1: should this ever happen?
	if (sv_client->download)
	{
		Com_Printf ("WARNING: Client %s started a download '%s' with an already existing download of '%s'.\n", LOG_SERVER|LOG_WARNING, sv_client->name, name, sv_client->downloadFileName);

		FS_FreeFile (sv_client->download);
		sv_client->download = NULL;

		Z_Free (sv_client->downloadFileName);
		sv_client->downloadFileName = NULL;
	}

	sv_client->downloadsize = FS_LoadFile (name, NULL);

	//adjust case and re-try
#ifdef LINUX
	if (sv_client->downloadsize == -1)
	{
		Q_strlwr (name);
		sv_client->downloadsize = FS_LoadFile (name, NULL);
	}
#endif

	if (sv_client->downloadsize == -1)
	{
		Com_Printf ("Couldn't download %s to %s\n", LOG_SERVER|LOG_DOWNLOAD|LOG_NOTICE, name, sv_client->name);

		MSG_BeginWriting (svc_download);
		MSG_WriteShort (-1);
		MSG_WriteByte (0);
		SV_AddMessage (sv_client, true);
		return;
	}

	if (sv_max_download_size->intvalue && sv_client->downloadsize > sv_max_download_size->intvalue)
	{
		SV_ClientPrintf (sv_client, PRINT_HIGH, "Refusing download, file is %d bytes larger than allowed: ", sv_client->downloadsize - sv_max_download_size->intvalue);

		MSG_BeginWriting (svc_download);
		MSG_WriteShort (-1);
		MSG_WriteByte (0);
		SV_AddMessage (sv_client, true);

		//FS_FreeFile (sv_client->download);
		//sv_client->download = NULL;

		Com_Printf ("Refused %s to %s because it exceeds sv_max_download_size\n", LOG_SERVER|LOG_DOWNLOAD|LOG_NOTICE, name, sv_client->name);
		return;
	}

	if (offset > sv_client->downloadsize)
	{
		char	*ext;
		Com_Printf ("Refused %s to %s because offset %d is larger than file size %d\n", LOG_SERVER|LOG_DOWNLOAD|LOG_NOTICE, name, sv_client->name, offset, sv_client->downloadsize);

		ext = strrchr (name, '.');
		if (ext)
		{
			strncpy (ext+1, "tmp", length - ((ext+1) - name));
		}
		SV_ClientPrintf (sv_client, PRINT_HIGH, "Refusing download, file size differs. Please delete %s: ", name);

		MSG_BeginWriting (svc_download);
		MSG_WriteShort (-1);
		MSG_WriteByte (0);
		SV_AddMessage (sv_client, true);

		//FS_FreeFile (sv_client->download);
		//sv_client->download = NULL;
		return;		
	}
	else if (offset == sv_client->downloadsize)
	{
		//they have the full file but don't realise it
		MSG_BeginWriting (svc_download);
		MSG_WriteShort (0);
		MSG_WriteByte (100);
		SV_AddMessage (sv_client, true);

		//FS_FreeFile (sv_client->download);
		//sv_client->download = NULL;
		return;
	}

	//download should be ok by here
	FS_LoadFile (name, (void **)&sv_client->download);

	sv_client->downloadcount = offset;

	if (strncmp(name, "maps/", 5) == 0)
	{
		if (sv_mapdownload_ok_message->modified)
		{
			ExpandNewLines (sv_mapdownload_ok_message->string);

			//make sure it fits
			if (strlen (sv_mapdownload_ok_message->string) >= MAX_USABLEMSG-16)
			{
				Com_Printf ("WARNING: sv_mapdownload_ok_message string is too long!\n", LOG_SERVER|LOG_WARNING);
				Cvar_Set ("sv_mapdownload_ok_message", "");
			}
			sv_mapdownload_ok_message->modified = false;
		}
		if (sv_mapdownload_ok_message->string[0])
			SV_ClientPrintf (sv_client, PRINT_HIGH, "%s\n", sv_mapdownload_ok_message->string);
	}

	//r1: r1q2 zlib udp downloads?
#ifndef NO_ZLIB
	if (!Q_stricmp (Cmd_Argv(3), "udp-zlib"))
		sv_client->downloadCompressed = true;
	else
#endif
		sv_client->downloadCompressed = false;

	sv_client->downloadFileName = CopyString (name, TAGMALLOC_CLIENT_DOWNLOAD);

	Com_Printf ("UDP downloading %s to %s%s\n", LOG_SERVER|LOG_DOWNLOAD, name, sv_client->name, sv_client->downloadCompressed ? " with zlib" : "");
	SV_NextDownload_f ();
}



//============================================================================


/*
=================
SV_Disconnect_f

The client is going to disconnect, so remove the connection immediately
=================
*/
static void SV_Disconnect_f (void)
{
//	SV_EndRedirect ();
	Com_Printf ("Dropping %s, client issued 'disconnect'.\n", LOG_SERVER|LOG_DROP, sv_client->name);
	SV_DropClient (sv_client, true);
}


/*
==================
SV_ShowServerinfo_f

Dumps the serverinfo info string
==================
*/
static void SV_ShowServerinfo_f (void)
{
	char	*s;
	char	*p;

	int		flip;

	//r1: this is a client issued command !
	//Info_Print (Cvar_Serverinfo());

	s = Cvar_Serverinfo();

	//skip beginning \\ char
	s++;

	flip = 0;
	p = s;

	//make it more readable
	while (*p)
	{
		if (*p == '\\')
		{
			if (flip)
				*p = '\n';
			else
				*p = '=';
			flip ^= 1;
		}
		p++;
	}

	SV_ClientPrintf (sv_client, PRINT_HIGH, "%s\n", s);
}

static void SV_ClientServerinfo_f (void)
{
	char	*strafejump_msg;
	char	*delta_msg;
	char	*optimize_msg;
	char	*packetents_msg;
	int		maxLen;

	strafejump_msg = sv_strafejump_hack->intvalue == 2 ? "Forced" : sv_strafejump_hack->intvalue ? "Enabled (requires protocol 35 client)" : "Disabled";
	delta_msg = sv_advanced_deltas->intvalue ? "Enabled (requires protocol 35 client)" : "Disabled";
	optimize_msg = sv_optimize_deltas->intvalue == 2 ? "Forced" : sv_optimize_deltas->intvalue ? "Enabled" : "Disabled";
	packetents_msg = sv_packetentities_hack->intvalue == 2 ? "Enabled (with protocol 35 zlib support)" : sv_packetentities_hack->intvalue ? "Enabled (without protocol 35 zlib support)" : "Disabled";

	maxLen = Cvar_IntValue ("net_maxmsglen");
	if (maxLen == 0)
		maxLen = MAX_USABLEMSG;

	SV_ClientPrintf (sv_client, PRINT_HIGH, 
		"Server Protocol Settings\n"
		"------------------------\n"
		"Your protocol  : %d\n"
		"Your max packet: %d (server max allowed: %d)\n"
		"Strafejump hack: %s\n"
		"Advanced deltas: %s\n"
		"Optimize deltas: %s\n"
		"Packetents hack: %s\n",
		sv_client->protocol,
		sv_client->netchan.message.buffsize, maxLen,
		strafejump_msg,
		delta_msg,
		optimize_msg,
		packetents_msg);
}

static void SV_NoGameData_f (void)
{
	sv_client->nodata ^= 1;
}

static void CvarBanDrop (const char *match, const banmatch_t *ban, const char *result)
{
	if (ban->message[0])
		SV_ClientPrintf (sv_client, PRINT_HIGH, "%s\n", ban->message);

	if (ban->blockmethod == CVARBAN_BLACKHOLE)
		Blackhole (&sv_client->netchan.remote_address, false, sv_blackhole_mask->intvalue, BLACKHOLE_SILENT, "cvarban: %s == %s", match, result);

	Com_Printf ("Dropped client %s, cvarban: %s == %s\n", LOG_SERVER|LOG_DROP, sv_client->name, match, result);

	SV_DropClient (sv_client, (ban->blockmethod == CVARBAN_BLACKHOLE) ? false : true);
}

const banmatch_t *VarBanMatch (varban_t *bans, const char *var, const char *result)
{
	banmatch_t			*match;
	char				*matchvalue;
	int					not;

	while (bans->next)
	{
		bans = bans->next;

		if (!Q_stricmp (bans->varname, var))
		{
			match = &bans->match;

			while (match->next)
			{
				match = match->next;

				matchvalue = match->matchvalue;

				not = 1;

				if (matchvalue[0] == '!')
				{
					not = 0;
					matchvalue++;
				}

				if (matchvalue[0] == '*')
				{
					if ((result[0] == 0 ? 0 : 1) == not)
						return match;
					continue;
				}
				else
				{
					if (!result[0])
						continue;
				}

				if (matchvalue[1])
				{
					float intresult, matchint;
					intresult = (float)atof(result);

					matchint = (float)atof(matchvalue+1);

					switch (matchvalue[0])
					{
						case '>':
							if ((intresult > matchint) == not)
								return match;
							continue;

						case '<':
							if ((intresult < matchint) == not)
								return match;
							continue;
						
						case '=':
							if ((intresult == matchint) == not)
								return match;
							continue;
						
						case '~':
							if ((strstr (result, matchvalue + 1) == NULL ? 0 : 1) == not)
								return match;
							continue;
						
						case '#':
							if (!Q_stricmp (matchvalue+1, result) ==  not)
								return match;
							continue;
						default:
							break;
					}
				}

				if (!Q_stricmp (matchvalue, result) ==  not)
					return match;
			}

			return NULL;
		}
	}

	return NULL;
}

static void SV_CvarResult_f (void)
{
	const banmatch_t	*match;
	const char			*result;

	result = Cmd_Args2(2);

	if (!strcmp (Cmd_Argv(1), "version"))
	{
		if (!sv_client->versionString && dedicated->intvalue)
			Com_Printf ("%s[%s]: protocol %d: \"%s\"\n", LOG_SERVER|LOG_CONNECT, sv_client->name, NET_AdrToString(&sv_client->netchan.remote_address), sv_client->protocol, result);
		if (sv_client->versionString)
			Z_Free (sv_client->versionString);
		sv_client->versionString = CopyString (result, TAGMALLOC_CVARBANS);
	}
	else if (!strcmp (Cmd_Argv(1), sv_client->reconnect_var))
	{
		sv_client->reconnect_var[0] = sv_client->reconnect_value[0] = 0;
		sv_client->reconnect_done = true;
		return;
	}

	//cvar responses don't count as malicious
	stringCmdCount--;

	match = VarBanMatch (&cvarbans, Cmd_Argv(1), result);

	if (match)
	{
		switch (match->blockmethod)
		{
			case CVARBAN_LOGONLY:
				Com_Printf ("LOG: %s[%s] matched cvarban: %s == %s\n", LOG_SERVER, sv_client->name, NET_AdrToString (&sv_client->netchan.remote_address), Cmd_Argv(1), result);
				break;
			case CVARBAN_MESSAGE:
				SV_ClientPrintf (sv_client, PRINT_HIGH, "%s\n", match->message);
				break;
			case CVARBAN_EXEC:
				Cbuf_AddText (match->message);
				Cbuf_AddText ("\n");
				break;
			case CVARBAN_STUFF:
				MSG_BeginWriting (svc_stufftext);
				MSG_WriteString (va ("%s\n",match->message));
				SV_AddMessage (sv_client, true);
				break;
			default:
				CvarBanDrop (Cmd_Argv(1), match, result);
		}
	}
	else
	{
		varban_t	*bans;

		if (!strcmp (Cmd_Argv(1), "version"))
			return;

		bans = &cvarbans;

		while (bans->next)
		{
			bans = bans->next;

			if (!strcmp (Cmd_Argv(1), bans->varname))
				return;
		}

		if (sv_badcvarcheck->intvalue == 0)
		{
			Com_Printf ("LOG: %s[%s] sent unrequested cvarban response: %s == %s\n", LOG_SERVER, sv_client->name, NET_AdrToString (&sv_client->netchan.remote_address), Cmd_Argv(1), result);
		}
		else
		{
			//note that certain versions of frkq2 can trigger this since the frkq2_cvar hiding code is bugged
			Com_Printf ("Dropping %s for bad cvar check result ('%s' unrequested).\n", LOG_SERVER|LOG_DROP, sv_client->name, Cmd_Argv(1));
			if (sv_badcvarcheck->intvalue == 2)
				Blackhole (&sv_client->netchan.remote_address, false, sv_blackhole_mask->intvalue, BLACKHOLE_SILENT, "unrequested cvarcheck result '%s'", Cmd_Argv(1));
			SV_DropClient (sv_client, false);
		}
	}
}

void SV_Nextserver (void)
{
	const char	*v;

	//ZOID, ss_pic can be nextserver'd in coop mode
	if (sv.state == ss_game || (sv.state == ss_pic && !Cvar_IntValue("coop")))
		return;		// can't nextserver while playing a normal game

	svs.spawncount++;	// make sure another doesn't sneak in
	v = Cvar_VariableString ("nextserver");
	if (!v[0])
		Cbuf_AddText ("killserver\n");
	else
	{
		Cbuf_AddText (v);
		Cbuf_AddText ("\n");
	}
	Cvar_Set ("nextserver","");
}

/*
==================
SV_Nextserver_f

A cinematic has completed or been aborted by a client, so move
to the next server,
==================
*/
static void SV_Nextserver_f (void)
{
	if ( atoi(Cmd_Argv(1)) != svs.spawncount ) {
		Com_DPrintf ("Nextserver() from wrong level, from %s\n", sv_client->name);
		return;		// leftover from last server
	}

	Com_DPrintf ("Nextserver() from %s\n", sv_client->name);

	SV_Nextserver ();
}

typedef struct
{
	char	/*@null@*/ *name;
	void	/*@null@*/ (*func) (void);
} ucmd_t;

static ucmd_t ucmds[] =
{
	// auto issued
	{"new", SV_New_f},
	{"configstrings", SV_BadCommand_f},	//these no longer work as commands under r1q2
	{"baselines", SV_BadCommand_f},
	{"begin", SV_Begin_f},

	{"nextserver", SV_Nextserver_f},

	{"disconnect", SV_Disconnect_f},

	// issued by hand at client consoles	
	{"info", SV_ShowServerinfo_f},

	{"sinfo", SV_ClientServerinfo_f},
	{"\177c", SV_CvarResult_f},
	{"\177n", Q_NullFunc},

	{"nogamedata", SV_NoGameData_f},

	{"download", SV_BeginDownload_f},
	{"nextdl", SV_NextDownload_f},
	//{"debuginfo", SV_Shit_f},

	{NULL, NULL}
};

//metavars
const char *SV_GetClientID (void)
{
	static char	idBuff[4];

	if (!sv_client)
		return "";

	sprintf (idBuff, "%d", (int)(sv_client - svs.clients));
	return idBuff;
}

const char *SV_GetClientIP (void)
{
	char	*p;
	char	*q;

	if (!sv_client)
		return "";

	p = NET_AdrToString (&sv_client->netchan.remote_address);
	q = strchr (p, ':');
	if (q)
		q[0] = 0;

	return p;
}

const char *SV_GetClientName (void)
{
	if (!sv_client)
		return "";

	return sv_client->name;
}

/*
==================
SV_ExecuteUserCommand
==================
*/
static void SV_ExecuteUserCommand (char *s)
{
	const char			*teststring;
	const char			*flattened;

	ucmd_t				*u;
	bannedcommands_t	*x;
	linkednamelist_t	*y;
	linkedvaluelist_t	*z;
	
	//r1: catch attempted command expansions
	if (strchr(s, '$'))
	{
		teststring = Cmd_MacroExpandString(s);
		if (!teststring)
			return;

		if (strcmp (teststring, s))
		{
			Com_Printf ("EXPLOIT: Client %s[%s] attempted macro-expansion: %s\n", LOG_EXPLOIT|LOG_SERVER, sv_client->name, NET_AdrToString(&sv_client->netchan.remote_address), MakePrintable(s, 0));
			//we no longer kick or blackhole for this since the new "cool" thing to do is to tell people
			//to type "$GOD" FOR GOD MODE!!! and other stupid stuff that some server admins seem to fall for.
			//we still return instead of processing the command though in case something else could expand it.
			//SV_KickClient (sv_client, "attempted server exploit", NULL);
			return;
		}
	}

	//r1: catch end-of-message exploit
	if (strchr (s, '\xFF'))
	{
		const char	*ptr;
		const char	*p;
		ptr = strchr (s, '\xFF');
		ptr -= 8;
		if (ptr < s)
			ptr = s;
		p = MakePrintable (ptr, 0);
		Blackhole (&sv_client->netchan.remote_address, true, sv_blackhole_mask->intvalue, BLACKHOLE_SILENT, "0xFF in command packet (%.32s)", p);
		Com_Printf ("EXPLOIT: Client %s[%s] tried to use a command containing 0xFF: %s\n", LOG_EXPLOIT|LOG_SERVER, sv_client->name, NET_AdrToString(&sv_client->netchan.remote_address), p);
		SV_KickClient (sv_client, "attempted command exploit", NULL);
		return;
	}

	//r1: allow filter of high bit commands (eg \n\r in say cmds)
	if (sv_filter_stringcmds->intvalue)
		StripHighBits(s, (int)sv_filter_stringcmds->intvalue == 2);

	Cmd_TokenizeString (s, false);

	flattened = Cmd_Args2 (0);

	sv_player = sv_client->edict;

	for (z = serveraliases.next; z; z = z->next)
	{
		if (!strcmp (Cmd_Argv(0), z->name) || !strcmp (flattened, z->name))
		{
			MSG_BeginWriting (svc_stufftext);
			MSG_WriteString (z->value);
			SV_AddMessage (sv_client, true);
			break;
		}
	}

	for (x = bannedcommands.next; x; x = x->next)
	{
		if (!strcmp (Cmd_Argv(0), x->name) || !strcmp (flattened, x->name))
		{
			if (x->logmethod == CMDBAN_LOG_MESSAGE)
				Com_Printf ("SV_ExecuteUserCommand: %s tried to use '%s'\n", LOG_SERVER, sv_client->name, s);

			if (x->kickmethod == CMDBAN_MESSAGE)
			{
				SV_ClientPrintf (sv_client, PRINT_HIGH, "The '%s' command has been disabled by the server administrator.\n", x->name);
			}
			else if (x->kickmethod == CMDBAN_KICK)
			{
				Com_Printf ("Dropping %s, bancommand %s matched.\n", LOG_SERVER|LOG_DROP, sv_client->name, x->name);
				SV_DropClient (sv_client, true);
			}
			else if (x->kickmethod == CMDBAN_BLACKHOLE)
			{
				Blackhole (&sv_client->netchan.remote_address, false, sv_blackhole_mask->intvalue, BLACKHOLE_SILENT, "bancommand '%s'", x->name);
				SV_DropClient (sv_client, false);
			}

			return;
		}
	}

	for (u=ucmds ; u->name ; u++)
	{
		if (!strcmp (Cmd_Argv(0), u->name) )
		{
			u->func ();

			//r1ch: why break?
			return;
		}
	}

	//r1: do we really want to be passing commands from unconnected players
	//to the game dll at this point? doesn't sound like a good idea to me
	//especially if the game dll does its own banning functions after connect
	//as banned players could spam game commands (eg say) whilst connecting
	if (sv_client->state < cs_spawned && !sv_allow_unconnected_cmds->intvalue)
		return;

	//r1: say parser (ick)
	if (!strcmp (Cmd_Argv(0), "say"))
	{
		//r1: nocheat kicker/spam filter
		if (!Q_strncasecmp (Cmd_Args(), "\"NoCheat V2.", 12))
		{
			if (sv_nc_kick->intvalue)
			{
				if ((int)(sv_nc_kick->intvalue) & 256)
				{
					Com_Printf ("%s was dropped for using NoCheat\n", LOG_SERVER|LOG_DROP, sv_client->name);
					if (((int)sv_nc_announce->intvalue & 256) && sv_client->state == cs_spawned && sv_client->name[0])
						SV_BroadcastPrintf (PRINT_HIGH, "%s was dropped: client is using NoCheat\n", sv_client->name);

					SV_ClientPrintf (sv_client, PRINT_HIGH, "NoCheat is not permitted on this server, please use regular Quake II.\n");
					SV_DropClient (sv_client, true);
					return;
				}
				else if (((int)(sv_nc_kick->intvalue) & 1) && strstr(Cmd_Args(), "Code\\-1\\"))
				{
					Com_Printf ("%s was dropped for failing NoCheat code check\n", LOG_SERVER|LOG_DROP, sv_client->name);
					if (((int)sv_nc_announce->intvalue & 1) && sv_client->state == cs_spawned && sv_client->name[0])
						SV_BroadcastPrintf (PRINT_HIGH, "%s was dropped: invalid NoCheat code\n", sv_client->name);

					SV_ClientPrintf (sv_client, PRINT_HIGH, "This server requires a valid NoCheat code. Please check you are running in OpenGL mode.\n");
					SV_DropClient (sv_client, true);
					return;
				}
				else if (((int)(sv_nc_kick->intvalue) & 2) && strstr(Cmd_Args(), "Video"))
				{
					Com_Printf ("%s was dropped for failing NoCheat video check\n", LOG_SERVER|LOG_DROP, sv_client->name);
					if (((int)sv_nc_announce->intvalue & 2) && sv_client->state == cs_spawned && sv_client->name[0])
						SV_BroadcastPrintf (PRINT_HIGH, "%s was dropped: failed NoCheat video check\n", sv_client->name);

					SV_ClientPrintf (sv_client, PRINT_HIGH, "This server requires a NoCheat approved vid_ref.\n");
					SV_DropClient (sv_client, true);
					return;
				}
				else if (((int)(sv_nc_kick->intvalue) & 4) && strstr(Cmd_Args(), "modelCheck"))
				{
					Com_Printf ("%s was dropped for failing NoCheat model check\n", LOG_SERVER|LOG_DROP, sv_client->name);
					if (((int)sv_nc_announce->intvalue & 4) && sv_client->state == cs_spawned && sv_client->name[0])
						SV_BroadcastPrintf (PRINT_HIGH, "%s was dropped: failed NoCheat model check\n", sv_client->name);

					SV_ClientPrintf (sv_client, PRINT_HIGH, "This server requires NoCheat approved models.\n");
					SV_DropClient (sv_client, true);
					return;
				}
				else if (((int)(sv_nc_kick->intvalue) & 8) && (strstr(Cmd_Args(), "FrkQ2") || strstr(Cmd_Args(), "Hack") || strstr(Cmd_Args(), "modelCheck") || strstr(Cmd_Args(), "glCheck")))
				{
					Com_Printf ("%s was dropped for failing additional NoCheat checks\n", LOG_SERVER|LOG_DROP, sv_client->name);
					if (((int)sv_nc_announce->intvalue & 8) && sv_client->state == cs_spawned && sv_client->name[0])
						SV_BroadcastPrintf (PRINT_HIGH, "%s was dropped: failed NoCheat hack checks\n", sv_client->name);

					SV_ClientPrintf (sv_client, PRINT_HIGH, "This server requires NoCheat approved video settings.\n");
					SV_DropClient (sv_client, true);
					return;
				}

				Com_Printf ("%s passed NoCheat verifications\n", LOG_SERVER|LOG_NOTICE, sv_client->name);
				if (((int)sv_nc_announce->intvalue & 128) && sv_client->state == cs_spawned && sv_client->name[0])
					SV_BroadcastPrintf (PRINT_HIGH, "%s passed NoCheat verifications\n", sv_client->name);
			}

			sv_client->notes |= NOTE_CLIENT_NOCHEAT;

			if (sv_filter_nocheat_spam->intvalue)
				return;
		}

		//r1: anti q2ace code (for the various hacks that have turned q2ace into cheat)
		if (sv_deny_q2ace->intvalue && !strncmp (Cmd_Args(), "q2ace v", 7) && (
			strstr (Cmd_Args(), "- Authentication") ||
			strstr (Cmd_Args(), "- Failed Auth")
			))
		{
			SV_KickClient (sv_client, "client is using q2ace", "q2ace is not permitted on this server, please use regular Quake II.\n");
			return;
		}

		//r1: note, we can reset on say as it's a "known good" command. resetting on every command is bad
		//since stuff like q2admin spams tons of stuffcmds all the time...
		sv_client->idletime = 0;
	}

	for (y = nullcmds.next; y; y = y->next)
	{
		if (!strcmp (Cmd_Argv(0), y->name) || !strcmp (flattened, y->name))
			return;
	}

	//r1ch: pointless if !u->name, why would we be here?
	if (sv.state == ss_game)
		ge->ClientCommand (sv_player);
}

/*
===========================================================================

USER CMD EXECUTION

===========================================================================
*/
static void SV_ClientThink (client_t *cl, usercmd_t *cmd)
{
	cl->commandMsec -= cmd->msec;

	if (cl->commandMsec < 0 && sv_enforcetime->intvalue)
		return;

	ge->ClientThink (cl->edict, cmd);
}

static void SV_SetClientSetting (client_t *cl)
{
	uint32	setting;
	uint32	value;

	setting = MSG_ReadShort (&net_message);
	value = MSG_ReadShort (&net_message);

	//unknown settings are ignored
	if (setting >= CLSET_MAX)
		return;

	cl->settings[setting] = value;
}

#ifdef _DEBUG
void SV_RunMultiMoves (client_t *cl)
{
	int			i;
	unsigned	bits;
	unsigned	offset;
	unsigned	nummoves;

	int			lastframe;

	usercmd_t	last;
	usercmd_t	move;
	usercmd_t	*oldcmd;

	bits = MSG_ReadByte (&net_message);

	//3 bits   5 bits
	//[xxx]    [xxxxx]
	//nummoves offset

	nummoves = bits & 0xE0;
	offset = bits & 0x1F;

	//special value 31 indicates lastframe is beyond representation of 5 bits, suck up long
	if (offset == 31)
	{
		lastframe = MSG_ReadLong (&net_message);
	}
	else
	{
		if (cl->lastframe == -1)
		{
			SV_KickClient (cl, NULL, "Invalid delta offset with lastframe -1\n");
			return;
		}
	}
	
	lastframe = cl->lastframe + offset;

	//r1ch: allow server admins to stop clients from using nodelta
	//note, this doesn't affect server->client if the clients frame
	//was too old (ie client lagged out) so should be safe to enable
	//nodelta clients typically consume 4-5x bandwidth than normal.
	if (lastframe == -1 && cl->lastframe == -1)
	{
		if (++cl->nodeltaframes >= 100 && !sv_allownodelta->intvalue)
		{
			SV_KickClient (cl, "too many nodelta packets", "ERROR: You may not use cl_nodelta on this server as it consumes excessive bandwidth. Please set cl_nodelta 0. This error may also be caused by a very laggy connection.\n");
			return;
		}
	}
	else
	{
		cl->nodeltaframes = 0;
	}

	if (lastframe != cl->lastframe)
	{
		cl->lastframe = lastframe;
		if (cl->lastframe > 0)
		{
			//FIXME: should we adjust for FPS latency?
			cl->frame_latency[cl->lastframe&(LATENCY_COUNTS-1)] = 
				svs.realtime - cl->frames[cl->lastframe & UPDATE_MASK].senttime;
		}
	}

	if ( cl->state != cs_spawned )
	{
		cl->lastframe = -1;
		return;
	}

	memset (&last, 0, sizeof(last));

	oldcmd = &cl->lastcmd;

	//r1: check there are actually enough usercmds in the message!
	for (i = 0; i < nummoves; i++)
	{
		MSG_ReadDeltaUsercmd (&net_message, &last, &move);

		if (net_message.readcount > net_message.cursize)
		{
			SV_KickClient (cl, "bad usercmd read", NULL);
			return;
		}

		//r1: normal q2 client caps at 250 internally so this is a nice hack check
		if (cl->state == cs_spawned && move.msec > 250)
		{
			Com_Printf ("EXPLOIT: Client %s[%s] tried to use illegal msec value: %d\n", LOG_EXPLOIT|LOG_SERVER, cl->name, NET_AdrToString (&cl->netchan.remote_address), move.msec);
			Blackhole (&cl->netchan.remote_address, true, sv_blackhole_mask->intvalue, BLACKHOLE_SILENT, "illegal msec value (%d)", move.msec);
			SV_KickClient (cl, "illegal pmove msec detected", NULL);
			return;
		}

		//r1: reset idle time on activity
		if (move.buttons != oldcmd->buttons ||
			move.forwardmove != oldcmd->forwardmove ||
			move.upmove != oldcmd->upmove)
			cl->idletime = 0;

		SV_ClientThink (cl, &move);
		last = move;
	}

	//flag to see if this is actually a player or what (used in givemsec)
	cl->moved = true;
	cl->lastcmd = move;
}
#endif

#define	MAX_STRINGCMDS			8
#define	MAX_USERINFO_UPDATES	8
/*
===================
SV_ExecuteClientMessage

The current net_message is parsed for the given client
===================
*/
void SV_ExecuteClientMessage (client_t *cl)
{
	int			c;
	char		*s;
	usercmd_t	oldest, oldcmd, newcmd;
	int			net_drop;
	int			userinfoCount;
	qboolean	move_issued;
	int			lastframe;

	sv_client = cl;
	sv_player = sv_client->edict;

	// only allow one move command
	move_issued = false;

	userinfoCount = 0;
	stringCmdCount = 0;

	for (;;)
	{
		if (net_message.readcount > net_message.cursize)
		{
			SV_KickClient (cl, "bad read", "SV_ExecuteClientMessage: bad read\n");
			return;
		}

		c = MSG_ReadByte (&net_message);
		if (c == -1)
			break;

		switch (c)
		{
		case clc_move:
			if (move_issued)
			{
				SV_KickClient (cl, "client issued clc_move when move_issued", "SV_ExecuteClientMessage: clc_move when move_issued\n");
				return;		// someone is trying to cheat...
			}

			move_issued = true;
			//checksumIndex = net_message.readcount;

			//r1ch: suck up the extra checksum byte that is no longer used
			if (cl->protocol == ORIGINAL_PROTOCOL_VERSION)
			{
				MSG_ReadByte (&net_message);
			}
			else if (cl->protocol != ENHANCED_PROTOCOL_VERSION)
			{
				Com_Printf ("SV_ExecuteClientMessage: bad protocol %d (memory overwritten!)\n", LOG_SERVER, cl->protocol);
				SV_KickClient (cl, "client state corrupted", "SV_ExecuteClientMessage: client state corrupted\n");
				return;
			}

			lastframe = MSG_ReadLong (&net_message);

			//r1ch: allow server admins to stop clients from using nodelta
			//note, this doesn't affect server->client if the clients frame
			//was too old (ie client lagged out) so should be safe to enable
			//nodelta clients typically consume 4-5x bandwidth than normal.
			if (lastframe == -1 && cl->lastframe == -1)
			{
				if (++cl->nodeltaframes >= 100 && !sv_allownodelta->intvalue)
				{
					SV_KickClient (cl, "too many nodelta packets", "ERROR: You may not use cl_nodelta on this server as it consumes excessive bandwidth. Please set cl_nodelta 0. This error may also be caused by a very laggy connection.\n");
					return;
				}
			}
			else
			{
				cl->nodeltaframes = 0;
			}

			if (lastframe != cl->lastframe)
			{
				cl->lastframe = lastframe;
				if (cl->lastframe > 0)
				{
					//FIXME: should we adjust for FPS latency?
					cl->frame_latency[cl->lastframe&(LATENCY_COUNTS-1)] = 
						svs.realtime - cl->frames[cl->lastframe & UPDATE_MASK].senttime;

					//if (cl->fps)
					//	cl->frame_latency[cl->lastframe&(LATENCY_COUNTS-1)] -= 1000 / (cl->fps * 2);
				}
			}

			//memset (&nullcmd, 0, sizeof(nullcmd));

			//r1: check there are actually enough usercmds in the message!
			MSG_ReadDeltaUsercmd (&net_message, &null_usercmd, &oldest);
			if (net_message.readcount > net_message.cursize)
			{
				SV_KickClient (cl, "bad clc_move usercmd read (oldest)", NULL);
				return;
			}

			MSG_ReadDeltaUsercmd (&net_message, &oldest, &oldcmd);
			if (net_message.readcount > net_message.cursize)
			{
				SV_KickClient (cl, "bad clc_move usercmd read (oldcmd)", NULL);
				return;
			}

			MSG_ReadDeltaUsercmd (&net_message, &oldcmd, &newcmd);
			if (net_message.readcount > net_message.cursize)
			{
				SV_KickClient (cl, "bad clc_move usercmd read (newcmd)", NULL);
				return;
			}

			//r1: normal q2 client caps at 250 internally so this is a nice hack check
			if (cl->state == cs_spawned && newcmd.msec > 250)
			{
				Com_Printf ("EXPLOIT: Client %s[%s] tried to use illegal msec value: %d\n", LOG_EXPLOIT|LOG_SERVER, cl->name, NET_AdrToString (&cl->netchan.remote_address), newcmd.msec);
				Blackhole (&cl->netchan.remote_address, true, sv_blackhole_mask->intvalue, BLACKHOLE_SILENT, "illegal msec value (%d)", newcmd.msec);
				SV_KickClient (cl, "illegal pmove msec detected", NULL);
				return;
			}

			if ( cl->state != cs_spawned )
			{
				cl->lastframe = -1;
				break;
			}

			//r1: reset idle time on activity
			if (newcmd.buttons != oldcmd.buttons ||
				newcmd.forwardmove != oldcmd.forwardmove ||
				newcmd.upmove != oldcmd.upmove)
				cl->idletime = 0;

			//flag to see if this is actually a player or what (used in givemsec)
			cl->moved = true;

			// if the checksum fails, ignore the rest of the packet
			//r1ch: removed, this has been hacked to death anyway so is waste of cycles
			/*calculatedChecksum = COM_BlockSequenceCRCByte (
				net_message_buffer + checksumIndex + 1,
				net_message.readcount - checksumIndex - 1,
				cl->netchan.incoming_sequence);

			if (calculatedChecksum != checksum)
			{
				Com_DPrintf ("Failed command checksum for %s (%d != %d)/%d\n", 
					cl->name, calculatedChecksum, checksum, 
					cl->netchan.incoming_sequence);
				return;
			}*/

			if (!sv_paused->intvalue)
			{
				net_drop = cl->netchan.dropped;

				//r1: server configurable command backup limit
				if (net_drop > sv_max_netdrop->intvalue)
					net_drop = sv_max_netdrop->intvalue; 

				if (net_drop)
				{

//if (net_drop > 2)

//	Com_Printf ("drop %i\n", net_drop);
					//if predicting, limit the amount of time they can catch up for
					if (sv_predict_on_lag->intvalue)
					{
						if (net_drop > 2)
							net_drop = 2;
						if (oldest.msec > 25)
							oldest.msec = 25;
						if (oldcmd.msec > 25)
							oldcmd.msec = 25;
						if (newcmd.msec > 75)
							newcmd.msec = 75;
					}
						
					while (net_drop > 2)
					{
						SV_ClientThink (cl, &cl->lastcmd);

						net_drop--;
					}
					if (net_drop > 1)
						SV_ClientThink (cl, &oldest);

					if (net_drop > 0)
						SV_ClientThink (cl, &oldcmd);

				}
				SV_ClientThink (cl, &newcmd);
			}

			cl->lastcmd = newcmd;
			break;

		case clc_stringcmd:
			c = net_message.readcount;
			s = MSG_ReadString (&net_message);

			//r1: another security check, client caps at 256+1, but a hacked client could
			//    send huge strings, if they are then used in a mod which sends a %s cprintf
			//    to the exe, this could result in a buffer overflow for example.

			// XXX: where is this capped?
			c = net_message.readcount - c;
			if (c > 256)
			{
				//Com_Printf ("%s: excessive stringcmd discarded.\n", cl->name);
				//break;
				Com_Printf ("Warning, %d byte stringcmd from %s: '%.32s...'\n", LOG_SERVER|LOG_WARNING, c, cl->name, s);
			}

			if (move_issued)
				Com_Printf ("Warning, out-of-order stringcmd '%.32s...' from %s\n", LOG_SERVER|LOG_WARNING, s, cl->name);

			// malicious users may try using too many string commands
			if (++stringCmdCount < MAX_STRINGCMDS)
				SV_ExecuteUserCommand (s);

			if (cl->state == cs_zombie)
				return;	// disconnect command
			break;

		case clc_userinfo:
			//r1: limit amount of userinfo per packet
			if (++userinfoCount < MAX_USERINFO_UPDATES)
			{
				strncpy (cl->userinfo, MSG_ReadString (&net_message), sizeof(cl->userinfo)-1);
				SV_UserinfoChanged (cl);
			}
			else
			{
				Com_Printf ("Warning, too many userinfo updates (%d) in single packet from %s\n", LOG_SERVER|LOG_WARNING, userinfoCount, cl->name);
				MSG_ReadString (&net_message);
			}

			if (move_issued)
				Com_Printf ("Warning, out-of-order userinfo from %s\n", LOG_SERVER|LOG_WARNING, cl->name);

			if (cl->state == cs_zombie)
				return;	//kicked

			break;

			//FIXME: remove this?
		case clc_nop:
			break;

		//r1ch ************* BEGIN R1Q2 SPECIFIC ****************************
		case clc_setting:
			SV_SetClientSetting (cl);
			break;

#ifdef _DEBUG
		case clc_multimoves:
			SV_RunMultiMoves (cl);
			break;
#endif
		//r1ch ************* END R1Q2 SPECIFIC ****************************

		default:
			Com_Printf ("SV_ExecuteClientMessage: unknown command byte %d from %s\n", LOG_SERVER|LOG_WARNING, c, cl->name);
			SV_KickClient (cl, "unknown command byte", va("unknown command byte %d\n", c));
			return;
		}
	}
}
