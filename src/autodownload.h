/*
 * Copyright (c) 2001-2002, Raphael Manfredi
 *
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
 */

#ifndef _autodownload_h_
#define _autodownload_h_

extern int use_autodownload;
extern gchar *auto_download_file;

extern void autodownload_init();
extern void autodownload_notify(gchar * file, guint32 size,
                                guint32 record_index, guint32 ip,
                                guint16 port, gchar *guid,
								gchar *sha1, guint32, gboolean);

#endif /* _autodownload_h_ */

