/*
 * Copyright (c) 2003, Jeroen Asselman
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
 * Tigertree hash verification.
 *
 * @author Jeroen Asselman
 * @date 2003
 */

#include "common.h"

#include "huge.h"
#include "share.h"
#include "tth_cache.h"
#include "verify_tth.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"

#include "lib/base32.h"
#include "lib/halloc.h"
#include "lib/once.h"
#include "lib/tiger.h"
#include "lib/tigertree.h"
#include "lib/tm.h"

#include "lib/override.h"		/* Must be the last inclusion */

static struct {
	struct verify	*verify;
	TTH_CONTEXT		*context;
	struct tth		digest;
} verify_tth;

static const char *
verify_tth_name(void)
{
	return "TTH";
}

static void
verify_tth_reset(filesize_t size)
{
	if G_LIKELY(verify_tth.context != NULL)
		tt_init(verify_tth.context, size);
}

static int
verify_tth_update(const void *data, size_t size)
{
	if G_UNLIKELY(NULL == verify_tth.context)
		return -1;

	tt_update(verify_tth.context, data, size);
	return 0;
}

static int
verify_tth_final(void)
{
	if G_UNLIKELY(NULL == verify_tth.context)
		return -1;

	tt_digest(verify_tth.context, &verify_tth.digest);
	return 0;
}

static const struct verify_hash verify_hash_tth = {
	verify_tth_name,
	verify_tth_reset,
	verify_tth_update,
	verify_tth_final,
};

const struct tth *
verify_tth_digest(const struct verify *ctx)
{
	g_return_val_if_fail(verify_status(ctx) == VERIFY_DONE, NULL);
	return &verify_tth.digest;
}

const struct tth *
verify_tth_leaves(const struct verify *ctx)
{
	g_return_val_if_fail(verify_status(ctx) == VERIFY_DONE, NULL);
	return tt_leaves(verify_tth.context);
}

size_t
verify_tth_leave_count(const struct verify *ctx)
{
	g_return_val_if_fail(verify_status(ctx) == VERIFY_DONE, 0);
	return tt_leave_count(verify_tth.context);
}

static G_GNUC_COLD void
verify_tth_init_once(void)
{
	verify_tth.context = halloc(tt_size());
	verify_tth.verify = verify_new(&verify_hash_tth);
}

G_GNUC_COLD void
verify_tth_init(void)
{
	static once_flag_t initialized;

	/*
	 * We cannot use once_flag_run() because verify_new() can create a thread
	 * and cause the current thread to sleep with a lock (the mutex that
	 * the once layer will acquire).
	 *
	 * Therefore we need to use once_flag_runwait(), which can block the
	 * calling thread on a condition but does not hold any lock when invoking
	 * the init routine.
	 */

	once_flag_runwait(&initialized, verify_tth_init_once);
}

/**
 * Stops the background task for tigertree verification.
 */
G_GNUC_COLD void
verify_tth_shutdown(void)
{
	verify_free(&verify_tth.verify);
}

/**
 * Release memory resources used by tigertree verification.
 */
G_GNUC_COLD void
verify_tth_close(void)
{
	HFREE_NULL(verify_tth.context);
}

static bool 
request_tigertree_callback(const struct verify *ctx, enum verify_status status,
	void *user_data)
{
	shared_file_t *sf = user_data;

	shared_file_check(sf);
	switch (status) {
	case VERIFY_START:
		if (!shared_file_indexed(sf)) {
			/*
			 * After a rescan, there might be files in the queue which are
			 * no longer shared.
			 */

			if (GNET_PROPERTY(verify_debug) > 1) {
				g_debug("skipping TTH computation for %s: no longer shared",
					shared_file_path(sf));
			}
			return FALSE;
		}
		if (
			shared_file_tth(sf) &&
			tth_cache_lookup(shared_file_tth(sf), shared_file_size(sf)) > 0
		) {
			if (
				GNET_PROPERTY(tigertree_debug) > 1 ||
				GNET_PROPERTY(verify_debug) > 1
			) {
				g_debug("TTH for %s is already cached (%s)",
					shared_file_path(sf), tth_base32(shared_file_tth(sf)));
			}
			return FALSE;
		}
		gnet_prop_set_boolean_val(PROP_TTH_REBUILDING, TRUE);
		return TRUE;
	case VERIFY_PROGRESS:
		return 0 != (SHARE_F_INDEXED & shared_file_flags(sf));
	case VERIFY_DONE:
		{
			const struct tth *tth = verify_tth_digest(ctx);
			
			huge_update_hashes(sf, shared_file_sha1(sf), tth);
			tth_cache_insert(tth, verify_tth_leaves(ctx),
				verify_tth_leave_count(ctx));
		}
		/* FALL THROUGH */
	case VERIFY_ERROR:
	case VERIFY_SHUTDOWN:
		shared_file_unref(&sf);
		gnet_prop_set_boolean_val(PROP_TTH_REBUILDING, FALSE);
		return TRUE;
	case VERIFY_INVALID:
		break;
	}
	g_assert_not_reached();
	return FALSE;
}

bool
verify_tth_append(const char *pathname,
	filesize_t offset, filesize_t amount,
	verify_callback callback, void *user_data)
{
	return verify_enqueue(verify_tth.verify, FALSE,
				pathname, offset, amount, callback, user_data);
}

bool
verify_tth_prepend(const char *pathname,
	filesize_t offset, filesize_t amount,
	verify_callback callback, void *user_data)
{
	return verify_enqueue(verify_tth.verify, TRUE,
				pathname, offset, amount, callback, user_data);
}

void
request_tigertree(shared_file_t *sf, bool high_priority)
{
	int inserted;

	verify_tth_init();

	shared_file_check(sf);
	g_return_if_fail(!shared_file_is_partial(sf));

	if (!shared_file_indexed(sf))
		return;		/* "stale" shared file, has been superseded or removed */

	/*
	 * This routine can be called when the VERIFY_DONE event is received by
	 * huge_verify_callback().  We may have already shutdown the TTH
	 * verification thread.
	 */

	if G_UNLIKELY(NULL == verify_tth.verify)
		return;

	sf = shared_file_ref(sf);

	inserted = verify_enqueue(verify_tth.verify, high_priority,
					shared_file_path(sf), 0, shared_file_size(sf),
					request_tigertree_callback, sf);

	if (!inserted)
		shared_file_unref(&sf);
}

/* vi: set ts=4 sw=4 cindent: */
