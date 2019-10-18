/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * udl_cursor.h
 *
 * Copyright (c) 2015 The Chromium OS Authors
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _UDL_CURSOR_H_
#define _UDL_CURSOR_H_

#include <linux/module.h>
#include <drm/drm_crtc.h>

#define UDL_CURSOR_W 64
#define UDL_CURSOR_H 64
#define UDL_CURSOR_BUF (UDL_CURSOR_W * UDL_CURSOR_H)
struct udl_cursor {
	uint32_t buffer[UDL_CURSOR_BUF];
	bool enabled;
	int x;
	int y;
};
struct udl_cursor_hline {
	uint32_t *buffer;
	int width;
	int offset;
};

extern void udl_cursor_get_hline(struct udl_cursor *cursor, int x, int y,
		struct udl_cursor_hline *hline);
extern int udl_cursor_move(struct udl_cursor *cursor, int x, int y);
extern int udl_cursor_download(struct udl_cursor *cursor, const struct iosys_map *map);

#endif
