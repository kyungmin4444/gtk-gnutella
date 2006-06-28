/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Richard Eckart
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
 * Needs brief description here.
 *
 * @author Richard Eckart
 * @date 2001-2003
 */

#include "common.h"

RCSID("$Id$");

#include "gnet_stats.h"
#include "gmsg.h"

#include "if/gnet_property_priv.h"

#include "lib/event.h"
#include "lib/endian.h"
#include "lib/override.h"		/* Must be the last header included */

static guint8 stats_lut[256];

static gnet_stats_t gnet_stats;
static gnet_stats_t gnet_tcp_stats;
static gnet_stats_t gnet_udp_stats;

static const gchar * const msg_drop_reason[MSG_DROP_REASON_COUNT] = {
	N_("Bad size"),							/**< MSG_DROP_BAD_SIZE */
	N_("Too small"),						/**< MSG_DROP_TOO_SMALL */
	N_("Too large"),						/**< MSG_DROP_TOO_LARGE */
	N_("Way too large"),					/**< MSG_DROP_WAY_TOO_LARGE */
	N_("Unknown message type"),				/**< MSG_DROP_UNKNOWN_TYPE */
	N_("Unexpected message"),				/**< MSG_DROP_UNEXPECTED */
	N_("Message sent with TTL = 0"),		/**< MSG_DROP_TTL0 */
	N_("Improper hops/ttl combination"),	/**< MSG_DROP_IMPROPER_HOPS_TTL */
	N_("Max TTL exceeded"),					/**< MSG_DROP_MAX_TTL_EXCEEDED */
	N_("Message throttle"),					/**< MSG_DROP_THROTTLE */
	N_("Unusable Pong"),					/**< MSG_DROP_PONG_UNUSABLE */
	N_("Hard TTL limit reached"),			/**< MSG_DROP_HARD_TTL_LIMIT */
	N_("Max hop count reached"),			/**< MSG_DROP_MAX_HOP_COUNT */
	N_("Route lost"),						/**< MSG_DROP_ROUTE_LOST */
	N_("No route"),							/**< MSG_DROP_NO_ROUTE */
	N_("Duplicate message"),				/**< MSG_DROP_DUPLICATE */
	N_("Message to banned GUID"),			/**< MSG_DROP_BANNED */
	N_("Node shutting down"),				/**< MSG_DROP_SHUTDOWN */
	N_("TX flow control"),					/**< MSG_DROP_FLOW_CONTROL */
	N_("Query text had no trailing NUL"),	/**< MSG_DROP_QUERY_NO_NUL */
	N_("Query text too short"),				/**< MSG_DROP_QUERY_TOO_SHORT */
	N_("Query had unnecessary overhead"),	/**< MSG_DROP_QUERY_OVERHEAD */
	N_("Message with malformed SHA1"),		/**< MSG_DROP_MALFORMED_SHA1 */
	N_("Message with malformed UTF-8"),		/**< MSG_DROP_MALFORMED_UTF_8 */
	N_("Malformed Query Hit"),				/**< MSG_DROP_BAD_RESULT */
	N_("Bad return address"),				/**< MSG_DROP_BAD_RETURN_ADDRESS */
	N_("Hostile IP address"),				/**< MSG_DROP_HOSTILE_IP */
	N_("Spam"),								/**< MSG_DROP_SPAM */
	N_("Evil filename"),					/**< MSG_DROP_EVIL */
};

/***
 *** Public functions
 ***/

void
gnet_stats_init(void)
{
    memset(stats_lut, MSG_UNKNOWN, sizeof(stats_lut));
    stats_lut[GTA_MSG_INIT] = MSG_INIT;
    stats_lut[GTA_MSG_INIT_RESPONSE]= MSG_INIT_RESPONSE;
    stats_lut[GTA_MSG_BYE] = MSG_BYE;
    stats_lut[GTA_MSG_QRP] = MSG_QRP;
    stats_lut[GTA_MSG_VENDOR] = MSG_VENDOR;
    stats_lut[GTA_MSG_STANDARD] = MSG_STANDARD;
    stats_lut[GTA_MSG_PUSH_REQUEST] = MSG_PUSH_REQUEST;
    stats_lut[GTA_MSG_SEARCH] = MSG_SEARCH;
    stats_lut[GTA_MSG_SEARCH_RESULTS] = MSG_SEARCH_RESULTS;
    stats_lut[GTA_MSG_HSEP_DATA] = MSG_HSEP;

    memset(&gnet_stats, 0, sizeof(gnet_stats));
    memset(&gnet_udp_stats, 0, sizeof(gnet_udp_stats));
}

/**
 * Called when Gnutella header has been read.
 */
void
gnet_stats_count_received_header(gnutella_node_t *n)
{
	guint t = stats_lut[n->header.function];
	guint i;
	gnet_stats_t *stats;

	stats = NODE_IS_UDP(n) ? &gnet_udp_stats : &gnet_tcp_stats;

    n->received++;

    gnet_stats.pkg.received[MSG_TOTAL]++;
    gnet_stats.pkg.received[t]++;
    gnet_stats.byte.received[MSG_TOTAL] += GTA_HEADER_SIZE;
    gnet_stats.byte.received[t] += GTA_HEADER_SIZE;

    stats->pkg.received[MSG_TOTAL]++;
    stats->pkg.received[t]++;
    stats->byte.received[MSG_TOTAL] += GTA_HEADER_SIZE;
    stats->byte.received[t] += GTA_HEADER_SIZE;

	i = MIN(n->header.ttl, STATS_RECV_COLUMNS-1);
    stats->pkg.received_ttl[i][MSG_TOTAL]++;
    stats->pkg.received_ttl[i][t]++;

	i = MIN(n->header.hops, STATS_RECV_COLUMNS-1);
    stats->pkg.received_hops[i][MSG_TOTAL]++;
    stats->pkg.received_hops[i][t]++;
}

/**
 * Called when Gnutella payload has been read.
 */
void
gnet_stats_count_received_payload(const gnutella_node_t *n)
{
    guint32 size = n->size;
	guint t = stats_lut[n->header.function];
	guint i;
	gnet_stats_t *stats;

	stats = NODE_IS_UDP(n) ? &gnet_udp_stats : &gnet_tcp_stats;

    gnet_stats.byte.received[MSG_TOTAL] += size;
    gnet_stats.byte.received[t] += size;

    stats->byte.received[MSG_TOTAL] += size;
    stats->byte.received[t] += size;

	i = MIN(n->header.ttl, STATS_RECV_COLUMNS-1);
    stats->byte.received_ttl[i][MSG_TOTAL] += size;
    stats->byte.received_ttl[i][t] += size;

	i = MIN(n->header.hops, STATS_RECV_COLUMNS-1);
    stats->byte.received_hops[i][MSG_TOTAL] += size;
    stats->byte.received_hops[i][t] += size;
}

void
gnet_stats_count_queued(const gnutella_node_t *n,
	guint8 type, guint8 hops, guint32 size)
{
	guint64 *stats_pkg;
	guint64 *stats_byte;
	guint t = stats_lut[type];
	gnet_stats_t *stats;

	g_assert(t != MSG_UNKNOWN);

	stats = NODE_IS_UDP(n) ? &gnet_udp_stats : &gnet_tcp_stats;

	stats_pkg = hops ? gnet_stats.pkg.queued : gnet_stats.pkg.gen_queued;
	stats_byte = hops ? gnet_stats.byte.queued : gnet_stats.byte.gen_queued;

    stats_pkg[MSG_TOTAL]++;
    stats_pkg[t]++;
    stats_byte[MSG_TOTAL] += size;
    stats_byte[t] += size;

	stats_pkg = hops ? stats->pkg.queued : stats->pkg.gen_queued;
	stats_byte = hops ? stats->byte.queued : stats->byte.gen_queued;

    stats_pkg[MSG_TOTAL]++;
    stats_pkg[t]++;
    stats_byte[MSG_TOTAL] += size;
    stats_byte[t] += size;
}

void
gnet_stats_count_sent(const gnutella_node_t *n,
	guint8 type, guint8 hops, guint32 size)
{
	guint64 *stats_pkg;
	guint64 *stats_byte;
	guint t = stats_lut[type];
	gnet_stats_t *stats;

	g_assert(t != MSG_UNKNOWN);

	stats = NODE_IS_UDP(n) ? &gnet_udp_stats : &gnet_tcp_stats;

	stats_pkg = hops ? gnet_stats.pkg.relayed : gnet_stats.pkg.generated;
	stats_byte = hops ? gnet_stats.byte.relayed : gnet_stats.byte.generated;

    stats_pkg[MSG_TOTAL]++;
    stats_pkg[t]++;
    stats_byte[MSG_TOTAL] += size;
    stats_byte[t] += size;

	stats_pkg = hops ? stats->pkg.relayed : stats->pkg.generated;
	stats_byte = hops ? stats->byte.relayed : stats->byte.generated;

    stats_pkg[MSG_TOTAL]++;
    stats_pkg[t]++;
    stats_byte[MSG_TOTAL] += size;
    stats_byte[t] += size;
}

void
gnet_stats_count_expired(const gnutella_node_t *n)
{
    guint32 size = n->size + sizeof(n->header);
	guint t = stats_lut[n->header.function];
	gnet_stats_t *stats;

	stats = NODE_IS_UDP(n) ? &gnet_udp_stats : &gnet_tcp_stats;

    gnet_stats.pkg.expired[MSG_TOTAL]++;
    gnet_stats.pkg.expired[t]++;
    gnet_stats.byte.expired[MSG_TOTAL] += size;
    gnet_stats.byte.expired[t] += size;

    stats->pkg.expired[MSG_TOTAL]++;
    stats->pkg.expired[t]++;
    stats->byte.expired[MSG_TOTAL] += size;
    stats->byte.expired[t] += size;
}

#define DROP_STATS(gs,t,s) do {							\
    if (												\
        (reason == MSG_DROP_ROUTE_LOST) ||				\
        (reason == MSG_DROP_DUPLICATE) ||				\
        (reason == MSG_DROP_NO_ROUTE)					\
    )													\
        gnet_stats.general[GNR_ROUTING_ERRORS]++;		\
														\
    gnet_stats.drop_reason[reason][MSG_TOTAL]++;		\
    gnet_stats.drop_reason[reason][t]++;				\
    gnet_stats.pkg.dropped[MSG_TOTAL]++;				\
    gnet_stats.pkg.dropped[t]++;						\
    gnet_stats.byte.dropped[MSG_TOTAL] += (s);			\
    gnet_stats.byte.dropped[t] += (s);					\
    gs->pkg.dropped[MSG_TOTAL]++;						\
    gs->pkg.dropped[t]++;								\
    gs->byte.dropped[MSG_TOTAL] += (s);					\
    gs->byte.dropped[t] += (s);							\
} while (0)

void
gnet_stats_count_dropped(gnutella_node_t *n, msg_drop_reason_t reason)
{
	guint32 size;
	guint type;
	gnet_stats_t *stats;

	g_assert((gint) reason >= 0 && reason < MSG_DROP_REASON_COUNT);

    size = n->size + sizeof(n->header);
	type = stats_lut[n->header.function];
	stats = NODE_IS_UDP(n) ? &gnet_udp_stats : &gnet_tcp_stats;

	DROP_STATS(stats, type, size);
	node_inc_rxdrop(n);

	switch (reason) {
	case MSG_DROP_HOSTILE_IP: n->n_hostile++; break;
	case MSG_DROP_SPAM: n->n_spam++; break;
	case MSG_DROP_EVIL: n->n_evil++; break;
	default: ;
	}

	if (dbg > 4)
		gmsg_log_dropped(&n->header, "from %s <%s>: %s",
			node_addr(n), node_vendor(n), msg_drop_reason[reason]);
}

void
gnet_stats_count_general(gnr_stats_t type, guint32 x)
{
	g_assert((gint) type >= 0 && type < GNR_TYPE_COUNT);

    gnet_stats.general[type] += x;
}

void
gnet_stats_count_dropped_nosize(
	const gnutella_node_t *n, msg_drop_reason_t reason)
{
	guint type;
	gnet_stats_t *stats;
	g_assert((gint) reason >= 0 && reason < MSG_DROP_REASON_COUNT);

	type = stats_lut[n->header.function];
	stats = NODE_IS_UDP(n) ? &gnet_udp_stats : &gnet_tcp_stats;

	/* Data part of message not read */
	DROP_STATS(stats, type, sizeof(n->header));

	if (dbg > 4)
		gmsg_log_dropped(&n->header, "from %s <%s>: %s",
			node_addr(n), node_vendor(n), msg_drop_reason[reason]);
}

void
gnet_stats_count_flowc(gconstpointer head)
{
    const struct gnutella_header *h = head;
	guint t;
	guint i;
	guint32 size;

	READ_GUINT32_LE(h->size, size);
#if 0
	g_message("FLOWC function=%d ttl=%d hops=%d", h->function, h->ttl, h->hops);
#endif

	t = stats_lut[h->function];

	i = MIN(h->hops, STATS_FLOWC_COLUMNS-1);
	gnet_stats.pkg.flowc_hops[i][t]++;
	gnet_stats.pkg.flowc_hops[i][MSG_TOTAL]++;
	gnet_stats.byte.flowc_hops[i][t] += size;
	gnet_stats.byte.flowc_hops[i][MSG_TOTAL] += size;

	i = MIN(h->ttl, STATS_FLOWC_COLUMNS-1);

	g_assert(i != 0);			/* Cannot send a message with TTL=0 */

	gnet_stats.pkg.flowc_ttl[i][t]++;
	gnet_stats.pkg.flowc_ttl[i][MSG_TOTAL]++;
	gnet_stats.byte.flowc_ttl[i][t] += size;
	gnet_stats.byte.flowc_ttl[i][MSG_TOTAL] += size;
}

/***
 *** Public functions (gnet.h)
 ***/

void
gnet_stats_get(gnet_stats_t *s)
{
    g_assert(s != NULL);
    *s = gnet_stats;
}

void
gnet_stats_tcp_get(gnet_stats_t *s)
{
    g_assert(s != NULL);
    *s = gnet_tcp_stats;
}

void
gnet_stats_udp_get(gnet_stats_t *s)
{
    g_assert(s != NULL);
    *s = gnet_udp_stats;
}

/* vi: set ts=4 sw=4 cindent: */
