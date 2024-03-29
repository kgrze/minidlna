/* MiniDLNA project
 *
 * http://sourceforge.net/projects/minidlna/
 *
 * MiniDLNA media server
 * Copyright (C) 2008-2012  Justin Maggard
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
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>
#include <libgen.h>
#include <pwd.h>

#include "config.h"

#ifdef ENABLE_NLS
#include <locale.h>
#include <libintl.h>
#endif

#include "upnpglobalvars.h"
#include "sql.h"
#include "upnphttp.h"
#include "upnpdescgen.h"
#include "minidlnapath.h"
#include "getifaddr.h"
#include "upnpsoap.h"
#include "utils.h"
#include "minissdp.h"
#include "minidlnatypes.h"
#include "process.h"
#include "scanner.h"
#include "log.h"

#if SQLITE_VERSION_NUMBER < 3005001
# warning "Your SQLite3 library appears to be too old!  Please use 3.5.1 or newer."
# define sqlite3_threadsafe() 0
#endif
 
/* OpenAndConfHTTPSocket() :
 * setup the socket used to handle incoming HTTP connections. */
static int
OpenAndConfHTTPSocket(unsigned short port)
{
	int s;
	int i = 1;
	struct sockaddr_in listenname;

	s = socket(PF_INET, SOCK_STREAM, 0);
	if (s < 0)
	{
		DPRINTF(E_ERROR, L_GENERAL, "socket(http): %s\n", strerror(errno));
		return -1;
	}

	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i)) < 0)
		DPRINTF(E_WARN, L_GENERAL, "setsockopt(http, SO_REUSEADDR): %s\n", strerror(errno));

	memset(&listenname, 0, sizeof(struct sockaddr_in));
	listenname.sin_family = AF_INET;
	listenname.sin_port = htons(port);
	listenname.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(s, (struct sockaddr *)&listenname, sizeof(struct sockaddr_in)) < 0)
	{
		DPRINTF(E_ERROR, L_GENERAL, "bind(http): %s\n", strerror(errno));
		close(s);
		return -1;
	}

	if (listen(s, 16) < 0)
	{
		DPRINTF(E_ERROR, L_GENERAL, "listen(http): %s\n", strerror(errno));
		close(s);
		return -1;
	}

	return s;
}

/* Handler for the SIGTERM signal (kill) 
 * SIGINT is also handled */
static void
sigterm(int sig)
{
	signal(sig, SIG_IGN);	/* Ignore this signal while we are quitting */

	DPRINTF(E_WARN, L_GENERAL, "received signal %d, good-bye\n", sig);

	quitting = 1;
}

static void
sigusr1(int sig)
{
	signal(sig, sigusr1);
	DPRINTF(E_WARN, L_GENERAL, "received signal %d, clear cache\n", sig);
}

static void
sighup(int sig)
{
	signal(sig, sighup);
	DPRINTF(E_WARN, L_GENERAL, "received signal %d, re-read\n", sig);

	reload_ifaces(1);
}

/* record the startup time */
static void
set_startup_time(void)
{
	startup_time = time(NULL);
}

static time_t
_get_dbtime(void)
{
	char path[PATH_MAX];
	struct stat st;

	snprintf(path, sizeof(path), "%s/files.db", db_path);
	if (stat(path, &st) != 0)
		return 0;
	return st.st_mtime;
}

static int
open_db(sqlite3 **sq3)
{
	char path[PATH_MAX];
	int new_db = 0;

	snprintf(path, sizeof(path), "%s/files.db", db_path);
	if (access(path, F_OK) != 0)
	{
		new_db = 1;
		make_dir(db_path, S_ISVTX|S_IRWXU|S_IRWXG|S_IRWXO);
	}
	if (sqlite3_open(path, &db) != SQLITE_OK)
		DPRINTF(E_FATAL, L_GENERAL, "ERROR: Failed to open sqlite database!  Exiting...\n");
	if (sq3)
		*sq3 = db;
	sqlite3_busy_timeout(db, 5000);
	sql_exec(db, "pragma page_size = 4096");
	sql_exec(db, "pragma journal_mode = OFF");
	sql_exec(db, "pragma synchronous = OFF;");
	sql_exec(db, "pragma default_cache_size = 8192;");

	return new_db;
}

static void
check_db(sqlite3 *db, int new_db, pid_t *scanner_pid)
{
	char cmd[PATH_MAX*2];
	int ret;

	ret = db_upgrade(db);
	if (ret != 0)
	{
		if (ret < 0)
			DPRINTF(E_WARN, L_GENERAL, "Creating new database at %s/files.db\n", db_path);
		else if (ret == 1)
			DPRINTF(E_WARN, L_GENERAL, "New media_dir detected; rebuilding...\n");
		else if (ret == 2)
			DPRINTF(E_WARN, L_GENERAL, "Removed media_dir detected; rebuilding...\n");
		else
			DPRINTF(E_WARN, L_GENERAL, "Database version mismatch (%d => %d); need to recreate...\n",
				ret, DB_VERSION);
		sqlite3_close(db);

		snprintf(cmd, sizeof(cmd), "rm -rf %s/files.db %s/art_cache", db_path, db_path);
		if (system(cmd) != 0)
			DPRINTF(E_FATAL, L_GENERAL, "Failed to clean old file cache!  Exiting...\n");

		open_db(&db);
		if (CreateDatabase() != 0)
			DPRINTF(E_FATAL, L_GENERAL, "ERROR: Failed to create sqlite database!  Exiting...\n");
	}
	if (ret)
	{
		SETFLAG(SCANNING_MASK);
		sqlite3_close(db);
		open_db(&db);
		start_scanner();
	}
}

static int
writepidfile(const char *fname, int pid, uid_t uid)
{
	FILE *pidfile;
	struct stat st;
	char path[PATH_MAX], *dir;
	int ret = 0;

	if(!fname || *fname == '\0')
		return -1;

	/* Create parent directory if it doesn't already exist */
	strncpyt(path, fname, sizeof(path));
	dir = dirname(path);
	if (stat(dir, &st) == 0)
	{
		if (!S_ISDIR(st.st_mode))
		{
			DPRINTF(E_ERROR, L_GENERAL, "Pidfile path is not a directory: %s\n",
				fname);
			return -1;
		}
	}
	else
	{
		if (make_dir(dir, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH) != 0)
		{
			DPRINTF(E_ERROR, L_GENERAL, "Unable to create pidfile directory: %s\n",
				fname);
			return -1;
		}
		if (uid > 0)
		{
			if (chown(dir, uid, -1) != 0)
				DPRINTF(E_WARN, L_GENERAL, "Unable to change pidfile %s ownership: %s\n",
					dir, strerror(errno));
		}
	}
	
	pidfile = fopen(fname, "w");
	if (!pidfile)
	{
		DPRINTF(E_ERROR, L_GENERAL, "Unable to open pidfile for writing %s: %s\n",
			fname, strerror(errno));
		return -1;
	}

	if (fprintf(pidfile, "%d\n", pid) <= 0)
	{
		DPRINTF(E_ERROR, L_GENERAL, 
			"Unable to write to pidfile %s: %s\n", fname, strerror(errno));
		ret = -1;
	}
	if (uid > 0)
	{
		if (fchown(fileno(pidfile), uid, -1) != 0)
			DPRINTF(E_WARN, L_GENERAL, "Unable to change pidfile %s ownership: %s\n",
				fname, strerror(errno));
	}

	fclose(pidfile);

	return ret;
}

static void init_nls(void)
{
#ifdef ENABLE_NLS
	const char *messages, *ctype, *locale_dir;

	ctype = setlocale(LC_CTYPE, "");
	if (!ctype || !strcmp(ctype, "C"))
		ctype = setlocale(LC_CTYPE, "en_US.utf8");
	if (!ctype)
		DPRINTF(E_WARN, L_GENERAL, "Unset locale\n");
	else if (!strstr(ctype, "utf8") && !strstr(ctype, "UTF8") &&
		 !strstr(ctype, "utf-8") && !strstr(ctype, "UTF-8"))
		DPRINTF(E_WARN, L_GENERAL, "Using unsupported non-utf8 locale '%s'\n", ctype);
	messages = setlocale(LC_MESSAGES, "");
	if (!messages)
		messages = "unset";
	locale_dir = bindtextdomain("minidlna", getenv("TEXTDOMAINDIR"));
	DPRINTF(E_DEBUG, L_GENERAL, "Using locale dir '%s' and locale langauge %s/%s\n", locale_dir, messages, ctype);
	textdomain("minidlna");
#endif
}

/* init phase :
 * 4) check and write pid file
 * 5) set startup time stamp
 * 6) compute presentation URL
 * 7) set signal handlers */
static int
init(int argc, char **argv)
{
	int pid;
	struct sigaction sa;
	char mac_str[13];
	char buf[PATH_MAX];
	char log_str[75] = "general,artwork,database,inotify,scanner,metadata,http,ssdp=warn";
	char *log_level = NULL;
	struct media_dir_s *media_dir;
	uid_t uid = 0;

	/* set up uuid based on mac address */
	if (getsyshwaddr(mac_str, sizeof(mac_str)) < 0)
	{
		DPRINTF(E_OFF, L_GENERAL, "No MAC address found.  Falling back to generic UUID.\n");
		strcpy(mac_str, "554e4b4e4f57");
	}
	strcpy(uuidvalue+5, "4d696e69-444c-164e-9d41-");
	strncat(uuidvalue, mac_str, 12);
	
	runtime_vars.port = 8200;
	runtime_vars.notify_interval = 895;	/* seconds between SSDP announces */
	runtime_vars.max_connections = 50;
	runtime_vars.root_container = NULL;
	runtime_vars.ifaces[0] = NULL;

	media_dir = calloc(1, sizeof(struct media_dir_s));
	media_dir->path = strdup(realpath("../content", buf));
	media_dir->types = ALL_MEDIA;
	media_dirs = media_dir;

	strncpyt(log_path, DEFAULT_LOG_PATH, PATH_MAX);
	strncpyt(db_path, DEFAULT_DB_PATH, PATH_MAX);

	/* Set the default log file path to NULL (stdout) */
	pid = getpid();
	strcpy(log_str+60, "maxdebug");
	log_level = log_str;
	log_init(NULL, log_level);

	set_startup_time();

	/* presentation url */
	strcpy(presentationurl, "/");

	/* set signal handlers */
	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = sigterm;
	if (sigaction(SIGTERM, &sa, NULL))
		DPRINTF(E_FATAL, L_GENERAL, "Failed to set %s handler. EXITING.\n", "SIGTERM");
	if (sigaction(SIGINT, &sa, NULL))
		DPRINTF(E_FATAL, L_GENERAL, "Failed to set %s handler. EXITING.\n", "SIGINT");
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to set %s handler. EXITING.\n", "SIGPIPE");
	if (signal(SIGHUP, &sighup) == SIG_ERR)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to set %s handler. EXITING.\n", "SIGHUP");
	signal(SIGUSR1, &sigusr1);
	sa.sa_handler = process_handle_child_termination;
	if (sigaction(SIGCHLD, &sa, NULL))
		DPRINTF(E_FATAL, L_GENERAL, "Failed to set %s handler. EXITING.\n", "SIGCHLD");

	if (writepidfile(pidfilename, pid, uid) != 0)
		pidfilename = NULL;

	if (uid > 0)
	{
		struct stat st;
		if (stat(db_path, &st) == 0 && st.st_uid != uid && chown(db_path, uid, -1) != 0)
			DPRINTF(E_ERROR, L_GENERAL, "Unable to set db_path [%s] ownership to %d: %s\n",
				db_path, uid, strerror(errno));
	}

	if (uid > 0 && setuid(uid) == -1)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to switch to uid '%d'. [%s] EXITING.\n",
			uid, strerror(errno));

	children = calloc(runtime_vars.max_connections, sizeof(struct child));
	if (!children)
	{
		DPRINTF(E_ERROR, L_GENERAL, "Allocation failed\n");
		return 1;
	}

	return 0;
}

/* === main === */
/* process HTTP or SSDP requests */
int
main(int argc, char **argv)
{
	int ret, i;
	int shttpl = -1;
	int smonitor = -1;
	LIST_HEAD(httplisthead, upnphttp) upnphttphead;
	struct upnphttp * e = 0;
	struct upnphttp * next;
	fd_set readset;	/* for select() */
	fd_set writeset;
	struct timeval timeout, timeofday, lastnotifytime = {0, 0};
	time_t lastupdatetime = 0, lastdbtime = 0;
	int max_fd = -1;
	int last_changecnt = 0;
	pid_t scanner_pid = 0;
	pthread_t inotify_thread = 0;

	for (i = 0; i < L_MAX; i++)
		log_level[i] = E_WARN;

	ret = init(argc, argv);
	if (ret != 0)
		return 1;
	init_nls();

	DPRINTF(E_WARN, L_GENERAL, "Starting " SERVER_NAME " version " MINIDLNA_VERSION ".\n");
	if (sqlite3_libversion_number() < 3005001)
	{
		DPRINTF(E_WARN, L_GENERAL, "SQLite library is old.  Please use version 3.5.1 or newer.\n");
	}

	LIST_INIT(&upnphttphead);

	ret = open_db(NULL);
	check_db(db, ret, &scanner_pid);
	lastdbtime = _get_dbtime();

	smonitor = OpenAndConfMonitorSocket();

	sssdp = OpenAndConfSSDPReceiveSocket();
	if (sssdp < 0)
	{
		DPRINTF(E_INFO, L_GENERAL, "Failed to open socket for receiving SSDP. Trying to use MiniSSDPd\n");
		reload_ifaces(0);	/* populate lan_addr[0].str */
		if (SubmitServicesToMiniSSDPD(lan_addr[0].str, runtime_vars.port) < 0)
			DPRINTF(E_FATAL, L_GENERAL, "Failed to connect to MiniSSDPd. EXITING");
	}
	/* open socket for HTTP connections. */
	shttpl = OpenAndConfHTTPSocket(runtime_vars.port);
	if (shttpl < 0)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to open socket for HTTP. EXITING\n");
	DPRINTF(E_WARN, L_GENERAL, "HTTP listening on port %d\n", runtime_vars.port);

	reload_ifaces(0);
	lastnotifytime.tv_sec = time(NULL) + runtime_vars.notify_interval;

	/* main loop */
	while (!quitting)
	{
		/* Check if we need to send SSDP NOTIFY messages and do it if
		 * needed */
		if (gettimeofday(&timeofday, 0) < 0)
		{
			DPRINTF(E_ERROR, L_GENERAL, "gettimeofday(): %s\n", strerror(errno));
			timeout.tv_sec = runtime_vars.notify_interval;
			timeout.tv_usec = 0;
		}
		else
		{
			/* the comparison is not very precise but who cares ? */
			if (timeofday.tv_sec >= (lastnotifytime.tv_sec + runtime_vars.notify_interval))
			{
				DPRINTF(E_DEBUG, L_SSDP, "Sending SSDP notifies\n");
				for (i = 0; i < n_lan_addr; i++)
				{
					SendSSDPNotifies(lan_addr[i].snotify, lan_addr[i].str,
						runtime_vars.port, runtime_vars.notify_interval);
				}
				memcpy(&lastnotifytime, &timeofday, sizeof(struct timeval));
				timeout.tv_sec = runtime_vars.notify_interval;
				timeout.tv_usec = 0;
			}
			else
			{
				timeout.tv_sec = lastnotifytime.tv_sec + runtime_vars.notify_interval
				                 - timeofday.tv_sec;
				if (timeofday.tv_usec > lastnotifytime.tv_usec)
				{
					timeout.tv_usec = 1000000 + lastnotifytime.tv_usec
					                  - timeofday.tv_usec;
					timeout.tv_sec--;
				}
				else
					timeout.tv_usec = lastnotifytime.tv_usec - timeofday.tv_usec;
			}
		}

		if (GETFLAG(SCANNING_MASK))
		{
			if (!scanner_pid || kill(scanner_pid, 0) != 0)
			{
				CLEARFLAG(SCANNING_MASK);
				if (_get_dbtime() != lastdbtime)
					updateID++;
			}
		}

		/* select open sockets (SSDP, HTTP listen, and all HTTP soap sockets) */
		FD_ZERO(&readset);

		if (sssdp >= 0) 
		{
			FD_SET(sssdp, &readset);
			max_fd = MAX(max_fd, sssdp);
		}
		
		if (shttpl >= 0) 
		{
			FD_SET(shttpl, &readset);
			max_fd = MAX(max_fd, shttpl);
		}
		if (smonitor >= 0) 
		{
			FD_SET(smonitor, &readset);
			max_fd = MAX(max_fd, smonitor);
		}

		i = 0;	/* active HTTP connections count */
		for (e = upnphttphead.lh_first; e != NULL; e = e->entries.le_next)
		{
			if ((e->socket >= 0) && (e->state <= 2))
			{
				FD_SET(e->socket, &readset);
				max_fd = MAX(max_fd, e->socket);
				i++;
			}
		}
		FD_ZERO(&writeset);

		ret = select(max_fd+1, &readset, &writeset, 0, &timeout);
		if (ret < 0)
		{
			if(quitting) goto shutdown;
			if(errno == EINTR) continue;
			DPRINTF(E_ERROR, L_GENERAL, "select(all): %s\n", strerror(errno));
			DPRINTF(E_FATAL, L_GENERAL, "Failed to select open sockets. EXITING\n");
		}
		/* process SSDP packets */
		if (sssdp >= 0 && FD_ISSET(sssdp, &readset))
		{
			/*DPRINTF(E_DEBUG, L_GENERAL, "Received SSDP Packet\n");*/
			ProcessSSDPRequest(sssdp, (unsigned short)runtime_vars.port);
		}
		if (smonitor >= 0 && FD_ISSET(smonitor, &readset))
		{
			ProcessMonitorEvent(smonitor);
		}
		/* increment SystemUpdateID if the content database has changed,
		 * and if there is an active HTTP connection, at most once every 2 seconds */
		if (i && (timeofday.tv_sec >= (lastupdatetime + 2)))
		{
			if (GETFLAG(SCANNING_MASK))
			{
				time_t dbtime = _get_dbtime();
				if (dbtime != lastdbtime)
				{
					lastdbtime = dbtime;
					last_changecnt = -1;
				}
			}
			if (sqlite3_total_changes(db) != last_changecnt)
			{
				updateID++;
				last_changecnt = sqlite3_total_changes(db);
				lastupdatetime = timeofday.tv_sec;
			}
		}
		/* process active HTTP connections */
		for (e = upnphttphead.lh_first; e != NULL; e = e->entries.le_next)
		{
			if ((e->socket >= 0) && (e->state <= 2) && (FD_ISSET(e->socket, &readset)))
				Process_upnphttp(e);
		}
		/* process incoming HTTP connections */
		if (shttpl >= 0 && FD_ISSET(shttpl, &readset))
		{
			int shttp;
			socklen_t clientnamelen;
			struct sockaddr_in clientname;
			clientnamelen = sizeof(struct sockaddr_in);
			shttp = accept(shttpl, (struct sockaddr *)&clientname, &clientnamelen);
			if (shttp<0)
			{
				DPRINTF(E_ERROR, L_GENERAL, "accept(http): %s\n", strerror(errno));
			}
			else
			{
				struct upnphttp * tmp = 0;
				DPRINTF(E_DEBUG, L_GENERAL, "HTTP connection from %s:%d\n",
					inet_ntoa(clientname.sin_addr),
					ntohs(clientname.sin_port) );
				/*if (fcntl(shttp, F_SETFL, O_NONBLOCK) < 0) {
					DPRINTF(E_ERROR, L_GENERAL, "fcntl F_SETFL, O_NONBLOCK\n");
				}*/
				/* Create a new upnphttp object and add it to
				 * the active upnphttp object list */
				tmp = New_upnphttp(shttp);
				if (tmp)
				{
					tmp->clientaddr = clientname.sin_addr;
					LIST_INSERT_HEAD(&upnphttphead, tmp, entries);
				}
				else
				{
					DPRINTF(E_ERROR, L_GENERAL, "New_upnphttp() failed\n");
					close(shttp);
				}
			}
		}
		/* delete finished HTTP connections */
		for (e = upnphttphead.lh_first; e != NULL; e = next)
		{
			next = e->entries.le_next;
			if(e->state >= 100)
			{
				LIST_REMOVE(e, entries);
				Delete_upnphttp(e);
			}
		}
	}

shutdown:
	/* kill the scanner */
	if (GETFLAG(SCANNING_MASK) && scanner_pid)
		kill(scanner_pid, SIGKILL);

	/* close out open sockets */
	while (upnphttphead.lh_first != NULL)
	{
		e = upnphttphead.lh_first;
		LIST_REMOVE(e, entries);
		Delete_upnphttp(e);
	}
	if (sssdp >= 0)
		close(sssdp);
	if (shttpl >= 0)
		close(shttpl);
	if (smonitor >= 0)
		close(smonitor);
	
	for (i = 0; i < n_lan_addr; i++)
	{
		SendSSDPGoodbyes(lan_addr[i].snotify);
		close(lan_addr[i].snotify);
	}

	if (inotify_thread)
	{
		pthread_kill(inotify_thread, SIGCHLD);
		pthread_join(inotify_thread, NULL);
	}

	/* kill other child processes */
	process_reap_children();
	free(children);

	sqlite3_close(db);

	if (pidfilename && unlink(pidfilename) < 0)
		DPRINTF(E_ERROR, L_GENERAL, "Failed to remove pidfile %s: %s\n", pidfilename, strerror(errno));

	log_close();

	exit(EXIT_SUCCESS);
}

