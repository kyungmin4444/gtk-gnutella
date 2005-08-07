/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Alex Bennee <alex@bennee.com> & Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup core
 * @file
 *
 * Search queue.
 *
 * This file takes care of paceing search messages out at a rate
 * that doesn't flood the gnutella network. A search queue is
 * maintained for each gnutella node and regularly polled by the
 * timer function to release messages into the lower message queues
 *
 * For ultrapeers conducting dynamic querying for their own queries,
 * this system of having one search queue per node is not used.  Instead,
 * we have one global search queue, which is used to space launching
 * of dynamic queries.
 *
 * @author Alex Bennee <alex@bennee.com>
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#include "common.h"

RCSID("$Id$");

#include "sq.h"					/* search_queue structures */
#include "pmsg.h"
#include "nodes.h"
#include "search.h"
#include "dq.h"

#include "if/gnet_property_priv.h"

#include "lib/atoms.h"
#include "lib/walloc.h"
#include "lib/override.h"		/* Must be the last header included */

/*
 * Compute start of search string (which is NUL terminated) in query.
 * The "+2" skips the "speed" field in the query.
 */
#define QUERY_TEXT(m)	((m) + sizeof(struct gnutella_header) + 2)

/**
 * A search queue entry.
 *
 * Each entry references the search that issued the query.  Before sending
 * the query message, a check will be made to make sure we are not
 * over-querying for that particular search.
 */
typedef struct smsg {
	pmsg_t *mb;					/**< The message block for the query */
	gnet_search_t shandle;		/**< Handle to search that originated query */
	query_hashvec_t *qhv;		/**< The query hash vector for QRP matching */
} smsg_t;

/**
 * Message information for mutated blocks.
 *
 * Records meta-information about the message being queued so that we may
 * react when the message queue informs us it has processed it.
 */
struct smsg_info {
	gpointer search;			/**< The search object which sends the query */
	guint32 id;					/**< The unique search ID */
	guint32 node_id;			/**< The unique node ID to which we're sending */
};

static squeue_t *global_sq = NULL;

static void cap_queue(squeue_t *sq);

/**
 * Free routine for a query message.
 */
static void
sq_pmsg_free(pmsg_t *mb, gpointer arg)
{
	struct smsg_info *smi = (struct smsg_info *) arg;

	g_assert(pmsg_is_extended(mb));

	/*
	 * If we're still in leaf mode, let the search know that we sent a
	 * query for it to the specified node ID.
	 */

	if (current_peermode == NODE_P_LEAF)
		search_notify_sent(smi->search, smi->id, smi->node_id);

	wfree(smi, sizeof(*smi));
}

/***
 *** Search queue entry management.
 ***/

/**
 * Allocate a new search queue entry.
 */
static smsg_t *
smsg_alloc(gnet_search_t sh, pmsg_t *mb, query_hashvec_t *qhv)
{
	smsg_t *sb = walloc(sizeof(*sb));

	sb->shandle = sh;
	sb->mb = mb;
	sb->qhv = qhv;

	return sb;
}

/**
 * Dispose of the search queue entry.
 */
static void
smsg_free(smsg_t *sb)
{
	g_assert(sb);

	wfree(sb, sizeof(*sb));
}

/**
 * Dispose of the search queue entry and of all its contained data.
 * Used only when the query described in `sb' is not dispatched.
 */
static void
smsg_discard(smsg_t *sb)
{
	pmsg_free(sb->mb);
	if (sb->qhv)
		qhvec_free(sb->qhv);

	smsg_free(sb);
}

/**
 * Mutate the message so that we can be notified about its freeing by
 * the mq to which it will be sent to.
 */
static void
smsg_mutate(smsg_t *sb, struct gnutella_node *n)
{
	struct smsg_info *smi;
	pmsg_t *omb;

	smi = (struct smsg_info *) walloc(sizeof(*smi));
	smi->id = search_get_id(sb->shandle, &smi->search);
	smi->node_id = n->id;

	omb = sb->mb;
	sb->mb = pmsg_clone_extend(omb, sq_pmsg_free, smi);
	pmsg_free(omb);
}

/***
 *** "handle" hash table management.
 ***/

/**
 * Checks whether an entry exists in the search queue for given search handle.
 */
static gboolean
sqh_exists(squeue_t *sq, gnet_search_t sh)
{
	g_assert(sq != NULL);

	return NULL != g_hash_table_lookup(sq->handles, GUINT_TO_POINTER(sh));
}

/**
 * Record search handle in the hash table.
 */
static void
sqh_put(squeue_t *sq, gnet_search_t sh)
{
	g_assert(sq != NULL);
	g_assert(!sqh_exists(sq, sh));

	g_hash_table_insert(sq->handles, GUINT_TO_POINTER(sh), GINT_TO_POINTER(1));
}

/**
 * Remove search handle from the hash table.
 */
static void
sqh_remove(squeue_t *sq, gnet_search_t sh)
{
	gpointer key;
	gpointer value;
	gboolean found;

	g_assert(sq != NULL);

	found = g_hash_table_lookup_extended(sq->handles,
				GUINT_TO_POINTER(sh), &key, &value);

	g_assert(found);
	g_assert((gnet_search_t) GPOINTER_TO_UINT(key) == sh);

	g_hash_table_remove(sq->handles, GUINT_TO_POINTER(sh));
}

/***
 *** Search queue.
 ***/

/**
 * Create a new search queue.
 */
squeue_t *
sq_make(struct gnutella_node *node)
{
    squeue_t *sq;

    sq = walloc(sizeof(*sq));

	/*
	 * By initializing `last_sent' to the current time and not to `0', we
	 * ensure that we won't send the query to the node during the first
	 * "search_queue_spacing" seconds of its connection.  This prevent
	 * useless traffic on Gnet, because if the connection is held for that
	 * long, chances are it will hold until we get some results back.
	 *
	 *		--RAM, 01/05/2002
	 */

	sq->count		= 0;
	sq->last_sent 	= time(NULL);
	sq->searches 	= NULL;
	sq->n_sent 		= 0;
	sq->n_dropped 	= 0;
	sq->node        = node;
	sq->handles     = g_hash_table_new(NULL, NULL);

	return sq;
}

/**
 * Clear all queued searches.
 */
void
sq_clear(squeue_t *sq)
{
	GList *l;

	g_assert(sq);

	if (dbg > 3)
		printf("clearing sq node %s (sent=%d, dropped=%d)\n",
			sq->node ? node_addr(sq->node) : "GLOBAL",
			sq->n_sent, sq->n_dropped);

	for (l = sq->searches; l; l = g_list_next(l)) {
		smsg_t *sb = (smsg_t *) l->data;

		smsg_discard(sb);
	}

	g_list_free(sq->searches);

	sq->searches = NULL;
	sq->count = 0;
}

/**
 * Free queue and all queued searches.
 */
void
sq_free(squeue_t *sq)
{
	g_assert(sq);

	sq_clear(sq);
	g_hash_table_destroy(sq->handles);
	wfree(sq, sizeof(*sq));
}

/**
 * Enqueue query message in specified queue.
 */
static void
sq_puthere(squeue_t *sq, gnet_search_t sh, pmsg_t *mb, query_hashvec_t *qhv)
{
	smsg_t *sb;

	g_assert(sq);
	g_assert(mb);

	if (sqh_exists(sq, sh))
		return;						/* Search already in queue */

	sb = smsg_alloc(sh, mb, qhv);

	sqh_put(sq, sh);
	sq->searches = g_list_prepend(sq->searches, sb);
	sq->count++;

	if (sq->count > search_queue_size)
		cap_queue(sq);
}


/**
 * Enqueue a single query (LIFO behaviour).
 *
 * Having the search handle allows us to check before sending the query
 * that we are not over-querying for a given search.  It's also handy
 * to remove the queries when a search is closed, and avoid queuing twice
 * the same search.
 *
 * @param sq DOCUMENT THIS!
 * @param mb the query message
 * @param sh the search handle
 */
void
sq_putq(squeue_t *sq, gnet_search_t sh, pmsg_t *mb)
{
	sq_puthere(sq, sh, mb, NULL);
}

/**
 * Enqueue a single query waiting for dynamic querying into global SQ.
 *
 * @param mb the query message
 * @param sh the search handle
 * @param qhv the query hash vector for QRP matching
 */
void
sq_global_putq(gnet_search_t sh, pmsg_t *mb, query_hashvec_t *qhv)
{
	sq_puthere(global_sq, sh, mb, qhv);
}

/**
 * Decides if the queue can send a message. Currently use simple fixed
 * time base heuristics. May add bursty control later...
 */
void
sq_process(squeue_t *sq, time_t now)
{
	GList *item;
	smsg_t *sb;
	struct gnutella_node *n;
	gboolean sent;

	g_assert(sq->node == NULL || sq->node->outq != NULL);

retry:
	/*
	 * We don't need to do anything if either:
	 *
	 * 1. The queue is empty.
	 * 2. We sent our last search less than "search_queue_spacing" seconds ago.
	 * 3. We never got a packet from that node.
	 * 4. The node activated hops-flow to shut all queries
	 * 5. We activated flow-control on the node locally.
	 *
	 *		--RAM, 01/05/2002
	 */

	if (sq->count == 0)
		return;

    if (delta_time(now, sq->last_sent) < search_queue_spacing)
		return;

	n = sq->node;					/* Will be NULL for the global SQ */

	if (n != NULL) {
		if (n->received == 0)		/* RX = 0, wait for handshaking ping */
			return;

		if (!node_query_hops_ok(n, 0))		/* Cannot send hops=0 query */
			return;

		if (!NODE_IS_WRITABLE(n))
			return;

		if (NODE_IN_TX_FLOW_CONTROL(n))		/* Don't add to the mqueue yet */
			return;
	} else {
		/*
		 * Processing the global SQ.
		 */

		if (current_peermode != NODE_P_ULTRA)
			return;

		if (node_keep_missing() * 3 > 2 * up_connections)
			return;							/* Not enough nodes for querying */
	}

	/*
	 * Queue is managed as a LIFO: we extract the first message, i.e. the last
	 * one enqueued, and pass it along to the node's message queue.
	 */

	g_assert(sq->searches);

	item = g_list_first(sq->searches);
	sb = (smsg_t *) item->data;

	g_assert(sq->count > 0);
	sq->count--;
	sent = TRUE;			/* Assume we're going to send/initiate it */

	if (n == NULL) {
		g_assert(sb->qhv != NULL);		/* Enqueued via sq_global_putq() */

		if (dbg > 2)
			printf("sq GLOBAL, queuing \"%s\" (%u left, %d sent)\n",
				QUERY_TEXT(pmsg_start(sb->mb)), sq->count, sq->n_sent);

		dq_launch_local(sb->shandle, sb->mb, sb->qhv);

	} else if (search_query_allowed(sb->shandle)) {
		/*
		 * Must log before sending, in case the queue discards the message
		 * buffer immediately.
		 */

		g_assert(sb->qhv == NULL);		/* Enqueued via sq_putq() */

		if (dbg > 2)
			printf("sq for node %s, queuing \"%s\" (%u left, %d sent)\n",
				node_addr(n), QUERY_TEXT(pmsg_start(sb->mb)),
				sq->count, sq->n_sent);

		/*
		 * If we're a leaf node, we're doing a leaf-guided dynamic query.
		 * In order to be able to report hits we get to the UPs to whom
		 * we sent our searches, we need to be notified of all the physical
		 * queries that go out.
		 */

		if (current_peermode == NODE_P_LEAF)
			smsg_mutate(sb, n);

		mq_putq(n->outq, sb->mb);

	} else {
		if (dbg > 4)
			printf("sq for node %s, ignored \"%s\" (%u left, %d sent)\n",
				node_addr(n), QUERY_TEXT(pmsg_start(sb->mb)),
				sq->count, sq->n_sent);
		pmsg_free(sb->mb);
		if (sb->qhv)
			qhvec_free(sb->qhv);
		sent = FALSE;
	}

	if (sent) {
		sq->n_sent++;
		sq->last_sent = now;
	}

	sqh_remove(sq, sb->shandle);
	smsg_free(sb);
	sq->searches = g_list_remove_link(sq->searches, item);
	g_list_free_1(item);

	/*
	 * If we ignored the query, retry with the next in the queue.
	 * We don't use a do/while() loop to avoid identing the whole body.
	 */

	if (!sent)
		goto retry;
}

/**
 * Decides if it needs to drop the oldest messages on the
 * search queue based on the search count
 */
static void
cap_queue(squeue_t *sq)
{
    while (sq->count > search_queue_size) {
    	GList *item = g_list_last(sq->searches);
		smsg_t *sb = (smsg_t *) item->data;

		sq->searches = g_list_remove_link(sq->searches, item);

		g_assert(sq->count > 0);
		sq->count--;
		sq->n_dropped++;

		if (dbg > 4)
			printf("sq for node %s, dropped \"%s\" (%u left, %d dropped)\n",
				node_addr(sq->node), QUERY_TEXT(pmsg_start(sb->mb)),
				sq->count, sq->n_dropped);

		sqh_remove(sq, sb->shandle);
		smsg_discard(sb);
		g_list_free_1(item);
    }
}

/**
 * Signals the search queue that a search was closed.
 * Any query for that search still in the queue is dropped.
 */
void
sq_search_closed(squeue_t *sq, gnet_search_t sh)
{
	GList *l;
	GList *next;

	for (l = sq->searches; l; l = next) {
		smsg_t *sb = (smsg_t *) l->data;

		next = g_list_next(l);

		if (sb->shandle != sh)
			continue;

		g_assert(sq->count > 0);
		sq->count--;
		sq->searches = g_list_remove_link(sq->searches, l);

		if (dbg > 4)
			printf("sq for node %s, dropped \"%s\" on search close (%u left)\n",
				sq->node ? node_addr(sq->node) : "GLOBAL",
				QUERY_TEXT(pmsg_start(sb->mb)), sq->count);

		sqh_remove(sq, sb->shandle);
		smsg_discard(sb);
		g_list_free_1(l);
	}

	g_assert(sq->searches || sq->count == 0);
}

/**
 * Invoked when the current peermode changes.
 */
void
sq_set_peermode(node_peer_t mode)
{
	/*
	 * Get rid of all the searches enqueued whilst we were an UP.
	 * Searches will be re-issued as a leaf node at their next retry time.
	 *
	 * XXX could perhaps go back and reschedule searches to start soon,
	 * XXX so that they don't get penalized too badly from being demoted?
	 * XXX		--RAM, 2004-09-02
	 */

	if (mode != NODE_P_ULTRA)
		sq_clear(global_sq);
}

/**
 * @returns global queue.
 */
squeue_t *
sq_global_queue(void)
{
	return global_sq;
}

/**
 * Initialization of SQ at startup.
 */
void
sq_init(void)
{
	global_sq = sq_make(NULL);
}

/**
 * Cleanup at shutdown time.
 */
void
sq_close(void)
{
	sq_free(global_sq);
	global_sq = NULL;
}

/* vi: set ts=4: */
