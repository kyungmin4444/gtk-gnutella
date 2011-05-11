/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi, Richard Eckart
 * Copyright (c) 2011, Raphael Manfredi
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
 * Host cache management.
 *
 * @author Raphael Manfredi
 * @author Richard Eckart
 * @date 2002-2003
 *
 * @author Raphael Manfredi
 * @date 2011
 *
 * @todo
 * TODO:
 *
 *	- finer grained stats:
 *		-# hits/misses while adding,
 *		-# hits/misses while bad checking
 *		-# how many hosts were tried to connect to?
 *	- move unstable servant code from nodes.c to hcache.c
 *	- make sure hosts we are currently connected too are also saved
 *		to disk on exit!
 *	- save more metadata if we can make use of it.
 *
 */

#include "common.h"

RCSID("$Id$")

#include "bogons.h"
#include "hcache.h"
#include "hostiles.h"
#include "hosts.h"
#include "nodes.h"
#include "pcache.h"
#include "settings.h"
#include "ctl.h"

#include "lib/atoms.h"
#include "lib/ascii.h"
#include "lib/cq.h"
#include "lib/file.h"
#include "lib/getdate.h"
#include "lib/hashlist.h"
#include "lib/path.h"
#include "lib/random.h"
#include "lib/timestamp.h"
#include "lib/tm.h"
#include "lib/vmm.h"
#include "lib/walloc.h"
#include "lib/wq.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"

#include "lib/override.h"			/* Must be the last header included */

#define HOSTCACHE_EXPIRY (60 * 30) /* 30 minutes */

#define HCACHE_SAVE_PERIOD	63		/**< in seconds, every minute or so */			
#define MIN_RESERVE_SIZE	1024	/**< we'd like that many pongs in reserve */

/**
 * An entry within the hostcache.
 *
 * We don't really store the IP/port, as those are stored in the key of
 * hash table recording all known hosts.  Rather, we store "metadata" about
 * the host.
 */
typedef struct hostcache_entry {
    hcache_type_t type;				/**< Hostcache which contains this host */
    time_t        time_added;		/**< Time when entry was added */
} hostcache_entry_t;

/** No metadata for host */
static const void *no_metadata;
#define NO_METADATA			(no_metadata)

/**
 * A hostcache table.
 */
typedef struct hostcache {
	const char		*name;		        /**< Name of the cache */
	hcache_type_t   type;				/**< Cache type */
	hcache_class_t  class;				/**< Cache class */

    gboolean        addr_only;          /**< Use IP only, port always 0 */
    gboolean        dirty;            	/**< If updated since last disk flush */
    hash_list_t *   hostlist;           /**< Host list: IP/Port  */

    guint           hits;               /**< Hits to the cache */
    guint           misses;             /**< Misses to the cache */

    gnet_property_t hosts_in_catcher;   /**< Property to update host count */
    int            mass_update;        /**< If a mass update is in progess */
} hostcache_t;

static hostcache_t *caches[HCACHE_MAX];
static cperiodic_t *hcache_save_ev;
static cperiodic_t *hcache_timer_ev;
static gboolean hcache_close_running = FALSE;

static const char HOSTS_FILE[]	= "hosts";
static const char ULTRAS_FILE[]	= "ultras";
static const char GUESS_FILE[]	= "guess";

/**
 * Names of the host caches.
 *
 * @note
 * Has to be in the same order as in the hcache_type_t definition
 * in gnet_nodes.h.
 */
static const char * const names[HCACHE_MAX] = {
    "fresh regular",
    "valid regular",
    "fresh ultra",
    "valid ultra",
    "timeout",
    "busy",
    "unstable",
    "alien",
	"GUESS (running)",
	"GUESS (introductions)",
    "none",
};

static const char * const host_type_names[HOST_MAX] = {
    "any",
    "ultra",
	"guess"
};

enum {
    HCACHE_ALREADY_CONNECTED,
    HCACHE_INVALID_HOST,
    HCACHE_LOCAL_INSTANCE,
    HCACHE_STATS_MAX
};

static guint stats[HCACHE_STATS_MAX];

/**
 * Maps a host cache type to its corresponding class.
 */
static hcache_class_t
hcache_class(hcache_type_t type)
{
	switch (type) {
	case HCACHE_FRESH_ANY:
	case HCACHE_VALID_ANY:
	case HCACHE_FRESH_ULTRA:
	case HCACHE_VALID_ULTRA:
	case HCACHE_TIMEOUT:
	case HCACHE_BUSY:
	case HCACHE_UNSTABLE:
	case HCACHE_ALIEN:
		return HCACHE_CLASS_HOST;
	case HCACHE_GUESS:
	case HCACHE_GUESS_INTRO:
		return HCACHE_CLASS_GUESS;
	case HCACHE_NONE:
	case HCACHE_MAX:
		break;
	}
	g_assert_not_reached();
	return 0;
}

/**
 * Initiate mass update of host cache. While mass updates are in
 * progress, the hosts_in_catcher property will not be updated.
 */
static void
start_mass_update(hostcache_t *hc)
{
    hc->mass_update++;
}

/**
 * End mass update of host cache. If a hostcache has already been freed
 * when this function is called, it will not tigger a property update
 * for that hostcache and all of its group (ANY, ULTRA, BAD).
 */
static void
stop_mass_update(hostcache_t *hc)
{
    g_assert(hc->mass_update > 0);
    hc->mass_update--;

    if (hc->mass_update == 0) {
        switch (hc->type) {
        case HCACHE_FRESH_ANY:
        case HCACHE_VALID_ANY:
           	gnet_prop_set_guint32_val(hc->hosts_in_catcher,
				hcache_size(HOST_ANY));
            break;
        case HCACHE_FRESH_ULTRA:
        case HCACHE_VALID_ULTRA:
            gnet_prop_set_guint32_val(hc->hosts_in_catcher,
				hcache_size(HOST_ULTRA));
            break;
        case HCACHE_TIMEOUT:
        case HCACHE_UNSTABLE:
        case HCACHE_BUSY:
        case HCACHE_ALIEN:
            gnet_prop_set_guint32_val(hc->hosts_in_catcher,
                hash_list_length(caches[HCACHE_TIMEOUT]->hostlist) +
                hash_list_length(caches[HCACHE_UNSTABLE]->hostlist) +
                hash_list_length(caches[HCACHE_BUSY]->hostlist) +
                hash_list_length(caches[HCACHE_ALIEN]->hostlist));
            break;
        case HCACHE_GUESS:
        case HCACHE_GUESS_INTRO:
            gnet_prop_set_guint32_val(hc->hosts_in_catcher,
                hash_list_length(caches[hc->type]->hostlist));
			break;
		case HCACHE_NONE:
		case HCACHE_MAX:
            g_error("stop_mass_update: unknown cache type: %d", hc->type);
        }
    }
}

/**
 * Hashtable: IP/Port -> Metadata for HCACHE_CLASS_HOST.
 */
static GHashTable *ht_known_hosts;

/**
 * Hashtable: IP/Port -> Metadata for HCACHE_CLASS_GUESS.
 */
static GHashTable *ht_guess_hosts;

static GHashTable *
hcache_ht_by_class(hcache_class_t class)
{
	switch (class) {
	case HCACHE_CLASS_HOST:		return ht_known_hosts;
	case HCACHE_CLASS_GUESS:	return ht_guess_hosts;
	}
	g_assert_not_reached();
	return NULL;
}

static void
hcache_update_low_on_pongs(void)
{
    host_low_on_pongs = hcache_size(HOST_ANY) <
							(GNET_PROPERTY(max_hosts_cached) / 8);
}

/***
 *** Metadata allocation.
 ***/

static hostcache_entry_t *
hce_alloc(void)
{
	static const hostcache_entry_t zero_hce;
	hostcache_entry_t *hce;

	hce = walloc(sizeof *hce);
	*hce = zero_hce;
	return hce;
}

static void
hce_free(struct hostcache_entry *hce)
{
	g_assert(hce);
	g_assert(hce != NO_METADATA);

	wfree(hce, sizeof *hce);
}

/**
 * Output contents information about a hostcache.
 */
static void
hcache_dump_info(const struct hostcache *hc, const char *what)
{
    g_debug("[%s|%s] %u hosts (%u hits, %u misses)",
        hc->name, what, hash_list_length(hc->hostlist), hc->hits, hc->misses);
}

/***
 *** Hostcache access.
 ***/

/**
 * Get information about the host entry, both the host and the metadata.
 *
 * @param class	the class of host cache the host should be looked up in
 * @param addr	the address of the host
 * @param port	the port used by the host
 * @param h		filled with the host entry in the table
 * @param e		filled with the meta data of the host, as held in table
 *
 * @return FALSE if entry was not found in the cache.
 */
static gboolean
hcache_ht_get(hcache_class_t class, const host_addr_t addr, guint16 port,
	gnet_host_t **h, hostcache_entry_t **e)
{
	gnet_host_t host;
	gpointer k, v;
	gboolean found;
	GHashTable *ht;

	gnet_host_set(&host, addr, port);
	ht = hcache_ht_by_class(class);

	found = g_hash_table_lookup_extended(ht, &host, &k, &v);
	if (found) {
		*h = k;
		*e = v;
	}

	return found;
}

/**
 * Add host to the hash table host cache.
 *
 * Also creates a metadata struct unless the host was added to HL_CAUGHT
 * in which case we cannot know anything about the host. Yet we cannot
 * assert that HL_CAUGHT never contains a host with metadata because when
 * HL_CAUGHT becomes empty, move all hosts from HL_VALID to HL_CAUGHT. We
 * can however assert that any host which does not have metadata is in
 * HL_CAUGHT.
 *
 * @return Pointer to metadata struct for the added host or NO_METADATA
 *         if no metadata was added.
 */
static hostcache_entry_t *
hcache_ht_add(hcache_type_t type, const gnet_host_t *host)
{
    hostcache_entry_t *hce;
	GHashTable *ht;

    hce = hce_alloc();
    hce->type = type;
    hce->time_added = tm_time();

	ht = hcache_ht_by_class(hcache_class(type));
	gm_hash_table_insert_const(ht, host, hce);

    return hce;
}

/**
 * Remove host from the hash table host cache.
 */
static void
hcache_ht_remove(hcache_class_t class, gnet_host_t *host)
{
	hostcache_entry_t *hce;
	gpointer key, value;
	GHashTable *ht;

	ht = hcache_ht_by_class(class);

	if (!g_hash_table_lookup_extended(ht, host, &key, &value)) {
		g_carp("hcache_ht_remove: attempt to remove unknown host: %s",
			  gnet_host_to_string(host));
		return;
	}
	hce = value;
	g_hash_table_remove(ht, host);

	if (hce != NO_METADATA)
		hce_free(hce);
}

/**
 * Get metadata for host.
 *
 * @return NULL if host was not found, NO_METADATA if no metadata was stored
 *         or a pointer to a hostcache_entry struct which holds the metadata.
 */
static hostcache_entry_t *
hcache_get_metadata(hcache_class_t class, const gnet_host_t *host)
{
	GHashTable *ht = hcache_ht_by_class(class);

    return g_hash_table_lookup(ht, host);
}

/**
 * @return TRUE if the host is in one of the "bad hosts" caches.
 */
gboolean
hcache_node_is_bad(const host_addr_t addr)
{
    hostcache_entry_t *hce;
    gnet_host_t h;

	/*
	 * When we're low on pongs, we cannot afford the luxury of discarding
	 * any IP address, or we'll end up contacting web caches for more.
	 */

	if (host_low_on_pongs)
		return FALSE;

	gnet_host_set(&h, addr, 0);
    hce = hcache_get_metadata(HCACHE_CLASS_HOST, &h);

    if (hce == NULL || hce == NO_METADATA)
        return FALSE;

    caches[hce->type]->hits++;

    switch (hce->type) {
    case HCACHE_FRESH_ANY:
    case HCACHE_VALID_ANY:
    case HCACHE_FRESH_ULTRA:
    case HCACHE_VALID_ULTRA:
        return FALSE;
    default:
        return TRUE;
    }
}

/**
 * Move entries from one hostcache to another. This only works if the
 * target hostcache is empty.
 */
static void
hcache_move_entries(hostcache_t *to, hostcache_t *from)
{
	hash_list_iter_t *iter;
	gpointer item;

	g_assert(to != NULL);
	g_assert(from != NULL);
    g_assert(hash_list_length(to->hostlist) == 0);
	g_assert(to->class == from->class);

	hash_list_free(&to->hostlist);
    to->hostlist = from->hostlist;
    from->hostlist = hash_list_new(NULL, NULL);

    /*
     * Make sure that after switching hce->list points to the new
     * list HL_CAUGHT
     */

	iter = hash_list_iterator(to->hostlist);

	while (NULL != (item = hash_list_iter_next(iter))) {
        hostcache_entry_t *hce;

        hce = hcache_get_metadata(from->class, item);
        if (hce == NULL || hce == NO_METADATA)
            continue;
        hce->type = to->type;
    }

	hash_list_iter_release(&iter);
}

/**
 * Make sure we have some host available in HCACHE_FRESH_ANY
 * and HCACHE_FRESH_ULTRA.
 *
 * If one of the both is empty, all hosts from HCACHE_VALID_XXX
 * are moved to HCACHE_VALID_XXX. When caught on other hcaches than
 * FRESH_ANY and FRESH_ULTRA nothing happens.
 *
 * @return TRUE if host are available in hc after the call.
 */
static gboolean
hcache_require_caught(hostcache_t *hc)
{
    g_assert(NULL != hc);

    switch (hc->type) {
    case HCACHE_FRESH_ANY:
    case HCACHE_VALID_ANY:
        if (hash_list_length(caches[hc->type]->hostlist) == 0) {
            hcache_move_entries(caches[hc->type], caches[HCACHE_VALID_ANY]);
        }
        return hash_list_length(caches[hc->type]->hostlist) != 0;
    case HCACHE_FRESH_ULTRA:
    case HCACHE_VALID_ULTRA:
        if (hash_list_length(caches[hc->type]->hostlist) == 0) {
            hcache_move_entries(caches[hc->type], caches[HCACHE_VALID_ULTRA]);
        }
        return hash_list_length(caches[hc->type]->hostlist) != 0;
    default:
        return hash_list_length(hc->hostlist) != 0;
    }
}

/**
 * Remove host from a hostcache.
 */
static void
hcache_unlink_host(hostcache_t *hc, gnet_host_t *host)
{
	gconstpointer orig_key;
	
	g_assert(hc->hostlist != NULL);
	g_assert(hash_list_length(hc->hostlist) > 0);

	orig_key = hash_list_remove(hc->hostlist, host);
	g_assert(orig_key);

    if (hc->mass_update == 0) {
        guint32 cur;
        gnet_prop_get_guint32_val(hc->hosts_in_catcher, &cur);
        gnet_prop_set_guint32_val(hc->hosts_in_catcher, cur - 1);
    }

	hc->dirty = TRUE;
	hcache_ht_remove(hc->class, host);
	atom_host_free(host);

	if (!hcache_close_running) {
		/* This must not be called during a close sequence as it
		 * would refill some caches and cause an assertion failure */
    	hcache_require_caught(hc);
	}
}

/**
 * Convert host cache type to string.
 */
const char *
hcache_type_to_string(hcache_type_t type)
{
	g_assert((guint) type < HCACHE_MAX);

	return names[type];
}

/**
 * Convert host type to string.
 */
const char *
host_type_to_string(host_type_t type)
{
	g_assert((guint) type < HOST_MAX);

	return host_type_names[type];
}

static int
hcache_slots_max(hcache_type_t type)
{
	g_assert(UNSIGNED(type) < HCACHE_MAX);

    switch (type) {
    case HCACHE_FRESH_ANY:
    case HCACHE_VALID_ANY:
        return GNET_PROPERTY(max_hosts_cached);
    case HCACHE_FRESH_ULTRA:
    case HCACHE_VALID_ULTRA:
        return GNET_PROPERTY(max_ultra_hosts_cached);
	case HCACHE_BUSY:
	case HCACHE_TIMEOUT:
	case HCACHE_UNSTABLE:
	case HCACHE_ALIEN:
		return GNET_PROPERTY(max_bad_hosts_cached);
	case HCACHE_GUESS:
		return GNET_PROPERTY(max_guess_hosts_cached);
	case HCACHE_GUESS_INTRO:
		return GNET_PROPERTY(max_guess_intro_hosts_cached);
	case HCACHE_NONE:
	case HCACHE_MAX:
		break;
    }
	g_assert_not_reached();
	return 0;
}

/**
 * @return the number of slots which can be added to the given type.
 *
 * @note
 * Several types share common pools. Adding a host of one type may
 * affect the number of slots left on other types.
 */
static int
hcache_slots_left(hcache_type_t type)
{
	int limit, current = 0;

    g_assert(UNSIGNED(type) < HCACHE_MAX);

	limit = hcache_slots_max(type);
    switch (type) {
    case HCACHE_FRESH_ANY:
    case HCACHE_VALID_ANY:
		current = hcache_size(HOST_ANY);
		break;
    case HCACHE_FRESH_ULTRA:
    case HCACHE_VALID_ULTRA:
        current = hcache_size(HOST_ULTRA);
		break;
	case HCACHE_BUSY:
	case HCACHE_TIMEOUT:
	case HCACHE_UNSTABLE:
	case HCACHE_ALIEN:
	case HCACHE_GUESS:
	case HCACHE_GUESS_INTRO:
		current = hash_list_length(caches[type]->hostlist);
		break;
	case HCACHE_NONE:
	case HCACHE_MAX:
		g_assert_not_reached();
    }
	return limit - current;
}

/**
 * Check whether a slot is available and use a simple probability filter to
 * prevent that the lists can be easily flooded with potentially unwanted
 * items.
 *
 * @return TRUE whether there is an available slot which should be used.
 */
static gboolean
hcache_request_slot(hcache_type_t type)
{
	guint limit, left;

	limit = hcache_slots_max(type);
	left = hcache_slots_left(type);

	return limit > 0
		&& left > 0
		&& ((left > limit / 2) || (random_u32() % limit < left));
}

/**
 * Register a host.
 *
 * If a host is already on the known hosts hashtable, it will not be
 * registered. Otherwise it will be added to the hashtable of known hosts
 * and added to one of the host lists as indicated by the "list" parameter.
 * Sanity checks are only applied when the host is added to HL_CAUGHT, since
 * when a host is added to any of the other lists it must have been in
 * HL_CAUGHT or in the pong-cache before.
 *
 * When the host passes validation checks and should be added to the cache,
 * anyone waiting via wq_sleep() on the "hcache_add" key is notified about
 * the new host, the wakeup information supplying a hcache_new_host structure.
 * 
 * @return TRUE when IP/port passed sanity checks, regardless of whether it
 *         was added to the cache. (See above)
 */
static gboolean
hcache_add_internal(hcache_type_t type, time_t added,
	const host_addr_t addr, guint16 port, const char *what)
{
	gnet_host_t *host;
	const gnet_host_t *host_atom;
	hostcache_t *hc;
	hostcache_entry_t *hce;

	g_assert(UNSIGNED(type) < HCACHE_MAX);
	g_assert(type != HCACHE_NONE);

	if (GNET_PROPERTY(stop_host_get))
		return FALSE;

	/*
	 * Don't add anything to the "unstable" cache if they don't want to
	 * monitor unstable servents or when we're low on pongs (thereby
	 * automatically disabling this monitoring).  The aim is to avoid
	 * the host discarding the last few IP addresses it has, forcing it
	 * to contact the web caches...
	 */

    if (type == HCACHE_UNSTABLE) {
    	if (!GNET_PROPERTY(node_monitor_unstable_ip) || host_low_on_pongs)
			return FALSE;
	}

	if (is_my_address_and_port(addr, port)) {
        stats[HCACHE_LOCAL_INSTANCE]++;
		return FALSE;
    }

	switch (type) {
	case HCACHE_FRESH_ANY:
	case HCACHE_FRESH_ULTRA:
	case HCACHE_VALID_ANY:
	case HCACHE_VALID_ULTRA:
		if (node_host_is_connected(addr, port)) {
			stats[HCACHE_ALREADY_CONNECTED]++;
			return FALSE;			/* Connected to that host? */
		}
		break;
	default:
		break;
	}

	hc = caches[type];
    g_assert(hc->type == type);

    if (
		!host_addr_is_routable(addr) &&
		(!hc->addr_only || !port_is_valid(port))
	) {
        stats[HCACHE_INVALID_HOST]++;
		return FALSE;			/* Is host valid? */
    }

    if (bogons_check(addr) || hostiles_check(addr)) {
        stats[HCACHE_INVALID_HOST]++;
		return FALSE;			/* Is host valid? */
    }

	/*
	 * Keep the amount of peers with ports around 6346 low because
	 * these are often used by outdated and deprecated software
	 * and the ports are frequently blocked or throttled.
	 */
	if (
		port >= 6346 &&
		port <= 6350 &&
		!host_low_on_pongs &&
		(random_u32() & 0xff) > 31
	) {
		return FALSE;		/* Did not pass port sanity checks */
	}

	/*
	 * If host is already known, check whether we could not simply move the
	 * entry from one cache to another.
	 */

	if (hcache_ht_get(hcache_class(type), addr, port, &host, &hce)) {
		gconstpointer orig_key;

        g_assert(hce != NULL);

        hc->hits++;

		switch (type) {
		case HCACHE_TIMEOUT:
		case HCACHE_BUSY:
		case HCACHE_UNSTABLE:
		case HCACHE_ALIEN:
			/*
			 * Move host to the proper cache, if not already in one of the
			 * "bad" caches.
			 */

			switch (hce->type) {
			case HCACHE_TIMEOUT:
			case HCACHE_BUSY:
			case HCACHE_UNSTABLE:
			case HCACHE_ALIEN:
				return TRUE;
			default:
				break;				/* Move it */
			}
			break;

		case HCACHE_VALID_ULTRA:
		case HCACHE_FRESH_ULTRA:
			/*
			 * Move the host to the "ultra" cache if it's in the "any" ones.
			 */

			switch (hce->type) {
			case HCACHE_VALID_ANY:
			case HCACHE_FRESH_ANY:
				break;				/* Move it */
			default:
				return TRUE;
			}
			break;

		case HCACHE_GUESS:
		case HCACHE_GUESS_INTRO:
			/*
			 * ID Smearing Alogrithm to limit pong cache poisonning:
			 * When attempting to add a host already present, we actually
			 * remove the host from the cache!
			 *
			 * See research article "Pong-Cache Poisoning in GUESS".
			 */

			if (GNET_PROPERTY(guess_server_debug) > 2) {
				g_debug("HCACHE smearing %s out of %s",
					gnet_host_to_string(host),
					hcache_type_to_string(hce->type));
			}

			hcache_unlink_host(caches[hce->type], host);
			return TRUE;

		default:
			return TRUE;
		}

		/*
		 * OK, we can move it from the `hce->type' cache to the `type' one.
		 */

		orig_key = hash_list_remove(caches[hce->type]->hostlist, host);
		g_assert(orig_key);

		hash_list_prepend(hc->hostlist, host);
		caches[hce->type]->dirty = hc->dirty = TRUE;

		hce->type = type;
		hce->time_added = added;

		return TRUE;
    }

	host = NULL;		/* Safety */

	/*
	 * If someone is waiting on a new host via wq_sleep(), notify them.
	 */

	{
		struct hcache_new_host nhost;

		nhost.type = type;
		nhost.addr = addr;
		nhost.port = port;
		wq_wakeup(cast_func_to_pointer((func_ptr_t) hcache_add), &nhost);
	}

	if (!hcache_request_slot(hc->type))
		return TRUE;

	/* Okay, we got a new host */

	{
		gnet_host_t packed;
		gnet_host_set(&packed, addr, port);
		host_atom = atom_host_get(&packed);
	}

	hcache_ht_add(type, host_atom);

    switch (type) {
    case HCACHE_FRESH_ANY:
    case HCACHE_FRESH_ULTRA:
		/*
		 * Prepend, so that we use the freshest entries.
		 */
        hash_list_prepend(hc->hostlist, host_atom);
        break;

    case HCACHE_VALID_ANY:
    case HCACHE_VALID_ULTRA:
        /*
         * We prepend to the list instead of appending because the day
         * we switch it as HCACHE_FRESH_XXX, we'll start reading from there,
         * in effect using the most recent hosts we know about.
         */
        hash_list_prepend(hc->hostlist, host_atom);
        break;

	case HCACHE_GUESS:
	case HCACHE_GUESS_INTRO:
        hash_list_prepend(hc->hostlist, host_atom);
        break;

    default:
        /*
         * hcache_expire() depends on the fact that new entries are
         * added to the beginning of the list
         */
        hash_list_prepend(hc->hostlist, host_atom);
        break;
    }

    hc->misses++;
	hc->dirty = TRUE;

    if (hc->mass_update == 0) {
        guint32 cur;
        gnet_prop_get_guint32_val(hc->hosts_in_catcher, &cur);
        gnet_prop_set_guint32_val(hc->hosts_in_catcher, cur + 1);
    }

    hcache_prune(hc->type);
    hcache_update_low_on_pongs();

    if (GNET_PROPERTY(hcache_debug) > 8) {
        g_debug("HCACHE added %s %s (%s)",
			what, gnet_host_to_string(host_atom),
            (type == HCACHE_FRESH_ANY || type == HCACHE_VALID_ANY) ?
                (host_low_on_pongs ? "LOW" : "OK") : "");
    }

	return TRUE;
}

/**
 * Add host to the proper cache.
 */
gboolean
hcache_add(hcache_type_t type,
	const host_addr_t addr, guint16 port, const char *what)
{
	return hcache_add_internal(type, tm_time(), addr, port, what);
}

/**
 * Add a caught (fresh) host to the right list depending on the host type.
 */
gboolean
hcache_add_caught(host_type_t type, const host_addr_t addr, guint16 port,
	const char *what)
{
	if (ctl_limit(addr, CTL_D_CACHE))
		return FALSE;

    switch (type) {
    case HOST_ANY:
    	return hcache_add(HCACHE_FRESH_ANY, addr, port, what);
    case HOST_ULTRA:
    	return hcache_add(HCACHE_FRESH_ULTRA, addr, port, what);
    case HOST_GUESS:
    	return hcache_add(HCACHE_GUESS, addr, port, what);
    case HOST_MAX:
		g_assert_not_reached();
    }

    g_error("hcache_add_caught: unknown host type: %d", type);
    return FALSE;
}

/**
 * Add a valid host to the right list depending on the host type.
 */
gboolean
hcache_add_valid(host_type_t type, const host_addr_t addr, guint16 port,
	const char *what)
{
	if (ctl_limit(addr, CTL_D_CACHE))
		return FALSE;

    switch (type) {
    case HOST_ANY:
    	return hcache_add(HCACHE_VALID_ANY, addr, port, what);
    case HOST_ULTRA:
    	return hcache_add(HCACHE_VALID_ULTRA, addr, port, what);
    case HOST_GUESS:
    	return hcache_add(HCACHE_GUESS_INTRO, addr, port, what);
    case HOST_MAX:
		g_assert_not_reached();
    }

    g_error("hcache_add_valid: unknown host type: %d", type);
    return FALSE;
}

/**
 * Remove host from cache.
 *
 * After removing hcache_require_caught is called.
 */
static void
hcache_remove(hcache_class_t class, gnet_host_t *h)
{
    hostcache_entry_t *hce;
    hostcache_t *hc;

    hce = hcache_get_metadata(class, h);
    if (hce == NULL) {
		g_warning("hcache_remove: attempt to remove unknown host: %s",
			gnet_host_to_string(h));
        return; /* Host is not in hashtable */
    }

    hc = caches[hce->type];

    hcache_unlink_host(hc, h);
}

/**
 * Purge host from fresh/valid caches.
 */
void
hcache_purge(const host_addr_t addr, guint16 port)
{
	hostcache_entry_t *hce;
	gnet_host_t *host;

	if (hcache_ht_get(HCACHE_CLASS_HOST, addr, port, &host, &hce)) {
		switch (hce->type) {
		case HCACHE_FRESH_ANY:
		case HCACHE_VALID_ANY:
		case HCACHE_FRESH_ULTRA:
		case HCACHE_VALID_ULTRA:
			hcache_remove(HCACHE_CLASS_HOST, host);
			return;
		default:
			break;
		}
	}
}

/**
 * Do we have less that our mimumum amount of hosts in the cache?
 */
gboolean
hcache_is_low(host_type_t type)
{
	return hcache_size(type) < MIN_RESERVE_SIZE;
}

/**
 * Remove all entries from hostcache.
 */
static void
hcache_remove_all(hostcache_t *hc)
{
	gnet_host_t *h;

    if (hash_list_length(hc->hostlist) == 0)
        return;

    /* FIXME: may be possible to do this faster */

    start_mass_update(hc);

    while (NULL != (h = hash_list_head(hc->hostlist)))
        hcache_remove(hc->class, h);

    g_assert(hash_list_length(hc->hostlist) == 0);

    stop_mass_update(hc);
    g_assert(hash_list_length(hc->hostlist) == 0);
}

/**
 * Clear the whole host cache for a host type and the pong cache of
 * the same type. Use this to clear the "ultra" and "any" host caches.
 */
void
hcache_clear_host_type(host_type_t type)
{
	gboolean valid = FALSE;

    switch (type) {
    case HOST_ANY:
        hcache_remove_all(caches[HCACHE_FRESH_ANY]);
        hcache_remove_all(caches[HCACHE_VALID_ANY]);
		valid = TRUE;
        break;
    case HOST_ULTRA:
        hcache_remove_all(caches[HCACHE_FRESH_ULTRA]);
        hcache_remove_all(caches[HCACHE_VALID_ULTRA]);
		valid = TRUE;
        break;
    case HOST_GUESS:
        hcache_remove_all(caches[HCACHE_GUESS]);
        hcache_remove_all(caches[HCACHE_GUESS_INTRO]);
		valid = TRUE;
		break;
    case HOST_MAX:
		g_assert_not_reached();
    }

	if (!valid)
        g_error("hcache_clear_host_type: unknown host type: %d", type);

	pcache_clear_recent(type);
}

/**
 * Clear the whole host cache but does not clear the pong caches. Use
 * this to clear the "bad" host caches.
 */
void
hcache_clear(hcache_type_t type)
{
    g_assert((guint) type < HCACHE_MAX);

    hcache_remove_all(caches[type]);
}

/**
 * @return the amount of hosts in the cache.
 */
guint
hcache_size(host_type_t type)
{
    switch (type) {
    case HOST_ANY:
        return hash_list_length(caches[HCACHE_FRESH_ANY]->hostlist) +
            hash_list_length(caches[HCACHE_VALID_ANY]->hostlist);
    case HOST_ULTRA:
        return hash_list_length(caches[HCACHE_FRESH_ULTRA]->hostlist) +
            hash_list_length(caches[HCACHE_VALID_ULTRA]->hostlist);
    case HOST_GUESS:
        return hash_list_length(caches[HCACHE_GUESS]->hostlist) +
        	hash_list_length(caches[HCACHE_GUESS_INTRO]->hostlist);
    case HOST_MAX:
		g_assert_not_reached();
    }
    g_error("hcache_is_low: unknown host type: %d", type);
    return -1; /* Only here to make -Wall happy */
}

/**
 * Expire hosts from a single hostlist in a hostcache. Also removes
 * it from the host hashtable.
 *
 * @return total number of expired entries
 */
static guint32
hcache_expire_cache(hostcache_t *hc, time_t now)
{
    guint32 expire_count = 0;
	gnet_host_t *h;

    /*
	 * Prune all the expired ones from the list until the list is empty
     * or we find one which is not expired, in which case we know that
     * all the following are also not expired, because the list is
     * sorted by time_added
	 */

    while (NULL != (h = hash_list_tail(hc->hostlist))) {
        hostcache_entry_t *hce = hcache_get_metadata(hc->class, h);

        g_assert(hce != NULL);

        if (delta_time(now, hce->time_added) > HOSTCACHE_EXPIRY) {
            hcache_remove(hc->class, h);
            expire_count++;
        } else {
            /* Found one which has not expired. Stopping */
            break;
        }
    }

    return expire_count;
}


/**
 * Expire hosts from the HL_BUSY, HL_TIMEOUT and HL_UNSTABLE lists
 * and remove them from the hosts hashtable too.
 *
 * @return total number of expired entries
 */
static guint32
hcache_expire_all(time_t now)
{
    guint32 expire_count = 0;

    expire_count += hcache_expire_cache(caches[HCACHE_TIMEOUT], now);
    expire_count += hcache_expire_cache(caches[HCACHE_BUSY], now);
    expire_count += hcache_expire_cache(caches[HCACHE_UNSTABLE], now);

    return expire_count;
}

/**
 * Remove hosts that exceed our maximum.
 *
 * This can be called on HCACHE_FRESH_ANY and on HCACHE_FRESH_ULTRA.
 *
 * If too many hosts are in the cache, then it will prune the HCACHE_FRESH_XXX
 * list. Only after HCACHE_FRESH_XXX is empty HCACHE_VALID_XXX will be moved
 * to HCACHE_FRESH_XXX and then it is purged.
 */
void
hcache_prune(hcache_type_t type)
{
	hostcache_t *hc;
    int extra;

    g_assert((guint) type < HCACHE_MAX);

	hc = caches[type];

#define HALF_PRUNE(x) G_STMT_START {		\
	if (hash_list_length(hc->hostlist) < \
			hash_list_length(caches[x]->hostlist)) \
		hc = caches[x];			\
} G_STMT_END

    switch (type) {
    case HCACHE_VALID_ANY:
		HALF_PRUNE(HCACHE_FRESH_ANY);
        break;
    case HCACHE_VALID_ULTRA:
		HALF_PRUNE(HCACHE_FRESH_ULTRA);
        break;
    case HCACHE_FRESH_ANY:
		HALF_PRUNE(HCACHE_VALID_ANY);
		break;
    case HCACHE_FRESH_ULTRA:
		HALF_PRUNE(HCACHE_VALID_ULTRA);
		break;
    default:
        break;
    }

#undef HALF_PRUNE

    extra = hcache_slots_left(hc->type);
    if (extra >= 0)
        return;

    start_mass_update(hc);

    hcache_require_caught(hc);
	while (extra++ < 0) {
		gnet_host_t *h;

		/*
		 * The GUESS running cache is managed in an MRU fashion to limit the
		 * impact of possible poisoning of the cache by malicious hosts.
		 * See the "Pong-Cache Poisoning in GUESS" article.
		 *
		 * However, in practice this does not work as desired because only
		 * the last slot ends up being updated and all the other hosts quickly
		 * become stale.  Hence use probability replacement: there is a 70%
		 * chance that the most recent entry will be replaced, otherewise we'll
		 * use normal LRU replacement.
		 */

		if (HCACHE_GUESS == hc->type && random_u32() % 100 < 70) {
			/*
			 * The newest entry is the one we just added to the cache.
			 * Remove the next one, unless there is none and pruning means
			 * that we have to remove the only entry we have!
			 */

			h = hash_list_head(hc->hostlist);	/* Newest entry */
			h = hash_list_next(hc->hostlist, h);
			if (NULL == h)
				h = hash_list_head(hc->hostlist);
		} else {
			h = hash_list_tail(hc->hostlist);	/* Oldest entry */
		}

		if (NULL == h) {
			g_carp("BUG: asked to remove hosts, "
                "but hostcache list is empty: %s", hc->name);
			break;
		}
		hcache_remove(hc->class, h);
	}

    stop_mass_update(hc);
}

/**
 * Fill `hosts', an array of `hcount' hosts already allocated with at most
 * `hcount' hosts from out caught list, without removing those hosts from
 * the list.
 *
 * @return amount of hosts filled
 */
int
hcache_fill_caught_array(host_type_t type, gnet_host_t *hosts, int hcount)
{
	int i;
	hostcache_t *hc = NULL;
	GHashTable *seen_host = g_hash_table_new(gnet_host_hash, gnet_host_eq);
	hash_list_iter_t *iter;

    switch (type) {
    case HOST_ANY:
        hc = caches[HCACHE_FRESH_ANY];
        break;
    case HOST_ULTRA:
        hc = caches[HCACHE_FRESH_ULTRA];
        break;
    case HOST_GUESS:
        hc = caches[HCACHE_GUESS];
		if (0 == hash_list_length(hc->hostlist))
			hc = caches[HCACHE_GUESS_INTRO];
        break;
    case HOST_MAX:
		g_assert_not_reached();
    }

	if (!hc)
        g_error("hcache_get_caught: unknown host type: %d", type);

	/*
	 * Not enough fresh pongs, get some from our reserve.
	 */

	iter = hash_list_iterator(hc->hostlist);
	for (i = 0; i < hcount; i++) {
		gnet_host_t *h;

		h = hash_list_iter_next(iter);
		if (NULL == h)
			break;

		if (g_hash_table_lookup(seen_host, h))
			continue;

		hosts[i] = *h;			/* struct copy */

		g_hash_table_insert(seen_host, &hosts[i], GUINT_TO_POINTER(1));
	}
	hash_list_iter_release(&iter);
	gm_hash_table_destroy_null(&seen_host);	/* Keys point into vector */

	return i;				/* Amount of hosts we filled */
}

/**
 * Finds a host in either the pong_cache or the host_cache that is in
 * one of the local networks.
 *
 * @return TRUE if host is found
 */
gboolean
hcache_find_nearby(host_type_t type, host_addr_t *addr, guint16 *port)
{
	gnet_host_t *h;
	hostcache_t *hc = NULL;
	hash_list_iter_t *iter;

    switch (type) {
    case HOST_ANY:
        hc = caches[HCACHE_FRESH_ANY];
		break;
    case HOST_ULTRA:
        hc = caches[HCACHE_FRESH_ULTRA];
		break;
    case HOST_GUESS:
        hc = caches[HCACHE_GUESS];
		break;
    case HOST_MAX:
		g_assert_not_reached();
    }

	if (!hc)
        g_error("hcache_get_caught: unknown host type: %d", type);

	/* iterate through whole list */

	iter = hash_list_iterator(hc->hostlist);
	while (NULL != (h = hash_list_iter_next(iter))) {
		if (host_is_nearby(gnet_host_get_addr(h))) {
            *addr = gnet_host_get_addr(h);
            *port = gnet_host_get_port(h);
			break;
		}
	}
	hash_list_iter_release(&iter);

	if (h) {
		hcache_unlink_host(hc, h);
		return TRUE;
	}
	return FALSE;
}

/**
 * Sorting callback, by decreading added time.
 */
static int
hcache_cmp_added_time(const void *a, const void *b, void *data)
{
	const gnet_host_t *ha = a;
	const gnet_host_t *hb = b;
	const hostcache_entry_t *hce_a;
	const hostcache_entry_t *hce_b;
	hcache_class_t class = pointer_to_int(data);

	hce_a = hcache_get_metadata(class, ha);
	hce_b = hcache_get_metadata(class, hb);

	if (hce_a == NULL) {
		return hce_b == NULL ? 0 : +1;	/* Put b first */
	} else if (hce_b == NULL) {
		return -1;						/* Put a first */
	} else {
		/* Put entry with highest time first */
		return CMP(hce_b->time_added, hce_a->time_added);
	}
}

/**
 * Sort cache by reverse added time, putting oldest entries at the tail.
 */
static void
hcache_sort_by_added_time(hcache_type_t type)
{
	hostcache_t *hc = caches[type];

	if (GNET_PROPERTY(hcache_debug))
		g_debug("HCACHE sorting %s cache", hcache_type_to_string(type));

	hash_list_sort_with_data(hc->hostlist, hcache_cmp_added_time,
		int_to_pointer(hc->class));

	if (GNET_PROPERTY(hcache_debug)) {
		unsigned count = hash_list_length(hc->hostlist);
		g_debug("HCACHE sorted %s cache (%u item%s)",
			hcache_type_to_string(type), count, 1 == count ? "" : "s");
	}
}

/**
 * Get host IP/port information from our caught host list.
 *
 * @return TRUE on sucess, FALSE on failure.
 */
gboolean
hcache_get_caught(host_type_t type, host_addr_t *addr, guint16 *port)
{
	hostcache_t *hc = NULL;
	extern guint32 number_local_networks;
	gnet_host_t *h;
	gboolean available;

	g_assert(addr);
	g_assert(port);

	*addr = zero_host_addr;
	*port = 0;
	
    switch (type) {
    case HOST_ANY:
        hc = caches[HCACHE_FRESH_ANY];
        break;
    case HOST_ULTRA:
        hc = caches[HCACHE_FRESH_ULTRA];
        break;
    case HOST_GUESS:
        hc = caches[HCACHE_GUESS];
		if (0 == hash_list_length(hc->hostlist))
			hc = caches[HCACHE_GUESS_INTRO];
        break;
    case HOST_MAX:
		g_assert_not_reached();
    }

	if (!hc)
        g_error("hcache_get_caught: unknown host type: %d", type);

    available = hcache_require_caught(hc);

    hcache_update_low_on_pongs();

	if (!available)
		return FALSE;

	/*
	 * First, try to find a local host (for non-GUESS hosts only).
	 */

	if (
		GNET_PROPERTY(use_netmasks) &&
		number_local_networks &&
		HOST_GUESS != type &&
		hcache_find_nearby(type, addr, port)
	)
		return TRUE;

	h = hash_list_head(hc->hostlist);
	if (h) {
		*addr = gnet_host_get_addr(h);
		*port = gnet_host_get_port(h);
		hcache_unlink_host(hc, h);
		return TRUE;
	}

	return FALSE;
}

/***
 *** Hostcache management.
 ***/

/**
 * Allocate hostcache of type `type'.
 *
 * The `incache' property is what needs to be updated so that the GUI can
 * display the proper amount of hosts we currently hold.
 *
 * The `maxhosts' variable is the pointer to the variable giving the maximum
 * amount of hosts we can store.
 */
static hostcache_t *
hcache_alloc(hcache_type_t type, gnet_property_t catcher, const char *name)
{
	struct hostcache *hc;

	g_assert((guint) type < HCACHE_MAX);

	hc = g_malloc0(sizeof *hc);

	hc->hostlist = hash_list_new(NULL, NULL);
	hc->name = name;
	hc->type = type;
	hc->class = hcache_class(type);
    hc->hosts_in_catcher = catcher;
    hc->addr_only = FALSE;

	return hc;
}

/**
 * Dispose of the hostcache.
 */
static void
hcache_free(hostcache_t *hc)
{
    g_assert(hc != NULL);
    g_assert(hash_list_length(hc->hostlist) == 0);

	hash_list_free(&hc->hostlist);
	G_FREE_NULL(hc);
}

/**
 * Parse and load the hostcache file.
 */
static void
hcache_load_file(hostcache_t *hc, FILE *f)
{
	char buffer[1024];
	time_t now;

	g_return_if_fail(hc);
	g_return_if_fail(f);

	now = tm_time();
	while (fgets(buffer, sizeof buffer, f)) {
		const char *endptr;
		host_addr_t addr;
		guint16 port;
		time_t added;

		if (!string_to_host_addr_port(buffer, &endptr, &addr, &port))
			continue;

		endptr = skip_ascii_spaces(endptr);
		added = date2time(endptr, now);

		/* NOTE: hcache_expire_cache() stops on the first item which has
		 *		 not yet expired.
		 */
		if (
			(time_t)-1 == added ||
			delta_time(now, added) < 0 ||
			delta_time(now, added) > HOSTCACHE_EXPIRY
		) {
			added = now - HOSTCACHE_EXPIRY;
		}

		hcache_add_internal(hc->type, added, addr, port, "on-disk cache");
		if (hcache_slots_left(hc->type) < 1)
			break;
	}

	hcache_sort_by_added_time(hc->type);	/* Ensure cache sorted */
}

/**
 * Loads caught hosts from text file.
 */
static void
hcache_retrieve(hostcache_t *hc, const char *filename)
{
	file_path_t fp[1];
	FILE *f;

	file_path_set(fp, settings_config_dir(), filename);
	f = file_config_open_read(hc->name, fp, G_N_ELEMENTS(fp));
	if (f) {
		hcache_load_file(hc, f);
		fclose(f);
	}
}

/**
 * Write all data from cache to supplied file.
 */
static void
hcache_write(FILE *f, hostcache_t *hc)
{
	hash_list_iter_t *iter;
	gnet_host_t *h;

	iter = hash_list_iterator(hc->hostlist);
	while (NULL != (h = hash_list_iter_next(iter))) {
		const hostcache_entry_t *hce;

		hce = hcache_get_metadata(hc->class, h);
    	if (hce == NULL || hce == NO_METADATA)
			continue;
		
		fprintf(f, "%s %s\n",
			gnet_host_to_string(h), timestamp_utc_to_string(hce->time_added));
	}
	hash_list_iter_release(&iter);
}

/**
 * Persist hostcache to disk.
 * If `extra' is not HCACHE_NONE, it is appended after the dump of `type'.
 */
static void
hcache_store(hcache_type_t type, const char *filename, hcache_type_t extra)
{
	FILE *f;
	file_path_t fp;

	g_assert((guint) type < HCACHE_MAX && type != HCACHE_NONE);
	g_assert((guint) extra < HCACHE_MAX);
	g_assert(caches[type] != NULL);
	g_assert(extra == HCACHE_NONE || caches[extra] != NULL);

	file_path_set(&fp, settings_config_dir(), filename);
	f = file_config_open_write(filename, &fp);

	if (!f)
		return;

	hcache_write(f, caches[type]);

	if (extra != HCACHE_NONE)
		hcache_write(f, caches[extra]);

	file_config_close(f, &fp);
}

/**
 * Get statistical information about the caches.
 *
 * @param s must point to an hcache_stats_t[HCACHE_MAX] array.
 */
void
hcache_get_stats(hcache_stats_t *s)
{
    guint n;

    for (n = 0; n < HCACHE_MAX; n++) {
		if (n == HCACHE_NONE)
			continue;
        s[n].host_count = hash_list_length(caches[n]->hostlist);
        s[n].hits       = caches[n]->hits;
        s[n].misses     = caches[n]->misses;
        s[n].reading    = FALSE;
    }
}

/**
 * Host cache timer.
 */
static gboolean
hcache_timer(gpointer unused_obj)
{
	time_t now = tm_time();

	(void) unused_obj;

    hcache_expire_all(now);

    if (GNET_PROPERTY(hcache_debug) >= 15) {
        hcache_dump_info(caches[HCACHE_FRESH_ANY],   "timer");
        hcache_dump_info(caches[HCACHE_VALID_ANY],   "timer");

        hcache_dump_info(caches[HCACHE_FRESH_ULTRA], "timer");
        hcache_dump_info(caches[HCACHE_VALID_ULTRA], "timer");

        hcache_dump_info(caches[HCACHE_TIMEOUT],  "timer");
        hcache_dump_info(caches[HCACHE_BUSY],     "timer");
        hcache_dump_info(caches[HCACHE_UNSTABLE], "timer");

        g_debug("HCACHE global: local %u   alrdy connected %u   invalid %u",
            stats[HCACHE_LOCAL_INSTANCE], stats[HCACHE_ALREADY_CONNECTED],
            stats[HCACHE_INVALID_HOST]);
    }

	return TRUE;	/* Keep calling */
}

/**
 * Save hostcache data to disk, for the relevant host type.
 */
static void
hcache_store_if_dirty(host_type_t type)
{
	hcache_type_t first, second;
	const char *file;

	switch (type) {
    case HOST_ANY:
		first = HCACHE_VALID_ANY;
		second = HCACHE_FRESH_ANY;
		file = HOSTS_FILE;
        break;
    case HOST_ULTRA:
		first = HCACHE_VALID_ULTRA;
		second = HCACHE_FRESH_ULTRA;
		file = ULTRAS_FILE;
		break;
    case HOST_GUESS:
		first = HCACHE_GUESS_INTRO;
		second = HCACHE_GUESS;
		file = GUESS_FILE;
		break;
	default:
		g_error("can't store cache for host type %d", type);
		return;
	}

	if (!caches[first]->dirty && !caches[second]->dirty)
		return;

	hcache_store(first, file, second);

	caches[first]->dirty = caches[second]->dirty = FALSE;
}

/**
 * Host cache periodic saving.
 */
static gboolean
hcache_periodic_save(gpointer unused_obj)
{
	static unsigned i;

	(void) unused_obj;

	switch (i) {
	case 0: hcache_store_if_dirty(HOST_ANY); break;
	case 1: hcache_store_if_dirty(HOST_ULTRA); break;
	case 2: hcache_store_if_dirty(HOST_GUESS); break;
	default:
		g_assert_not_reached();
	}
	i = (i + 1) % 3;

	return TRUE;		/* Keep calling */
}

/**
 * Initialize host caches.
 */
void
hcache_init(void)
{
	ht_known_hosts = g_hash_table_new(gnet_host_hash, gnet_host_eq);
	ht_guess_hosts = g_hash_table_new(gnet_host_hash, gnet_host_eq);
	no_metadata = vmm_trap_page();

    caches[HCACHE_FRESH_ANY] = hcache_alloc(
        HCACHE_FRESH_ANY,
        PROP_HOSTS_IN_CATCHER,
        "hosts.fresh.any");

    caches[HCACHE_FRESH_ULTRA] = hcache_alloc(
        HCACHE_FRESH_ULTRA,
        PROP_HOSTS_IN_ULTRA_CATCHER,
        "hosts.fresh.ultra");

    caches[HCACHE_VALID_ANY] = hcache_alloc(
        HCACHE_VALID_ANY,
        PROP_HOSTS_IN_CATCHER,
        "hosts.valid.any");

    caches[HCACHE_VALID_ULTRA] = hcache_alloc(
        HCACHE_VALID_ULTRA,
        PROP_HOSTS_IN_ULTRA_CATCHER,
        "hosts.valid.ultra");

    caches[HCACHE_TIMEOUT] = hcache_alloc(
        HCACHE_TIMEOUT,
        PROP_HOSTS_IN_BAD_CATCHER,
        "hosts.timeout");
    caches[HCACHE_TIMEOUT]->addr_only = TRUE;

    caches[HCACHE_BUSY] = hcache_alloc(
        HCACHE_BUSY,
        PROP_HOSTS_IN_BAD_CATCHER,
        "hosts.busy");
    caches[HCACHE_BUSY]->addr_only = TRUE;

    caches[HCACHE_UNSTABLE] = hcache_alloc(
        HCACHE_UNSTABLE,
        PROP_HOSTS_IN_BAD_CATCHER,
        "hosts.unstable");
    caches[HCACHE_UNSTABLE]->addr_only = TRUE;

    caches[HCACHE_ALIEN] = hcache_alloc(
        HCACHE_ALIEN,
        PROP_HOSTS_IN_BAD_CATCHER,
        "hosts.alien");

    caches[HCACHE_GUESS] = hcache_alloc(
        HCACHE_GUESS,
        PROP_HOSTS_IN_GUESS_CATCHER,
        "hosts.guess.running");

    caches[HCACHE_GUESS_INTRO] = hcache_alloc(
        HCACHE_GUESS_INTRO,
        PROP_HOSTS_IN_GUESS_INTRO_CATCHER,
        "hosts.guess.intro");

	hcache_save_ev = cq_periodic_main_add(
		HCACHE_SAVE_PERIOD * 1000, hcache_periodic_save, NULL);
	hcache_timer_ev = cq_periodic_main_add(1000, hcache_timer, NULL);
}

/**
 * Load hostcache data from disk.
 */
void
hcache_retrieve_all(void)
{
	hcache_retrieve(caches[HCACHE_FRESH_ANY], HOSTS_FILE);
	hcache_retrieve(caches[HCACHE_FRESH_ULTRA], ULTRAS_FILE);
	hcache_retrieve(caches[HCACHE_GUESS], GUESS_FILE);
}

/**
 * Shutdown host caches.
 */
void
hcache_shutdown(void)
{
	cq_periodic_remove(&hcache_save_ev);
	hcache_store(HCACHE_VALID_ANY, HOSTS_FILE, HCACHE_FRESH_ANY);
	hcache_store(HCACHE_VALID_ULTRA, ULTRAS_FILE, HCACHE_FRESH_ULTRA);
	hcache_store(HCACHE_GUESS, GUESS_FILE, HCACHE_GUESS_INTRO);
}

/**
 * Destroy all host caches.
 */
void
hcache_close(void)
{
	static const hcache_type_t types[] = {
        HCACHE_FRESH_ANY,
        HCACHE_VALID_ANY,
        HCACHE_FRESH_ULTRA,
        HCACHE_VALID_ULTRA,
        HCACHE_TIMEOUT,
        HCACHE_BUSY,
        HCACHE_UNSTABLE,
        HCACHE_ALIEN,
        HCACHE_GUESS,
        HCACHE_GUESS_INTRO,
    };
	guint i;

	g_assert(!hcache_close_running);
	hcache_close_running = TRUE;

    /*
     * First we stop all background processes and remove all hosts,
     * only then we free the hcaches. This is important because
     * hcache_require_caught will crash if we free certain hostcaches.
     */

	for (i = 0; i < G_N_ELEMENTS(types); i++) {
		guint j;
		hcache_type_t type = types[i];

		/* Make sure all previous caches have been cleared */
		for (j = 0; j < type; j++)
    		g_assert(hash_list_length(caches[j]->hostlist) == 0);

        hcache_remove_all(caches[type]);

		/* Make sure no caches have been refilled */
		for (j = 0; j <= type; j++)
    		g_assert(hash_list_length(caches[j]->hostlist) == 0);
	}

	for (i = 0; i < G_N_ELEMENTS(types); i++) {
		hcache_type_t type = types[i];

		hcache_free(caches[type]);
        caches[type] = NULL;
	}

    g_assert(g_hash_table_size(ht_known_hosts) == 0);
    g_assert(g_hash_table_size(ht_guess_hosts) == 0);

	gm_hash_table_destroy_null(&ht_known_hosts);
	gm_hash_table_destroy_null(&ht_guess_hosts);
	cq_periodic_remove(&hcache_timer_ev);
}

/* vi: set ts=4 sw=4 cindent: */
