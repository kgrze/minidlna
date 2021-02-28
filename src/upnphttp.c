/* MiniDLNA project
 *
 * http://sourceforge.net/projects/minidlna/
 *
 * MiniDLNA media server
 * Copyright (C) 2008-2009  Justin Maggard
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
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <limits.h>

#include "config.h"
#include "upnpglobalvars.h"
#include "upnphttp.h"
#include "upnpdescgen.h"
#include "minidlnapath.h"
#include "upnpsoap.h"
#include "utils.h"
#include "getifaddr.h"
#include "log.h"
#include "sql.h"
#include <libexif/exif-loader.h>
#include "process.h"
#include "sendfile.h"

#define MAX_BUFFER_SIZE 2147483647
#define MIN_BUFFER_SIZE 65536

#define INIT_STR(s, d) { s.data = d; s.size = sizeof(d); s.off = 0; }

enum event_type {
	E_INVALID,
	E_SUBSCRIBE,
	E_RENEW
};

static void SendResp_dlnafile(struct upnphttp *, char * url);

struct upnphttp * 
New_upnphttp(int s)
{
	struct upnphttp * ret;
	if(s<0)
		return NULL;
	ret = (struct upnphttp *)malloc(sizeof(struct upnphttp));
	if(ret == NULL)
		return NULL;
	memset(ret, 0, sizeof(struct upnphttp));
	ret->socket = s;
	return ret;
}

void
CloseSocket_upnphttp(struct upnphttp * h)
{
	if(close(h->socket) < 0)
	{
		DPRINTF(E_ERROR, L_HTTP, "CloseSocket_upnphttp: close(%d): %s\n", h->socket, strerror(errno));
	}
	h->socket = -1;
	h->state = 100;
}

void
Delete_upnphttp(struct upnphttp * h)
{
	if(h)
	{
		if(h->socket >= 0)
			CloseSocket_upnphttp(h);
		free(h->req_buf);
		free(h->res_buf);
		free(h);
	}
}

/* parse HttpHeaders of the REQUEST */
static void
ParseHttpHeaders(struct upnphttp * h)
{
	char * line;
	char * colon;
	char * p;
	int n;
	line = h->req_buf;
	/* TODO : check if req_buf, contentoff are ok */
	while(line < (h->req_buf + h->req_contentoff))
	{
		colon = strchr(line, ':');
		if(colon)
		{
			if(strncasecmp(line, "Content-Length", 14)==0)
			{
				p = colon;
				while(*p && (*p < '0' || *p > '9'))
					p++;
				h->req_contentlen = atoi(p);
				if(h->req_contentlen < 0) {
					DPRINTF(E_WARN, L_HTTP, "Invalid Content-Length %d", h->req_contentlen);
					h->req_contentlen = 0;
				}
			}
			else if(strncasecmp(line, "SOAPAction", 10)==0)
			{
				p = colon;
				n = 0;
				while(*p == ':' || *p == ' ' || *p == '\t')
					p++;
				while(p[n] >= ' ')
					n++;
				if(n >= 2 &&
				   ((p[0] == '"' && p[n-1] == '"') ||
				    (p[0] == '\'' && p[n-1] == '\'')))
				{
					p++;
					n -= 2;
				}
				h->req_soapAction = p;
				h->req_soapActionLen = n;
			}
			else if(strncasecmp(line, "Callback", 8)==0)
			{
				p = colon;
				while(*p && *p != '<' && *p != '\r' )
					p++;
				n = 0;
				while(p[n] && p[n] != '>' && p[n] != '\r' )
					n++;
				h->req_Callback = p + 1;
				h->req_CallbackLen = MAX(0, n - 1);
			}
			else if(strncasecmp(line, "SID", 3)==0)
			{
				//zqiu: fix bug for test 4.0.5
				//Skip extra headers like "SIDHEADER: xxxxxx xxx"
				for(p=line+3;p<colon;p++)
				{
					if(!isspace(*p))
					{
						p = NULL; //unexpected header
						break;
					}
				}
				if(p) {
					p = colon + 1;
					while(isspace(*p))
						p++;
					n = 0;
					while(p[n] && !isspace(p[n]))
						n++;
					h->req_SID = p;
					h->req_SIDLen = n;
				}
			}
			else if(strncasecmp(line, "NT", 2)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				n = 0;
				while(p[n] && !isspace(p[n]))
					n++;
				h->req_NT = p;
				h->req_NTLen = n;
			}
			/* Timeout: Seconds-nnnn */
			/* TIMEOUT
			Recommended. Requested duration until subscription expires,
			either number of seconds or infinite. Recommendation
			by a UPnP Forum working committee. Defined by UPnP vendor.
			Consists of the keyword "Second-" followed (without an
			intervening space) by either an integer or the keyword "infinite". */
			else if(strncasecmp(line, "Timeout", 7)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "Second-", 7)==0) {
					h->req_Timeout = atoi(p+7);
				}
			}
			// Range: bytes=xxx-yyy
			else if(strncasecmp(line, "Range", 5)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "bytes=", 6)==0) {
					h->reqflags |= FLAG_RANGE;
					h->req_RangeStart = strtoll(p+6, &colon, 10);
					h->req_RangeEnd = colon ? atoll(colon+1) : 0;
					DPRINTF(E_DEBUG, L_HTTP, "Range Start-End: %lld - %lld\n",
						(long long)h->req_RangeStart,
						h->req_RangeEnd ? (long long)h->req_RangeEnd : -1);
				}
			}
			else if(strncasecmp(line, "Host", 4)==0)
			{
				int i;
				h->reqflags |= FLAG_HOST;
				p = colon + 1;
				while(isspace(*p))
					p++;
				for(n = 0; n<n_lan_addr; n++)
				{
					for(i=0; lan_addr[n].str[i]; i++)
					{
						if(lan_addr[n].str[i] != p[i])
							break;
					}
					if(!lan_addr[n].str[i])
					{
						h->iface = n;
						break;
					}
				}
			}
			else if(strncasecmp(line, "Transfer-Encoding", 17)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "chunked", 7)==0)
				{
					h->reqflags |= FLAG_CHUNKED;
				}
			}
			else if(strncasecmp(line, "Accept-Language", 15)==0)
			{
				h->reqflags |= FLAG_LANGUAGE;
			}
			else if(strncasecmp(line, "getcontentFeatures.dlna.org", 27)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if( (*p != '1') || !isspace(p[1]) )
					h->reqflags |= FLAG_INVALID_REQ;
			}
			else if(strncasecmp(line, "TimeSeekRange.dlna.org", 22)==0)
			{
				h->reqflags |= FLAG_TIMESEEK;
			}
			else if(strncasecmp(line, "PlaySpeed.dlna.org", 18)==0)
			{
				h->reqflags |= FLAG_PLAYSPEED;
			}
			else if(strncasecmp(line, "realTimeInfo.dlna.org", 21)==0)
			{
				h->reqflags |= FLAG_REALTIMEINFO;
			}
			else if(strncasecmp(line, "getAvailableSeekRange.dlna.org", 21)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if( (*p != '1') || !isspace(p[1]) )
					h->reqflags |= FLAG_INVALID_REQ;
			}
			else if(strncasecmp(line, "transferMode.dlna.org", 21)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "Streaming", 9)==0)
				{
					h->reqflags |= FLAG_XFERSTREAMING;
				}
				if(strncasecmp(p, "Interactive", 11)==0)
				{
					h->reqflags |= FLAG_XFERINTERACTIVE;
				}
				if(strncasecmp(p, "Background", 10)==0)
				{
					h->reqflags |= FLAG_XFERBACKGROUND;
				}
			}
			else if(strncasecmp(line, "getCaptionInfo.sec", 18)==0)
			{
				h->reqflags |= FLAG_CAPTION;
			}
			else if(strncasecmp(line, "uctt.upnp.org:", 14)==0)
			{
				/* Conformance testing */
				SETFLAG(DLNA_STRICT_MASK);
			}
		}
		line = strstr(line, "\r\n");
		if (!line)
			return;
		line += 2;
	}
	if( h->reqflags & FLAG_CHUNKED )
	{
		char *endptr;
		h->req_chunklen = -1;
		if( h->req_buflen <= h->req_contentoff )
			return;
		while( (line < (h->req_buf + h->req_buflen)) &&
		       (h->req_chunklen = strtol(line, &endptr, 16)) &&
		       (endptr != line) )
		{
			endptr = strstr(endptr, "\r\n");
			if (!endptr)
			{
				return;
			}
			line = endptr+h->req_chunklen+2;
		}

		if( endptr == line )
		{
			h->req_chunklen = -1;
			return;
		}
	}
}

/* very minimalistic 400 error message */
static void
Send400(struct upnphttp * h)
{
	static const char body400[] =
		"<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>"
		"<BODY><H1>Bad Request</H1>The request is invalid"
		" for this HTTP version.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 400, "Bad Request",
	                    body400, sizeof(body400) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

/* very minimalistic 403 error message */
static void
Send403(struct upnphttp * h)
{
	static const char body403[] =
		"<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>"
		"<BODY><H1>Forbidden</H1>You don't have permission to access this resource."
		"</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 403, "Forbidden",
	                    body403, sizeof(body403) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

/* very minimalistic 404 error message */
static void
Send404(struct upnphttp * h)
{
	static const char body404[] =
		"<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>"
		"<BODY><H1>Not Found</H1>The requested URL was not found"
		" on this server.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 404, "Not Found",
	                    body404, sizeof(body404) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

/* very minimalistic 406 error message */
static void
Send406(struct upnphttp * h)
{
	static const char body406[] =
		"<HTML><HEAD><TITLE>406 Not Acceptable</TITLE></HEAD>"
		"<BODY><H1>Not Acceptable</H1>An unsupported operation"
		" was requested.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 406, "Not Acceptable",
	                    body406, sizeof(body406) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

/* very minimalistic 416 error message */
static void
Send416(struct upnphttp * h)
{
	static const char body416[] =
		"<HTML><HEAD><TITLE>416 Requested Range Not Satisfiable</TITLE></HEAD>"
		"<BODY><H1>Requested Range Not Satisfiable</H1>The requested range"
		" was outside the file's size.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 416, "Requested Range Not Satisfiable",
	                    body416, sizeof(body416) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

/* very minimalistic 500 error message */
void
Send500(struct upnphttp * h)
{
	static const char body500[] = 
		"<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>"
		"<BODY><H1>Internal Server Error</H1>Server encountered "
		"and Internal Error.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 500, "Internal Server Errror",
	                    body500, sizeof(body500) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

/* very minimalistic 501 error message */
void
Send501(struct upnphttp * h)
{
	static const char body501[] = 
		"<HTML><HEAD><TITLE>501 Not Implemented</TITLE></HEAD>"
		"<BODY><H1>Not Implemented</H1>The HTTP Method "
		"is not implemented by this server.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 501, "Not Implemented",
	                    body501, sizeof(body501) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

/* Sends the description generated by the parameter */
static void
sendXMLdesc(struct upnphttp * h, char * (f)(int *))
{
	char * desc;
	int len;
	desc = f(&len);
	if(!desc)
	{
		DPRINTF(E_ERROR, L_HTTP, "Failed to generate XML description\n");
		Send500(h);
		return;
	}
	BuildResp_upnphttp(h, desc, len);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
	free(desc);
}

/* ProcessHTTPPOST_upnphttp()
 * executes the SOAP query if it is possible */
static void
ProcessHTTPPOST_upnphttp(struct upnphttp * h)
{
	if((h->req_buflen - h->req_contentoff) >= h->req_contentlen)
	{
		if(h->req_soapAction)
		{
			/* we can process the request */
			DPRINTF(E_DEBUG, L_HTTP, "SOAPAction: %.*s\n", h->req_soapActionLen, h->req_soapAction);
			ExecuteSoapAction(h, 
				h->req_soapAction,
				h->req_soapActionLen);
		}
		else
		{
			static const char err400str[] =
				"<html><body>Bad request</body></html>";
			DPRINTF(E_WARN, L_HTTP, "No SOAPAction in HTTP headers\n");
			h->respflags = FLAG_HTML;
			BuildResp2_upnphttp(h, 400, "Bad Request",
			                    err400str, sizeof(err400str) - 1);
			SendResp_upnphttp(h);
			CloseSocket_upnphttp(h);
		}
	}
	else
	{
		/* waiting for remaining data */
		h->state = 1;
	}
}

/* Parse and process Http Query 
 * called once all the HTTP headers have been received. */
static void
ProcessHttpQuery_upnphttp(struct upnphttp * h)
{
	char HttpCommand[16];
	char HttpUrl[512];
	char * HttpVer;
	char * p;
	int i;
	p = h->req_buf;
	if(!p)
		return;
	for(i = 0; i<15 && *p && *p != ' ' && *p != '\r'; i++)
		HttpCommand[i] = *(p++);
	HttpCommand[i] = '\0';
	while(*p==' ')
		p++;
	for(i = 0; i<511 && *p && *p != ' ' && *p != '\r'; i++)
		HttpUrl[i] = *(p++);
	HttpUrl[i] = '\0';
	while(*p==' ')
		p++;
	HttpVer = h->HttpVer;
	for(i = 0; i<15 && *p && *p != '\r'; i++)
		HttpVer[i] = *(p++);
	HttpVer[i] = '\0';

	/* set the interface here initially, in case there is no Host header */
	for(i = 0; i<n_lan_addr; i++)
	{
		if( (h->clientaddr.s_addr & lan_addr[i].mask.s_addr)
		   == (lan_addr[i].addr.s_addr & lan_addr[i].mask.s_addr))
		{
			h->iface = i;
			break;
		}
	}

	ParseHttpHeaders(h);

	/* see if we need to wait for remaining data */
	if( (h->reqflags & FLAG_CHUNKED) )
	{
		if( h->req_chunklen == -1)
		{
			Send400(h);
			return;
		}
		if( h->req_chunklen )
		{
			h->state = 2;
			return;
		}
		char *chunkstart, *chunk, *endptr, *endbuf;
		chunk = endbuf = chunkstart = h->req_buf + h->req_contentoff;

		while( (h->req_chunklen = strtol(chunk, &endptr, 16)) && (endptr != chunk) )
		{
			endptr = strstr(endptr, "\r\n");
			if (!endptr)
			{
				Send400(h);
				return;
			}
			endptr += 2;

			memmove(endbuf, endptr, h->req_chunklen);

			endbuf += h->req_chunklen;
			chunk = endptr + h->req_chunklen;
		}
		h->req_contentlen = endbuf - chunkstart;
		h->req_buflen = endbuf - h->req_buf;
		h->state = 100;
	}

	DPRINTF(E_DEBUG, L_HTTP, "HTTP REQUEST: %.*s\n", h->req_buflen, h->req_buf);
	if(strcmp("POST", HttpCommand) == 0)
	{
		h->req_command = EPost;
		ProcessHTTPPOST_upnphttp(h);
	}
	else if((strcmp("GET", HttpCommand) == 0) || (strcmp("HEAD", HttpCommand) == 0))
	{
		if( ((strcmp(h->HttpVer, "HTTP/1.1")==0) && !(h->reqflags & FLAG_HOST)) || (h->reqflags & FLAG_INVALID_REQ) )
		{
			DPRINTF(E_WARN, L_HTTP, "Invalid request, responding ERROR 400.  (No Host specified in HTTP headers?)\n");
			Send400(h);
			return;
		}
		/* 7.3.33.4 */
		else if( (h->reqflags & (FLAG_TIMESEEK|FLAG_PLAYSPEED)) &&
		         !(h->reqflags & FLAG_RANGE) )
		{
			DPRINTF(E_WARN, L_HTTP, "DLNA %s requested, responding ERROR 406\n",
				h->reqflags&FLAG_TIMESEEK ? "TimeSeek" : "PlaySpeed");
			Send406(h);
			return;
		}
		else if(strcmp("GET", HttpCommand) == 0)
		{
			h->req_command = EGet;
		}
		else
		{
			h->req_command = EHead;
		}
		if(strcmp(ROOTDESC_PATH, HttpUrl) == 0)
		{

			sendXMLdesc(h, genRootDesc);
		}
		else if(strcmp(CONTENTDIRECTORY_PATH, HttpUrl) == 0)
		{
			sendXMLdesc(h, genContentDirectory);
		}
		else if(strcmp(CONNECTIONMGR_PATH, HttpUrl) == 0)
		{
			sendXMLdesc(h, genConnectionManager);
		}
		else if(strncmp(HttpUrl, "/MediaItems/", 12) == 0)
		{
			SendResp_dlnafile(h, HttpUrl+12);
		}
		else
		{
			DPRINTF(E_WARN, L_HTTP, "%s not found, responding ERROR 404\n", HttpUrl);
			Send404(h);
		}
	}
	else
	{
		DPRINTF(E_WARN, L_HTTP, "Unsupported HTTP Command %s\n", HttpCommand);
		Send501(h);
	}
}


void
Process_upnphttp(struct upnphttp * h)
{
	char buf[2048];
	int n;
	if(!h)
		return;
	switch(h->state)
	{
	case 0:
		n = recv(h->socket, buf, 2048, 0);
		if(n<0)
		{
			DPRINTF(E_ERROR, L_HTTP, "recv (state0): %s\n", strerror(errno));
			h->state = 100;
		}
		else if(n==0)
		{
			DPRINTF(E_WARN, L_HTTP, "HTTP Connection closed unexpectedly\n");
			h->state = 100;
		}
		else
		{
			int new_req_buflen;
			const char * endheaders;
			/* if 1st arg of realloc() is null,
			 * realloc behaves the same as malloc() */
			new_req_buflen = n + h->req_buflen + 1;
			if (new_req_buflen >= 1024 * 1024)
			{
				DPRINTF(E_ERROR, L_HTTP, "Receive headers too large (received %d bytes)\n", new_req_buflen);
				h->state = 100;
				break;
			}
			h->req_buf = (char *)realloc(h->req_buf, new_req_buflen);
			if (!h->req_buf)
			{
				DPRINTF(E_ERROR, L_HTTP, "Receive headers: %s\n", strerror(errno));
				h->state = 100;
				break;
			}
			memcpy(h->req_buf + h->req_buflen, buf, n);
			h->req_buflen += n;
			h->req_buf[h->req_buflen] = '\0';
			/* search for the string "\r\n\r\n" */
			endheaders = strstr(h->req_buf, "\r\n\r\n");
			if(endheaders)
			{
				h->req_contentoff = endheaders - h->req_buf + 4;
				h->req_contentlen = h->req_buflen - h->req_contentoff;
				ProcessHttpQuery_upnphttp(h);
			}
		}
		break;
	case 1:
	case 2:
		n = recv(h->socket, buf, sizeof(buf), 0);
		if(n < 0)
		{
			DPRINTF(E_ERROR, L_HTTP, "recv (state%d): %s\n", h->state, strerror(errno));
			h->state = 100;
		}
		else if(n == 0)
		{
			DPRINTF(E_WARN, L_HTTP, "HTTP Connection closed unexpectedly\n");
			h->state = 100;
		}
		else
		{
			buf[sizeof(buf)-1] = '\0';
			/*fwrite(buf, 1, n, stdout);*/	/* debug */
			h->req_buf = (char *)realloc(h->req_buf, n + h->req_buflen);
			if (!h->req_buf)
			{
				DPRINTF(E_ERROR, L_HTTP, "Receive request body: %s\n", strerror(errno));
				h->state = 100;
				break;
			}
			memcpy(h->req_buf + h->req_buflen, buf, n);
			h->req_buflen += n;
			if((h->req_buflen - h->req_contentoff) >= h->req_contentlen)
			{
				/* Need the struct to point to the realloc'd memory locations */
				if( h->state == 1 )
				{
					ParseHttpHeaders(h);
					ProcessHTTPPOST_upnphttp(h);
				}
				else if( h->state == 2 )
				{
					ProcessHttpQuery_upnphttp(h);
				}
			}
		}
		break;
	default:
		DPRINTF(E_WARN, L_HTTP, "Unexpected state: %d\n", h->state);
	}
}

/* with response code and response message
 * also allocate enough memory */

void
BuildHeader_upnphttp(struct upnphttp * h, int respcode,
                     const char * respmsg,
                     int bodylen)
{
	static const char httpresphead[] =
		"%s %d %s\r\n"
		"Content-Type: %s\r\n"
		"Connection: close\r\n"
		"Content-Length: %d\r\n"
		"Server: " MINIDLNA_SERVER_STRING "\r\n";
	time_t curtime = time(NULL);
	char date[30];
	int templen;
	struct string_s res;
	if(!h->res_buf)
	{
		templen = sizeof(httpresphead) + 256 + bodylen;
		h->res_buf = (char *)malloc(templen);
		h->res_buf_alloclen = templen;
	}
	res.data = h->res_buf;
	res.size = h->res_buf_alloclen;
	res.off = 0;
	strcatf(&res, httpresphead, "HTTP/1.1",
	              respcode, respmsg,
	              (h->respflags&FLAG_HTML)?"text/html":"text/xml; charset=\"utf-8\"",
							 bodylen);
	/* Additional headers */
	if(h->respflags & FLAG_TIMEOUT) {
		strcatf(&res, "Timeout: Second-");
		if(h->req_Timeout) {
			strcatf(&res, "%d\r\n", h->req_Timeout);
		} else {
			strcatf(&res, "300\r\n");
		}
	}
	if(h->respflags & FLAG_SID) {
		strcatf(&res, "SID: %.*s\r\n", h->req_SIDLen, h->req_SID);
	}
	if(h->reqflags & FLAG_LANGUAGE) {
		strcatf(&res, "Content-Language: en\r\n");
	}
	strftime(date, 30,"%a, %d %b %Y %H:%M:%S GMT" , gmtime(&curtime));
	strcatf(&res, "Date: %s\r\n", date);
	strcatf(&res, "EXT:\r\n");
	strcatf(&res, "\r\n");
	h->res_buflen = res.off;
	if(h->res_buf_alloclen < (h->res_buflen + bodylen))
	{
		h->res_buf = (char *)realloc(h->res_buf, (h->res_buflen + bodylen));
		h->res_buf_alloclen = h->res_buflen + bodylen;
	}
}

void
BuildResp2_upnphttp(struct upnphttp * h, int respcode,
                    const char * respmsg,
                    const char * body, int bodylen)
{
	BuildHeader_upnphttp(h, respcode, respmsg, bodylen);
	if( h->req_command == EHead )
		return;
	if(body)
		memcpy(h->res_buf + h->res_buflen, body, bodylen);
	h->res_buflen += bodylen;
}

/* responding 200 OK ! */
void
BuildResp_upnphttp(struct upnphttp *h, const char *body, int bodylen)
{
	BuildResp2_upnphttp(h, 200, "OK", body, bodylen);
}

void
SendResp_upnphttp(struct upnphttp * h)
{
	int n;
	DPRINTF(E_DEBUG, L_HTTP, "HTTP RESPONSE: %.*s\n", h->res_buflen, h->res_buf);
	n = send(h->socket, h->res_buf, h->res_buflen, 0);
	if(n<0)
	{
		DPRINTF(E_ERROR, L_HTTP, "send(res_buf): %s\n", strerror(errno));
	}
	else if(n < h->res_buflen)
	{
		/* TODO : handle correctly this case */
		DPRINTF(E_ERROR, L_HTTP, "send(res_buf): %d bytes sent (out of %d)\n",
						n, h->res_buflen);
	}
}

static int
send_data(struct upnphttp * h, char * header, size_t size, int flags)
{
	int n;

	n = send(h->socket, header, size, flags);
	if(n<0)
	{
		DPRINTF(E_ERROR, L_HTTP, "send(res_buf): %s\n", strerror(errno));
	} 
	else if(n < h->res_buflen)
	{
		/* TODO : handle correctly this case */
		DPRINTF(E_ERROR, L_HTTP, "send(res_buf): %d bytes sent (out of %d)\n",
						n, h->res_buflen);
	}
	else
	{
		return 0;
	}
	return 1;
}

static void
send_file(struct upnphttp * h, int sendfd, off_t offset, off_t end_offset)
{
	off_t send_size;
	off_t ret;
	char *buf = NULL;
#if HAVE_SENDFILE
	int try_sendfile = 1;
#endif

	while( offset <= end_offset )
	{
#if HAVE_SENDFILE
		if( try_sendfile )
		{
			send_size = ( ((end_offset - offset) < MAX_BUFFER_SIZE) ? (end_offset - offset + 1) : MAX_BUFFER_SIZE);
			ret = sys_sendfile(h->socket, sendfd, &offset, send_size);
			if( ret == -1 )
			{
				DPRINTF(E_DEBUG, L_HTTP, "sendfile error :: error no. %d [%s]\n", errno, strerror(errno));
				/* If sendfile isn't supported on the filesystem, don't bother trying to use it again. */
				if( errno == EOVERFLOW || errno == EINVAL )
					try_sendfile = 0;
				else if( errno != EAGAIN )
					break;
			}
			else
			{
				//DPRINTF(E_DEBUG, L_HTTP, "sent %lld bytes to %d. offset is now %lld.\n", ret, h->socket, offset);
				continue;
			}
		}
#endif
		/* Fall back to regular I/O */
		if( !buf )
			buf = malloc(MIN_BUFFER_SIZE);
		send_size = (((end_offset - offset) < MIN_BUFFER_SIZE) ? (end_offset - offset + 1) : MIN_BUFFER_SIZE);
		lseek(sendfd, offset, SEEK_SET);
		ret = read(sendfd, buf, send_size);
		if( ret == -1 ) {
			DPRINTF(E_DEBUG, L_HTTP, "read error :: error no. %d [%s]\n", errno, strerror(errno));
			if( errno == EAGAIN )
				continue;
			else
				break;
		}
		ret = write(h->socket, buf, ret);
		if( ret == -1 ) {
			DPRINTF(E_DEBUG, L_HTTP, "write error :: error no. %d [%s]\n", errno, strerror(errno));
			if( errno == EAGAIN )
				continue;
			else
				break;
		}
		offset += ret;
	}
	free(buf);
}

static void
start_dlna_header(struct string_s *str, int respcode, const char *tmode, const char *mime)
{
	char date[30];
	time_t now;

	now = time(NULL);
	strftime(date, sizeof(date),"%a, %d %b %Y %H:%M:%S GMT" , gmtime(&now));
	strcatf(str, "HTTP/1.1 %d OK\r\n"
	             "Connection: close\r\n"
	             "Date: %s\r\n"
	             "Server: " MINIDLNA_SERVER_STRING "\r\n"
	             "EXT:\r\n"
	             "realTimeInfo.dlna.org: DLNA.ORG_TLAG=*\r\n"
	             "transferMode.dlna.org: %s\r\n"
	             "Content-Type: %s\r\n",
	             respcode, date, tmode, mime);
}

static int
_open_file(const char *orig_path)
{
	struct media_dir_s *media_path;
	char buf[PATH_MAX];
	const char *path;
	int fd;

	if (!GETFLAG(WIDE_LINKS_MASK))
	{
		path = realpath(orig_path, buf);
		if (!path)
		{
			DPRINTF(E_ERROR, L_HTTP, "Error resolving path %s: %s\n",
						orig_path, strerror(errno));
			return -1;
		}

		for (media_path = media_dirs; media_path; media_path = media_path->next)
		{
			if (strncmp(path, media_path->path, strlen(media_path->path)) == 0)
				break;
		}
		if (!media_path && strncmp(path, db_path, strlen(db_path)))
		{
			DPRINTF(E_ERROR, L_HTTP, "Rejecting wide link %s -> %s\n",
						orig_path, path);
			return -403;
		}
	}
	else
		path = orig_path;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		DPRINTF(E_ERROR, L_HTTP, "Error opening %s\n", path);

	return fd;
}

static void
SendResp_dlnafile(struct upnphttp *h, char *object)
{
	char header[1024];
	struct string_s str;
	char buf[128];
	char **result;
	int rows, ret;
	off_t total, offset, size;
	int64_t id;
	int sendfh;
	uint32_t dlna_flags = DLNA_FLAG_DLNA_V1_5|DLNA_FLAG_HTTP_STALLING|DLNA_FLAG_TM_B;
	const char *tmode;
	static struct { int64_t id;
	                char path[PATH_MAX];
	                char mime[32];
	                char dlna[96];
	              } last_file = { 0 };
	pid_t newpid = 0;

	id = strtoll(object, NULL, 10);
	if( id != last_file.id )
	{
		snprintf(buf, sizeof(buf), "SELECT PATH, MIME, DLNA_PN from DETAILS where ID = '%lld'", (long long)id);
		ret = sql_get_table(db, buf, &result, &rows, NULL);
		if( (ret != SQLITE_OK) )
		{
			DPRINTF(E_ERROR, L_HTTP, "Didn't find valid file for %lld!\n", (long long)id);
			Send500(h);
			return;
		}
		if( !rows || !result[3] || !result[4] )
		{
			DPRINTF(E_WARN, L_HTTP, "%s not found, responding ERROR 404\n", object);
			sqlite3_free_table(result);
			Send404(h);
			return;
		}
		/* Cache the result */
		last_file.id = id;
		strncpy(last_file.path, result[3], sizeof(last_file.path)-1);
		if( result[4] )
		{
			strncpy(last_file.mime, result[4], sizeof(last_file.mime)-1);
		}
		if( result[5] )
			snprintf(last_file.dlna, sizeof(last_file.dlna), "DLNA.ORG_PN=%s;", result[5]);
		else
			last_file.dlna[0] = '\0';
		sqlite3_free_table(result);
	}
	newpid = process_fork();
	if( newpid > 0 )
	{
		CloseSocket_upnphttp(h);
		return;
	}

	DPRINTF(E_INFO, L_HTTP, "Serving DetailID: %lld [%s]\n", (long long)id, last_file.path);

	if( h->reqflags & FLAG_XFERSTREAMING )
	{
		if( strncmp(last_file.mime, "image", 5) == 0 )
		{
			DPRINTF(E_WARN, L_HTTP, "Client tried to specify transferMode as Streaming with an image!\n");
			Send406(h);
			goto error;
		}
	}
	else if( h->reqflags & FLAG_XFERINTERACTIVE )
	{
		if( h->reqflags & FLAG_REALTIMEINFO )
		{
			DPRINTF(E_WARN, L_HTTP, "Bad realTimeInfo flag with Interactive request!\n");
			Send400(h);
			goto error;
		}
		if( strncmp(last_file.mime, "image", 5) != 0 )
		{
			DPRINTF(E_WARN, L_HTTP, "Client tried to specify transferMode as Interactive without an image!\n");
			/* Samsung TVs (well, at least the A950) do this for some reason,
			 * and I don't see them fixing this bug any time soon. */
			if(GETFLAG(DLNA_STRICT_MASK) )
			{
				Send406(h);
				goto error;
			}
		}
	}

	offset = h->req_RangeStart;
	sendfh = _open_file(last_file.path);
	if( sendfh < 0 ) {
		if (sendfh == -403)
			Send403(h);
		else
			Send404(h);
		goto error;
	}
	size = lseek(sendfh, 0, SEEK_END);
	lseek(sendfh, 0, SEEK_SET);

	INIT_STR(str, header);

	if( (h->reqflags & FLAG_XFERBACKGROUND) && (setpriority(PRIO_PROCESS, 0, 19) == 0) )
		tmode = "Background";
	else if( strncmp(last_file.mime, "image", 5) == 0 )
		tmode = "Interactive";
	else
		tmode = "Streaming";

	start_dlna_header(&str, (h->reqflags & FLAG_RANGE ? 206 : 200), tmode, last_file.mime);

	if( h->reqflags & FLAG_RANGE )
	{
		if( !h->req_RangeEnd || h->req_RangeEnd == size )
		{
			h->req_RangeEnd = size - 1;
		}
		if( (h->req_RangeStart > h->req_RangeEnd) || (h->req_RangeStart < 0) )
		{
			DPRINTF(E_WARN, L_HTTP, "Specified range was invalid!\n");
			Send400(h);
			close(sendfh);
			goto error;
		}
		if( h->req_RangeEnd >= size )
		{
			DPRINTF(E_WARN, L_HTTP, "Specified range was outside file boundaries!\n");
			Send416(h);
			close(sendfh);
			goto error;
		}

		total = h->req_RangeEnd - h->req_RangeStart + 1;
		strcatf(&str, "Content-Length: %jd\r\n"
		              "Content-Range: bytes %jd-%jd/%jd\r\n",
		              (intmax_t)total, (intmax_t)h->req_RangeStart,
		              (intmax_t)h->req_RangeEnd, (intmax_t)size);
	}
	else
	{
		h->req_RangeEnd = size - 1;
		total = size;
		strcatf(&str, "Content-Length: %jd\r\n", (intmax_t)total);
	}

	switch( *last_file.mime )
	{
		case 'i':
			dlna_flags |= DLNA_FLAG_TM_I;
			break;
		case 'a':
		case 'v':
		default:
			dlna_flags |= DLNA_FLAG_TM_S;
			break;
	}

	if( h->reqflags & FLAG_CAPTION )
	{
		if( sql_get_int_field(db, "SELECT ID from CAPTIONS where ID = '%lld'", (long long)id) > 0 )
			strcatf(&str, "CaptionInfo.sec: http://%s:%d/Captions/%lld.srt\r\n",
			              lan_addr[h->iface].str, runtime_vars.port, (long long)id);
	}

	strcatf(&str, "Accept-Ranges: bytes\r\n"
	              "contentFeatures.dlna.org: %sDLNA.ORG_OP=%02X;DLNA.ORG_CI=%X;DLNA.ORG_FLAGS=%08X%024X\r\n\r\n",
	              last_file.dlna, 1, 0, dlna_flags, 0);

	//DEBUG DPRINTF(E_DEBUG, L_HTTP, "RESPONSE: %s\n", str.data);
	if( send_data(h, str.data, str.off, MSG_MORE) == 0 )
	{
		if( h->req_command != EHead )
			send_file(h, sendfh, offset, h->req_RangeEnd);
	}
	close(sendfh);

	CloseSocket_upnphttp(h);
error:
	if( newpid == 0 )
		_exit(0);
	return;
}
