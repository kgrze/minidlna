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

/* Standard DLNA/UPnP filter flags */
#define FILTER_RES				0x00000040
#define FILTER_RES_SIZE				0x00001000
#define FILTER_UPNP_STORAGEUSED			0x00400000
/* Masks */
#define STANDARD_FILTER_MASK			0x00FFFFFF

static uint32_t
set_filter_flags(char *filter)
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
		if( strcmp(item, "res") == 0 )
		{
			flags |= FILTER_RES;
		}
		item = strtok_r(NULL, ",", &saveptr);
	}

	return flags;
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

#define COLUMNS "ID, CLASS, SIZE, TITLE, MIME "
#define SELECT_COLUMNS "SELECT OBJECT_ID, PARENT_ID, " COLUMNS

#define NON_ZERO(x) (x && atoi(x))
#define IS_ZERO(x) (!x || !atoi(x))

static int
callback(void *args, int argc, char **argv, char **azColName)
{
	struct Response *passed_args = (struct Response *)args;
	char *objectId = argv[0], *parent = argv[1], *id = argv[2], *class = argv[3], *size = argv[4], *title = argv[5], *mime = argv[6];
	char dlna_buf[128];
	const char *ext;
	struct string_s *str = passed_args->str;
	int ret = 0;

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

		ret = strcatf(str, "&lt;item id=\"%s\" parentID=\"%s\" restricted=\"1\"", objectId, parent);
		ret = strcatf(str, "&gt;"
		                   "&lt;dc:title&gt;%s&lt;/dc:title&gt;"
		                   "&lt;upnp:class&gt;object.%s&lt;/upnp:class&gt;",
		                   title, class);

		if( passed_args->filter & FILTER_RES ) {
			ext = mime_to_ext(mime);
			add_res(size, dlna_buf, mime, id, ext, passed_args);
		}

		ret = strcatf(str, "&lt;/item&gt;");
	}
	else if( strncmp(class, "container", 9) == 0 )
	{
		ret = strcatf(str, "&lt;container id=\"%s\" parentID=\"%s\" restricted=\"1\" ", objectId, parent);
		ret = strcatf(str, "&gt;"
		                   "&lt;dc:title&gt;%s&lt;/dc:title&gt;"
		                   "&lt;upnp:class&gt;object.%s&lt;/upnp:class&gt;",
		                   title, class);
		if( (passed_args->filter & FILTER_UPNP_STORAGEUSED) || strcmp(class+10, "storageFolder") == 0 ) {
			/* TODO: Implement real folder size tracking */
			ret = strcatf(str, "&lt;upnp:storageUsed&gt;%s&lt;/upnp:storageUsed&gt;", (size ? size : "-1"));
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
	args.filter = set_filter_flags(Filter);
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
		sql = sqlite3_mprintf("SELECT OBJECT_ID, PARENT_ID, " COLUMNS
				"from OBJECTS where OBJECT_ID = '%q';", id);
		DPRINTF(E_DEBUG, L_HTTP, "Browse SQL: %s\n", sql);
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

		/* If it's a DLNA client, return an error for bad sort criteria */
		if(GETFLAG(DLNA_STRICT_MASK))
		{
			SoapError(h, 709, "Unsupported or invalid sort criteria");
			goto browse_error;
		}

		sql = sqlite3_mprintf("SELECT OBJECT_ID, PARENT_ID, " COLUMNS
				      "from OBJECTS where %s %s limit %d, %d;", where, THISORNUL(orderBy), StartingIndex, RequestedCount);
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

static const struct
{
	const char * methodName;
	void (*methodImpl)(struct upnphttp *, const char *);
}
soapMethods[] =
{
	{ "Browse", BrowseContentDirectory},
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

