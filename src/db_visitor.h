/*
 * Copyright (C) 2003-2011 The Music Player Daemon Project
 * http://www.musicpd.org
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
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPD_DB_VISITOR_H
#define MPD_DB_VISITOR_H

struct directory;
struct song;
struct playlist_metadata;

struct db_visitor {
	/**
	 * Visit a directory.  Optional method.
	 *
	 * @return true to continue the operation, false on error (set error_r)
	 */
	bool (*directory)(const struct directory *directory, void *ctx,
			  GError **error_r);

	/**
	 * Visit a song.  Optional method.
	 *
	 * @return true to continue the operation, false on error (set error_r)
	 */
	bool (*song)(struct song *song, void *ctx, GError **error_r);

	/**
	 * Visit a playlist.  Optional method.
	 *
	 * @return true to continue the operation, false on error (set error_r)
	 */
	bool (*playlist)(const struct playlist_metadata *playlist, void *ctx,
			 GError **error_r);
};

#endif
