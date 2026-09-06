/* PoxChat
 * Copyright (C) 2024 PoxChat Contributors
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * SQLite-based scrollback storage implementation
 */

#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <glib.h>
#include <gio/gio.h>
#include <sqlite3.h>

#include "poxchat.h"
#include "poxchatc.h"
#include "scrollback.h"
#include "util.h"
#include "sqlite-zstd-vfs.h"
#include "cfgfiles.h"
#include "text.h"

struct scrollback_db {
	sqlite3 *db;
	char *network;
	int transaction_depth;	/* ref-counted transaction nesting */

	/* Prepared statements for performance */
	sqlite3_stmt *stmt_insert;
	sqlite3_stmt *stmt_load;
	sqlite3_stmt *stmt_newest_msgid;
	sqlite3_stmt *stmt_global_newest_msgid;
	sqlite3_stmt *stmt_oldest_msgid;
	sqlite3_stmt *stmt_newest_time;
	sqlite3_stmt *stmt_has_msgid;
	sqlite3_stmt *stmt_clear;

	/* IRCv3 reactions and replies */
	sqlite3_stmt *stmt_save_reaction;
	sqlite3_stmt *stmt_remove_reaction;
	sqlite3_stmt *stmt_load_reactions;
	sqlite3_stmt *stmt_load_reactions_by_msgid;
	sqlite3_stmt *stmt_save_reply;
	sqlite3_stmt *stmt_load_reply;
	sqlite3_stmt *stmt_load_reply_by_msgid;
	sqlite3_stmt *stmt_update_pending;
	sqlite3_stmt *stmt_redact;

	/* Virtual scrollback support */
	sqlite3_stmt *stmt_count;
	sqlite3_stmt *stmt_load_range;
	sqlite3_stmt *stmt_load_range_fwd;	/* keyset: rows at/after an anchor, ascending */
	sqlite3_stmt *stmt_load_range_bwd;	/* keyset: rows at/before an anchor, descending */
	GHashTable *chan_cache;			/* channel_id -> sb_chan_cache (counts + ordinal anchors) */
	sqlite3_stmt *stmt_row_key;		/* (timestamp) of one rowid */

	/* FTS5 trigram index over stripped message text (search bar).
	 * fts is FALSE when the SQLite build lacks FTS5; every path then
	 * falls back to the linear scan. */
	gboolean fts;
	sqlite3_stmt *stmt_fts_insert;
	sqlite3_stmt *stmt_fts_delete;
	sqlite3_stmt *stmt_fts_exists;
	sqlite3_stmt *stmt_fts_search;
	sqlite3_stmt *stmt_fts_backfill;
	GHashTable *fts_state;			/* channel_id -> FTS_* state */
	GQueue *fts_queue;				/* gint64* channel ids awaiting backfill */
	guint fts_idle;
	gint64 fts_cur_channel;			/* channel being backfilled (0 = none) */
	gint64 fts_cur_next;			/* resume after this rowid */
	gint64 fts_cur_max;				/* snapshot upper bound */
	sqlite3_stmt *stmt_count_span;	/* rows in ((ts,id) lo, (ts,id) hi] for a channel */
	sqlite3_stmt *stmt_max_rowid;
	sqlite3_stmt *stmt_index_of_rowid;
	sqlite3_stmt *stmt_search_text;

	/* Channel name normalization */
	sqlite3_stmt *stmt_channel_insert;
	sqlite3_stmt *stmt_channel_lookup;
	GHashTable *channel_id_cache;    /* channel name -> GINT_TO_POINTER(channel_id) */

	/* Gap ledger */
	sqlite3_stmt *stmt_gap_list;
	sqlite3_stmt *stmt_gap_ordinal;
};

/* Hash table of open databases: network -> scrollback_db */
static GHashTable *open_dbs = NULL;

static char *
get_scrollback_dir (void)
{
	return g_build_filename (get_xdir (), "scrollback", NULL);
}

static char *
get_db_path (const char *network)
{
	char *dir = get_scrollback_dir ();
	char *safe_network = g_strdup (network);
	char *path;

	/* Sanitize network name for use as filename */
	for (char *p = safe_network; *p; p++)
	{
		if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
		    *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|')
			*p = '_';
	}

	path = g_build_filename (dir, safe_network, NULL);
	g_free (dir);

	/* Add .db extension */
	char *full_path = g_strdup_printf ("%s.db", path);
	g_free (path);
	g_free (safe_network);

	return full_path;
}

/* --- Channel ID resolver --- */

/* --- Per-channel ordinal cache ---------------------------------------
 *
 * The virtual scrollback addresses rows by ordinal (position in
 * (timestamp, id) order).  Two things made that expensive on the GUI
 * thread: COUNT(*) on every layout pass, and LIMIT/OFFSET paging that
 * walks `offset` index rows through the decompressing VFS on every
 * scroll step.  This cache remembers, per channel:
 *
 *   - the row count and newest timestamp, so a pure append (timestamp
 *     >= newest) just bumps the count instead of re-counting;
 *   - a bounded set of (ordinal -> (timestamp, id)) anchors taken from
 *     the edges of every range loaded, so a later load near a known
 *     ordinal can seek from the anchor with a keyset predicate and only
 *     skip the short distance between them.
 *
 * Anything that can shift ordinals (an insert that isn't an append, any
 * delete) invalidates the channel's entry; the next count/load rebuilds
 * it.  Ordinals are only ever consumed on the GUI thread, so no locking. */

#define SB_ANCHOR_MAX 128

/* POXCHAT_NO_ANCHORS=1 disables keyset seeking (plain OFFSET paging and
 * prefix counts only) so the anchor cache can be ruled in or out. */
static gboolean
sb_anchors_disabled (void)
{
	static int state = -1;
	if (state < 0)
		state = g_getenv ("POXCHAT_NO_ANCHORS") ? 1 : 0;
	return state == 1;
}

typedef struct {
	int ordinal;
	gint64 ts;
	gint64 id;
} sb_anchor;

typedef struct {
	int count;			/* -1 = unknown */
	gint64 max_ts;		/* newest timestamp when count was taken/bumped */
	GArray *anchors;	/* sb_anchor, sorted by ordinal */
} sb_chan_cache;

static void
chan_cache_free (gpointer data)
{
	sb_chan_cache *c = data;
	if (c->anchors)
		g_array_free (c->anchors, TRUE);
	g_free (c);
}

static sb_chan_cache *
chan_cache_get (scrollback_db *db, gint64 channel_id, gboolean create)
{
	sb_chan_cache *c;

	if (!db->chan_cache)
	{
		if (!create)
			return NULL;
		db->chan_cache = g_hash_table_new_full (g_int64_hash, g_int64_equal,
		                                        g_free, chan_cache_free);
	}
	c = g_hash_table_lookup (db->chan_cache, &channel_id);
	if (!c && create)
	{
		gint64 *key = g_new (gint64, 1);
		*key = channel_id;
		c = g_new0 (sb_chan_cache, 1);
		c->count = -1;
		c->anchors = g_array_new (FALSE, FALSE, sizeof (sb_anchor));
		g_hash_table_insert (db->chan_cache, key, c);
	}
	return c;
}

static void
chan_cache_invalidate (scrollback_db *db, gint64 channel_id)
{
	sb_chan_cache *c = chan_cache_get (db, channel_id, FALSE);
	if (!c)
		return;
	c->count = -1;
	c->max_ts = 0;
	g_array_set_size (c->anchors, 0);
}

static void
chan_cache_invalidate_all (scrollback_db *db)
{
	if (db->chan_cache)
		g_hash_table_remove_all (db->chan_cache);
}

/* Record that row (ts, id) sits at `ordinal`.  Keeps the array sorted and
 * bounded: when full, the anchor farthest from the newcomer is dropped,
 * which keeps the cluster around where the user is actually scrolling. */
static void
chan_cache_add_anchor (sb_chan_cache *c, int ordinal, gint64 ts, gint64 id)
{
	sb_anchor a;
	guint i;

	if (!c || c->count < 0 || ordinal < 0)
		return;

	for (i = 0; i < c->anchors->len; i++)
	{
		sb_anchor *e = &g_array_index (c->anchors, sb_anchor, i);
		if (e->ordinal == ordinal)
		{
			e->ts = ts;
			e->id = id;
			return;
		}
		if (e->ordinal > ordinal)
			break;
	}

	if (c->anchors->len >= SB_ANCHOR_MAX)
	{
		guint far_i = 0;
		int far_d = -1;
		guint j;
		for (j = 0; j < c->anchors->len; j++)
		{
			int d = ABS (g_array_index (c->anchors, sb_anchor, j).ordinal - ordinal);
			if (d > far_d)
			{
				far_d = d;
				far_i = j;
			}
		}
		g_array_remove_index (c->anchors, far_i);
		if (far_i < i)
			i--;
	}

	a.ordinal = ordinal;
	a.ts = ts;
	a.id = id;
	g_array_insert_val (c->anchors, i, a);
}

/* Best anchor at or before `ordinal` (largest ordinal <= it), and best at
 * or after `end` (smallest ordinal >= it).  Either may be NULL. */
static void
chan_cache_find_anchors (sb_chan_cache *c, int ordinal, int end,
                         sb_anchor **before, sb_anchor **after)
{
	guint i;

	*before = NULL;
	*after = NULL;
	if (!c || c->count < 0)
		return;

	for (i = 0; i < c->anchors->len; i++)
	{
		sb_anchor *e = &g_array_index (c->anchors, sb_anchor, i);
		if (e->ordinal <= ordinal)
			*before = e;
		if (e->ordinal >= end)
		{
			*after = e;
			break;
		}
	}
}

/* Count + newest timestamp for a channel, from cache or a fresh scan. */
static sb_chan_cache *
chan_cache_ensure_count (scrollback_db *db, gint64 channel_id)
{
	sb_chan_cache *c = chan_cache_get (db, channel_id, TRUE);

	if (c->count >= 0)
		return c;

	sqlite3_reset (db->stmt_count);
	sqlite3_bind_int64 (db->stmt_count, 1, channel_id);
	if (sqlite3_step (db->stmt_count) == SQLITE_ROW)
	{
		c->count = sqlite3_column_int (db->stmt_count, 0);
		c->max_ts = sqlite3_column_int64 (db->stmt_count, 1);
	}
	else
		c->count = 0;
	return c;
}

/* A row was stored: keep the cache coherent without a rescan when the
 * row lands at the end of the channel's order. */
static void
chan_cache_note_insert (scrollback_db *db, gint64 channel_id, gint64 ts)
{
	sb_chan_cache *c = chan_cache_get (db, channel_id, FALSE);
	if (!c || c->count < 0)
		return;
	if (c->count == 0 || ts >= c->max_ts)
	{
		c->count++;
		c->max_ts = ts;
	}
	else
	{
		poxchat_timing_log ("sbcache ch=%" G_GINT64_FORMAT " INVALIDATE backdated ts=%" G_GINT64_FORMAT
		                    " max_ts=%" G_GINT64_FORMAT " count=%d anchors=%u",
		                    channel_id, ts, c->max_ts, c->count, c->anchors->len);
		chan_cache_invalidate (db, channel_id);
	}
}

static gint64 scrollback_get_channel_id (scrollback_db *sdb, const char *channel);

/* --- FTS5 index maintenance ----------------------------------------- */

#define FTS_UNINDEXED 0		/* channel never scheduled; live rows are not indexed */
#define FTS_BUILDING  1		/* queued or in progress; live rows are indexed */
#define FTS_BUILT     2		/* channels.fts_built = 1 */

#define FTS_BACKFILL_CHUNK 500

static int
fts_state_get (scrollback_db *db, gint64 channel_id)
{
	gpointer v;
	int st = FTS_UNINDEXED;

	if (!db->fts)
		return FTS_UNINDEXED;
	if (!db->fts_state)
		db->fts_state = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
	if (g_hash_table_lookup_extended (db->fts_state, &channel_id, NULL, &v))
		return GPOINTER_TO_INT (v);

	{
		sqlite3_stmt *stmt;
		if (sqlite3_prepare_v2 (db->db,
			"SELECT fts_built FROM channels WHERE id = ?1", -1, &stmt, NULL) == SQLITE_OK)
		{
			sqlite3_bind_int64 (stmt, 1, channel_id);
			if (sqlite3_step (stmt) == SQLITE_ROW && sqlite3_column_int (stmt, 0))
				st = FTS_BUILT;
			sqlite3_finalize (stmt);
		}
	}
	{
		gint64 *key = g_new (gint64, 1);
		*key = channel_id;
		g_hash_table_insert (db->fts_state, key, GINT_TO_POINTER (st));
	}
	return st;
}

static void
fts_state_set (scrollback_db *db, gint64 channel_id, int st)
{
	gint64 *key;

	if (!db->fts_state)
		db->fts_state = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
	key = g_new (gint64, 1);
	*key = channel_id;
	g_hash_table_insert (db->fts_state, key, GINT_TO_POINTER (st));
}

static void
fts_index_row (scrollback_db *db, gint64 rowid, gint64 channel_id, const char *text)
{
	char *plain;

	if (!db->fts || !text)
		return;
	plain = strip_color (text, -1, STRIP_ALL);
	sqlite3_reset (db->stmt_fts_insert);
	sqlite3_bind_int64 (db->stmt_fts_insert, 1, rowid);
	sqlite3_bind_text (db->stmt_fts_insert, 2, plain, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64 (db->stmt_fts_insert, 3, channel_id);
	sqlite3_step (db->stmt_fts_insert);
	g_free (plain);
}

static gboolean
fts_row_indexed (scrollback_db *db, gint64 rowid)
{
	gboolean yes;
	sqlite3_reset (db->stmt_fts_exists);
	sqlite3_bind_int64 (db->stmt_fts_exists, 1, rowid);
	yes = sqlite3_step (db->stmt_fts_exists) == SQLITE_ROW;
	return yes;
}

static void
fts_unindex_row (scrollback_db *db, gint64 rowid)
{
	if (!db->fts)
		return;
	sqlite3_reset (db->stmt_fts_delete);
	sqlite3_bind_int64 (db->stmt_fts_delete, 1, rowid);
	sqlite3_step (db->stmt_fts_delete);
}

/* Idle backfill: one chunk per tick, oldest rowid first, in a
 * transaction.  Rows the live path already indexed (saved after the
 * channel was queued) are skipped by rowid lookup, so the order in which
 * a channel is queued versus written never matters. */
static gboolean
fts_backfill_idle (gpointer data)
{
	scrollback_db *db = data;
	int n = 0;
	gint64 last = 0;

	if (db->fts_cur_channel == 0)
	{
		gint64 *next = db->fts_queue ? g_queue_pop_head (db->fts_queue) : NULL;
		sqlite3_stmt *stmt;

		if (!next)
		{
			db->fts_idle = 0;
			return G_SOURCE_REMOVE;
		}
		db->fts_cur_channel = *next;
		db->fts_cur_next = 0;
		db->fts_cur_max = 0;
		g_free (next);

		if (sqlite3_prepare_v2 (db->db,
			"SELECT IFNULL(MAX(id), 0) FROM messages WHERE channel_id = ?1",
			-1, &stmt, NULL) == SQLITE_OK)
		{
			sqlite3_bind_int64 (stmt, 1, db->fts_cur_channel);
			if (sqlite3_step (stmt) == SQLITE_ROW)
				db->fts_cur_max = sqlite3_column_int64 (stmt, 0);
			sqlite3_finalize (stmt);
		}
	}

	scrollback_begin_transaction (db);
	sqlite3_reset (db->stmt_fts_backfill);
	sqlite3_bind_int64 (db->stmt_fts_backfill, 1, db->fts_cur_channel);
	sqlite3_bind_int64 (db->stmt_fts_backfill, 2, db->fts_cur_next);
	sqlite3_bind_int64 (db->stmt_fts_backfill, 3, db->fts_cur_max);
	sqlite3_bind_int (db->stmt_fts_backfill, 4, FTS_BACKFILL_CHUNK);
	while (sqlite3_step (db->stmt_fts_backfill) == SQLITE_ROW)
	{
		gint64 id = sqlite3_column_int64 (db->stmt_fts_backfill, 0);
		const char *text = (const char *) sqlite3_column_text (db->stmt_fts_backfill, 1);
		if (!fts_row_indexed (db, id))
			fts_index_row (db, id, db->fts_cur_channel, text);
		last = id;
		n++;
	}
	sqlite3_reset (db->stmt_fts_backfill);

	if (n < FTS_BACKFILL_CHUNK)
	{
		char *sql = sqlite3_mprintf ("UPDATE channels SET fts_built = 1 WHERE id = %lld",
		                             (long long) db->fts_cur_channel);
		sqlite3_exec (db->db, sql, NULL, NULL, NULL);
		sqlite3_free (sql);
		fts_state_set (db, db->fts_cur_channel, FTS_BUILT);
		db->fts_cur_channel = 0;
	}
	else
		db->fts_cur_next = last;
	scrollback_commit_transaction (db);

	return G_SOURCE_CONTINUE;
}

void
scrollback_fts_schedule (scrollback_db *db, const char *channel)
{
	gint64 channel_id, *item;

	if (!db || !db->fts || !channel)
		return;
	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0 || fts_state_get (db, channel_id) != FTS_UNINDEXED)
		return;

	fts_state_set (db, channel_id, FTS_BUILDING);
	if (!db->fts_queue)
		db->fts_queue = g_queue_new ();
	item = g_new (gint64, 1);
	*item = channel_id;
	g_queue_push_tail (db->fts_queue, item);
	if (!db->fts_idle)
		db->fts_idle = g_idle_add_full (G_PRIORITY_LOW, fts_backfill_idle, db, NULL);
}

gboolean
scrollback_fts_ready (scrollback_db *db, const char *channel)
{
	gint64 channel_id;

	if (!db || !db->fts || !channel)
		return FALSE;
	channel_id = scrollback_get_channel_id (db, channel);
	return channel_id > 0 && fts_state_get (db, channel_id) == FTS_BUILT;
}

GSList *
scrollback_search_fts (scrollback_db *db, const char *channel, const char *needle)
{
	GSList *list = NULL;
	GString *q;
	const char *c;
	gint64 channel_id;

	if (!db || !db->fts || !channel || !needle || !needle[0])
		return NULL;
	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return NULL;

	/* One quoted phrase: trigram MATCH on a phrase is a substring test,
	 * case-insensitive.  Only '"' needs escaping inside a phrase. */
	q = g_string_new ("\"");
	for (c = needle; *c; c++)
	{
		if (*c == '"')
			g_string_append_c (q, '"');
		g_string_append_c (q, *c);
	}
	g_string_append_c (q, '"');

	sqlite3_reset (db->stmt_fts_search);
	sqlite3_bind_int64 (db->stmt_fts_search, 1, channel_id);
	sqlite3_bind_text (db->stmt_fts_search, 2, q->str, -1, SQLITE_TRANSIENT);
	while (sqlite3_step (db->stmt_fts_search) == SQLITE_ROW)
	{
		scrollback_msg *m = g_new0 (scrollback_msg, 1);
		m->id = sqlite3_column_int64 (db->stmt_fts_search, 0);
		m->text = g_strdup ((const char *) sqlite3_column_text (db->stmt_fts_search, 1));
		list = g_slist_prepend (list, m);
	}
	sqlite3_reset (db->stmt_fts_search);
	g_string_free (q, TRUE);
	return g_slist_reverse (list);
}

static gint64
scrollback_get_channel_id (scrollback_db *sdb, const char *channel)
{
	gint64 id;
	gpointer cached;

	if (!channel || !channel[0])
		return -1;

	/* Check cache first */
	if (sdb->channel_id_cache &&
	    g_hash_table_lookup_extended (sdb->channel_id_cache, channel, NULL, &cached))
		return (gint64)GPOINTER_TO_INT (cached);

	/* Insert if new, then fetch ID */
	sqlite3_reset (sdb->stmt_channel_insert);
	sqlite3_bind_text (sdb->stmt_channel_insert, 1, channel, -1, SQLITE_TRANSIENT);
	sqlite3_step (sdb->stmt_channel_insert);

	sqlite3_reset (sdb->stmt_channel_lookup);
	sqlite3_bind_text (sdb->stmt_channel_lookup, 1, channel, -1, SQLITE_TRANSIENT);
	if (sqlite3_step (sdb->stmt_channel_lookup) == SQLITE_ROW)
		id = sqlite3_column_int64 (sdb->stmt_channel_lookup, 0);
	else
		return -1;

	/* Cache it */
	if (!sdb->channel_id_cache)
		sdb->channel_id_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_insert (sdb->channel_id_cache, g_strdup (channel), GINT_TO_POINTER ((int)id));

	return id;
}

static gboolean
ensure_scrollback_dir (void)
{
	char *dir = get_scrollback_dir ();
	gboolean result = TRUE;

	if (!g_file_test (dir, G_FILE_TEST_IS_DIR))
	{
		if (g_mkdir_with_parents (dir, 0700) != 0)
		{
			g_warning ("Failed to create scrollback directory: %s", dir);
			result = FALSE;
		}
	}

	g_free (dir);
	return result;
}

static gboolean
init_database (scrollback_db *sdb)
{
	const char *schema =
		"CREATE TABLE IF NOT EXISTS messages ("
		"    id INTEGER PRIMARY KEY,"
		"    channel TEXT NOT NULL,"
		"    timestamp INTEGER NOT NULL,"
		"    msgid TEXT,"
		"    text TEXT NOT NULL,"
		"    redacted_by TEXT,"
		"    redact_reason TEXT,"
		"    redact_time INTEGER"
		");"
		"CREATE INDEX IF NOT EXISTS idx_channel_time ON messages(channel, timestamp);"
		/* IRCv3 reactions: persisted per (channel, target_msgid,
		 * reaction_text, nick).  msgids are only unique per channel
		 * (multi-target messages share one), so the key is channel-scoped.
		 * Legacy DBs with the old global UNIQUE are rebuilt below. */
		"CREATE TABLE IF NOT EXISTS reactions ("
		"    id INTEGER PRIMARY KEY,"
		"    channel TEXT NOT NULL,"
		"    target_msgid TEXT NOT NULL,"
		"    reaction_text TEXT NOT NULL,"
		"    nick TEXT NOT NULL,"
		"    is_self INTEGER NOT NULL DEFAULT 0,"
		"    timestamp INTEGER NOT NULL,"
		"    channel_id INTEGER REFERENCES channels(id),"
		"    UNIQUE(channel_id, target_msgid, reaction_text, nick)"
		");"
		"CREATE INDEX IF NOT EXISTS idx_reactions_target ON reactions(channel, target_msgid);"
		/* IRCv3 replies: persisted as (channel, msgid → target_msgid).
		 * Channel-scoped for the same reason; legacy shape rebuilt below. */
		"CREATE TABLE IF NOT EXISTS replies ("
		"    id INTEGER PRIMARY KEY,"
		"    channel_id INTEGER REFERENCES channels(id),"
		"    msgid TEXT NOT NULL,"
		"    target_msgid TEXT NOT NULL,"
		"    target_nick TEXT,"
		"    target_preview TEXT,"
		"    UNIQUE(channel_id, msgid)"
		");";

	char *errmsg = NULL;
	int rc;

	/* Inner DB uses MEMORY journal — atomicity comes from the outer
	 * (compressed) DB's own transactions via the zstd VFS. */
	sqlite3_exec (sdb->db, "PRAGMA journal_mode=MEMORY;", NULL, NULL, NULL);
	/* The zstd VFS has no page cache of its own: every miss here is an
	 * outer-DB lookup plus a decompress.  SQLite's default (~2 MB) is far
	 * too small for a scroll through a large channel; give the inner
	 * connection a real working set (negative = KiB). */
	sqlite3_exec (sdb->db, "PRAGMA cache_size=-32768;", NULL, NULL, NULL);

	rc = sqlite3_exec (sdb->db, schema, NULL, NULL, &errmsg);

	if (rc != SQLITE_OK)
	{
		g_warning ("Failed to initialize scrollback database: %s", errmsg);
		sqlite3_free (errmsg);
		return FALSE;
	}

	/* Channel name normalization table */
	sqlite3_exec (sdb->db,
		"CREATE TABLE IF NOT EXISTS channels ("
		"    id INTEGER PRIMARY KEY,"
		"    name TEXT NOT NULL UNIQUE"
		");",
		NULL, NULL, NULL);

	/* Gap ledger: recorded holes in stored history (chathistory gap fill).
	 * Bounds are anchored to real stored rows, exclusive on both ends. */
	sqlite3_exec (sdb->db,
		"CREATE TABLE IF NOT EXISTS gaps ("
		"    id INTEGER PRIMARY KEY,"
		"    channel_id INTEGER NOT NULL REFERENCES channels(id),"
		"    start_ts INTEGER NOT NULL,"
		"    start_msgid TEXT,"
		"    end_ts INTEGER NOT NULL,"
		"    end_msgid TEXT,"
		"    state INTEGER NOT NULL DEFAULT 0,"
		"    attempts INTEGER NOT NULL DEFAULT 0,"
		"    last_attempt INTEGER NOT NULL DEFAULT 0"
		");",
		NULL, NULL, NULL);
	sqlite3_exec (sdb->db,
		"CREATE INDEX IF NOT EXISTS idx_gaps_channel ON gaps(channel_id, start_ts);",
		NULL, NULL, NULL);
	/* One-shot bootstrap latch per channel (heuristic candidate scan) */
	sqlite3_exec (sdb->db,
		"ALTER TABLE channels ADD COLUMN gap_bootstrap_done INTEGER NOT NULL DEFAULT 0;",
		NULL, NULL, NULL);

	/* Search index: an FTS5 trigram table gives true substring matching
	 * straight off the index, so the search bar no longer has to stream
	 * the whole channel through the decompressing VFS.  Rows are stored
	 * with IRC colour/attribute codes stripped.  Existing channels are
	 * indexed lazily (channels.fts_built) by an idle backfill. */
	sdb->fts = sqlite3_compileoption_used ("ENABLE_FTS5") != 0;
	if (sdb->fts)
	{
		sdb->fts = sqlite3_exec (sdb->db,
			"CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts "
			"USING fts5(text, channel_id UNINDEXED, tokenize='trigram');",
			NULL, NULL, NULL) == SQLITE_OK;
		sqlite3_exec (sdb->db,
			"ALTER TABLE channels ADD COLUMN fts_built INTEGER NOT NULL DEFAULT 0;",
			NULL, NULL, NULL);
	}

	/* Add channel_id column to messages (NULL = legacy, use channel TEXT) */
	sqlite3_exec (sdb->db,
		"ALTER TABLE messages ADD COLUMN channel_id INTEGER REFERENCES channels(id);",
		NULL, NULL, NULL);

	/* is_user_msg: 1 for PRIVMSG/NOTICE/ACTION, 0 for events (JOIN/QUIT/etc.).
	 * Drives whether re-materialized chathistory entries get hover reply/react
	 * buttons.  Legacy rows default to 0 — old user-speech entries won't get
	 * the buttons until they're re-saved, which is fine. */
	sqlite3_exec (sdb->db,
		"ALTER TABLE messages ADD COLUMN is_user_msg INTEGER NOT NULL DEFAULT 0;",
		NULL, NULL, NULL);

	/* Add channel_id column to reactions */
	sqlite3_exec (sdb->db,
		"ALTER TABLE reactions ADD COLUMN channel_id INTEGER REFERENCES channels(id);",
		NULL, NULL, NULL);

	/* Index on channel_id for messages (replaces idx_channel_time over time) */
	sqlite3_exec (sdb->db,
		"CREATE INDEX IF NOT EXISTS idx_channel_id_time ON messages(channel_id, timestamp);",
		NULL, NULL, NULL);

	/* Index on channel_id for reactions */
	sqlite3_exec (sdb->db,
		"CREATE INDEX IF NOT EXISTS idx_reactions_channel_id ON reactions(channel_id, target_msgid);",
		NULL, NULL, NULL);

	/* Migrate existing rows: populate channels table and channel_id */
	{
		int migrated = 0;
		sqlite3_stmt *sel_ch;
		rc = sqlite3_prepare_v2 (sdb->db,
			"SELECT DISTINCT channel FROM messages WHERE channel_id IS NULL AND channel IS NOT NULL",
			-1, &sel_ch, NULL);
		if (rc == SQLITE_OK)
		{
			while (sqlite3_step (sel_ch) == SQLITE_ROW)
			{
				const char *ch_name = (const char *)sqlite3_column_text (sel_ch, 0);
				if (ch_name)
				{
					char *sql = sqlite3_mprintf (
						"INSERT OR IGNORE INTO channels (name) VALUES (%Q);"
						"UPDATE messages SET channel_id = (SELECT id FROM channels WHERE name = %Q) "
						"WHERE channel = %Q AND channel_id IS NULL;"
						"UPDATE reactions SET channel_id = (SELECT id FROM channels WHERE name = %Q) "
						"WHERE channel = %Q AND channel_id IS NULL;",
						ch_name, ch_name, ch_name, ch_name, ch_name);
					sqlite3_exec (sdb->db, sql, NULL, NULL, NULL);
					sqlite3_free (sql);
					migrated++;
				}
			}
			sqlite3_finalize (sel_ch);
			if (migrated > 0)
				g_message ("scrollback: migrated %d channels to normalized IDs for %s",
				           migrated, sdb->network);
		}
	}

	/* Per-channel msgid uniqueness.  A multi-target PRIVMSG (or a
	 * STATUSMSG variant like @#chan) delivers one msgid to several
	 * targets; under the old global UNIQUE(msgid) the second channel's
	 * save was silently dropped, so its copy vanished on eviction and
	 * after restart.  Per-channel uniqueness is strictly weaker than
	 * global, so the new index always builds on existing data.  Must
	 * run after the channel_id backfill above so legacy rows are
	 * scoped too (rows still NULL compare distinct, which is safe).
	 * INSERT OR IGNORE dedup semantics are preserved per channel. */
	sqlite3_exec (sdb->db, "DROP INDEX IF EXISTS idx_msgid;", NULL, NULL, NULL);
	sqlite3_exec (sdb->db,
		"CREATE UNIQUE INDEX IF NOT EXISTS idx_channel_msgid "
		"ON messages(channel_id, msgid) WHERE msgid IS NOT NULL;",
		NULL, NULL, NULL);

	/* Rebuild legacy reactions/replies tables whose keys predate channel
	 * scoping.  Inline UNIQUE constraints can't be altered in place, so
	 * detect the old shape via sqlite_master and copy into the new shape.
	 * Must run after the channel_id backfill above so the copied rows
	 * carry their channel scope. */
	{
		sqlite3_stmt *chk;
		gboolean rebuild_reactions = FALSE, rebuild_replies = FALSE;

		if (sqlite3_prepare_v2 (sdb->db,
			"SELECT name, sql FROM sqlite_master WHERE type='table' "
			"AND name IN ('reactions','replies')",
			-1, &chk, NULL) == SQLITE_OK)
		{
			while (sqlite3_step (chk) == SQLITE_ROW)
			{
				const char *name = (const char *)sqlite3_column_text (chk, 0);
				const char *sql = (const char *)sqlite3_column_text (chk, 1);
				if (!name || !sql)
					continue;
				if (strcmp (name, "reactions") == 0 &&
				    strstr (sql, "UNIQUE(target_msgid") != NULL)
					rebuild_reactions = TRUE;
				if (strcmp (name, "replies") == 0 &&
				    strstr (sql, "msgid TEXT PRIMARY KEY") != NULL)
					rebuild_replies = TRUE;
			}
			sqlite3_finalize (chk);
		}

		if (rebuild_reactions)
		{
			rc = sqlite3_exec (sdb->db,
				"BEGIN;"
				"CREATE TABLE reactions_new ("
				"    id INTEGER PRIMARY KEY,"
				"    channel TEXT NOT NULL,"
				"    target_msgid TEXT NOT NULL,"
				"    reaction_text TEXT NOT NULL,"
				"    nick TEXT NOT NULL,"
				"    is_self INTEGER NOT NULL DEFAULT 0,"
				"    timestamp INTEGER NOT NULL,"
				"    channel_id INTEGER REFERENCES channels(id),"
				"    UNIQUE(channel_id, target_msgid, reaction_text, nick)"
				");"
				"INSERT OR IGNORE INTO reactions_new "
				"    (id, channel, target_msgid, reaction_text, nick, is_self, timestamp, channel_id) "
				"    SELECT id, channel, target_msgid, reaction_text, nick, is_self, timestamp, channel_id "
				"    FROM reactions;"
				"DROP TABLE reactions;"
				"ALTER TABLE reactions_new RENAME TO reactions;"
				"CREATE INDEX IF NOT EXISTS idx_reactions_target ON reactions(channel, target_msgid);"
				"CREATE INDEX IF NOT EXISTS idx_reactions_channel_id ON reactions(channel_id, target_msgid);"
				"COMMIT;",
				NULL, NULL, &errmsg);
			if (rc != SQLITE_OK)
			{
				g_warning ("scrollback: reactions channel-scope rebuild failed: %s", errmsg);
				sqlite3_free (errmsg);
				errmsg = NULL;
				sqlite3_exec (sdb->db, "ROLLBACK;", NULL, NULL, NULL);
			}
		}

		if (rebuild_replies)
		{
			/* channel_id backfill: pre-rebuild data was written under the
			 * global msgid uniqueness, so resolving through messages is
			 * unambiguous.  Orphaned reply rows (message gone) get NULL and
			 * are simply never loaded. */
			rc = sqlite3_exec (sdb->db,
				"BEGIN;"
				"CREATE TABLE replies_new ("
				"    id INTEGER PRIMARY KEY,"
				"    channel_id INTEGER REFERENCES channels(id),"
				"    msgid TEXT NOT NULL,"
				"    target_msgid TEXT NOT NULL,"
				"    target_nick TEXT,"
				"    target_preview TEXT,"
				"    UNIQUE(channel_id, msgid)"
				");"
				"INSERT OR IGNORE INTO replies_new "
				"    (channel_id, msgid, target_msgid, target_nick, target_preview) "
				"    SELECT (SELECT m.channel_id FROM messages m WHERE m.msgid = r.msgid LIMIT 1), "
				"           r.msgid, r.target_msgid, r.target_nick, r.target_preview "
				"    FROM replies r;"
				"DROP TABLE replies;"
				"ALTER TABLE replies_new RENAME TO replies;"
				"COMMIT;",
				NULL, NULL, &errmsg);
			if (rc != SQLITE_OK)
			{
				g_warning ("scrollback: replies channel-scope rebuild failed: %s", errmsg);
				sqlite3_free (errmsg);
				errmsg = NULL;
				sqlite3_exec (sdb->db, "ROLLBACK;", NULL, NULL, NULL);
			}
		}
	}

	/* A reply cannot precede what it answers.  Reply rows keyed to a
	 * message older than their target were written by the old attach
	 * fallback: a reply whose own line was not materialized (tab scrolled
	 * up, window full) got hung on the newest entry on screen instead,
	 * and persisted that way.  Drop them; the fixed attach never writes
	 * them again.  Cheap — both joins hit the (channel_id, msgid) index. */
	sqlite3_exec (sdb->db,
		"DELETE FROM replies WHERE id IN ("
		"  SELECT r.id FROM replies r"
		"  JOIN messages a ON a.channel_id = r.channel_id AND a.msgid = r.msgid"
		"  JOIN messages b ON b.channel_id = r.channel_id AND b.msgid = r.target_msgid"
		"  WHERE a.timestamp < b.timestamp)",
		NULL, NULL, NULL);

	return TRUE;
}

static gboolean
prepare_statements (scrollback_db *sdb)
{
	int rc;

	/* Channel name resolution */
	rc = sqlite3_prepare_v2 (sdb->db,
		"INSERT OR IGNORE INTO channels (name) VALUES (?)",
		-1, &sdb->stmt_channel_insert, NULL);
	if (rc != SQLITE_OK) goto fail;

	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT id FROM channels WHERE name = ?",
		-1, &sdb->stmt_channel_lookup, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Insert statement (channel kept for NOT NULL compat with original schema) */
	rc = sqlite3_prepare_v2 (sdb->db,
		"INSERT OR IGNORE INTO messages (channel, channel_id, timestamp, msgid, text, is_user_msg) "
		"VALUES (?, ?, ?, ?, ?, ?)",
		-1, &sdb->stmt_insert, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Load statement - get newest N messages in chronological order */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT id, channel_id, timestamp, msgid, text, redacted_by, redact_reason, "
		"redact_time, is_user_msg "
		"FROM messages WHERE channel_id = ? ORDER BY timestamp DESC, id DESC LIMIT ?",
		-1, &sdb->stmt_load, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Get newest msgid */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT msgid FROM messages WHERE channel_id = ? AND msgid IS NOT NULL "
		"AND msgid NOT LIKE 'pending:%' ORDER BY timestamp DESC, id DESC LIMIT 1",
		-1, &sdb->stmt_newest_msgid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Get newest msgid across every channel (draft/persistence attach
	 * cursor).  No (timestamp, id) index exists — see the design note at
	 * the call site — so this takes each channel's newest qualifying row
	 * off idx_channel_id_time and picks the newest of those. */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT m.msgid FROM channels c "
		"JOIN messages m ON m.id = ("
		"    SELECT id FROM messages"
		"     WHERE channel_id = c.id AND msgid IS NOT NULL AND msgid NOT LIKE 'pending:%'"
		"     ORDER BY timestamp DESC, id DESC LIMIT 1)"
		"ORDER BY m.timestamp DESC, m.id DESC LIMIT 1",
		-1, &sdb->stmt_global_newest_msgid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Get oldest msgid */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT msgid FROM messages WHERE channel_id = ? AND msgid IS NOT NULL "
		"AND msgid NOT LIKE 'pending:%' ORDER BY timestamp ASC, id ASC LIMIT 1",
		-1, &sdb->stmt_oldest_msgid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Get newest timestamp */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT MAX(timestamp) FROM messages WHERE channel_id = ?",
		-1, &sdb->stmt_newest_time, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Check msgid exists (channel-scoped: msgids are only unique per
	 * channel — a multi-target message shares one msgid across targets).
	 * Optional timestamp match (?3 = 0 skips it) mirrors the chathistory
	 * dedup key, guarding against servers that reuse msgids after a
	 * restart. */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT 1 FROM messages WHERE channel_id = ?1 AND msgid = ?2 "
		"AND (?3 = 0 OR timestamp = ?3) LIMIT 1",
		-1, &sdb->stmt_has_msgid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Clear channel */
	rc = sqlite3_prepare_v2 (sdb->db,
		"DELETE FROM messages WHERE channel_id = ?",
		-1, &sdb->stmt_clear, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* IRCv3 reactions: save (channel kept for NOT NULL compat) */
	rc = sqlite3_prepare_v2 (sdb->db,
		"INSERT OR REPLACE INTO reactions (channel, channel_id, target_msgid, reaction_text, nick, is_self, timestamp) "
		"VALUES (?, ?, ?, ?, ?, ?, ?)",
		-1, &sdb->stmt_save_reaction, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* IRCv3 reactions: remove (channel-scoped) */
	rc = sqlite3_prepare_v2 (sdb->db,
		"DELETE FROM reactions WHERE channel_id = ? AND target_msgid = ? "
		"AND reaction_text = ? AND nick = ?",
		-1, &sdb->stmt_remove_reaction, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* IRCv3 reactions: load all for a channel */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT target_msgid, reaction_text, nick, is_self FROM reactions "
		"WHERE channel_id = ? ORDER BY target_msgid, reaction_text",
		-1, &sdb->stmt_load_reactions, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* IRCv3 reactions: load for a single target msgid (used on virt re-materialize
	 * so re-built historical entries regain their badges); channel-scoped */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT target_msgid, reaction_text, nick, is_self FROM reactions "
		"WHERE channel_id = ? AND target_msgid = ? ORDER BY reaction_text",
		-1, &sdb->stmt_load_reactions_by_msgid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* IRCv3 replies: save (channel-scoped upsert on UNIQUE(channel_id, msgid)) */
	rc = sqlite3_prepare_v2 (sdb->db,
		"INSERT OR REPLACE INTO replies (channel_id, msgid, target_msgid, target_nick, target_preview) "
		"VALUES (?, ?, ?, ?, ?)",
		-1, &sdb->stmt_save_reply, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* IRCv3 replies: load all for a channel (replies carry channel_id
	 * directly; rows with NULL channel_id are pre-migration orphans whose
	 * message is gone, so never loading them is correct) */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT msgid, target_msgid, target_nick, target_preview "
		"FROM replies WHERE channel_id = ?",
		-1, &sdb->stmt_load_reply, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* IRCv3 replies: load one by msgid (for re-materialization); channel-scoped */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT target_msgid, target_nick, target_preview "
		"FROM replies WHERE channel_id = ? AND msgid = ?",
		-1, &sdb->stmt_load_reply_by_msgid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Echo-message: update pending placeholder msgid to real msgid */
	rc = sqlite3_prepare_v2 (sdb->db,
		"UPDATE messages SET msgid = ?1 WHERE channel_id = ?2 AND msgid = ?3",
		-1, &sdb->stmt_update_pending, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Redact: mark a message as redacted by msgid (channel-scoped so a
	 * multi-target copy in another channel is not redacted collaterally) */
	rc = sqlite3_prepare_v2 (sdb->db,
		"UPDATE messages SET redacted_by = ?, redact_reason = ?, redact_time = ? "
		"WHERE channel_id = ? AND msgid = ?",
		-1, &sdb->stmt_redact, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Virtual scrollback: total message count */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT COUNT(*), IFNULL(MAX(timestamp), 0) FROM messages WHERE channel_id = ?",
		-1, &sdb->stmt_count, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Virtual scrollback: load a window of entries by position.
	 * ORDER BY (timestamp, id) for deterministic, chronological ordering. */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT id, timestamp, msgid, text, redacted_by, redact_reason, redact_time, "
		"is_user_msg "
		"FROM messages WHERE channel_id = ? ORDER BY timestamp ASC, id ASC LIMIT ? OFFSET ?",
		-1, &sdb->stmt_load_range, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Keyset variants of load_range: seek from a known (timestamp, id)
	 * anchor (inclusive) and only OFFSET the short remaining distance.
	 * Same column list and (timestamp, id) ordering as stmt_load_range;
	 * the backward one is walked in reverse and the result flipped. */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT id, timestamp, msgid, text, redacted_by, redact_reason, redact_time, "
		"is_user_msg "
		"FROM messages WHERE channel_id = ?1 AND "
		"(timestamp > ?2 OR (timestamp = ?2 AND id >= ?3)) "
		"ORDER BY timestamp ASC, id ASC LIMIT ?4 OFFSET ?5",
		-1, &sdb->stmt_load_range_fwd, NULL);
	if (rc != SQLITE_OK) goto fail;
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT id, timestamp, msgid, text, redacted_by, redact_reason, redact_time, "
		"is_user_msg "
		"FROM messages WHERE channel_id = ?1 AND "
		"(timestamp < ?2 OR (timestamp = ?2 AND id <= ?3)) "
		"ORDER BY timestamp DESC, id DESC LIMIT ?4 OFFSET ?5",
		-1, &sdb->stmt_load_range_bwd, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* index_of_rowid via anchors: the row's own key, then the number of
	 * rows in the half-open span (lo, hi] between an anchor and it. */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT timestamp FROM messages WHERE id = ?",
		-1, &sdb->stmt_row_key, NULL);
	if (rc != SQLITE_OK) goto fail;
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT COUNT(*) FROM messages WHERE channel_id = ?1 AND "
		"(timestamp > ?2 OR (timestamp = ?2 AND id > ?3)) AND "
		"(timestamp < ?4 OR (timestamp = ?4 AND id <= ?5))",
		-1, &sdb->stmt_count_span, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Virtual scrollback: maximum row ID for a channel */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT MAX(id) FROM messages WHERE channel_id = ?",
		-1, &sdb->stmt_max_rowid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Virtual scrollback: positional index of a specific row ID.
	 * Must match load_range ordering: (timestamp, id) ASC.
	 * Counts entries strictly before the target in chronological order. */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT COUNT(*) FROM messages WHERE channel_id = ?1 AND "
		"(timestamp < (SELECT timestamp FROM messages WHERE id = ?2) OR "
		" (timestamp = (SELECT timestamp FROM messages WHERE id = ?2) AND id < ?2))",
		-1, &sdb->stmt_index_of_rowid, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Virtual scrollback: search message text */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT id, text FROM messages WHERE channel_id = ? AND text LIKE ? "
		"ORDER BY timestamp ASC, id ASC",
		-1, &sdb->stmt_search_text, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Gap ledger: all gaps for a channel, chronological */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT id, start_ts, start_msgid, end_ts, end_msgid, state, "
		"attempts, last_attempt FROM gaps WHERE channel_id = ? "
		"ORDER BY start_ts ASC",
		-1, &sdb->stmt_gap_list, NULL);
	if (rc != SQLITE_OK) goto fail;

	/* Gap ledger: ordinal of a gap's end bound in load_range order */
	rc = sqlite3_prepare_v2 (sdb->db,
		"SELECT COUNT(*) FROM messages WHERE channel_id = ?1 AND timestamp < ?2",
		-1, &sdb->stmt_gap_ordinal, NULL);
	if (rc != SQLITE_OK) goto fail;

	if (sdb->fts)
	{
		if (sqlite3_prepare_v2 (sdb->db,
			"INSERT INTO messages_fts (rowid, text, channel_id) VALUES (?1, ?2, ?3)",
			-1, &sdb->stmt_fts_insert, NULL) != SQLITE_OK ||
		    sqlite3_prepare_v2 (sdb->db,
			"DELETE FROM messages_fts WHERE rowid = ?1",
			-1, &sdb->stmt_fts_delete, NULL) != SQLITE_OK ||
		    sqlite3_prepare_v2 (sdb->db,
			"SELECT 1 FROM messages_fts WHERE rowid = ?1",
			-1, &sdb->stmt_fts_exists, NULL) != SQLITE_OK ||
		    sqlite3_prepare_v2 (sdb->db,
			"SELECT m.id, m.text FROM messages_fts JOIN messages m ON m.id = messages_fts.rowid "
			"WHERE messages_fts.channel_id = ?1 AND messages_fts MATCH ?2 "
			"ORDER BY m.timestamp ASC, m.id ASC",
			-1, &sdb->stmt_fts_search, NULL) != SQLITE_OK ||
		    sqlite3_prepare_v2 (sdb->db,
			"SELECT id, text FROM messages WHERE channel_id = ?1 AND id > ?2 AND id <= ?3 "
			"ORDER BY id ASC LIMIT ?4",
			-1, &sdb->stmt_fts_backfill, NULL) != SQLITE_OK)
		{
			g_warning ("scrollback: FTS5 statements unavailable (%s); search falls back to scanning",
			           sqlite3_errmsg (sdb->db));
			sdb->fts = FALSE;
		}
	}

	return TRUE;

fail:
	g_warning ("Failed to prepare scrollback statement: %s", sqlite3_errmsg (sdb->db));
	return FALSE;
}

static void
finalize_statements (scrollback_db *sdb)
{
	if (sdb->stmt_channel_insert) sqlite3_finalize (sdb->stmt_channel_insert);
	if (sdb->stmt_channel_lookup) sqlite3_finalize (sdb->stmt_channel_lookup);
	if (sdb->stmt_insert) sqlite3_finalize (sdb->stmt_insert);
	if (sdb->stmt_load) sqlite3_finalize (sdb->stmt_load);
	if (sdb->stmt_newest_msgid) sqlite3_finalize (sdb->stmt_newest_msgid);
	if (sdb->stmt_global_newest_msgid) sqlite3_finalize (sdb->stmt_global_newest_msgid);
	if (sdb->stmt_oldest_msgid) sqlite3_finalize (sdb->stmt_oldest_msgid);
	if (sdb->stmt_newest_time) sqlite3_finalize (sdb->stmt_newest_time);
	if (sdb->stmt_has_msgid) sqlite3_finalize (sdb->stmt_has_msgid);
	if (sdb->stmt_clear) sqlite3_finalize (sdb->stmt_clear);
	if (sdb->stmt_save_reaction) sqlite3_finalize (sdb->stmt_save_reaction);
	if (sdb->stmt_remove_reaction) sqlite3_finalize (sdb->stmt_remove_reaction);
	if (sdb->stmt_load_reactions) sqlite3_finalize (sdb->stmt_load_reactions);
	if (sdb->stmt_load_reactions_by_msgid) sqlite3_finalize (sdb->stmt_load_reactions_by_msgid);
	if (sdb->stmt_save_reply) sqlite3_finalize (sdb->stmt_save_reply);
	if (sdb->stmt_load_reply) sqlite3_finalize (sdb->stmt_load_reply);
	if (sdb->stmt_load_reply_by_msgid) sqlite3_finalize (sdb->stmt_load_reply_by_msgid);
	if (sdb->stmt_update_pending) sqlite3_finalize (sdb->stmt_update_pending);
	if (sdb->stmt_redact) sqlite3_finalize (sdb->stmt_redact);
	if (sdb->stmt_count) sqlite3_finalize (sdb->stmt_count);
	if (sdb->stmt_load_range) sqlite3_finalize (sdb->stmt_load_range);
	if (sdb->stmt_load_range_fwd) sqlite3_finalize (sdb->stmt_load_range_fwd);
	if (sdb->stmt_load_range_bwd) sqlite3_finalize (sdb->stmt_load_range_bwd);
	if (sdb->stmt_row_key) sqlite3_finalize (sdb->stmt_row_key);
	if (sdb->stmt_fts_insert) sqlite3_finalize (sdb->stmt_fts_insert);
	if (sdb->stmt_fts_delete) sqlite3_finalize (sdb->stmt_fts_delete);
	if (sdb->stmt_fts_exists) sqlite3_finalize (sdb->stmt_fts_exists);
	if (sdb->stmt_fts_search) sqlite3_finalize (sdb->stmt_fts_search);
	if (sdb->stmt_fts_backfill) sqlite3_finalize (sdb->stmt_fts_backfill);
	if (sdb->fts_idle) { g_source_remove (sdb->fts_idle); sdb->fts_idle = 0; }
	if (sdb->fts_state) g_hash_table_destroy (sdb->fts_state);
	if (sdb->fts_queue) g_queue_free_full (sdb->fts_queue, g_free);
	if (sdb->stmt_count_span) sqlite3_finalize (sdb->stmt_count_span);
	if (sdb->chan_cache) g_hash_table_destroy (sdb->chan_cache);
	if (sdb->stmt_max_rowid) sqlite3_finalize (sdb->stmt_max_rowid);
	if (sdb->stmt_index_of_rowid) sqlite3_finalize (sdb->stmt_index_of_rowid);
	if (sdb->stmt_search_text) sqlite3_finalize (sdb->stmt_search_text);
	if (sdb->stmt_gap_list) sqlite3_finalize (sdb->stmt_gap_list);
	if (sdb->stmt_gap_ordinal) sqlite3_finalize (sdb->stmt_gap_ordinal);
	if (sdb->channel_id_cache) g_hash_table_destroy (sdb->channel_id_cache);
}

/* Sentinel value stored in open_dbs to indicate a previously-failed open.
 * Prevents retrying (and re-warning) for every channel on the same network. */
#define SCROLLBACK_FAILED_SENTINEL ((scrollback_db *)(gsize)1)

typedef enum {
	SB_INTEGRITY_OK,
	SB_INTEGRITY_CORRUPT,	/* quick_check ran and reported malformation */
	SB_INTEGRITY_ERROR	/* indeterminate (busy / I/O) — do NOT recreate */
} sb_integrity;

/* Check database integrity.  Only SB_INTEGRITY_CORRUPT may trigger the
 * backup-and-recreate path; transient errors must never cost history. */
static sb_integrity
scrollback_check_integrity (sqlite3 *db)
{
	sqlite3_stmt *stmt;
	int rc;
	sb_integrity result;

	rc = sqlite3_prepare_v2 (db, "PRAGMA quick_check(1)", -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB)
			? SB_INTEGRITY_CORRUPT : SB_INTEGRITY_ERROR;

	rc = sqlite3_step (stmt);
	if (rc == SQLITE_ROW)
	{
		const char *text = (const char *)sqlite3_column_text (stmt, 0);
		result = (text && strcmp (text, "ok") == 0)
			? SB_INTEGRITY_OK : SB_INTEGRITY_CORRUPT;
	}
	else
		result = (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB)
			? SB_INTEGRITY_CORRUPT : SB_INTEGRITY_ERROR;

	sqlite3_finalize (stmt);
	return result;
}

/* Back up a corrupt DB file (together with its -wal sidecar) by renaming
 * it with a timestamp suffix.  Returns FALSE if the path is not safe to
 * recreate a database at (backup failed, or a stale -wal is stranded). */
static gboolean
scrollback_backup_corrupt (const char *path)
{
	char *backup_path;
	gboolean ok;

	backup_path = g_strdup_printf ("%s.corrupt.%" G_GINT64_FORMAT, path, (gint64)time (NULL));
	ok = zstd_vfs_backup_db (path, backup_path) == 0;
	if (!ok)
		g_warning ("Failed to move corrupt scrollback DB aside: %s", path);
	g_free (backup_path);
	return ok;
}

scrollback_db *
scrollback_open (const char *network)
{
	static gboolean vfs_registered = FALSE;
	scrollback_db *sdb;
	char *path;
	int rc;
	gboolean dirty_exit;

	if (!network || !network[0])
		return NULL;

	if (!vfs_registered)
	{
		zstd_vfs_register ("zstd");
		vfs_registered = TRUE;
	}

	/* Ensure cache hash table exists */
	if (!open_dbs)
		open_dbs = g_hash_table_new (g_str_hash, g_str_equal);

	/* Check if already open (or previously failed) */
	{
		gpointer val;
		if (g_hash_table_lookup_extended (open_dbs, network, NULL, &val))
		{
			if (val == SCROLLBACK_FAILED_SENTINEL)
				return NULL;	/* previously failed, don't retry */
			return (scrollback_db *)val;
		}
	}

	if (!ensure_scrollback_dir ())
		return NULL;

	sdb = g_new0 (scrollback_db, 1);
	sdb->network = g_strdup (network);

	path = get_db_path (network);

	/* Clean-shutdown fast path.  The outer DB runs in WAL mode; on a
	 * clean close SQLite checkpoints and deletes the -wal sidecar, so a
	 * lingering -wal is itself the marker of an unclean previous exit
	 * (docs/design/2026-08-12-outer-wal-design.md).  Only pay the
	 * whole-DB quick_check when that marker is present: the scan
	 * decompresses essentially every page through the zstd VFS and was
	 * the largest fixed cost on the connect path (~242 ms warm on a
	 * 190k-row DB; disk-bound and worse on a cold cache).  Sampled
	 * before sqlite3_open_v2 so the file state still reflects the
	 * previous session, not this connection. */
	{
		char *wal_path = g_strdup_printf ("%s-wal", path);
		dirty_exit = g_file_test (wal_path, G_FILE_TEST_EXISTS);
		g_free (wal_path);
	}

	rc = sqlite3_open_v2 (path, &sdb->db,
	                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "zstd");

	if (rc != SQLITE_OK)
	{
		g_warning ("Failed to open scrollback database %s: %s", path, sqlite3_errmsg (sdb->db));
		sqlite3_close (sdb->db);
		g_free (path);
		g_free (sdb->network);
		g_hash_table_insert (open_dbs, g_strdup (network), SCROLLBACK_FAILED_SENTINEL);
		g_free (sdb);
		return NULL;
	}

	if (!dirty_exit)
	{
		poxchat_timing_log ("scrollback_open %s: clean shutdown — integrity check skipped",
		                    network);
	}
	else
	/* Check for corruption before using the database */
	{
		gint64 t_integ = g_get_monotonic_time ();
		sb_integrity integ;

		g_message ("scrollback: unclean previous exit for %s — verifying integrity",
		           network);
		integ = scrollback_check_integrity (sdb->db);

		poxchat_timing_log ("scrollback_open %s: integrity quick_check %.1f ms",
		                    network, (g_get_monotonic_time () - t_integ) / 1000.0);

		if (integ == SB_INTEGRITY_ERROR)
		{
			g_warning ("Scrollback database for %s unreadable right now "
			           "(busy or I/O error) — will retry next session", network);
			sqlite3_close (sdb->db);
			g_free (path);
			g_free (sdb->network);
			g_hash_table_insert (open_dbs, g_strdup (network), SCROLLBACK_FAILED_SENTINEL);
			g_free (sdb);
			return NULL;
		}

		if (integ == SB_INTEGRITY_CORRUPT)
		{
			g_warning ("Scrollback database corrupt for %s — backing up and recreating", network);
			sqlite3_close (sdb->db);
			if (!scrollback_backup_corrupt (path))
			{
				/* The path still holds the old DB (or a stranded -wal):
				 * creating a fresh database here would replay stale WAL
				 * into it.  Treat like a transient error — retry next
				 * session. */
				g_free (path);
				g_free (sdb->network);
				g_hash_table_insert (open_dbs, g_strdup (network), SCROLLBACK_FAILED_SENTINEL);
				g_free (sdb);
				return NULL;
			}

			/* Re-open — creates a fresh empty database */
			rc = sqlite3_open_v2 (path, &sdb->db,
			                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "zstd");
			if (rc != SQLITE_OK)
			{
				g_warning ("Failed to recreate scrollback database %s: %s", path, sqlite3_errmsg (sdb->db));
				sqlite3_close (sdb->db);
				g_free (path);
				g_free (sdb->network);
				g_hash_table_insert (open_dbs, g_strdup (network), SCROLLBACK_FAILED_SENTINEL);
				g_free (sdb);
				return NULL;
			}
		}
	}

	g_free (path);

	{
		gint64 t_init = g_get_monotonic_time ();
		if (!init_database (sdb))
		{
			sqlite3_close (sdb->db);
			g_free (sdb->network);
			g_hash_table_insert (open_dbs, g_strdup (network), SCROLLBACK_FAILED_SENTINEL);
			g_free (sdb);
			return NULL;
		}
		poxchat_timing_log ("scrollback_open %s: init/migrations %.1f ms",
		                    network, (g_get_monotonic_time () - t_init) / 1000.0);
	}

	if (!prepare_statements (sdb))
	{
		sqlite3_close (sdb->db);
		g_free (sdb->network);
		g_hash_table_insert (open_dbs, g_strdup (network), SCROLLBACK_FAILED_SENTINEL);
		g_free (sdb);
		return NULL;
	}

	/* Add to open databases */
	g_hash_table_insert (open_dbs, sdb->network, sdb);

	return sdb;
}

void
scrollback_db_close (scrollback_db *db)
{
	if (!db)
		return;

	if (open_dbs)
		g_hash_table_remove (open_dbs, db->network);

	finalize_statements (db);
	sqlite3_close (db->db);
	g_free (db->network);
	g_free (db);
}

gint64
scrollback_db_save (scrollback_db *db, const char *channel,
                 time_t timestamp, const char *msgid, const char *text,
                 gboolean is_user_msg)
{
	int rc;

	if (!db || !channel || !text)
		return -1;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return -1;

	sqlite3_reset (db->stmt_insert);
	sqlite3_bind_text (db->stmt_insert, 1, channel, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64 (db->stmt_insert, 2, channel_id);
	sqlite3_bind_int64 (db->stmt_insert, 3, (sqlite3_int64)timestamp);

	if (msgid && msgid[0])
		sqlite3_bind_text (db->stmt_insert, 4, msgid, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null (db->stmt_insert, 4);

	sqlite3_bind_text (db->stmt_insert, 5, text, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int (db->stmt_insert, 6, is_user_msg ? 1 : 0);

	rc = sqlite3_step (db->stmt_insert);

	if (rc != SQLITE_DONE)
	{
		/* SQLITE_CONSTRAINT is expected for duplicate msgid - not an error */
		if (rc != SQLITE_CONSTRAINT)
			g_warning ("scrollback_db_save failed: %s", sqlite3_errmsg (db->db));
		return -1;
	}

	/* INSERT OR IGNORE returns SQLITE_DONE even when the row is silently
	 * dropped due to a UNIQUE constraint conflict on msgid.  In that case
	 * sqlite3_changes() is 0 and sqlite3_last_insert_rowid() still returns
	 * the rowid of the most recent *actual* insert (not the duplicate that
	 * was ignored).  Returning that stale rowid would cause the caller to
	 * assign a stale entry_id to a brand-new entry, producing duplicate
	 * (entry_id, stamp) pairs in xtext's tree and a flood of orphans.
	 * Treat the duplicate-ignored case as "row not stored". */
	if (sqlite3_changes (db->db) == 0)
		return -1;

	chan_cache_note_insert (db, channel_id, (gint64) timestamp);

	{
		gint64 rowid = (gint64) sqlite3_last_insert_rowid (db->db);
		/* Channels never scheduled stay unindexed until their backfill
		 * runs; it will pick this row up then. */
		if (db->fts && fts_state_get (db, channel_id) != FTS_UNINDEXED)
			fts_index_row (db, rowid, channel_id, text);
		return rowid;
	}
}

/* Drop unconfirmed echo-message rows left behind by a previous session.
 *
 * A "pending:<label>" msgid is the placeholder written when we send a
 * message and have not seen the server echo yet; one that survived a
 * restart will never be confirmed, so it must go before anything reads
 * the channel's newest msgid or counts its rows.  This used to be a side
 * effect of scrollback_db_load, which the deferred replay fill no longer
 * calls — scrollback_load () runs it directly now. */
void
scrollback_purge_pending (scrollback_db *db, const char *channel)
{
	gint64 channel_id;
	char *errmsg = NULL;
	char *sql;

	if (!db || !channel)
		return;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return;

	if (db->fts)
	{
		sql = sqlite3_mprintf (
			"DELETE FROM messages_fts WHERE rowid IN (SELECT id FROM messages "
			"WHERE channel_id = %lld AND msgid LIKE 'pending:%%')",
			(long long)channel_id);
		sqlite3_exec (db->db, sql, NULL, NULL, NULL);
		sqlite3_free (sql);
	}
	sql = sqlite3_mprintf (
		"DELETE FROM messages WHERE channel_id = %lld AND msgid LIKE 'pending:%%'",
		(long long)channel_id);
	sqlite3_exec (db->db, sql, NULL, NULL, &errmsg);
	if (errmsg)
	{
		g_warning ("Failed to purge pending entries: %s", errmsg);
		sqlite3_free (errmsg);
	}
	else if (sqlite3_changes (db->db) > 0)
		chan_cache_invalidate (db, channel_id);
	sqlite3_free (sql);
}

GSList *
scrollback_db_load (scrollback_db *db, const char *channel, int limit)
{
	GSList *list = NULL;
	int rc;

	if (!db || !channel)
		return NULL;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return NULL;

	if (limit <= 0)
		limit = 500; /* Default */

	scrollback_purge_pending (db, channel);

	sqlite3_reset (db->stmt_load);
	sqlite3_bind_int64 (db->stmt_load, 1, channel_id);
	sqlite3_bind_int (db->stmt_load, 2, limit);

	while ((rc = sqlite3_step (db->stmt_load)) == SQLITE_ROW)
	{
		scrollback_msg *msg = g_new0 (scrollback_msg, 1);

		msg->id = sqlite3_column_int64 (db->stmt_load, 0);
		msg->channel = g_strdup (channel);
		msg->timestamp = (time_t)sqlite3_column_int64 (db->stmt_load, 2);

		const char *msgid_text = (const char *)sqlite3_column_text (db->stmt_load, 3);
		msg->msgid = msgid_text ? g_strdup (msgid_text) : NULL;

		msg->text = g_strdup ((const char *)sqlite3_column_text (db->stmt_load, 4));

		{
			const char *rby = (const char *)sqlite3_column_text (db->stmt_load, 5);
			const char *rreason = (const char *)sqlite3_column_text (db->stmt_load, 6);
			msg->redacted_by = rby ? g_strdup (rby) : NULL;
			msg->redact_reason = rreason ? g_strdup (rreason) : NULL;
			msg->redact_time = (time_t)sqlite3_column_int64 (db->stmt_load, 7);
		}
		msg->is_user_msg = sqlite3_column_int (db->stmt_load, 8) ? TRUE : FALSE;

		/* Prepend to get correct order (query returns DESC, we want ASC) */
		list = g_slist_prepend (list, msg);
	}

	if (rc != SQLITE_DONE)
		g_warning ("Error loading scrollback: %s", sqlite3_errmsg (db->db));

	return list;
}

char *
scrollback_get_newest_msgid (scrollback_db *db, const char *channel)
{
	char *msgid = NULL;
	int rc;

	if (!db || !channel)
		return NULL;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return NULL;

	sqlite3_reset (db->stmt_newest_msgid);
	sqlite3_bind_int64 (db->stmt_newest_msgid, 1, channel_id);

	rc = sqlite3_step (db->stmt_newest_msgid);
	if (rc == SQLITE_ROW)
	{
		const char *text = (const char *)sqlite3_column_text (db->stmt_newest_msgid, 0);
		if (text)
			msgid = g_strdup (text);
	}

	return msgid;
}

char *
scrollback_get_global_newest_msgid (scrollback_db *db)
{
	char *msgid = NULL;

	if (!db || !db->stmt_global_newest_msgid)
		return NULL;

	sqlite3_reset (db->stmt_global_newest_msgid);
	if (sqlite3_step (db->stmt_global_newest_msgid) == SQLITE_ROW)
	{
		const char *text = (const char *) sqlite3_column_text (db->stmt_global_newest_msgid, 0);
		if (text)
			msgid = g_strdup (text);
	}
	sqlite3_reset (db->stmt_global_newest_msgid);
	return msgid;
}

char *
scrollback_get_oldest_msgid (scrollback_db *db, const char *channel)
{
	char *msgid = NULL;
	int rc;

	if (!db || !channel)
		return NULL;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return NULL;

	sqlite3_reset (db->stmt_oldest_msgid);
	sqlite3_bind_int64 (db->stmt_oldest_msgid, 1, channel_id);

	rc = sqlite3_step (db->stmt_oldest_msgid);
	if (rc == SQLITE_ROW)
	{
		const char *text = (const char *)sqlite3_column_text (db->stmt_oldest_msgid, 0);
		if (text)
			msgid = g_strdup (text);
	}

	return msgid;
}

time_t
scrollback_get_newest_time (scrollback_db *db, const char *channel)
{
	time_t timestamp = 0;
	int rc;

	if (!db || !channel)
		return 0;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return 0;

	sqlite3_reset (db->stmt_newest_time);
	sqlite3_bind_int64 (db->stmt_newest_time, 1, channel_id);

	rc = sqlite3_step (db->stmt_newest_time);
	if (rc == SQLITE_ROW)
		timestamp = (time_t)sqlite3_column_int64 (db->stmt_newest_time, 0);

	return timestamp;
}

gboolean
scrollback_has_msgid (scrollback_db *db, const char *channel,
                      const char *msgid, time_t timestamp)
{
	int rc;
	gint64 channel_id;

	if (!db || !channel || !msgid || !msgid[0])
		return FALSE;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return FALSE;

	sqlite3_reset (db->stmt_has_msgid);
	sqlite3_bind_int64 (db->stmt_has_msgid, 1, channel_id);
	sqlite3_bind_text (db->stmt_has_msgid, 2, msgid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64 (db->stmt_has_msgid, 3, (sqlite3_int64) timestamp);

	rc = sqlite3_step (db->stmt_has_msgid);
	return (rc == SQLITE_ROW);
}

/* Delete a single message row by rowid.  Used to drop a pending
 * placeholder that lost the echo-vs-chathistory race (the replayed row
 * with the real msgid survives).  Pending rows have no reaction/reply
 * dependents — those are keyed by real msgids. */
gboolean
scrollback_delete_by_rowid (scrollback_db *db, gint64 rowid)
{
	sqlite3_stmt *stmt;
	int rc;

	if (!db || rowid <= 0)
		return FALSE;

	if (sqlite3_prepare_v2 (db->db,
		"DELETE FROM messages WHERE id = ?",
		-1, &stmt, NULL) != SQLITE_OK)
		return FALSE;

	sqlite3_bind_int64 (stmt, 1, rowid);
	rc = sqlite3_step (stmt);
	sqlite3_finalize (stmt);
	if (rc == SQLITE_DONE && sqlite3_changes (db->db) > 0)
	{
		/* No channel in hand; ordinals after the row shifted. */
		chan_cache_invalidate_all (db);
		fts_unindex_row (db, rowid);
		return TRUE;
	}
	return FALSE;
}

gboolean
scrollback_update_pending_msgid (scrollback_db *db, const char *channel,
                                  const char *pending_msgid, const char *real_msgid)
{
	int rc;

	gint64 channel_id;

	if (!db || !db->stmt_update_pending || !channel || !pending_msgid || !real_msgid)
		return FALSE;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return FALSE;

	sqlite3_reset (db->stmt_update_pending);
	sqlite3_bind_text (db->stmt_update_pending, 1, real_msgid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64 (db->stmt_update_pending, 2, channel_id);
	sqlite3_bind_text (db->stmt_update_pending, 3, pending_msgid, -1, SQLITE_TRANSIENT);

	rc = sqlite3_step (db->stmt_update_pending);
	return (rc == SQLITE_DONE && sqlite3_changes (db->db) > 0);
}

gboolean
scrollback_redact_message (scrollback_db *db, const char *channel,
                           const char *msgid,
                           const char *redacted_by, const char *reason,
                           time_t redact_time)
{
	int rc;
	gint64 channel_id;

	if (!db || !db->stmt_redact || !channel || !msgid)
		return FALSE;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return FALSE;

	sqlite3_reset (db->stmt_redact);
	sqlite3_bind_text (db->stmt_redact, 1, redacted_by ? redacted_by : "unknown", -1, SQLITE_TRANSIENT);
	if (reason)
		sqlite3_bind_text (db->stmt_redact, 2, reason, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null (db->stmt_redact, 2);
	sqlite3_bind_int64 (db->stmt_redact, 3, (sqlite3_int64) redact_time);
	sqlite3_bind_int64 (db->stmt_redact, 4, channel_id);
	sqlite3_bind_text (db->stmt_redact, 5, msgid, -1, SQLITE_TRANSIENT);

	rc = sqlite3_step (db->stmt_redact);
	return (rc == SQLITE_DONE && sqlite3_changes (db->db) > 0);
}

void
scrollback_clear (scrollback_db *db, const char *channel)
{
	gint64 channel_id;
	sqlite3_stmt *stmt;

	if (!db || !channel)
		return;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return;

	/* Purge dependent rows first.  Both tables now carry channel_id, but
	 * pre-migration rows can have it NULL — match those through the
	 * channel's msgids as a second leg.  Without this, cleared channels
	 * accumulate orphaned reaction/reply rows that re-attach stale state
	 * to re-fetched msgids. */
	if (sqlite3_prepare_v2 (db->db,
		"DELETE FROM replies WHERE channel_id = ?1 OR "
		"(channel_id IS NULL AND msgid IN "
		"(SELECT msgid FROM messages WHERE channel_id = ?1 AND msgid IS NOT NULL))",
		-1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_int64 (stmt, 1, channel_id);
		sqlite3_step (stmt);
		sqlite3_finalize (stmt);
	}
	if (sqlite3_prepare_v2 (db->db,
		"DELETE FROM reactions WHERE channel_id = ?1 OR "
		"(channel_id IS NULL AND target_msgid IN "
		"(SELECT msgid FROM messages WHERE channel_id = ?1 AND msgid IS NOT NULL))",
		-1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_int64 (stmt, 1, channel_id);
		sqlite3_step (stmt);
		sqlite3_finalize (stmt);
	}

	chan_cache_invalidate (db, channel_id);
	if (db->fts)
	{
		char *sql = sqlite3_mprintf ("DELETE FROM messages_fts WHERE channel_id = %lld",
		                             (long long) channel_id);
		sqlite3_exec (db->db, sql, NULL, NULL, NULL);
		sqlite3_free (sql);
	}
	sqlite3_reset (db->stmt_clear);
	sqlite3_bind_int64 (db->stmt_clear, 1, channel_id);
	sqlite3_step (db->stmt_clear);
}

void
scrollback_msg_free (scrollback_msg *msg)
{
	if (!msg)
		return;

	g_free (msg->channel);
	g_free (msg->msgid);
	g_free (msg->text);
	g_free (msg->redacted_by);
	g_free (msg->redact_reason);
	g_free (msg);
}

void
scrollback_msg_list_free (GSList *list)
{
	g_slist_free_full (list, (GDestroyNotify)scrollback_msg_free);
}

/* Migration from old text-based scrollback */

static char *
get_old_scrollback_path (const char *network, const char *channel)
{
	char *dir = get_scrollback_dir ();
	char *safe_channel = g_strdup (channel);
	char *path;

	/* Sanitize channel name for filename */
	for (char *p = safe_channel; *p; p++)
	{
		if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
		    *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|')
			*p = '_';
	}

	path = g_build_filename (dir, network, safe_channel, NULL);
	g_free (dir);

	char *full_path = g_strdup_printf ("%s.txt", path);
	g_free (path);
	g_free (safe_channel);

	return full_path;
}

int
scrollback_migrate (scrollback_db *db, const char *network, const char *channel)
{
	char *old_path;
	GFile *file;
	GInputStream *stream;
	GDataInputStream *istream;
	char *line;
	int count = 0;
	char *errmsg = NULL;

	if (!db || !network || !channel)
		return -1;

	old_path = get_old_scrollback_path (network, channel);

	if (!g_file_test (old_path, G_FILE_TEST_EXISTS))
	{
		g_free (old_path);
		return 0; /* No old file to migrate */
	}

	file = g_file_new_for_path (old_path);
	stream = G_INPUT_STREAM (g_file_read (file, NULL, NULL));

	if (!stream)
	{
		g_object_unref (file);
		g_free (old_path);
		return -1;
	}

	istream = g_data_input_stream_new (stream);
	g_data_input_stream_set_newline_type (istream, G_DATA_STREAM_NEWLINE_TYPE_ANY);
	g_object_unref (stream);

	/* Begin transaction for batch insert */
	sqlite3_exec (db->db, "BEGIN TRANSACTION", NULL, NULL, &errmsg);
	if (errmsg)
	{
		g_warning ("Migration transaction begin failed: %s", errmsg);
		sqlite3_free (errmsg);
	}

	while ((line = g_data_input_stream_read_line_utf8 (istream, NULL, NULL, NULL)) != NULL)
	{
		time_t timestamp = 0;
		const char *text = line;

		/* Parse old format: T <timestamp> <text> */
		if (line[0] == 'T' && line[1] == ' ')
		{
			if (sizeof (time_t) == 4)
				timestamp = strtoul (line + 2, NULL, 10);
			else
				timestamp = g_ascii_strtoull (line + 2, NULL, 10);

			text = strchr (line + 3, ' ');
			if (text)
				text++; /* Skip the space */
			else
				text = "";
		}

		if (timestamp > 0 && text && text[0])
		{
			/* Insert without msgid (old format doesn't have them);
			 * is_user_msg unknown for legacy imports — default to FALSE. */
			if (scrollback_db_save (db, channel, timestamp, NULL, text, FALSE) >= 0)
				count++;
		}

		g_free (line);
	}

	/* Commit transaction */
	sqlite3_exec (db->db, "COMMIT", NULL, NULL, &errmsg);
	if (errmsg)
	{
		g_warning ("Migration transaction commit failed: %s", errmsg);
		sqlite3_free (errmsg);
	}

	g_object_unref (istream);
	g_object_unref (file);

	/* Rename old file to .migrated */
	if (count > 0)
	{
		char *migrated_path = g_strdup_printf ("%s.migrated", old_path);
		g_rename (old_path, migrated_path);
		g_free (migrated_path);
	}

	g_free (old_path);

	return count;
}

void
scrollback_init (void)
{
	if (!open_dbs)
		open_dbs = g_hash_table_new (g_str_hash, g_str_equal);
}

static void
close_db_callback (gpointer key, gpointer value, gpointer user_data)
{
	scrollback_db *db = value;
	if (db == SCROLLBACK_FAILED_SENTINEL)
	{
		g_free (key);	/* network name was g_strdup'd for sentinel entries */
		return;
	}
	finalize_statements (db);
	sqlite3_close (db->db);
	g_free (db->network);
	g_free (db);
}

void
scrollback_begin_transaction (scrollback_db *db)
{
	if (!db || !db->db)
		return;
	db->transaction_depth++;
	if (db->transaction_depth == 1)
		sqlite3_exec (db->db, "BEGIN TRANSACTION", NULL, NULL, NULL);
}

void
scrollback_commit_transaction (scrollback_db *db)
{
	if (!db || !db->db || db->transaction_depth <= 0)
		return;
	db->transaction_depth--;
	if (db->transaction_depth == 0)
	{
		char *errmsg = NULL;
		int rc = sqlite3_exec (db->db, "COMMIT", NULL, NULL, &errmsg);
		if (rc != SQLITE_OK)
		{
			g_warning ("Scrollback commit failed for %s: %s",
			           db->network ? db->network : "?",
			           errmsg ? errmsg : "unknown error");
			sqlite3_free (errmsg);
		}
	}
}

void
scrollback_shutdown (void)
{
	if (open_dbs)
	{
		g_hash_table_foreach (open_dbs, close_db_callback, NULL);
		g_hash_table_destroy (open_dbs);
		open_dbs = NULL;
	}
	zstd_vfs_shutdown ();
}

/* IRCv3 reactions: save a reaction to scrollback */
gboolean
scrollback_save_reaction (scrollback_db *db, const char *channel,
                          const char *target_msgid, const char *reaction_text,
                          const char *nick, gboolean is_self, time_t timestamp)
{
	int rc;

	gint64 channel_id;

	if (!db || !db->stmt_save_reaction || !channel || !target_msgid ||
	    !reaction_text || !nick)
		return FALSE;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return FALSE;

	sqlite3_reset (db->stmt_save_reaction);
	sqlite3_bind_text (db->stmt_save_reaction, 1, channel, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64 (db->stmt_save_reaction, 2, channel_id);
	sqlite3_bind_text (db->stmt_save_reaction, 3, target_msgid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (db->stmt_save_reaction, 4, reaction_text, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (db->stmt_save_reaction, 5, nick, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int (db->stmt_save_reaction, 6, is_self ? 1 : 0);
	sqlite3_bind_int64 (db->stmt_save_reaction, 7, (sqlite3_int64)timestamp);

	rc = sqlite3_step (db->stmt_save_reaction);
	return rc == SQLITE_DONE;
}

/* IRCv3 reactions: remove a reaction from scrollback */
gboolean
scrollback_remove_reaction (scrollback_db *db, const char *channel,
                            const char *target_msgid,
                            const char *reaction_text, const char *nick)
{
	int rc;
	gint64 channel_id;

	if (!db || !db->stmt_remove_reaction || !channel || !target_msgid ||
	    !reaction_text || !nick)
		return FALSE;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return FALSE;

	sqlite3_reset (db->stmt_remove_reaction);
	sqlite3_bind_int64 (db->stmt_remove_reaction, 1, channel_id);
	sqlite3_bind_text (db->stmt_remove_reaction, 2, target_msgid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (db->stmt_remove_reaction, 3, reaction_text, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (db->stmt_remove_reaction, 4, nick, -1, SQLITE_TRANSIENT);

	rc = sqlite3_step (db->stmt_remove_reaction);
	return rc == SQLITE_DONE;
}

/* IRCv3 reactions: load all reactions for a channel.
 * Returns a GSList of scrollback_reaction* (caller frees with scrollback_reaction_list_free).
 */
GSList *
scrollback_load_reactions (scrollback_db *db, const char *channel)
{
	GSList *list = NULL;
	int rc;

	gint64 channel_id;

	if (!db || !db->stmt_load_reactions || !channel)
		return NULL;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return NULL;

	sqlite3_reset (db->stmt_load_reactions);
	sqlite3_bind_int64 (db->stmt_load_reactions, 1, channel_id);

	while ((rc = sqlite3_step (db->stmt_load_reactions)) == SQLITE_ROW)
	{
		scrollback_reaction *r = g_new0 (scrollback_reaction, 1);
		r->target_msgid = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reactions, 0));
		r->reaction_text = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reactions, 1));
		r->nick = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reactions, 2));
		r->is_self = sqlite3_column_int (db->stmt_load_reactions, 3) != 0;
		list = g_slist_prepend (list, r);
	}

	return g_slist_reverse (list);
}

GSList *
scrollback_load_reactions_by_msgid (scrollback_db *db, const char *channel,
                                    const char *target_msgid)
{
	GSList *list = NULL;
	int rc;
	gint64 channel_id;

	if (!db || !db->stmt_load_reactions_by_msgid || !channel ||
	    !target_msgid || !target_msgid[0])
		return NULL;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return NULL;

	sqlite3_reset (db->stmt_load_reactions_by_msgid);
	sqlite3_bind_int64 (db->stmt_load_reactions_by_msgid, 1, channel_id);
	sqlite3_bind_text (db->stmt_load_reactions_by_msgid, 2, target_msgid,
	                   -1, SQLITE_TRANSIENT);

	while ((rc = sqlite3_step (db->stmt_load_reactions_by_msgid)) == SQLITE_ROW)
	{
		scrollback_reaction *r = g_new0 (scrollback_reaction, 1);
		r->target_msgid = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reactions_by_msgid, 0));
		r->reaction_text = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reactions_by_msgid, 1));
		r->nick = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reactions_by_msgid, 2));
		r->is_self = sqlite3_column_int (db->stmt_load_reactions_by_msgid, 3) != 0;
		list = g_slist_prepend (list, r);
	}

	return g_slist_reverse (list);
}

void
scrollback_reaction_free (scrollback_reaction *r)
{
	if (!r)
		return;
	g_free (r->target_msgid);
	g_free (r->reaction_text);
	g_free (r->nick);
	g_free (r);
}

void
scrollback_reaction_list_free (GSList *list)
{
	g_slist_free_full (list, (GDestroyNotify)scrollback_reaction_free);
}

/* IRCv3 replies: save reply context to scrollback */
gboolean
scrollback_save_reply (scrollback_db *db, const char *channel,
                       const char *msgid,
                       const char *target_msgid, const char *target_nick,
                       const char *target_preview)
{
	int rc;
	gint64 channel_id;

	if (!db || !db->stmt_save_reply || !channel || !msgid || !target_msgid)
		return FALSE;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return FALSE;

	sqlite3_reset (db->stmt_save_reply);
	sqlite3_bind_int64 (db->stmt_save_reply, 1, channel_id);
	sqlite3_bind_text (db->stmt_save_reply, 2, msgid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (db->stmt_save_reply, 3, target_msgid, -1, SQLITE_TRANSIENT);
	if (target_nick)
		sqlite3_bind_text (db->stmt_save_reply, 4, target_nick, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null (db->stmt_save_reply, 4);
	if (target_preview)
		sqlite3_bind_text (db->stmt_save_reply, 5, target_preview, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null (db->stmt_save_reply, 5);

	rc = sqlite3_step (db->stmt_save_reply);
	return rc == SQLITE_DONE;
}

/* IRCv3 replies: load all reply contexts for messages in a channel.
 * Returns a GSList of scrollback_reply* (caller frees with scrollback_reply_list_free).
 */
GSList *
scrollback_load_replies (scrollback_db *db, const char *channel)
{
	GSList *list = NULL;
	int rc;

	gint64 channel_id;

	if (!db || !db->stmt_load_reply || !channel)
		return NULL;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return NULL;

	sqlite3_reset (db->stmt_load_reply);
	sqlite3_bind_int64 (db->stmt_load_reply, 1, channel_id);

	while ((rc = sqlite3_step (db->stmt_load_reply)) == SQLITE_ROW)
	{
		scrollback_reply *r = g_new0 (scrollback_reply, 1);
		r->msgid = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reply, 0));
		r->target_msgid = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reply, 1));
		r->target_nick = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reply, 2));
		r->target_preview = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reply, 3));
		list = g_slist_prepend (list, r);
	}

	return g_slist_reverse (list);
}

/* IRCv3 replies: load one reply by msgid.  Used by virtual-scrollback
 * re-materialization to reattach reply context when an entry that was
 * evicted gets reloaded from the DB. */
scrollback_reply *
scrollback_load_reply_by_msgid (scrollback_db *db, const char *channel,
                                const char *msgid)
{
	scrollback_reply *r;
	int rc;
	gint64 channel_id;

	if (!db || !db->stmt_load_reply_by_msgid || !channel || !msgid || !msgid[0])
		return NULL;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return NULL;

	sqlite3_reset (db->stmt_load_reply_by_msgid);
	sqlite3_bind_int64 (db->stmt_load_reply_by_msgid, 1, channel_id);
	sqlite3_bind_text (db->stmt_load_reply_by_msgid, 2, msgid, -1, SQLITE_TRANSIENT);

	rc = sqlite3_step (db->stmt_load_reply_by_msgid);
	if (rc != SQLITE_ROW)
		return NULL;

	r = g_new0 (scrollback_reply, 1);
	r->msgid = g_strdup (msgid);
	r->target_msgid = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reply_by_msgid, 0));
	r->target_nick = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reply_by_msgid, 1));
	r->target_preview = g_strdup ((const char *)sqlite3_column_text (db->stmt_load_reply_by_msgid, 2));
	return r;
}

void
scrollback_reply_free (scrollback_reply *r)
{
	if (!r)
		return;
	g_free (r->msgid);
	g_free (r->target_msgid);
	g_free (r->target_nick);
	g_free (r->target_preview);
	g_free (r);
}

void
scrollback_reply_list_free (GSList *list)
{
	g_slist_free_full (list, (GDestroyNotify)scrollback_reply_free);
}

/* --- Batched reply/reaction lookup for a window of messages ---
 *
 * Re-materializing a scrolled-in range used to probe replies and
 * reactions once per entry (two statements x hundreds of rows per scroll
 * step).  These build one IN (...) query per chunk of msgids and hand
 * back hash tables the caller consults instead. */

#define SB_IN_CHUNK 400

static GPtrArray *
collect_msgids (GSList *msgs)
{
	GPtrArray *ids = g_ptr_array_new ();
	GSList *it;
	for (it = msgs; it; it = it->next)
	{
		scrollback_msg *m = it->data;
		if (m && m->msgid && m->msgid[0])
			g_ptr_array_add (ids, m->msgid);
	}
	return ids;
}

static char *
build_in_sql (const char *prefix, guint n, const char *suffix)
{
	GString *sql = g_string_new (prefix);
	guint i;
	g_string_append (sql, " IN (");
	for (i = 0; i < n; i++)
		g_string_append (sql, i ? ",?" : "?");
	g_string_append (sql, ")");
	if (suffix)
		g_string_append (sql, suffix);
	return g_string_free (sql, FALSE);
}

static void
reaction_list_free_cb (gpointer data)
{
	scrollback_reaction_list_free ((GSList *) data);
}

GHashTable *
scrollback_load_replies_for_msgs (scrollback_db *db, const char *channel, GSList *msgs)
{
	GHashTable *out;
	GPtrArray *ids;
	gint64 channel_id;
	guint pos;

	if (!db || !channel)
		return NULL;
	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return NULL;

	out = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
	                             (GDestroyNotify) scrollback_reply_free);
	ids = collect_msgids (msgs);

	for (pos = 0; pos < ids->len; pos += SB_IN_CHUNK)
	{
		guint n = MIN (SB_IN_CHUNK, ids->len - pos);
		char *sql = build_in_sql (
			"SELECT msgid, target_msgid, target_nick, target_preview "
			"FROM replies WHERE channel_id = ? AND msgid", n, NULL);
		sqlite3_stmt *stmt = NULL;
		guint i;

		if (sqlite3_prepare_v2 (db->db, sql, -1, &stmt, NULL) == SQLITE_OK)
		{
			sqlite3_bind_int64 (stmt, 1, channel_id);
			for (i = 0; i < n; i++)
				sqlite3_bind_text (stmt, 2 + i, g_ptr_array_index (ids, pos + i),
				                   -1, SQLITE_STATIC);
			while (sqlite3_step (stmt) == SQLITE_ROW)
			{
				scrollback_reply *r = g_new0 (scrollback_reply, 1);
				r->msgid = g_strdup ((const char *) sqlite3_column_text (stmt, 0));
				r->target_msgid = g_strdup ((const char *) sqlite3_column_text (stmt, 1));
				r->target_nick = g_strdup ((const char *) sqlite3_column_text (stmt, 2));
				r->target_preview = g_strdup ((const char *) sqlite3_column_text (stmt, 3));
				g_hash_table_replace (out, r->msgid, r);
			}
		}
		if (stmt)
			sqlite3_finalize (stmt);
		g_free (sql);
	}

	g_ptr_array_free (ids, TRUE);
	return out;
}

GHashTable *
scrollback_load_reactions_for_msgs (scrollback_db *db, const char *channel, GSList *msgs)
{
	GHashTable *out;
	GPtrArray *ids;
	gint64 channel_id;
	guint pos;

	if (!db || !channel)
		return NULL;
	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return NULL;

	out = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
	                             reaction_list_free_cb);
	ids = collect_msgids (msgs);

	for (pos = 0; pos < ids->len; pos += SB_IN_CHUNK)
	{
		guint n = MIN (SB_IN_CHUNK, ids->len - pos);
		char *sql = build_in_sql (
			"SELECT target_msgid, reaction_text, nick, is_self "
			"FROM reactions WHERE channel_id = ? AND target_msgid", n,
			" ORDER BY target_msgid, reaction_text");
		sqlite3_stmt *stmt = NULL;
		guint i;

		if (sqlite3_prepare_v2 (db->db, sql, -1, &stmt, NULL) == SQLITE_OK)
		{
			sqlite3_bind_int64 (stmt, 1, channel_id);
			for (i = 0; i < n; i++)
				sqlite3_bind_text (stmt, 2 + i, g_ptr_array_index (ids, pos + i),
				                   -1, SQLITE_STATIC);
			while (sqlite3_step (stmt) == SQLITE_ROW)
			{
				const char *target = (const char *) sqlite3_column_text (stmt, 0);
				scrollback_reaction *r;
				GSList *lst;
				char *key;

				if (!target)
					continue;
				r = g_new0 (scrollback_reaction, 1);
				r->target_msgid = g_strdup (target);
				r->reaction_text = g_strdup ((const char *) sqlite3_column_text (stmt, 1));
				r->nick = g_strdup ((const char *) sqlite3_column_text (stmt, 2));
				r->is_self = sqlite3_column_int (stmt, 3) != 0;

				/* Steal the list so the table's destroy notify doesn't
				 * free it while we append (ORDER BY keeps rows grouped). */
				if (g_hash_table_lookup_extended (out, target, (gpointer *) &key, (gpointer *) &lst))
				{
					g_hash_table_steal (out, target);
					lst = g_slist_append (lst, r);
					g_hash_table_insert (out, key, lst);
				}
				else
					g_hash_table_insert (out, g_strdup (target), g_slist_append (NULL, r));
			}
		}
		if (stmt)
			sqlite3_finalize (stmt);
		g_free (sql);
	}

	g_ptr_array_free (ids, TRUE);
	return out;
}

/* --- Virtual scrollback query functions --- */

int
scrollback_count (scrollback_db *db, const char *channel)
{
	int count = 0;

	if (!db || !channel)
		return 0;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return 0;

	count = chan_cache_ensure_count (db, channel_id)->count;

	return count;
}

GSList *
scrollback_load_range (scrollback_db *db, const char *channel, int offset, int limit)
{
	GSList *list = NULL;
	sqlite3_stmt *stmt;
	int rc;
	int n = 0;

	if (!db || !channel)
		return NULL;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return NULL;

	if (limit <= 0)
		limit = 500;
	if (offset < 0)
		offset = 0;

	/* Pick the cheapest way to reach `offset`: a plain OFFSET walk costs
	 * `offset` index rows; seeking from a cached anchor costs only the
	 * distance between the anchor and the requested edge. */
	{
		sb_chan_cache *c = chan_cache_ensure_count (db, channel_id);
		int end = offset + limit - 1;
		sb_anchor *before, *after;
		int cost_plain = offset;
		int cost_fwd = G_MAXINT, cost_bwd = G_MAXINT;
		gboolean backward = FALSE;

		if (c->count <= 0 || offset >= c->count)
			return NULL;
		if (end > c->count - 1)
			end = c->count - 1;
		limit = end - offset + 1;

		if (!sb_anchors_disabled ())
			chan_cache_find_anchors (c, offset, end, &before, &after);
		if (before)
			cost_fwd = offset - before->ordinal;
		if (after)
			cost_bwd = after->ordinal - end;

		if (cost_fwd <= cost_plain && cost_fwd <= cost_bwd)
		{
			stmt = db->stmt_load_range_fwd;
			sqlite3_reset (stmt);
			sqlite3_bind_int64 (stmt, 1, channel_id);
			sqlite3_bind_int64 (stmt, 2, before->ts);
			sqlite3_bind_int64 (stmt, 3, before->id);
			sqlite3_bind_int (stmt, 4, limit);
			sqlite3_bind_int (stmt, 5, cost_fwd);
		}
		else if (cost_bwd < cost_plain)
		{
			stmt = db->stmt_load_range_bwd;
			backward = TRUE;
			sqlite3_reset (stmt);
			sqlite3_bind_int64 (stmt, 1, channel_id);
			sqlite3_bind_int64 (stmt, 2, after->ts);
			sqlite3_bind_int64 (stmt, 3, after->id);
			sqlite3_bind_int (stmt, 4, limit);
			sqlite3_bind_int (stmt, 5, cost_bwd);
		}
		else
		{
			stmt = db->stmt_load_range;
			sqlite3_reset (stmt);
			sqlite3_bind_int64 (stmt, 1, channel_id);
			sqlite3_bind_int (stmt, 2, limit);
			sqlite3_bind_int (stmt, 3, offset);
		}

		while ((rc = sqlite3_step (stmt)) == SQLITE_ROW)
		{
			scrollback_msg *msg = g_new0 (scrollback_msg, 1);

			msg->id = sqlite3_column_int64 (stmt, 0);
			msg->channel = g_strdup (channel);
			msg->timestamp = (time_t)sqlite3_column_int64 (stmt, 1);

			const char *msgid_text = (const char *)sqlite3_column_text (stmt, 2);
			msg->msgid = msgid_text ? g_strdup (msgid_text) : NULL;

			msg->text = g_strdup ((const char *)sqlite3_column_text (stmt, 3));

			{
				const char *rby = (const char *)sqlite3_column_text (stmt, 4);
				const char *rreason = (const char *)sqlite3_column_text (stmt, 5);
				msg->redacted_by = rby ? g_strdup (rby) : NULL;
				msg->redact_reason = rreason ? g_strdup (rreason) : NULL;
				msg->redact_time = (time_t)sqlite3_column_int64 (stmt, 6);
			}
			msg->is_user_msg = sqlite3_column_int (stmt, 7) ? TRUE : FALSE;

			list = g_slist_prepend (list, msg);
			n++;
		}

		if (rc != SQLITE_DONE)
			g_warning ("Error loading scrollback range: %s", sqlite3_errmsg (db->db));

		/* Prepending ASC rows yields DESC; prepending DESC rows yields ASC. */
		if (!backward)
			list = g_slist_reverse (list);

		poxchat_timing_log ("load_range %s off=%d lim=%d count=%d path=%s anchor=%d n=%d first=(%" G_GINT64_FORMAT
		                    ",%" G_GINT64_FORMAT ") last=(%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ")",
		                    channel, offset, limit, c->count,
		                    stmt == db->stmt_load_range_fwd ? "fwd" : backward ? "bwd" : "plain",
		                    stmt == db->stmt_load_range_fwd ? before->ordinal : backward ? after->ordinal : -1,
		                    n, n ? (gint64) ((scrollback_msg *) list->data)->timestamp : 0,
		                    n ? ((scrollback_msg *) list->data)->id : 0,
		                    n ? (gint64) ((scrollback_msg *) g_slist_last (list)->data)->timestamp : 0,
		                    n ? ((scrollback_msg *) g_slist_last (list)->data)->id : 0);

		/* Remember both edges of what we just walked: the next scroll step
		 * will ask for a range adjacent to this one. */
		if (n > 0)
		{
			scrollback_msg *first = list->data;
			scrollback_msg *last = g_slist_last (list)->data;
			/* A short read (stale count) leaves the walked rows hugging
			 * the anchor we started from, so derive ordinals from that
			 * side rather than assuming the full span came back. */
			int first_ord = backward ? end - n + 1 : offset;
			chan_cache_add_anchor (c, first_ord, (gint64) first->timestamp, first->id);
			chan_cache_add_anchor (c, first_ord + n - 1, (gint64) last->timestamp, last->id);
		}
	}

	return list;
}

gint64
scrollback_get_max_rowid (scrollback_db *db, const char *channel)
{
	gint64 max_id = 0;

	if (!db || !channel)
		return 0;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return 0;

	sqlite3_reset (db->stmt_max_rowid);
	sqlite3_bind_int64 (db->stmt_max_rowid, 1, channel_id);

	if (sqlite3_step (db->stmt_max_rowid) == SQLITE_ROW)
		max_id = sqlite3_column_int64 (db->stmt_max_rowid, 0);

	return max_id;
}

int
scrollback_get_index_of_rowid (scrollback_db *db, const char *channel, gint64 rowid)
{
	int index = 0;

	if (!db || !channel)
		return 0;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return 0;

	/* Seek from the nearest cached anchor when one is closer than the
	 * start of the channel: count only the rows between them. */
	{
		sb_chan_cache *c = chan_cache_ensure_count (db, channel_id);
		gint64 ts = 0;
		gboolean have_ts = FALSE;

		sqlite3_reset (db->stmt_row_key);
		sqlite3_bind_int64 (db->stmt_row_key, 1, rowid);
		if (sqlite3_step (db->stmt_row_key) == SQLITE_ROW)
		{
			ts = sqlite3_column_int64 (db->stmt_row_key, 0);
			have_ts = TRUE;
		}

		if (have_ts && c && c->count > 0 && c->anchors->len > 0 && !sb_anchors_disabled ())
		{
			sb_anchor *before = NULL, *after = NULL;
			guint i;
			int span;

			for (i = 0; i < c->anchors->len; i++)
			{
				sb_anchor *e = &g_array_index (c->anchors, sb_anchor, i);
				gboolean le = e->ts < ts || (e->ts == ts && e->id <= rowid);
				if (le)
					before = e;
				else
				{
					after = e;
					break;
				}
			}

			/* Anchor at the row itself */
			if (before && before->ts == ts && before->id == rowid)
				return before->ordinal;

			/* Prefer counting forward from the anchor below the row; the
			 * span is bounded by the next anchor above either way. */
			if (before)
			{
				/* rows in (before, row] */
				sqlite3_reset (db->stmt_count_span);
				sqlite3_bind_int64 (db->stmt_count_span, 1, channel_id);
				sqlite3_bind_int64 (db->stmt_count_span, 2, before->ts);
				sqlite3_bind_int64 (db->stmt_count_span, 3, before->id);
				sqlite3_bind_int64 (db->stmt_count_span, 4, ts);
				sqlite3_bind_int64 (db->stmt_count_span, 5, rowid);
				if (sqlite3_step (db->stmt_count_span) == SQLITE_ROW)
				{
					span = sqlite3_column_int (db->stmt_count_span, 0);
					index = before->ordinal + span;
					poxchat_timing_log ("index_of_rowid %s row=%" G_GINT64_FORMAT " ts=%" G_GINT64_FORMAT
					                    " via before(ord=%d) span=%d -> %d count=%d",
					                    channel, rowid, ts, before->ordinal, span, index, c->count);
					chan_cache_add_anchor (c, index, ts, rowid);
					return index;
				}
			}
			else if (after)
			{
				/* rows in (row, after] */
				sqlite3_reset (db->stmt_count_span);
				sqlite3_bind_int64 (db->stmt_count_span, 1, channel_id);
				sqlite3_bind_int64 (db->stmt_count_span, 2, ts);
				sqlite3_bind_int64 (db->stmt_count_span, 3, rowid);
				sqlite3_bind_int64 (db->stmt_count_span, 4, after->ts);
				sqlite3_bind_int64 (db->stmt_count_span, 5, after->id);
				if (sqlite3_step (db->stmt_count_span) == SQLITE_ROW)
				{
					span = sqlite3_column_int (db->stmt_count_span, 0);
					index = after->ordinal - span;
					poxchat_timing_log ("index_of_rowid %s row=%" G_GINT64_FORMAT " ts=%" G_GINT64_FORMAT
					                    " via after(ord=%d) span=%d -> %d count=%d",
					                    channel, rowid, ts, after->ordinal, span, index, c->count);
					chan_cache_add_anchor (c, index, ts, rowid);
					return index;
				}
			}
		}
	}

	sqlite3_reset (db->stmt_index_of_rowid);
	sqlite3_bind_int64 (db->stmt_index_of_rowid, 1, channel_id);
	sqlite3_bind_int64 (db->stmt_index_of_rowid, 2, rowid);

	if (sqlite3_step (db->stmt_index_of_rowid) == SQLITE_ROW)
		index = sqlite3_column_int (db->stmt_index_of_rowid, 0);

	poxchat_timing_log ("index_of_rowid %s row=%" G_GINT64_FORMAT " via prefix-count -> %d",
	                    channel, rowid, index);
	return index;
}

char *
scrollback_get_text_by_msgid (scrollback_db *db, const char *channel, const char *msgid)
{
	char *text = NULL;
	gint64 channel_id;
	sqlite3_stmt *stmt = NULL;

	if (!db || !channel || !msgid || !msgid[0])
		return NULL;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return NULL;

	/* One-off query — only for a reply whose target is not materialized */
	if (sqlite3_prepare_v2 (db->db,
			"SELECT text FROM messages WHERE channel_id = ? AND msgid = ? LIMIT 1",
			-1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_int64 (stmt, 1, channel_id);
		sqlite3_bind_text (stmt, 2, msgid, -1, SQLITE_TRANSIENT);
		if (sqlite3_step (stmt) == SQLITE_ROW)
		{
			const char *t = (const char *) sqlite3_column_text (stmt, 0);
			if (t && t[0])
				text = g_strdup (t);
		}
		sqlite3_finalize (stmt);
	}
	return text;
}

gint64
scrollback_get_rowid_by_msgid (scrollback_db *db, const char *channel, const char *msgid)
{
	gint64 rowid = 0;
	int rc;

	if (!db || !channel || !msgid || !msgid[0])
		return 0;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return 0;

	/* Use a one-off query — this is rare (reply click on evicted entry) */
	{
		sqlite3_stmt *stmt = NULL;
		rc = sqlite3_prepare_v2 (db->db,
			"SELECT id FROM messages WHERE channel_id = ? AND msgid = ? LIMIT 1",
			-1, &stmt, NULL);
		if (rc == SQLITE_OK)
		{
			sqlite3_bind_int64 (stmt, 1, channel_id);
			sqlite3_bind_text (stmt, 2, msgid, -1, SQLITE_TRANSIENT);
			if (sqlite3_step (stmt) == SQLITE_ROW)
				rowid = sqlite3_column_int64 (stmt, 0);
			sqlite3_finalize (stmt);
		}
	}

	return rowid;
}

/* --- Gap ledger (chathistory gap fill) --- */

/* Record a hole in stored history.  Merges with any overlapping-or-touching
 * non-dead gap of the same channel (union of bounds; msgids taken from
 * whichever row contributes each bound; resulting state = MIN of merged
 * states so witnessed absorbs candidate).  Returns the surviving row id,
 * or -1 on failure.  Runs inside a ref-counted transaction. */
gint64
scrollback_gap_record (scrollback_db *db, const char *channel,
                       gint64 start_ts, const char *start_msgid,
                       gint64 end_ts, const char *end_msgid, int state)
{
	gint64 channel_id, result = -1;
	gint64 u_start = start_ts, u_end = end_ts;
	char *u_start_msgid = g_strdup (start_msgid);
	char *u_end_msgid = g_strdup (end_msgid);
	int u_state = state;
	sqlite3_stmt *stmt = NULL;

	if (!db || !channel || start_ts <= 0 || end_ts <= start_ts)
		goto out;
	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		goto out;

	scrollback_begin_transaction (db);

	/* Fold every overlapping-or-touching live gap into the union */
	if (sqlite3_prepare_v2 (db->db,
		"SELECT id, start_ts, start_msgid, end_ts, end_msgid, state FROM gaps "
		"WHERE channel_id = ?1 AND state != 2 AND start_ts <= ?2 AND end_ts >= ?3",
		-1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_int64 (stmt, 1, channel_id);
		sqlite3_bind_int64 (stmt, 2, end_ts);
		sqlite3_bind_int64 (stmt, 3, start_ts);
		while (sqlite3_step (stmt) == SQLITE_ROW)
		{
			gint64 o_start = sqlite3_column_int64 (stmt, 1);
			gint64 o_end = sqlite3_column_int64 (stmt, 3);
			int o_state = sqlite3_column_int (stmt, 5);
			if (o_start < u_start)
			{
				u_start = o_start;
				g_free (u_start_msgid);
				u_start_msgid = g_strdup ((const char *) sqlite3_column_text (stmt, 2));
			}
			if (o_end > u_end)
			{
				u_end = o_end;
				g_free (u_end_msgid);
				u_end_msgid = g_strdup ((const char *) sqlite3_column_text (stmt, 4));
			}
			if (o_state < u_state)
				u_state = o_state;	/* witnessed (0) absorbs candidate (1) */
			{
				char *del = sqlite3_mprintf (
					"DELETE FROM gaps WHERE id = %lld",
					(long long) sqlite3_column_int64 (stmt, 0));
				sqlite3_exec (db->db, del, NULL, NULL, NULL);
				sqlite3_free (del);
			}
		}
		sqlite3_finalize (stmt);
		stmt = NULL;
	}

	if (sqlite3_prepare_v2 (db->db,
		"INSERT INTO gaps (channel_id, start_ts, start_msgid, end_ts, end_msgid, state) "
		"VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
		-1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_int64 (stmt, 1, channel_id);
		sqlite3_bind_int64 (stmt, 2, u_start);
		if (u_start_msgid)
			sqlite3_bind_text (stmt, 3, u_start_msgid, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_null (stmt, 3);
		sqlite3_bind_int64 (stmt, 4, u_end);
		if (u_end_msgid)
			sqlite3_bind_text (stmt, 5, u_end_msgid, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_null (stmt, 5);
		sqlite3_bind_int (stmt, 6, u_state);
		if (sqlite3_step (stmt) == SQLITE_DONE)
			result = sqlite3_last_insert_rowid (db->db);
		sqlite3_finalize (stmt);
		stmt = NULL;
	}

	scrollback_commit_transaction (db);
out:
	if (stmt)
		sqlite3_finalize (stmt);
	g_free (u_start_msgid);
	g_free (u_end_msgid);
	return result;
}

/* Return all gaps for a channel (all states, including dead — consumers
 * filter), ordered by start_ts.  Free with scrollback_gap_list_free. */
GList *
scrollback_gap_list (scrollback_db *db, const char *channel)
{
	GList *list = NULL;
	gint64 channel_id;
	int rc;

	if (!db || !db->stmt_gap_list || !channel)
		return NULL;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return NULL;

	sqlite3_reset (db->stmt_gap_list);
	sqlite3_bind_int64 (db->stmt_gap_list, 1, channel_id);

	while ((rc = sqlite3_step (db->stmt_gap_list)) == SQLITE_ROW)
	{
		scrollback_gap *gap = g_new0 (scrollback_gap, 1);
		gap->id = sqlite3_column_int64 (db->stmt_gap_list, 0);
		gap->start_ts = sqlite3_column_int64 (db->stmt_gap_list, 1);
		gap->start_msgid = g_strdup ((const char *) sqlite3_column_text (db->stmt_gap_list, 2));
		gap->end_ts = sqlite3_column_int64 (db->stmt_gap_list, 3);
		gap->end_msgid = g_strdup ((const char *) sqlite3_column_text (db->stmt_gap_list, 4));
		gap->state = sqlite3_column_int (db->stmt_gap_list, 5);
		gap->attempts = sqlite3_column_int (db->stmt_gap_list, 6);
		gap->last_attempt = sqlite3_column_int64 (db->stmt_gap_list, 7);
		list = g_list_prepend (list, gap);
	}
	if (rc != SQLITE_DONE)
		g_warning ("Error loading gap list: %s", sqlite3_errmsg (db->db));

	return g_list_reverse (list);
}

static void
gap_free_and_clear (gpointer data)
{
	scrollback_gap *gap = (scrollback_gap *) data;
	scrollback_gap_clear (gap);
	g_free (gap);
}

/* Free a list returned by scrollback_gap_list. */
void
scrollback_gap_list_free (GList *gaps)
{
	g_list_free_full (gaps, gap_free_and_clear);
}

/* Fill *out with the gap identified by gap_id (strdup'd msgids).
 * Caller frees with scrollback_gap_clear (frees the msgids only, not
 * the struct).  Returns FALSE if no such row exists. */
gboolean
scrollback_gap_get (scrollback_db *db, gint64 gap_id, scrollback_gap *out)
{
	sqlite3_stmt *stmt = NULL;
	gboolean found = FALSE;

	if (!db || gap_id <= 0 || !out)
		return FALSE;

	memset (out, 0, sizeof (*out));

	if (sqlite3_prepare_v2 (db->db,
		"SELECT id, start_ts, start_msgid, end_ts, end_msgid, state, "
		"attempts, last_attempt FROM gaps WHERE id = ?",
		-1, &stmt, NULL) != SQLITE_OK)
		return FALSE;

	sqlite3_bind_int64 (stmt, 1, gap_id);
	if (sqlite3_step (stmt) == SQLITE_ROW)
	{
		out->id = sqlite3_column_int64 (stmt, 0);
		out->start_ts = sqlite3_column_int64 (stmt, 1);
		out->start_msgid = g_strdup ((const char *) sqlite3_column_text (stmt, 2));
		out->end_ts = sqlite3_column_int64 (stmt, 3);
		out->end_msgid = g_strdup ((const char *) sqlite3_column_text (stmt, 4));
		out->state = sqlite3_column_int (stmt, 5);
		out->attempts = sqlite3_column_int (stmt, 6);
		out->last_attempt = sqlite3_column_int64 (stmt, 7);
		found = TRUE;
	}
	sqlite3_finalize (stmt);
	return found;
}

/* Free the msgids owned by a scrollback_gap filled by scrollback_gap_get.
 * Does not free the struct itself (stack-allocated by the caller). */
void
scrollback_gap_clear (scrollback_gap *gap)
{
	if (!gap)
		return;
	g_free (gap->start_msgid);
	g_free (gap->end_msgid);
	gap->start_msgid = NULL;
	gap->end_msgid = NULL;
}

/* Narrow a gap's bounds after a partial fill.  0/NULL for a new_*
 * parameter means "keep this side unchanged".  Resets attempts and
 * last_attempt to 0 — progress re-earns a fast retry. */
void
scrollback_gap_shrink (scrollback_db *db, gint64 gap_id,
                       gint64 new_start_ts, const char *new_start_msgid,
                       gint64 new_end_ts, const char *new_end_msgid)
{
	sqlite3_stmt *stmt;
	if (!db || gap_id <= 0)
		return;
	if (sqlite3_prepare_v2 (db->db,
		"UPDATE gaps SET "
		"start_ts = CASE WHEN ?2 > 0 THEN ?2 ELSE start_ts END, "
		"start_msgid = CASE WHEN ?2 > 0 THEN ?3 ELSE start_msgid END, "
		"end_ts = CASE WHEN ?4 > 0 THEN ?4 ELSE end_ts END, "
		"end_msgid = CASE WHEN ?4 > 0 THEN ?5 ELSE end_msgid END, "
		"attempts = 0, last_attempt = 0 "
		"WHERE id = ?1",
		-1, &stmt, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_int64 (stmt, 1, gap_id);
	sqlite3_bind_int64 (stmt, 2, new_start_ts);
	if (new_start_msgid)
		sqlite3_bind_text (stmt, 3, new_start_msgid, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null (stmt, 3);
	sqlite3_bind_int64 (stmt, 4, new_end_ts);
	if (new_end_msgid)
		sqlite3_bind_text (stmt, 5, new_end_msgid, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null (stmt, 5);
	sqlite3_step (stmt);
	sqlite3_finalize (stmt);

	/* Defense in depth: callers key the direction of a shrink off the
	 * gap-fill request's own approach direction (see chathistory.c's
	 * finish_batch_processing), so a mismatch there -- or any other
	 * caller narrowing the wrong side -- could otherwise leave
	 * start_ts >= end_ts behind: an inverted record that is not a real
	 * gap, and whose attempts/last_attempt reset on every shrink means
	 * it would keep re-requesting forever without ever being
	 * satisfiable.  Treat an inverted result as closed and delete it. */
	if (sqlite3_prepare_v2 (db->db,
		"DELETE FROM gaps WHERE id = ?1 AND start_ts >= end_ts",
		-1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_int64 (stmt, 1, gap_id);
		sqlite3_step (stmt);
		sqlite3_finalize (stmt);
	}
}

/* Set a gap's state (SCROLLBACK_GAP_*). */
void
scrollback_gap_set_state (scrollback_db *db, gint64 gap_id, int state)
{
	sqlite3_stmt *stmt;

	if (!db || gap_id <= 0)
		return;

	if (sqlite3_prepare_v2 (db->db,
		"UPDATE gaps SET state = ? WHERE id = ?",
		-1, &stmt, NULL) != SQLITE_OK)
		return;

	sqlite3_bind_int (stmt, 1, state);
	sqlite3_bind_int64 (stmt, 2, gap_id);
	sqlite3_step (stmt);
	sqlite3_finalize (stmt);
}

/* Record a fill attempt: attempts+1, last_attempt = now.  Returns the
 * new attempts value. */
int
scrollback_gap_touch (scrollback_db *db, gint64 gap_id)
{
	sqlite3_stmt *stmt = NULL;
	int attempts = 0;
	gint64 now = (gint64) time (NULL);

	if (!db || gap_id <= 0)
		return 0;

	if (sqlite3_prepare_v2 (db->db,
		"UPDATE gaps SET attempts = attempts + 1, last_attempt = ?2 WHERE id = ?1",
		-1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_int64 (stmt, 1, gap_id);
		sqlite3_bind_int64 (stmt, 2, now);
		sqlite3_step (stmt);
		sqlite3_finalize (stmt);
		stmt = NULL;
	}

	if (sqlite3_prepare_v2 (db->db,
		"SELECT attempts FROM gaps WHERE id = ?",
		-1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_int64 (stmt, 1, gap_id);
		if (sqlite3_step (stmt) == SQLITE_ROW)
			attempts = sqlite3_column_int (stmt, 0);
		sqlite3_finalize (stmt);
	}

	return attempts;
}

/* Delete a gap row outright (e.g. a candidate proven not to be a real
 * hole). */
void
scrollback_gap_delete (scrollback_db *db, gint64 gap_id)
{
	sqlite3_stmt *stmt;

	if (!db || gap_id <= 0)
		return;

	if (sqlite3_prepare_v2 (db->db,
		"DELETE FROM gaps WHERE id = ?",
		-1, &stmt, NULL) != SQLITE_OK)
		return;

	sqlite3_bind_int64 (stmt, 1, gap_id);
	sqlite3_step (stmt);
	sqlite3_finalize (stmt);
}

/* Forget a gap's msgid anchors and reset its backoff so the next request
 * uses timestamp refs.  Returns TRUE if there was a msgid to drop. */
gboolean
scrollback_gap_drop_msgids (scrollback_db *db, gint64 gap_id)
{
	sqlite3_stmt *stmt;
	gboolean changed = FALSE;

	if (!db || gap_id <= 0)
		return FALSE;

	if (sqlite3_prepare_v2 (db->db,
		"UPDATE gaps SET start_msgid = NULL, end_msgid = NULL, "
		"attempts = 0, last_attempt = 0 "
		"WHERE id = ? AND (start_msgid IS NOT NULL OR end_msgid IS NOT NULL)",
		-1, &stmt, NULL) != SQLITE_OK)
		return FALSE;

	sqlite3_bind_int64 (stmt, 1, gap_id);
	if (sqlite3_step (stmt) == SQLITE_DONE)
		changed = sqlite3_changes (db->db) > 0;
	sqlite3_finalize (stmt);
	return changed;
}

int
scrollback_gap_reset (scrollback_db *db, const char *channel, gint64 gap_id)
{
	sqlite3_stmt *stmt;
	gint64 channel_id;
	int changed = 0;

	if (!db || !channel)
		return 0;
	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return 0;

	if (sqlite3_prepare_v2 (db->db,
		"UPDATE gaps SET state = ?3, attempts = 0, last_attempt = 0, "
		"start_msgid = NULL, end_msgid = NULL "
		"WHERE channel_id = ?1 AND (?2 = 0 OR id = ?2)",
		-1, &stmt, NULL) != SQLITE_OK)
		return 0;

	sqlite3_bind_int64 (stmt, 1, channel_id);
	sqlite3_bind_int64 (stmt, 2, gap_id > 0 ? gap_id : 0);
	sqlite3_bind_int (stmt, 3, SCROLLBACK_GAP_WITNESSED);
	if (sqlite3_step (stmt) == SQLITE_DONE)
		changed = sqlite3_changes (db->db);
	sqlite3_finalize (stmt);
	return changed;
}

void
scrollback_gap_bootstrap_reset (scrollback_db *db, const char *channel)
{
	gint64 channel_id;
	char *sql;

	if (!db || !channel)
		return;
	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return;
	sql = sqlite3_mprintf ("UPDATE channels SET gap_bootstrap_done = 0 WHERE id = %lld",
	                       (long long) channel_id);
	sqlite3_exec (db->db, sql, NULL, NULL, NULL);
	sqlite3_free (sql);
}

/* Position of a gap's end bound in the same (timestamp, id) ordinal
 * space scrollback_load_range uses: COUNT(*) of messages strictly
 * before end_ts.  Ties at end_ts are the end-flanking row itself and
 * belong after the gap — the off-by-tie is irrelevant for a proximity
 * margin. */
int
scrollback_gap_ordinal (scrollback_db *db, const char *channel, gint64 end_ts)
{
	int count = 0;
	gint64 channel_id;

	if (!db || !db->stmt_gap_ordinal || !channel)
		return 0;

	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return 0;

	sqlite3_reset (db->stmt_gap_ordinal);
	sqlite3_bind_int64 (db->stmt_gap_ordinal, 1, channel_id);
	sqlite3_bind_int64 (db->stmt_gap_ordinal, 2, end_ts);

	if (sqlite3_step (db->stmt_gap_ordinal) == SQLITE_ROW)
		count = sqlite3_column_int (db->stmt_gap_ordinal, 0);

	return count;
}

/* One-shot per-channel bootstrap scan: walks stored messages in order and
 * records a candidate gap for every adjacent pair whose timestamps are
 * >= threshold_secs apart.  Latches via channels.gap_bootstrap_done so a
 * channel is never rescanned, even when it found nothing.  Returns
 * candidates recorded, or -1 if already done / bad args / error. */
int
scrollback_gap_bootstrap (scrollback_db *db, const char *channel,
                          gint64 threshold_secs)
{
	gint64 channel_id;
	sqlite3_stmt *stmt = NULL;
	gint64 prev_ts = 0;
	char *prev_msgid = NULL;
	int recorded = 0;
	int done = 0;

	if (!db || !channel || threshold_secs <= 0)
		return -1;
	channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id <= 0)
		return -1;

	/* One-shot latch */
	if (sqlite3_prepare_v2 (db->db,
		"SELECT gap_bootstrap_done FROM channels WHERE id = ?1",
		-1, &stmt, NULL) != SQLITE_OK)
		return -1;
	sqlite3_bind_int64 (stmt, 1, channel_id);
	if (sqlite3_step (stmt) == SQLITE_ROW)
		done = sqlite3_column_int (stmt, 0);
	sqlite3_finalize (stmt);
	stmt = NULL;
	if (done)
		return -1;

	scrollback_begin_transaction (db);

	/* pending:* msgids are client-local echo-message placeholders that
	 * haven't been confirmed by the server yet (stale ones are purged
	 * before this bootstrap idle is scheduled, but a message sent between
	 * join and the idle firing can still be mid-flight).  The row's
	 * timestamp is real activity and must still bound the gap -- excluding
	 * the row would fabricate silence -- but the placeholder itself must
	 * never be snapshotted as a gap boundary msgid, so it's nulled out
	 * here; scrollback_gap_record's NULL handling and the ledger's
	 * documented timestamp= fallback take it from there. */
	if (sqlite3_prepare_v2 (db->db,
		"SELECT timestamp, CASE WHEN msgid LIKE 'pending:%' THEN NULL ELSE msgid END "
		"FROM messages WHERE channel_id = ?1 "
		"ORDER BY timestamp ASC, id ASC",
		-1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_int64 (stmt, 1, channel_id);
		while (sqlite3_step (stmt) == SQLITE_ROW)
		{
			gint64 ts = sqlite3_column_int64 (stmt, 0);
			const char *msgid = (const char *) sqlite3_column_text (stmt, 1);
			if (prev_ts > 0 && ts - prev_ts >= threshold_secs)
			{
				if (scrollback_gap_record (db, channel, prev_ts, prev_msgid,
				                           ts, msgid,
				                           SCROLLBACK_GAP_CANDIDATE) > 0)
					recorded++;
			}
			prev_ts = ts;
			g_free (prev_msgid);
			prev_msgid = g_strdup (msgid);
		}
		sqlite3_finalize (stmt);
	}
	g_free (prev_msgid);

	{
		char *upd = sqlite3_mprintf (
			"UPDATE channels SET gap_bootstrap_done = 1 WHERE id = %lld",
			(long long) channel_id);
		sqlite3_exec (db->db, upd, NULL, NULL, NULL);
		sqlite3_free (upd);
	}

	scrollback_commit_transaction (db);
	return recorded;
}

GSList *
scrollback_search_text (scrollback_db *db, const char *channel, const char *pattern)
{
	GSList *list = NULL;
	int rc;

	if (!db || !channel || !pattern)
		return NULL;

	gint64 channel_id = scrollback_get_channel_id (db, channel);
	if (channel_id < 0)
		return NULL;

	sqlite3_reset (db->stmt_search_text);
	sqlite3_bind_int64 (db->stmt_search_text, 1, channel_id);
	sqlite3_bind_text (db->stmt_search_text, 2, pattern, -1, SQLITE_TRANSIENT);

	while ((rc = sqlite3_step (db->stmt_search_text)) == SQLITE_ROW)
	{
		scrollback_msg *msg = g_new0 (scrollback_msg, 1);

		msg->id = sqlite3_column_int64 (db->stmt_search_text, 0);
		msg->channel = g_strdup (channel);
		msg->text = g_strdup ((const char *)sqlite3_column_text (db->stmt_search_text, 1));

		list = g_slist_prepend (list, msg);
	}

	if (rc != SQLITE_DONE)
		g_warning ("Error searching scrollback: %s", sqlite3_errmsg (db->db));

	return g_slist_reverse (list);
}
