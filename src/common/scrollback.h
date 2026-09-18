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
 * SQLite-based scrollback storage for message history persistence
 */

#ifndef POXCHAT_SCROLLBACK_H
#define POXCHAT_SCROLLBACK_H

#include <glib.h>
#include <time.h>

/* Opaque handle to scrollback database */
typedef struct scrollback_db scrollback_db;

/* Message record from scrollback */
typedef struct {
	gint64 id;           /* Database row ID */
	char *channel;       /* Channel/query name */
	time_t timestamp;    /* Message timestamp */
	char *msgid;         /* IRCv3 msgid (may be NULL) */
	char *text;          /* Message text (formatted for display) */
	char *redacted_by;   /* Who redacted this message (NULL = not redacted) */
	char *redact_reason; /* Redaction reason (may be NULL) */
	time_t redact_time;  /* When redaction occurred */
	gboolean is_user_msg;/* True for PRIVMSG/NOTICE/ACTION; false for events.
	                      * Gates hover reply/react buttons on re-materialized entries. */
} scrollback_msg;

/**
 * Open (or create) the scrollback database for a network.
 * Database is stored in scrollback/{network}.db
 *
 * @param network Network name (used for filename)
 * @return Database handle, or NULL on error
 */
scrollback_db *scrollback_open (const char *network);

/**
 * Close the scrollback database.
 *
 * @param db Database handle
 */
void scrollback_db_close (scrollback_db *db);

/**
 * Save a message to scrollback.
 * Uses INSERT OR IGNORE for automatic deduplication by msgid.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @param timestamp Message timestamp
 * @param msgid IRCv3 message ID (may be NULL)
 * @param text Message text (formatted for display)
 * @return Inserted rowid on success, -1 on failure
 */
gint64 scrollback_db_save (scrollback_db *db, const char *channel,
                          time_t timestamp, const char *msgid, const char *text,
                          gboolean is_user_msg);

/**
 * Load the most recent messages for a channel.
 * Returns messages in chronological order (oldest first).
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @param limit Maximum messages to load
 * @return GSList of scrollback_msg* (caller must free with scrollback_msg_list_free)
 */
GSList *scrollback_db_load (scrollback_db *db, const char *channel, int limit);

/**
 * Delete this channel's unconfirmed "pending:<label>" echo rows.
 *
 * They are placeholders for messages we sent and never saw echoed; one
 * that outlived its session will never be confirmed.  Run this before
 * reading the channel's newest msgid or counting its rows.
 *
 * @param db Database handle
 * @param channel Channel/query name
 */
void scrollback_purge_pending (scrollback_db *db, const char *channel);

/**
 * Get the newest msgid for a channel (for CHATHISTORY AFTER requests).
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Newest msgid (caller must g_free), or NULL if none
 */
char *scrollback_get_newest_msgid (scrollback_db *db, const char *channel);

/**
 * Newest non-pending msgid across every channel; caller frees. The
 * draft/persistence attach cursor.
 *
 * @param db Database handle
 * @return Newest msgid across all channels (caller must g_free), or NULL if none
 */
char *scrollback_get_global_newest_msgid (scrollback_db *db);

/**
 * Get the oldest msgid for a channel (for CHATHISTORY BEFORE requests).
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Oldest msgid (caller must g_free), or NULL if none
 */
char *scrollback_get_oldest_msgid (scrollback_db *db, const char *channel);

/**
 * Get the newest timestamp for a channel.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Newest timestamp, or 0 if no messages
 */
time_t scrollback_get_newest_time (scrollback_db *db, const char *channel);

/**
 * Check if a msgid already exists in a channel (for deduplication).
 * Channel-scoped: msgids are only unique per channel (multi-target
 * messages share one msgid across targets).
 *
 * @param db Database handle
 * @param channel Channel name
 * @param msgid Message ID to check
 * @param timestamp Require an exact timestamp match too (0 = msgid alone);
 *                  guards against servers that reuse msgids after restarts
 * @return TRUE if msgid exists in that channel
 */
gboolean scrollback_has_msgid (scrollback_db *db, const char *channel,
                               const char *msgid, time_t timestamp);

/**
 * Delete a single message row by rowid.  Used to drop a pending
 * placeholder row that lost the echo-vs-chathistory race.
 *
 * @return TRUE if a row was deleted
 */
gboolean scrollback_delete_by_rowid (scrollback_db *db, gint64 rowid);

/**
 * Update a pending placeholder msgid to the real server-assigned msgid.
 * Used by echo-message confirmation: pending entry saved with "pending:<label>"
 * gets updated to the real msgid when the echo arrives.
 */
gboolean scrollback_update_pending_msgid (scrollback_db *db, const char *channel,
                                           const char *pending_msgid, const char *real_msgid);

/**
 * Mark a message as redacted in scrollback.
 * Preserves the original text for accountability; stores who redacted it and why.
 * Channel-scoped: a multi-target copy of the msgid in another channel is
 * not redacted collaterally.
 *
 * @param db Database handle
 * @param channel Channel the redaction was issued in
 * @param msgid Message ID to redact
 * @param redacted_by Nick who performed the redaction
 * @param reason Redaction reason (may be NULL)
 * @param redact_time When the redaction occurred
 * @return TRUE if a message was updated
 */
gboolean scrollback_redact_message (scrollback_db *db, const char *channel,
                                    const char *msgid,
                                    const char *redacted_by, const char *reason,
                                    time_t redact_time);

/**
 * Clear all messages for a channel.
 *
 * @param db Database handle
 * @param channel Channel/query name
 */
void scrollback_clear (scrollback_db *db, const char *channel);

/**
 * Free a scrollback message.
 *
 * @param msg Message to free
 */
void scrollback_msg_free (scrollback_msg *msg);

/**
 * Free a list of scrollback messages.
 *
 * @param list List returned by scrollback_db_load
 */
void scrollback_msg_list_free (GSList *list);

/**
 * Migrate old text-based scrollback to SQLite.
 * Called automatically when opening a database if old files exist.
 *
 * @param db Database handle
 * @param network Network name
 * @param channel Channel name
 * @return Number of messages migrated, or -1 on error
 */
int scrollback_migrate (scrollback_db *db, const char *network, const char *channel);

/* IRCv3 reaction record from scrollback */
typedef struct {
	char *target_msgid;      /* msgid of the message reacted to */
	char *reaction_text;     /* reaction content (emoji or text) */
	char *nick;              /* who reacted */
	gboolean is_self;        /* was this our own reaction? */
} scrollback_reaction;

/* IRCv3 reply record from scrollback */
typedef struct {
	char *msgid;             /* msgid of the reply message */
	char *target_msgid;      /* msgid of the message being replied to */
	char *target_nick;       /* nick of the original message */
	char *target_preview;    /* truncated preview of original message */
} scrollback_reply;

/**
 * Save a reaction to scrollback.
 */
gboolean scrollback_save_reaction (scrollback_db *db, const char *channel,
                                   const char *target_msgid, const char *reaction_text,
                                   const char *nick, gboolean is_self, time_t timestamp);

/**
 * Remove a reaction from scrollback (channel-scoped).
 */
gboolean scrollback_remove_reaction (scrollback_db *db, const char *channel,
                                     const char *target_msgid,
                                     const char *reaction_text, const char *nick);

/**
 * Load all reactions for a channel.
 * @return GSList of scrollback_reaction* (caller frees with scrollback_reaction_list_free)
 */
GSList *scrollback_load_reactions (scrollback_db *db, const char *channel);

/**
 * Load reactions for a single target msgid.  Used to re-hydrate badges on
 * virt-materialized historical entries after eviction + ensure_range.
 *
 * @return GSList of scrollback_reaction* (caller frees via scrollback_reaction_list_free)
 */
GSList *scrollback_load_reactions_by_msgid (scrollback_db *db, const char *channel,
                                            const char *target_msgid);
void scrollback_reaction_free (scrollback_reaction *r);
void scrollback_reaction_list_free (GSList *list);

/**
 * Save reply context for a message (channel-scoped upsert).
 */
gboolean scrollback_save_reply (scrollback_db *db, const char *channel,
                                const char *msgid,
                                const char *target_msgid, const char *target_nick,
                                const char *target_preview);

/**
 * Load all reply contexts for messages in a channel.
 * @return GSList of scrollback_reply* (caller frees with scrollback_reply_list_free)
 */
GSList *scrollback_load_replies (scrollback_db *db, const char *channel);
void scrollback_reply_free (scrollback_reply *r);
void scrollback_reply_list_free (GSList *list);

/**
 * Load the reply context for a single message by its msgid.
 * Used by virtual-scrollback re-materialization to reattach reply info
 * to entries that were evicted and reloaded from the DB.
 * @return scrollback_reply* (caller frees with scrollback_reply_free) or NULL if no reply.
 */
scrollback_reply *scrollback_load_reply_by_msgid (scrollback_db *db, const char *channel,
                                                  const char *msgid);

/**
 * Batched lookups for a window of messages (virtual-scrollback
 * re-materialization).  One IN(...) query per few hundred msgids instead
 * of one probe per entry.  Both return a hash table the caller destroys
 * with g_hash_table_destroy; values are owned by the table.
 *   replies:   msgid -> scrollback_reply*
 *   reactions: target_msgid -> GSList* of scrollback_reaction*
 * NULL only when the channel is unknown.
 */
GHashTable *scrollback_load_replies_for_msgs (scrollback_db *db, const char *channel,
                                              GSList *msgs);
GHashTable *scrollback_load_reactions_for_msgs (scrollback_db *db, const char *channel,
                                                GSList *msgs);

/* --- FTS5 search index --- */

/**
 * Queue a channel for (idle, chunked) indexing if it hasn't been indexed
 * yet.  No-op when the SQLite build lacks FTS5.
 */
void scrollback_fts_schedule (scrollback_db *db, const char *channel);

/**
 * TRUE once the channel's index is complete and scrollback_search_fts
 * can be trusted to return every substring match.
 */
gboolean scrollback_fts_ready (scrollback_db *db, const char *channel);

/**
 * Candidate rows whose stripped text contains `needle` (case-insensitive,
 * needle must be at least three characters), chronological.  Each
 * scrollback_msg carries only id and text; free with
 * scrollback_msg_list_free.  Callers re-verify with their own matcher
 * for case-sensitive searches.
 */
GSList *scrollback_search_fts (scrollback_db *db, const char *channel,
                               const char *needle);

/* --- Virtual scrollback query functions --- */

/**
 * Get total message count for a channel.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Total message count, or 0 on error
 */
int scrollback_count (scrollback_db *db, const char *channel);

/**
 * Load a window of messages by position (offset/limit).
 * Returns messages in chronological order (oldest first).
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @param offset 0-based starting position
 * @param limit Maximum messages to load
 * @return GSList of scrollback_msg* (caller must free with scrollback_msg_list_free)
 */
GSList *scrollback_load_range (scrollback_db *db, const char *channel,
                               int offset, int limit);

/**
 * Get the maximum message row ID for a channel.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @return Max row ID, or 0 if no messages
 */
gint64 scrollback_get_max_rowid (scrollback_db *db, const char *channel);

/**
 * Get the 0-based positional index of a specific row ID in a channel.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @param rowid Row ID to look up
 * @return 0-based index of the entry
 */
int scrollback_get_index_of_rowid (scrollback_db *db, const char *channel,
                                    gint64 rowid);

/**
 * Find the DB rowid for a message by its IRCv3 msgid.
 * Returns 0 if not found.
 */
gint64 scrollback_get_rowid_by_msgid (scrollback_db *db, const char *channel,
                                       const char *msgid);

/**
 * The stored formatted text of a message by its IRCv3 msgid, for quoting
 * a reply target that is not on screen.  Caller frees; NULL if not found.
 */
char *scrollback_get_text_by_msgid (scrollback_db *db, const char *channel,
                                    const char *msgid);

/**
 * Search message text using SQL LIKE pattern.
 * Returns matching messages in chronological order.
 * Only id, channel, and text fields are populated in the results.
 *
 * @param db Database handle
 * @param channel Channel/query name
 * @param pattern SQL LIKE pattern (e.g., "%search term%")
 * @return GSList of scrollback_msg* (caller must free with scrollback_msg_list_free)
 */
GSList *scrollback_search_text (scrollback_db *db, const char *channel,
                                const char *pattern);

/* --- Gap ledger (chathistory gap fill) --- */

#define SCROLLBACK_GAP_WITNESSED 0	/* both flanking rows are real stored messages */
#define SCROLLBACK_GAP_CANDIDATE 1	/* heuristic guess (e.g. timestamp jump) */
#define SCROLLBACK_GAP_DEAD      2	/* fill attempts exhausted or proven not a hole */

/* A recorded hole in stored history.  Bounds are anchored to real stored
 * rows and are exclusive on both ends: start_ts/start_msgid identify the
 * newest stored message BEFORE the hole, end_ts/end_msgid the oldest
 * stored message AFTER it.  Either msgid may be NULL when its flanking
 * row has none. */
typedef struct {
	gint64 id;
	gint64 start_ts;      /* newest stored msg BEFORE the hole (exclusive bound) */
	char *start_msgid;    /* NULL when the flanking row has no msgid */
	gint64 end_ts;        /* oldest stored msg AFTER the hole (exclusive bound) */
	char *end_msgid;
	int state;            /* SCROLLBACK_GAP_* */
	int attempts;
	gint64 last_attempt;  /* unix time of last fill attempt (0 = never) */
} scrollback_gap;

/**
 * Record a hole in stored history.  Merges with any overlapping-or-touching
 * non-dead gap of the same channel: bounds become the union, each bound's
 * msgid is taken from whichever row contributes it, and the resulting
 * state is the MIN of the merged states (witnessed absorbs candidate).
 * Runs inside scrollback_begin_transaction/commit (ref-counted, nests
 * safely with an outer transaction).
 *
 * @return The surviving row id, or -1 on failure (bad args, or
 *         end_ts <= start_ts).
 */
gint64 scrollback_gap_record (scrollback_db *db, const char *channel,
                              gint64 start_ts, const char *start_msgid,
                              gint64 end_ts, const char *end_msgid, int state);

/**
 * List all gaps for a channel, including dead ones (consumers filter),
 * ordered by start_ts ascending.
 *
 * @return GList of scrollback_gap* (caller frees with scrollback_gap_list_free)
 */
GList *scrollback_gap_list (scrollback_db *db, const char *channel);

/**
 * Free a list returned by scrollback_gap_list.
 */
void scrollback_gap_list_free (GList *gaps);

/**
 * Fill *out with the gap identified by gap_id (msgids are strdup'd).
 * Caller frees with scrollback_gap_clear, which frees only the msgids,
 * not the struct itself.
 *
 * @return TRUE if the gap was found.
 */
gboolean scrollback_gap_get (scrollback_db *db, gint64 gap_id, scrollback_gap *out);

/**
 * Free the msgids owned by a scrollback_gap filled by scrollback_gap_get.
 * Does not free the struct itself.
 */
void scrollback_gap_clear (scrollback_gap *gap);

/**
 * Narrow a gap's bounds after a partial fill.  A new_start_ts/new_end_ts
 * of 0 (with the paired msgid NULL) means "keep this side unchanged".
 * Resets attempts and last_attempt to 0 -- progress re-earns a fast retry.
 *
 * Defense in depth: if the narrowing would invert the record
 * (start_ts >= end_ts), the row is deleted instead of left inverted --
 * an inverted gap isn't a real hole, and would otherwise re-request
 * forever since every shrink resets attempts/last_attempt.
 */
void scrollback_gap_shrink (scrollback_db *db, gint64 gap_id,
                            gint64 new_start_ts, const char *new_start_msgid,
                            gint64 new_end_ts, const char *new_end_msgid);

/**
 * Set a gap's state (SCROLLBACK_GAP_*).
 */
void scrollback_gap_set_state (scrollback_db *db, gint64 gap_id, int state);

/**
 * Record a fill attempt against a gap: increments attempts and sets
 * last_attempt to now.
 *
 * @return The new attempts value.
 */
int scrollback_gap_touch (scrollback_db *db, gint64 gap_id);

/**
 * Delete a gap row outright (e.g. a candidate proven not to be a real
 * hole).
 */
void scrollback_gap_delete (scrollback_db *db, gint64 gap_id);

/**
 * Forget a gap's msgid anchors (keeping its timestamps) and reset its
 * attempt counter/backoff, so the next fill request anchors on
 * timestamps.  Used when the server no longer resolves the msgids
 * (e.g. the flanking rows fell outside its retention window).
 *
 * @return TRUE if the row had at least one msgid to drop.
 */
gboolean scrollback_gap_drop_msgids (scrollback_db *db, gint64 gap_id);

/**
 * Put a channel's gaps (or one gap when gap_id > 0) back into the
 * witnessed state with a clean attempt counter and no msgid anchors, so
 * the fill machinery tries them again from scratch on timestamps.
 *
 * @return Number of rows reset.
 */
int scrollback_gap_reset (scrollback_db *db, const char *channel, gint64 gap_id);

/**
 * Retention cutoff for the network: now - the widest retention any linked
 * store advertises (evilnet/CHATHISTORYRETENTION).  In-memory only; set on
 * every 005 and re-announcement.  A gap whose end bound predates it is
 * "parked": no marker, no probe, but never dead-marked -- the bound can
 * widen when a store relinks, and the comparison then wakes it.  Only a
 * real answer (an empty, complete page over its span) closes a gap.
 */
void     scrollback_set_retention_cutoff (scrollback_db *db, gint64 cutoff_ts);
gint64   scrollback_get_retention_cutoff (scrollback_db *db);
gboolean scrollback_gap_is_parked (scrollback_db *db, const scrollback_gap *g);

/**
 * Delete a channel's CANDIDATE-state gaps, leaving WITNESSED and DEAD
 * rows untouched.  Used to clear the bootstrap heuristic's false
 * positives from query windows, where a multi-day silence between
 * conversations is normal and never indicates missed messages.
 *
 * @return Number of rows deleted.
 */
int scrollback_gap_delete_candidates (scrollback_db *db, const char *channel);

/**
 * Clear the channel's gap-bootstrap latch so scrollback_gap_bootstrap
 * will scan it again.
 */
void scrollback_gap_bootstrap_reset (scrollback_db *db, const char *channel);

/**
 * Position of a gap's end bound in the same (timestamp, id) ordinal
 * space scrollback_load_range uses: the count of messages strictly
 * before end_ts.  Used as a proximity margin, so the tie-at-end_ts
 * off-by-one (the end-flanking row itself) doesn't matter.
 */
int scrollback_gap_ordinal (scrollback_db *db, const char *channel, gint64 end_ts);

/**
 * One-shot per-channel bootstrap scan: walks stored messages in order and
 * records a SCROLLBACK_GAP_CANDIDATE for every adjacent pair whose
 * timestamps are >= threshold_secs apart.  Latches via
 * channels.gap_bootstrap_done so it never rescans a channel, even if it
 * found nothing the first time.
 *
 * @return Candidates recorded (>= 0), or -1 if already done, or on
 *         bad args / error.
 */
int scrollback_gap_bootstrap (scrollback_db *db, const char *channel,
                              gint64 threshold_secs);

/**
 * Begin a ref-counted transaction.  Multiple begin calls nest;
 * the actual SQL BEGIN only fires on the first.
 */
void scrollback_begin_transaction (scrollback_db *db);

/**
 * Commit a ref-counted transaction.  The actual SQL COMMIT only
 * fires when the outermost transaction ends (depth returns to 0).
 */
void scrollback_commit_transaction (scrollback_db *db);

/**
 * Initialize the scrollback subsystem.
 * Called once at startup.
 */
void scrollback_init (void);

/**
 * Shutdown the scrollback subsystem.
 * Called once at exit.
 */
void scrollback_shutdown (void);

#endif /* POXCHAT_SCROLLBACK_H */
