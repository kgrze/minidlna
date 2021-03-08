/* MiniDLNA project
 *
 * http://sourceforge.net/projects/minidlna/
 *
 * MiniDLNA media server
 * Copyright (C) 2008-2017  Justin Maggard
 *
 * This file is part of MiniDLNA.
 *
 * MiniDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiniDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MiniDLNA. If not, see <http://www.gnu.org/licenses/>.
 *
 * Portions of the code from the MiniUPnP project:
 *
 * Copyright (c) 2006-2007, Thomas Bernard
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * The name of the author may not be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ctype.h>

#include "upnpglobalvars.h"
#include "utils.h"
#include "upnphttp.h"
#include "upnpsoap.h"
#include "upnpreplyparse.h"
#include "getifaddr.h"
#include "scanner.h"
#include "sql.h"
#include "log.h"

#ifdef __sparc__ /* Sorting takes too long on slow processors with very large containers */
# define __SORT_LIMIT if( totalMatches < 10000 )
#else
# define __SORT_LIMIT
#endif

/* Standard Errors:
 *
 * errorCode errorDescription Description
 * --------	---------------- -----------
 * 401 		Invalid Action 	No action by that name at this service.
 * 402 		Invalid Args 	Could be any of the following: not enough in args,
 * 							too many in args, no in arg by that name,
 * 							one or more in args are of the wrong data type.
 * 403 		Out of Sync 	Out of synchronization.
 * 501 		Action Failed 	May be returned in current state of service
 * 							prevents invoking that action.
 * 600-699 	TBD 			Common action errors. Defined by UPnP Forum
 * 							Technical Committee.
 * 700-799 	TBD 			Action-specific errors for standard actions.
 * 							Defined by UPnP Forum working committee.
 * 800-899 	TBD 			Action-specific errors for non-standard actions.
 * 							Defined by UPnP vendor.
*/
static void
SoapError(struct upnphttp * h, int errCode, const char * errDesc)
{
	static const char resp[] =
		"<s:Envelope "
		"xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
		"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
		"<s:Body>"
		"<s:Fault>"
		"<faultcode>s:Client</faultcode>"
		"<faultstring>UPnPError</faultstring>"
		"<detail>"
		"<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
		"<errorCode>%d</errorCode>"
		"<errorDescription>%s</errorDescription>"
		"</UPnPError>"
		"</detail>"
		"</s:Fault>"
		"</s:Body>"
		"</s:Envelope>";

	char body[2048];
	int bodylen;

	DPRINTF(E_WARN, L_HTTP, "Returning UPnPError %d: %s\n", errCode, errDesc);
	bodylen = snprintf(body, sizeof(body), resp, errCode, errDesc);
	BuildResp2_upnphttp(h, 500, "Internal Server Error", body, bodylen);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

static void
BuildSendAndCloseSoapResp(struct upnphttp * h,
                          const char * body, int bodylen)
{
	static const char beforebody[] =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
		"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
		"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
		"<s:Body>";

	static const char afterbody[] =
		"</s:Body>"
		"</s:Envelope>\r\n";

	if (!body || bodylen < 0)
	{
		Send500(h);
		return;
	}

	BuildHeader_upnphttp(h, 200, "OK",  sizeof(beforebody) - 1
		+ sizeof(afterbody) - 1 + bodylen );

	memcpy(h->res_buf + h->res_buflen, beforebody, sizeof(beforebody) - 1);
	h->res_buflen += sizeof(beforebody) - 1;

	memcpy(h->res_buf + h->res_buflen, body, bodylen);
	h->res_buflen += bodylen;

	memcpy(h->res_buf + h->res_buflen, afterbody, sizeof(afterbody) - 1);
	h->res_buflen += sizeof(afterbody) - 1;

	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

static void
GetSystemUpdateID(struct upnphttp * h, const char * action)
{
	static const char resp[] =
		"<u:%sResponse "
		"xmlns:u=\"%s\">"
		"<Id>%d</Id>"
		"</u:%sResponse>";

	char body[512];
	int bodylen;

	bodylen = snprintf(body, sizeof(body), resp,
		action, "urn:schemas-upnp-org:service:ContentDirectory:1",
		updateID, action);
	BuildSendAndCloseSoapResp(h, body, bodylen);
}

static void
GetSortCapabilities(struct upnphttp * h, const char * action)
{
	static const char resp[] =
		"<u:%sResponse "
		"xmlns:u=\"%s\">"
		"<SortCaps>"
		  "dc:title,"
		  "dc:date,"
		  "upnp:class,"
		  "upnp:album,"
		  "upnp:originalTrackNumber"
		"</SortCaps>"
		"</u:%sResponse>";

	char body[512];
	int bodylen;

	bodylen = snprintf(body, sizeof(body), resp,
		action, "urn:schemas-upnp-org:service:ContentDirectory:1",
		action);
	BuildSendAndCloseSoapResp(h, body, bodylen);
}

static void
GetSearchCapabilities(struct upnphttp * h, const char * action)
{
	static const char resp[] =
		"<u:%sResponse xmlns:u=\"%s\">"
		"<SearchCaps>"
		  "dc:creator,"
		  "dc:date,"
		  "dc:title,"
		  "upnp:album,"
		  "upnp:actor,"
		  "upnp:artist,"
		  "upnp:class,"
		  "upnp:genre,"
		  "@id,"
		  "@parentID,"
		  "@refID"
		"</SearchCaps>"
		"</u:%sResponse>";

	char body[512];
	int bodylen;

	bodylen = snprintf(body, sizeof(body), resp,
		action, "urn:schemas-upnp-org:service:ContentDirectory:1",
		action);
	BuildSendAndCloseSoapResp(h, body, bodylen);
}

/* Standard DLNA/UPnP filter flags */
#define FILTER_CHILDCOUNT			0x00000001
#define FILTER_DC_CREATOR			0x00000002
#define FILTER_DC_DATE				0x00000004
#define FILTER_DC_DESCRIPTION			0x00000008
#define FILTER_DLNA_NAMESPACE			0x00000010
#define FILTER_REFID				0x00000020
#define FILTER_RES				0x00000040
#define FILTER_RES_BITRATE			0x00000080
#define FILTER_RES_DURATION			0x00000100
#define FILTER_RES_NRAUDIOCHANNELS		0x00000200
#define FILTER_RES_RESOLUTION			0x00000400
#define FILTER_RES_SAMPLEFREQUENCY		0x00000800
#define FILTER_RES_SIZE				0x00001000
#define FILTER_SEARCHABLE			0x00002000
#define FILTER_UPNP_ACTOR			0x00004000
#define FILTER_UPNP_ALBUM			0x00008000
#define FILTER_UPNP_ALBUMARTURI			0x00010000
#define FILTER_UPNP_ALBUMARTURI_DLNA_PROFILEID	0x00020000
#define FILTER_UPNP_ARTIST			0x00040000
#define FILTER_UPNP_GENRE			0x00080000
#define FILTER_UPNP_ORIGINALTRACKNUMBER		0x00100000
#define FILTER_UPNP_SEARCHCLASS			0x00200000
#define FILTER_UPNP_STORAGEUSED			0x00400000
/* Not normally used, so leave out of the default filter */
#define FILTER_UPNP_PLAYBACKCOUNT		0x01000000
#define FILTER_UPNP_LASTPLAYBACKPOSITION	0x02000000
/* Vendor-specific filter flags */
#define FILTER_SEC_CAPTION_INFO_EX		0x04000000
#define FILTER_SEC_DCM_INFO			0x08000000
#define FILTER_PV_SUBTITLE_FILE_TYPE		0x10000000
#define FILTER_PV_SUBTITLE_FILE_URI		0x20000000
#define FILTER_PV_SUBTITLE			0x30000000
#define FILTER_AV_MEDIA_CLASS			0x40000000
/* Masks */
#define STANDARD_FILTER_MASK			0x00FFFFFF
#define FILTER_BOOKMARK_MASK			(FILTER_UPNP_PLAYBACKCOUNT | \
						 FILTER_UPNP_LASTPLAYBACKPOSITION | \
						 FILTER_SEC_DCM_INFO)

static uint32_t
set_filter_flags(char *filter, struct upnphttp *h)
{
	char *item, *saveptr = NULL;
	uint32_t flags = 0;

	if( !filter || (strlen(filter) <= 1) ) {
		/* Not the full 32 bits.  Skip vendor-specific stuff by default. */
		flags = STANDARD_FILTER_MASK;
	}
	if (flags)
		return flags;

	item = strtok_r(filter, ",", &saveptr);
	while( item != NULL )
	{
		if( saveptr )
			*(item-1) = ',';
		while( isspace(*item) )
			item++;
		if( strcmp(item, "@childCount") == 0 )
		{
			flags |= FILTER_CHILDCOUNT;
		}
		else if( strcmp(item, "@searchable") == 0 )
		{
			flags |= FILTER_SEARCHABLE;
		}
		else if( strcmp(item, "dc:creator") == 0 )
		{
			flags |= FILTER_DC_CREATOR;
		}
		else if( strcmp(item, "dc:date") == 0 )
		{
			flags |= FILTER_DC_DATE;
		}
		else if( strcmp(item, "dc:description") == 0 )
		{
			flags |= FILTER_DC_DESCRIPTION;
		}
		else if( strcmp(item, "dlna") == 0 )
		{
			flags |= FILTER_DLNA_NAMESPACE;
		}
		else if( strcmp(item, "@refID") == 0 )
		{
			flags |= FILTER_REFID;
		}
		else if( strcmp(item, "upnp:album") == 0 )
		{
			flags |= FILTER_UPNP_ALBUM;
		}
		else if( strcmp(item, "upnp:albumArtURI") == 0 )
		{
			flags |= FILTER_UPNP_ALBUMARTURI;
		}
		else if( strcmp(item, "upnp:albumArtURI@dlna:profileID") == 0 )
		{
			flags |= FILTER_UPNP_ALBUMARTURI;
			flags |= FILTER_UPNP_ALBUMARTURI_DLNA_PROFILEID;
		}
		else if( strcmp(item, "upnp:artist") == 0 )
		{
			flags |= FILTER_UPNP_ARTIST;
		}
		else if( strcmp(item, "upnp:actor") == 0 )
		{
			flags |= FILTER_UPNP_ACTOR;
		}
		else if( strcmp(item, "upnp:genre") == 0 )
		{
			flags |= FILTER_UPNP_GENRE;
		}
		else if( strcmp(item, "upnp:originalTrackNumber") == 0 )
		{
			flags |= FILTER_UPNP_ORIGINALTRACKNUMBER;
		}
		else if( strcmp(item, "upnp:searchClass") == 0 )
		{
			flags |= FILTER_UPNP_SEARCHCLASS;
		}
		else if( strcmp(item, "upnp:storageUsed") == 0 )
		{
			flags |= FILTER_UPNP_STORAGEUSED;
		}
		else if( strcmp(item, "res") == 0 )
		{
			flags |= FILTER_RES;
		}
		else if( (strcmp(item, "res@bitrate") == 0) ||
		         (strcmp(item, "@bitrate") == 0) ||
		         ((strcmp(item, "bitrate") == 0) && (flags & FILTER_RES)) )
		{
			flags |= FILTER_RES;
			flags |= FILTER_RES_BITRATE;
		}
		else if( (strcmp(item, "res@duration") == 0) ||
		         (strcmp(item, "@duration") == 0) ||
		         ((strcmp(item, "duration") == 0) && (flags & FILTER_RES)) )
		{
			flags |= FILTER_RES;
			flags |= FILTER_RES_DURATION;
		}
		else if( (strcmp(item, "res@nrAudioChannels") == 0) ||
		         (strcmp(item, "@nrAudioChannels") == 0) ||
		         ((strcmp(item, "nrAudioChannels") == 0) && (flags & FILTER_RES)) )
		{
			flags |= FILTER_RES;
			flags |= FILTER_RES_NRAUDIOCHANNELS;
		}
		else if( (strcmp(item, "res@resolution") == 0) ||
		         (strcmp(item, "@resolution") == 0) ||
		         ((strcmp(item, "resolution") == 0) && (flags & FILTER_RES)) )
		{
			flags |= FILTER_RES;
			flags |= FILTER_RES_RESOLUTION;
		}
		else if( (strcmp(item, "res@sampleFrequency") == 0) ||
		         (strcmp(item, "@sampleFrequency") == 0) ||
		         ((strcmp(item, "sampleFrequency") == 0) && (flags & FILTER_RES)) )
		{
			flags |= FILTER_RES;
			flags |= FILTER_RES_SAMPLEFREQUENCY;
		}
		else if( (strcmp(item, "res@size") == 0) ||
		         (strcmp(item, "@size") == 0) ||
		         (strcmp(item, "size") == 0) )
		{
			flags |= FILTER_RES;
			flags |= FILTER_RES_SIZE;
		}
		else if( strcmp(item, "upnp:playbackCount") == 0 )
		{
			flags |= FILTER_UPNP_PLAYBACKCOUNT;
		}
		else if( strcmp(item, "upnp:lastPlaybackPosition") == 0 )
		{
			flags |= FILTER_UPNP_LASTPLAYBACKPOSITION;
		}
		else if( strcmp(item, "sec:CaptionInfoEx") == 0 )
		{
			flags |= FILTER_SEC_CAPTION_INFO_EX;
		}
		else if( strcmp(item, "sec:dcmInfo") == 0 )
		{
			flags |= FILTER_SEC_DCM_INFO;
		}
		else if( strcmp(item, "res@pv:subtitleFileType") == 0 )
		{
			flags |= FILTER_PV_SUBTITLE_FILE_TYPE;
		}
		else if( strcmp(item, "res@pv:subtitleFileUri") == 0 )
		{
			flags |= FILTER_PV_SUBTITLE_FILE_URI;
		}
		else if( strcmp(item, "av:mediaClass") == 0 )
		{
			flags |= FILTER_AV_MEDIA_CLASS;
		}
		item = strtok_r(NULL, ",", &saveptr);
	}

	return flags;
}

static char *
parse_sort_criteria(char *sortCriteria, int *error)
{
	char *order = NULL;
	char *item, *saveptr;
	int i, ret, reverse, title_sorted = 0;
	struct string_s str;
	*error = 0;

	if( force_sort_criteria )
		sortCriteria = strdup(force_sort_criteria);
	if( !sortCriteria )
		return NULL;

	if( (item = strtok_r(sortCriteria, ",", &saveptr)) )
	{
		order = malloc(4096);
		str.data = order;
		str.size = 4096;
		str.off = 0;
		strcatf(&str, "order by ");
	}
	for( i = 0; item != NULL; i++ )
	{
		reverse=0;
		if( i )
			strcatf(&str, ", ");
		if( *item == '+' )
		{
			item++;
		}
		else if( *item == '-' )
		{
			reverse = 1;
			item++;
		}
		else
		{
			DPRINTF(E_ERROR, L_HTTP, "No order specified [%s]\n", item);
			goto bad_direction;
		}
		if( strcasecmp(item, "upnp:class") == 0 )
		{
			strcatf(&str, "o.CLASS");
		}
		else if( strcasecmp(item, "dc:title") == 0 )
		{
			strcatf(&str, "d.TITLE");
			title_sorted = 1;
		}
		else if( strcasecmp(item, "dc:date") == 0 )
		{
			strcatf(&str, "d.DATE");
		}
		else if( strcasecmp(item, "upnp:originalTrackNumber") == 0 )
		{
			strcatf(&str, "d.DISC, d.TRACK");
		}
		else if( strcasecmp(item, "upnp:album") == 0 )
		{
			strcatf(&str, "d.ALBUM");
		}
		else
		{
			DPRINTF(E_ERROR, L_HTTP, "Unhandled SortCriteria [%s]\n", item);
		bad_direction:
			*error = -1;
			if( i )
			{
				ret = strlen(order);
				order[ret-2] = '\0';
			}
			i--;
			goto unhandled_order;
		}

		if( reverse )
			strcatf(&str, " DESC");
		unhandled_order:
		item = strtok_r(NULL, ",", &saveptr);
	}
	if( i <= 0 )
	{
		free(order);
		if( force_sort_criteria )
			free(sortCriteria);
		return NULL;
	}
	/* Add a "tiebreaker" sort order */
	if( !title_sorted )
		strcatf(&str, ", TITLE ASC");

	if( force_sort_criteria )
		free(sortCriteria);

	return order;
}

inline static void
add_resized_res(int srcw, int srch, int reqw, int reqh, char *dlna_pn,
                char *detailID, struct Response *args)
{
	int dstw = reqw;
	int dsth = reqh;

	strcatf(args->str, "&lt;res ");
	if( args->filter & FILTER_RES_RESOLUTION )
	{
		dstw = reqw;
		dsth = ((((reqw<<10)/srcw)*srch)>>10);
		if( dsth > reqh ) {
			dsth = reqh;
			dstw = (((reqh<<10)/srch) * srcw>>10);
		}
		strcatf(args->str, "resolution=\"%dx%d\" ", dstw, dsth);
	}
	strcatf(args->str, "protocolInfo=\"http-get:*:image/jpeg:"
	                          "DLNA.ORG_PN=%s;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=%08X%024X\"&gt;"
	                          "http://%s:%d/Resized/%s.jpg?width=%d,height=%d"
	                          "&lt;/res&gt;",
	                          dlna_pn, DLNA_FLAG_DLNA_V1_5|DLNA_FLAG_HTTP_STALLING|DLNA_FLAG_TM_B|DLNA_FLAG_TM_I, 0,
	                          lan_addr[args->iface].str, runtime_vars.port,
	                          detailID, dstw, dsth);
}

inline static void
add_res(char *size, char *dlna_pn, char *mime,
        char *detailID, const char *ext, struct Response *args)
{
	strcatf(args->str, "&lt;res ");
	if( size && (args->filter & FILTER_RES_SIZE) ) {
		strcatf(args->str, "size=\"%s\" ", size);
	}

	strcatf(args->str, "protocolInfo=\"http-get:*:%s:%s\"&gt;"
	                          "http://%s:%d/MediaItems/%s.%s"
	                          "&lt;/res&gt;",
	                          mime, dlna_pn, lan_addr[args->iface].str,
	                          runtime_vars.port, detailID, ext);
}

static int
get_child_count(const char *object)
{
	int ret;
	ret = sql_get_int_field(db, "SELECT count(*) from OBJECTS where PARENT_ID = '%s';", object);

	return (ret > 0) ? ret : 0;
}

static int
object_exists(const char *object)
{
	int ret;
	ret = sql_get_int_field(db, "SELECT count(*) from OBJECTS where OBJECT_ID = '%q'",
				strcmp(object, "*") == 0 ? "0" : object);
	return (ret > 0);
}

#define COLUMNS "o.DETAIL_ID, o.CLASS," \
                " d.SIZE, d.TITLE, d.MIME "
#define SELECT_COLUMNS "SELECT o.OBJECT_ID, o.PARENT_ID, o.REF_ID, " COLUMNS

#define NON_ZERO(x) (x && atoi(x))
#define IS_ZERO(x) (!x || !atoi(x))

static int
callback(void *args, int argc, char **argv, char **azColName)
{
	struct Response *passed_args = (struct Response *)args;
	char *id = argv[0], *parent = argv[1], *refID = argv[2], *detailID = argv[3], *class = argv[4], *size = argv[5], *title = argv[6], *mime = argv[7];
	char dlna_buf[128];
	const char *ext;
	struct string_s *str = passed_args->str;
	int ret = 0;

	/* Make sure we have at least 8KB left of allocated memory to finish the response. */
	if( str->off > (str->size - 8192) )
	{
#if MAX_RESPONSE_SIZE > 0
		if( (str->size+DEFAULT_RESP_SIZE) <= MAX_RESPONSE_SIZE )
		{
#endif
			str->data = realloc(str->data, (str->size+DEFAULT_RESP_SIZE));
			if( str->data )
			{
				str->size += DEFAULT_RESP_SIZE;
				DPRINTF(E_DEBUG, L_HTTP, "UPnP SOAP response enlarged to %lu. [%d results so far]\n",
					(unsigned long)str->size, passed_args->returned);
			}
			else
			{
				DPRINTF(E_ERROR, L_HTTP, "UPnP SOAP response was too big, and realloc failed!\n");
				return -1;
			}
#if MAX_RESPONSE_SIZE > 0
		}
		else
		{
			DPRINTF(E_ERROR, L_HTTP, "UPnP SOAP response cut short, to not exceed the max response size [%lld]!\n", (long long int)MAX_RESPONSE_SIZE);
			return -1;
		}
#endif
	}
	passed_args->returned++;

	if( strncmp(class, "item", 4) == 0 )
	{
		uint32_t dlna_flags = DLNA_FLAG_DLNA_V1_5|DLNA_FLAG_HTTP_STALLING|DLNA_FLAG_TM_B;
		/* We may need special handling for certain MIME types */
		if( *mime == 'v' )
		{
			dlna_flags |= DLNA_FLAG_TM_S;
		}

		strcpy(dlna_buf, "*");

		ret = strcatf(str, "&lt;item id=\"%s\" parentID=\"%s\" restricted=\"1\"", id, parent);
		if( refID && (passed_args->filter & FILTER_REFID) ) {
			ret = strcatf(str, " refID=\"%s\"", refID);
		}
		ret = strcatf(str, "&gt;"
		                   "&lt;dc:title&gt;%s&lt;/dc:title&gt;"
		                   "&lt;upnp:class&gt;object.%s&lt;/upnp:class&gt;",
		                   title, class);

		if( passed_args->filter & FILTER_RES ) {
			ext = mime_to_ext(mime);
			add_res(size, dlna_buf, mime, detailID, ext, passed_args);
		}

		ret = strcatf(str, "&lt;/item&gt;");
	}
	else if( strncmp(class, "container", 9) == 0 )
	{
		ret = strcatf(str, "&lt;container id=\"%s\" parentID=\"%s\" restricted=\"1\" ", id, parent);
		/* If the client calls for BrowseMetadata on root, we have to include our "upnp:searchClass"'s, unless they're filtered out */
		if( passed_args->requested == 1 && strcmp(id, "0") == 0 && (passed_args->filter & FILTER_UPNP_SEARCHCLASS) ) {
			ret = strcatf(str, "&gt;&lt;upnp:searchClass includeDerived=\"1\"&gt;object.item.videoItem&lt;/upnp:searchClass");
		}
		ret = strcatf(str, "&gt;"
		                   "&lt;dc:title&gt;%s&lt;/dc:title&gt;"
		                   "&lt;upnp:class&gt;object.%s&lt;/upnp:class&gt;",
		                   title, class);
		if( (passed_args->filter & FILTER_UPNP_STORAGEUSED) || strcmp(class+10, "storageFolder") == 0 ) {
			/* TODO: Implement real folder size tracking */
			ret = strcatf(str, "&lt;upnp:storageUsed&gt;%s&lt;/upnp:storageUsed&gt;", (size ? size : "-1"));
		}

		if( passed_args->filter & FILTER_AV_MEDIA_CLASS ) {
			char class;
			if( strncmp(id, VIDEO_ID, sizeof(VIDEO_ID)) == 0 )
				class = 'V';
			else
				class = 0;
			if( class )
				ret = strcatf(str, "&lt;av:mediaClass xmlns:av=\"urn:schemas-sony-com:av\"&gt;"
				                    "%c&lt;/av:mediaClass&gt;", class);
		}
		ret = strcatf(str, "&lt;/container&gt;");
	}

	(void)ret;

	return 0;
}

static void
BrowseContentDirectory(struct upnphttp * h, const char * action)
{
	static const char resp0[] =
			"<u:BrowseResponse "
			"xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
			"<Result>"
			"&lt;DIDL-Lite"
			CONTENT_DIRECTORY_SCHEMAS;
	char *zErrMsg = NULL;
	char *sql, *ptr;
	struct Response args;
	struct string_s str;
	int totalMatches = 0;
	int ret;
	const char *ObjectID, *BrowseFlag;
	char *Filter, *SortCriteria;
	const char *objectid_sql = "o.OBJECT_ID";
	const char *parentid_sql = "o.PARENT_ID";
	const char *refid_sql = "o.REF_ID";
	char where[256] = "";
	char *orderBy = NULL;
	struct NameValueParserData data;
	int RequestedCount = 0;
	int StartingIndex = 0;

	memset(&args, 0, sizeof(args));
	memset(&str, 0, sizeof(str));

	ParseNameValue(h->req_buf + h->req_contentoff, h->req_contentlen, &data, 0);

	ObjectID = GetValueFromNameValueList(&data, "ObjectID");
	Filter = GetValueFromNameValueList(&data, "Filter");
	BrowseFlag = GetValueFromNameValueList(&data, "BrowseFlag");
	SortCriteria = GetValueFromNameValueList(&data, "SortCriteria");

	if( (ptr = GetValueFromNameValueList(&data, "RequestedCount")) )
		RequestedCount = atoi(ptr);
	if( RequestedCount < 0 )
	{
		SoapError(h, 402, "Invalid Args");
		goto browse_error;
	}
	if( !RequestedCount )
		RequestedCount = -1;
	if( (ptr = GetValueFromNameValueList(&data, "StartingIndex")) )
		StartingIndex = atoi(ptr);
	if( StartingIndex < 0 )
	{
		SoapError(h, 402, "Invalid Args");
		goto browse_error;
	}
	if( !BrowseFlag || (strcmp(BrowseFlag, "BrowseDirectChildren") && strcmp(BrowseFlag, "BrowseMetadata")) )
	{
		SoapError(h, 402, "Invalid Args");
		goto browse_error;
	}
	if( !ObjectID && !(ObjectID = GetValueFromNameValueList(&data, "ContainerID")) )
	{
		SoapError(h, 402, "Invalid Args");
		goto browse_error;
	}

	str.data = malloc(DEFAULT_RESP_SIZE);
	str.size = DEFAULT_RESP_SIZE;
	str.off = sprintf(str.data, "%s", resp0);
	/* See if we need to include DLNA namespace reference */
	args.iface = h->iface;
	args.filter = set_filter_flags(Filter, h);
	if( args.filter & FILTER_DLNA_NAMESPACE )
		ret = strcatf(&str, DLNA_NAMESPACE);
	if( args.filter & FILTER_PV_SUBTITLE )
		ret = strcatf(&str, PV_NAMESPACE);
	strcatf(&str, "&gt;\n");

	args.returned = 0;
	args.requested = RequestedCount;
	args.flags = 0;
	args.str = &str;
	DPRINTF(E_DEBUG, L_HTTP, "Browsing ContentDirectory:\n"
	                         " * ObjectID: %s\n"
	                         " * Count: %d\n"
	                         " * StartingIndex: %d\n"
	                         " * BrowseFlag: %s\n"
	                         " * Filter: %s\n"
	                         " * SortCriteria: %s\n",
				ObjectID, RequestedCount, StartingIndex,
	                        BrowseFlag, Filter, SortCriteria);

	if( strcmp(BrowseFlag+6, "Metadata") == 0 )
	{
		const char *id = ObjectID;
		args.requested = 1;
		sql = sqlite3_mprintf("SELECT %s, %s, %s, " COLUMNS
				      "from OBJECTS o left join DETAILS d on (d.ID = o.DETAIL_ID)"
				      " where OBJECT_ID = '%q';",
				      objectid_sql, parentid_sql, refid_sql, id);
		ret = sqlite3_exec(db, sql, callback, (void *) &args, &zErrMsg);
		totalMatches = args.returned;
	}
	else
	{
		if (!where[0])
			sqlite3_snprintf(sizeof(where), where, "PARENT_ID = '%q'", ObjectID);

		if (!totalMatches)
			totalMatches = get_child_count(ObjectID);
		ret = 0;
		if (SortCriteria && !orderBy)
		{
			__SORT_LIMIT
			orderBy = parse_sort_criteria(SortCriteria, &ret);
		}
		else if (!orderBy)
		{
			if( strncmp(ObjectID, MUSIC_PLIST_ID, strlen(MUSIC_PLIST_ID)) == 0 )
			{
				if( strcmp(ObjectID, MUSIC_PLIST_ID) == 0 )
					ret = xasprintf(&orderBy, "order by d.TITLE");
				else
					ret = xasprintf(&orderBy, "order by length(OBJECT_ID), OBJECT_ID");
			}
			else
				orderBy = parse_sort_criteria(SortCriteria, &ret);
			if( ret == -1 )
			{
				free(orderBy);
				orderBy = NULL;
				ret = 0;
			}
		}
		/* If it's a DLNA client, return an error for bad sort criteria */
		if(GETFLAG(DLNA_STRICT_MASK))
		{
			SoapError(h, 709, "Unsupported or invalid sort criteria");
			goto browse_error;
		}

		sql = sqlite3_mprintf("SELECT %s, %s, %s, " COLUMNS
				      "from OBJECTS o left join DETAILS d on (d.ID = o.DETAIL_ID)"
				      " where %s %s limit %d, %d;",
				      objectid_sql, parentid_sql, refid_sql,
				      where, THISORNUL(orderBy), StartingIndex, RequestedCount);
		DPRINTF(E_DEBUG, L_HTTP, "Browse SQL: %s\n", sql);
		ret = sqlite3_exec(db, sql, callback, (void *) &args, &zErrMsg);
	}
	if( (ret != SQLITE_OK) && (zErrMsg != NULL) )
	{
		DPRINTF(E_WARN, L_HTTP, "SQL error: %s\nBAD SQL: %s\n", zErrMsg, sql);
		sqlite3_free(zErrMsg);
		SoapError(h, 709, "Unsupported or invalid sort criteria");
		goto browse_error;
	}
	sqlite3_free(sql);
	/* Does the object even exist? */
	if( !totalMatches )
	{
		if( !object_exists(ObjectID) )
		{
			SoapError(h, 701, "No such object error");
			goto browse_error;
		}
	}
	ret = strcatf(&str, "&lt;/DIDL-Lite&gt;</Result>\n"
	                    "<NumberReturned>%u</NumberReturned>\n"
	                    "<TotalMatches>%u</TotalMatches>\n"
	                    "<UpdateID>%u</UpdateID>"
	                    "</u:BrowseResponse>",
	                    args.returned, totalMatches, updateID);
	BuildSendAndCloseSoapResp(h, str.data, str.off);
browse_error:
	ClearNameValueList(&data);
	free(orderBy);
	free(str.data);
}

static inline void
charcat(struct string_s *str, char c)
{
	if (str->size <= str->off)
	{
		str->data[str->size-1] = '\0';
		return;
	}
	str->data[str->off] = c;
	str->off += 1;
}

static inline char *
parse_search_criteria(const char *str, char *sep)
{
	struct string_s criteria;
	int len;
	int literal = 0, like = 0;
	const char *s;

	if (!str)
		return strdup("1 = 1");

	len = strlen(str) + 32;
	criteria.data = malloc(len);
	criteria.size = len;
	criteria.off = 0;

	s = str;

	while (isspace(*s))
		s++;

	while (*s)
	{
		if (literal)
		{
			switch (*s) {
			case '&':
				if (strncmp(s, "&quot;", 6) == 0)
					s += 5;
				else if (strncmp(s, "&apos;", 6) == 0)
				{
					strcatf(&criteria, "'");
					s += 6;
					continue;
				}
				else
					break;
			case '"':
				literal = 0;
				if (like)
				{
					charcat(&criteria, '%');
					like--;
				}
				charcat(&criteria, '"');
				break;
			case '\\':
				if (strncmp(s, "\\&quot;", 7) == 0)
				{
					strcatf(&criteria, "&amp;quot;");
					s += 7;
					continue;
				}
				break;
			case 'o':
				if (strncmp(s, "object.", 7) == 0)
					s += 7;
				else if (strncmp(s, "object\"", 7) == 0 ||
				         strncmp(s, "object&quot;", 12) == 0)
				{
					s += 6;
					continue;
				}
			default:
				charcat(&criteria, *s);
				break;
			}
		}
		else
		{
			switch (*s) {
			case '\\':
				if (strncmp(s, "\\&quot;", 7) == 0)
				{
					strcatf(&criteria, "&amp;quot;");
					s += 7;
					continue;
				}
				else
					charcat(&criteria, *s);
				break;
			case '"':
				literal = 1;
				charcat(&criteria, *s);
				if (like == 2)
				{
					charcat(&criteria, '%');
					like--;
				}
				break;
			case '&':
				if (strncmp(s, "&quot;", 6) == 0)
				{
					literal = 1;
					strcatf(&criteria, "\"");
					if (like == 2)
					{
						charcat(&criteria, '%');
						like--;
					}
					s += 5;
				}
				else if (strncmp(s, "&apos;", 6) == 0)
				{
					strcatf(&criteria, "'");
					s += 5;
				}
				else if (strncmp(s, "&lt;", 4) == 0)
				{
					strcatf(&criteria, "<");
					s += 3;
				}
				else if (strncmp(s, "&gt;", 4) == 0)
				{
					strcatf(&criteria, ">");
					s += 3;
				}
				else
					charcat(&criteria, *s);
				break;
			case '@':
				if (strncmp(s, "@refID", 6) == 0)
				{
					strcatf(&criteria, "REF_ID");
					s += 6;
					continue;
				}
				else if (strncmp(s, "@id", 3) == 0)
				{
					strcatf(&criteria, "OBJECT_ID");
					s += 3;
					continue;
				}
				else if (strncmp(s, "@parentID", 9) == 0)
				{
					strcatf(&criteria, "PARENT_ID");
					s += 9;
					strcpy(sep, "*");
					continue;
				}
				else
					charcat(&criteria, *s);
				break;
			case 'c':
				if (strncmp(s, "contains", 8) == 0)
				{
					strcatf(&criteria, "like");
					s += 8;
					like = 2;
					continue;
				}
				else
					charcat(&criteria, *s);
				break;
			case 'd':
				if (strncmp(s, "derivedfrom", 11) == 0)
				{
					strcatf(&criteria, "like");
					s += 11;
					like = 1;
					continue;
				}
				else if (strncmp(s, "dc:title", 8) == 0)
				{
					strcatf(&criteria, "d.TITLE");
					s += 8;
					continue;
				}
				else
					charcat(&criteria, *s);
				break;
			case 'e':
				if (strncmp(s, "exists", 6) == 0)
				{
					s += 6;
					while (isspace(*s))
						s++;
					if (strncmp(s, "true", 4) == 0)
					{
						strcatf(&criteria, "is not NULL");
						s += 3;
					}
					else if (strncmp(s, "false", 5) == 0)
					{
						strcatf(&criteria, "is NULL");
						s += 4;
					}
				}
				else
					charcat(&criteria, *s);
				break;
			case 'u':
				if (strncmp(s, "upnp:class", 10) == 0)
				{
					strcatf(&criteria, "o.CLASS");
					s += 10;
					continue;
				}
				else
					charcat(&criteria, *s);
				break;
			case '(':
				if (s > str && !isspace(s[-1]))
					charcat(&criteria, ' ');
				charcat(&criteria, *s);
				break;
			case ')':
				charcat(&criteria, *s);
				if (!isspace(s[1]))
					charcat(&criteria, ' ');
				break;
			default:
				charcat(&criteria, *s);
				break;
			}
		}
		s++;
	}
	charcat(&criteria, '\0');

	return criteria.data;
}

static void
SearchContentDirectory(struct upnphttp * h, const char * action)
{
	static const char resp0[] =
			"<u:SearchResponse "
			"xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
			"<Result>"
			"&lt;DIDL-Lite"
			CONTENT_DIRECTORY_SCHEMAS;
	char *zErrMsg = NULL;
	char *sql, *ptr;
	struct Response args;
	struct string_s str;
	int totalMatches;
	int ret;
	const char *ContainerID;
	char *Filter, *SearchCriteria, *SortCriteria;
	char *orderBy = NULL, *where = NULL, sep[] = "$*";
	char groupBy[] = "group by DETAIL_ID";
	struct NameValueParserData data;
	int RequestedCount = 0;
	int StartingIndex = 0;

	memset(&args, 0, sizeof(args));
	memset(&str, 0, sizeof(str));

	ParseNameValue(h->req_buf + h->req_contentoff, h->req_contentlen, &data, 0);

	ContainerID = GetValueFromNameValueList(&data, "ContainerID");
	Filter = GetValueFromNameValueList(&data, "Filter");
	SearchCriteria = GetValueFromNameValueList(&data, "SearchCriteria");
	SortCriteria = GetValueFromNameValueList(&data, "SortCriteria");

	if( (ptr = GetValueFromNameValueList(&data, "RequestedCount")) )
		RequestedCount = atoi(ptr);
	if( !RequestedCount )
		RequestedCount = -1;
	if( (ptr = GetValueFromNameValueList(&data, "StartingIndex")) )
		StartingIndex = atoi(ptr);
	if( !ContainerID )
	{
		if( !(ContainerID = GetValueFromNameValueList(&data, "ObjectID")) )
		{
			SoapError(h, 402, "Invalid Args");
			goto search_error;
		}
	}

	str.data = malloc(DEFAULT_RESP_SIZE);
	str.size = DEFAULT_RESP_SIZE;
	str.off = sprintf(str.data, "%s", resp0);
	/* See if we need to include DLNA namespace reference */
	args.iface = h->iface;
	args.filter = set_filter_flags(Filter, h);
	if( args.filter & FILTER_DLNA_NAMESPACE )
	{
		ret = strcatf(&str, DLNA_NAMESPACE);
	}
	strcatf(&str, "&gt;\n");

	args.returned = 0;
	args.requested = RequestedCount;
	args.flags = 0;
	args.str = &str;
	DPRINTF(E_DEBUG, L_HTTP, "Searching ContentDirectory:\n"
	                         " * ObjectID: %s\n"
	                         " * Count: %d\n"
	                         " * StartingIndex: %d\n"
	                         " * SearchCriteria: %s\n"
	                         " * Filter: %s\n"
	                         " * SortCriteria: %s\n",
				ContainerID, RequestedCount, StartingIndex,
	                        SearchCriteria, Filter, SortCriteria);

	if( strcmp(ContainerID, "0") == 0 )
		ContainerID = "*";

	if( strcmp(ContainerID, MUSIC_ALL_ID) == 0 ||
	    GETFLAG(DLNA_STRICT_MASK) )
		groupBy[0] = '\0';

	where = parse_search_criteria(SearchCriteria, sep);
	DPRINTF(E_DEBUG, L_HTTP, "Translated SearchCriteria: %s\n", where);

	totalMatches = sql_get_int_field(db, "SELECT (select count(distinct DETAIL_ID)"
	                                     " from OBJECTS o left join DETAILS d on (o.DETAIL_ID = d.ID)"
	                                     " where (OBJECT_ID glob '%q%s') and (%s))"
	                                     " + "
	                                     "(select count(*) from OBJECTS o left join DETAILS d on (o.DETAIL_ID = d.ID)"
	                                     " where (OBJECT_ID = '%q') and (%s))",
	                                     ContainerID, sep, where, ContainerID, where);
	if( totalMatches < 0 )
	{
		/* Must be invalid SQL, so most likely bad or unhandled search criteria. */
		SoapError(h, 708, "Unsupported or invalid search criteria");
		goto search_error;
	}
	/* Does the object even exist? */
	if( !totalMatches )
	{
		if( !object_exists(ContainerID) )
		{
			SoapError(h, 710, "No such container");
			goto search_error;
		}
	}
	ret = 0;
	__SORT_LIMIT
	orderBy = parse_sort_criteria(SortCriteria, &ret);
	/* If it's a DLNA client, return an error for bad sort criteria */
	if( ret < 0 && GETFLAG(DLNA_STRICT_MASK))
	{
		SoapError(h, 709, "Unsupported or invalid sort criteria");
		goto search_error;
	}

	sql = sqlite3_mprintf( SELECT_COLUMNS
	                      "from OBJECTS o left join DETAILS d on (d.ID = o.DETAIL_ID)"
	                      " where OBJECT_ID glob '%q%s' and (%s) %s "
	                      "%z %s"
	                      " limit %d, %d",
	                      ContainerID, sep, where, groupBy,
	                      (*ContainerID == '*') ? NULL :
	                      sqlite3_mprintf("UNION ALL " SELECT_COLUMNS
	                                      "from OBJECTS o left join DETAILS d on (d.ID = o.DETAIL_ID)"
	                                      " where OBJECT_ID = '%q' and (%s) ", ContainerID, where),
	                      orderBy, StartingIndex, RequestedCount);
	DPRINTF(E_DEBUG, L_HTTP, "Search SQL: %s\n", sql);
	ret = sqlite3_exec(db, sql, callback, (void *) &args, &zErrMsg);
	if( (ret != SQLITE_OK) && (zErrMsg != NULL) )
	{
		DPRINTF(E_WARN, L_HTTP, "SQL error: %s\nBAD SQL: %s\n", zErrMsg, sql);
		sqlite3_free(zErrMsg);
	}
	sqlite3_free(sql);
	ret = strcatf(&str, "&lt;/DIDL-Lite&gt;</Result>\n"
	                    "<NumberReturned>%u</NumberReturned>\n"
	                    "<TotalMatches>%u</TotalMatches>\n"
	                    "<UpdateID>%u</UpdateID>"
	                    "</u:SearchResponse>",
	                    args.returned, totalMatches, updateID);
	BuildSendAndCloseSoapResp(h, str.data, str.off);
search_error:
	ClearNameValueList(&data);
	free(orderBy);
	free(where);
	free(str.data);
}

/*
If a control point calls QueryStateVariable on a state variable that is not
buffered in memory within (or otherwise available from) the service,
the service must return a SOAP fault with an errorCode of 404 Invalid Var.

QueryStateVariable remains useful as a limited test tool but may not be
part of some future versions of UPnP.
*/
static void
QueryStateVariable(struct upnphttp * h, const char * action)
{
	static const char resp[] =
	"<u:%sResponse "
	"xmlns:u=\"%s\">"
		"<return>%s</return>"
	"</u:%sResponse>";

	char body[512];
	struct NameValueParserData data;
	const char * var_name;

	ParseNameValue(h->req_buf + h->req_contentoff, h->req_contentlen, &data, 0);
	/*var_name = GetValueFromNameValueList(&data, "QueryStateVariable"); */
	/*var_name = GetValueFromNameValueListIgnoreNS(&data, "varName");*/
	var_name = GetValueFromNameValueList(&data, "varName");

	DPRINTF(E_INFO, L_HTTP, "QueryStateVariable(%.40s)\n", var_name);

	if(!var_name)
	{
		SoapError(h, 402, "Invalid Args");
	}
	else if(strcmp(var_name, "ConnectionStatus") == 0)
	{
		int bodylen;
		bodylen = snprintf(body, sizeof(body), resp,
		           action, "urn:schemas-upnp-org:control-1-0",
		                   "Connected", action);
		BuildSendAndCloseSoapResp(h, body, bodylen);
	}
	else
	{
		DPRINTF(E_WARN, L_HTTP, "%s: Unknown: %s\n", action, THISORNUL(var_name));
		SoapError(h, 404, "Invalid Var");
	}

	ClearNameValueList(&data);
}

static const struct
{
	const char * methodName;
	void (*methodImpl)(struct upnphttp *, const char *);
}
soapMethods[] =
{
	{ "QueryStateVariable", QueryStateVariable},
	{ "Browse", BrowseContentDirectory},
	{ "Search", SearchContentDirectory},
	{ "GetSearchCapabilities", GetSearchCapabilities},
	{ "GetSortCapabilities", GetSortCapabilities},
	{ "GetSystemUpdateID", GetSystemUpdateID},
	{ 0, 0 }
};

void
ExecuteSoapAction(struct upnphttp * h, const char * action, int n)
{
	char * p;

	p = strchr(action, '#');
	if(p)
	{
		int i = 0;
		int len;
		int methodlen;
		char * p2;
		p++;
		p2 = strchr(p, '"');
		if(p2)
			methodlen = p2 - p;
		else
			methodlen = n - (p - action);
		DPRINTF(E_DEBUG, L_HTTP, "SoapMethod: %.*s\n", methodlen, p);
		while(soapMethods[i].methodName)
		{
			len = strlen(soapMethods[i].methodName);
			if(strncmp(p, soapMethods[i].methodName, len) == 0)
			{
				soapMethods[i].methodImpl(h, soapMethods[i].methodName);
				return;
			}
			i++;
		}

		DPRINTF(E_WARN, L_HTTP, "SoapMethod: Unknown: %.*s\n", methodlen, p);
	}

	SoapError(h, 401, "Invalid Action");
}

