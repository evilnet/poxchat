/* PoxChat
 * Copyright (C) 1998-2010 Peter Zelezny.
 * Copyright (C) 2009-2013 Berke Viktor.
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
 */

#include <time.h>
#include "textenums.h"

#ifndef POXCHAT_TEXT_H
#define POXCHAT_TEXT_H

/* timestamp is non-zero if we are using server-time */
#define EMIT_SIGNAL_TIMESTAMP(i, sess, a, b, c, d, e, timestamp) \
	text_emit(i, sess, a, b, c, d, timestamp)
#define EMIT_SIGNAL(i, sess, a, b, c, d, e) \
	text_emit(i, sess, a, b, c, d, 0)

struct text_event
{
	char *name;
	char * const *help;
	int num_args;
	char *def;
};

void scrollback_close (session *sess);
void scrollback_load (session *sess);
/* Materialize a session's deferred scrollback tail right now (no-op unless
 * scrollback_load left it queued).  Call before a tab becomes visible. */
void scrollback_fill_now (session *sess);

/* IRCv3 reactions/replies scrollback persistence */
void scrollback_save_reaction_for_session (session *sess, const char *target_msgid,
                                           const char *reaction_text, const char *nick,
                                           gboolean is_self);
void scrollback_remove_reaction_for_session (session *sess, const char *target_msgid,
                                             const char *reaction_text, const char *nick);
void scrollback_save_reply_for_session (session *sess, const char *msgid,
                                        const char *target_msgid, const char *target_nick,
                                        const char *target_preview);
void scrollback_confirm_pending (session *sess, const char *label, const char *real_msgid);
gboolean scrollback_session_has_msgid (session *sess, const char *msgid,
                                       time_t timestamp);
/* Connect-path timing instrumentation (see text.c); no-op unless the
 * POXCHAT_TIMING env var is set, then logs to c:/tmp/poxchat-timing.log. */
void poxchat_timing_log (const char *fmt, ...);
void scrollback_redact_for_session (session *sess, const char *msgid,
                                    const char *redacted_by, const char *reason,
                                    time_t redact_time);

int text_word_check (char *word, int len);
void PrintText (session *sess, char *text);
void PrintTextTimeStamp (session *sess, char *text, time_t timestamp);
void PrintTextf (session *sess, const char *format, ...) G_GNUC_PRINTF (2, 3);
void PrintTextTimeStampf (session *sess, time_t timestamp, const char *format, ...) G_GNUC_PRINTF (3, 4);
void log_close (session *sess);
void log_open_or_close (session *sess);
void load_text_events (void);
void pevent_save (char *fn);
int pevt_build_string (const char *input, char **output, int *max_arg);
int pevent_load (char *filename);
void pevent_make_pntevts (void);
int text_color_of (char *name);
void text_emit (int index, session *sess, char *a, char *b, char *c, char *d,
		time_t timestamp);
int text_emit_by_name (char *name, session *sess, time_t timestamp,
					   char *a, char *b, char *c, char *d);
gchar *text_convert_invalid (const gchar* text, gssize len, GIConv converter, const gchar *fallback, gsize *len_out);
gchar *text_fixup_invalid_utf8 (const gchar* text, gssize len, gsize *len_out);
int get_stamp_str (char *fmt, time_t tim, char **ret);
void format_event (session *sess, int index, char **args, char *o, gsize sizeofo, unsigned int stripcolor_args);
void text_record_event (session *sess, char *text, time_t stamp, const char *msgid);

/* The msgid of the inbound line being dispatched.  Text events it raises
 * that don't set their own current_msgid — JOIN/PART/QUIT/MODE/NICK/TOPIC
 * lines — store it, so event-playback replays of the same line dedupe
 * against the live row and the newest-msgid catch-up anchor stays current.
 * One row per session per line carries it: a second event from the same
 * line (a multi-mode MODE) would collide with the first on the store's
 * unique (channel, msgid) and be dropped.  Local annotations raised while
 * a line is dispatched (the reconnect marker) must not borrow it at all —
 * bracket them with suspend/resume. */
void text_inbound_msgid_begin (const char *msgid);
void text_inbound_msgid_end (void);
const char *text_inbound_msgid_suspend (void);
void text_inbound_msgid_resume (const char *saved);

/* Reply quotes ("> <nick> preview").  text is a formatted line: either
 * the stored "<nick>\tmessage" form (left_len < 0 — split at the tab) or an
 * xtext entry's str, whose left part is left_len bytes followed by one
 * space.  Colour codes are stripped, the nick's <> / «» trimmed, the
 * preview cut to fit. */
void text_reply_quote (const char *text, int len, int left_len,
                       char *nick, gsize nick_size,
                       char *preview, gsize preview_size);
/* The same from the stored copy of target_msgid, for a target that is not
 * on screen.  FALSE (and empty buffers) when the store has no such row. */
struct scrollback_db;
gboolean text_reply_quote_from_db (struct scrollback_db *db, const char *channel,
                                   const char *target_msgid,
                                   char *nick, gsize nick_size,
                                   char *preview, gsize preview_size);
gboolean text_reply_quote_from_store (session *sess, const char *target_msgid,
                                      char *nick, gsize nick_size,
                                      char *preview, gsize preview_size);
char *text_find_format_string (char *name);

extern const gchar* unicode_fallback_string;
extern const gchar* arbitrary_encoding_fallback_string;

void sound_play (const char *file, gboolean quiet);
void sound_play_event (int i);
void sound_beep (session *);
void sound_load (void);
void sound_save (void);

#endif
