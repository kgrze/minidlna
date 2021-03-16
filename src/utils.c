/* MiniDLNA media server
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
 */
#include "config.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>

#include "minidlnatypes.h"
#include "upnpglobalvars.h"
#include "utils.h"
#include "log.h"

int
xasprintf(char **strp, char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vasprintf(strp, fmt, args);
	va_end(args);
	if( ret < 0 )
	{
		DPRINTF(E_WARN, L_GENERAL, "xasprintf: allocation failed\n");
		*strp = NULL;
	}
	return ret;
}

int
ends_with(const char * haystack, const char * needle)
{
	const char * end;
	int nlen = strlen(needle);
	int hlen = strlen(haystack);

	if( nlen > hlen )
		return 0;
	end = haystack + hlen - nlen;

	return (strcasecmp(end, needle) ? 0 : 1);
}

char *
strcasestrc(const char *s, const char *p, const char t)
{
	char *endptr;
	size_t slen, plen;

	endptr = strchr(s, t);
	if (!endptr)
		return strcasestr(s, p);

	plen = strlen(p);
	slen = endptr - s;
	while (slen >= plen)
	{
		if (*s == *p && strncasecmp(s+1, p+1, plen-1) == 0)
			return (char*)s;
		s++;
		slen--;
	}

	return NULL;
} 

char *
modifyString(char *string, const char *before, const char *after, int noalloc)
{
	int oldlen, newlen, chgcnt = 0;
	char *s, *p;

	/* If there is no match, just return */
	s = strstr(string, before);
	if (!s)
		return string;

	oldlen = strlen(before);
	newlen = strlen(after);
	if (newlen > oldlen)
	{
		if (noalloc)
			return string;

		while ((p = strstr(s, before)))
		{
			chgcnt++;
			s = p + oldlen;
		}
		s = realloc(string, strlen(string)+((newlen-oldlen)*chgcnt)+1);
		/* If we failed to realloc, return the original alloc'd string */
		if( s )
			string = s;
		else
			return string;
	}

	s = string;
	while (s)
	{
		p = strstr(s, before);
		if (!p)
			return string;
		memmove(p + newlen, p + oldlen, strlen(p + oldlen) + 1);
		memcpy(p, after, newlen);
		s = p + newlen;
	}

	return string;
}

char *
escape_tag(const char *tag, int force_alloc)
{
	char *esc_tag = NULL;

	if( strchr(tag, '&') || strchr(tag, '<') || strchr(tag, '>') || strchr(tag, '"') )
	{
		esc_tag = strdup(tag);
		esc_tag = modifyString(esc_tag, "&", "&amp;amp;", 0);
		esc_tag = modifyString(esc_tag, "<", "&amp;lt;", 0);
		esc_tag = modifyString(esc_tag, ">", "&amp;gt;", 0);
		esc_tag = modifyString(esc_tag, "\"", "&amp;quot;", 0);
	}
	else if( force_alloc )
		esc_tag = strdup(tag);

	return esc_tag;
}

char *
strip_ext(char *name)
{
	char *period;

	if (!name)
		return NULL;
	period = strrchr(name, '.');
	if (period)
		*period = '\0';

	return period;
}

/* Code basically stolen from busybox */
int
make_dir(char * path, mode_t mode)
{
	char * s = path;
	char c;
	struct stat st;

	do {
		c = '\0';

		/* Before we do anything, skip leading /'s, so we don't bother
		 * trying to create /. */
		while (*s == '/')
			++s;

		/* Bypass leading non-'/'s and then subsequent '/'s. */
		while (*s) {
			if (*s == '/') {
				do {
					++s;
				} while (*s == '/');
				c = *s;     /* Save the current char */
				*s = '\0';     /* and replace it with nul. */
				break;
			}
			++s;
		}

		if (mkdir(path, mode) < 0) {
			/* If we failed for any other reason than the directory
			 * already exists, output a diagnostic and return -1.*/
			if ((errno != EEXIST && errno != EISDIR)
			    || (stat(path, &st) < 0 || !S_ISDIR(st.st_mode))) {
				DPRINTF(E_WARN, L_GENERAL, "make_dir: cannot create directory '%s'\n", path);
				if (c)
					*s = c;
				return -1;
			}
		}
		if (!c)
			return 0;

		/* Remove any inserted nul from the path. */
		*s = c;

	} while (1);
}

const char *
mime_to_ext(const char * mime)
{
	if( strcmp(mime+6, "avi") == 0 )
		return "avi";
	else if( strcmp(mime+6, "divx") == 0 )
		return "avi";
	else if( strcmp(mime+6, "x-msvideo") == 0 )
		return "avi";
	else if( strcmp(mime+6, "mpeg") == 0 )
		return "mpg";
	else if( strcmp(mime+6, "mp4") == 0 )
		return "mp4";
	else if( strcmp(mime+6, "x-ms-wmv") == 0 )
		return "wmv";
	else if( strcmp(mime+6, "x-matroska") == 0 )
		return "mkv";
	else if( strcmp(mime+6, "x-mkv") == 0 )
		return "mkv";
	else if( strcmp(mime+6, "x-flv") == 0 )
		return "flv";
	else if( strcmp(mime+6, "vnd.dlna.mpeg-tts") == 0 )
		return "mpg";
	else if( strcmp(mime+6, "quicktime") == 0 )
		return "mov";
	else if( strcmp(mime+6, "3gpp") == 0 )
		return "3gp";
	return "dat";
}

int
is_video(const char * file)
{
	return (ends_with(file, ".mpg") || ends_with(file, ".mpeg")  ||
		ends_with(file, ".avi") || ends_with(file, ".divx")  ||
		ends_with(file, ".asf") || ends_with(file, ".wmv")   ||
		ends_with(file, ".mp4") || ends_with(file, ".m4v")   ||
		ends_with(file, ".mts") || ends_with(file, ".m2ts")  ||
		ends_with(file, ".m2t") || ends_with(file, ".mkv")   ||
		ends_with(file, ".vob") || ends_with(file, ".ts")    ||
		ends_with(file, ".flv") || ends_with(file, ".xvid")  ||
		ends_with(file, ".mov") || ends_with(file, ".3gp"));
}

media_types
get_media_type(const char *file)
{
	const char *ext = strrchr(file, '.');
	if (!ext)
		return NO_MEDIA;
	if (is_video(ext))
		return TYPE_VIDEO;
	if (is_nfo(ext))
		return TYPE_NFO;
	return NO_MEDIA;
}

media_types
valid_media_types(const char *path)
{
	struct media_dir_s *media_dir = media_dirs;

	if (strncmp(path, media_dir->path, strlen(media_dir->path)) == 0)
		return media_dir->types;

	return ALL_MEDIA;
}
