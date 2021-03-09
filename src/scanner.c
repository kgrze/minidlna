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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <locale.h>
#include <libgen.h>
#include <inttypes.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "config.h"

#ifdef ENABLE_NLS
#include <libintl.h>
#endif
#include <sqlite3.h>
#include "libav.h"

#include "scanner_sqlite.h"
#include "upnpglobalvars.h"
#include "metadata.h"
#include "utils.h"
#include "sql.h"
#include "scanner.h"
#include "log.h"

#if SCANDIR_CONST
typedef const struct dirent scan_filter;
#else
typedef struct dirent scan_filter;
#endif
#ifndef AV_LOG_PANIC
#define AV_LOG_PANIC AV_LOG_FATAL
#endif

int valid_cache = 0;

struct virtual_item
{
	int64_t objectID;
	char parentID[64];
	char name[256];
};

int64_t
get_next_available_id(const char *table, const char *parentID)
{
		char *ret, *base;
		int64_t objectID = 0;

		ret = sql_get_text_field(db, "SELECT OBJECT_ID from %s where ID = "
		                             "(SELECT max(ID) from %s where PARENT_ID = '%s')",
		                             table, table, parentID);
		if( ret )
		{
			base = strrchr(ret, '$');
			if( base )
				objectID = strtoll(base+1, NULL, 16) + 1;
			sqlite3_free(ret);
		}

		return objectID;
}

int64_t
insert_directory(const char *name, const char *path, const char *base, const char *parentID, int objectID)
{
	int64_t detailID = 0;
	char class[] = "container.storageFolder";

	detailID = GetFolderMetadata(name, path);
	sql_exec(db, "INSERT into OBJECTS"
	             " (OBJECT_ID, PARENT_ID, DETAIL_ID, CLASS, NAME) "
	             "VALUES"
	             " ('%s%s$%X', '%s%s', %lld, '%s', '%q')",
	             base, parentID, objectID, base, parentID, detailID, class, name);

	return detailID;
}

int
insert_file(const char *name, const char *path, const char *parentID, int object, media_types types)
{
	const char *class;
	char objectID[64];
	int64_t detailID = 0;
	char base[8];
	char *objname;
	media_types mtype = get_media_type(name);

	if( mtype == TYPE_VIDEO && (types & TYPE_VIDEO) )
	{
		strcpy(base, VIDEO_DIR_ID);
		class = "item.videoItem";
		detailID = GetVideoMetadata(path, name);
	}

	sprintf(objectID, "%s%s$%X", BROWSEDIR_ID, parentID, object);
	objname = strdup(name);
	strip_ext(objname);

	sql_exec(db, "INSERT into OBJECTS"
	             " (OBJECT_ID, PARENT_ID, CLASS, DETAIL_ID, NAME) "
	             "VALUES"
	             " ('%s', '%s%s', '%s', %lld, '%q')",
	             objectID, BROWSEDIR_ID, parentID, class, detailID, objname);

	free(objname);

	return 0;
}

int
CreateDatabase(void)
{
	int ret, i;
	const char *containers[] = { "0","-1",   "root",
	                    		BROWSEDIR_ID, "0", _("VIDEO"),
						       0};

	ret = sql_exec(db, create_objectTable_sqlite);
	if( ret != SQLITE_OK )
		goto sql_failed;
	ret = sql_exec(db, create_detailTable_sqlite);
	if( ret != SQLITE_OK )
		goto sql_failed;
	for( i=0; containers[i]; i=i+3 )
	{
		ret = sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, DETAIL_ID, CLASS, NAME)"
		                   " values "
		                   "('%s', '%s', %lld, 'container.storageFolder', '%q')",
		                   containers[i], containers[i+1], GetFolderMetadata(containers[i+2], NULL), containers[i+2]);
		if( ret != SQLITE_OK )
			goto sql_failed;
	}

sql_failed:
	if( ret != SQLITE_OK )
		DPRINTF(E_ERROR, L_DB_SQL, "Error creating SQLite3 database!\n");
	return (ret != SQLITE_OK);
}

static inline int
filter_hidden(scan_filter *d)
{
	return (d->d_name[0] != '.');
}

static int
filter_type(scan_filter *d)
{
#if HAVE_STRUCT_DIRENT_D_TYPE
	return ( (d->d_type == DT_DIR) ||
		 (d->d_type == DT_LNK) ||
		 (d->d_type == DT_UNKNOWN)
		);
#else
	return 1;
#endif
}

static int
filter_v(scan_filter *d)
{
	return ( filter_hidden(d) &&
		 (filter_type(d) ||
		  (is_reg(d) &&
		   is_video(d->d_name)))
		);
}

static void
ScanDirectory(const char *dir, const char *parent, media_types dir_types)
{
	struct dirent **namelist;
	int i, n, startID = 0;
	char *full_path;
	char *name = NULL;
	static long long unsigned int fileno = 0;
	enum file_types type;

	DPRINTF(parent?E_INFO:E_WARN, L_SCANNER, _("Scanning %s\n"), dir);
	n = scandir(dir, &namelist, filter_v, alphasort);
	if( n < 0 )
	{
		DPRINTF(E_WARN, L_SCANNER, "Error scanning %s [%s]\n",
			dir, strerror(errno));
		return;
	}

	full_path = malloc(PATH_MAX);
	if (!full_path)
	{
		DPRINTF(E_ERROR, L_SCANNER, "Memory allocation failed scanning %s\n", dir);
		return;
	}

	if( !parent )
	{
		startID = get_next_available_id("OBJECTS", BROWSEDIR_ID);
	}

	for (i=0; i < n; i++)
	{
		type = TYPE_UNKNOWN;
		snprintf(full_path, PATH_MAX, "%s/%s", dir, namelist[i]->d_name);
		name = escape_tag(namelist[i]->d_name, 1);

		if( is_dir(namelist[i]) == 1 )
		{
			type = TYPE_DIR;
		}
		else if( is_reg(namelist[i]) == 1 )
		{
			type = TYPE_FILE;
		}

		if( (type == TYPE_DIR) && (access(full_path, R_OK|X_OK) == 0) )
		{
			char *parent_id;
			insert_directory(name, full_path, BROWSEDIR_ID, THISORNUL(parent), i+startID);
			xasprintf(&parent_id, "%s$%X", THISORNUL(parent), i+startID);
			ScanDirectory(full_path, parent_id, dir_types);
			free(parent_id);
		}
		else if( type == TYPE_FILE && (access(full_path, R_OK) == 0) )
		{
			if( insert_file(name, full_path, THISORNUL(parent), i+startID, dir_types) == 0 )
				fileno++;
		}
		free(name);
		free(namelist[i]);
	}
	free(namelist);
	free(full_path);
	if( !parent )
	{
		DPRINTF(E_WARN, L_SCANNER, _("Scanning %s finished (%llu files)!\n"), dir, fileno);
	}
}

void
start_scanner(void)
{
	struct media_dir_s *media_path;
	char path[MAXPATHLEN];
	int64_t id;
	char *bname, *parent = NULL;

	if (setpriority(PRIO_PROCESS, 0, 15) == -1)
		DPRINTF(E_WARN, L_INOTIFY,  "Failed to reduce scanner thread priority\n");

	setlocale(LC_COLLATE, "");
	av_log_set_level(AV_LOG_PANIC);

	media_path = media_dirs;

	strncpyt(path, media_path->path, sizeof(path));
	bname = basename(path);
	id = GetFolderMetadata(bname, media_path->path);
	/* Use TIMESTAMP to store the media type */
	sql_exec(db, "UPDATE DETAILS set TIMESTAMP = %d where ID = %lld", media_path->types, (long long)id);
	ScanDirectory(media_path->path, parent, media_path->types);

	DPRINTF(E_DEBUG, L_SCANNER, "Initial file scan completed\n");
	//JM: Set up a db version number, so we know if we need to rebuild due to a new structure.
	sql_exec(db, "pragma user_version = %d;", DB_VERSION);
}
