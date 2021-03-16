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
#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <fcntl.h>

#include <libexif/exif-loader.h>
#include <jpeglib.h>
#include <setjmp.h>
#include "libav.h"

#include "upnpglobalvars.h"
#include "upnpreplyparse.h"
#include "metadata.h"
#include "utils.h"
#include "sql.h"
#include "log.h"

#define FLAG_TITLE	0x00000001
#define FLAG_MIME	0x00000100

void
GetVideoMetadata(metadata_t * const meta, const char *path, const char *name)
{
	struct stat file;
	int ret, i;
	AVFormatContext *ctx = NULL;
	AVStream *vstream = NULL;
	int video_stream = -1;
	char *path_cpy, *basepath;

	memset(meta, '\0', sizeof(metadata_t));

	DPRINTF(E_DEBUG, L_METADATA, "Parsing video %s...\n", name);
	if ( stat(path, &file) != 0 )
		return;
	DPRINTF(E_DEBUG, L_METADATA, " * size: %jd\n", file.st_size);

	meta->file_size = file.st_size;

	ret = lav_open(&ctx, path);
	if( ret != 0 )
	{
		char err[128];
		av_strerror(ret, err, sizeof(err));
		DPRINTF(E_WARN, L_METADATA, "Opening %s failed! [%s]\n", path, err);
		return;
	}
	for( i=0; i < ctx->nb_streams; i++)
	{
		if( lav_codec_type(ctx->streams[i]) == AVMEDIA_TYPE_VIDEO &&
		         video_stream == -1 )
		{
			video_stream = i;
			vstream = ctx->streams[video_stream];
			continue;
		}
	}
	path_cpy = strdup(path);
	basepath = basename(path_cpy);

	if( vstream )
	{
		DPRINTF(E_DEBUG, L_METADATA, "Container: '%s' [%s]\n", ctx->iformat->name, basepath);

		if( strcmp(ctx->iformat->name, "avi") == 0 )
			sprintf(meta->mime, "video/x-msvideo");
 		else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
			if( ends_with(path, ".mov") )
				sprintf(meta->mime, "video/quicktime");
			else
				sprintf(meta->mime, "video/mp4");
		else if( strncmp(ctx->iformat->name, "matroska", 8) == 0 )
			sprintf(meta->mime, "video/x-matroska");
		else if( strcmp(ctx->iformat->name, "flv") == 0 )
			sprintf(meta->mime, "video/x-flv");
		else if( strncmp(ctx->iformat->name, "mpeg", 4) == 0 )
			sprintf(meta->mime, "video/mpeg");
		else
			DPRINTF(E_WARN, L_METADATA, "%s: Unhandled format: %s\n", path, ctx->iformat->name);
	}

	strcpy(meta->title, name);
	strip_ext(meta->title);

	lav_close(ctx);

	free(path_cpy);

	return;
}
