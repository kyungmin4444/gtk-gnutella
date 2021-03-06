/*
 * Copyright (c) 2014 Raphael Manfredi
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
 * @ingroup shell
 * @file
 *
 * The "random" command.
 *
 * This cheaply turns gtk-gnutella into a random number server.
 *
 * The random numbers generated come from the AJE layer, i.e. are perfectly
 * random and the sequence is totally unpredictable.  The AJE layer is fed
 * some entropy on a regular basis and the output is cryptographically strong,
 * meaning these random numbers can be used to generate certificates or keys.
 *
 * @author Raphael Manfredi
 * @date 2014
 */

#include "common.h"

#include "cmd.h"

#include "lib/aje.h"
#include "lib/base16.h"
#include "lib/options.h"
#include "lib/parse.h"
#include "lib/random.h"
#include "lib/str.h"
#include "lib/xmalloc.h"

#include "lib/override.h"		/* Must be the last header included */

#define RANDOM_BYTES_MAX	4096	/* Max amount of random bytes we generate */
#define RANDOM_NUM_MAX		1024	/* Max amount of entries we generate */

/**
 * Parse value as an unsigned 32-bit integer.
 *
 * @param sh		the shell for which we're processing the command
 * @param what		the item being parsed
 * @param value		the option value
 * @param result	where the parsed value is returned
 *
 * @return TRUE if OK, FALSE on error with an error message emitted.
 */
static bool
shell_parse_uint32(struct gnutella_shell *sh,
	const char *what, const char *value, uint32 *result)
{
	int error;
	uint base;
	const char *start;

	base = parse_base(value, &start);
	if (0 == base) {
		error = EINVAL;
		goto failed;
	}

	*result = parse_uint32(start, NULL, base, &error);
	if (error != 0)
		goto failed;

	return TRUE;

failed:
	shell_write_linef(sh, REPLY_ERROR, "cannot parse %s: %s",
		what, g_strerror(error));

	return FALSE;
}

/**
 * Generate random numbers.
 */
enum shell_reply
shell_exec_random(struct gnutella_shell *sh, int argc, const char *argv[])
{
	const char *opt_x, *opt_b, *opt_n;
	const option_t options[] = {
		{ "b:", &opt_b },			/* how many bytes to generate */
		{ "n:", &opt_n },			/* how many numbers to generate */
		{ "x",  &opt_x },			/* display in hexadecimal */
	};
	uint32 upper = 255, lower = 0;
	uint32 bytes = 1, amount = 1;
	int parsed;
	char *buf = NULL, *hexbuf = NULL;
	enum shell_reply result = REPLY_ERROR;

	shell_check(sh);
	g_assert(argv);
	g_assert(argc > 0);

	parsed = shell_options_parse(sh, argv, options, G_N_ELEMENTS(options));
	if (parsed < 0)
		return REPLY_ERROR;

	argv += parsed;		/* argv[0] is now the first command argument */
	argc -= parsed;		/* Only counts remaining arguments */

	if (argc >= 1) {
		if (!shell_parse_uint32(sh, "upper", argv[0], &upper))
			goto failed;
	}

	if (argc >= 2) {
		if (!shell_parse_uint32(sh, "lower", argv[1], &lower))
			goto failed;
	}

	if (upper < lower) {
		shell_write_line(sh, REPLY_ERROR,
			"upper boundary smaller than the lower one");
		goto failed;
	}

	if (opt_b != NULL) {
		if (argc >= 1) {
			shell_write_line(sh, REPLY_ERROR,
				"cannot specify upper or lower boundaries with -b");
			goto failed;
		}

		if (!shell_parse_uint32(sh, "-b", opt_b, &bytes))
			goto failed;

		bytes = MIN(bytes, RANDOM_BYTES_MAX);
		buf = xmalloc(bytes);
		hexbuf = xmalloc(2 * bytes + 1);	/* Hexa format + trailing NUL */
		hexbuf[2 * bytes] = '\0';
	}

	if (opt_n != NULL) {
		if (!shell_parse_uint32(sh, "-n", opt_n, &amount))
			goto failed;

		amount = MIN(amount, RANDOM_NUM_MAX);
	}

	while (amount-- != 0) {
		if (buf != NULL) {
			aje_random_bytes(buf, bytes);
			base16_encode(hexbuf, 2 * bytes, buf, bytes);
			shell_write_line(sh, REPLY_READY, hexbuf);
		} else {
			int32 r = lower + random_upto(aje_rand_strong, upper - lower);
			shell_write_line(sh, REPLY_READY,
				str_smsg(opt_x != NULL ? "%x" : "%d", r));
		}
	}

	result = REPLY_READY;
	goto done;

failed:
	shell_set_msg(sh, _("Invalid command syntax"));

	/* FALL THROUGH */

done:
	XFREE_NULL(buf);
	XFREE_NULL(hexbuf);
	return result;
}

const char *
shell_summary_random(void)
{
	return "Generate random numbers";
}

const char *
shell_help_random(int argc, const char *argv[])
{
	g_assert(argv);
	g_assert(argc > 0);

	return "random [-b bytes] [-n amount] [-x] [upper [lower]]\n"
		"Generate uniformly distributed random numbers.\n"
		"By default: upper=255, lower=0\n"
		"Values given as decimal, hexadecimal (0x), octal (0) or binary (0b)\n"
		"-b : amount of random bytes to generate (implies -x), max 4096.\n"
		"-n : amount of numbers or sequences of random bytes (1024 max).\n"
		"-x : display numbers in hexadecimal.\n";
}

/* vi: set ts=4 sw=4 cindent: */
