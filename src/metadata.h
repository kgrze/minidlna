/* Metadata extraction
 *
 * Project : minidlna
 * Website : http://sourceforge.net/projects/minidlna/
 * Author  : Justin Maggard
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
 */
#ifndef __METADATA_H__
#define __METADATA_H__

typedef struct metadata_s {
	char title[150];
	char mime[40];
	__off64_t file_size;
} metadata_t;

typedef enum {
	NONE,
	EMPTY,
	VALID
} ts_timestamp_t;

int
ends_with(const char *haystack, const char *needle);

void
GetVideoMetadata(metadata_t * const meta, const char *path, const char *name);

#endif
