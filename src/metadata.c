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
#define FLAG_ARTIST	0x00000002
#define FLAG_ALBUM	0x00000004
#define FLAG_GENRE	0x00000008
#define FLAG_COMMENT	0x00000010
#define FLAG_CREATOR	0x00000020
#define FLAG_DATE	0x00000040
#define FLAG_DLNA_PN	0x00000080
#define FLAG_MIME	0x00000100
#define FLAG_DURATION	0x00000200
#define FLAG_RESOLUTION	0x00000400

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
check_for_captions(const char *path, int64_t detailID)
{
	char file[MAXPATHLEN];
	char *p;
	int ret;

	strncpyt(file, path, sizeof(file));
	p = strip_ext(file);
	if (!p)
		return;

	/* If we weren't given a detail ID, look for one. */
	if (!detailID)
	{
		detailID = sql_get_int64_field(db, "SELECT ID from DETAILS where (PATH > '%q.' and PATH <= '%q.z')"
						   " and MIME glob 'video/*' limit 1", file, file);
		if (detailID <= 0)
		{
			//DPRINTF(E_MAXDEBUG, L_METADATA, "No file found for caption %s.\n", path);
			return;
		}
	}

	strcpy(p, ".srt");
	ret = access(file, R_OK);
	if (ret != 0)
	{
		strcpy(p, ".smi");
		ret = access(file, R_OK);
	}

	if (ret == 0)
	{
		sql_exec(db, "INSERT OR REPLACE into CAPTIONS"
		             " (ID, PATH) "
		             "VALUES"
		             " (%lld, %Q)", detailID, file);
	}
}

void
parse_nfo(const char *path, metadata_t *m)
{
	FILE *nfo;
	char *buf;
	struct NameValueParserData xml;
	struct stat file;
	size_t nread;
	char *val, *val2;

	if (stat(path, &file) != 0 ||
	    file.st_size > 65535)
	{
		DPRINTF(E_INFO, L_METADATA, "Not parsing very large .nfo file %s\n", path);
		return;
	}
	DPRINTF(E_DEBUG, L_METADATA, "Parsing .nfo file: %s\n", path);
	buf = calloc(1, file.st_size + 1);
	if (!buf)
		return;
	nfo = fopen(path, "r");
	if (!nfo)
	{
		free(buf);
		return;
	}
	nread = fread(buf, 1, file.st_size, nfo);
	fclose(nfo);
	
	ParseNameValue(buf, nread, &xml, 0);

	//printf("\ttype: %s\n", GetValueFromNameValueList(&xml, "rootElement"));
	val = GetValueFromNameValueList(&xml, "title");
	if (val)
	{
		char *esc_tag, *title;
		val2 = GetValueFromNameValueList(&xml, "episodetitle");
		if (val2)
			xasprintf(&title, "%s - %s", val, val2);
		else
			title = strdup(val);
		esc_tag = unescape_tag(title, 1);
		m->title = escape_tag(esc_tag, 1);
		free(esc_tag);
		free(title);
	}

	val = GetValueFromNameValueList(&xml, "plot");
	if (val)
	{
		char *esc_tag = unescape_tag(val, 1);
		m->comment = escape_tag(esc_tag, 1);
		free(esc_tag);
	}

	val = GetValueFromNameValueList(&xml, "capturedate");
	if (val)
	{
		char *esc_tag = unescape_tag(val, 1);
		m->date = escape_tag(esc_tag, 1);
		free(esc_tag);
	}

	val = GetValueFromNameValueList(&xml, "genre");
	if (val)
	{
		char *esc_tag = unescape_tag(val, 1);
		free(m->genre);
		m->genre = escape_tag(esc_tag, 1);
		free(esc_tag);
	}

	val = GetValueFromNameValueList(&xml, "mime");
	if (val)
	{
		char *esc_tag = unescape_tag(val, 1);
		free(m->mime);
		m->mime = escape_tag(esc_tag, 1);
		free(esc_tag);
	}

	ClearNameValueList(&xml);
	free(buf);
}

void
free_metadata(metadata_t *m, uint32_t flags)
{
	if( flags & FLAG_TITLE )
		free(m->title);
	if( flags & FLAG_ARTIST )
		free(m->artist);
	if( flags & FLAG_ALBUM )
		free(m->album);
	if( flags & FLAG_GENRE )
		free(m->genre);
	if( flags & FLAG_CREATOR )
		free(m->creator);
	if( flags & FLAG_DATE )
		free(m->date);
	if( flags & FLAG_COMMENT )
		free(m->comment);
	if( flags & FLAG_DLNA_PN )
		free(m->dlna_pn);
	if( flags & FLAG_MIME )
		free(m->mime);
	if( flags & FLAG_DURATION )
		free(m->duration);
	if( flags & FLAG_RESOLUTION )
		free(m->resolution);
}

int64_t
GetFolderMetadata(const char *name, const char *path, const char *artist, const char *genre)
{
	int ret;

	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (TITLE, PATH, CREATOR, ARTIST, GENRE) "
	                   "VALUES"
	                   " ('%q', %Q, %Q, %Q, %Q);",
	                   name, path, artist, artist, genre);
	if( ret != SQLITE_OK )
		ret = 0;
	else
		ret = sqlite3_last_insert_rowid(db);

	return ret;
}

int64_t
GetVideoMetadata(const char *path, const char *name)
{
	struct stat file;
	int ret, i;
	struct tm *modtime;
	AVFormatContext *ctx = NULL;
	AVStream *astream = NULL, *vstream = NULL;
	int audio_stream = -1, video_stream = -1;
	enum audio_profiles audio_profile = PROFILE_AUDIO_UNKNOWN;
	char fourcc[4];
	char nfo[MAXPATHLEN], *ext;
	metadata_t m;
	uint32_t free_flags = 0xFFFFFFFF;
	char *path_cpy, *basepath;

	memset(&m, '\0', sizeof(m));

	//DEBUG DPRINTF(E_DEBUG, L_METADATA, "Parsing video %s...\n", name);
	if ( stat(path, &file) != 0 )
		return 0;
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * size: %jd\n", file.st_size);

	ret = lav_open(&ctx, path);
	if( ret != 0 )
	{
		char err[128];
		av_strerror(ret, err, sizeof(err));
		DPRINTF(E_WARN, L_METADATA, "Opening %s failed! [%s]\n", path, err);
		return 0;
	}
	//dump_format(ctx, 0, NULL, 0);
	for( i=0; i < ctx->nb_streams; i++)
	{
		if( lav_codec_type(ctx->streams[i]) == AVMEDIA_TYPE_AUDIO &&
		    audio_stream == -1 )
		{
			audio_stream = i;
			astream = ctx->streams[audio_stream];
			continue;
		}
		else if( lav_codec_type(ctx->streams[i]) == AVMEDIA_TYPE_VIDEO &&
		         !lav_is_thumbnail_stream(ctx->streams[i], &m.thumb_data, &m.thumb_size) &&
		         video_stream == -1 )
		{
			video_stream = i;
			vstream = ctx->streams[video_stream];
			continue;
		}
	}
	path_cpy = strdup(path);
	basepath = basename(path_cpy);
	if( !vstream )
	{
		/* This must not be a video file. */
		lav_close(ctx);
		if( !is_audio(path) )
			DPRINTF(E_DEBUG, L_METADATA, "File %s does not contain a video stream.\n", basepath);
		free(path_cpy);
		return 0;
	}

	if( astream )
	{
		aac_object_type_t aac_type = AAC_INVALID;
		switch( lav_codec_id(astream) )
		{
			case AV_CODEC_ID_MP3:
				audio_profile = PROFILE_AUDIO_MP3;
				break;
			case AV_CODEC_ID_AAC:
				if( !lav_codec_extradata(astream) )
				{
					DPRINTF(E_DEBUG, L_METADATA, "No AAC type\n");
				}
				else
				{
					uint8_t data;
					memcpy(&data, lav_codec_extradata(astream), 1);
					aac_type = data >> 3;
				}
				switch( aac_type )
				{
					/* AAC Low Complexity variants */
					case AAC_LC:
					case AAC_LC_ER:
						if( lav_sample_rate(astream) < 8000 ||
						    lav_sample_rate(astream) > 48000 )
						{
							DPRINTF(E_DEBUG, L_METADATA, "Unsupported AAC: sample rate is not 8000 < %d < 48000\n",
								lav_sample_rate(astream));
							break;
						}
						/* AAC @ Level 1/2 */
						if( lav_channels(astream) <= 2 &&
						    lav_bit_rate(astream) <= 576000 )
							audio_profile = PROFILE_AUDIO_AAC;
						else if( lav_channels(astream) <= 6 &&
							 lav_bit_rate(astream) <= 1440000 )
							audio_profile = PROFILE_AUDIO_AAC_MULT5;
						else
							DPRINTF(E_DEBUG, L_METADATA, "Unhandled AAC: %lld channels, %lld bitrate\n",
								(long long)lav_channels(astream),
								(long long)lav_bit_rate(astream));
						break;
					default:
						DPRINTF(E_DEBUG, L_METADATA, "Unhandled AAC type [%d]\n", aac_type);
						break;
				}
				break;
			case AV_CODEC_ID_AC3:
			case AV_CODEC_ID_DTS:
				audio_profile = PROFILE_AUDIO_AC3;
				break;
			case AV_CODEC_ID_WMAV1:
			case AV_CODEC_ID_WMAV2:
				/* WMA Baseline: stereo, up to 48 KHz, up to 192,999 bps */
				if ( lav_bit_rate(astream) <= 193000 )
					audio_profile = PROFILE_AUDIO_WMA_BASE;
				/* WMA Full: stereo, up to 48 KHz, up to 385 Kbps */
				else if ( lav_bit_rate(astream) <= 385000 )
					audio_profile = PROFILE_AUDIO_WMA_FULL;
				break;
			case AV_CODEC_ID_WMAPRO:
				audio_profile = PROFILE_AUDIO_WMA_PRO;
				break;
			case AV_CODEC_ID_MP2:
				audio_profile = PROFILE_AUDIO_MP2;
				break;
			case AV_CODEC_ID_AMR_NB:
				audio_profile = PROFILE_AUDIO_AMR;
				break;
			default:
				if( (lav_codec_id(astream) >= AV_CODEC_ID_PCM_S16LE) &&
				    (lav_codec_id(astream) < AV_CODEC_ID_ADPCM_IMA_QT) )
					audio_profile = PROFILE_AUDIO_PCM;
				else
					DPRINTF(E_DEBUG, L_METADATA, "Unhandled audio codec [0x%X]\n", lav_codec_id(astream));
				break;
		}
		m.frequency = lav_sample_rate(astream);
		m.channels = lav_channels(astream);
	}
	if( vstream )
	{
		int off;
		ts_timestamp_t ts_timestamp = NONE;
		DPRINTF(E_DEBUG, L_METADATA, "Container: '%s' [%s]\n", ctx->iformat->name, basepath);
		xasprintf(&m.resolution, "%dx%d", lav_width(vstream), lav_height(vstream));
		if( ctx->bit_rate > 8 )
			m.bitrate = ctx->bit_rate / 8;
		if( ctx->duration > 0 )
			m.duration = duration_str(ctx->duration / (AV_TIME_BASE/1000));

		/* NOTE: The DLNA spec only provides for ASF (WMV), TS, PS, and MP4 containers.
		 * Skip DLNA parsing for everything else. */
		if( strcmp(ctx->iformat->name, "avi") == 0 )
		{
			xasprintf(&m.mime, "video/x-msvideo");
			if( lav_codec_id(vstream) == AV_CODEC_ID_MPEG4 )
			{
				fourcc[0] = lav_codec_tag(vstream)     & 0xff;
				fourcc[1] = lav_codec_tag(vstream)>>8  & 0xff;
				fourcc[2] = lav_codec_tag(vstream)>>16 & 0xff;
				fourcc[3] = lav_codec_tag(vstream)>>24 & 0xff;
				if( memcmp(fourcc, "XVID", 4) == 0 ||
				    memcmp(fourcc, "DX50", 4) == 0 ||
				    memcmp(fourcc, "DIVX", 4) == 0 )
					xasprintf(&m.creator, "DiVX");
			}
		}
		else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 &&
		         ends_with(path, ".mov") )
			xasprintf(&m.mime, "video/quicktime");
		else if( strncmp(ctx->iformat->name, "matroska", 8) == 0 )
			xasprintf(&m.mime, "video/x-matroska");
		else if( strcmp(ctx->iformat->name, "flv") == 0 )
			xasprintf(&m.mime, "video/x-flv");
		if( m.mime )
			goto video_no_dlna;

		switch( lav_codec_id(vstream) )
		{
			case AV_CODEC_ID_MPEG1VIDEO:
				if( strcmp(ctx->iformat->name, "mpeg") == 0 )
				{
					if( (lav_width(vstream)  == 352) &&
					    (lav_height(vstream) <= 288) )
					{
						m.dlna_pn = strdup("MPEG1");
					}
					xasprintf(&m.mime, "video/mpeg");
				}
				break;
			case AV_CODEC_ID_MPEG2VIDEO:
				m.dlna_pn = malloc(64);
				off = sprintf(m.dlna_pn, "MPEG_");
				if( strcmp(ctx->iformat->name, "mpegts") == 0 )
				{
					int raw_packet_size;
					int dlna_ts_present = dlna_timestamp_is_present(path, &raw_packet_size);
					DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is %s MPEG2 TS packet size %d\n",
						video_stream, basepath, m.resolution, raw_packet_size);
					off += sprintf(m.dlna_pn+off, "TS_");
					if( (lav_width(vstream)  >= 1280) &&
					    (lav_height(vstream) >= 720) )
					{
						off += sprintf(m.dlna_pn+off, "HD_NA");
					}
					else
					{
						off += sprintf(m.dlna_pn+off, "SD_");
						if( (lav_height(vstream) == 576) ||
						    (lav_height(vstream) == 288) )
							off += sprintf(m.dlna_pn+off, "EU");
						else
							off += sprintf(m.dlna_pn+off, "NA");
					}
					if( raw_packet_size == MPEG_TS_PACKET_LENGTH_DLNA )
					{
						if (dlna_ts_present)
							ts_timestamp = VALID;
						else
							ts_timestamp = EMPTY;
					}
					else if( raw_packet_size != MPEG_TS_PACKET_LENGTH )
					{
						DPRINTF(E_DEBUG, L_METADATA, "Unsupported DLNA TS packet size [%d] (%s)\n",
							raw_packet_size, basepath);
						free(m.dlna_pn);
						m.dlna_pn = NULL;
					}
					switch( ts_timestamp )
					{
						case NONE:
							xasprintf(&m.mime, "video/mpeg");
							if( m.dlna_pn )
								off += sprintf(m.dlna_pn+off, "_ISO");
							break;
						case VALID:
							off += sprintf(m.dlna_pn+off, "_T");
						case EMPTY:
							xasprintf(&m.mime, "video/vnd.dlna.mpeg-tts");
						default:
							break;
					}
				}
				else if( strcmp(ctx->iformat->name, "mpeg") == 0 )
				{
					DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is %s MPEG2 PS\n",
						video_stream, basepath, m.resolution);
					off += sprintf(m.dlna_pn+off, "PS_");
					if( (lav_height(vstream) == 576) ||
					    (lav_height(vstream) == 288) )
						off += sprintf(m.dlna_pn+off, "PAL");
					else
						off += sprintf(m.dlna_pn+off, "NTSC");
					xasprintf(&m.mime, "video/mpeg");
				}
				else
				{
					DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s [%s] is %s non-DLNA MPEG2\n",
						video_stream, basepath, ctx->iformat->name, m.resolution);
					free(m.dlna_pn);
					m.dlna_pn = NULL;
				}
				break;
			case AV_CODEC_ID_H264:
				m.dlna_pn = malloc(128);
				off = sprintf(m.dlna_pn, "AVC_");

				if( strcmp(ctx->iformat->name, "mpegts") == 0 )
				{
					AVRational display_aspect_ratio;
					int fps, interlaced;
					int raw_packet_size;
					int dlna_ts_present = dlna_timestamp_is_present(path, &raw_packet_size);

					off += sprintf(m.dlna_pn+off, "TS_");
					if (lav_sample_aspect_ratio(vstream).num) {
						av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
						          lav_width(vstream) * lav_sample_aspect_ratio(vstream).num,
						          lav_height(vstream) * lav_sample_aspect_ratio(vstream).den,
						          1024*1024);
					}
					fps = lav_get_fps(vstream);
					interlaced = lav_get_interlaced(vstream);
					if( ((((lav_width(vstream) == 1920 || lav_width(vstream) == 1440) && lav_height(vstream) == 1080) ||
					      (lav_width(vstream) == 720 && lav_height(vstream) == 480)) && fps == 59 && interlaced) ||
					    ((lav_width(vstream) == 1280 && lav_height(vstream) == 720) && fps == 59 && !interlaced) )
					{
						if( (lav_profile(vstream) == FF_PROFILE_H264_MAIN || lav_profile(vstream) == FF_PROFILE_H264_HIGH) &&
						    audio_profile == PROFILE_AUDIO_AC3 )
						{
							off += sprintf(m.dlna_pn+off, "HD_60_");
							lav_profile(vstream) = FF_PROFILE_SKIP;
						}
					}
					else if( ((lav_width(vstream) == 1920 && lav_height(vstream) == 1080) ||
					          (lav_width(vstream) == 1440 && lav_height(vstream) == 1080) ||
					          (lav_width(vstream) == 1280 && lav_height(vstream) ==  720) ||
					          (lav_width(vstream) ==  720 && lav_height(vstream) ==  576)) &&
					          interlaced && fps == 50 )
					{
						if( (lav_profile(vstream) == FF_PROFILE_H264_MAIN || lav_profile(vstream) == FF_PROFILE_H264_HIGH) &&
						    audio_profile == PROFILE_AUDIO_AC3 )
						{
							off += sprintf(m.dlna_pn+off, "HD_50_");
							lav_profile(vstream) = FF_PROFILE_SKIP;
						}
					}
					switch( lav_profile(vstream) )
					{
						case FF_PROFILE_H264_BASELINE:
						case FF_PROFILE_H264_CONSTRAINED_BASELINE:
							off += sprintf(m.dlna_pn+off, "BL_");
							if( lav_width(vstream)  <= 352 &&
							    lav_height(vstream) <= 288 &&
							    lav_bit_rate(vstream) <= 384000 )
							{
								off += sprintf(m.dlna_pn+off, "CIF15_");
								break;
							}
							else if( lav_width(vstream)  <= 352 &&
							         lav_height(vstream) <= 288 &&
							         lav_bit_rate(vstream) <= 3000000 )
							{
								off += sprintf(m.dlna_pn+off, "CIF30_");
								break;
							}
							/* Fall back to Main Profile if it doesn't match a Baseline DLNA profile. */
							else
								off -= 3;
						default:
						case FF_PROFILE_H264_MAIN:
							off += sprintf(m.dlna_pn+off, "MP_");
							if( lav_profile(vstream) != FF_PROFILE_H264_BASELINE &&
							    lav_profile(vstream) != FF_PROFILE_H264_CONSTRAINED_BASELINE &&
							    lav_profile(vstream) != FF_PROFILE_H264_MAIN )
							{
								DPRINTF(E_DEBUG, L_METADATA, "Unknown AVC profile %d; assuming MP. [%s]\n",
									lav_profile(vstream), basepath);
							}
							if( lav_width(vstream)  <= 720 &&
							    lav_height(vstream) <= 576 &&
							    lav_bit_rate(vstream) <= 10000000 )
							{
								off += sprintf(m.dlna_pn+off, "SD_");
							}
							else if( lav_width(vstream)  <= 1920 &&
							         lav_height(vstream) <= 1152 &&
							         lav_bit_rate(vstream) <= 20000000 )
							{
								off += sprintf(m.dlna_pn+off, "HD_");
							}
							else
							{
								DPRINTF(E_DEBUG, L_METADATA, "Unsupported h.264 video profile! [%s, %dx%d, %lldbps : %s]\n",
									m.dlna_pn, lav_width(vstream), lav_height(vstream),
									(long long)lav_bit_rate(vstream), basepath);
								free(m.dlna_pn);
								m.dlna_pn = NULL;
							}
							break;
						case FF_PROFILE_H264_HIGH:
							off += sprintf(m.dlna_pn+off, "HP_");
							if( lav_width(vstream)  <= 1920 &&
							    lav_height(vstream) <= 1152 &&
							    lav_bit_rate(vstream) <= 30000000 &&
							    audio_profile == PROFILE_AUDIO_AC3 )
							{
								off += sprintf(m.dlna_pn+off, "HD_");
							}
							else
							{
								DPRINTF(E_DEBUG, L_METADATA, "Unsupported h.264 HP video profile! [%lldbps, %d audio : %s]\n",
									(long long)lav_bit_rate(vstream), audio_profile, basepath);
								free(m.dlna_pn);
								m.dlna_pn = NULL;
							}
							break;
						case FF_PROFILE_SKIP:
							break;
					}
					if( !m.dlna_pn )
						break;
					switch( audio_profile )
					{
						case PROFILE_AUDIO_MP3:
							off += sprintf(m.dlna_pn+off, "MPEG1_L3");
							break;
						case PROFILE_AUDIO_AC3:
							off += sprintf(m.dlna_pn+off, "AC3");
							break;
						case PROFILE_AUDIO_AAC:
						case PROFILE_AUDIO_AAC_MULT5:
							off += sprintf(m.dlna_pn+off, "AAC_MULT5");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for %s file [%s]\n",
								m.dlna_pn, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
					if( !m.dlna_pn )
						break;
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
						free(m.dlna_pn);
						m.dlna_pn = NULL;
					}
					switch( ts_timestamp )
					{
						case NONE:
							if( m.dlna_pn )
								off += sprintf(m.dlna_pn+off, "_ISO");
							break;
						case VALID:
							off += sprintf(m.dlna_pn+off, "_T");
						case EMPTY:
							xasprintf(&m.mime, "video/vnd.dlna.mpeg-tts");
						default:
							break;
					}
				}
				else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
				{
					off += sprintf(m.dlna_pn+off, "MP4_");

					switch( lav_profile(vstream) ) {
					case FF_PROFILE_H264_BASELINE:
					case FF_PROFILE_H264_CONSTRAINED_BASELINE:
						if( lav_width(vstream)  <= 352 &&
						    lav_height(vstream) <= 288 )
						{
							if( ctx->bit_rate < 600000 )
								off += sprintf(m.dlna_pn+off, "BL_CIF15_");
							else if( ctx->bit_rate < 5000000 )
								off += sprintf(m.dlna_pn+off, "BL_CIF30_");
							else
								goto mp4_mp_fallback;

							if( audio_profile == PROFILE_AUDIO_AMR )
							{
								off += sprintf(m.dlna_pn+off, "AMR");
							}
							else if( audio_profile == PROFILE_AUDIO_AAC )
							{
								off += sprintf(m.dlna_pn+off, "AAC_");
								if( ctx->bit_rate < 520000 )
								{
									off += sprintf(m.dlna_pn+off, "520");
								}
								else if( ctx->bit_rate < 940000 )
								{
									off += sprintf(m.dlna_pn+off, "940");
								}
								else
								{
									off -= 13;
									goto mp4_mp_fallback;
								}
							}
							else
							{
								off -= 9;
								goto mp4_mp_fallback;
							}
						}
						else if( lav_width(vstream)  <= 720 &&
						         lav_height(vstream) <= 576 )
						{
							if( lav_level(vstream) == 30 &&
							    audio_profile == PROFILE_AUDIO_AAC &&
							    ctx->bit_rate <= 5000000 )
								off += sprintf(m.dlna_pn+off, "BL_L3L_SD_AAC");
							else if( lav_level(vstream) <= 31 &&
							         audio_profile == PROFILE_AUDIO_AAC &&
							         ctx->bit_rate <= 15000000 )
								off += sprintf(m.dlna_pn+off, "BL_L31_HD_AAC");
							else
								goto mp4_mp_fallback;
						}
						else if( lav_width(vstream)  <= 1280 &&
						         lav_height(vstream) <= 720 )
						{
							if( lav_level(vstream) <= 31 &&
							    audio_profile == PROFILE_AUDIO_AAC &&
							    ctx->bit_rate <= 15000000 )
								off += sprintf(m.dlna_pn+off, "BL_L31_HD_AAC");
							else if( lav_level(vstream) <= 32 &&
							         audio_profile == PROFILE_AUDIO_AAC &&
							         ctx->bit_rate <= 21000000 )
								off += sprintf(m.dlna_pn+off, "BL_L32_HD_AAC");
							else
								goto mp4_mp_fallback;
						}
						else
							goto mp4_mp_fallback;
						break;
					case FF_PROFILE_H264_MAIN:
					mp4_mp_fallback:
						off += sprintf(m.dlna_pn+off, "MP_");
						/* AVC MP4 SD profiles - 10 Mbps max */
						if( lav_width(vstream)  <= 720 &&
						    lav_height(vstream) <= 576 &&
						    lav_bit_rate(vstream) <= 10000000 )
						{
							sprintf(m.dlna_pn+off, "SD_");
							if( audio_profile == PROFILE_AUDIO_AC3 )
								off += sprintf(m.dlna_pn+off, "AC3");
							else if( audio_profile == PROFILE_AUDIO_AAC ||
							         audio_profile == PROFILE_AUDIO_AAC_MULT5 )
								off += sprintf(m.dlna_pn+off, "AAC_MULT5");
							else if( audio_profile == PROFILE_AUDIO_MP3 )
								off += sprintf(m.dlna_pn+off, "MPEG1_L3");
							else
								m.dlna_pn[10] = '\0';
						}
						else if( lav_width(vstream)  <= 1280 &&
						         lav_height(vstream) <= 720 &&
						         lav_bit_rate(vstream) <= 15000000 &&
						         audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "HD_720p_AAC");
						}
						else if( lav_width(vstream)  <= 1920 &&
						         lav_height(vstream) <= 1080 &&
						         lav_bit_rate(vstream) <= 21000000 &&
						         audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "HD_1080i_AAC");
						}
						if( strlen(m.dlna_pn) <= 11 )
						{
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for %s file %s\n",
								m.dlna_pn, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
						}
						break;
					case FF_PROFILE_H264_HIGH:
						if( lav_width(vstream)  <= 1920 &&
						    lav_height(vstream) <= 1080 &&
						    lav_bit_rate(vstream) <= 25000000 &&
						    audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "HP_HD_AAC");
						}
						break;
					default:
						DPRINTF(E_DEBUG, L_METADATA, "AVC profile [%d] not recognized for file %s\n",
							lav_profile(vstream), basepath);
						free(m.dlna_pn);
						m.dlna_pn = NULL;
						break;
					}
				}
				else
				{
					free(m.dlna_pn);
					m.dlna_pn = NULL;
				}
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is h.264\n", video_stream, basepath);
				break;
			case AV_CODEC_ID_MPEG4:
				fourcc[0] = lav_codec_tag(vstream)     & 0xff;
				fourcc[1] = lav_codec_tag(vstream)>>8  & 0xff;
				fourcc[2] = lav_codec_tag(vstream)>>16 & 0xff;
				fourcc[3] = lav_codec_tag(vstream)>>24 & 0xff;
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is MPEG4 [%c%c%c%c/0x%X]\n",
					video_stream, basepath,
					isprint(fourcc[0]) ? fourcc[0] : '_',
					isprint(fourcc[1]) ? fourcc[1] : '_',
					isprint(fourcc[2]) ? fourcc[2] : '_',
					isprint(fourcc[3]) ? fourcc[3] : '_',
					lav_codec_tag(vstream));

				if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
				{
					m.dlna_pn = malloc(128);
					off = sprintf(m.dlna_pn, "MPEG4_P2_");

					if( ends_with(path, ".3gp") )
					{
						xasprintf(&m.mime, "video/3gpp");
						switch( audio_profile )
						{
							case PROFILE_AUDIO_AAC:
								off += sprintf(m.dlna_pn+off, "3GPP_SP_L0B_AAC");
								break;
							case PROFILE_AUDIO_AMR:
								off += sprintf(m.dlna_pn+off, "3GPP_SP_L0B_AMR");
								break;
							default:
								DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for MPEG4-P2 3GP/%d file %s\n",
								        audio_profile, basepath);
								free(m.dlna_pn);
								m.dlna_pn = NULL;
								break;
						}
					}
					else
					{
						if( ctx->bit_rate <= 1000000 &&
						    audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "MP4_ASP_AAC");
						}
						else if( ctx->bit_rate <= 4000000 &&
						         lav_width(vstream)  <= 640 &&
						         lav_height(vstream) <= 480 &&
						         audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "MP4_SP_VGA_AAC");
						}
						else
						{
							DPRINTF(E_DEBUG, L_METADATA, "Unsupported h.264 video profile! [%dx%d, %lldbps]\n",
								lav_width(vstream),
								lav_height(vstream),
								(long long)ctx->bit_rate);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
						}
					}
				}
				break;
			case AV_CODEC_ID_WMV3:
				/* I'm not 100% sure this is correct, but it works on everything I could get my hands on */
				if( lav_codec_extradata(vstream) )
				{
					if( !((lav_codec_extradata(vstream)[0] >> 3) & 1) )
						lav_level(vstream) = 0;
					if( !((lav_codec_extradata(vstream)[0] >> 6) & 1) )
						lav_profile(vstream) = 0;
				}
			case AV_CODEC_ID_VC1:
				if( strcmp(ctx->iformat->name, "asf") != 0 )
				{
					DPRINTF(E_DEBUG, L_METADATA, "Skipping DLNA parsing for non-ASF VC1 file %s\n", path);
					break;
				}
				m.dlna_pn = malloc(64);
				off = sprintf(m.dlna_pn, "WMV");
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is VC1\n", video_stream, basepath);
				xasprintf(&m.mime, "video/x-ms-wmv");
				if( (lav_width(vstream)  <= 176) &&
				    (lav_height(vstream) <= 144) &&
				    (lav_level(vstream) == 0) )
				{
					off += sprintf(m.dlna_pn+off, "SPLL_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_MP3:
							off += sprintf(m.dlna_pn+off, "MP3");
							break;
						case PROFILE_AUDIO_WMA_BASE:
							off += sprintf(m.dlna_pn+off, "BASE");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVSPLL/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				else if( (lav_width(vstream)  <= 352) &&
				         (lav_height(vstream) <= 288) &&
				         (lav_profile(vstream) == 0) &&
				         (ctx->bit_rate/8 <= 384000) )
				{
					off += sprintf(m.dlna_pn+off, "SPML_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_MP3:
							off += sprintf(m.dlna_pn+off, "MP3");
							break;
						case PROFILE_AUDIO_WMA_BASE:
							off += sprintf(m.dlna_pn+off, "BASE");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVSPML/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				else if( (lav_width(vstream)  <= 720) &&
				         (lav_height(vstream) <= 576) &&
				         (ctx->bit_rate/8 <= 10000000) )
				{
					off += sprintf(m.dlna_pn+off, "MED_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_WMA_PRO:
							off += sprintf(m.dlna_pn+off, "PRO");
							break;
						case PROFILE_AUDIO_WMA_FULL:
							off += sprintf(m.dlna_pn+off, "FULL");
							break;
						case PROFILE_AUDIO_WMA_BASE:
							off += sprintf(m.dlna_pn+off, "BASE");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVMED/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				else if( (lav_width(vstream)  <= 1920) &&
				         (lav_height(vstream) <= 1080) &&
				         (ctx->bit_rate/8 <= 20000000) )
				{
					off += sprintf(m.dlna_pn+off, "HIGH_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_WMA_PRO:
							off += sprintf(m.dlna_pn+off, "PRO");
							break;
						case PROFILE_AUDIO_WMA_FULL:
							off += sprintf(m.dlna_pn+off, "FULL");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVHIGH/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				break;
			case AV_CODEC_ID_MSMPEG4V3:
				xasprintf(&m.mime, "video/x-msvideo");
			default:
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is %s [type %d]\n",
					video_stream, basepath, m.resolution, lav_codec_id(vstream));
				break;
		}
	}

video_no_dlna:

	strcpy(nfo, path);
	ext = strrchr(nfo, '.');
	if( ext )
	{
		strcpy(ext+1, "nfo");
		if( access(nfo, F_OK) == 0 )
		{
			parse_nfo(nfo, &m);
		}
	}

	if( !m.mime )
	{
		if( strcmp(ctx->iformat->name, "avi") == 0 )
			xasprintf(&m.mime, "video/x-msvideo");
		else if( strncmp(ctx->iformat->name, "mpeg", 4) == 0 )
			xasprintf(&m.mime, "video/mpeg");
		else if( strcmp(ctx->iformat->name, "asf") == 0 )
			xasprintf(&m.mime, "video/x-ms-wmv");
		else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
			if( ends_with(path, ".mov") )
				xasprintf(&m.mime, "video/quicktime");
			else
				xasprintf(&m.mime, "video/mp4");
		else if( strncmp(ctx->iformat->name, "matroska", 8) == 0 )
			xasprintf(&m.mime, "video/x-matroska");
		else if( strcmp(ctx->iformat->name, "flv") == 0 )
			xasprintf(&m.mime, "video/x-flv");
		else
			DPRINTF(E_WARN, L_METADATA, "%s: Unhandled format: %s\n", path, ctx->iformat->name);
	}

	if( !m.date )
	{
		m.date = malloc(20);
		modtime = localtime(&file.st_mtime);
		strftime(m.date, 20, "%FT%T", modtime);
	}

	if( !m.title )
	{
		m.title = strdup(name);
		strip_ext(m.title);
	}

	lav_close(ctx);

	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (PATH, SIZE, TIMESTAMP, DURATION, DATE, CHANNELS, BITRATE, SAMPLERATE, RESOLUTION,"
	                   "  TITLE, CREATOR, ARTIST, GENRE, COMMENT, DLNA_PN, MIME) "
	                   "VALUES"
	                   " (%Q, %lld, %lld, %Q, %Q, %u, %u, %u, %Q, '%q', %Q, %Q, %Q, %Q, %Q, '%q');",
	                   path, (long long)file.st_size, (long long)file.st_mtime, m.duration,
	                   m.date, m.channels, m.bitrate, m.frequency, m.resolution,
	                   m.title, m.creator, m.artist, m.genre, m.comment, m.dlna_pn,
	                   m.mime);
	if( ret != SQLITE_OK )
	{
		DPRINTF(E_ERROR, L_METADATA, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}
	else
	{
		ret = sqlite3_last_insert_rowid(db);
		check_for_captions(path, ret);
	}
	free_metadata(&m, free_flags);
	free(path_cpy);

	return ret;
}
