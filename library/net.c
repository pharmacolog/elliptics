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

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sendfile.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

#include "elliptics.h"
#include "dnet/packet.h"
#include "dnet/interface.h"

int dnet_socket_create_addr(struct dnet_node *n, int sock_type, int proto,
		struct sockaddr *sa, unsigned int salen, int listening)
{
	int s, err = -1;

	s = socket(sa->sa_family, sock_type, proto);
	if (s < 0) {
		dnet_log_err(n, "Failed to create socket for %s:%d: "
				"family: %d, sock_type: %d, proto: %d",
				dnet_server_convert_addr(sa, salen),
				dnet_server_convert_port(sa, salen),
				sa->sa_family, sock_type, proto);
		goto err_out_exit;
	}

	if (listening) {
		err = 1;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &err, 4);

		err = bind(s, sa, salen);
		if (err) {
			dnet_log_err(n, "Failed to bind to %s:%d",
				dnet_server_convert_addr(sa, salen),
				dnet_server_convert_port(sa, salen));
			goto err_out_exit;
		}

		err = listen(s, 1024);
		if (err) {
			dnet_log_err(n, "Failed to listen at %s:%d",
				dnet_server_convert_addr(sa, salen),
				dnet_server_convert_port(sa, salen));
			goto err_out_exit;
		}

		dnet_log(n, "Server is now listening at %s:%d.\n",
				dnet_server_convert_addr(sa, salen),
				dnet_server_convert_port(sa, salen));
	} else {
		err = connect(s, sa, salen);
		if (err) {
			dnet_log_err(n, "Failed to connect to %s:%d",
				dnet_server_convert_addr(sa, salen),
				dnet_server_convert_port(sa, salen));
			goto err_out_exit;
		}

		dnet_log(n, "Connected to %s:%d.\n",
			dnet_server_convert_addr(sa, salen),
			dnet_server_convert_port(sa, salen));

		fcntl(s, F_SETFL, O_NONBLOCK);
	}

	return s;

err_out_exit:
	return err;
}

int dnet_socket_create(struct dnet_node *n, struct dnet_config *cfg,
		struct sockaddr *sa, unsigned int *addr_len, int listening)
{
	int s, err = -EINVAL;
	struct addrinfo *ai, hint;

	memset(&hint, 0, sizeof(struct addrinfo));

	hint.ai_flags = AI_NUMERICSERV;
	hint.ai_family = cfg->family;
	hint.ai_socktype = cfg->sock_type;
	hint.ai_protocol = cfg->proto;

	err = getaddrinfo(cfg->addr, cfg->port, &hint, &ai);
	if (err) {
		dnet_log(n, "Failed to get address info for %s:%s, family: %d, err: %d.\n",
				cfg->addr, cfg->port, cfg->family, err);
		goto err_out_exit;
	}

	s = dnet_socket_create_addr(n, cfg->sock_type, cfg->proto,
			ai->ai_addr, ai->ai_addrlen, listening);
	if (s < 0) {
		err = -1;
		goto err_out_free;
	}

	if (*addr_len >= ai->ai_addrlen)
		*addr_len = ai->ai_addrlen;
	else {
		dnet_log(n, "Failed to copy address: size %u is too small (must be more than %u).\n",
				*addr_len, ai->ai_addrlen);
		err = -ENOBUFS;
		goto err_out_close;
	}
	memcpy(sa, ai->ai_addr, *addr_len);

	freeaddrinfo(ai);

	return s;

err_out_close:
	close(s);
err_out_free:
	freeaddrinfo(ai);
err_out_exit:
	return err;
}

static int dnet_wait_fd(int s, unsigned int events, long timeout)
{
	struct pollfd pfd;
	int err;

	pfd.fd = s;
	pfd.revents = 0;
	pfd.events = events;

	err = poll(&pfd, 1, timeout);
	if (err < 0)
		goto out_exit;

	if (err == 0) {
		err = -EAGAIN;
		goto out_exit;
	}

	if (pfd.revents & events) {
		err = 0;
		goto out_exit;
	}

	err = -EINVAL;
out_exit:
	return err;
}

static int dnet_net_reconnect(struct dnet_net_state *st)
{
	int err;

	if (st->join_state == DNET_CLIENT)
		return -EINVAL;

	err = dnet_socket_create_addr(st->n, st->n->sock_type, st->n->proto,
			(struct sockaddr *)&st->addr, st->addr.addr_len, 0);
	if (err < 0)
		return err;

	if (st->s > 0)
		close(st->s);
	st->s = err;

	st->join_state = DNET_REJOIN;
	st->n->join_state = DNET_REJOIN;

	return 0;
}

int dnet_send(struct dnet_net_state *st, void *data, unsigned int size)
{
	int err = 0;
	unsigned int orig_size = size;
	void *orig_data = data;
	struct dnet_node *n = st->n;

again:
	while (size) {
		err = dnet_wait_fd(st->s, POLLOUT, st->timeout);
		if (st->n->need_exit) {
			err = -EIO;
			break;
		}

		if (err == -EAGAIN)
			continue;

		if (err < 0) {
			dnet_log(n, "Failed to wait for descriptor: err: %d, socket: %d.\n", err, st->s);
			break;
		}

		err = send(st->s, data, size, 0);
		if (err < 0) {
			err = -errno;
			dnet_log_err(n, "Failed to send packet: size: %u, socket: %d", size, st->s);
			break;
		}

		if (err == 0) {
			dnet_log(n, "Peer has dropped the connection: socket: %d.\n", st->s);
			err = -ECONNRESET;
			break;
		}

		data += err;
		size -= err;

		err = 0;
	}

	if (!err)
		return 0;

	size = orig_size;
	data = orig_data;

	goto again;
}

int dnet_wait(struct dnet_net_state *st)
{
	return dnet_wait_fd(st->s, POLLIN, st->timeout);
}

int dnet_recv(struct dnet_net_state *st, void *data, unsigned int size)
{
	int err;

	while (size) {
		err = dnet_wait_fd(st->s, POLLIN, st->timeout);
		if (st->n->need_exit)
			return -EIO;
		if (err == -EAGAIN)
			continue;
		if (err < 0)
			return err;

		err = recv(st->s, data, size, 0);
		if (err < 0) {
			dnet_log_err(st->n, "Failed to recv packet: size: %u", size);
			return err;
		}

		if (err == 0) {
			dnet_log(st->n, "Peer has disconnected.\n");
			return -ECONNRESET;
		}

		data += err;
		size -= err;
	}

	return 0;
}

void *dnet_state_process(void *data)
{
	struct dnet_net_state *st = data;
	struct dnet_node *n = st->n;
	int err;

	while (!n->need_exit) {
		err = dnet_trans_process(st);
		if ((err < 0) && (err != -EAGAIN)) {
			dnet_log(n, "%s: state processing error: %d.\n", dnet_dump_id(st->id), err);

			if (st->join_state == DNET_CLIENT)
				break;

			pthread_mutex_lock(&st->lock);
			err = dnet_net_reconnect(st);
			pthread_mutex_unlock(&st->lock);
			if (err)
				sleep(1);
		}
	}

	dnet_log(n, "%s: stopped client %s processing, refcnt: %d.\n", dnet_dump_id(st->id),
		dnet_server_convert_dnet_addr(&st->addr), st->refcnt);

	dnet_state_put(st);

	return NULL;
}

struct dnet_net_state *dnet_state_create(struct dnet_node *n, unsigned char *id,
		struct dnet_addr *addr, int s, void *(* process)(void *))
{
	int err = -ENOMEM;
	struct dnet_net_state *st;

	st = malloc(sizeof(struct dnet_net_state));
	if (!st)
		goto err_out_exit;

	memset(st, 0, sizeof(struct dnet_net_state));

	st->timeout = n->wait_ts.tv_sec * 1000;
	st->s = s;
	st->n = n;
	st->refcnt = 1;

	memcpy(&st->addr, addr, sizeof(struct dnet_addr));

	err = pthread_mutex_init(&st->lock, NULL);
	if (err) {
		dnet_log_err(n, "%s: failed to initialize state lock: err: %d", dnet_dump_id(st->id), err);
		goto err_out_state_free;
	}

	st->join_state = DNET_CLIENT;
	if (!id) {
		pthread_mutex_lock(&n->state_lock);
		list_add_tail(&st->state_entry, &n->empty_state_list);
		pthread_mutex_unlock(&n->state_lock);
	} else {
		memcpy(st->id, id, DNET_ID_SIZE);
		err = dnet_state_insert(st);
		if (err)
			goto err_out_lock_destroy;
	}

	err = pthread_create(&st->tid, NULL, process, st);
	if (err) {
		dnet_log_err(n, "%s: failed to create network state processing thread: err: %d",
				dnet_dump_id(st->id), err);
		goto err_out_state_remove;
	}

	return st;

err_out_state_remove:
	dnet_state_remove(st);
err_out_lock_destroy:
	pthread_mutex_destroy(&st->lock);
err_out_state_free:
	free(st);
err_out_exit:
	return NULL;
}

void dnet_state_put(struct dnet_net_state *st)
{
	int destroy = 0;

	if (!st)
		return;

	pthread_mutex_lock(&st->lock);
	st->refcnt--;

	if (st->refcnt == 0)
		destroy = 1;
	pthread_mutex_unlock(&st->lock);

	if (!destroy)
		return;

	dnet_state_remove(st);

	if (st->s)
		close(st->s);

	pthread_mutex_destroy(&st->lock);

	dnet_log(st->n, "%s: freeing state %s.\n", dnet_dump_id(st->id),
		dnet_server_convert_dnet_addr(&st->addr));

	free(st);
}

int dnet_sendfile_data(struct dnet_net_state *st, char *file,
		int fd, off_t offset, size_t size,
		void *header, unsigned int hsize)
{
	ssize_t err;

	pthread_mutex_lock(&st->lock);
	err = dnet_send(st, header, hsize);
	if (err)
		goto err_out_unlock;

	while (size) {
		err = dnet_wait_fd(st->s, POLLOUT, st->timeout);
		if (st->n->need_exit) {
			err = -EIO;
			break;
		}

		if (err == -EAGAIN)
			continue;

		if (err < 0) {
			dnet_log(st->n, "Failed to wait for descriptor: err: %d, socket: %d.\n", err, st->s);
			break;
		}

		err = sendfile(st->s, fd, &offset, size);
		if (err < 0) {
			dnet_log_err(st->n, "%s: failed to send file data, err: %d", dnet_dump_id(st->id), err);
			goto err_out_unlock;
		}

		if (err == 0) {
			dnet_log(st->n, "%s: looks like truncated file, size: %zu.\n", dnet_dump_id(st->id), size);
			break;
		}

		dnet_log(st->n, "%s: size: %zu, rest: %zu, offset: %llu, err: %zu.\n",
				dnet_dump_id(st->id), size, size-err, offset, err);

		size -= err;
	}

	if (size) {
		char buf[4096];
		unsigned int sz;

		memset(buf, 0, sizeof(buf));

		dnet_log(st->n, "%s: truncated file: '%s', orig: %zu, zeroes: %zu bytes.\n",
				dnet_dump_id(st->id), file, size + err, size);

		while (size) {
			sz = size;
			if (sz > sizeof(buf))
				sz = sizeof(buf);

			err = dnet_send(st, buf, sz);
			if (err)
				goto err_out_unlock;

			size -= sz;
		}
	}
	pthread_mutex_unlock(&st->lock);

	return 0;

err_out_unlock:
	pthread_mutex_unlock(&st->lock);
	return err;
}
