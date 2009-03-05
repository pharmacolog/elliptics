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

#ifndef __DNET_INTERFACE_H
#define __DNET_INTERFACE_H

#include "elliptics.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This set of helpers is called in the completion callbacks to
 * get the appropriate pointers.
 *
 * Callback transaction structure.
 *
 * [el_cmd]
 * [el_attr] [attributes]
 *
 * [el_cmd] header when present shows number of attached bytes.
 * It should be equal to the al_attr structure at least in the
 * correct message, otherwise it should be discarded.
 * One can also check cmd->flags if it has DNET_FLAGS_MORE or
 * DNET_FLAGS_DESTROY bit set. The former means that callback
 * will be invoked again in the future and transaction is not
 * yet completed. The latter means that transaction is about
 * to be destroyed.
 *
 * If command size makes sense, its data can be obtained using
 * dnet_trans_data() helper. Returned pointer (if not NULL) should
 * be dereferenced into el_attr structure and its size has to be
 * checked. All data is always packed as set of nested attributes.
 *
 * Private data stored during transaction setup can be obtained
 * using dnet_trans_private() helper.
 */
static inline struct el_cmd *dnet_trans_cmd(struct dnet_trans *t)
{
	if (t)
		return &t->cmd;
	return NULL;
}

static inline void *dnet_trans_private(struct dnet_trans *t)
{
	if (t)
		return t->priv;
	return NULL;
}

static inline void *dnet_trans_data(struct dnet_trans *t)
{
	if (t)
		return t->data;
	return NULL;
}

/*
 * IO helpers.
 *
 * dnet_node is a node pointer returned by calling dnet_node_create()
 * el_io_attr contains IO details (size, offset and the checksum)
 * completion callback (if present) will be invoked when IO transaction is finished
 * private data will be stored in the appropriate transaction and can be obtained
 * when transaction completion callback is invoked. It will be automatically
 * freed when transaction is completed.
 */
int dnet_read_object(struct dnet_node *n, struct el_io_attr *io,
	int (* complete)(struct dnet_trans *t, struct dnet_net_state *st), void *priv);
int dnet_read_file(struct dnet_node *n, char *file, __u64 offset, __u64 size);

int dnet_write_object(struct dnet_node *n, unsigned char *id, struct el_io_attr *io,
		int (* complete)(struct dnet_trans *t, struct dnet_net_state *st), void *priv,
		void *data);
int dnet_update_file(struct dnet_node *n, char *file, off_t offset, void *data, unsigned int size, int append);
int dnet_write_file(struct dnet_node *n, char *file);

#define DNET_MAX_ADDRLEN		256
#define DNET_MAX_PORTLEN		8

/*
 * Node configuration interface.
 */
struct dnet_config
{
	/*
	 * Unique network-wide ID.
	 */
	unsigned char		id[EL_ID_SIZE];

	/*
	 * Socket type (SOCK_STREAM, SOCK_DGRAM and so on),
	 * a protocol (IPPROTO_TCP for example) and
	 * a family (AF_INET, AF_INET6 and so on)
	 * of the appropriate socket. These parameters are
	 * sent in the lookup replies so that remote nodes
	 * could know how to connect to this one.
	 */
	int			sock_type, proto, family;

	/*
	 * Socket address/port suitable for the getaddrinfo().
	 */
	char			addr[DNET_MAX_ADDRLEN];
	char			port[DNET_MAX_PORTLEN];
};

/*
 * Transformation functions are used to create ID from the provided data content.
 * One can add/remove them in a run-time. init/update/final sequence is used
 * each time for every transformed block, update can be invoked multiple times
 * between init and final ones.
 */
int dnet_add_transform(struct dnet_node *n, void *priv, char *name,
	int (* init)(void *priv),
	int (* update)(void *priv, void *src, __u64 size,
		void *dst, unsigned int *dsize, unsigned int flags),
	int (* final)(void *priv, void *dst, unsigned int *dsize, unsigned int flags));
int dnet_remove_transform(struct dnet_node *n, char *name);

/*
 * Node creation/destruction callbacks. Node is a building block of the storage
 * and it is needed for every operation one may want to do with the network.
 */
struct dnet_node *dnet_node_create(struct dnet_config *);
void dnet_node_destroy(struct dnet_node *n);

/*
 * dnet_add_state() is used to add a node into the route list, the more
 * routes are added the less network lookups will be performed to send/receive
 * data requests.
 */
int dnet_add_state(struct dnet_node *n, struct dnet_config *cfg);

/*
 * This is used to join the network. When function is completed, node will be
 * used to store data sent from the network.
 */
int dnet_join(struct dnet_node *n);

/*
 * Sets the root directory to store data objects.
 */
int dnet_setup_root(struct dnet_node *n, char *root);

#ifdef __cplusplus
}
#endif

#endif /* __DNET_INTERFACE_H */