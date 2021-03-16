/* MiniUPnP project
 * http://miniupnp.free.fr/ or http://miniupnp.tuxfamily.org/
 *
 * Copyright (c) 2006-2008, Thomas Bernard
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "getifaddr.h"
#include "upnpdescgen.h"
#include "minidlnapath.h"
#include "upnpglobalvars.h"

#undef DESC_DEBUG

static const char * const upnptypes[] =
{
	"string",
	"boolean",
	"ui2",
	"ui4",
	"i4",
	"uri",
	"int",
	"bin.base64"
};

static const char * const upnpdefaultvalues[] =
{
	0,
	"Unconfigured"
};

static const char * const upnpallowedvalues[] =
{
	0,			/* 0 */
	"DSL",			/* 1 */
	"POTS",
	"Cable",
	"Ethernet",
	0,
	"Up",			/* 6 */
	"Down",
	"Initializing",
	"Unavailable",
	0,
	"TCP",			/* 11 */
	"UDP",
	0,
	"Unconfigured",		/* 14 */
	"IP_Routed",
	"IP_Bridged",
	0,
	"Unconfigured",		/* 18 */
	"Connecting",
	"Connected",
	"PendingDisconnect",
	"Disconnecting",
	"Disconnected",
	0,
	"ERROR_NONE",		/* 25 */
	0,
	"OK",			/* 27 */
	"ContentFormatMismatch",
	"InsufficientBandwidth",
	"UnreliableChannel",
	"Unknown",
	0,
	"Input",		/* 33 */
	"Output",
	0,
	"BrowseMetadata",	/* 36 */
	"BrowseDirectChildren",
	0,
	"COMPLETED",		/* 39 */
	"ERROR",
	"IN_PROGRESS",
	"STOPPED",
	0,
	RESOURCE_PROTOCOL_INFO_VALUES,		/* 44 */
	0,
	"0",			/* 46 */
	0,
	"",			/* 48 */
	0
};

static const char xmlver[] = 
	"<?xml version=\"1.0\"?>\r\n";
static const char root_service[] =
	"scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"";
static const char root_device[] = 
	"root xmlns=\"urn:schemas-upnp-org:device-1-0\"";

/* root Description of the UPnP Device */
static const struct XMLElt rootDesc[] =
{
	{root_device, INITHELPER(1,2)},
	{"specVersion", INITHELPER(3,2)},
	{"device", INITHELPER(5,(13))},
	{"/major", "1"},
	{"/minor", "0"},
	{"/deviceType", "urn:schemas-upnp-org:device:MediaServer:1"},
	{"/friendlyName", friendly_name},	/* required */
	{"/manufacturer", ROOTDEV_MANUFACTURER},		/* required */
	{"/manufacturerURL", ROOTDEV_MANUFACTURERURL},	/* optional */
	{"/modelDescription", ROOTDEV_MODELDESCRIPTION}, /* recommended */
	{"/modelName", modelname},	/* required */
	{"/modelNumber", modelnumber},
	{"/modelURL", ROOTDEV_MODELURL},
	{"/serialNumber", serialnumber},
	{"/UDN", uuidvalue},	/* required */
	{"/dlna:X_DLNADOC xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\"", "DMS-1.50"},
	{"/presentationURL", presentationurl},	/* recommended */
	{"serviceList", INITHELPER(18,2)},
	{"service", INITHELPER(20,5)},
	{"service", INITHELPER(25,5)},
	{"/serviceType", "urn:schemas-upnp-org:service:ContentDirectory:1"},
	{"/serviceId", "urn:upnp-org:serviceId:ContentDirectory"},
	{"/controlURL", CONTENTDIRECTORY_CONTROLURL},
	{"/eventSubURL", CONTENTDIRECTORY_EVENTURL},
	{"/SCPDURL", CONTENTDIRECTORY_PATH},
	{"/serviceType", "urn:schemas-upnp-org:service:ConnectionManager:1"},
	{"/serviceId", "urn:upnp-org:serviceId:ConnectionManager"},
	{"/controlURL", CONNECTIONMGR_CONTROLURL},
	{"/eventSubURL", CONNECTIONMGR_EVENTURL},
	{"/SCPDURL", CONNECTIONMGR_PATH},
	{0, 0}
};

/* For ConnectionManager */
static const struct argument GetProtocolInfoArgs[] =
{
	{"Source", 2, 0},
	{"Sink", 2, 1},
	{NULL, 0, 0}
};

static const struct argument GetCurrentConnectionIDsArgs[] =
{
	{"ConnectionIDs", 2, 2},
	{NULL, 0, 0}
};

static const struct argument GetCurrentConnectionInfoArgs[] =
{
	{"ConnectionID", 1, 7},
	{"RcsID", 2, 9},
	{"AVTransportID", 2, 8},
	{"ProtocolInfo", 2, 6},
	{"PeerConnectionManager", 2, 4},
	{"PeerConnectionID", 2, 7},
	{"Direction", 2, 5},
	{"Status", 2, 3},
	{NULL, 0, 0}
};

static const struct action ConnectionManagerActions[] =
{
	{"GetProtocolInfo", GetProtocolInfoArgs}, /* R */
	{"GetCurrentConnectionIDs", GetCurrentConnectionIDsArgs}, /* R */
	{"GetCurrentConnectionInfo", GetCurrentConnectionInfoArgs}, /* R */
	{0, 0}
};

static const struct stateVar ConnectionManagerVars[] =
{
	{"SourceProtocolInfo", 0|EVENTED, 0, 0, 44}, /* required */
	{"SinkProtocolInfo", 0|EVENTED, 0, 0, 48}, /* required */
	{"CurrentConnectionIDs", 0|EVENTED, 0, 0, 46}, /* required */
	{"A_ARG_TYPE_ConnectionStatus", 0, 0, 27}, /* required */
	{"A_ARG_TYPE_ConnectionManager", 0, 0}, /* required */
	{"A_ARG_TYPE_Direction", 0, 0, 33}, /* required */
	{"A_ARG_TYPE_ProtocolInfo", 0, 0}, /* required */
	{"A_ARG_TYPE_ConnectionID", 4, 0}, /* required */
	{"A_ARG_TYPE_AVTransportID", 4, 0}, /* required */
	{"A_ARG_TYPE_RcsID", 4, 0}, /* required */
	{0, 0}
};

static const struct argument GetSearchCapabilitiesArgs[] =
{
	{"SearchCaps", 2, 11},
	{0, 0}
};

static const struct argument GetSortCapabilitiesArgs[] =
{
	{"SortCaps", 2, 12},
	{0, 0}
};

static const struct argument GetSystemUpdateIDArgs[] =
{
	{"Id", 2, 13},
	{0, 0}
};

static const struct argument UpdateObjectArgs[] =
{
	{"ObjectID", 1, 1},
	{"CurrentTagValue", 1, 10},
	{"NewTagValue", 1, 10},
	{0, 0}
};

static const struct argument BrowseArgs[] =
{
	{"ObjectID", 1, 1},
	{"BrowseFlag", 1, 4},
	{"Filter", 1, 5},
	{"StartingIndex", 1, 7},
	{"RequestedCount", 1, 8},
	{"SortCriteria", 1, 6},
	{"Result", 2, 2},
	{"NumberReturned", 2, 8},
	{"TotalMatches", 2, 8},
	{"UpdateID", 2, 9},
	{0, 0}
};

static const struct argument SearchArgs[] =
{
	{"ContainerID", 1, 1},
	{"SearchCriteria", 1, 3},
	{"Filter", 1, 5},
	{"StartingIndex", 1, 7},
	{"RequestedCount", 1, 8},
	{"SortCriteria", 1, 6},
	{"Result", 2, 2},
	{"NumberReturned", 2, 8},
	{"TotalMatches", 2, 8},
	{"UpdateID", 2, 9},
	{0, 0}
};

static const struct action ContentDirectoryActions[] =
{
	{"GetSearchCapabilities", GetSearchCapabilitiesArgs}, /* R */
	{"GetSortCapabilities", GetSortCapabilitiesArgs}, /* R */
	{"GetSystemUpdateID", GetSystemUpdateIDArgs}, /* R */
	{"Browse", BrowseArgs}, /* R */
	{"Search", SearchArgs}, /* O */
	{"UpdateObject", UpdateObjectArgs}, /* O */
	{0, 0}
};

static const struct stateVar ContentDirectoryVars[] =
{
	{"TransferIDs", 0|EVENTED, 0, 0, 48}, /* 0 */
	{"A_ARG_TYPE_ObjectID", 0, 0},
	{"A_ARG_TYPE_Result", 0, 0},
	{"A_ARG_TYPE_SearchCriteria", 0, 0},
	{"A_ARG_TYPE_BrowseFlag", 0, 0, 36},
	/* Allowed Values : BrowseMetadata / BrowseDirectChildren */
	{"A_ARG_TYPE_Filter", 0, 0}, /* 5 */
	{"A_ARG_TYPE_SortCriteria", 0, 0},
	{"A_ARG_TYPE_Index", 3, 0},
	{"A_ARG_TYPE_Count", 3, 0},
	{"A_ARG_TYPE_UpdateID", 3, 0},
	{"A_ARG_TYPE_TagValueList", 0, 0},
	{"SearchCapabilities", 0, 0},
	{"SortCapabilities", 0, 0},
	{"SystemUpdateID", 3|EVENTED, 0, 0, 255},
	{0, 0}
};

static const struct serviceDesc scpdContentDirectory =
{ ContentDirectoryActions, ContentDirectoryVars };

static const struct serviceDesc scpdConnectionManager =
{ ConnectionManagerActions, ConnectionManagerVars };

/* strcat_str()
 * concatenate the string and use realloc to increase the
 * memory buffer if needed. */
static char *
strcat_str(char * str, int * len, int * tmplen, const char * s2)
{
	char *p;
	int s2len;
	s2len = (int)strlen(s2);
	if(*tmplen <= (*len + s2len))
	{
		if(s2len < 256)
			*tmplen += 256;
		else
			*tmplen += s2len + 1;
		p = realloc(str, *tmplen);
		if (!p)
		{
			if(s2len < 256)
				*tmplen -= 256;
			else
				*tmplen -= s2len + 1;
			return str;
		}
		else
			str = p;
	}
	/*strcpy(str + *len, s2); */
	memcpy(str + *len, s2, s2len + 1);
	*len += s2len;
	return str;
}

/* strcat_char() :
 * concatenate a character and use realloc to increase the
 * size of the memory buffer if needed */
static char *
strcat_char(char * str, int * len, int * tmplen, char c)
{
	char *p;
	if(*tmplen <= (*len + 1))
	{
		*tmplen += 256;
		p = (char *)realloc(str, *tmplen);
		if (!p)
		{
			*tmplen -= 256;
			return str;
		}
		else
			str = p;
	}
	str[*len] = c;
	(*len)++;
	return str;
}

/* iterative subroutine using a small stack
 * This way, the progam stack usage is kept low */
static char *
genXML(char * str, int * len, int * tmplen,
                   const struct XMLElt * p)
{
	uint16_t i, j, k;
	int top;
	const char * eltname, *s;
	char c;
	char element[64];
	struct {
		unsigned short i;
		unsigned short j;
		const char * eltname;
	} pile[16]; /* stack */
	top = -1;
	i = 0;	/* current node */
	j = 1;	/* i + number of nodes*/
	for(;;)
	{
		eltname = p[i].eltname;
		if(!eltname)
			return str;
		if(eltname[0] == '/')
		{
			#ifdef DESC_DEBUG
			printf("DBG: <%s>%s<%s>\n", eltname+1, p[i].data, eltname);
			#endif
			str = strcat_char(str, len, tmplen, '<');
			str = strcat_str(str, len, tmplen, eltname+1);
			str = strcat_char(str, len, tmplen, '>');
			str = strcat_str(str, len, tmplen, p[i].data);
			str = strcat_char(str, len, tmplen, '<');
			sscanf(eltname, "%s", element);
			str = strcat_str(str, len, tmplen, element);
			str = strcat_char(str, len, tmplen, '>');
			for(;;)
			{
				if(top < 0)
					return str;
				i = ++(pile[top].i);
				j = pile[top].j;
				#ifdef DESC_DEBUG
				printf("DBG:  pile[%d]\t%d %d\n", top, i, j); 
				#endif
				if(i==j)
				{
					#ifdef DESC_DEBUG
					printf("DBG: i==j, </%s>\n", pile[top].eltname); 
					#endif
					str = strcat_char(str, len, tmplen, '<');
					str = strcat_char(str, len, tmplen, '/');
					s = pile[top].eltname;
					for(c = *s; c > ' '; c = *(++s))
						str = strcat_char(str, len, tmplen, c);
					str = strcat_char(str, len, tmplen, '>');
					top--;
				}
				else
					break;
			}
		}
		else
		{
			#ifdef DESC_DEBUG
			printf("DBG: [%d] <%s>\n", i, eltname); 
			#endif
			str = strcat_char(str, len, tmplen, '<');
			str = strcat_str(str, len, tmplen, eltname);
			str = strcat_char(str, len, tmplen, '>');
			k = i;
			/*i = p[k].index; */
			/*j = i + p[k].nchild; */
			i = (unsigned long)p[k].data & 0xffff;
			j = i + ((unsigned long)p[k].data >> 16);
			top++;
			#ifdef DESC_DEBUG
			printf("DBG: +pile[%d]\t%d %d\n", top, i, j); 
			#endif
			pile[top].i = i;
			pile[top].j = j;
			pile[top].eltname = eltname;
		}
	}
}

/* genRootDesc() :
 * - Generate the root description of the UPnP device.
 * - the len argument is used to return the length of
 *   the returned string. 
 * - tmp_uuid argument is used to build the uuid string */
char *
genRootDesc(int * len)
{
	char * str;
	int tmplen;
	tmplen = 2560;
	str = (char *)malloc(tmplen);
	if(str == NULL)
		return NULL;
	* len = strlen(xmlver);
	memcpy(str, xmlver, *len + 1);
	str = genXML(str, len, &tmplen, rootDesc);
	str[*len] = '\0';
	return str;
}

/* genServiceDesc() :
 * Generate service description with allowed methods and 
 * related variables. */
static char *
genServiceDesc(int * len, const struct serviceDesc * s)
{
	int i, j;
	const struct action * acts;
	const struct stateVar * vars;
	const struct argument * args;
	const char * p;
	char * str;
	int tmplen;
	tmplen = 2048;
	str = (char *)malloc(tmplen);
	if(str == NULL)
		return NULL;
	/*strcpy(str, xmlver); */
	*len = strlen(xmlver);
	memcpy(str, xmlver, *len + 1);
	
	acts = s->actionList;
	vars = s->serviceStateTable;

	str = strcat_char(str, len, &tmplen, '<');
	str = strcat_str(str, len, &tmplen, root_service);
	str = strcat_char(str, len, &tmplen, '>');

	str = strcat_str(str, len, &tmplen,
		"<specVersion><major>1</major><minor>0</minor></specVersion>");

	i = 0;
	str = strcat_str(str, len, &tmplen, "<actionList>");
	while(acts[i].name)
	{
		str = strcat_str(str, len, &tmplen, "<action><name>");
		str = strcat_str(str, len, &tmplen, acts[i].name);
		str = strcat_str(str, len, &tmplen, "</name>");
		/* argument List */
		args = acts[i].args;
		if(args)
		{
			str = strcat_str(str, len, &tmplen, "<argumentList>");
			j = 0;
			while(args[j].dir)
			{
				str = strcat_str(str, len, &tmplen, "<argument><name>");
				p = vars[args[j].relatedVar].name;
				str = strcat_str(str, len, &tmplen, (args[j].name ? args[j].name : p));
				str = strcat_str(str, len, &tmplen, "</name><direction>");
				str = strcat_str(str, len, &tmplen, args[j].dir==1?"in":"out");
				str = strcat_str(str, len, &tmplen,
						"</direction><relatedStateVariable>");
				str = strcat_str(str, len, &tmplen, p);
				str = strcat_str(str, len, &tmplen,
						"</relatedStateVariable></argument>");
				j++;
			}
			str = strcat_str(str, len, &tmplen,"</argumentList>");
		}
		str = strcat_str(str, len, &tmplen, "</action>");
		/*str = strcat_char(str, len, &tmplen, '\n'); // TEMP ! */
		i++;
	}
	str = strcat_str(str, len, &tmplen, "</actionList><serviceStateTable>");
	i = 0;
	while(vars[i].name)
	{
		str = strcat_str(str, len, &tmplen,
				"<stateVariable sendEvents=\"");
		str = strcat_str(str, len, &tmplen, (vars[i].itype & EVENTED)?"yes":"no");
		str = strcat_str(str, len, &tmplen, "\"><name>");
		str = strcat_str(str, len, &tmplen, vars[i].name);
		str = strcat_str(str, len, &tmplen, "</name><dataType>");
		str = strcat_str(str, len, &tmplen, upnptypes[vars[i].itype & 0x0f]);
		str = strcat_str(str, len, &tmplen, "</dataType>");
		if(vars[i].iallowedlist)
		{
		  str = strcat_str(str, len, &tmplen, "<allowedValueList>");
		  for(j=vars[i].iallowedlist; upnpallowedvalues[j]; j++)
		  {
		    str = strcat_str(str, len, &tmplen, "<allowedValue>");
		    str = strcat_str(str, len, &tmplen, upnpallowedvalues[j]);
		    str = strcat_str(str, len, &tmplen, "</allowedValue>");
		  }
		  str = strcat_str(str, len, &tmplen, "</allowedValueList>");
		}
		/*if(vars[i].defaultValue) */
		if(vars[i].idefault)
		{
		  str = strcat_str(str, len, &tmplen, "<defaultValue>");
		  /*str = strcat_str(str, len, &tmplen, vars[i].defaultValue); */
		  str = strcat_str(str, len, &tmplen, upnpdefaultvalues[vars[i].idefault]);
		  str = strcat_str(str, len, &tmplen, "</defaultValue>");
		}
		str = strcat_str(str, len, &tmplen, "</stateVariable>");
		/*str = strcat_char(str, len, &tmplen, '\n'); // TEMP ! */
		i++;
	}
	str = strcat_str(str, len, &tmplen, "</serviceStateTable></scpd>");
	str[*len] = '\0';
	return str;
}

/* genContentDirectory() :
 * Generate the ContentDirectory xml description */
char *
genContentDirectory(int * len)
{
	return genServiceDesc(len, &scpdContentDirectory);
}

/* genConnectionManager() :
 * Generate the ConnectionManager xml description */
char *
genConnectionManager(int * len)
{
	return genServiceDesc(len, &scpdConnectionManager);
}
