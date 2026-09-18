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

#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "poxchat.h"
#include "poxchatc.h"
#include "chathistory.h"
#include "server.h"
#include "inbound.h"
#include "text.h"
#include "fe.h"
#include "proto-irc.h"
#include "modes.h"
#include "scrollback.h"
#include "persistence.h"

/* Forward declarations */
static void schedule_background_fetch (session *sess);
static void schedule_before_catchup (server *serv);
static void catchup_enter_latest_phase (session *sess);
static void send_reconnect_targets_request (server *serv);
static void chathistory_replay_mode (session *sess, char *nick,
                                     char *mode_str, char **params,
                                     int param_count,
                                     const message_tags_data *tags_data);

/* draft/chathistory-context: server-appended companion message (react,
 * redact, edit...) that follows its parent and doesn't count toward limits. */
static gboolean
batch_msg_is_context (const batch_message *m)
{
	return m->tags && g_hash_table_contains (m->tags, "draft/chathistory-context");
}

/* Format a timestamp message reference the way the spec spells it:
 * timestamp=YYYY-MM-DDThh:mm:ss.sssZ (UTC).  Servers are not required
 * to accept a bare epoch and Nefarious does not. */
static void
chathistory_ts_ref (char *buf, gsize len, gint64 ts)
{
	GDateTime *dt = g_date_time_new_from_unix_utc (ts);
	char *iso = dt ? g_date_time_format (dt, "%Y-%m-%dT%H:%M:%S") : NULL;

	g_snprintf (buf, (gulong) len, "timestamp=%s.000Z", iso ? iso : "1970-01-01T00:00:00");
	g_free (iso);
	if (dt)
		g_date_time_unref (dt);
}

static char *
chathistory_ts_ref_dup (gint64 ts)
{
	char buf[64];
	chathistory_ts_ref (buf, sizeof (buf), ts);
	return g_strdup (buf);
}

/* Get effective limit for a CHATHISTORY request.
 * Prefers the server-advertised ISUPPORT CHATHISTORY=N value (the server is
 * telling us the optimal batch size).  Falls back to CHATHISTORY_DEFAULT_LIMIT
 * when the server doesn't advertise one, and never exceeds CHATHISTORY_MAX_LIMIT
 * as a safety cap. */
static int
get_effective_limit (server *serv, int requested)
{
	int limit;

	/* Use server-advertised limit when available, otherwise the caller's value */
	if (serv->chathistory_limit > 0)
		limit = serv->chathistory_limit;
	else if (requested > 0)
		limit = requested;
	else
		limit = CHATHISTORY_DEFAULT_LIMIT;

	if (limit > CHATHISTORY_MAX_LIMIT)
		limit = CHATHISTORY_MAX_LIMIT;

	return limit;
}

/* Get the target name for a session (channel or nick for query) */
static const char *
get_target_name (session *sess)
{
	if (sess->type == SESS_CHANNEL)
		return sess->channel;
	else if (sess->type == SESS_DIALOG)
		return sess->channel; /* For dialogs, channel holds the nick */
	else
		return NULL;
}

/* --- Request queue infrastructure --- */

chreq *
chreq_new (chreq_type type, const char *reference, const char *end_ref,
           int limit, chreq_priority priority,
           gboolean is_catchup, gboolean used_msgid)
{
	chreq *req = g_new0 (chreq, 1);
	req->type = type;
	req->reference = g_strdup (reference);
	req->end_ref = g_strdup (end_ref);
	req->limit = limit;
	req->priority = priority;
	req->is_catchup = is_catchup ? 1 : 0;
	req->used_msgid = used_msgid ? 1 : 0;
	return req;
}

void
chreq_free (chreq *req)
{
	if (!req)
		return;
	g_free (req->reference);
	g_free (req->end_ref);
	g_free (req);
}

/* Check if two requests are duplicates (same type + same reference[s]) */
static gboolean
chreq_is_dup (const chreq *a, const chreq *b)
{
	if (!a || !b)
		return FALSE;
	if (a->type != b->type)
		return FALSE;
	if (g_strcmp0 (a->reference, b->reference) != 0)
		return FALSE;
	return g_strcmp0 (a->end_ref, b->end_ref) == 0;
}

/* Dispatch a request onto the wire immediately. Sets session flags. */
static void
chathistory_dispatch_now (session *sess, chreq *req)
{
	server *serv = sess->server;
	const char *target;
	int effective_limit;

	target = get_target_name (sess);
	if (!target || !target[0])
	{
		chreq_free (req);
		return;
	}

	if (!serv->have_chathistory || !serv->connected || serv->chathistory_suppressed)
	{
		chreq_free (req);
		return;
	}

	sess->ch_active = req;

	/* Set compatibility flags from request */
	sess->history_loading = TRUE;
	sess->history_request_is_before = (req->type == CHREQ_BEFORE);
	sess->history_request_is_after = (req->type == CHREQ_AFTER);
	sess->history_request_used_msgid = req->used_msgid;

	effective_limit = get_effective_limit (serv, req->limit);

	switch (req->type)
	{
	case CHREQ_LATEST:
		{
			const char *ref = (req->reference && req->reference[0]) ? req->reference : "*";
			tcp_sendf_labeled_tracked (serv, "CHATHISTORY", target,
			                           "CHATHISTORY LATEST %s %s %d\r\n",
			                           target, ref, effective_limit);
		}
		break;

	case CHREQ_BEFORE:
		tcp_sendf_labeled_tracked (serv, "CHATHISTORY", target,
		                           "CHATHISTORY BEFORE %s %s %d\r\n",
		                           target, req->reference, effective_limit);
		break;

	case CHREQ_AFTER:
		tcp_sendf_labeled_tracked (serv, "CHATHISTORY", target,
		                           "CHATHISTORY AFTER %s %s %d\r\n",
		                           target, req->reference, effective_limit);
		break;

	case CHREQ_AROUND:
		tcp_sendf_labeled_tracked (serv, "CHATHISTORY", target,
		                           "CHATHISTORY AROUND %s %s %d\r\n",
		                           target, req->reference, effective_limit);
		break;

	case CHREQ_BETWEEN:
		tcp_sendf_labeled_tracked (serv, "CHATHISTORY", target,
		                           "CHATHISTORY BETWEEN %s %s %s %d\r\n",
		                           target, req->reference, req->end_ref,
		                           effective_limit);
		break;
	}
}

gboolean
chathistory_submit (session *sess, chreq *req)
{
	if (!sess || !req)
	{
		chreq_free (req);
		return FALSE;
	}

	/* Dedup against active request */
	if (chreq_is_dup (sess->ch_active, req))
	{
		chreq_free (req);
		return FALSE;
	}

	/* Dedup against pending request */
	if (chreq_is_dup (sess->ch_pending, req))
	{
		chreq_free (req);
		return FALSE;
	}

	/* No active request → dispatch immediately */
	if (!sess->ch_active)
	{
		chathistory_dispatch_now (sess, req);
		return TRUE;
	}

	/* Active request exists → queue as pending */
	if (!sess->ch_pending || req->priority > sess->ch_pending->priority)
	{
		/* Replace lower-priority pending with this request */
		chreq_free (sess->ch_pending);
		sess->ch_pending = req;
		return TRUE;
	}
	else
	{
		/* Equal or lower priority — drop */
		chreq_free (req);
		return FALSE;
	}
}

void
chathistory_request_complete (session *sess)
{
	chreq *pending;

	if (!sess)
		return;

	chreq_free (sess->ch_active);
	sess->ch_active = NULL;
	sess->history_loading = FALSE;

	/* Dispatch pending request if any */
	pending = sess->ch_pending;
	if (pending)
	{
		sess->ch_pending = NULL;
		chathistory_dispatch_now (sess, pending);
	}
}

void
chathistory_queue_free (session *sess)
{
	if (!sess)
		return;
	chreq_free (sess->ch_active);
	sess->ch_active = NULL;
	chreq_free (sess->ch_pending);
	sess->ch_pending = NULL;
	sess->history_loading = FALSE;
}

/* --- Request API --- */

/* Infer request priority from session state */
static chreq_priority
infer_priority (session *sess)
{
	if (sess->background_history_active)
		return CHREQ_PRI_BACKGROUND;
	if (sess->catchup_in_progress)
		return CHREQ_PRI_CATCHUP;
	return CHREQ_PRI_USER;
}

void
chathistory_request_latest (session *sess, const char *reference, int limit)
{
	chreq *req;
	gboolean used_msgid;

	if (!sess->server->have_chathistory || !sess->server->connected)
		return;
	if (!get_target_name (sess))
		return;

	used_msgid = (reference && g_str_has_prefix (reference, "msgid="));
	req = chreq_new (CHREQ_LATEST, reference, NULL, limit,
	                  infer_priority (sess), sess->catchup_in_progress, used_msgid);
	chathistory_submit (sess, req);
}

void
chathistory_request_before (session *sess, const char *reference, int limit)
{
	chreq *req;
	gboolean used_msgid;

	if (!sess->server->have_chathistory || !sess->server->connected)
		return;
	if (!get_target_name (sess))
		return;
	if (!reference || !reference[0])
		return;

	used_msgid = g_str_has_prefix (reference, "msgid=");
	req = chreq_new (CHREQ_BEFORE, reference, NULL, limit,
	                  infer_priority (sess), sess->catchup_in_progress, used_msgid);
	chathistory_submit (sess, req);
}

void
chathistory_request_after (session *sess, const char *reference, int limit)
{
	chreq *req;
	gboolean used_msgid;

	if (!sess->server->have_chathistory || !sess->server->connected)
		return;
	if (!get_target_name (sess))
		return;
	if (!reference || !reference[0])
		return;

	used_msgid = g_str_has_prefix (reference, "msgid=");
	req = chreq_new (CHREQ_AFTER, reference, NULL, limit,
	                  infer_priority (sess), sess->catchup_in_progress, used_msgid);
	chathistory_submit (sess, req);
}

void
chathistory_request_around (session *sess, const char *reference, int limit)
{
	chreq *req;

	if (!sess->server->have_chathistory || !sess->server->connected)
		return;
	if (!get_target_name (sess))
		return;
	if (!reference || !reference[0])
		return;

	req = chreq_new (CHREQ_AROUND, reference, NULL, limit,
	                  infer_priority (sess), sess->catchup_in_progress, FALSE);
	chathistory_submit (sess, req);
}

void
chathistory_request_between (session *sess, const char *start_ref,
                             const char *end_ref, int limit)
{
	chreq *req;

	if (!sess->server->have_chathistory || !sess->server->connected)
		return;
	if (!get_target_name (sess))
		return;
	if (!start_ref || !start_ref[0] || !end_ref || !end_ref[0])
		return;

	req = chreq_new (CHREQ_BETWEEN, start_ref, end_ref, limit,
	                  infer_priority (sess), sess->catchup_in_progress, FALSE);
	chathistory_submit (sess, req);
}

void
chathistory_request_targets (server *serv, const char *start_ref,
                             const char *end_ref, int limit)
{
	int effective_limit;

	if (!serv->have_chathistory || !serv->connected)
		return;

	if (!start_ref || !start_ref[0] || !end_ref || !end_ref[0])
		return;

	effective_limit = get_effective_limit (serv, limit);

	tcp_sendf_labeled_tracked (serv, "CHATHISTORY", NULL,
	                           "CHATHISTORY TARGETS %s %s %d\r\n",
	                           start_ref, end_ref, effective_limit);
}

void
chathistory_request_after_timestamp (session *sess, time_t timestamp, int limit)
{
	char ref[64];

	/* Format timestamp reference per IRCv3 spec.
	 * Use gint64 cast — time_t is 64-bit on Windows x64 but long is 32-bit. */
	chathistory_ts_ref (ref, sizeof (ref), (gint64) timestamp);
	chathistory_request_after (sess, ref, limit);
}

void
chathistory_request_after_msgid (session *sess, const char *msgid, int limit)
{
	char *ref;

	if (!msgid || !msgid[0])
		return;

	/* Format msgid reference per IRCv3 spec */
	ref = g_strdup_printf ("msgid=%s", msgid);

	chathistory_request_after (sess, ref, limit);

	g_free (ref);
}

void
chathistory_request_before_msgid (session *sess, const char *msgid, int limit)
{
	char *ref;

	if (!msgid || !msgid[0])
		return;

	/* Format msgid reference per IRCv3 spec */
	ref = g_strdup_printf ("msgid=%s", msgid);
	chathistory_request_before (sess, ref, limit);

	g_free (ref);
}

void
chathistory_request_older (session *sess)
{
	const char *reference = NULL;
	char *ref;
	chreq *req;

	if (!sess || !sess->server || !sess->server->have_chathistory)
		return;
	if (sess->history_exhausted)
		return;

	/* Try oldest_msgid first (tracks the oldest message from chathistory batches) */
	if (sess->oldest_msgid && sess->oldest_msgid[0])
		reference = sess->oldest_msgid;
	/* Fall back to scrollback_oldest_msgid (from loaded scrollback) */
	else if (sess->scrollback_oldest_msgid && sess->scrollback_oldest_msgid[0])
		reference = sess->scrollback_oldest_msgid;

	if (!reference)
		return;

	/* Submit as USER priority — preempts queued catch-up/background requests.
	 * The queue handles dedup and serialization, no flag hacking needed. */
	ref = g_strdup_printf ("msgid=%s", reference);
	req = chreq_new (CHREQ_BEFORE, ref, NULL, prefs.hex_irc_chathistory_lines,
	                  CHREQ_PRI_USER, FALSE, TRUE);
	chathistory_submit (sess, req);
	g_free (ref);
}

gboolean chathistory_request_gap_fill (session *sess, gint64 gap_id, int approach_dir);

/* A gap-fill request anchored on msgids came back empty or FAILed.
 * Servers answer an unknown msgid with an empty batch rather than an
 * error (Nefarious: the flanking message aged out of retention, so the
 * reference resolves to nothing even though the span itself may still
 * be served).  Before dead-marking, forget the msgids and re-ask with
 * the timestamps we also hold.  Returns TRUE if a retry was submitted;
 * the caller then leaves the ledger state alone. */
static gboolean
gap_fill_retry_with_timestamps (session *sess, gint64 gap_id, int gap_dir)
{
	const char *network;
	scrollback_db *db;

	if (!sess || !sess->server || gap_id <= 0 ||
	    !sess->history_request_used_msgid ||
	    sess->server->chathistory_suppressed)
		return FALSE;

	network = server_get_network (sess->server, FALSE);
	db = network ? scrollback_open (network) : NULL;
	if (!db || !scrollback_gap_drop_msgids (db, gap_id))
		return FALSE;

	fe_gap_updated (sess, gap_id);
	return chathistory_request_gap_fill (sess, gap_id, gap_dir);
}

/* Gap fill: request history for a recorded hole, anchored at the edge
 * the user approached from so adjacent content arrives first.
 * approach_dir: -1 = gap is above the viewport (user scrolling up),
 * +1 = below.  Rate limiting lives in the ledger (attempts/last_attempt,
 * reset by any batch that shrinks the gap).
 * Returns TRUE only when chathistory_submit() reports the request was
 * actually dispatched or queued as pending (not dropped as a duplicate
 * or by losing the pending-slot priority contest) — callers use this to
 * decide whether the gap is genuinely in flight. */
gboolean
chathistory_request_gap_fill (session *sess, gint64 gap_id, int approach_dir)
{
	const char *network;
	scrollback_db *db;
	scrollback_gap gap;
	char *near_ref, *far_ref;
	gboolean near_is_msgid, far_is_msgid;
	chreq *req;
	gint64 wait;

	if (!sess || !sess->server || !sess->server->have_chathistory ||
	    sess->server->chathistory_suppressed)
		return FALSE;
	if (!prefs.hex_irc_gapfill || gap_id <= 0)
		return FALSE;
	/* Belt for queue contention: chathistory_process_batch's
	 * is_catchup/active_gap_id split covers the race where catchup
	 * starts while a gap fill is already in flight, but don't even
	 * start a fresh gap-fill request while catch-up owns the queue. */
	if (sess->catchup_in_progress)
		return FALSE;

	network = server_get_network (sess->server, FALSE);
	db = network ? scrollback_open (network) : NULL;
	if (!db || !scrollback_gap_get (db, gap_id, &gap))
		return FALSE;

	if (gap.state == SCROLLBACK_GAP_DEAD)
	{
		scrollback_gap_clear (&gap);
		return FALSE;
	}

	/* Parked: ends before the network's retention cutoff, so no linked
	 * store can serve it right now.  Don't spend a request -- and don't
	 * dead-mark: the bound can widen (a store relinks) and this gap is
	 * then probed normally.  Retention is a hint, never a verdict. */
	if (scrollback_gap_is_parked (db, &gap))
	{
		scrollback_gap_clear (&gap);
		return FALSE;
	}

	/* Ledger backoff: 5s per prior attempt, capped at 60s */
	wait = 5 * gap.attempts;
	if (wait > 60)
		wait = 60;
	if (gap.last_attempt > 0 && time (NULL) - gap.last_attempt < wait)
	{
		scrollback_gap_clear (&gap);
		return FALSE;
	}

	if (approach_dir < 0)
	{
		/* Gap above viewport: fill newest-first from its end bound */
		near_is_msgid = (gap.end_msgid && gap.end_msgid[0]);
		near_ref = near_is_msgid
			? g_strdup_printf ("msgid=%s", gap.end_msgid)
			: chathistory_ts_ref_dup (gap.end_ts);
		far_is_msgid = (gap.start_msgid && gap.start_msgid[0]);
		far_ref = far_is_msgid
			? g_strdup_printf ("msgid=%s", gap.start_msgid)
			: chathistory_ts_ref_dup (gap.start_ts);
	}
	else
	{
		near_is_msgid = (gap.start_msgid && gap.start_msgid[0]);
		near_ref = near_is_msgid
			? g_strdup_printf ("msgid=%s", gap.start_msgid)
			: chathistory_ts_ref_dup (gap.start_ts);
		far_is_msgid = (gap.end_msgid && gap.end_msgid[0]);
		far_ref = far_is_msgid
			? g_strdup_printf ("msgid=%s", gap.end_msgid)
			: chathistory_ts_ref_dup (gap.end_ts);
	}

	if (sess->server->chathistory_between_unsupported)
	{
		/* Fallback pagination anchored at the near edge; termination is
		 * the ledger shrink logic (bounds clamp per batch). */
		req = chreq_new (approach_dir < 0 ? CHREQ_BEFORE : CHREQ_AFTER,
		                 near_ref, NULL, prefs.hex_irc_chathistory_lines,
		                 CHREQ_PRI_USER, FALSE, near_is_msgid);
	}
	else
	{
		/* used_msgid covers either anchor: an unresolvable msgid on
		 * the far end makes the server return empty just as surely. */
		req = chreq_new (CHREQ_BETWEEN, near_ref, far_ref,
		                 prefs.hex_irc_chathistory_lines,
		                 CHREQ_PRI_USER, FALSE, near_is_msgid || far_is_msgid);
	}
	req->gap_id = gap_id;
	req->gap_dir = approach_dir;

	/* Submit first and only touch the ledger if chathistory_submit
	 * reports the request actually went out (dispatched now or queued
	 * as pending).  An exact-duplicate pre-check here isn't enough:
	 * chathistory_submit can also silently drop and free req by losing
	 * the pending-slot priority contest (an active request plus an
	 * equal-or-higher-priority ch_pending already queued) even when req
	 * isn't a duplicate of either.  A dropped request must not count as
	 * an attempt (ledger touch) or be reported as submitted — both would
	 * desync in_flight and the attempts/backoff counter from what
	 * actually went out on the wire.  Touching after submit is fine
	 * either way: it's bookkeeping, not a precondition, and nothing can
	 * re-enter between the two calls in this single-threaded GLib main
	 * loop. */
	if (!chathistory_submit (sess, req))
	{
		g_free (near_ref);
		g_free (far_ref);
		scrollback_gap_clear (&gap);
		return FALSE;
	}

	scrollback_gap_touch (db, gap_id);

	g_free (near_ref);
	g_free (far_ref);
	scrollback_gap_clear (&gap);
	return TRUE;
}

/* Compare batch messages by timestamp for sorting (ascending order) */
static gint
compare_batch_msg_timestamp (gconstpointer a, gconstpointer b)
{
	const batch_message *msg_a = a;
	const batch_message *msg_b = b;

	if (msg_a->timestamp < msg_b->timestamp)
		return -1;
	if (msg_a->timestamp > msg_b->timestamp)
		return 1;
	return 0;
}

/* Find session for a target */
static session *
find_session_for_target (server *serv, const char *target)
{
	session *sess;

	if (!target || !target[0])
		return NULL;

	/* Try as channel first */
	sess = find_channel (serv, (char *)target);
	if (sess)
		return sess;

	/* Try as dialog */
	sess = find_dialog (serv, (char *)target);

	return sess;
}

/* TRUE when this batch is nested inside an evilnet.github.io/bouncer-replay
 * wrapper, i.e. it is one leg of the server's draft/persistence catch-up
 * replay.  Distinct from "unsolicited" below because a wrapper child answers
 * no request of ours even when one is in flight, so it must not be attributed
 * to that request's gap fill. */
static gboolean
batch_in_replay_wrapper (server *serv, batch_info *batch)
{
	batch_info *outer;

	if (!serv || !batch || !batch->outer_batch || !serv->active_batches)
		return FALSE;

	outer = g_hash_table_lookup (serv->active_batches, batch->outer_batch);

	return outer && outer->type &&
	       g_ascii_strcasecmp (persistence_strip_namespace (outer->type),
	                           "bouncer-replay") == 0;
}

/* TRUE when this chathistory batch is server-driven rather than a reply to
 * a request of ours: either it is nested inside an
 * evilnet.github.io/bouncer-replay wrapper (draft/persistence catch-up
 * replay), or we simply have nothing in flight for the session.  Such a
 * batch must not pop the session's request queue, and its
 * draft/chathistory-end means "this target's replay is complete", not
 * "no older history exists". */
gboolean
chathistory_batch_is_unsolicited (server *serv, batch_info *batch, session *sess)
{
	if (!serv || !batch || !sess)
		return FALSE;

	if (batch_in_replay_wrapper (serv, batch))
		return TRUE;

	return sess->ch_active == NULL && !sess->history_loading;
}

/* Adopt an unsolicited batch as this session's LATEST phase: the witness
 * and gap-bridging logic in finish_batch_processing only runs for a
 * catch-up batch, and the BEFORE pass that follows needs the same anchors
 * our own LATEST would have set.
 *
 * Returns TRUE only when the session actually entered the phase and took a
 * chathistory_latest_pending slot.  The caller must treat the batch as
 * catch-up — and hand the slot back when it finishes — if and only if this
 * returned TRUE; otherwise the batch is plain history rows and must leave
 * every catch-up phase field alone, because the loop it would be stepping
 * belongs to a request the batch does not answer. */
gboolean
chathistory_begin_unsolicited_catchup (session *sess)
{
	server *serv;

	if (!sess || !sess->server)
		return FALSE;
	serv = sess->server;
	/* Adoption is not free: it opens the very catch-up loop our own
	 * LATEST would have opened, and that loop's BEFORE phase goes on to
	 * send CHATHISTORY of its own.  A client that does no chathistory —
	 * server without the cap, auto-catch-up turned off, or the server
	 * having FAILed us into suppression — must not enter it, or the
	 * pending slot it takes gates a BEFORE pass that can never run.
	 * Same guard as chathistory_start_catchup (whose requests
	 * chathistory_dispatch_now drops while suppressed anyway); the
	 * batch's rows still land, just as plain history. */
	if (!serv->have_chathistory || !prefs.hex_irc_chathistory_auto ||
	    serv->chathistory_suppressed)
		return FALSE;
	if (sess->catchup_in_progress)
		return FALSE;		/* already in our own LATEST/BEFORE loop — leave it */
	if (sess->history_exhausted)
		return FALSE;		/* nothing left to page — send_deferred_latest
					 * declines the same session for the same reason */
	catchup_enter_latest_phase (sess);
	serv->chathistory_latest_pending++;
	return TRUE;
}

void
chathistory_replay_wrapper_begin (server *serv)
{
	gboolean was_provisional;

	if (!serv)
		return;

	/* The replay we consented to at ATTACH time is real.  Until this
	 * point persistence_server_drives_replay was only our expectation:
	 * the server may have replayed nothing at all and said nothing about
	 * it (REPLAY OFF, policy, no gap since the cursor).  Everything that
	 * held off on that expectation now stands down for good — the gate
	 * drops, the provisional grace timer goes away, and the TARGETS
	 * request we deferred is dropped because the replay covers PM
	 * correspondents itself. */
	was_provisional = persistence_server_drives_replay (serv);
	serv->persistence_replay_seen = TRUE;

	/* Only the timer this gate armed.  A wrapper on a connection that
	 * deferred nothing — no profile, so no cursor, so no gate — must not
	 * silently swallow an ordinary post-JOIN fan-out that happens to be
	 * pending; the same holds for any later wrapper, whose grace timer
	 * was cancelled by the first. */
	if (!was_provisional)
		return;

	if (serv->chathistory_start_timer > 0)
	{
		g_source_remove (serv->chathistory_start_timer);
		serv->chathistory_start_timer = 0;
	}
	serv->chathistory_targets_deferred = FALSE;
}

void
chathistory_replay_wrapper_end (server *serv)
{
	/* Every nested chathistory batch already ran its LATEST-phase
	 * completion (decrementing latest_pending).  If the wrapper had no
	 * chathistory children nothing is pending; kick the eager BEFORE pass
	 * exactly once either way. */
	if (serv->chathistory_latest_pending == 0)
		chathistory_check_before_catchup (serv);
}

/* Complete a catch-up operation.  Inserts separator, clears state, starts
 * background fetch.  Called when the catch-up loop finishes (either the gap
 * is filled, the server returned empty, or all messages were duplicates). */
static void
finish_catchup (session *sess)
{
	sess->catchup_in_progress = FALSE;
	sess->catchup_is_before = FALSE;
	sess->history_catchup_stale_count = 0;
	sess->history_catchup_retrieved = 0;
	sess->catchup_gap_id = 0;

	/* Catch-up sets oldest_msgid to its oldest batch message, which may be
	 * newer than the DB's oldest.  Reset to the DB's oldest so that
	 * scroll-to-load BEFORE requests reference the true oldest known message
	 * rather than fetching history the DB already has. */
	if (sess->scrollback_oldest_msgid && sess->scrollback_oldest_msgid[0])
	{
		g_free (sess->oldest_msgid);
		sess->oldest_msgid = g_strdup (sess->scrollback_oldest_msgid);
	}

	fe_reset_scroll_top_backoff (sess);

	/* If this session was the active BEFORE target, clear it so
	 * check_before_catchup can pick the next session. */
	if (sess->server && sess->server->chathistory_before_sess == sess)
		sess->server->chathistory_before_sess = NULL;
}

void
chathistory_start_catchup (session *sess)
{
	server *serv;

	if (!sess || !sess->server)
		return;

	serv = sess->server;

	if (!prefs.hex_irc_chathistory_auto || !serv->have_chathistory)
		return;

	if (sess->history_loading || sess->catchup_in_progress)
		return;

	sess->catchup_in_progress = TRUE;

	/* Choose LATEST reference based on available scrollback */
	if (sess->scrollback_newest_msgid && sess->scrollback_newest_msgid[0])
	{
		char *ref = g_strdup_printf ("msgid=%s", sess->scrollback_newest_msgid);
		chathistory_request_latest (sess, ref, prefs.hex_irc_chathistory_lines);
		g_free (ref);
	}
	else if (sess->scrollback_newest_time > 0)
	{
		char ref[64];
		chathistory_ts_ref (ref, sizeof (ref), (gint64) sess->scrollback_newest_time);
		chathistory_request_latest (sess, ref, prefs.hex_irc_chathistory_lines);
	}
	else
	{
		/* No scrollback — get most recent context */
		chathistory_request_latest (sess, NULL, prefs.hex_irc_chathistory_lines);
	}
}

void
chathistory_cancel_catchup (session *sess)
{
	if (!sess)
		return;

	sess->catchup_in_progress = FALSE;
	chathistory_queue_free (sess);
	chathistory_stop_background_fetch (sess);
}

/* Determine if a channel mode takes an argument, replicating modes.c logic.
 * type A (list modes like b,e,I) always take args.
 * type B (like k) always take args.
 * type C (like l) take args on + only.
 * type D (like n,t) never take args.
 * Nick modes (o,v,h,etc) always take args. */
static int
chathistory_mode_has_arg (server *serv, char sign, char mode)
{
	char *cm;
	int type = 0;

	/* nick modes always have an arg */
	if (serv->nick_modes[0] && strchr (serv->nick_modes, mode))
		return 1;

	/* check CHANMODES= sections (comma-separated: A,B,C,D) */
	cm = serv->chanmodes;
	if (cm)
	{
		while (*cm)
		{
			if (*cm == ',')
				type++;
			else if (*cm == mode)
			{
				switch (type)
				{
				case 0: /* type A - list modes */
				case 1: /* type B - always has arg */
					return 1;
				case 2: /* type C - arg on + only */
					return (sign == '+') ? 1 : 0;
				default: /* type D - no arg */
					return 0;
				}
			}
			cm++;
		}
	}

	return 0;
}

/* Check if 'q' is a list-type chanmode (quiet) on this server,
 * as opposed to owner prefix mode. */
static gboolean
chathistory_server_supports_quiet (server *serv)
{
	char *cm = serv->chanmodes;
	if (!cm)
		return FALSE;
	/* q must appear before the first comma (type A = list modes) */
	while (*cm && *cm != ',')
	{
		if (*cm == 'q')
			return TRUE;
		cm++;
	}
	return FALSE;
}

/* Replay a MODE message from chathistory using per-mode text events.
 * This is display-only — no nicklist or channel state updates. */
static void
chathistory_replay_mode (session *sess, char *nick, char *mode_str,
                         char **params, int param_count,
                         const message_tags_data *tags_data)
{
	server *serv = sess->server;
	char sign = '+';
	int arg_idx = 2;  /* params[0]=target, params[1]=modes, params[2..]=args */
	char *op = NULL, *deop = NULL, *voice = NULL, *devoice = NULL;
	gboolean supportsq = chathistory_server_supports_quiet (serv);

	while (*mode_str)
	{
		if (*mode_str == '+' || *mode_str == '-')
		{
			/* Flush batched modes at sign change */
			if (op)
			{
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANOP, sess, nick, op,
				                       NULL, NULL, 0, tags_data->timestamp);
				g_free (op); op = NULL;
			}
			if (deop)
			{
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANDEOP, sess, nick, deop,
				                       NULL, NULL, 0, tags_data->timestamp);
				g_free (deop); deop = NULL;
			}
			if (voice)
			{
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANVOICE, sess, nick, voice,
				                       NULL, NULL, 0, tags_data->timestamp);
				g_free (voice); voice = NULL;
			}
			if (devoice)
			{
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANDEVOICE, sess, nick, devoice,
				                       NULL, NULL, 0, tags_data->timestamp);
				g_free (devoice); devoice = NULL;
			}
			sign = *mode_str;
			mode_str++;
			continue;
		}

		char mode = *mode_str;
		char *arg = "";

		/* Consume argument if this mode takes one */
		if (chathistory_mode_has_arg (serv, sign, mode) &&
		    arg_idx < param_count && params[arg_idx])
		{
			arg = params[arg_idx];
			if (*arg == ':')
				arg++;
			arg_idx++;
		}

		/* Dispatch to per-mode text events */
		switch (sign)
		{
		case '+':
			switch (mode)
			{
			case 'b':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANBAN, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'e':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANEXEMPT, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'I':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANINVITE, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'k':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANSETKEY, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'l':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANSETLIMIT, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'o':
				if (op)
				{
					char *tmp = g_strconcat (op, " ", arg, NULL);
					g_free (op);
					op = tmp;
				}
				else
					op = g_strdup (arg);
				break;
			case 'h':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANHOP, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'v':
				if (voice)
				{
					char *tmp = g_strconcat (voice, " ", arg, NULL);
					g_free (voice);
					voice = tmp;
				}
				else
					voice = g_strdup (arg);
				break;
			case 'q':
				if (supportsq)
				{
					EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANQUIET, sess, nick, arg,
					                       NULL, NULL, 0, tags_data->timestamp);
					break;
				}
				/* fall through to generic if q is owner mode */
			default:
				goto genmode;
			}
			break;
		case '-':
			switch (mode)
			{
			case 'b':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANUNBAN, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'e':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANRMEXEMPT, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'I':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANRMINVITE, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'k':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANRMKEY, sess, nick, NULL,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'l':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANRMLIMIT, sess, nick, NULL,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'o':
				if (deop)
				{
					char *tmp = g_strconcat (deop, " ", arg, NULL);
					g_free (deop);
					deop = tmp;
				}
				else
					deop = g_strdup (arg);
				break;
			case 'h':
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANDEHOP, sess, nick, arg,
				                       NULL, NULL, 0, tags_data->timestamp);
				break;
			case 'v':
				if (devoice)
				{
					char *tmp = g_strconcat (devoice, " ", arg, NULL);
					g_free (devoice);
					devoice = tmp;
				}
				else
					devoice = g_strdup (arg);
				break;
			case 'q':
				if (supportsq)
				{
					EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANUNQUIET, sess, nick, arg,
					                       NULL, NULL, 0, tags_data->timestamp);
					break;
				}
				/* fall through to generic if q is owner mode */
			default:
				goto genmode;
			}
			break;
		default:
			goto genmode;
		}

		mode_str++;
		continue;

	genmode:
		{
			char outbuf[4];
			outbuf[0] = sign;
			outbuf[1] = 0;
			outbuf[2] = mode;
			outbuf[3] = 0;
			if (*arg)
			{
				char *buf = g_strdup_printf ("%s %s", sess->channel, arg);
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANMODEGEN, sess, nick, outbuf,
				                       outbuf + 2, buf, 0, tags_data->timestamp);
				g_free (buf);
			}
			else
			{
				EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANMODEGEN, sess, nick, outbuf,
				                       outbuf + 2, sess->channel, 0,
				                       tags_data->timestamp);
			}
		}
		mode_str++;
	}

	/* Flush any remaining batched modes */
	if (op)
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANOP, sess, nick, op,
		                       NULL, NULL, 0, tags_data->timestamp);
		g_free (op);
	}
	if (deop)
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANDEOP, sess, nick, deop,
		                       NULL, NULL, 0, tags_data->timestamp);
		g_free (deop);
	}
	if (voice)
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANVOICE, sess, nick, voice,
		                       NULL, NULL, 0, tags_data->timestamp);
		g_free (voice);
	}
	if (devoice)
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANDEVOICE, sess, nick, devoice,
		                       NULL, NULL, 0, tags_data->timestamp);
		g_free (devoice);
	}
}

/* Process a single message from the batch.
 * Returns TRUE if message was processed, FALSE if it was a duplicate. */
static gboolean
process_batch_message (server *serv, session *sess, batch_message *msg)
{
	message_tags_data tags_data;
	char *nick = NULL;
	char *host = NULL;
	char *text = NULL;

	if (!msg || !msg->command)
		return FALSE;

	/* Skip duplicate messages (already displayed from previous batches or live).
	 * Uses msgid+timestamp because some servers reuse msgids after restarts. */
	if (msg->msgid && chathistory_is_duplicate_msgid (sess, msg->msgid, msg->timestamp))
	{
		return FALSE;
	}

	/* Initialize tags data */
	memset (&tags_data, 0, sizeof (tags_data));
	tags_data.timestamp = msg->timestamp;
	if (msg->tags)
		tags_data.all_tags = msg->tags;

	if (msg->msgid)
	{
		/* Set msgid so it gets saved to scrollback via inbound functions */
		tags_data.msgid = msg->msgid;
		/* Track msgids for pagination and deduplication */
		chathistory_track_msgid_ts (sess, msg->msgid, msg->timestamp, TRUE);
		/* Set current_msgid directly for ALL message types including events.
		 * Only inbound_chanmsg/inbound_action set this from tags_data,
		 * but events (JOIN/PART/etc) also need their msgids captured.
		 * Default to non-speech; the inbound_{chanmsg,action,notice} path
		 * below will flip is_user_msg back to TRUE when appropriate. */
		g_free (sess->current_msgid);
		sess->current_msgid = g_strdup (msg->msgid);
		sess->current_msgid_is_user_msg = FALSE;
	}
	else
	{
		g_free (sess->current_msgid);
		sess->current_msgid = NULL;
		sess->current_msgid_is_user_msg = FALSE;
	}

	/* Extract nick from prefix */
	if (msg->prefix)
	{
		char *bang = strchr (msg->prefix, '!');
		if (bang)
		{
			nick = g_strndup (msg->prefix, bang - msg->prefix);
			host = g_strdup (bang + 1);
		}
		else
		{
			nick = g_strdup (msg->prefix);
		}
	}

	/* Handle different command types */
	if (g_ascii_strcasecmp (msg->command, "PRIVMSG") == 0)
	{
		if (msg->param_count >= 2)
		{
			text = msg->params[1];
			if (text && text[0] == ':')
				text++;

			/* Check for CTCP ACTION */
			if (text && strncmp (text, "\001ACTION ", 8) == 0)
			{
				char *action_text = text + 8;
				char *end = strchr (action_text, '\001');
				if (end)
					*end = '\0';
				inbound_action (sess, sess->channel, nick, host ? host : "",
				                action_text, FALSE, 0, &tags_data);
			}
			else
			{
				/* If text contains \n it was collapsed from a draft/multiline
				 * batch — keep it as a single entry instead of splitting */
				if (strchr (text, '\n'))
					fe_begin_multiline_group (sess);
				inbound_chanmsg (serv, sess, sess->channel, nick, text,
				                 FALSE, 0, &tags_data);
				if (strchr (text, '\n'))
					fe_end_multiline_group (sess);
			}
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "NOTICE") == 0)
	{
		if (msg->param_count >= 2)
		{
			text = msg->params[1];
			if (text && text[0] == ':')
				text++;
			inbound_notice (serv, sess->channel, nick, text,
			                host ? host : "", 0, &tags_data);
		}
	}
	/* event-playback: Handle JOIN, PART, QUIT, KICK, MODE, TOPIC, NICK */
	else if (g_ascii_strcasecmp (msg->command, "JOIN") == 0)
	{
		/* Skip the JOIN that started *this* session — inbound_ujoin already
		 * displayed "Now talking on" for it.  Other historical self-JOINs
		 * (previous visits) are kept so JOIN/PART history stays balanced. */
		if (sess->join_msgid && msg->msgid &&
		    strcmp (sess->join_msgid, msg->msgid) == 0)
		{
			g_free (nick);
			g_free (host);
			return TRUE;  /* consumed, not a duplicate */
		}

		/* Historical JOIN - display only, don't modify nicklist.
		 * The current channel membership comes from NAMES, not replayed history. */
		char *account = NULL;
		if (msg->param_count >= 2)
			account = msg->params[1];
		if (account && *account == ':')
			account++;
		EMIT_SIGNAL_TIMESTAMP (XP_TE_JOIN, sess, nick, sess->channel,
		                       host ? host : "", account, 0, tags_data.timestamp);
	}
	else if (g_ascii_strcasecmp (msg->command, "PART") == 0)
	{
		char *reason = NULL;
		if (msg->param_count >= 2)
		{
			reason = msg->params[1];
			if (reason && *reason == ':')
				reason++;
		}
		/* Historical PART - display only, don't modify nicklist */
		if (reason && *reason)
			EMIT_SIGNAL_TIMESTAMP (XP_TE_PARTREASON, sess, nick, host ? host : "",
			                       sess->channel, reason, 0, tags_data.timestamp);
		else
			EMIT_SIGNAL_TIMESTAMP (XP_TE_PART, sess, nick, host ? host : "",
			                       sess->channel, NULL, 0, tags_data.timestamp);
	}
	else if (g_ascii_strcasecmp (msg->command, "QUIT") == 0)
	{
		char *reason = NULL;
		if (msg->param_count >= 1)
		{
			reason = msg->params[0];
			if (reason && *reason == ':')
				reason++;
		}
		/* For historical QUITs, emit directly to session instead of using inbound_quit()
		 * because inbound_quit() requires the user to be in the userlist (which they won't be
		 * for historical events - they already quit). */
		EMIT_SIGNAL_TIMESTAMP (XP_TE_QUIT, sess, nick, reason ? reason : "",
		                       host ? host : "", NULL, 0, tags_data.timestamp);
	}
	else if (g_ascii_strcasecmp (msg->command, "KICK") == 0)
	{
		if (msg->param_count >= 2)
		{
			char *kicked = msg->params[1];
			char *reason = NULL;
			if (msg->param_count >= 3)
			{
				reason = msg->params[2];
				if (reason && *reason == ':')
					reason++;
			}
			/* Historical KICK - display only, don't modify nicklist */
			EMIT_SIGNAL_TIMESTAMP (XP_TE_KICK, sess, nick, kicked,
			                       sess->channel, reason ? reason : "", 0,
			                       tags_data.timestamp);
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "TOPIC") == 0)
	{
		if (msg->param_count >= 2)
		{
			text = msg->params[1];
			if (text && *text == ':')
				text++;
			/* Historical TOPIC - display only, don't update current topic */
			EMIT_SIGNAL_TIMESTAMP (XP_TE_NEWTOPIC, sess, nick, text,
			                       sess->channel, NULL, 0, tags_data.timestamp);
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "NICK") == 0)
	{
		if (msg->param_count >= 1)
		{
			char *newnick = msg->params[0];
			if (newnick && *newnick == ':')
				newnick++;
			/* Historical NICK - display only, don't modify nicklist */
			EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANGENICK, sess, nick,
			                       newnick, NULL, NULL, 0, tags_data.timestamp);
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "MODE") == 0)
	{
		if (msg->param_count >= 2)
		{
			char *mode_str = msg->params[1];
			if (mode_str && *mode_str == ':')
				mode_str++;
			chathistory_replay_mode (sess, nick, mode_str,
			                         msg->params, msg->param_count,
			                         &tags_data);
		}
	}
	else if (g_ascii_strcasecmp (msg->command, "REDACT") == 0)
	{
		/* Historical REDACT: target msgid may or may not exist locally.
		 * Format: :nick!user@host REDACT <target> <msgid> [:<reason>] */
		if (msg->param_count >= 2)
		{
			char *target_msgid = msg->params[1];
			char *reason = (msg->param_count >= 3) ? msg->params[2] : NULL;
			if (target_msgid && *target_msgid == ':')
				target_msgid++;
			if (reason && *reason == ':')
				reason++;
			if (reason && !*reason)
				reason = NULL;

			{
				time_t rtime = tags_data.timestamp ? tags_data.timestamp
				                                   : time (NULL);
				/* Try visual redaction — harmless no-op if entry doesn't exist */
				fe_redact_message (sess, target_msgid, nick, reason, rtime);
				/* Mark as redacted in scrollback (preserves original text) */
				scrollback_redact_for_session (sess, target_msgid, nick, reason, rtime);
			}
		}
	}

	g_free (nick);
	g_free (host);
	return TRUE;
}

/* Find a session on this server that has a pending history request.
 * Used when FAIL response doesn't include the target channel. */
static session *
find_session_with_pending_history (server *serv)
{
	GSList *list;

	for (list = sess_list; list; list = list->next)
	{
		session *sess = list->data;
		if (sess->server == serv && sess->history_loading)
			return sess;
	}
	return NULL;
}

void
chathistory_handle_fail (server *serv, const char *code, const char *context)
{
	session *sess = NULL;

	/* Try to find session from context (may be the target channel) */
	if (context && context[0])
		sess = find_session_for_target (serv, context);

	/* Fall back to finding any session with a pending history request */
	if (!sess)
		sess = find_session_with_pending_history (serv);

	if (!sess)
		return;

	{
		gboolean used_msgid = sess->history_request_used_msgid;
		gboolean was_catchup_latest = sess->catchup_in_progress &&
			!sess->catchup_is_before;
		chreq_type failed_type = sess->ch_active ? sess->ch_active->type
		                                         : CHREQ_LATEST;
		gint64 failed_gap_id = sess->ch_active ? sess->ch_active->gap_id : 0;
		int failed_gap_dir = sess->ch_active ? sess->ch_active->gap_dir : 0;
		gboolean between_latched = FALSE;

		/* Repeated-FAIL brake: an auth-walled or broken server would
		 * otherwise get re-asked on every join and every scroll. */
		serv->chathistory_fail_streak++;
		if (serv->chathistory_fail_streak >= 4)
			serv->chathistory_suppressed = TRUE;

		/* BETWEEN not understood → remember; the gap-fill layer falls
		 * back to BEFORE/AFTER pagination on its next trigger. */
		if (failed_type == CHREQ_BETWEEN && code && code[0] &&
		    (g_ascii_strcasecmp (code, "UNKNOWN_COMMAND") == 0 ||
		     g_ascii_strcasecmp (code, "NEED_MORE_PARAMS") == 0 ||
		     g_ascii_strcasecmp (code, "INVALID_PARAMS") == 0 ||
		     g_ascii_strcasecmp (code, "INVALID_MSGREFTYPES") == 0))
		{
			serv->chathistory_between_unsupported = TRUE;
			between_latched = TRUE;
		}

		/* Clear active request and advance queue */
		chathistory_request_complete (sess);

		/* Gap-fill FAIL: three strikes → dead.  Read the attempts count
		 * rather than touching it here — chathistory_request_gap_fill
		 * already touched the ledger once when it submitted this
		 * request, so touching again would double-count (dead after 2
		 * real failures instead of 3).
		 * A BETWEEN FAIL that just latched chathistory_between_unsupported
		 * above retries naturally: the next proximity trigger takes the
		 * BEFORE/AFTER fallback after the ledger's attempt backoff. */
		if (failed_gap_id > 0)
		{
			const char *gnet = server_get_network (serv, FALSE);
			scrollback_db *gdb = gnet ? scrollback_open (gnet) : NULL;

			/* The msgid anchors themselves may be what the server
			 * choked on; re-anchor on timestamps before counting a
			 * strike.  Not when the FAIL was BETWEEN-unsupported: the
			 * msgids are fine and the BEFORE/AFTER fallback keeps them. */
			if (!between_latched &&
			    gap_fill_retry_with_timestamps (sess, failed_gap_id,
			                                    failed_gap_dir))
				return;

			if (gdb)
			{
				scrollback_gap g;
				if (scrollback_gap_get (gdb, failed_gap_id, &g))
				{
					if (g.attempts >= 3)
						scrollback_gap_set_state (gdb, failed_gap_id,
						                          SCROLLBACK_GAP_DEAD);
					scrollback_gap_clear (&g);
				}
				fe_gap_updated (sess, failed_gap_id);
			}
			return;	/* not a catchup request; nothing further */
		}

		if (sess->catchup_in_progress)
		{
			/* Catch-up: server rejected our reference.  If we used a
			 * msgid the server doesn't recognise, retry with timestamp.
			 * The retry is a continuation of the same LATEST-phase
			 * generation for this session — don't touch latest_pending
			 * here, or it gets decremented twice for one increment
			 * (once now, once when the retry itself later completes). */
			if (!serv->chathistory_suppressed &&
			    used_msgid && sess->scrollback_newest_time > 0)
			{
				char ref[64];
				chathistory_ts_ref (ref, sizeof (ref), (gint64) sess->scrollback_newest_time);
				chathistory_request_latest (sess, ref,
				                            prefs.hex_irc_chathistory_lines);
				return;
			}
			/* A failed catchup LATEST previously left latest_pending
			 * stranded, which blocked the BEFORE phase forever.  Only
			 * decrement here, in the no-retry branch, once this
			 * session's LATEST-phase generation has actually ended. */
			if (was_catchup_latest && serv->chathistory_latest_pending > 0)
				serv->chathistory_latest_pending--;
			/* All fallbacks exhausted — finish with whatever we have */
			finish_catchup (sess);
			if (serv->chathistory_latest_pending == 0)
				chathistory_check_before_catchup (serv);
		}
	}
}

/* --- Chunked batch processing state --- */

typedef struct {
	session *sess;
	server *serv;
	GSList *remaining;			/* next message to process */
	GSList *all_messages;		/* head of list, for freeing */
	int msg_count;				/* messages successfully processed so far */
	int raw_count;				/* total messages in batch */
	time_t oldest_timestamp;
	time_t newest_timestamp;
	char *batch_oldest_msgid;	/* owned copy */
	gboolean is_catchup;
	gboolean chathistory_end;	/* server signalled no more history (draft/chathistory-end) */
	unsigned int unsolicited:1;	/* server-driven replay, not a reply to a request of ours */
	unsigned int latest_owned:1;	/* this batch holds a chathistory_latest_pending slot */
	unsigned int latest_slot_owed:1;	/* ...and finish_batch_processing has not handed it back yet:
										 * snapshot of is_catchup && latest_owned && !catchup_is_before
										 * at creation, cleared on entry to finish_batch_processing, so
										 * a chunk abandoned before then can return the slot exactly once */
	guint idle_tag;
	scrollback_db *db;			/* for transaction begin/commit between chunks */
	gint64 gap_id;				/* gap-fill request this batch answers (0 = none) */
	int gap_dir;				/* gap-fill approach direction (-1 above, +1 below) */
	char *batch_newest_msgid;	/* owned copy (chunked) / borrowed (sync) */
} chathistory_chunk_state;

static void
chunk_state_free (chathistory_chunk_state *chunk)
{
	if (!chunk)
		return;
	if (chunk->idle_tag > 0)
	{
		g_source_remove (chunk->idle_tag);
		chunk->idle_tag = 0;
	}
	if (chunk->sess && chunk->sess->chunk_state == chunk)
		chunk->sess->chunk_state = NULL;
	g_slist_free_full (chunk->all_messages, (GDestroyNotify) batch_message_free);
	g_free (chunk->batch_oldest_msgid);
	g_free (chunk->batch_newest_msgid);
	g_free (chunk);
}

/* Give back the chathistory_latest_pending slot a chunk took, for the
 * paths that abandon it before finish_batch_processing runs: session
 * destroyed under us, or the whole batch cancelled.  latest_slot_owed is
 * the exact condition under which finish_batch_processing would have
 * decremented, and it clears it on entry, so the slot comes back once
 * whichever way the chunk ends.  Reads nothing from chunk->sess — it may
 * already be freed at the abandon sites. */
static void
release_latest_slot (chathistory_chunk_state *chunk)
{
	if (!chunk || !chunk->latest_slot_owed)
		return;

	chunk->latest_slot_owed = FALSE;
	if (chunk->serv && chunk->serv->chathistory_latest_pending > 0)
		chunk->serv->chathistory_latest_pending--;

	/* Deliberately no chathistory_check_before_catchup() here, unlike
	 * finish_batch_processing: the cancel path runs from session_free,
	 * which has not yet cleared current_sess, so kicking the BEFORE
	 * phase could pick the very session being destroyed and dispatch a
	 * request against it.  Reaching zero is picked up by the next batch
	 * completion, BEFORE timer hop or tab switch instead. */
}

/* Post-processing after all messages in a batch have been processed.
 * Handles mode flag cleanup, pagination, and background fetch scheduling. */
static void
finish_batch_processing (chathistory_chunk_state *chunk)
{
	session *sess = chunk->sess;
	server *serv = chunk->serv;

	/* From here the slot is this function's business: every return path
	 * below either gives it back (the LATEST arm) or established at
	 * creation time that there was none to give.  Clearing it up front
	 * means an abandon path that somehow runs afterwards cannot return
	 * the same slot twice. */
	chunk->latest_slot_owed = FALSE;

	serv->chathistory_fail_streak = 0;

	/* Clear processing mode flags and advance the request queue.
	 * chathistory_request_complete dispatches any pending request. */
	sess->history_prepend_mode = FALSE;
	sess->history_insert_sorted_mode = FALSE;
	fe_set_batch_mode (sess, FALSE);
	/* An unsolicited batch answers no request of ours: there is no
	 * ch_active to retire, and dispatching a queued user request in the
	 * middle of the server's replay would interleave the two. */
	if (!chunk->unsolicited)
		chathistory_request_complete (sess);

	/* Update oldest_msgid for scroll-to-load pagination.
	 * Always update if the batch had messages, even if all were duplicates
	 * (msg_count == 0).  The batch's oldest msgid is a valid pagination
	 * cursor regardless of whether the messages were already known from
	 * scrollback DB.  Without this, BEFORE requests loop with the same
	 * reference when the overlap region is entirely in the local DB.
	 *
	 * One exception: an unadopted replay child (unsolicited, and the
	 * session was already paginating a catch-up loop of its own, so no
	 * LATEST phase was taken for it).  That batch is plain history, and its
	 * oldest msgid is newer than the cursor the BEFORE loop is walking
	 * backwards from — writing it here would rewind that walk onto a page
	 * already fetched.  An adopted replay child (latest_owned) opened the
	 * phase itself and updates the cursor exactly like our own LATEST. */
	if (chunk->batch_oldest_msgid && chunk->raw_count > 0 &&
	    !(chunk->unsolicited && !chunk->latest_owned))
	{
		g_free (sess->oldest_msgid);
		sess->oldest_msgid = g_strdup (chunk->batch_oldest_msgid);
	}

	/* Server explicitly signalled end of history via draft/chathistory-end tag.
	 * A gap-fill batch's chathistory-end refers only to the requested range,
	 * not to the session's full history — don't poison history_exhausted.
	 * Neither does an unsolicited replay's: there the tag means "this
	 * target's replay is complete", and older history is still fetchable. */
	if (chunk->chathistory_end && chunk->gap_id == 0 && !chunk->unsolicited)
		sess->history_exhausted = TRUE;

	/* Catch-up loop */
	if (chunk->is_catchup)
	{
		if (sess->catchup_is_before)
		{
			/* --- BEFORE pagination phase --- */
			sess->history_catchup_retrieved += chunk->msg_count;

			/* Each BEFORE batch narrows the witnessed gap from its end side */
			if (sess->catchup_gap_id > 0 && chunk->raw_count > 0 &&
			    chunk->oldest_timestamp > 0)
			{
				const char *network = server_get_network (serv, FALSE);
				scrollback_db *gdb = network ? scrollback_open (network) : NULL;
				if (gdb)
				{
					scrollback_gap_shrink (gdb, sess->catchup_gap_id,
						0, NULL,
						chunk->oldest_timestamp, chunk->batch_oldest_msgid);
					fe_gap_updated (sess, sess->catchup_gap_id);
				}
			}

			/* No more history (empty batch or chathistory-end tag) */
			if (chunk->raw_count == 0 || sess->history_exhausted)
			{
				if (sess->catchup_gap_id > 0)
				{
					const char *network = server_get_network (serv, FALSE);
					scrollback_db *gdb = network ? scrollback_open (network) : NULL;
					if (gdb)
					{
						scrollback_gap_set_state (gdb, sess->catchup_gap_id,
						                          SCROLLBACK_GAP_DEAD);
						fe_gap_updated (sess, sess->catchup_gap_id);
					}
				}
				finish_catchup (sess);
				chathistory_check_before_catchup (serv);
				return;
			}

			/* Timestamp stop: earliest message is older than lower bound → gap bridged */
			if (sess->catchup_lower_bound > 0 && chunk->oldest_timestamp > 0 &&
			    chunk->oldest_timestamp < sess->catchup_lower_bound)
			{
				if (sess->catchup_gap_id > 0)
				{
					const char *network = server_get_network (serv, FALSE);
					scrollback_db *gdb = network ? scrollback_open (network) : NULL;
					if (gdb)
					{
						scrollback_gap_delete (gdb, sess->catchup_gap_id);
						fe_gap_updated (sess, sess->catchup_gap_id);
					}
				}
				finish_catchup (sess);
				chathistory_check_before_catchup (serv);
				return;
			}

			/* Stale count: all duplicates */
			if (chunk->msg_count == 0)
			{
				sess->history_catchup_stale_count++;
				if (sess->history_catchup_stale_count >= 3)
				{
					sess->history_exhausted = TRUE;
					if (sess->catchup_gap_id > 0)
					{
						const char *network = server_get_network (serv, FALSE);
						scrollback_db *gdb = network ? scrollback_open (network) : NULL;
						if (gdb)
						{
							scrollback_gap_set_state (gdb, sess->catchup_gap_id,
							                          SCROLLBACK_GAP_DEAD);
							fe_gap_updated (sess, sess->catchup_gap_id);
						}
					}
					finish_catchup (sess);
					chathistory_check_before_catchup (serv);
					return;
				}
			}
			else
			{
				sess->history_catchup_stale_count = 0;
			}

			/* Per-channel eager-close budget: beyond this, the remainder
			 * stays recorded in the gap ledger for lazy scroll-fill.
			 * CHATHISTORY_SANITY_LIMIT below stays as the outer backstop. */
			if (prefs.hex_irc_gapfill &&
			    prefs.hex_irc_gapfill_catchup_budget > 0 &&
			    sess->history_catchup_retrieved >=
			    prefs.hex_irc_gapfill_catchup_budget)
			{
				finish_catchup (sess);
				chathistory_check_before_catchup (serv);
				return;
			}

			/* Sanity limit — only for automatic post-connect catch-up.
			 * Scroll-to-top and gap-fill are user-driven and uncapped. */
			if (sess->history_catchup_retrieved >= CHATHISTORY_SANITY_LIMIT)
			{
				finish_catchup (sess);
				chathistory_check_before_catchup (serv);
				return;
			}

			/* Continue after a delay — every hop goes through the
			 * scheduler, which re-picks the target (active tab first,
			 * then background sessions).  That re-pick is what the
			 * explicit tab-switch pause used to accomplish. */
			if (chunk->batch_oldest_msgid)
			{
				serv->chathistory_before_sess = NULL;
				schedule_before_catchup (serv);
			}
			else
			{
				finish_catchup (sess);
				chathistory_check_before_catchup (serv);
			}
			return;
		}

		/* --- Initial LATEST phase --- */

		/* Gap-ledger witness: the LATEST batch landed but did not reach
		 * back to our newest stored row — the span between them is a
		 * real hole.  Record it before the BEFORE loop starts shrinking
		 * it, so an interrupted catchup leaves the truth in the ledger.
		 * Only the batch that opened this LATEST phase may write the
		 * witness: catchup_gap_id holds one row, so a second writer
		 * would orphan the first row's id. */
		if (chunk->latest_owned && prefs.hex_irc_gapfill && chunk->raw_count > 0 &&
		    sess->catchup_prev_newest_time > 0 &&
		    chunk->oldest_timestamp > 0 &&
		    sess->catchup_lower_bound > 0 &&
		    chunk->oldest_timestamp > sess->catchup_lower_bound)
		{
			const char *network = server_get_network (serv, FALSE);
			scrollback_db *gdb = network ? scrollback_open (network) : NULL;
			if (gdb)
			{
				sess->catchup_gap_id = scrollback_gap_record (gdb,
					sess->channel,
					sess->catchup_prev_newest_time,
					sess->catchup_prev_newest_msgid,
					chunk->oldest_timestamp,
					chunk->batch_oldest_msgid,
					SCROLLBACK_GAP_WITNESSED);
				if (sess->catchup_gap_id > 0)
					fe_gap_updated (sess, sess->catchup_gap_id);
			}
		}

		/* Give back only a slot this batch actually took.  TRUE for every
		 * requested catch-up batch; for an unsolicited one it tracks
		 * whether chathistory_begin_unsolicited_catchup adopted it. */
		if (chunk->latest_owned && serv->chathistory_latest_pending > 0)
			serv->chathistory_latest_pending--;

		/* All LATEST batches done → start BEFORE catch-up on active tab */
		if (serv->chathistory_latest_pending == 0)
			chathistory_check_before_catchup (serv);

		return;
	}

	/* --- Non-catch-up post-processing (scroll-to-load, background fetch) --- */

	/* Check if we hit the age limit (only for background fetching) */
	if (sess->background_history_active && chunk->oldest_timestamp > 0)
	{
		time_t max_age_cutoff = 0;
		if (prefs.hex_irc_chathistory_background_max_age > 0)
			max_age_cutoff = time (NULL) - (prefs.hex_irc_chathistory_background_max_age * 3600);
		if (max_age_cutoff > 0 && chunk->oldest_timestamp < max_age_cutoff)
		{
			sess->background_history_active = FALSE;
		}
	}

	/* All messages were duplicates — stop background fetching but don't
	 * mark history as exhausted.  During catch-up, the overlap between
	 * local DB and server history can produce all-dupe batches that don't
	 * mean the server has no more history.  The oldest_msgid update above
	 * ensures the next request uses a new cursor past the overlap. */
	if (chunk->raw_count > 0 && chunk->msg_count == 0)
	{
		sess->background_history_active = FALSE;
	}

	/* Gap fill: clamp the ledger record to the edge this batch attached
	 * to.  Gap bounds are exclusive, so which side moved is determined
	 * by the request's own approach direction, not by comparing the
	 * batch's bounds against the gap's — for an end-side fill (BETWEEN
	 * anchored at the end ref, or the BEFORE fallback) every returned
	 * message has timestamp strictly < g.end_ts by construction, so a
	 * "newest_timestamp >= g.end_ts" test can never fire and the
	 * fallthrough would move the wrong bound (inverted: the record
	 * becomes exactly the sliver that was just filled).  All-duplicate
	 * batches still shrink — the span content was already stored
	 * locally. */
	if (chunk->gap_id > 0 && chunk->raw_count > 0)
	{
		const char *gnet = server_get_network (serv, FALSE);
		scrollback_db *gdb = gnet ? scrollback_open (gnet) : NULL;
		scrollback_gap g;

		if (gdb && scrollback_gap_get (gdb, chunk->gap_id, &g))
		{
			gboolean bridged = FALSE;

			/* The bridge test must be keyed on the request's own
			 * approach direction, not tested against a fixed bound
			 * regardless of direction.  For an end-side fill (gap_dir <
			 * 0), the batch's oldest edge is the one closing on the
			 * gap, so test it against the start bound.  For a
			 * start-side fill (gap_dir >= 0, including the AFTER
			 * fallback), the batch's newest edge is the one closing on
			 * the gap, so test it against the end bound instead.
			 * Testing the wrong edge is wrong in both directions: a
			 * start-side batch's oldest edge sits adjacent to the start
			 * bound *by construction*, so testing it there would
			 * prematurely bridge (delete) the record on a same-second
			 * tie after a single batch even though the far (end) side
			 * is still open; and on the AFTER fallback a batch that
			 * overshoots the gap's end would never satisfy an
			 * oldest<=start test, so it would always fall through to
			 * shrink and could write start_ts past end_ts -- an
			 * inverted record whose attempts keep resetting, causing
			 * request churn.  (scrollback_gap_shrink also guards against
			 * an inverted result outright, as defense in depth.) */
			if (chunk->gap_dir < 0)
			{
				if (chunk->oldest_timestamp > 0 &&
				    chunk->oldest_timestamp <= g.start_ts)
					bridged = TRUE;	/* batch overlaps the start bound: covered */
			}
			else
			{
				if (chunk->newest_timestamp > 0 &&
				    chunk->newest_timestamp >= g.end_ts)
					bridged = TRUE;	/* batch overlaps the end bound: covered */
			}

			if (!bridged && chunk->raw_count <
			    get_effective_limit (serv, prefs.hex_irc_chathistory_lines))
				bridged = TRUE;	/* server returned everything in range */

			if (!bridged)
			{
				if (chunk->gap_dir < 0)
					/* End-side fill: end bound moves down to the batch's
					 * oldest edge. */
					scrollback_gap_shrink (gdb, chunk->gap_id, 0, NULL,
						chunk->oldest_timestamp, chunk->batch_oldest_msgid);
				else
					/* Start-side fill: start bound moves up to the batch's
					 * newest edge. */
					scrollback_gap_shrink (gdb, chunk->gap_id,
						chunk->newest_timestamp, chunk->batch_newest_msgid,
						0, NULL);
			}

			if (bridged)
				scrollback_gap_delete (gdb, chunk->gap_id);

			fe_gap_updated (sess, chunk->gap_id);
			scrollback_gap_clear (&g);
		}
	}

	fe_reset_scroll_top_backoff (sess);

	/* Schedule next background fetch if active */
	if (sess->background_history_active && !sess->history_exhausted)
	{
		schedule_background_fetch (sess);
	}
}

/* Process up to CHUNK_SIZE messages from the remaining list. */
static void
process_chunk_messages (chathistory_chunk_state *chunk)
{
	int i;
	GSList *iter;

	for (i = 0, iter = chunk->remaining; iter && i < CHATHISTORY_CHUNK_SIZE;
	     iter = iter->next, i++)
	{
		batch_message *msg = iter->data;

		if (process_batch_message (chunk->serv, chunk->sess, msg))
			chunk->msg_count++;

		if (msg->timestamp > 0)
		{
			if (chunk->oldest_timestamp == 0 || msg->timestamp < chunk->oldest_timestamp)
				chunk->oldest_timestamp = msg->timestamp;
			if (msg->timestamp > chunk->newest_timestamp)
				chunk->newest_timestamp = msg->timestamp;
		}
	}
	chunk->remaining = iter;
}

/* Chunk-at-a-time batch processing.  Scheduled as a short default-priority
 * timer rather than an idle: GTK paints at GDK_PRIORITY_REDRAW, above
 * G_PRIORITY_DEFAULT_IDLE, so a frame-clock animation (the tab strip's
 * activity pulse right after a join burst) starved the idle version for
 * seconds.  One frame between chunks keeps the display and the socket
 * (same priority) interleaving between them. */
#define CHATHISTORY_CHUNK_INTERVAL_MS 16

static gboolean
chunk_idle_cb (gpointer data)
{
	chathistory_chunk_state *chunk = data;

	/* Session may have been destroyed since we were scheduled */
	if (!is_session (chunk->sess))
	{
		chunk->idle_tag = 0;
		chunk->sess = NULL;  /* prevent chunk_state_free from clearing sess->chunk_state */
		/* finish_batch_processing will never run for this batch, so the
		 * LATEST slot it took has to come back here or the server's
		 * BEFORE phase waits on a session that no longer exists. */
		release_latest_slot (chunk);
		chunk_state_free (chunk);
		return G_SOURCE_REMOVE;
	}

	{
		gint64 t_chunk = g_get_monotonic_time ();
		int before = g_slist_length (chunk->remaining);

		if (chunk->db)
			scrollback_begin_transaction (chunk->db);

		process_chunk_messages (chunk);

		if (chunk->db)
			scrollback_commit_transaction (chunk->db);

		poxchat_timing_log ("chathistory %s: chunk %d msgs %.1f ms (%d left)",
		                    chunk->sess->channel,
		                    before - g_slist_length (chunk->remaining),
		                    (g_get_monotonic_time () - t_chunk) / 1000.0,
		                    g_slist_length (chunk->remaining));
	}

	if (chunk->remaining == NULL)
	{
		/* All messages processed */
		chunk->idle_tag = 0;
		finish_batch_processing (chunk);
		chunk_state_free (chunk);
		return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}

void
chathistory_cancel_chunk_processing (session *sess)
{
	chathistory_chunk_state *chunk;

	if (!sess || !sess->chunk_state)
		return;

	chunk = sess->chunk_state;

	/* Commit any in-flight transaction */
	if (chunk->db)
		scrollback_commit_transaction (chunk->db);

	/* Clear batch mode on the buffer so it renders properly */
	sess->history_prepend_mode = FALSE;
	sess->history_insert_sorted_mode = FALSE;
	fe_set_batch_mode (sess, FALSE);
	chathistory_queue_free (sess);

	/* The batch is abandoned mid-flight, so finish_batch_processing will
	 * never return the LATEST slot it took.  Hand it back here — an
	 * orphaned slot never reaches zero, and chathistory_check_before_catchup
	 * refuses to start the BEFORE phase for every other session on the
	 * server until it does. */
	release_latest_slot (chunk);

	chunk_state_free (chunk);
}

/* Catch-up state machine — every shape of chathistory batch we can see,
 * and what each one is allowed to touch.  "nested" is
 * batch_in_replay_wrapper (a leg of the draft/persistence catch-up
 * replay); "unsolicited" is chathistory_batch_is_unsolicited (nested, or
 * nothing of ours in flight for the session); "latest_owned" is whether
 * chathistory_begin_unsolicited_catchup adopted the batch and so took a
 * chathistory_latest_pending slot (always TRUE for a batch that answers
 * a request of ours, which took its slot when it was sent).  "session
 * idle" below means no catch-up loop of its own is running and its
 * history is not exhausted; "chathistory enabled" means have_chathistory
 * && hex_irc_chathistory_auto && !chathistory_suppressed.
 *
 * | Batch                                                                     | nested | unsolicited | latest_owned | is_catchup          | gap ids        | request_complete | witness     | pending--   | oldest_msgid update | history_exhausted from end-tag |
 * |---------------------------------------------------------------------------|--------|-------------|--------------|---------------------|----------------|------------------|-------------|-------------|---------------------|--------------------------------|
 * | our LATEST/BEFORE                                                         | F      | F           | T            | catchup_in_progress | 0              | yes              | LATEST only | LATEST only | yes                 | yes                            |
 * | our gap fill                                                              | F      | F           | T            | F                   | from ch_active | yes              | no          | no          | yes                 | no                             |
 * | top-level, no request, session idle, chathistory enabled                  | F      | T           | T            | T                   | 0              | no               | yes         | yes         | yes                 | no                             |
 * | top-level, no request, session already catching up / exhausted / disabled | F      | T           | F            | F                   | 0              | no               | no          | no          | no                  | no                             |
 * | wrapper child, session idle, chathistory enabled (any ch_active)          | T      | T           | T            | T                   | forced 0       | no               | yes         | yes         | yes                 | no                             |
 * | wrapper child, session already catching up / exhausted / disabled         | T      | T           | F            | F                   | forced 0       | no               | no          | no          | no                  | no                             |
 *
 * Where each column is enforced:
 *   is_catchup            — below: catchup_in_progress && no gap id, then
 *                           overridden by the adoption result when unsolicited.
 *   gap ids               — active_gap_id/dir come from ch_active, and the
 *                           nested arm forces them to 0: a wrapper child
 *                           answers no gap fill of ours even while one is in
 *                           flight.
 *   request_complete      — finish_batch_processing and the empty-batch arm,
 *                           both under !unsolicited.
 *   witness / pending--   — finish_batch_processing's LATEST arm, both under
 *                           chunk->latest_owned; the empty-batch arm mirrors
 *                           the decrement, and release_latest_slot returns it
 *                           for a chunk abandoned before either runs.
 *   oldest_msgid update   — finish_batch_processing, skipped for an unadopted
 *                           replay child (unsolicited && !latest_owned) whose
 *                           oldest msgid would rewind somebody else's walk.
 *   history_exhausted     — finish_batch_processing and the empty-batch arm,
 *                           both under gap_id == 0 && !unsolicited: a replay's
 *                           chathistory-end means "this target's replay is
 *                           done", not "no older history exists". */
void
chathistory_process_batch (server *serv, batch_info *batch)
{
	session *sess = NULL;
	gboolean is_catchup;
	gboolean unsolicited;
	gboolean nested;
	gboolean latest_owned = TRUE;
	gint64 active_gap_id;
	int active_gap_dir;
	char *batch_oldest_msgid = NULL;
	const char *batch_newest_msgid = NULL;
	int raw_count;
	const char *network;
	scrollback_db *db;

	if (!batch || !batch->type)
		return;

	nested = batch_in_replay_wrapper (serv, batch);

	/* batch->params[0] should be the target */
	if (batch->param_count >= 1 && batch->params[0])
	{
		sess = find_session_for_target (serv, batch->params[0]);

		/* The replay covers PM correspondents after channels, and we may
		 * have no window open for the ones that spoke while we were away.
		 * Open the dialog the way the CHATHISTORY TARGETS path does for a
		 * missed correspondent — new_ircwindow loads the scrollback, which
		 * the catch-up anchors below read.  Only for wrapper children: a
		 * stray unnested batch for an unknown target is still dropped. */
		if (!sess && nested && batch->params[0][0] &&
		    !is_channel (serv, batch->params[0]))
			sess = new_ircwindow (serv, batch->params[0], SESS_DIALOG, 0);
	}

	if (!sess)
		return;

	/* Capture before is_catchup: a gap-fill batch must never be treated
	 * as catchup, even if catchup_in_progress happens to be set (e.g.
	 * catchup started on this session while a gap fill was already in
	 * flight) — otherwise it shrinks the wrong ledger row and drives
	 * catchup pagination off a gap-fill response. */
	active_gap_id = sess->ch_active ? sess->ch_active->gap_id : 0;
	active_gap_dir = sess->ch_active ? sess->ch_active->gap_dir : 0;

	/* A wrapper child answers no request of ours, so it must not be
	 * attributed to an in-flight gap fill: with the gap forgotten here the
	 * empty branch can neither retry nor dead-mark it, and the non-empty
	 * path carries gap_id 0 into the chunk instead of clamping the ledger
	 * row to a span this batch never covered.  ch_active itself is left
	 * alone (see the request_complete skip below), so the real gap-fill
	 * reply is still attributed normally when it arrives. */
	if (nested)
	{
		active_gap_id = 0;
		active_gap_dir = 0;
	}

	is_catchup = sess->catchup_in_progress && active_gap_id == 0;

	/* A batch the server sent on its own (draft/persistence replay, or a
	 * hand-typed /quote CHATHISTORY) may be treated as this session's
	 * LATEST phase: the witness/bridge bookkeeping runs, but the request
	 * queue and history_exhausted are left alone.  Only if the adoption
	 * succeeds, though — a session already running a catch-up loop of its
	 * own keeps it, and this batch is then plain history rows that must
	 * not step, witness or complete somebody else's phase.  (active_gap_id
	 * is necessarily 0 here: the wrapper arm cleared it just above, and
	 * the other arm requires ch_active == NULL.) */
	unsolicited = chathistory_batch_is_unsolicited (serv, batch, sess);
	if (unsolicited)
	{
		latest_owned = chathistory_begin_unsolicited_catchup (sess);
		is_catchup = latest_owned;
	}

	/* Empty batch handling */
	if (!batch->messages)
	{
		gboolean used_msgid = sess->history_request_used_msgid;
		/* A gap-fill batch's chathistory-end tag refers only to the
		 * requested range, not the session's full history — don't
		 * poison history_exhausted for a probe that came back empty.
		 * Nor for an unsolicited replay, whose tag only says the
		 * server finished replaying this target. */
		if (batch->chathistory_end && active_gap_id == 0 && !unsolicited)
			sess->history_exhausted = TRUE;
		/* Nothing of ours to retire for an unsolicited batch, and a
		 * queued user request must not be dispatched mid-replay. */
		if (!unsolicited)
			chathistory_request_complete (sess);

		/* Gap-fill probe that returned empty: the span is genuinely
		 * empty (or beyond retention).  Dead-mark and stop — don't
		 * fall through to the catchup/exhausted paths below, which
		 * don't apply to a gap-fill request. */
		if (active_gap_id > 0)
		{
			const char *gnet = server_get_network (serv, FALSE);
			scrollback_db *gdb = gnet ? scrollback_open (gnet) : NULL;

			/* An unknown msgid anchor yields an empty batch, not a
			 * FAIL.  Re-ask on timestamps once before giving up. */
			if (gap_fill_retry_with_timestamps (sess, active_gap_id,
			                                    active_gap_dir))
				return;

			if (gdb)
			{
				scrollback_gap_set_state (gdb, active_gap_id, SCROLLBACK_GAP_DEAD);
				fe_gap_updated (sess, active_gap_id);
			}
			return;
		}

		if (is_catchup)
		{
			/* Server may not recognize our msgid (e.g., server restart).
			 * Fall back to timestamp-based LATEST, then LATEST *.
			 * But not if chathistory-end tells us there's nothing —
			 * nor for an unsolicited batch, which answers no request
			 * of ours and so has nothing to fall back from. */
			if (!unsolicited && !sess->history_exhausted &&
			    used_msgid && sess->scrollback_newest_time > 0)
			{
				char ref[64];
				chathistory_ts_ref (ref, sizeof (ref), (gint64) sess->scrollback_newest_time);
				chathistory_request_latest (sess, ref, prefs.hex_irc_chathistory_lines);
				return;
			}
			/* Catch-up complete — no new messages since last disconnect.
			 * Don't set history_exhausted unless chathistory-end was sent:
			 * older history may still exist for scroll-to-top requests. */
			finish_catchup (sess);
			/* Give back only a slot this batch actually took (see the
			 * matching decrement in finish_batch_processing). */
			if (latest_owned && serv->chathistory_latest_pending > 0)
				serv->chathistory_latest_pending--;
			if (serv->chathistory_latest_pending == 0)
				chathistory_check_before_catchup (serv);
			return;
		}
		/* Non-catch-up empty batch: server has no more history in this
		 * direction.  Never conclude that from an unsolicited batch — an
		 * empty replay leg says the server had nothing to replay for this
		 * target, not that the target's older history is gone.  (Reachable
		 * for a replay child only when the session was already running a
		 * catch-up loop, so the batch was not adopted.) */
		if (!unsolicited)
			sess->history_exhausted = TRUE;
		return;
	}

	/* Sort batch messages by timestamp ascending.  The IRCv3 spec requires
	 * servers to return messages in ascending order, but not all servers
	 * comply.  Sorting here makes us robust against any server ordering and
	 * also ensures batch_oldest_msgid is captured correctly below. */
	batch->messages = g_slist_sort (batch->messages, compare_batch_msg_timestamp);

	/* Pagination cursors and the "did the server hit its limit" count must
	 * only consider real history entries.  draft/chathistory-context messages
	 * (reacts, redactions, edits the server appends after their parent) are
	 * not counted against the request limit and may lie outside the requested
	 * range, so using one as a cursor would skip or repeat history. */
	raw_count = 0;
	{
		GSList *iter;
		for (iter = batch->messages; iter; iter = iter->next)
		{
			batch_message *m = iter->data;
			if (!m || batch_msg_is_context (m))
				continue;
			raw_count++;
			if (!batch_oldest_msgid && m->msgid)
				batch_oldest_msgid = m->msgid;
			if (m->msgid)
				batch_newest_msgid = m->msgid;
		}
	}

	/* Keep history_loading TRUE until finish_batch_processing clears it —
	 * this prevents new requests from being sent during chunked processing. */
	sess->history_insert_sorted_mode = TRUE;
	fe_set_batch_mode (sess, TRUE);

	network = server_get_network (serv, FALSE);
	db = network ? scrollback_open (network) : NULL;

	if (raw_count <= CHATHISTORY_CHUNK_SIZE)
	{
		/* Small batch — process synchronously */
		chathistory_chunk_state sync_state = { 0 };
		sync_state.sess = sess;
		sync_state.serv = serv;
		sync_state.remaining = batch->messages;
		sync_state.raw_count = raw_count;
		sync_state.is_catchup = is_catchup;
		sync_state.unsolicited = unsolicited;
		sync_state.latest_owned = latest_owned;
		sync_state.latest_slot_owed = is_catchup && latest_owned &&
		                              !sess->catchup_is_before;
		sync_state.chathistory_end = batch->chathistory_end;
		sync_state.batch_oldest_msgid = (char *)batch_oldest_msgid; /* borrowed, not freed */
		sync_state.gap_id = active_gap_id;
		sync_state.gap_dir = active_gap_dir;
		sync_state.batch_newest_msgid = (char *) batch_newest_msgid; /* borrowed, not freed */

		gint64 t_sync = g_get_monotonic_time ();

		if (db)
			scrollback_begin_transaction (db);

		process_chunk_messages (&sync_state);

		if (db)
			scrollback_commit_transaction (db);

		/* finish_batch_processing reads from the chunk state but doesn't
		 * free it — safe to use a stack-allocated struct here. */
		finish_batch_processing (&sync_state);
		poxchat_timing_log ("chathistory %s: %d msgs sync %.1f ms", sess->channel,
		                    raw_count, (g_get_monotonic_time () - t_sync) / 1000.0);
	}
	else
	{
		/* Large batch — process in chunks via idle callbacks.
		 * Steal the message list from batch so batch_info_free won't free it. */
		chathistory_chunk_state *chunk = g_new0 (chathistory_chunk_state, 1);
		chunk->sess = sess;
		chunk->serv = serv;
		chunk->all_messages = batch->messages;
		chunk->remaining = batch->messages;
		chunk->raw_count = raw_count;
		chunk->is_catchup = is_catchup;
		chunk->unsolicited = unsolicited;
		chunk->latest_owned = latest_owned;
		/* Snapshot, not a live read: the abandon paths cannot touch the
		 * session (it may be freed).  catchup_is_before is stable for the
		 * chunk's lifetime — history_loading stays TRUE until
		 * finish_batch_processing, and check_before_catchup skips a
		 * session that is loading. */
		chunk->latest_slot_owed = is_catchup && latest_owned &&
		                          !sess->catchup_is_before;
		chunk->chathistory_end = batch->chathistory_end;
		chunk->db = db;
		chunk->batch_oldest_msgid = g_strdup (batch_oldest_msgid);
		chunk->gap_id = active_gap_id;
		chunk->gap_dir = active_gap_dir;
		chunk->batch_newest_msgid = g_strdup (batch_newest_msgid);
		batch->messages = NULL;  /* prevent batch_info_free from freeing */

		sess->chunk_state = chunk;

		/* Defer ALL processing off the socket read handler so WHO replies
		 * and GTK renders interleave between chunks instead of being
		 * blocked by inline processing (see chunk_idle_cb for why this is
		 * a timer and not an idle). */
		chunk->idle_tag = g_timeout_add_full (G_PRIORITY_DEFAULT,
		                                      CHATHISTORY_CHUNK_INTERVAL_MS,
		                                      chunk_idle_cb, chunk, NULL);
	}
}

void
chathistory_parse_isupport (server *serv, const char *value)
{
	/* Format: CHATHISTORY=<limit> or more complex options */
	if (!value || !value[0])
	{
		/* No value means feature supported but no specific limit */
		serv->chathistory_limit = 0;
		return;
	}

	/* Try to parse as simple integer limit */
	serv->chathistory_limit = atoi (value);

	/* Handle more complex formats like "limit=1000,retention=7d" */
	if (serv->chathistory_limit == 0 && strchr (value, '='))
	{
		char **tokens = g_strsplit (value, ",", 0);
		int i;

		for (i = 0; tokens[i]; i++)
		{
			if (g_str_has_prefix (tokens[i], "limit="))
			{
				serv->chathistory_limit = atoi (tokens[i] + 6);
			}
			/* Retention is its own token (CHATHISTORY_RETENTION_TOKEN),
			 * not a sub-key here. */
		}

		g_strfreev (tokens);
	}
}

gint64
chathistory_retention_cutoff (server *serv)
{
	if (!serv || serv->chathistory_retention_secs <= 0)
		return 0;
	return (gint64) time (NULL) - serv->chathistory_retention_secs;
}

void
chathistory_parse_retention (server *serv, const char *value)
{
	gint64 cutoff;
	const char *network;
	scrollback_db *db;
	GSList *list;

	if (!serv)
		return;

	serv->chathistory_retention_secs = 0;
	if (value && value[0])
	{
		gint64 secs = g_ascii_strtoll (value, NULL, 10);
		if (secs > 0)
			serv->chathistory_retention_secs = secs;
	}

	/* Publish the cutoff to the network's ledger.  Gaps ending before it
	 * are parked (no marker, no probe) by comparison at read time, so a
	 * re-announced wider bound -- a store relinked -- wakes them with no
	 * further bookkeeping.  Nothing is ever dead-marked on retention:
	 * only an empty, complete answer to a real probe closes a gap.  The
	 * server promises the value is the widest retention over the stores
	 * currently linked (local included), which is what makes parking on
	 * it safe: a span past it is unservable *right now*, not gone. */
	cutoff = chathistory_retention_cutoff (serv);
	network = server_get_network (serv, FALSE);
	db = network ? scrollback_open (network) : NULL;
	if (!db || scrollback_get_retention_cutoff (db) == cutoff)
		return;
	scrollback_set_retention_cutoff (db, cutoff);
	for (list = sess_list; list; list = list->next)
	{
		session *sess = list->data;
		if (sess->server == serv)
			fe_gap_updated (sess, 0);
	}
}

/* Build a dedup key from msgid + timestamp.  Some servers (e.g. Nefarious)
 * reset their msgid counter on restart, producing collisions.  Adding the
 * timestamp makes false duplicates virtually impossible. */
static char *
make_dedup_key (const char *msgid, time_t timestamp)
{
	return g_strdup_printf ("%s@%" G_GINT64_FORMAT, msgid, (gint64) timestamp);
}

void
chathistory_track_msgid (session *sess, const char *msgid, gboolean is_history)
{
	chathistory_track_msgid_ts (sess, msgid, 0, is_history);
}

void
chathistory_track_msgid_ts (session *sess, const char *msgid, time_t timestamp,
                            gboolean is_history)
{
	gboolean is_new;
	char *key;

	if (!msgid || !msgid[0])
		return;

	/* Build dedup key: msgid alone if no timestamp, msgid+timestamp otherwise.
	 * Scrollback-loaded entries have timestamps; live messages may not yet. */
	key = (timestamp > 0) ? make_dedup_key (msgid, timestamp) : g_strdup (msgid);

	/* Add to known msgids for deduplication.
	 * g_hash_table_add returns TRUE if this is a new entry. */
	if (sess->known_msgids)
		is_new = g_hash_table_add (sess->known_msgids, key);
	else
	{
		g_free (key);
		is_new = TRUE;
	}

	/* Only update oldest/newest tracking for truly new msgids.
	 * This prevents re-tracking from inbound functions after chathistory already tracked. */
	if (!is_new)
		return;

	if (is_history)
	{
		/* Historical message - update oldest_msgid if this is older
		 * (messages come in order, so the first one we see is oldest) */
		if (!sess->oldest_msgid)
		{
			sess->oldest_msgid = g_strdup (msgid);
		}
		/* Don't update newest_msgid for historical messages */
	}
	else
	{
		/* Live message - update newest_msgid */
		g_free (sess->newest_msgid);
		sess->newest_msgid = g_strdup (msgid);

		/* If we don't have an oldest yet, this is also the oldest */
		if (!sess->oldest_msgid)
		{
			sess->oldest_msgid = g_strdup (msgid);
		}
	}
}

/**
 * Check if a message with this msgid+timestamp has already been displayed.
 * Uses both fields because some servers reuse msgids after restarts.
 *
 * @param sess Session to check
 * @param msgid The message ID to check
 * @param timestamp The message timestamp (0 to match by msgid alone)
 * @return TRUE if msgid is known (duplicate), FALSE if new
 */
gboolean
chathistory_is_duplicate_msgid (session *sess, const char *msgid, time_t timestamp)
{
	if (!sess || !msgid || !msgid[0])
		return FALSE;

	if (sess->known_msgids)
	{
		if (timestamp > 0)
		{
			char *key = make_dedup_key (msgid, timestamp);
			gboolean found = g_hash_table_contains (sess->known_msgids, key);
			g_free (key);
			if (found)
				return TRUE;
		}
		else if (g_hash_table_contains (sess->known_msgids, msgid))
			return TRUE;
	}

	/* Fallback: the hash only seeds from the initially-loaded window
	 * (and this session's traffic), so a replay overlapping older
	 * history slips past it — the source of duplicate own-message rows
	 * when chathistory races the echo confirm.  The DB check is one
	 * indexed lookup on (channel_id, msgid), timestamp-matched to guard
	 * against servers that reuse msgids after restarts. */
	return scrollback_session_has_msgid (sess, msgid, timestamp);
}

gboolean
chathistory_can_request_more (session *sess)
{
	if (!sess || !sess->server)
		return FALSE;

	if (!sess->server->have_chathistory)
		return FALSE;

	if (sess->history_exhausted)
		return FALSE;

	if (sess->history_loading)
		return FALSE;

	return TRUE;
}

/* Timer callback for background history fetching */
static gboolean
background_history_timer_cb (gpointer data)
{
	session *sess = (session *)data;

	/* Clear the timer tag first */
	sess->background_history_timer = 0;

	/* Check if we should continue fetching */
	if (!sess->background_history_active)
		return G_SOURCE_REMOVE;

	if (!chathistory_can_request_more (sess))
	{
		/* Can't request more - stop background fetching */
		sess->background_history_active = FALSE;
		return G_SOURCE_REMOVE;
	}

	/* Request older history */
	if (sess->oldest_msgid && sess->oldest_msgid[0])
	{
		chathistory_request_before_msgid (sess, sess->oldest_msgid,
		                                  prefs.hex_irc_chathistory_lines);
	}
	else
	{
		/* No oldest_msgid to reference - can't make BEFORE request */
		sess->background_history_active = FALSE;
	}

	return G_SOURCE_REMOVE;
}

/* Schedule the next background history fetch */
static void
schedule_background_fetch (session *sess)
{
	int delay_secs;

	if (!sess->background_history_active)
		return;

	if (sess->background_history_timer > 0)
		return; /* Already scheduled */

	delay_secs = prefs.hex_irc_chathistory_background_delay;
	if (delay_secs < 5)
		delay_secs = 5; /* Minimum 5 seconds between fetches */

	sess->background_history_timer = g_timeout_add_seconds (delay_secs,
	                                                        background_history_timer_cb,
	                                                        sess);
}

void
chathistory_start_background_fetch (session *sess)
{
	if (!sess || !sess->server)
		return;

	/* Check if background fetching is enabled */
	if (!prefs.hex_irc_chathistory_background)
		return;

	/* Don't start if server doesn't support chathistory */
	if (!sess->server->have_chathistory)
		return;

	/* Don't start if already exhausted */
	if (sess->history_exhausted)
		return;

	/* Don't start if no oldest_msgid to reference */
	if (!sess->oldest_msgid || !sess->oldest_msgid[0])
		return;

	/* Mark as active and schedule first fetch */
	sess->background_history_active = TRUE;
	schedule_background_fetch (sess);
}

void
chathistory_stop_background_fetch (session *sess)
{
	if (!sess)
		return;

	sess->background_history_active = FALSE;

	if (sess->background_history_timer > 0)
	{
		g_source_remove (sess->background_history_timer);
		sess->background_history_timer = 0;
	}
}

/* --- Deferred catch-up coordination --- */

/* Put a session into the LATEST phase of the catch-up loop: refresh the
 * newest-stored anchors, arm the gap-ledger witness bounds, and flag the
 * loop as running.  Sends nothing — the caller either dispatches its own
 * LATEST (send_deferred_latest) or is adopting a batch the server sent
 * unasked (chathistory_begin_unsolicited_catchup). */
static void
catchup_enter_latest_phase (session *sess)
{
	gint64 t_enter = g_get_monotonic_time ();

	/* Refresh the newest-stored snapshot from the DB.  The session's
	 * scrollback_newest_* fields are loaded once at scrollback-load time
	 * and go stale as soon as catchup or live traffic writes newer rows —
	 * a reconnect without an app restart would otherwise anchor LATEST
	 * and the gap ledger on app-start-era values. */
	{
		const char *network = server_get_network (sess->server, FALSE);
		scrollback_db *db = network ? scrollback_open (network) : NULL;
		if (db)
		{
			g_free (sess->scrollback_newest_msgid);
			sess->scrollback_newest_msgid =
				scrollback_get_newest_msgid (db, sess->channel);
			sess->scrollback_newest_time =
				scrollback_get_newest_time (db, sess->channel);
		}
	}
	sess->catchup_prev_newest_time = sess->scrollback_newest_time;
	g_free (sess->catchup_prev_newest_msgid);
	sess->catchup_prev_newest_msgid = g_strdup (sess->scrollback_newest_msgid);
	sess->catchup_gap_id = 0;

	sess->catchup_in_progress = TRUE;
	sess->catchup_is_before = FALSE;
	if (sess->scrollback_newest_time > CHATHISTORY_FUZZ_INTERVAL)
		sess->catchup_lower_bound = sess->scrollback_newest_time - CHATHISTORY_FUZZ_INTERVAL;
	else if (prefs.hex_irc_chathistory_background_max_age > 0)
		sess->catchup_lower_bound = time (NULL) - (prefs.hex_irc_chathistory_background_max_age * 3600);
	else
		sess->catchup_lower_bound = 0;
	poxchat_timing_log ("catchup_enter %s: snapshot %.1f ms", sess->channel,
	                    (g_get_monotonic_time () - t_enter) / 1000.0);
}

/* Send LATEST for a single session as part of deferred catch-up */
static void
send_deferred_latest (session *sess)
{
	if (!sess || !sess->server)
		return;
	if (sess->history_loading || sess->catchup_in_progress || sess->history_exhausted)
		return;
	if (sess->type != SESS_CHANNEL || !sess->channel[0])
		return;

	catchup_enter_latest_phase (sess);

	/* Choose LATEST reference based on available scrollback */
	if (sess->scrollback_newest_msgid && sess->scrollback_newest_msgid[0])
	{
		char *ref = g_strdup_printf ("msgid=%s", sess->scrollback_newest_msgid);
		chathistory_request_latest (sess, ref, prefs.hex_irc_chathistory_lines);
		g_free (ref);
	}
	else if (sess->scrollback_newest_time > 0)
	{
		char ref[64];
		chathistory_ts_ref (ref, sizeof (ref), (gint64) sess->scrollback_newest_time);
		chathistory_request_latest (sess, ref, prefs.hex_irc_chathistory_lines);
	}
	else
	{
		chathistory_request_latest (sess, NULL, prefs.hex_irc_chathistory_lines);
	}

	sess->server->chathistory_latest_pending++;
}

/* Timer callback: fires 2s after the last 366, sends LATEST for all channels */
static gboolean
chathistory_deferred_start_cb (gpointer data)
{
	server *serv = data;
	GSList *list;

	serv->chathistory_start_timer = 0;

	if (!is_server (serv) || !serv->connected || !serv->have_chathistory)
		return G_SOURCE_REMOVE;

	if (!prefs.hex_irc_chathistory_auto)
		return G_SOURCE_REMOVE;

	/* We held TARGETS back for a server-driven replay that never opened
	 * a wrapper (chathistory_replay_wrapper_begin would have cancelled
	 * this timer and cleared the flag), so discover the missed DMs the
	 * usual way after all. */
	if (serv->chathistory_targets_deferred)
	{
		serv->chathistory_targets_deferred = FALSE;
		send_reconnect_targets_request (serv);
	}

	serv->chathistory_latest_pending = 0;

	/* Send LATEST for current_sess first (active tab gets priority) */
	if (current_sess && current_sess->server == serv)
		send_deferred_latest (current_sess);

	/* Then all other sessions on this server */
	for (list = sess_list; list; list = list->next)
	{
		session *sess = list->data;
		if (sess->server == serv && sess != current_sess)
			send_deferred_latest (sess);
	}

	/* If no sessions needed catch-up, nothing to do */
	if (serv->chathistory_latest_pending == 0)
		return G_SOURCE_REMOVE;

	return G_SOURCE_REMOVE;
}

void
chathistory_schedule_deferred (server *serv)
{
	guint delay;

	if (!serv || !serv->have_chathistory || !prefs.hex_irc_chathistory_auto)
		return;

	/* While the server is expected to replay every buffer from the
	 * cursor our ATTACH carried, our fan-out would fetch the same lines
	 * over again — but that expectation is only our own consent, never
	 * the server's promise.  PERSISTENCE REPLAY OFF, server policy, or a
	 * fresh session whose cursor is still known all end in nothing being
	 * replayed and no FAIL to say so, and dropping the fan-out on that
	 * would leave the hole between our newest stored row and live
	 * traffic silent until the user happened to scroll.
	 *
	 * So the gate only buys time.  The first bouncer-replay wrapper
	 * START normally arrives within a second of the MOTD end and cancels
	 * this timer outright (chathistory_replay_wrapper_begin); when it
	 * never comes, the old fan-out runs late instead of never.  A replay
	 * that turns up after the fan-out already started is absorbed by
	 * msgid dedup and by the adoption rules above
	 * chathistory_process_batch — the cost of guessing wrong is one
	 * redundant round of LATEST requests. */
	delay = persistence_server_drives_replay (serv)
		? CHATHISTORY_DEFERRED_DELAY * 10
		: CHATHISTORY_DEFERRED_DELAY;

	/* Reset the timer — each new 366 pushes the start out by that delay */
	if (serv->chathistory_start_timer > 0)
		g_source_remove (serv->chathistory_start_timer);

	serv->chathistory_start_timer = g_timeout_add (delay,
	                                                chathistory_deferred_start_cb,
	                                                serv);
}

static gboolean
chathistory_before_timer_cb (gpointer data)
{
	server *serv = data;

	serv->chathistory_before_timer = 0;

	if (!is_server (serv) || !serv->connected)
		return G_SOURCE_REMOVE;

	chathistory_check_before_catchup (serv);
	return G_SOURCE_REMOVE;
}

/* Schedule check_before_catchup after CHATHISTORY_BEFORE_DELAY seconds */
static void
schedule_before_catchup (server *serv)
{
	if (serv->chathistory_before_timer > 0)
		g_source_remove (serv->chathistory_before_timer);

	serv->chathistory_before_timer = g_timeout_add_seconds (
		CHATHISTORY_BEFORE_DELAY, chathistory_before_timer_cb, serv);
}

void
chathistory_check_before_catchup (server *serv)
{
	GSList *list;
	session *target = NULL;

	if (!serv || !serv->connected || !serv->have_chathistory)
		return;

	/* Don't start BEFORE until all LATEST are done */
	if (serv->chathistory_latest_pending > 0)
		return;

	/* If there's already an active BEFORE session with a pending request, wait */
	if (serv->chathistory_before_sess &&
	    serv->chathistory_before_sess->history_loading)
		return;

	/* Prefer current_sess if it needs BEFORE catch-up */
	if (current_sess && current_sess->server == serv &&
	    current_sess->catchup_in_progress && !current_sess->history_exhausted &&
	    !current_sess->history_loading)
	{
		target = current_sess;
	}

	/* Background channels: their residual gap used to be silently
	 * abandoned here (active-tab-only).  With the gap ledger they are
	 * closed eagerly too, one session at a time, budget-bounded. */
	if (!target && prefs.hex_irc_gapfill)
	{
		for (list = sess_list; list; list = list->next)
		{
			session *s = list->data;
			if (s->server == serv && s->catchup_in_progress &&
			    !s->history_exhausted && !s->history_loading)
			{
				target = s;
				break;
			}
		}
	}

	if (!target)
	{
		serv->chathistory_before_sess = NULL;
		return;
	}

	/* Start/resume BEFORE catch-up on the target session */
	serv->chathistory_before_sess = target;
	target->catchup_is_before = TRUE;

	if (target->oldest_msgid && target->oldest_msgid[0])
	{
		chathistory_request_before_msgid (target, target->oldest_msgid,
		                                  CHATHISTORY_BEFORE_LIMIT);
	}
	else
	{
		/* No msgid to reference — can't paginate */
		finish_catchup (target);
		serv->chathistory_before_sess = NULL;
	}
}

void
chathistory_notify_tab_switch (session *new_sess)
{
	server *serv;

	if (!new_sess || !new_sess->server)
		return;

	serv = new_sess->server;

	if (!serv->have_chathistory)
		return;

	/* Every BEFORE hop re-picks its target via check_before_catchup
	 * (see finish_batch_processing), so a tab switch is picked up on
	 * the next scheduled hop regardless.  This just nudges it to
	 * happen immediately instead of waiting out the delay — but only
	 * when there's no in-flight request to interrupt. */
	if (serv->chathistory_before_sess != new_sess &&
	    serv->chathistory_latest_pending == 0)
	{
		chathistory_check_before_catchup (serv);
	}
}

void
chathistory_process_targets_batch (server *serv, batch_info *batch)
{
	GSList *iter;

	if (!batch)
		return;

	for (iter = batch->messages; iter; iter = iter->next)
	{
		batch_message *msg = iter->data;
		session *sess;
		char *target;

		if (!msg || !msg->command)
			continue;

		/* TARGETS entries have command="CHATHISTORY", params: TARGETS <target> <timestamp> */
		if (g_ascii_strcasecmp (msg->command, "CHATHISTORY") != 0)
			continue;
		if (msg->param_count < 3)
			continue;
		if (g_ascii_strcasecmp (msg->params[0], "TARGETS") != 0)
			continue;

		target = msg->params[1];
		if (!target || !target[0])
			continue;

		/* Skip channel targets — channels get catch-up from their JOIN handler */
		if (is_channel (serv, target))
			continue;

		/* Find or create a dialog session for this DM target */
		sess = find_dialog (serv, target);
		if (!sess)
		{
			/* new_ircwindow calls scrollback_load automatically,
			 * populating scrollback_newest_msgid/time for catch-up */
			sess = new_ircwindow (serv, target, SESS_DIALOG, 0);
		}

		if (sess)
			chathistory_start_catchup (sess);
	}
}

/* The actual CHATHISTORY TARGETS send: window the query from now back to
 * just before the last disconnect.  Shared by the immediate path and by
 * the deferred-start timer, which runs it when the server-driven replay
 * we held it for never materialised. */
static void
send_reconnect_targets_request (server *serv)
{
	gint64 now_val, lower_bound;
	char start_ref[64], end_ref[64];

	if (!serv->have_chathistory || !serv->connected)
		return;

	/* Only fire on reconnect, not first connect */
	if (serv->last_disconnect_time == 0)
		return;

	now_val = (gint64) time (NULL);
	lower_bound = (gint64) serv->last_disconnect_time - CHATHISTORY_FUZZ_INTERVAL;

	chathistory_ts_ref (start_ref, sizeof (start_ref), now_val + CHATHISTORY_FUZZ_INTERVAL);
	chathistory_ts_ref (end_ref, sizeof (end_ref), lower_bound);

	chathistory_request_targets (serv, start_ref, end_ref, 0);
}

void
chathistory_request_targets_on_reconnect (server *serv)
{
	if (!serv)
		return;

	if (persistence_server_drives_replay (serv))
	{
		/* The replay we consented to covers PM correspondents after the
		 * channels, so TARGETS would ask for a list already on its way.
		 * Defer rather than drop — that expectation is not a promise
		 * (see chathistory_schedule_deferred): if no wrapper opens, the
		 * deferred-start timer sends this after the grace delay, and if
		 * one does, chathistory_replay_wrapper_begin clears the flag. */
		serv->chathistory_targets_deferred = TRUE;
		return;
	}

	send_reconnect_targets_request (serv);
}
