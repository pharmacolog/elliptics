/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/syscall.h>

#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <netinet/in.h>

#include "elliptics/packet.h"
#include "elliptics/interface.h"

#include "backends.h"
#include "common.h"

#ifndef __unused
#define __unused	__attribute__ ((unused))
#endif

static struct dnet_log ioclient_logger;

static void dnet_usage(char *p)
{
	fprintf(stderr, "Usage: %s\n"
			" -r addr:port:family  - adds a route to the given node\n"
			" -W file              - write given file to the network storage\n"
			" -s                   - request IO counter stats from node\n"
			" -z                   - request VFS IO stats from node\n"
			" -a                   - request stats from all connected nodes\n"
			" -U status            - update server status: 1 - elliptics exits, 2 - goes RO\n"
			" -R file              - read given file from the network into the local storage\n"
			" -I id                - transaction id (used to read data)\n"
			" -g groups            - group IDs to connect\n"
			" -c cmd-event         - execute command with given event on the remote node\n"
			" -L file              - lookup a storage which hosts given file\n"
			" -l log               - log file. Default: disabled\n"
			" -w timeout           - wait timeout in seconds used to wait for content sync.\n"
			" ...                  - parameters can be repeated multiple times\n"
			"                        each time they correspond to the last added node\n"
			" -m level             - log level\n"
			" -M level             - set new log level\n"
			" -F flags             - change node flags (see @cfg->flags comments in include/elliptics/interface.h)\n"
			" -O offset            - read/write offset in the file\n"
			" -S size              - read/write transaction size\n"
			" -u file              - unlink file\n"
			" -N namespace         - use this namespace for operations\n"
			" -D object            - read latest data for given object, if -I id is specified, this field is unused\n"
			" -C flags             - command flags\n"
			" -t column            - column ID to read or write\n"
			" -d                   - start defragmentation\n"
			" -i flags             - IO flags (see DNET_IO_FLAGS_* in include/elliptics/packet.h\n"
			, p);
}

int main(int argc, char *argv[])
{
	int ch, err, i, have_remote = 0;
	int io_counter_stat = 0, vfs_stat = 0, single_node_stat = 1;
	struct dnet_node_status node_status;
	int update_status = 0;
	struct dnet_node *n = NULL;
	struct dnet_config cfg, rem, *remotes = NULL;
	char *logfile = "/dev/stderr", *readf = NULL, *writef = NULL, *cmd = NULL, *lookup = NULL;
	char *read_data = NULL;
	char *removef = NULL;
	unsigned char trans_id[DNET_ID_SIZE], *id = NULL;
	FILE *log = NULL;
	uint64_t offset, size;
	int *groups = NULL, group_num = 0;
	int type = EBLOB_TYPE_DATA;
	uint64_t cflags = 0;
	uint64_t ioflags = 0;
	int defrag = 0;
	sigset_t mask;

	memset(&node_status, 0, sizeof(struct dnet_node_status));
	memset(&cfg, 0, sizeof(struct dnet_config));

	node_status.nflags = -1;
	node_status.status_flags = -1;
	node_status.log_level = ~0U;

	size = offset = 0;

	cfg.sock_type = SOCK_STREAM;
	cfg.proto = IPPROTO_TCP;
	cfg.wait_timeout = 60;
	ioclient_logger.log_level = DNET_LOG_ERROR;

	memcpy(&rem, &cfg, sizeof(struct dnet_config));

	while ((ch = getopt(argc, argv, "i:dC:t:A:F:M:N:g:u:O:S:m:zsU:aL:w:l:c:I:r:W:R:D:h")) != -1) {
		switch (ch) {
			case 'i':
				ioflags = strtoull(optarg, NULL, 0);
				break;
			case 'd':
				defrag = 1;
				break;
			case 'C':
				cflags = strtoull(optarg, NULL, 0);
				break;
			case 't':
				type = atoi(optarg);
				break;
			case 'F':
				node_status.nflags = strtol(optarg, NULL, 0);
				update_status = 1;
				break;
			case 'M':
				node_status.log_level = atoi(optarg);
				update_status = 1;
				break;
			case 'N':
				cfg.ns = optarg;
				cfg.nsize = strlen(optarg);
				break;
			case 'u':
				removef = optarg;
				break;
			case 'O':
				offset = strtoull(optarg, NULL, 0);
				break;
			case 'S':
				size = strtoull(optarg, NULL, 0);
				break;
			case 'm':
				ioclient_logger.log_level = atoi(optarg);
				break;
			case 's':
				io_counter_stat = 1;
				break;
			case 'U':
				node_status.status_flags = strtol(optarg, NULL, 0);
				update_status = 1;
				break;
			case 'z':
				vfs_stat = 1;
				break;
			case 'a':
				single_node_stat = 0;
				break;
			case 'L':
				lookup = optarg;
				break;
			case 'w':
				cfg.check_timeout = cfg.wait_timeout = atoi(optarg);
				break;
			case 'l':
				logfile = optarg;
				break;
			case 'c':
				cmd = optarg;
				break;
			case 'I':
				err = dnet_parse_numeric_id(optarg, trans_id);
				if (err)
					return err;
				id = trans_id;
				break;
			case 'g':
				group_num = dnet_parse_groups(optarg, &groups);
				if (group_num <= 0)
					return -1;
				break;
			case 'r':
				err = dnet_parse_addr(optarg, &rem);
				if (err)
					return err;
				have_remote++;
				remotes = realloc(remotes, sizeof(rem) * have_remote);
				if (!remotes)
					return -ENOMEM;
				memcpy(&remotes[have_remote - 1], &rem, sizeof(rem));
				break;
			case 'W':
				writef = optarg;
				break;
			case 'R':
				readf = optarg;
				break;
			case 'D':
				read_data = optarg;
				break;
			case 'h':
			default:
				dnet_usage(argv[0]);
				return -1;
		}
	}
	
	log = fopen(logfile, "a");
	if (!log) {
		err = -errno;
		fprintf(stderr, "Failed to open log file %s: %s.\n", logfile, strerror(errno));
		return err;
	}

	ioclient_logger.log_private = log;
	ioclient_logger.log = dnet_common_log;
	cfg.log = &ioclient_logger;

	n = dnet_node_create(&cfg);
	if (!n)
		return -1;

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGCHLD);
	pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
	sigprocmask(SIG_UNBLOCK, &mask, NULL);

	dnet_node_set_groups(n, groups, group_num);

	if (have_remote) {
		int error = -ECONNRESET;
		for (i=0; i<have_remote; ++i) {
			if (single_node_stat && (vfs_stat || io_counter_stat))
				remotes[i].flags = DNET_CFG_NO_ROUTE_LIST;
			err = dnet_add_state(n, &remotes[i]);
			if (!err)
				error = 0;
		}

		if (error)
			return error;
	}

	if (defrag)
		return dnet_start_defrag(n, cflags);

	if (writef) {
		if (id) {
			struct dnet_id raw;

			dnet_setup_id(&raw, 0, id);
			raw.type = type;

			err = dnet_write_file_id(n, writef, &raw, offset, offset, size, cflags, ioflags);
		} else {
			err = dnet_write_file(n, writef, writef, strlen(writef), offset, offset, size, cflags, ioflags, type);
		}

		if (err)
			return err;
	}

	if (readf) {
		if (id) {
			struct dnet_id raw;

			dnet_setup_id(&raw, 0, id);
			raw.type = type;

			err = dnet_read_file_id(n, readf, &raw, offset, size);
		} else {
			err = dnet_read_file(n, readf, readf, strlen(readf), offset, size, type);
		}
		if (err)
			return err;
	}
	
	if (read_data) {
		void *data;
		struct dnet_id raw;
		struct dnet_io_attr io;

		if (!id) {
			dnet_transform(n, read_data, strlen(read_data), &raw);
		} else {
			memcpy(&raw.id, id, DNET_ID_SIZE);
		}
		raw.type = type;
		raw.group_id = 0; /* unused */

		memset(&io, 0, sizeof(io));
		io.type = type;
		io.flags = ioflags;
		memcpy(io.id, raw.id, DNET_ID_SIZE);
		memcpy(io.parent, raw.id, DNET_ID_SIZE);

		/* number of copies to check to find the latest data */
		io.num = group_num;

		err = dnet_read_latest(n, &raw, &io, cflags, &data);
		if (err)
			return err;

		data += sizeof(struct dnet_io_attr);
		io.size -= sizeof(struct dnet_io_attr);

		while (io.size) {
			err = write(1, data, io.size);
			if (err <= 0) {
				err = -errno;
				dnet_log_raw(n, DNET_LOG_ERROR, "%s: can not write data to stdout: %d %s",
						read_data, err, strerror(-err));
				return err;
			}

			io.size -= err;
		}
	}

	if (removef) {
		if (id) {
			struct dnet_id raw;

			dnet_setup_id(&raw, 0, id);
			raw.type = type;
			dnet_remove_object_now(n, &raw, cflags, ioflags);

			return 0;
		}

		err = dnet_remove_file(n, removef, strlen(removef), NULL, cflags, ioflags);
		if (err)
			return err;
	}

	if (cmd) {
		struct dnet_id __did, *did = NULL;
		struct sph *sph;
		int len = strlen(cmd);
		int event_size = len;
		char *ret = NULL;
		char *tmp;

		tmp = strchr(cmd, ' ');
		if (tmp) {
			event_size = tmp - cmd;
		}

		if (id) {
			did = &__did;

			dnet_setup_id(did, 0, id);
			did->type = type;
		}

		sph = malloc(sizeof(struct sph) + len + 1);
		if (!sph)
			return -ENOMEM;

		memset(sph, 0, sizeof(struct sph));

		sph->flags = DNET_SPH_FLAGS_SRC_BLOCK;
		sph->key = -1;
		sph->binary_size = 0;
		sph->data_size = len - event_size;
		sph->event_size = event_size;

		sprintf(sph->data, "%s", cmd);

		err = dnet_send_cmd(n, did, sph, (void **)&ret);
		if (err < 0)
			return err;

		free(sph);

		if (err > 0) {
			printf("%.*s\n", err, ret);
			free(ret);
		}
	}

	if (lookup) {
		err = dnet_lookup(n, lookup);
		if (err)
			return err;
	}

	if (vfs_stat) {
		err = dnet_request_stat(n, NULL, DNET_CMD_STAT, 0, NULL, NULL);
		if (err < 0)
			return err;
	}

	if (io_counter_stat) {
		err = dnet_request_stat(n, NULL, DNET_CMD_STAT_COUNT, DNET_ATTR_CNTR_GLOBAL, NULL, NULL);
		if (err < 0)
			return err;
	}

	if (update_status) {
		struct dnet_addr addr;

		for (i=0; i<have_remote; ++i) {
			memset(&addr, 0, sizeof(addr));
			addr.addr_len = sizeof(addr.addr);

			err = dnet_fill_addr(&addr, remotes[i].addr, remotes[i].port,
						remotes[i].family, remotes[i].sock_type, remotes[i].proto);
			if (err) {
				dnet_log_raw(n, DNET_LOG_ERROR, "ioclient: dnet_fill_addr: %s:%s:%d, sock_type: %d, proto: %d: %s %d\n",
						remotes[i].addr, remotes[i].port,
						remotes[i].family, remotes[i].sock_type, remotes[i].proto,
						strerror(-err), err);
			}

			err = dnet_update_status(n, &addr, NULL, &node_status);
			if (err) {
				dnet_log_raw(n, DNET_LOG_ERROR, "ioclient: dnet_update_status: %s:%s:%d, sock_type: %d, proto: %d: update: %d: "
						"%s %d\n",
						remotes[i].addr, remotes[i].port,
						remotes[i].family, remotes[i].sock_type, remotes[i].proto, update_status,
						strerror(-err), err);
			}
		}

	}

	dnet_node_destroy(n);

	return 0;
}

