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
 * IRCv3 draft/chathistory implementation
 * See https://ircv3.net/specs/extensions/chathistory
 */

#ifndef POXCHAT_CHATHISTORY_H
#define POXCHAT_CHATHISTORY_H

#include "poxchat.h"

/* --- Request queue types --- */

typedef enum {
	CHREQ_LATEST,
	CHREQ_BEFORE,
	CHREQ_AFTER,
	CHREQ_AROUND,
	CHREQ_BETWEEN,
} chreq_type;

typedef enum {
	CHREQ_PRI_BACKGROUND = 0,	/* background older-history fetch */
	CHREQ_PRI_CATCHUP    = 1,	/* automatic post-connect catch-up */
	CHREQ_PRI_USER       = 2,	/* user-initiated (scroll-to-top) */
} chreq_priority;

typedef struct chreq_tag {
	chreq_type type;
	char *reference;			/* "msgid=xxx" or "timestamp=xxx" or "*" */
	char *end_ref;				/* BETWEEN only (NULL otherwise) */
	int limit;
	chreq_priority priority;
	unsigned int is_catchup:1;	/* part of automatic catch-up */
	unsigned int used_msgid:1;	/* used msgid reference (for FAIL fallback) */
	gint64 gap_id;				/* gap-fill: ledger row this request serves (0 = none) */
	int gap_dir;				/* gap-fill: approach direction (-1 above, +1 below) */
} chreq;

/* Default number of messages to request if not specified */
#define CHATHISTORY_DEFAULT_LIMIT 50

/* Maximum messages to request in a single batch */
#define CHATHISTORY_MAX_LIMIT 200

/* Seconds of clock skew compensation for catch-up (spec recommends 1-10s) */
#define CHATHISTORY_FUZZ_INTERVAL 5

/**
 * Request the most recent messages for a target.
 * Uses CHATHISTORY LATEST <target> <reference> <limit>
 *
 * @param sess Session/channel to request history for
 * @param reference Lower bound reference (NULL for "*", or "msgid=X" / "timestamp=Y")
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_latest (session *sess, const char *reference, int limit);

/**
 * Request messages before a reference point.
 * Uses CHATHISTORY BEFORE <target> <reference> <limit>
 *
 * @param sess Session/channel to request history for
 * @param reference Either a msgid or timestamp= reference
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_before (session *sess, const char *reference, int limit);

/**
 * Request messages after a reference point (for catch-up).
 * Uses CHATHISTORY AFTER <target> <reference> <limit>
 *
 * @param sess Session/channel to request history for
 * @param reference Either a msgid or timestamp= reference
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_after (session *sess, const char *reference, int limit);

/**
 * Request messages after a timestamp (convenience wrapper).
 * Uses CHATHISTORY AFTER <target> timestamp=<time> <limit>
 *
 * @param sess Session/channel to request history for
 * @param timestamp Unix timestamp to fetch messages after
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_after_timestamp (session *sess, time_t timestamp, int limit);

/**
 * Request messages after a msgid (convenience wrapper).
 * Uses CHATHISTORY AFTER <target> msgid=<id> <limit>
 *
 * @param sess Session/channel to request history for
 * @param msgid The message ID to fetch messages after
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_after_msgid (session *sess, const char *msgid, int limit);

/**
 * Request messages before a msgid (convenience wrapper).
 * Uses CHATHISTORY BEFORE <target> msgid=<id> <limit>
 *
 * @param sess Session/channel to request history for
 * @param msgid The message ID to fetch messages before
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_before_msgid (session *sess, const char *msgid, int limit);

/**
 * Request older history for scroll-to-load.
 * Uses oldest_msgid from session if available.
 *
 * @param sess Session/channel to request history for
 */
void chathistory_request_older (session *sess);

/**
 * Request messages around a reference point.
 * Uses CHATHISTORY AROUND <target> <reference> <limit>
 *
 * @param sess Session/channel to request history for
 * @param reference Either a msgid or timestamp= reference
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_around (session *sess, const char *reference, int limit);

/**
 * Request messages between two reference points.
 * Uses CHATHISTORY BETWEEN <target> <start_ref> <end_ref> <limit>
 *
 * @param sess Session/channel to request history for
 * @param start_ref Start reference point
 * @param end_ref End reference point
 * @param limit Maximum messages to request (0 = use default)
 */
void chathistory_request_between (session *sess, const char *start_ref,
                                  const char *end_ref, int limit);

/**
 * Request history for a recorded gap-ledger hole, anchored at the edge
 * the user approached from so adjacent content arrives first.
 * Uses CHATHISTORY BETWEEN (or BEFORE/AFTER when the server lacks
 * BETWEEN support), rate-limited by the ledger's attempts/last_attempt.
 *
 * @param sess Session/channel to request history for
 * @param gap_id Ledger row id (from scrollback_gap) identifying the hole
 * @param approach_dir -1 if the gap is above the viewport (scrolling up),
 *                     +1 if below
 * @return TRUE if a request was actually submitted (not dropped as a
 *         duplicate, suppressed, backed off, or blocked by catch-up)
 */
gboolean chathistory_request_gap_fill (session *sess, gint64 gap_id, int approach_dir);

/**
 * Request list of active conversations.
 * Uses CHATHISTORY TARGETS <start_ref> <end_ref> <limit>
 *
 * @param serv Server to request from
 * @param start_ref Start timestamp reference
 * @param end_ref End timestamp reference
 * @param limit Maximum targets to return (0 = use default)
 */
void chathistory_request_targets (server *serv, const char *start_ref,
                                  const char *end_ref, int limit);

/**
 * Process a completed chathistory batch.
 * Called from inbound_batch_end() when batch type is "chathistory".
 *
 * @param serv Server the batch came from
 * @param batch The completed batch info
 */
void chathistory_process_batch (server *serv, batch_info *batch);

/**
 * TRUE if this chathistory batch was not requested by us (server-driven
 * replay: nested in a bouncer-replay wrapper, or no request in flight for
 * the session).
 *
 * @param serv Server the batch came from
 * @param batch The completed batch info
 * @param sess Session the batch's target resolved to
 */
gboolean chathistory_batch_is_unsolicited (server *serv, batch_info *batch,
                                           session *sess);

/**
 * Put sess into LATEST-phase catch-up state without sending anything.
 *
 * @param sess Session to adopt the server-driven replay for
 * @return TRUE only if the session entered the phase and took a
 *         chathistory_latest_pending slot, which the caller must hand back
 *         when the batch finishes.  FALSE when this client does no
 *         chathistory of its own (no server support, auto catch-up off, or
 *         suppressed after repeated FAILs — the same guard as
 *         chathistory_start_catchup), when a catch-up loop of our own is
 *         already running, or when the session's history is exhausted — the
 *         batch is then plain history and must leave catch-up state alone.
 */
gboolean chathistory_begin_unsolicited_catchup (session *sess);

/**
 * Called at the START of an evilnet.github.io/bouncer-replay wrapper: the
 * point at which the server-driven replay stops being an expectation and
 * becomes a fact.  Sets persistence_replay_seen (so
 * persistence_server_drives_replay goes FALSE), cancels the provisional
 * deferred-catch-up timer, and drops the deferred CHATHISTORY TARGETS.
 *
 * @param serv Server the wrapper came from
 */
void chathistory_replay_wrapper_begin (server *serv);

/**
 * Called at the END of an evilnet.github.io/bouncer-replay wrapper: the
 * single "replay complete" point for the whole missed-message block.
 *
 * @param serv Server the wrapper came from
 */
void chathistory_replay_wrapper_end (server *serv);

/**
 * Handle a FAIL CHATHISTORY response from the server.
 * Clears loading state and attempts fallback (msgid → timestamp → LATEST).
 *
 * @param serv Server the FAIL came from
 * @param code FAIL code (e.g. "INVALID_PARAMS")
 * @param context Optional context from FAIL message (may be target channel)
 */
void chathistory_handle_fail (server *serv, const char *code, const char *context);

/**
 * Parse CHATHISTORY ISUPPORT token.
 * Format: CHATHISTORY=<limit>
 *
 * @param serv Server to update
 * @param value ISUPPORT value (the part after =)
 */
void chathistory_parse_isupport (server *serv, const char *value);

/**
 * ISUPPORT token advertising how long the server keeps history.
 * Format: evilnet/CHATHISTORYRETENTION=<seconds>
 */
#define CHATHISTORY_RETENTION_TOKEN "evilnet/CHATHISTORYRETENTION"

/**
 * Parse the retention token into serv->chathistory_retention_secs and
 * dead-mark every ledger gap the server can no longer fill (end bound
 * older than now - retention).  value NULL or the token's removal form
 * clears it back to unknown.
 */
void chathistory_parse_retention (server *serv, const char *value);

/**
 * Oldest timestamp the server can still serve, or 0 if retention is
 * unknown.
 */
gint64 chathistory_retention_cutoff (server *serv);

/**
 * Update session's msgid tracking.
 * Should be called when displaying messages to track oldest/newest msgids.
 *
 * @param sess Session to update
 * @param msgid The message ID being displayed
 * @param is_history TRUE if this is a historical message (prepend to buffer)
 */
void chathistory_track_msgid (session *sess, const char *msgid, gboolean is_history);
void chathistory_track_msgid_ts (session *sess, const char *msgid, time_t timestamp, gboolean is_history);

/**
 * Check if a message with this msgid+timestamp has already been displayed.
 * Uses both fields because some servers reuse msgids after restarts.
 *
 * @param sess Session to check
 * @param msgid The message ID to check
 * @param timestamp The message timestamp (0 to match by msgid alone)
 * @return TRUE if msgid is known (duplicate), FALSE if new
 */
gboolean chathistory_is_duplicate_msgid (session *sess, const char *msgid, time_t timestamp);

/**
 * Check if a session can request more history (has older messages on server).
 *
 * @param sess Session to check
 * @return TRUE if more history may be available
 */
gboolean chathistory_can_request_more (session *sess);

/**
 * Start background history fetching for a session.
 * Gradually fetches older history to populate scrollback.
 *
 * @param sess Session to fetch background history for
 */
void chathistory_start_background_fetch (session *sess);

/**
 * Stop background history fetching for a session.
 * Called when leaving a channel or disconnecting.
 *
 * @param sess Session to stop background fetching for
 */
void chathistory_stop_background_fetch (session *sess);

/**
 * Start catch-up for a session (join or TARGETS).
 * Uses CHATHISTORY LATEST with scrollback refs as lower bound.
 * Pages backward with BEFORE if the gap is larger than one batch.
 *
 * @param sess Session to catch up
 */
void chathistory_start_catchup (session *sess);

/**
 * Cancel an in-progress catch-up (disconnect/part/kick cleanup).
 *
 * @param sess Session to cancel catch-up for
 */
void chathistory_cancel_catchup (session *sess);

/**
 * Process a completed draft/chathistory-targets batch.
 * Creates/finds query sessions for DM targets and starts catch-up.
 *
 * @param serv Server the batch came from
 * @param batch The completed batch info
 */
void chathistory_process_targets_batch (server *serv, batch_info *batch);

/**
 * Send CHATHISTORY TARGETS on reconnect to discover missed DMs.
 * Only fires if last_disconnect_time > 0 (not first connect).
 *
 * @param serv Server to request from
 */
void chathistory_request_targets_on_reconnect (server *serv);

/* --- Chunked batch processing --- */

#define CHATHISTORY_CHUNK_SIZE 50

/**
 * Cancel any in-progress chunked batch processing for a session.
 * Called from session_free to prevent use-after-free in idle callbacks.
 */
void chathistory_cancel_chunk_processing (session *sess);

/* --- Deferred catch-up coordination --- */

#define CHATHISTORY_DEFERRED_DELAY  2000	/* ms after last 366 before sending LATEST */
#define CHATHISTORY_BEFORE_LIMIT    50		/* messages per BEFORE catch-up request */
#define CHATHISTORY_BEFORE_DELAY    3		/* seconds between BEFORE catch-up requests */
#define CHATHISTORY_SANITY_LIMIT    5000	/* max total messages in BEFORE catch-up per session */

/**
 * Schedule deferred LATEST requests for all channels on a server.
 * Called from end-of-NAMES (366) handler instead of chathistory_start_catchup.
 * Resets a 2-second timer — fires after all JOINs settle.
 */
void chathistory_schedule_deferred (server *serv);

/**
 * Check if BEFORE catch-up should start/resume for the active session.
 * Called after all LATEST batches complete, after a BEFORE batch completes,
 * and on tab switch.
 */
void chathistory_check_before_catchup (server *serv);

/**
 * Notify chathistory system of a tab switch.
 * If the active session changes, BEFORE catch-up pauses on the old tab
 * and starts/resumes on the new tab.
 */
void chathistory_notify_tab_switch (session *new_sess);

/* --- Request queue management --- */

/**
 * Submit a chathistory request to the per-session queue.
 * Handles deduplication and priority. If no request is in-flight,
 * dispatches immediately. Otherwise queues as pending.
 * Takes ownership of req in all cases (caller must not free) --
 * on a dedup/priority drop, req is freed here rather than handed back.
 *
 * @return TRUE if the request was dispatched immediately or queued as
 *         pending; FALSE if it was dropped (freed) as a duplicate or by
 *         losing the pending-slot priority contest.  Callers that need
 *         to know whether a request is genuinely in flight (e.g. before
 *         touching bookkeeping tied to it) must check this return value
 *         rather than assuming submission always succeeds.
 */
gboolean chathistory_submit (session *sess, chreq *req);

/**
 * Mark the active request as complete and dispatch any pending request.
 * Called from finish_batch_processing, empty batch handler, and FAIL handler.
 */
void chathistory_request_complete (session *sess);

/**
 * Free a chreq and its strings.
 */
void chreq_free (chreq *req);

/**
 * Create a chreq. Caller passes to chathistory_submit().
 */
chreq *chreq_new (chreq_type type, const char *reference, const char *end_ref,
                   int limit, chreq_priority priority,
                   gboolean is_catchup, gboolean used_msgid);

/**
 * Free the request queue for a session (disconnect/session_free cleanup).
 */
void chathistory_queue_free (session *sess);

#endif /* POXCHAT_CHATHISTORY_H */
