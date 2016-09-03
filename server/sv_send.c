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
// sv_main.c -- server main program

#include "server.h"

/*
=============================================================================

Com_Printf redirection

=============================================================================
*/

char sv_outputbuf[SV_OUTPUTBUF_LENGTH];

void SV_FlushRedirect (int sv_redirected, char *outputbuf)
{
	if (sv_redirected == RD_PACKET)
	{
		Netchan_OutOfBandPrint (NS_SERVER, &net_from, "print\n%s", outputbuf);
	}
}


/*
=============================================================================

EVENT MESSAGES

=============================================================================
*/

void SV_MSGListIntegrityCheck (client_t *cl)
{
#ifndef NDEBUG
	messagelist_t	*msg;

	msg = cl->msgListStart;

	while (msg->next)
		msg = msg->next;

	if (msg != cl->msgListEnd)
		Com_Error (ERR_FATAL, "SV_MSGListIntegrityCheck: messagelist_t corrupted!");
#endif
}

/*
=================
SV_ClientPrintf

Sends text across to be displayed if the level passes
=================
*/
void SV_ClientPrintf (client_t *cl, int level, char *fmt, ...)
{
	va_list		argptr;
	char		string[MAX_USABLEMSG-3];
	int			msglen;

	if (level < cl->messagelevel)
		return;

	va_start (argptr,fmt);
	msglen = Q_vsnprintf (string, sizeof(string)-1, fmt, argptr);
	va_end (argptr);
	string[sizeof(string)-1] = 0;

	if (msglen == -1)
		Com_Printf ("WARNING: SV_ClientPrintf: overflow\n", LOG_SERVER|LOG_WARNING);

	MSG_BeginWriting (svc_print);
	MSG_WriteByte (level);
	MSG_WriteString (string);
	SV_AddMessage (cl, true);
}

/*
=================
SV_BroadcastPrintf

Sends text to all active clients
=================
*/
void EXPORT SV_BroadcastPrintf (int level, char *fmt, ...)
{
	va_list		argptr;
	char		string[MAX_USABLEMSG-3];
	client_t	*cl;
	int			i;
	int			msglen;

	va_start (argptr,fmt);
	msglen = Q_vsnprintf (string, sizeof(string)-1, fmt,argptr);
	va_end (argptr);
	
	string[sizeof(string)-1] = 0;

	if (msglen == -1)
		Com_Printf ("WARNING: SV_BroadcastPrintf: overflow\n", LOG_SERVER|LOG_WARNING);

	// echo to console
	if (dedicated->intvalue)
	{
		if (level == PRINT_CHAT)
			Com_Printf ("%s", LOG_SERVER|LOG_CHAT, string);
		else
			Com_Printf ("%s", LOG_SERVER, string);
	}

	for (i=0, cl = svs.clients ; i<maxclients->intvalue; i++, cl++)
	{
		if (level < cl->messagelevel)
			continue;

		if (cl->state != cs_spawned)
			continue;

		MSG_BeginWriting (svc_print);
		MSG_WriteByte (level);
		MSG_WriteString (string);
		SV_AddMessage (cl, true);
	}
}

messagelist_t * SV_DeleteMessage (client_t *cl, messagelist_t *message, messagelist_t *last)
{
	//only free if it was malloced
	if (message->cursize > MSG_MAX_SIZE_BEFORE_MALLOC)
		free (message->data);

	last->next = message->next;
	
	//end of the list
	if (message == cl->msgListEnd)
		cl->msgListEnd = last;

#ifdef _DEBUG
	memset (message, 0xCC, sizeof(*message));
#endif

	return last;
}

void SV_ClearMessageList (client_t *client)
{
	messagelist_t *message, *last;

	message = client->msgListStart;

	for (;;)
	{
		last = message;
		message = message->next;

		if (!message)
			break;

		message = SV_DeleteMessage (client, message, last);
	}
}

void SV_AddMessageSingle (client_t *cl, qboolean reliable)
{
	int				index;
	messagelist_t	*next;

	if (cl->state <= cs_zombie)
	{
		Com_Printf ("Warning, SV_AddMessage to zombie/free client.\n", LOG_SERVER|LOG_WARNING);
		return;
	}

	//an overflown client
	if (!cl->messageListData)
		return;

	//doesn't want unreliables (irc bots/etc)
	if (cl->nodata && !reliable)
		return;

	//get next message position
	index = (cl->msgListEnd - cl->msgListStart) + 1;

	//have they overflown?
	if (index >= MAX_MESSAGES_PER_LIST-1)
	{
		Com_Printf ("WARNING: Index overflow for %s.\n", LOG_SERVER|LOG_WARNING, cl->name);

		//clear the buffer for overflow print and malloc cleanup
		SV_ClearMessageList (cl);

		//drop them
		cl->notes |= NOTE_OVERFLOWED;
		return;
	}

	//set up links
	next = &cl->messageListData[index];
	cl->msgListEnd->next = next;

	cl->msgListEnd = next;
	next->next = NULL;

	SV_MSGListIntegrityCheck (cl);

	//write message to this buffer
	MSG_EndWrite (next);

	SV_MSGListIntegrityCheck (cl);

	//check its sane, should never happen...
	if (next->cursize >= MAX_USABLEMSG)
	{
		//uh oh...
		Com_Printf ("ALERT: SV_AddMessageSingle: Message size %d to %s is larger than MAX_USABLEMSG!!\n", LOG_SERVER|LOG_WARNING, next->cursize, cl->name);

		//clear the buffer for overflow print and malloc cleanup
		SV_ClearMessageList (cl);

		//drop them
		cl->notes |= NOTE_OVERFLOWED;
		return;
	}

	//set reliable flag
	next->reliable = reliable;
}

void SV_CheckForOverflow (void)
{
	int				i;
	client_t		*cl;

	for (i=0,cl=svs.clients ; i<maxclients->intvalue ; i++,cl++)
	{
		if (cl->state <= cs_zombie)
			continue;

		if (!(cl->notes & NOTE_OVERFLOWED) || (cl->notes & NOTE_OVERFLOW_DONE))
			continue;

		cl->notes |= NOTE_OVERFLOW_DONE;

		//drop message
		if (cl->name[0])
		{
			if (cl->state == cs_spawned)
			{
				SV_BroadcastPrintf (PRINT_HIGH, "%s overflowed\n", cl->name);
			}
			else
			{
				Com_Printf ("%s overflowed message list size!\n", LOG_SERVER|LOG_WARNING, cl->name);
			}
		}

		SV_DropClient (cl, true);
	}
}

void SV_AddMessage (client_t *cl, qboolean reliable)
{
	SV_AddMessageSingle (cl, reliable);
	MSG_FreeData ();
	SV_CheckForOverflow ();
}

void SV_AddMessageAll (qboolean reliable)
{
	int				i;
	client_t		*cl;

	for (i=0,cl=svs.clients ; i<maxclients->intvalue ; i++,cl++)
	{
		if (cl->state <= cs_zombie)
			continue;

		SV_AddMessageSingle (cl, reliable);
	}
	MSG_FreeData();
	SV_CheckForOverflow ();
}

/*
=================
SV_BroadcastCommand

Sends text to all active clients
=================
*/
void SV_BroadcastCommand (char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];

	if (!sv.state)
		return;
	
	va_start (argptr,fmt);
	vsnprintf (string, sizeof(string)-1, fmt, argptr);
	va_end (argptr);

	MSG_BeginWriting (svc_stufftext);
	MSG_WriteString (string);
	SV_Multicast (NULL, MULTICAST_ALL_R);
}


/*
=================
SV_Multicast

Sends the contents of sv.multicast to a subset of the clients,
then clears sv.multicast.

MULTICAST_ALL	same as broadcast (origin can be NULL)
MULTICAST_PVS	send to clients potentially visible from org
MULTICAST_PHS	send to clients potentially hearable from org
=================
*/
void EXPORT SV_Multicast (vec3_t /*@null@*/ origin, multicast_t to)
{
	client_t		*client;
	byte			*mask;
	int				leafnum, cluster;
	int				j;
	qboolean		reliable;
	int				area1, area2;

	reliable = false;

	if (to != MULTICAST_ALL_R && to != MULTICAST_ALL)
	{
		if (!origin)
			Com_Error (ERR_DROP, "SV_Multicast: bad multicast_t used with NULL origin");
		leafnum = CM_PointLeafnum (origin);
		area1 = CM_LeafArea (leafnum);
	}
	else
	{
		leafnum = 0;	// just to avoid compiler warnings
		area1 = 0;
	}

	//r1: check we have data in the multicast buffer
	if (!MSG_GetLength())
		Com_Error (ERR_DROP, "SV_Multicast: no data");

	// if doing a serverrecord, store everything
	if (svs.demofile)
		SZ_Write (&svs.demo_multicast, MSG_GetData(), MSG_GetLength());
	
	switch (to)
	{
	case MULTICAST_ALL_R:
		reliable = true;	// intentional fallthrough
	case MULTICAST_ALL:
		leafnum = 0;
		mask = NULL;
		break;

	case MULTICAST_PHS_R:
		reliable = true;	// intentional fallthrough
	case MULTICAST_PHS:
		leafnum = CM_PointLeafnum (origin);
		cluster = CM_LeafCluster (leafnum);
		mask = CM_ClusterPHS (cluster);
		break;

	case MULTICAST_PVS_R:
		reliable = true;	// intentional fallthrough
	case MULTICAST_PVS:
		leafnum = CM_PointLeafnum (origin);
		cluster = CM_LeafCluster (leafnum);
		mask = CM_ClusterPVS (cluster);
		break;

	default:
		mask = NULL;
		Com_Error (ERR_FATAL, "SV_Multicast: bad to:%i", to);
	}

	// send the data to all relevent clients
	for (j = 0, client = svs.clients; j < maxclients->intvalue; j++, client++)
	{
		if (client->state <= cs_zombie)
			continue;
		
		if (
				client->state != cs_spawned &&
				(
					!reliable ||
					(
					//r1: don't send these types to connecting clients, they are pointless even if reliable.
						MSG_GetType() == svc_muzzleflash ||
						MSG_GetType() == svc_muzzleflash2 ||
						MSG_GetType() == svc_temp_entity ||
						MSG_GetType() == svc_print ||
						MSG_GetType() == svc_sound ||
						MSG_GetType() == svc_centerprint
					)
				)
			)
			continue;

		if (mask)
		{
			leafnum = CM_PointLeafnum (client->edict->s.origin);
			cluster = CM_LeafCluster (leafnum);
			area2 = CM_LeafArea (leafnum);
			if (!CM_AreasConnected (area1, area2))
				continue;
			if ( mask && (!(mask[cluster>>3] & (1<<(cluster&7)) ) ) )
				continue;
		}

		SV_AddMessageSingle (client, reliable);
	}

	MSG_FreeData();
	SV_CheckForOverflow();
}


/*  
==================
SV_StartSound

Each entity can have eight independant sound sources, like voice,
weapon, feet, etc.

If cahnnel & 8, the sound will be sent to everyone, not just
things in the PHS.

FIXME: if entity isn't in PHS, they must be forced to be sent or
have the origin explicitly sent.

Channel 0 is an auto-allocate channel, the others override anything
already running on that entity/channel pair.

An attenuation of 0 will play full volume everywhere in the level.
Larger attenuations will drop off.  (max 4 attenuation)

Timeofs can range from 0.0 to 0.1 to cause sounds to be started
later in the frame than they normally would.

If origin is NULL, the origin is determined from the entity origin
or the midpoint of the entity box for bmodels.
==================
*/  
void EXPORT SV_StartSound (vec3_t origin, edict_t *entity, int channel,
					int soundindex, float volume,
					float attenuation, float timeofs)
{       
	int			sendchan;
    int			flags;
    int			j;
	int			ent;
	vec3_t		origin_v;
	qboolean	use_phs;
	client_t	*client;
//	sizebuf_t	*to;
	qboolean	force_pos= false;

	if (FLOAT_LT_ZERO(volume) || volume > 1.0)
		Com_Error (ERR_DROP, "SV_StartSound: volume = %f", volume);

	if (FLOAT_LT_ZERO(attenuation) || attenuation > 4)
		Com_Error (ERR_DROP, "SV_StartSound: attenuation = %f", attenuation);

//	if (channel < 0 || channel > 15)
//		Com_Error (ERR_FATAL, "SV_StartSound: channel = %i", channel);

	if (FLOAT_LT_ZERO(timeofs) || timeofs > 0.255)
		Com_Error (ERR_DROP, "SV_StartSound: timeofs = %f", timeofs);

	ent = NUM_FOR_EDICT(entity);

	if (channel & CHAN_NO_PHS_ADD)	// no PHS flag
	{
		use_phs = false;
		channel &= 7;
	}
	else
		use_phs = true;

	sendchan = (ent<<3) | (channel&7);

	flags = 0;

	if (volume != DEFAULT_SOUND_PACKET_VOLUME)
		flags |= SND_VOLUME;

	if (attenuation != DEFAULT_SOUND_PACKET_ATTENUATION)
		flags |= SND_ATTENUATION;

	if (attenuation == ATTN_NONE)
	{
		use_phs = false;
	}
	else
	{
		// the client doesn't know that bmodels have weird origins
		// the origin can also be explicitly set
		if ( (entity->svflags & SVF_NOCLIENT)
			|| (entity->solid == SOLID_BSP) 
			|| origin) {
			flags |= SND_POS;
			force_pos = true;
			}

		// use the entity origin unless it is a bmodel or explicitly specified
		if (!origin)
		{
			origin = origin_v;
			if (entity->solid == SOLID_BSP)
			{
				origin_v[0] = entity->s.origin[0]+0.5*(entity->mins[0]+entity->maxs[0]);
				origin_v[1] = entity->s.origin[1]+0.5*(entity->mins[1]+entity->maxs[1]);
				origin_v[2] = entity->s.origin[2]+0.5*(entity->mins[2]+entity->maxs[2]);
			}
			else
			{
				VectorCopy (entity->s.origin, origin_v);
			}
		}
	}

	// always send the entity number for channel overrides
	flags |= SND_ENT;

	if (timeofs)
		flags |= SND_OFFSET;

	for (j = 0, client = svs.clients; j < maxclients->intvalue; j++, client++)
	{
		if (client->state <= cs_zombie || (client->state != cs_spawned && !(channel & CHAN_RELIABLE)))
			continue;

		if (use_phs) {
			if (force_pos) {
				flags |= SND_POS;
			} else {
				if (!PF_inPHS (client->edict->s.origin, origin))
					continue;

				if (!PF_inPVS (client->edict->s.origin, origin))
					flags |= SND_POS;
				else
					flags &= ~SND_POS;
			}
		}

		MSG_BeginWriting (svc_sound);
		MSG_WriteByte (flags);
		MSG_WriteByte (soundindex);

		if (flags & SND_VOLUME)
			MSG_WriteByte (volume*255);

		if (flags & SND_ATTENUATION)
			MSG_WriteByte (attenuation*64);

		if (flags & SND_OFFSET)
			MSG_WriteByte (timeofs*1000);

		if (flags & SND_ENT)
			MSG_WriteShort (sendchan);

		if (flags & SND_POS)
			MSG_WritePos (origin);

		SV_AddMessage (client, (channel & CHAN_RELIABLE));
	}
	// if the sound doesn't attenuate,send it to everyone
	// (global radio chatter, voiceovers, etc)
	/*if (attenuation == ATTN_NONE)
		use_phs = false;

	if (channel & CHAN_RELIABLE)
	{
		if (use_phs)
			SV_Multicast (origin, MULTICAST_PHS_R);
		else
			SV_Multicast (origin, MULTICAST_ALL_R);
	}
	else
	{
		if (use_phs)
			SV_Multicast (origin, MULTICAST_PHS);
		else
			SV_Multicast (origin, MULTICAST_ALL);
	}*/
}           


/*
===============================================================================

FRAME UPDATES

===============================================================================
*/

void SV_WriteReliableMessages (client_t *client, int buffSize)
{
	//shouldn't happen...
	if (client->state == cs_free)
	{
		Com_Printf ("SV_WriteReliableMessages: Writing to free client!!\n", LOG_SERVER|LOG_WARNING);
		return;
	}

	//writing to overflowed client, don't bother trying to get any final messages through
	if (client->messageListData == NULL)
		return;

	//if the reliable is free, let's fill it up
	if (!client->netchan.reliable_length)
	{
		messagelist_t	*message, *last;
		
		client->netchan.message.maxsize = buffSize;

		message = client->msgListStart;

		for (;;)
		{
			last = message;
			message = message->next;

			if (!message)
				break;

			//its a reliable message
			if (message->reliable)
			{
				//but it wouldn't fit.
				if (message->cursize + client->netchan.message.cursize > client->netchan.message.maxsize)
					break;

				//it fits, write it in
				SZ_Write (&client->netchan.message, message->data, message->cursize);

				//and delete from the message list
				message = SV_DeleteMessage (client, message, last);
			}
		}

		SV_MSGListIntegrityCheck (client);

		//something very bad happened if this occurs
		if (client->netchan.message.overflowed)
			Com_Error (ERR_DROP, "SV_SendClientDatagram: netchan message overflow! (this should never happen)");
	}
}

/*
=======================
SV_SendClientDatagram
=======================
*/
static qboolean SV_SendClientDatagram (client_t *client)
{
	byte			msg_buf[MAX_USABLEMSG];
	sizebuf_t		msg;
	int				ret;
	messagelist_t	*message, *last;
	byte			*wanted;

	//init unreliable portion
	SZ_Init (&msg, msg_buf, sizeof(msg_buf));

	msg.allowoverflow = true;

	if (client->netchan.reliable_length)
	{
		//fix up maxsize for how much space we can fill up safely.
		//reliable is full, so we can fill up remainder of the packet.
		msg.maxsize = sizeof(msg_buf) - client->netchan.reliable_length;
	}
	else
	{
		//reliable is empty - we must allow for at least one reliable message, set available space appropriately.
		message = client->msgListStart;
		for (;;)
		{
			message = message->next;

			if (!message)
				break;

			if (message->reliable)
			{
				wanted = message->data;
				msg.maxsize = sizeof(msg_buf) - message->cursize;
				//Com_Printf ("SV_SendClientDatagram: Reserving %d bytes of buffer space for %s. Have %d for unreliable.\n", LOG_GENERAL, message->cursize, client->name, msg.maxsize);
				break;
			}
		}
	}

	//this will write an unreliable svc_frame to the message list
	if (!client->nodata)
	{
		byte		frame_buf[4096];
		sizebuf_t	frame;

		SV_BuildClientFrame (client);

		//we write svc_frame to it's own buffer to allow for compression
		SZ_Init (&frame, frame_buf, sizeof(frame_buf));
		frame.allowoverflow = true;

		//adjust for packetentities hack
		if (sv_packetentities_hack->intvalue == 1 || client->protocol == ORIGINAL_PROTOCOL_VERSION)
			frame.maxsize = msg.maxsize;

retryframe:

		// send over all the relevant entity_state_t
		// and the player_state_t
		SV_WriteFrameToClient (client, &frame);

		//if frame overflowed, we're screwed either way :)
		if (!frame.overflowed)
		{
			if (frame.cursize > msg.maxsize)
			{
				//r1q2 clients get compressed frame, normal clients get nothing
				byte	compressed_frame[4096];
				int		compressed_frame_len;

				compressed_frame_len = ZLibCompressChunk (frame_buf, frame.cursize, compressed_frame, sizeof(compressed_frame), Z_DEFAULT_COMPRESSION, -15);

				if (compressed_frame_len != -1 && compressed_frame_len <= msg.maxsize - 5)
				{
					Com_Printf ("SV_SendClientDatagram: svc_frame for %s: %d -> %d\n", LOG_GENERAL, client->name, frame.cursize, compressed_frame_len);
					SZ_WriteByte (&msg, svc_zpacket);
					SZ_WriteShort (&msg, compressed_frame_len);
					SZ_WriteShort (&msg, frame.cursize);
					SZ_Write (&msg, compressed_frame, compressed_frame_len);
				}
				else
				{
					if (sv_packetentities_hack->intvalue == 2)
					{
						Com_Printf ("SV_SendClientDatagram: zlib svc_frame %d -> %d for %s still didn't fit, using msg.maxsize of %d\n", LOG_GENERAL, frame.cursize, compressed_frame_len, client->name, msg.maxsize);
						SZ_Clear (&frame);
						frame.maxsize = msg.maxsize;
						goto retryframe;
					}
				}
			}
			else
			{
				//it fits as-is, write it out
				SZ_Write (&msg, frame_buf, frame.cursize);
			}
		}
	}

	if (msg.overflowed)
	{
		Com_Printf ("WARNING: Message overflow for %s after frame. Shouldn't happen!!\n", LOG_SERVER|LOG_WARNING, client->name);
		SZ_Clear (&msg);
	}

	//msg at this point now contains the svc_frame
	//now we fill it up with all the other unreliable messages, starting with the most "obvious"
	//effects - these are tempents (except ones which make no sound - waste), sounds, then the rest.

	//first we check if we have enough room for even bothering - packetentities may have filled it up!
	if (msg.cursize + 8 < msg.maxsize)
	{
		message = client->msgListStart;
		for (;;)
		{
			last = message;
			message = message->next;

			if (!message)
				break;

			if (!message->reliable)
			{
				if (message->data[0] == svc_temp_entity)
				{
					//don't include trivial in this part
					if (message->data[1] == TE_BLOOD || message->data[1] == TE_SPLASH)
						continue;

					//drop some semi-useless effects
					if (message->data[1] == TE_GUNSHOT || message->data[1] == TE_BULLET_SPARKS || message->data[1] == TE_SHOTGUN)
					{
						//randomly drop some of these
						if (randomMT() < 0x80000000)
							continue;
					}

					//not gonna fit, try another one
					if (msg.cursize + message->cursize > msg.maxsize)
					{
						message = SV_DeleteMessage (client, message, last);
						continue;
					}

					//write it in
					SZ_Write (&msg, message->data, message->cursize);

					//free it
					message = SV_DeleteMessage (client, message, last);
				}
			}
		}

		SV_MSGListIntegrityCheck (client);

		//recheck how much space we have
		if (msg.cursize + 8 < msg.maxsize)
		{
			//now we write sounds
			message = client->msgListStart;
			for (;;)
			{
				last = message;
				message = message->next;

				if (!message)
					break;

				if (!message->reliable)
				{
					if (message->data[0] == svc_sound)
					{
						//not gonna fit, try another one
						if (msg.cursize + message->cursize > msg.maxsize)
						{
							message = SV_DeleteMessage (client, message, last);
							continue;
						}

						//write it in
						SZ_Write (&msg, message->data, message->cursize);

						//free it
						message = SV_DeleteMessage (client, message, last);
					}
				}
			}

			SV_MSGListIntegrityCheck (client);

			//recheck how much space is available
			if (msg.cursize + 8 < msg.maxsize)
			{
				//everything else we can fit
				message = client->msgListStart;
				for (;;)
				{
					last = message;
					message = message->next;

					if (!message)
						break;

					if (!message->reliable)
					{
						//not gonna fit, try another one
						if (msg.cursize + message->cursize > msg.maxsize)
						{
							message = SV_DeleteMessage (client, message, last);
							continue;
						}

						//write it in
						SZ_Write (&msg, message->data, message->cursize);

						//free it
						message = SV_DeleteMessage (client, message, last);
					}
				}

				SV_MSGListIntegrityCheck (client);
			}
		}
	}

	//another "should never happen"...
	if (msg.overflowed)
		Com_Error (ERR_DROP, "SV_SendClientDatagram: unreliable message overflow!");

	//now we nuke all remaining unreliable content
	//unreliable is by nature time sensitive - no point queueing it.
	message = client->msgListStart;
	for (;;)
	{
		last = message;
		message = message->next;

		if (!message)
			break;

		if (!message->reliable)
		{
			Com_DPrintf ("SCD: Dropped an unreliable %s to %s.\n", svc_strings[*message->data], client->name);
			message = SV_DeleteMessage (client, message, last);
		}
	}

	SV_MSGListIntegrityCheck (client);

	//now we fill the reliable portion if it's empty -- but not too much so we can hopefully always
	//fit an svc_frame so we measure using hacks and frameSize. however we must commit to delivering
	//one reliable message at least to avoid getting stuck on never sending large messages.

	if (client->netchan.reliable_length)
	{
		SV_WriteReliableMessages (client, sizeof(msg_buf) - msg.cursize);
	}
	else
	{
		SV_WriteReliableMessages (client, sizeof(msg_buf) - msg.cursize);
		message = client->msgListStart;
		for (;;)
		{
			message = message->next;

			if (!message)
				break;

			if (message->reliable)
			{
				if (message->data == wanted)
					Com_Error (ERR_FATAL, "SV_SendClientDatagram: wanted to send message but it never got written!");
				break;
			}
		}
	}

	// send the datagram
	ret = Netchan_Transmit (&client->netchan, msg.cursize, msg.data);
	if (ret == -1)
	{
		SV_KickClient (client, "connection reset by peer", NULL);
		return false;
	}

	// record the size for rate estimation
	client->message_size[sv.framenum % RATE_MESSAGES] = msg.cursize;

	return true;
}


/*
==================
SV_DemoCompleted
==================
*/
void SV_DemoCompleted (void)
{
	if (sv.demofile)
	{
		fclose (sv.demofile);
		sv.demofile = NULL;
	}
	SV_Nextserver ();
}


/*
=======================
SV_RateDrop

Returns true if the client is over its current
bandwidth estimation and should not be sent another packet
=======================
*/
static qboolean SV_RateDrop (client_t *c)
{
	int		total;
	int		i;

	// never drop over the loopback
	if (NET_IsLocalHost (&c->netchan.remote_address))
		return false;

	total = 0;

	for (i = 0 ; i < RATE_MESSAGES ; i++)
	{
		total += c->message_size[i];
	}

	if (total > c->rate)
	{
		c->surpressCount++;
		c->message_size[sv.framenum % RATE_MESSAGES] = 0;
		return true;
	}

	return false;
}

/*
=======================
SV_SendClientMessages
=======================
*/
void SV_SendClientMessages (void)
{
	int			i;
	client_t	*c;
	int			msglen;
	byte		msgbuf[MAX_MSGLEN];
	int			r;

	msglen = 0;

	// read the next demo message if needed
	if (sv.demofile && sv.state == ss_demo)
	{
		if (!sv_paused->intvalue)
		{
			// get the next message
			r = fread (&msglen, 4, 1, sv.demofile);
			if (r != 1)
			{
				SV_DemoCompleted ();
				return;
			}
			msglen = LittleLong (msglen);
			if (msglen == -1)
			{
				SV_DemoCompleted ();
				return;
			}
			if (msglen > MAX_MSGLEN)
				Com_Error (ERR_DROP, "SV_SendClientMessages: msglen > MAX_MSGLEN");
			r = fread (msgbuf, msglen, 1, sv.demofile);
			if (r != 1)
			{
				SV_DemoCompleted ();
				return;
			}
		}
	}

	// send a message to each connected client
	for (i=0, c = svs.clients ; i<maxclients->intvalue; i++, c++)
	{
		if (c->state == cs_free)
			continue;

		//r1: totally rewrote how reliable/datagram works. concept of overflow
		//is now obsolete.

		if (sv.state == ss_cinematic || sv.state == ss_demo || sv.state == ss_pic)
		{
			SV_WriteReliableMessages (c, MAX_USABLEMSG);
			Netchan_Transmit (&c->netchan, msglen, msgbuf);
		}
		else if (c->state == cs_spawned)
		{
			// don't overrun bandwidth
			if (SV_RateDrop (c))
				continue;

			SV_SendClientDatagram (c);
		}
		else
		{
			SV_WriteReliableMessages (c, MAX_USABLEMSG);
			// just update reliable	if needed
			if (c->netchan.reliable_length || curtime - c->netchan.last_sent > 100)
				Netchan_Transmit (&c->netchan, 0, NULL);
		}
	}
}

