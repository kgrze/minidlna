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

/* Audio profile flags */
enum audio_profiles {
	PROFILE_AUDIO_UNKNOWN,
	PROFILE_AUDIO_MP3,
	PROFILE_AUDIO_AC3,
	PROFILE_AUDIO_WMA_BASE,
	PROFILE_AUDIO_WMA_FULL,
	PROFILE_AUDIO_WMA_PRO,
	PROFILE_AUDIO_MP2,
	PROFILE_AUDIO_PCM,
	PROFILE_AUDIO_AAC,
	PROFILE_AUDIO_AAC_MULT5,
	PROFILE_AUDIO_AMR
};

/* This function shamelessly copied from libdlna */
#define MPEG_TS_SYNC_CODE 0x47
#define MPEG_TS_PACKET_LENGTH 188
#define MPEG_TS_PACKET_LENGTH_DLNA 192 /* prepends 4 bytes to TS packet */
int
dlna_timestamp_is_present(const char *filename, int *raw_packet_size)
{
	unsigned char buffer[3*MPEG_TS_PACKET_LENGTH_DLNA];
	int fd, i;

	/* read file header */
	fd = open(filename, O_RDONLY);
	if( fd < 0 )
		return 0;
	i = read(fd, buffer, MPEG_TS_PACKET_LENGTH_DLNA*3);
	close(fd);
	if( i < 0 )
		return 0;
	for( i = 0; i < MPEG_TS_PACKET_LENGTH_DLNA; i++ )
	{
		if( buffer[i] == MPEG_TS_SYNC_CODE )
		{
			if (buffer[i + MPEG_TS_PACKET_LENGTH_DLNA] == MPEG_TS_SYNC_CODE &&
			    buffer[i + MPEG_TS_PACKET_LENGTH_DLNA*2] == MPEG_TS_SYNC_CODE)
			{
				*raw_packet_size = MPEG_TS_PACKET_LENGTH_DLNA;
				if (buffer[i+MPEG_TS_PACKET_LENGTH] == 0x00 &&
				    buffer[i+MPEG_TS_PACKET_LENGTH+1] == 0x00 &&
				    buffer[i+MPEG_TS_PACKET_LENGTH+2] == 0x00 &&
				    buffer[i+MPEG_TS_PACKET_LENGTH+3] == 0x00)
					return 0;
				else
					return 1;
			} else if (buffer[i + MPEG_TS_PACKET_LENGTH] == MPEG_TS_SYNC_CODE &&
				   buffer[i + MPEG_TS_PACKET_LENGTH*2] == MPEG_TS_SYNC_CODE) {
				*raw_packet_size = MPEG_TS_PACKET_LENGTH;
				return 0;
			}
		}
	}
	*raw_packet_size = 0;
	return 0;
}

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
		ts_timestamp_t ts_timestamp = NONE;
		DPRINTF(E_DEBUG, L_METADATA, "Container: '%s' [%s]\n", ctx->iformat->name, basepath);

		/* NOTE: The DLNA spec only provides for ASF (WMV), TS, PS, and MP4 containers.
		 * Skip DLNA parsing for everything else. */
		if( strcmp(ctx->iformat->name, "avi") == 0 )
		{
			sprintf(meta->mime, "video/x-msvideo");
		}
		else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 &&
		         ends_with(path, ".mov") )
			sprintf(meta->mime, "video/quicktime");
		else if( strncmp(ctx->iformat->name, "matroska", 8) == 0 )
			sprintf(meta->mime, "video/x-matroska");
		else if( strcmp(ctx->iformat->name, "flv") == 0 )
			sprintf(meta->mime, "video/x-flv");
		if( meta->mime )
			goto video_no_dlna;

		switch( lav_codec_id(vstream) )
		{
			case AV_CODEC_ID_MPEG1VIDEO:
				if( strcmp(ctx->iformat->name, "mpeg") == 0 )
				{
					sprintf(meta->mime, "video/mpeg");
				}
				break;
			case AV_CODEC_ID_MPEG2VIDEO:
				if( strcmp(ctx->iformat->name, "mpegts") == 0 )
				{
					int raw_packet_size;
					int dlna_ts_present = dlna_timestamp_is_present(path, &raw_packet_size);
					if( raw_packet_size == MPEG_TS_PACKET_LENGTH_DLNA )
					{
						if (dlna_ts_present)
							ts_timestamp = VALID;
						else
							ts_timestamp = EMPTY;
					}
					switch( ts_timestamp )
					{
						case NONE:
							sprintf(meta->mime, "video/mpeg");
							break;
						case VALID:
						case EMPTY:
							sprintf(meta->mime, "video/vnd.dlna.mpeg-tts");
						default:
							break;
					}
				}
				else if( strcmp(ctx->iformat->name, "mpeg") == 0 )
				{
					sprintf(meta->mime, "video/mpeg");
				}
				break;
			case AV_CODEC_ID_H264:
				if( strcmp(ctx->iformat->name, "mpegts") == 0 )
				{
					int raw_packet_size;
					int dlna_ts_present = dlna_timestamp_is_present(path, &raw_packet_size);

					if( raw_packet_size == MPEG_TS_PACKET_LENGTH_DLNA )
					{
						if( lav_profile(vstream) == FF_PROFILE_H264_HIGH ||
						    dlna_ts_present )
							ts_timestamp = VALID;
						else
							ts_timestamp = EMPTY;
					}
					else if( raw_packet_size != MPEG_TS_PACKET_LENGTH )
					{
						DPRINTF(E_DEBUG, L_METADATA, "Unsupported DLNA TS packet size [%d] (%s)\n",
							raw_packet_size, basepath);
					}
					switch( ts_timestamp )
					{
						case NONE:
							break;
						case VALID:
						case EMPTY:
							sprintf(meta->mime, "video/vnd.dlna.mpeg-tts");
						default:
							break;
					}
				}
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is h.264\n", video_stream, basepath);
				break;
			case AV_CODEC_ID_MPEG4:
				if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
				{
					if( ends_with(path, ".3gp") )
					{
						sprintf(meta->mime, "video/3gpp");
					}
				}
				break;
			case AV_CODEC_ID_VC1:
				if( strcmp(ctx->iformat->name, "asf") != 0 )
				{
					DPRINTF(E_DEBUG, L_METADATA, "Skipping DLNA parsing for non-ASF VC1 file %s\n", path);
					break;
				}
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is VC1\n", video_stream, basepath);
				sprintf(meta->mime, "video/x-ms-wmv");
				break;
			case AV_CODEC_ID_MSMPEG4V3:
				sprintf(meta->mime, "video/x-msvideo");
			default:
				break;
		}
	}

video_no_dlna:

	if( meta->mime[0] == '\0' )
	{
		if( strcmp(ctx->iformat->name, "avi") == 0 )
			sprintf(meta->mime, "video/x-msvideo");
		else if( strncmp(ctx->iformat->name, "mpeg", 4) == 0 )
			sprintf(meta->mime, "video/mpeg");
		else if( strcmp(ctx->iformat->name, "asf") == 0 )
			sprintf(meta->mime, "video/x-ms-wmv");
		else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
			if( ends_with(path, ".mov") )
				sprintf(meta->mime, "video/quicktime");
			else
				sprintf(meta->mime, "video/mp4");
		else if( strncmp(ctx->iformat->name, "matroska", 8) == 0 )
			sprintf(meta->mime, "video/x-matroska");
		else if( strcmp(ctx->iformat->name, "flv") == 0 )
			sprintf(meta->mime, "video/x-flv");
		else
			DPRINTF(E_WARN, L_METADATA, "%s: Unhandled format: %s\n", path, ctx->iformat->name);
	}

	strcpy(meta->title, name);
	strip_ext(meta->title);

	lav_close(ctx);

	free(path_cpy);

	return;
}
