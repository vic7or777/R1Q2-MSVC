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

#include "server.h"

#define	HEARTBEAT_SECONDS	300

netadr_t	master_adr[MAX_MASTERS];	// address of group servers
netadr_t	netaddress_pyroadmin;

client_t	*sv_client;			// current client

cvar_t	*sv_paused;
cvar_t	*sv_timedemo;
cvar_t	*sv_downloadport;
cvar_t	*sv_fpsflood;

cvar_t	*sv_enforcetime;

cvar_t	*timeout;				// seconds without any message
cvar_t	*zombietime;			// seconds to sink messages after disconnect

cvar_t	*rcon_password;			// password for remote server commands

cvar_t	*allow_download;
cvar_t *allow_download_players;
cvar_t *allow_download_models;
cvar_t *allow_download_sounds;
cvar_t *allow_download_maps;

cvar_t *sv_airaccelerate;

cvar_t	*sv_noreload;			// don't reload level state when reentering

cvar_t	*maxclients;			// FIXME: rename sv_maxclients

#ifndef DEDICATED_ONLY
cvar_t	*sv_showclamp;
#endif

cvar_t	*hostname;
cvar_t	*public_server;			// should heartbeats be sent

#ifdef USE_PYROADMIN
cvar_t	*pyroadminport;
#endif

cvar_t	*sv_locked;
cvar_t	*sv_restartmap;
cvar_t	*sv_password;

//r1: filtering of strings for non-ASCII
cvar_t	*sv_filter_q3names;
cvar_t	*sv_filter_userinfo;
cvar_t	*sv_filter_stringcmds;

//r1: stupid bhole code
cvar_t	*sv_blackholes;

//r1: allow server to ban people who use nodelta (uses 3-5x more bandwidth than regular client)
cvar_t	*sv_allownodelta;

//r1: allow server to block q2ace and associated hacks
cvar_t	*sv_deny_q2ace;

//r1: max connections from a single IP (prevent DoS)
cvar_t	*sv_iplimit;

//r1: allow server to send mini-motd
cvar_t	*sv_connectmessage;

//r1: for nocheat mods
cvar_t  *sv_nc_visibilitycheck;
cvar_t	*sv_nc_clientsonly;

//r1: max backup packets to allow from client
cvar_t	*sv_max_netdrop;

//r1: limit amount of info from status cmd
cvar_t	*sv_hidestatus;
cvar_t	*sv_hideplayers;

//r1: randomize server framenum
cvar_t	*sv_randomframe;

//r1: number of msecs to give
cvar_t	*sv_msecs;

cvar_t	*sv_nc_kick;
cvar_t	*sv_nc_announce;
cvar_t	*sv_filter_nocheat_spam ;

cvar_t	*sv_gamedebug;
cvar_t	*sv_recycle;

cvar_t	*sv_uptime;

cvar_t	*sv_strafejump_hack;
cvar_t	*sv_reserved_slots;
cvar_t	*sv_reserved_password;

cvar_t	*sv_allow_map;
cvar_t	*sv_allow_unconnected_cmds;

cvar_t	*sv_strict_userinfo_check;

cvar_t	*sv_calcpings_method;

//r1: not needed
//cvar_t	*sv_reconnect_limit;	// minimum seconds between connect messages

time_t	server_start_time;

blackhole_t			blackholes;
cvarban_t			cvarbans;
bannedcommands_t	bannedcommands;

int		pyroadminid;

void Master_Shutdown (void);

int CharVerify (char c)
{
	if ((int)sv_filter_q3names->value == 2)
		return !(isalnum(c));
	else
		return (c == '^');
}

//============================================================================

int NameColorFilterCheck (char *newname)
{
	int length;
	int i;

	if (!sv_filter_q3names->value)
		return 0;

	length = strlen (newname)-1;

	for (i = 0; i < length; i++) {
		if (CharVerify(*(newname+i)) && isdigit (*(newname+i+1)))	{
			return 1;
		}
	}

	return 0;
}

/*
=====================
SV_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing.
=====================
*/
void SV_DropClient (client_t *drop)
{
	message_queue_t *msgq;

	// add the disconnect
	MSG_BeginWriteByte (&drop->netchan.message, svc_disconnect);

	if (drop->state == cs_spawned)
	{
		// call the prog function for removing a client
		// this will remove the body, among other things
		ge->ClientDisconnect (drop->edict);
	}

	if (drop->download)
	{
		/*if (drop->protocol == ENHANCED_PROTOCOL_VERSION)
			free (drop->download);
		else*/
		FS_FreeFile (drop->download);

		drop->download = NULL;
	}

	//r1: free baselines
	if (drop->lastlines)
		Z_Free (drop->lastlines);

	//r1: free version string
	if (drop->versionString)
		Z_Free (drop->versionString);

	//r1: free message queue
	while (drop->messageQueue.next)
	{
		msgq = drop->messageQueue.next;
		drop->messageQueue.next = msgq->next;
		Z_Free (msgq->data);
		Z_Free (msgq);
	}

	drop->lastlines = NULL;

	drop->state = cs_zombie;		// become free in a few seconds
	drop->name[0] = 0;
}

void SV_KickClient (client_t *cl, char /*@null@*/*reason, char /*@null@*/*cprintf)
{
	if (reason && cl->state == cs_spawned && *cl->name)
		SV_BroadcastPrintf (PRINT_HIGH, "%s was dropped: %s\n", cl->name, reason);
	if (cprintf)
		SV_ClientPrintf (cl, PRINT_HIGH, "%s", cprintf);
	SV_DropClient (cl);
}


/*
==============================================================================

CONNECTIONLESS COMMANDS

==============================================================================
*/

/*
===============
SV_StatusString

Builds the string that is sent as heartbeats and status replies
===============
*/
char	*SV_StatusString (void)
{
	char	*serverinfo;
	char	player[1024];
	static char	status[MAX_MSGLEN - 16];
	int		i;
	client_t	*cl;
	int		statusLength;
	int		playerLength;
//	player_state_new	*ps;

	serverinfo = Cvar_Serverinfo();

	//r1: add uptime info
	if (sv_uptime->value)
	{
		char uptimeString[128];
		char tmpStr[16];

		time_t secs;

		int days = 0;
		int hours = 0;
		int mins = 0;
		int	years = 0;

		uptimeString[0] = 0;
		secs = time(NULL) - server_start_time;

		while (secs/60/60/24/365.25 >= 1)
		{
			years++;
			secs -= 60*60*24*365.25;
		}

		while (secs/60/60/24 >= 1)
		{
			days++;
			secs -= 60*60*24;
		}

		while (secs/60/60 >= 1)
		{
			hours++;
			secs -= 60*60;
		}

		while (secs/60 >= 1)
		{
			mins++;
			secs -= 60;
		}

		if (years)
		{
			if ((int)sv_uptime->value == 1)
			{
				sprintf (tmpStr, "%d year%s", years, years == 1 ? "" : "s");
				strcat (uptimeString, tmpStr);
			}
			else
			{
				days += 365.25;
			}
		}

		if (days)
		{
			if ((int)sv_uptime->value == 1)
			{
				sprintf (tmpStr, "%dday%s", days, days == 1 ? "" : "s");
				if (uptimeString[0])
					strcat (uptimeString, ", ");
			}
			else
			{
				sprintf (tmpStr, "%d+", days);
			}
			strcat (uptimeString, tmpStr);
		}

		if ((int)sv_uptime->value == 1)
		{
			if (hours)
			{
				sprintf (tmpStr, "%dhr%s", hours, hours == 1 ? "" : "s");
				if (uptimeString[0])
					strcat (uptimeString, ", ");
				strcat (uptimeString, tmpStr);
			}
		}
		else
		{
			if (days || hours)
			{
				if (days)
					sprintf (tmpStr, "%.2d:", hours);
				else
					sprintf (tmpStr, "%d:", hours);
				strcat (uptimeString, tmpStr);
			}
		}

		if ((int)sv_uptime->value == 1)
		{
			if (mins)
			{
				sprintf (tmpStr, "%dmin%s", mins, mins == 1 ? "" : "s");
				if (uptimeString[0])
					strcat (uptimeString, ", ");
				strcat (uptimeString, tmpStr);
			}
			else if (!uptimeString[0])
			{
				sprintf (uptimeString, "%dsec%s", secs, secs == 1 ? "" : "s");
			}
		}
		else
		{
			if (days || hours || mins)
			{
				if (hours)
					sprintf (tmpStr, "%.2d.", mins);
				else
					sprintf (tmpStr, "%d.", mins);
				strcat (uptimeString, tmpStr);
			}
			if (days || hours || mins || secs)
			{
				if (mins)
					sprintf (tmpStr, "%.2d", secs);
				else
					sprintf (tmpStr, "%d", secs);
				strcat (uptimeString, tmpStr);
			}
		}

		Info_SetValueForKey (serverinfo, "uptime", uptimeString);
	}

	//r1: hide reserved slots
	Info_SetValueForKey (serverinfo, "maxclients", va("%d", (int)maxclients->value - (int)sv_reserved_slots->value));

	strcpy (status, serverinfo);
	strcat (status, "\n");
	statusLength = strlen(status);

	if (!sv_hideplayers->value)
	{
		for (i=0 ; i<(int)maxclients->value ; i++)
		{
			cl = &svs.clients[i];
			if (cl->state == cs_connected || cl->state == cs_spawned )
			{
	#ifdef ENHANCED_SERVER
					Com_sprintf (player, sizeof(player), "%i %i \"%s\"\n", 
						((struct gclient_new_s *)(cl->edict->client))->ps.stats[STAT_FRAGS], cl->ping, cl->name);
	#else
					Com_sprintf (player, sizeof(player), "%i %i \"%s\"\n", 
						((struct gclient_old_s *)(cl->edict->client))->ps.stats[STAT_FRAGS], cl->ping, cl->name);
	#endif
				playerLength = strlen(player);
				if (statusLength + playerLength >= sizeof(status) )
					break;		// can't hold any more
				strcpy (status + statusLength, player);
				statusLength += playerLength;
			}
		}
	}

	return status;
}

qboolean RateLimited (ratelimit_t limit, netadr_t *from)
{
	return false;
}

/*
================
SVC_Status

Responds with all the info that qplug or qspy can see
================
*/
void SVC_Status (void)
{
	if (sv_hidestatus->value)
		return;

	//if (RateLimited (sv.ratelimit_status, net_from);

	Netchan_OutOfBandPrint (NS_SERVER, &net_from, "print\n%s", SV_StatusString());
}

/*
================
SVC_Ack

================
*/
void SVC_Ack (void)
{
	int i;
	//r1: could be used to flood server console - only show acks from masters.

	for (i=0 ; i<MAX_MASTERS ; i++) {
		if (master_adr[i].port && NET_CompareBaseAdr (&master_adr[i], &net_from)) {
			Com_Printf ("Ping acknowledge from %s\n", NET_AdrToString(&net_from));
			break;
		}
	}
}

/*
================
SVC_Info

Responds with short info for broadcast scans
The second parameter should be the current protocol version number.
================
*/
void SVC_Info (void)
{
	char	string[64];
	int		i, count;
	int		version;

	if (maxclients->value == 1)
		return;		// ignore in single player

	version = atoi (Cmd_Argv(1));

	if (version != ORIGINAL_PROTOCOL_VERSION && version != ENHANCED_PROTOCOL_VERSION)
	{
		//r1: return instead of sending another packet. prevents spoofed udp packet
		//    causing server <-> server info loops.
		return;
	}
	else
	{
		count = 0;
		for (i=0 ; i<maxclients->value ; i++)
			if (svs.clients[i].state >= cs_connected)
				count++;

		Com_sprintf (string, sizeof(string), "%20s %8s %2i/%2i\n", hostname->string, sv.name, count, (int)(maxclients->value - sv_reserved_slots->value));
	}

	Netchan_OutOfBandPrint (NS_SERVER, &net_from, "info\n%s", string);
}

/*
================
SVC_Ping

Just responds with an acknowledgement
================
*/
void SVC_Ping (void)
{
	Netchan_OutOfBandPrint (NS_SERVER, &net_from, "ack");
}


/*
=================
SVC_GetChallenge

Returns a challenge number that can be used
in a subsequent client_connect command.
We do this to prevent denial of service attacks that
flood the server with invalid connection IPs.  With a
challenge, they must give a valid IP address.
=================
*/

void SVC_GetChallenge (void)
{
	int		i;
	int		oldest = 0;
	unsigned int		oldestTime = 0xffffffff;

	// see if we already have a challenge for this ip
	for (i = 0 ; i < MAX_CHALLENGES ; i++)
	{
		if (NET_CompareBaseAdr (&net_from, &svs.challenges[i].adr))
			break;

		if (svs.challenges[i].time < oldestTime)
		{
			oldestTime = svs.challenges[i].time;
			oldest = i;
		}
	}

	if (i == MAX_CHALLENGES)
	{
		// overwrite the oldest
		svs.challenges[oldest].challenge = randomMT()&0x7FFFFFFF;
		svs.challenges[oldest].adr = net_from;
		svs.challenges[oldest].time = curtime;
		i = oldest;
	} else {
		svs.challenges[i].challenge = randomMT()&0x7FFFFFFF;
		svs.challenges[i].time = curtime;
	}

	// send it back
	Netchan_OutOfBandPrint (NS_SERVER, &net_from, "challenge %d", svs.challenges[i].challenge);
}

qboolean CheckUserInfoFields (char *userinfo)
{
	if (!Info_KeyExists (userinfo, "name")) /* || !Info_KeyExists (userinfo, "rate") 
		|| !Info_KeyExists (userinfo, "msg") || !Info_KeyExists (userinfo, "hand")
		|| !Info_KeyExists (userinfo, "skin"))*/
		return false;

	return true;
}

/*
==================
SVC_DirectConnect

A connection request that did not come from the master
==================
*/
void SVC_DirectConnect (void)
{
	char		userinfo[MAX_INFO_STRING];
	netadr_t	*adr;
	int			i;
	client_t	*cl, *newcl;
	//client_t	temp;
	edict_t		*ent;
	int			edictnum;
	int			version;
	//int			qport;
	int			challenge;
	int			previousclients;
	float		reserved;
	char		*pass;

	adr = &net_from;

	Com_DPrintf ("SVC_DirectConnect ()\n");

	//r1: check version first of all
	version = atoi(Cmd_Argv(1));
	if (version != ORIGINAL_PROTOCOL_VERSION && version != ENHANCED_PROTOCOL_VERSION)
	{
		Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nYou need Quake II 3.20 or above to play on this server (protocol >= 34).\n");
		Com_DPrintf ("    rejected connect from version %i\n", version);
		return;
	}

	challenge = atoi(Cmd_Argv(3));

	// see if the challenge is valid
	if (!NET_IsLocalHost (adr))
	{
		for (i=0 ; i<MAX_CHALLENGES ; i++)
		{
			if (svs.challenges[i].challenge && NET_CompareBaseAdr (&net_from, &svs.challenges[i].adr))
			{
				//r1: reset challenge
				if (challenge == svs.challenges[i].challenge) {
					svs.challenges[i].challenge = 0;
					break;
				}
				Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nBad challenge.\n");
				return;
			}
		}
		if (i == MAX_CHALLENGES)
		{
			Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nNo challenge for address.\n");
			return;
		}
	}

	//r1: deny if server is locked
	if (sv_locked->value)
	{
		Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nServer is locked.\n");
		return;
	}

	//r1: limit connections from a single IP
	previousclients = 0;
	for (i=0,cl=svs.clients ; i<maxclients->value ; i++,cl++)
	{
		if (cl->state == cs_free)
			continue;
		if (NET_CompareBaseAdr (adr, &cl->netchan.remote_address))
			previousclients++;
	}

	if (previousclients >= sv_iplimit->value) {
		Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nToo many connections from your host.\n");
		Com_DPrintf ("too many connections from %s\n", NET_AdrToString (adr));
		return;
	}

	//qport = atoi(Cmd_Argv(2));
	strncpy (userinfo, Cmd_Argv(4), sizeof(userinfo)-1);

	//r1: check it is not overflowed, save enough bytes for /ip/111.222.333.444
	if (strlen(userinfo) + 19 >= sizeof(userinfo)-1)
	{
		Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nUserinfo string length exceeded.\n");
		return;
	}

	//r1ch: ban anyone trying to use the end-of-message-in-string exploit
	if (strstr(userinfo, "\xFF"))
	{
		char *ptr;
		ptr = strstr (userinfo, "\xFF");
		ptr -= 8;
		if (ptr < userinfo)
			ptr = userinfo;
		Blackhole (&net_from, "0xFF in userinfo (%.32s)", MakePrintable(ptr));
		Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nConnection refused.\n");
		return;
	}

	if (sv_strict_userinfo_check->value)
	{
		if (!Info_CheckBytes (userinfo))
		{
			Com_Printf ("Warning, Info_CheckBytes failed for %s!\n", NET_AdrToString (adr));
			Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nUserinfo contains illegal bytes.\n");
			return;
		}
	}

	//r1ch: allow filtering of stupid "fun" names etc.
	if (sv_filter_userinfo->value)
		strncpy (userinfo, StripHighBits(userinfo, (int)sv_filter_userinfo->value == 2), sizeof(userinfo)-1);

 	userinfo[sizeof(userinfo) - 1] = 0;

	//r1ch: simple userinfo validation
	/*if (!CheckUserInfoFields(userinfo)) {
		Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nMalformed userinfo string.\n");
		Com_DPrintf ("illegal userinfo from %s\n", NET_AdrToString (adr));
		return;
	}*/

	if (!*Info_ValueForKey (userinfo, "name")) {
		Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nPlease set your name before connecting.\n");
		return;
	} else if (NameColorFilterCheck (Info_ValueForKey (userinfo, "name"))) {
		Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nQuake 3 style colored names are not permitted on this server.\n");
		return;
	} else if (Info_KeyExists (userinfo, "ip")) {
		Blackhole (&net_from, "attempted to spoof ip '%s'", Info_ValueForKey(userinfo, "ip"));
		Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nConnection refused.\n");
		return;
	}

	pass = Info_ValueForKey (userinfo, "password");

	if (*sv_password->string && strcmp(sv_password->string, "none"))
	{
		if (!*pass) {
			Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nPassword required.\n");
			return;
		} else if (strcmp (pass, sv_password->string)) {
			Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nInvalid password.\n");
			Com_DPrintf ("bad password from %s\n", NET_AdrToString (adr));
			return;
		}
	}

	// force the IP key/value pair so the game can filter based on ip
	Info_SetValueForKey (userinfo, "ip", NET_AdrToString(&net_from));

	// attractloop servers are ONLY for local clients
	// r1: demo serving anyone?
	/*if (sv.attractloop)
	{
		if (!NET_IsLocalAddress (adr))
		{
			Com_Printf ("Remote connect in attract loop.  Ignored.\n");
			Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nConnection refused.\n");
			return;
		}
	}*/


	//newcl = &temp;
//	memset (newcl, 0, sizeof(client_t));

	// if there is already a slot for this ip, reuse it
	for (i=0,cl=svs.clients ; i < maxclients->value; i++,cl++)
	{
		if (cl->state == cs_free)
			continue;

		if (NET_CompareAdr (adr, &cl->netchan.remote_address))
		{
			//r1: !! fix nasty bug where non-disconnected clients (from dropped disconnect
			//packets) could be overwritten!
			if (cl->state != cs_zombie)
				return;

			//r1: not needed
			/*if (!NET_IsLocalAddress (adr) && (svs.realtime - cl->lastconnect) < ((int)sv_reconnect_limit->value * 1000))
			{
				Com_DPrintf ("%s:reconnect rejected : too soon\n", NET_AdrToString (adr));
				return;
			}*/
			Com_DPrintf ("%s:reconnect\n", NET_AdrToString (adr));

			if (cl->lastlines)
				Com_Error (ERR_DROP, "cl->lastlines during reconnect!");

			newcl = cl;
			goto gotnewcl;
		}
	}

	if (!strcmp (pass, sv_reserved_password->string))
		reserved = 0;
	else
		reserved = sv_reserved_slots->value;

	// find a client slot
	newcl = NULL;
	for (i=0,cl=svs.clients ; i<(maxclients->value - reserved); i++,cl++)
	{
		if (cl->state == cs_free)
		{
			newcl = cl;
			break;
		}
	}

	if (!newcl)
	{
		if (reserved)
			Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nServer and reserved slots are full.\n");
		else
			Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nServer is full.\n");
		Com_DPrintf ("Rejected a connection.\n");
		return;
	}

gotnewcl:	
	// build a new connection
	// accept the new client
	// this is the only place a client_t is ever initialized
	
	sv_client = newcl;

	if (newcl->lastlines)
		Com_Error (ERR_DROP, "newcl->lastlines on SVC_DirectConnect!");

	memset (newcl, 0, sizeof(*newcl));

	edictnum = (newcl-svs.clients)+1;
	ent = EDICT_NUM(edictnum);
	newcl->edict = ent;
	newcl->challenge = challenge; // save challenge for checksumming

	if (Cmd_Argc() != 5)
	{
		Com_Printf ("Warning, unknown number of connection arguments from %s -- %d\n", NET_AdrToString (adr), Cmd_Argc());
	}

	// get the game a chance to reject this connection or modify the userinfo
	if (!(ge->ClientConnect (ent, userinfo)))
	{
		if (*Info_ValueForKey (userinfo, "rejmsg")) 
			Netchan_OutOfBandPrint (NS_SERVER, adr, "print\n%s\nConnection refused.\n",  
				Info_ValueForKey (userinfo, "rejmsg"));
		else
			Netchan_OutOfBandPrint (NS_SERVER, adr, "print\nConnection refused.\n" );
		Com_DPrintf ("Game rejected a connection.\n");
		return;
	}

	// parse some info from the info strings
	strncpy (newcl->userinfo, userinfo, sizeof(newcl->userinfo)-1);
	SV_UserinfoChanged (newcl);

	//userinfo validation kicked them
	if (newcl->state == cs_zombie)
		return;

	// send the connect packet to the client
	Netchan_OutOfBandPrint (NS_SERVER, adr, "client_connect");

	Netchan_Setup (NS_SERVER, &newcl->netchan, adr, version, 0);

	if (*sv_connectmessage->string)
	{
		if (sv_connectmessage->modified)
		{
			char *q = sv_connectmessage->string;
			char *s = q;
			while (*(q+1))
			{
				if (*q == '\\' && *(q+1) == 'n')
				{
					*s++ = '\n';
					q++;
				}
				else
				{
					*s++ = *q;
				}
				q++;
			}
			*s++ = *q;
			*s = '\0';
			sv_connectmessage->modified = false;
		}
		Netchan_OutOfBandPrint (NS_SERVER, adr, "print\n%s", sv_connectmessage->string);
	}

	newcl->protocol = version;
	newcl->state = cs_connected;

	newcl->lastlines = Z_TagMalloc (sizeof(entity_state_t) * MAX_EDICTS, TAGMALLOC_CL_BASELINES);

//	SV_CreateBaseline (newcl);
	
	SZ_Init (&newcl->datagram, newcl->datagram_buf, sizeof(newcl->datagram_buf) );
	newcl->datagram.allowoverflow = true;

	//r1ch: give them 5 secs to respond to the client_connect. since client_connect isn't
	//sent via netchan (its connectionless therefore unreliable), a dropped client_connect
	//will force a client to wait for the entire timeout->value before being allowed to try
	//and reconnect.
	//newcl->lastmessage = svs.realtime;	// don't timeout

	newcl->lastmessage = svs.realtime - ((timeout->value - 5) * 1000);
	//newcl->lastconnect = svs.realtime;
}

int Rcon_Validate (void)
{
	if (!strlen (rcon_password->string))
		return 0;

	if (strcmp (Cmd_Argv(1), rcon_password->string) )
		return 0;

	return 1;
}

void Blackhole (netadr_t *from, char *fmt, ...)
{
	blackhole_t *temp;
	va_list		argptr;

	if (!sv_blackholes->value)
		return;

	temp = &blackholes;
	while (temp->next)
		temp = temp->next;

	temp->next = Z_TagMalloc(sizeof(blackhole_t), TAGMALLOC_BLACKHOLE);
	temp = temp->next;
	temp->next = NULL;
	temp->netadr = *from;

	va_start (argptr,fmt);
	vsnprintf (temp->reason, sizeof(temp->reason)-1, fmt, argptr);
	va_end (argptr);

	Com_Printf ("Added %s to blackholes for %s.\n", NET_AdrToString (from), temp->reason);
	return;
}

qboolean UnBlackhole (int index)
{
	blackhole_t *temp, *last;
	int i;

	i = index;

	last = temp = &blackholes;
	while (temp->next)
	{
		last = temp;
		temp = temp->next;
		i--;
		if (i == 0)
			break;
		else if (i < 0)
			return false;
	}

	// if list ended before we could get to index
	if (i > 0)
		return false;

	// just copy the next over, don't care if it's null
	last->next = temp->next;

	Z_Free (temp);

	return true;
}

/*
===============
SVC_RemoteCommand

A client issued an rcon command.
Shift down the remaining args
Redirect all printfs
===============
*/
void SVC_RemoteCommand (void)
{
	static int last_rcon_time = 0;
	int		i;
	char	remaining[2048];

	//note, have to verify curtime didn't wrap!!
	if (last_rcon_time > curtime && abs(last_rcon_time - curtime) <= 500) {
		//Com_DPrintf ("dropped rcon, too fast\n");
		return;
	} else {
		last_rcon_time = curtime + 500;
	}

	i = Rcon_Validate ();

	Com_BeginRedirect (RD_PACKET, sv_outputbuf, SV_OUTPUTBUF_LENGTH, SV_FlushRedirect);

	if (!i)
	{
		Com_Printf ("Bad rcon_password.\n");
		Com_EndRedirect ();
		Com_Printf ("Bad rcon from %s (%s).\n", NET_AdrToString (&net_from), Cmd_Args());
	}
	else
	{
		remaining[0] = 0;

		//hack to allow clients to send rcon set commands properly
		if (!Q_stricmp (Cmd_Argv(2), "set")) {
			char setvar[2048];
			qboolean serverinfo = false;
			int params;

			setvar[0] = 0;
			params = Cmd_Argc();

			if (!Q_stricmp (Cmd_Argv(params-1), "s")) {
				serverinfo = true;
				params--;
			}

			for (i=4 ; i<params ; i++)
			{
				strcat (setvar, Cmd_Argv(i) );
				if (i+1 != params)
					strcat (setvar, " ");
			}

			Com_sprintf (remaining, sizeof(remaining), "set %s \"%s\"%s", Cmd_Argv(3), setvar, serverinfo ? " s" : "");
		} else {
			for (i=2 ; i<Cmd_Argc() ; i++)
			{
				if (strlen(remaining) + (strlen(Cmd_Argv(i)) + 3) > sizeof(remaining))
				{
					Com_Printf ("Rcon string length exceeded, discarded.\n");
					remaining[0] = 0;
					break;
				}
				strcat (remaining, Cmd_Argv(i) );
				if (i+1 != Cmd_Argc())
					strcat (remaining, " ");
			}
		}

		Cmd_ExecuteString (remaining);
		Com_EndRedirect ();
		Com_Printf ("Rcon from %s:\n%s\n", NET_AdrToString (&net_from), remaining);
	}

	//check for remote kill
	if (!svs.initialized)
		Com_Error (ERR_DROP, "server killed via rcon");
}

#ifdef USE_PYROADMIN
void SVC_PyroAdminCommand (char *s)
{
	if (pyroadminid && atoi(Cmd_Argv(1)) == pyroadminid)
	{
		char	*p = s;
		char	remaining[1024];

		memset (remaining, 0, sizeof(remaining));

		while (*p && *p != ' ')
			p++;

		p++;

		while (*p && *p != ' ')
			p++;

		p++;

		Com_Printf ("pyroadmin>> %s\n", p);
		Cmd_ExecuteString (p);
	}
}
#endif

/*
=================
SV_ConnectionlessPacket

A connectionless packet has four leading 0xff
characters to distinguish it from a game channel.
Clients that are in the game can still send
connectionless packets.
=================
*/
void SV_ConnectionlessPacket (void)
{
	blackhole_t *blackhole = &blackholes;
	char	*s;
	char	*c;

	//r1: ignore packets if IP is blackholed for abuse
	while (blackhole->next)
	{
		blackhole = blackhole->next;
		if (NET_CompareBaseAdr (&net_from, &blackhole->netadr))
			return;
	}

	//r1: should never happen, don't even bother trying to parse it
	if (net_message.cursize > 544)
	{
		Com_DPrintf ("dropped %d byte connectionless packet from %s\n", net_message.cursize, NET_AdrToString (&net_from));
		return;
	}

	//r1: make sure we never talk to ourselves
	if (NET_IsLocalAddress (&net_from) && !NET_IsLocalHost(&net_from) && ShortSwap(net_from.port) == server_port)
	{
		Com_DPrintf ("dropped %d byte connectionless packet from self! (spoofing attack?)\n", net_message.cursize);
		return;
	}

	MSG_BeginReading (&net_message);
	MSG_ReadLong (&net_message);		// skip the -1 marker

	s = MSG_ReadStringLine (&net_message);

	Cmd_TokenizeString (s, false);

	c = Cmd_Argv(0);
	Com_DPrintf ("Packet %s : %s\n", NET_AdrToString(&net_from), c);

	if (!strcmp(c, "ping"))
		SVC_Ping ();
	else if (!strcmp(c,"status"))
		SVC_Status ();
	else if (!strcmp(c,"info"))
		SVC_Info ();
	else if (!strcmp(c,"getchallenge"))
		SVC_GetChallenge ();
	else if (!strcmp(c,"connect"))
		SVC_DirectConnect ();
	else if (!strcmp(c, "rcon"))
		SVC_RemoteCommand ();
	else if (!strcmp(c, "ack"))
		SVC_Ack ();
#ifdef USE_PYROADMIN
	else if (!strcmp (c, "cmd"))
		SVC_PyroAdminCommand (s);
#endif
	else
		Com_DPrintf ("bad connectionless packet from %s:\n%s\n"
		, NET_AdrToString (&net_from), s);
}


//============================================================================

/*
===================
SV_CalcPings

Updates the cl->ping variables
===================
*/
void SV_CalcPings (void)
{
	int			i, j;
	client_t	*cl;
	int			total, count;
	int			best;
	int			method;

	method = sv_calcpings_method->value;

	for (i=0 ; i<maxclients->value ; i++)
	{
		cl = &svs.clients[i];
		if (cl->state != cs_spawned )
			continue;

		if (method == 1)
		{
			total = 0;
			count = 0;
			for (j=0 ; j<LATENCY_COUNTS ; j++)
			{
				if (cl->frame_latency[j] > 0)
				{
					count++;
					total += cl->frame_latency[j];
				}
			}
			if (!count)
				cl->ping = 0;
			else
				cl->ping = total / count;
		}
		else if (method == 2)
		{
			best = 9999;
			for (j=0 ; j<LATENCY_COUNTS ; j++)
			{
				if (cl->frame_latency[j] > 0 && cl->frame_latency[j] < best)
				{
					best = cl->frame_latency[j];
				}
			}

			cl->ping = best;
		}
		else
		{
			cl->ping = 0;
		}

		// let the game dll know about the ping
#ifdef ENHANCED_SERVER
		((struct gclient_new_s *)(cl->edict->client))->ping = cl->ping;
#else
		((struct gclient_old_s *)(cl->edict->client))->ping = cl->ping;
#endif
	}
}


/*
===================
SV_GiveMsec

Every few frames, gives all clients an allotment of milliseconds
for their command moves.  If they exceed it, assume cheating.
===================
*/
void SV_GiveMsec (void)
{
	int			i;
	client_t	*cl;

	if (sv.framenum & 15)
		return;

	for (i=0 ; i<maxclients->value ; i++)
	{
		cl = &svs.clients[i];
		if (cl->state == cs_free )
			continue;

		if (cl->state == cs_spawned)
		{
			//r1: better? speed cheat detection via use of msec underflows
			if (cl->commandMsec < 0)
			{
#ifndef _DEBUG
				//don't spam listen servers in release
				if (!Com_ServerState() && dedicated && !dedicated->value)
#endif
					Com_Printf ("%s commandMsec < 0: %d (possible speed cheat?)\n", cl->name, cl->commandMsec);
				cl->commandMsecOverflowCount++;
			}
			else if (cl->commandMsec == sv_msecs->value)
			{
				Com_DPrintf ("%s didn't use any commandMsec (lagged out?)\n", cl->name);
			}
			else if (cl->commandMsec > 800)
			{
				Com_DPrintf ("%s commandMsec > 800: %d (possible lag/float cheat?)\n", cl->name, cl->commandMsec);
			}
			else
			{
				cl->commandMsecOverflowCount *= 0.94;
			}

			if (cl->commandMsecOverflowCount > 0.05)
				Com_DPrintf ("%s has %.2f overflowCount\n", cl->name, cl->commandMsecOverflowCount);

			if (sv_enforcetime->value > 1 && cl->commandMsecOverflowCount >= 10)
			{
				SV_KickClient (cl, "possible speed cheat detected", "You were kicked from the game.\n");
				continue;
			}
		}

		cl->commandMsec = sv_msecs->value;		// 1600 + some slop
	}
}


/*
=================
SV_ReadPackets
=================
*/
void SV_ReadPackets (void)
{
	int			i, j;
	client_t	*cl;
	//int			qport;

	//Com_Printf ("ReadPackets\n");
	for (;;)
	{
		j = NET_GetPacket (NS_SERVER, &net_from, &net_message);
		if (!j)
			break;

		if (j == -1) {
			// check for packets from connected clients
			for (i=0, cl=svs.clients ; i<maxclients->value ; i++,cl++)
			{
				if (cl->state <= cs_zombie)
					continue;

				if (!NET_CompareAdr (&net_from, &cl->netchan.remote_address))
					continue;

				SV_KickClient (cl, "connection reset by peer", NULL);
				break;
			}
			continue;
		}

		// check for connectionless packet (0xffffffff) first
		if (*(int *)net_message.data == -1)
		{
			SV_ConnectionlessPacket ();
			continue;
		}

		// read the qport out of the message so we can fix up
		// stupid address translating routers
		//MSG_BeginReading (&net_message);
		//MSG_ReadLong (&net_message);		// sequence number
		//MSG_ReadLong (&net_message);		// sequence number
		
		//MSG_ReadShort (&net_message);

		// check for packets from connected clients
		for (i=0, cl=svs.clients ; i<maxclients->value ; i++,cl++)
		{
			if (cl->state == cs_free)
				continue;

			if (!NET_CompareAdr (&net_from, &cl->netchan.remote_address))
				continue;

			/*if (cl->netchan.qport != qport)
				continue
			if (cl->netchan.remote_address.port != net_from.port)
			{
				//Com_Printf ("SV_ReadPackets: fixing up a translated port\n");
				cl->netchan.remote_address.port = net_from.port;
			}*/

			if (Netchan_Process(&cl->netchan, &net_message))
			{	// this is a valid, sequenced packet, so process it
				if (cl->state != cs_zombie)
				{
					cl->lastmessage = svs.realtime;	// don't timeout
					SV_ExecuteClientMessage (cl);
					cl->packetCount++;

				}
			}
			break;
		}
		
		
		/* r1: a little pointless?
		if (i != maxclients->value)
			continue;
		*/
	}
}

/*
==================
SV_CheckTimeouts

If a packet has not been received from a client for timeout->value
seconds, drop the conneciton.  Server frames are used instead of
realtime to avoid dropping the local client while debugging.

When a client is normally dropped, the client_t goes into a zombie state
for a few seconds to make sure any final reliable message gets resent
if necessary
==================
*/
void SV_CheckTimeouts (void)
{
	int		i;
	client_t	*cl;
	int			droppoint;
	int			zombiepoint;
	int			flagRun = 0;

	droppoint = svs.realtime - 1000*timeout->value;
	zombiepoint = svs.realtime - 1000*zombietime->value;

	if (sv.framenum % 50 == 0)
		flagRun = 1;

	for (i=0,cl=svs.clients ; i<maxclients->value ; i++,cl++)
	{
		// message times may be wrong across a changelevel
		if (cl->lastmessage > svs.realtime)
			cl->lastmessage = svs.realtime;

		if (cl->state == cs_zombie && cl->lastmessage < zombiepoint)
		{
			cl->state = cs_free;	// can now be reused
			continue;
		}

		if (cl->state == cs_connected || cl->state == cs_spawned) 
		{
			if (flagRun)
			{
				cl->fps = (int)((float)cl->packetCount / 5.0);

				//r1ch: fps spam protection
				if (sv_fpsflood->value && cl->fps > sv_fpsflood->value)
					SV_KickClient (cl, va("too many packets/sec (%d)", cl->fps), "You were kicked from the game.\n(Tip: try lowering your cl_maxfps to avoid flooding the server)\n");

				cl->packetCount = 0;
			}

			if (cl->lastmessage < droppoint)
			{
				//r1: only message if they spawned (less spam plz)
				if (cl->state == cs_spawned && *cl->name)
					SV_BroadcastPrintf (PRINT_HIGH, "%s timed out\n", cl->name);
				SV_DropClient (cl); 
				cl->state = cs_free;	// don't bother with zombie state
			}
			/*else if (cl->lastmessage < (droppoint+2500))
			{
				if (sv.framenum & 1)
					cl->edict->s.effects |= EF_SPHERETRANS;
			}*/
		}
	}
}

/*
================
SV_PrepWorldFrame

This has to be done before the world logic, because
player processing happens outside RunWorldFrame
================
*/
void SV_PrepWorldFrame (void)
{
	edict_t	*ent;
	int		i;

	for (i=0 ; i<ge->num_edicts ; i++)
	{
		ent = EDICT_NUM(i);
		// events only last for a single message
		ent->s.event = 0;
	}

}

void SV_RunDownloadServer (void)
{

}

/*
=================
SV_RunGameFrame
=================
*/
void SV_RunGameFrame (void)
{
#ifndef DEDICATED_ONLY
	if (host_speeds->value)
		time_before_game = Sys_Milliseconds ();
#endif

	// we always need to bump framenum, even if we
	// don't run the world, otherwise the delta
	// compression can get confused when a client
	// has the "current" frame
	sv.framenum++;
	sv.time = sv.framenum*100;

	// don't run if paused
	if (!sv_paused->value || maxclients->value > 1)
	{
		ge->RunFrame ();

		// never get more than one tic behind
		if (sv.time < svs.realtime)
		{
#ifndef DEDICATED_ONLY
			if (sv_showclamp->value)
				Com_Printf ("sv highclamp\n");
#endif
			svs.realtime = sv.time;
		}
	}

#ifndef DEDICATED_ONLY
	if (host_speeds->value)
		time_after_game = Sys_Milliseconds ();
#endif

}

/*
==================
SV_Frame

==================
*/
void SV_Frame (int msec)
{
#ifndef DEDICATED_ONLY
	time_before_game = time_after_game = 0;
#endif

	// if server is not active, do nothing
	if (!svs.initialized)
	{
		Sys_Sleep (1);
		return;
	}

    svs.realtime += msec;

	// keep the random time dependent
	//rand ();

	// get packets from clients
	SV_ReadPackets ();

	// move autonomous things around if enough time has passed
	if (!sv_timedemo->value && svs.realtime < sv.time)
	{
		// never let the time get too far off
		if (sv.time - svs.realtime > 100)
		{
#ifndef DEDICATED_ONLY
			if (sv_showclamp->value)
				Com_Printf ("sv lowclamp\n");
#endif
			svs.realtime = sv.time - 100;
		}
		NET_Sleep(sv.time - svs.realtime);
		return;
	}

	//r1: run tcp downloads
	SV_RunDownloadServer ();

	// update ping based on the last known frame from all clients
	SV_CalcPings ();

	// give the clients some timeslices
	SV_GiveMsec ();

	// let everything in the world think and move
	SV_RunGameFrame ();

	// check timeouts
	SV_CheckTimeouts ();

	// send messages back to the clients that had packets read this frame
	SV_SendClientMessages ();

	// save the entire world state if recording a serverdemo
	SV_RecordDemoMessage ();

	// send a heartbeat to the master if needed
	Master_Heartbeat ();

	// clear teleport flags, etc for next frame
	SV_PrepWorldFrame ();

/*	//r1: update server console
#ifdef WIN32
	Sys_UpdateConsoleBuffer();
#endif*/
}

//============================================================================

/*
================
Master_Heartbeat

Send a message to the master every few minutes to
let it know we are alive, and log information
================
*/

void Master_Heartbeat (void)
{
	char		*string;
	int			i;

	if (!dedicated->value)
		return;		// only dedicated servers send heartbeats

	if (!public_server->value)
		return;		// a private dedicated game

	// check for time wraparound
	if (svs.last_heartbeat > svs.realtime)
		svs.last_heartbeat = svs.realtime;

	if (svs.realtime - svs.last_heartbeat < HEARTBEAT_SECONDS*1000)
		return;		// not time to send yet

	svs.last_heartbeat = svs.realtime;

	// send the same string that we would give for a status OOB command
	string = SV_StatusString();

	// send to group master
	for (i=0 ; i<MAX_MASTERS ; i++)
		if (master_adr[i].port)
		{
			Com_Printf ("Sending heartbeat to %s\n", NET_AdrToString (&master_adr[i]));
			Netchan_OutOfBandPrint (NS_SERVER, &master_adr[i], "heartbeat\n%s", string);
		}
}

/*
=================
Master_Shutdown

Informs all masters that this server is going down
=================
*/
void Master_Shutdown (void)
{
	int			i;

	//r1: server died before cvars had a chance to init!
	if (!dedicated)
		return;

	if (!dedicated->value)
		return;		// only dedicated servers send heartbeats

	if (public_server && !public_server->value)
		return;		// a private dedicated game

	// send to group master
	for (i=0 ; i<MAX_MASTERS ; i++)
		if (master_adr[i].port)
		{
			if (i > 0)
				Com_Printf ("Sending shutdown to %s\n", NET_AdrToString (&master_adr[i]));
			Netchan_OutOfBandPrint (NS_SERVER, &master_adr[i], "shutdown");
		}
}

//============================================================================


/*
=================
SV_UserinfoChanged

Pull specific info from a newly changed userinfo string
into a more C freindly form.
=================
*/
void SV_UserinfoChanged (client_t *cl)
{
	char	*val;
	int		i;

	//r1ch: ban anyone trying to use the end-of-message-in-string exploit
	if (strstr (cl->userinfo, "\xFF"))
	{
		char *ptr;
		ptr = strstr (cl->userinfo, "\xFF");
		ptr -= 8;
		if (ptr < cl->userinfo)
			ptr = cl->userinfo;
		Blackhole (&cl->netchan.remote_address, "0xFF in userinfo (%.32s)", MakePrintable (ptr));
		SV_KickClient (cl, "illegal userinfo", NULL);
		return;
	}

	if (sv_strict_userinfo_check->value)
	{
		if (!Info_CheckBytes (cl->userinfo))
		{
			SV_KickClient (cl, "illegal userinfo", "Userinfo contains illegal bytes.\n");
			return;
		}
	}

	//r1ch: allow filtering of stupid "fun" names etc.
	if (sv_filter_userinfo->value)
		strncpy (cl->userinfo, StripHighBits(cl->userinfo, (int)sv_filter_userinfo->value == 2), sizeof(cl->userinfo)-1);

	val = Info_ValueForKey (cl->userinfo, "name");

	if (NameColorFilterCheck (val))
	{
		SV_ClientPrintf (cl, PRINT_HIGH, "Invalid name '%s'\n", val);
		if (*cl->name)
		{
			MSG_BeginWriteByte (&cl->netchan.message, svc_stufftext);
			MSG_WriteString (&cl->netchan.message, va("set name \"%s\"\n", cl->name));
			Info_SetValueForKey (cl->userinfo, "name", cl->name);
		}
		else
		{
			Com_DPrintf ("dropping %s for malformed userinfo (%s)\n", cl->name, cl->userinfo);
			SV_KickClient (cl, "bad userinfo", NULL);
		}
	}

	//r1ch: drop clients who hack userinfo to miss required sections
	if (cl->state >= cs_connected)
	{
		if (!CheckUserInfoFields(cl->userinfo))
		{
			if (*cl->name)
			{
				MSG_BeginWriteByte (&cl->netchan.message, svc_stufftext);
				MSG_WriteString (&cl->netchan.message, va("set name \"%s\"\n", cl->name));
				Info_SetValueForKey (cl->userinfo, "name", cl->name);
			}
			else
			{
				Com_DPrintf ("dropping %s for malformed userinfo (%s)\n", cl->name, cl->userinfo);
				SV_KickClient (cl, "bad userinfo", NULL);
			}
		}

		if (Info_KeyExists (cl->userinfo, "ip"))
		{
			Com_DPrintf ("dropping %s for attempted IP spoof (%s)\n", cl->name, cl->userinfo);
			SV_KickClient (cl, "bad userinfo", NULL);
		}
	}

	// call prog code to allow overrides
	ge->ClientUserinfoChanged (cl->edict, cl->userinfo);

	//r1: notify console
	if (strcmp (cl->name, val))
		Com_Printf ("%s[%s] changed name to %s.\n", cl->name, NET_AdrToString (&cl->netchan.remote_address), val);
	
	// name for C code
	strncpy (cl->name, val, sizeof(cl->name)-1);

	// mask off high bit
	for (i=0 ; i<sizeof(cl->name) ; i++)
		cl->name[i] &= 127;

	// rate command
	val = Info_ValueForKey (cl->userinfo, "rate");
	if (strlen(val))
	{
		i = atoi(val);
		cl->rate = i;
		if (cl->rate < 100)
			cl->rate = 100;
		if (cl->rate > 15000)
			cl->rate = 15000;
	}
	else
		cl->rate = 5000;

	// msg command
	val = Info_ValueForKey (cl->userinfo, "msg");
	if (strlen(val))
	{
		cl->messagelevel = atoi(val);
	}
}

void SV_UpdateWindowTitle (cvar_t *cvar, char *old, char *new)
{
	if (dedicated->value)
	{
		char buff[512];
		Com_sprintf (buff, sizeof(buff)-1, "%s - R1Q2 " VERSION " (port %d)", new, server_port);

		//for win32 this will set window titlebar text
		//for linux this currently does nothing
		Sys_SetWindowText (buff);
	}
}

/*
===============
SV_Init

Only called at quake2.exe startup, not for each game
===============
*/

void SV_Init (void)
{
	SV_InitOperatorCommands	();

	rcon_password = Cvar_Get ("rcon_password", "", 0);

	Cvar_Get ("skill", "1", 0);

	//r1: default 1
	Cvar_Get ("deathmatch", "1", CVAR_LATCH);
	Cvar_Get ("coop", "0", CVAR_LATCH);
	Cvar_Get ("dmflags", va("%i", DF_INSTANT_ITEMS), CVAR_SERVERINFO);
	Cvar_Get ("fraglimit", "0", CVAR_SERVERINFO);
	Cvar_Get ("timelimit", "0", CVAR_SERVERINFO);
	Cvar_Get ("cheats", "0", CVAR_SERVERINFO|CVAR_LATCH);
	Cvar_Get ("protocol", va("%i", ENHANCED_PROTOCOL_VERSION), CVAR_NOSET);

	//r1: default 8
	maxclients = Cvar_Get ("maxclients", "8", CVAR_SERVERINFO | CVAR_LATCH);

	hostname = Cvar_Get ("hostname", "Unnamed R1Q2 Server", CVAR_SERVERINFO | CVAR_ARCHIVE);

	//r1: update window title on the fly
	hostname->changed = SV_UpdateWindowTitle;
	hostname->changed (hostname, hostname->string, hostname->string);

	//r1: dropped to 90 from 125
	timeout = Cvar_Get ("timeout", "90", 0);

	//r1: bumped to 3 from 2
	zombietime = Cvar_Get ("zombietime", "3", 0);

#ifndef DEDICATED_ONLY
	sv_showclamp = Cvar_Get ("showclamp", "0", 0);
#endif

	sv_paused = Cvar_Get ("paused", "0", 0);
	sv_timedemo = Cvar_Get ("timedemo", "0", 0);

	//r1: default 1
	sv_enforcetime = Cvar_Get ("sv_enforcetime", "1", 0);

#ifndef NO_SERVER
	allow_download = Cvar_Get ("allow_download", "0", CVAR_ARCHIVE);
	allow_download_players  = Cvar_Get ("allow_download_players", "3", CVAR_ARCHIVE);
	allow_download_models = Cvar_Get ("allow_download_models", "3", CVAR_ARCHIVE);
	allow_download_sounds = Cvar_Get ("allow_download_sounds", "3", CVAR_ARCHIVE);
	allow_download_maps	  = Cvar_Get ("allow_download_maps", "3", CVAR_ARCHIVE);
#endif

	sv_noreload = Cvar_Get ("sv_noreload", "0", 0);

	sv_airaccelerate = Cvar_Get("sv_airaccelerate", "0", CVAR_LATCH);

	public_server = Cvar_Get ("public", "0", 0);

	//r1: not needed
	//sv_reconnect_limit = Cvar_Get ("sv_reconnect_limit", "3", CVAR_ARCHIVE);

	SZ_Init (&net_message, net_message_buffer, sizeof(net_message_buffer));

	//r1: init pyroadmin support
#ifdef USE_PYROADMIN
	pyroadminport = Cvar_Get ("pyroadminport", "0", CVAR_NOSET);
#endif

	//r1: lock server (prevent new connections)
	sv_locked = Cvar_Get ("sv_locked", "0", 0);

	//r1: restart server on this map if the _GAME_ dll dies
	//    (only via a ERR_GAME drop of course)
	sv_restartmap = Cvar_Get ("sv_restartmap", "", 0);

	//r1: server-side password protection (why id put this into the game dll
	//    i will never figure out)
	sv_password = Cvar_Get ("sv_password", "", 0);

	//r1: text filtering: 1 = strip low bits, 2 = low+hi
	sv_filter_q3names = Cvar_Get ("sv_filter_q3names", "0", 0);
	sv_filter_userinfo = Cvar_Get ("sv_filter_userinfo", "0", 0);
	sv_filter_stringcmds = Cvar_Get ("sv_filter_stringcmds", "0", 0);

	//r1: enable blocking of clients that attempt to attack server/clients
	sv_blackholes = Cvar_Get ("sv_blackholes", "1", 0);

	//r1: allow clients to use cl_nodelta (increases server bw usage)
	sv_allownodelta = Cvar_Get ("sv_allownodelta", "1", 0);

	//r1: option to block q2ace due to hacked versions
	sv_deny_q2ace = Cvar_Get ("sv_deny_q2ace", "0", 0);

	//r1: limit connections per ip address (stop zombie dos/flood)
	sv_iplimit = Cvar_Get ("sv_iplimit", "3", 0);

	//r1: message to send to connecting clients via CONNECTIONLESS print immediately
	//    after client connects. \n is expanded to new line.
	sv_connectmessage = Cvar_Get ("sv_connectmessage", "", 0);

	//r1: nocheat visibility check support (cpu intensive -- warning)
	sv_nc_visibilitycheck = Cvar_Get ("sv_nc_visibilitycheck", "0", 0);
	sv_nc_clientsonly = Cvar_Get ("sv_nc_clientsonly", "1", 0);

	//r1: delay between sending 32k download packets via dl server
	sv_downloadwait = Cvar_Get ("sv_downloadwait", "250", 0);

	//r1: max allowed file size for autodownloading (bytes)
	sv_max_download_size = Cvar_Get ("sv_max_download_size", "0", 0);

	//r1: max backup packets to allow from lagged clients (id.default=20)
	sv_max_netdrop = Cvar_Get ("sv_max_netdrop", "20", 0);

	//r1: don't respond to status requests
	sv_hidestatus = Cvar_Get ("sv_hidestatus", "0", 0);

	//r1: hide player info from status requests
	sv_hideplayers = Cvar_Get ("sv_hideplayers", "0", 0);

	//r1: kick high fps users flooding packets
	sv_fpsflood = Cvar_Get ("sv_fpsflood", "0", 0);

	//r1: randomize starting framenum to thwart map timers
	sv_randomframe = Cvar_Get ("sv_randomframe", "0", 0);

	//r1: msecs to give clients
	sv_msecs = Cvar_Get ("sv_msecs", "1800", 0);

	//r1: nocheat parser shit
	sv_nc_kick = Cvar_Get ("sv_nc_kick", "0", 0);
	sv_nc_announce = Cvar_Get ("sv_nc_announce", "0", 0);
	sv_filter_nocheat_spam = Cvar_Get ("sv_filter_nocheat_spam", "0", 0);

	//r1: crash on game errors with int3
	sv_gamedebug = Cvar_Get ("sv_gamedebug", "0", 0);

	//r1: reload game dll on next map change (resets to 0 after)
	sv_recycle = Cvar_Get ("sv_recycle", "0", 0);

	//r1: track server uptime in serverinfo?
	sv_uptime = Cvar_Get ("sv_uptime", "0", 0);

	//r1: allow strafe jumping at high fps values (Requires hacked client (!!!))
	sv_strafejump_hack = Cvar_Get ("sv_strafejump_hack", "0", 0);

	//r1: reserved slots (set 'rp' userinfo)
	sv_reserved_password = Cvar_Get ("sv_reserved_password", "", 0);
	sv_reserved_slots = Cvar_Get ("sv_reserved_slots", "0", CVAR_LATCH);

	//r1: allow use of 'map' command after game dll is loaded?
	sv_allow_map = Cvar_Get ("sv_allow_map", "0", 0);

	//r1: allow unconnected clients to execute game commands (default enabled in id!)
	sv_allow_unconnected_cmds = Cvar_Get ("sv_allow_unconnected_cmds", "0", 0);

	//r1: validate userinfo strings explicitly according to info key rules?
	sv_strict_userinfo_check = Cvar_Get ("sv_strict_userinfo_check", "0", 0);

	//r1: method to calculate pings. 0 = disable entirely, 1 = average, 2 = min
	sv_calcpings_method = Cvar_Get ("sv_calcpings_method", "1", 0);

	//r1: init pyroadmin
#ifdef USE_PYROADMIN
	if (pyroadminport->value)
	{
		char buff[128];
		int len;

		NET_StringToAdr ("127.0.0.1", &netaddress_pyroadmin);
		netaddress_pyroadmin.port = ShortSwap((short)pyroadminport->value);

		pyroadminid = Sys_Milliseconds() & 0xFFFF;

		len = Com_sprintf (buff, sizeof(buff), "hello %d", pyroadminid);
		Netchan_OutOfBand (NS_SERVER, &netaddress_pyroadmin, len, (byte *)buff);
	}
#endif
}

/*
==================
SV_FinalMessage

Used by SV_Shutdown to send a final message to all
connected clients before the server goes down.  The messages are sent immediately,
not just stuck on the outgoing message list, because the server is going
to totally exit after returning from this function.
==================
*/
void SV_FinalMessage (char *message, qboolean reconnect)
{
	int			i;
	client_t	*cl;
	
	SZ_Clear (&net_message);
	MSG_BeginWriteByte (&net_message, svc_print);
	MSG_WriteByte (&net_message, PRINT_HIGH);
	MSG_WriteString (&net_message, message);

	if (reconnect)
		MSG_BeginWriteByte (&net_message, svc_reconnect);
	else
		MSG_BeginWriteByte (&net_message, svc_disconnect);

	// send it twice
	// stagger the packets to crutch operating system limited buffers

	for (i=0, cl = svs.clients ; i<maxclients->value ; i++, cl++)
		if (cl->state >= cs_connected)
		{
			Netchan_Transmit (&cl->netchan, net_message.cursize
			, net_message.data);
		}

	for (i=0, cl = svs.clients ; i<maxclients->value ; i++, cl++)
		if (cl->state >= cs_connected)
		{
			Netchan_Transmit (&cl->netchan, net_message.cursize
			, net_message.data);
		}
}



/*
================
SV_Shutdown

Called when each game quits,
before Sys_Quit or Sys_Error
================
*/
void SV_Shutdown (char *finalmsg, qboolean reconnect, qboolean crashing)
{
	if (svs.clients)
		SV_FinalMessage (finalmsg, reconnect);

	Master_Shutdown ();

	if (!crashing || dbg_unload->value)
		SV_ShutdownGameProgs ();

	if (sv_download_socket)
	{
		Com_Printf ("SV_ShutDown: Closing downloadserver!\n");
		NET_CloseSocket (sv_download_socket);
	}
	
	// free current level
	if (sv.demofile)
		fclose (sv.demofile);
	memset (&sv, 0, sizeof(sv));
	Com_SetServerState (sv.state);

	// free server static data
	if (svs.clients)
		Z_Free (svs.clients);

	if (svs.client_entities)
		Z_Free (svs.client_entities);

	if (svs.demofile)
		fclose (svs.demofile);

	memset (&svs, 0, sizeof(svs));
}
