/* X-Chat
 * Copyright (C) 1998 Peter Zelezny.
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

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "inet.h"

#include "poxchat.h"
#include "util.h"
#include "ignore.h"
#include "fe.h"
#include "modes.h"
#include "notify.h"
#include "outbound.h"
#include "inbound.h"
#include "server.h"
#include "servlist.h"
#include "text.h"
#include "ctcp.h"
#include "poxchatc.h"
#include "chanopt.h"
#include "chathistory.h"
#include "scrollback.h"
#include "network-icon.h"
#ifdef USE_LIBWEBSOCKETS
#include "oauth.h"
#endif


void
clear_typing_state (session *sess)
{
	if (sess->typing_sweep_timer)
	{
		fe_timeout_remove (sess->typing_sweep_timer);
		sess->typing_sweep_timer = 0;
	}
	if (sess->typing_send_timer)
	{
		fe_timeout_remove (sess->typing_send_timer);
		sess->typing_send_timer = 0;
	}
	g_slist_free_full (sess->typing_nicks, g_free);
	sess->typing_nicks = NULL;
	sess->typing_last_sent = 0;
	sess->typing_last_keystroke = 0;
	fe_typing_update (sess);
}

void
clear_channel (session *sess)
{
	if (sess->channel[0])
		strcpy (sess->waitchannel, sess->channel);
	sess->channel[0] = 0;
	sess->doing_who = FALSE;
	sess->done_away_check = FALSE;

	log_close (sess);

	if (sess->current_modes)
	{
		g_free (sess->current_modes);
		sess->current_modes = NULL;
	}

	if (sess->mode_timeout_tag)
	{
		fe_timeout_remove (sess->mode_timeout_tag);
		sess->mode_timeout_tag = 0;
	}

	chathistory_cancel_catchup (sess);

	clear_typing_state (sess);

	fe_clear_channel (sess);
	userlist_clear (sess);
	fe_set_nonchannel (sess, FALSE);
	fe_set_title (sess);
}

void
set_topic (session *sess, char *topic, char *stripped_topic)
{
	/* The topic of dialogs are the users hostname which is logged is new */
	if (sess->type == SESS_DIALOG && (!sess->topic || strcmp(sess->topic, stripped_topic))
		&& sess->logfd != -1)
	{
		char tbuf[1024];
		g_snprintf (tbuf, sizeof (tbuf), "[%s has address %s]\n", sess->channel, stripped_topic);
		HC_IGNORE_RESULT (write (sess->logfd, tbuf, strlen (tbuf)));
	}

	g_free (sess->topic);
	sess->topic = g_strdup (stripped_topic);
	fe_set_topic (sess, topic, stripped_topic);
}

static session *
find_session_from_nick (char *nick, server *serv)
{
	session *sess;
	GSList *list = sess_list;

	sess = find_dialog (serv, nick);
	if (sess)
		return sess;

	if (serv->front_session)
	{
		// If we are here for ChanServ, then it is usually a reply for the user
		if (!g_ascii_strcasecmp(nick, "ChanServ") || userlist_find (serv->front_session, nick))
			return serv->front_session;
	}

	if (current_sess && current_sess->server == serv)
	{
		if (userlist_find (current_sess, nick))
			return current_sess;
	}

	while (list)
	{
		sess = list->data;
		if (sess->server == serv)
		{
			if (userlist_find (sess, nick))
				return sess;
		}
		list = list->next;
	}
	return NULL;
}

static session *
inbound_open_dialog (server *serv, char *from,
							const message_tags_data *tags_data)
{
	session *sess;

	sess = new_ircwindow (serv, from, SESS_DIALOG, 0);
	/* for playing sounds */
	EMIT_SIGNAL_TIMESTAMP (XP_TE_OPENDIALOG, sess, NULL, NULL, NULL, NULL, 0,
								  tags_data->timestamp);

	return sess;
}

static void
inbound_make_idtext (server *serv, char *idtext, int max, int id)
{
	idtext[0] = 0;
	if (serv->have_idmsg || serv->have_accnotify)
	{
		if (id)
		{
			safe_strcpy (idtext, prefs.hex_irc_id_ytext, max);
		} else
		{
			safe_strcpy (idtext, prefs.hex_irc_id_ntext, max);
		}
		/* convert codes like %C,%U to the proper ones */
		check_special_chars (idtext, TRUE);
	}
}

void
inbound_privmsg (server *serv, char *from, char *ip, char *text, int id,
					  const message_tags_data *tags_data)
{
	session *sess;
	struct User *user;
	char idtext[64];
	gboolean nodiag = FALSE;

	sess = find_dialog (serv, from);

	if (sess || prefs.hex_gui_autoopen_dialog)
	{
		/*0=ctcp  1=priv will set hex_gui_autoopen_dialog=0 here is flud detected */
		if (!sess)
		{
			if (flood_check (from, ip, serv, current_sess, 1))
				/* Create a dialog session */
				sess = inbound_open_dialog (serv, from, tags_data);
			else
				sess = serv->server_session;
			if (!sess)
				return; /* ?? */
		}

		if (ip && ip[0])
			set_topic (sess, ip, ip);
		inbound_chanmsg (serv, NULL, NULL, from, text, FALSE, tags_data->identified, tags_data);
		return;
	}

	sess = find_session_from_nick (from, serv);
	if (!sess)
	{
		sess = serv->front_session;
		nodiag = TRUE; /* We don't want it to look like a normal message in front sess */
	}

	user = userlist_find (sess, from);
	if (user)
	{
		user->lasttalk = time (0);
		if (user->account)
			id = TRUE;
	}
	
	inbound_make_idtext (serv, idtext, sizeof (idtext), id);

	if (sess->type == SESS_DIALOG && !nodiag)
		EMIT_SIGNAL_TIMESTAMP (XP_TE_DPRIVMSG, sess, from, text, idtext, NULL, 0,
									  tags_data->timestamp);
	else
		EMIT_SIGNAL_TIMESTAMP (XP_TE_PRIVMSG, sess, from, text, idtext, NULL, 0, 
									  tags_data->timestamp);
}

/* used for Alerts section. Masks can be separated by commas and spaces. */

gboolean
alert_match_word (char *word, char *masks)
{
	char *p = masks;
	char endchar;
	int res;

	if (masks[0] == 0)
		return FALSE;

	while (1)
	{
		/* if it's a 0, space or comma, the word has ended. */
		if (*p == 0 || *p == ' ' || *p == ',')
		{
			endchar = *p;
			*p = 0;
			res = match (g_strchug (masks), word);
			*p = endchar;

			if (res)
				return TRUE;	/* yes, matched! */

			masks = p + 1;
			if (*p == 0)
				return FALSE;
		}
		p++;
	}
}

gboolean
alert_match_text (char *text, char *masks)
{
	unsigned char *p = text;
	unsigned char endchar;
	int res;

	if (masks[0] == 0)
		return FALSE;

	while (1)
	{
		if (*p >= '0' && *p <= '9')
		{
			p++;
			continue;
		}

		/* if it's RFC1459 <special>, it can be inside a word */
		switch (*p)
		{
		case '-': case '[': case ']': case '\\':
		case '`': case '^': case '{': case '}':
		case '_': case '|':
			p++;
			continue;
		}

		/* if it's a 0, space or comma, the word has ended. */
		if (*p == 0 || *p == ' ' || *p == ',' ||
			/* if it's anything BUT a letter, the word has ended. */
			 (!g_unichar_isalpha (g_utf8_get_char (p))))
		{
			endchar = *p;
			*p = 0;
			res = alert_match_word (text, masks);
			*p = endchar;

			if (res)
				return TRUE;	/* yes, matched! */

			text = p + g_utf8_skip [p[0]];
			if (*p == 0)
				return FALSE;
		}

		p += g_utf8_skip [p[0]];
	}
}

static int
is_hilight (char *from, char *text, session *sess, server *serv)
{
	if (alert_match_word (from, prefs.hex_irc_no_hilight))
		return 0;

	text = strip_color (text, -1, STRIP_ALL);

	if (alert_match_text (text, serv->nick) ||
		 alert_match_text (text, prefs.hex_irc_extra_hilight) ||
		 alert_match_word (from, prefs.hex_irc_nick_hilight))
	{
		g_free (text);
		if (sess != current_tab)
		{
			sess->tab_state |= TAB_STATE_NEW_HILIGHT;
			lastact_update (sess);
		}
		return 1;
	}

	g_free (text);
	return 0;
}

void
inbound_action (session *sess, char *chan, char *from, char *ip, char *text,
					 int fromme, int id, const message_tags_data *tags_data)
{
	session *def = sess;
	server *serv = sess->server;
	struct User *user;
	char nickchar[2] = "\000";
	char idtext[64];
	int privaction = FALSE;
	char *reply_arrow_text = NULL;

	/* If this action is a reply, prefix the text with the reply sentinel
	 * (U+FDD0 noncharacter); xtext vector-draws the arrow.  See inbound_chanmsg. */
	if (prefs.hex_irc_reply_show && tags_data->all_tags &&
	    g_hash_table_lookup (tags_data->all_tags, "+draft/reply"))
	{
		reply_arrow_text = g_strconcat ("\xef\xb7\x90 ", text, NULL);
		text = reply_arrow_text;
	}

	if (!fromme)
	{
		if (is_channel (serv, chan))
		{
			sess = find_channel (serv, chan);
		} else
		{
			/* it's a private action! */
			privaction = TRUE;
			/* find a dialog tab for it */
			sess = find_dialog (serv, from);
			/* if non found, open a new one */
			if (!sess && prefs.hex_gui_autoopen_dialog)
			{
				/* but only if it wouldn't flood */
				if (flood_check (from, ip, serv, current_sess, 1))
					sess = inbound_open_dialog (serv, from, tags_data);
				else
					sess = serv->server_session;
			}
			if (!sess)
			{
				sess = find_session_from_nick (from, serv);
				/* still not good? */
				if (!sess)
					sess = serv->front_session;
			}
		}
	}

	if (!sess)
		sess = def;

	if (sess != current_tab)
	{
		if (fromme)
			sess->tab_state |= TAB_STATE_NEW_DATA;
		else
			sess->tab_state |= TAB_STATE_NEW_MSG;
		lastact_update (sess);
	}

	user = userlist_find (sess, from);
	if (user)
	{
		nickchar[0] = user->prefix[0];
		user->lasttalk = time (0);
		if (user->account)
			id = TRUE;
		if (user->me)
			fromme = TRUE;
	}

	inbound_make_idtext (serv, idtext, sizeof (idtext), id);

	/* Set current msgid for scrollback_save to capture.
	 * Only overwrite if tags_data provides a real msgid — outbound Tier 2 code
	 * may have already set a "pending:<label>" placeholder that we must preserve. */
	if (tags_data->msgid)
	{
		g_free (sess->current_msgid);
		sess->current_msgid = g_strdup (tags_data->msgid);
		sess->current_msgid_is_user_msg = TRUE;
		chathistory_track_msgid_ts (sess, tags_data->msgid, tags_data->timestamp, FALSE);
	}

	if (!fromme && !privaction)
	{
		if (is_hilight (from, text, sess, serv))
		{
			EMIT_SIGNAL_TIMESTAMP (XP_TE_HCHANACTION, sess, from, text, nickchar,
										  idtext, 0, tags_data->timestamp);
			goto check_action_reply;
		}
	}

	if (fromme)
		EMIT_SIGNAL_TIMESTAMP (XP_TE_UACTION, sess, from, text, nickchar, idtext,
									  0, tags_data->timestamp);
	else if (!privaction)
		EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANACTION, sess, from, text, nickchar,
									  idtext, 0, tags_data->timestamp);
	else if (sess->type == SESS_DIALOG)
		EMIT_SIGNAL_TIMESTAMP (XP_TE_DPRIVACTION, sess, from, text, idtext, NULL,
									  0, tags_data->timestamp);
	else
		EMIT_SIGNAL_TIMESTAMP (XP_TE_PRIVACTION, sess, from, text, idtext, NULL, 0,
									  tags_data->timestamp);

check_action_reply:
	/* IRCv3 +draft/reply on ACTIONs — same current_msgid restore as check_reply */
	if (prefs.hex_irc_reply_show && tags_data->all_tags)
	{
		const char *reply_msgid = g_hash_table_lookup (tags_data->all_tags,
		                                                "+draft/reply");
		if (reply_msgid)
		{
			if (tags_data->msgid && !sess->current_msgid)
			{
				sess->current_msgid = g_strdup (tags_data->msgid);
				fe_reply_context_set (sess, reply_msgid);
				g_free (sess->current_msgid);
				sess->current_msgid = NULL;
			}
			else
				fe_reply_context_set (sess, reply_msgid);
		}
	}

	g_free (reply_arrow_text);
}

void
inbound_chanmsg (server *serv, session *sess, char *chan, char *from,
					  char *text, char fromme, int id,
					  const message_tags_data *tags_data)
{
	struct User *user;
	int hilight = FALSE;
	char nickchar[2] = "\000";
	char idtext[64];
	char *reply_arrow_text = NULL;

	if (!sess)
	{
		if (chan)
		{
			sess = find_channel (serv, chan);
			if (!sess && !is_channel (serv, chan))
				sess = find_dialog (serv, chan);
		} else
		{
			sess = find_dialog (serv, from);
		}
		if (!sess)
			return;
	}

	/* If this message is a reply, prefix the text with the reply sentinel
	 * "\xef\xb7\x90 " (U+FDD0, a Unicode noncharacter — never legitimate content,
	 * so it can't collide with a glyph the user typed).  Not shown as a glyph:
	 * xtext's strip pass turns it into a U+FFFC placeholder and vector-draws the
	 * arrow (xtext_draw_reply_arrow) pointing up at the quoted context line above;
	 * copy/paste strips it back out.  Keep the bytes in sync with
	 * XTEXT_REPLY_SENTINEL (fe-gtk/xtext-render.h).  Caller still owns `text`. */
	if (prefs.hex_irc_reply_show && tags_data->all_tags &&
	    g_hash_table_lookup (tags_data->all_tags, "+draft/reply"))
	{
		reply_arrow_text = g_strconcat ("\xef\xb7\x90 ", text, NULL);
		text = reply_arrow_text;
	}

	/* Set current msgid for scrollback_save to capture.
	 * Only overwrite if tags_data provides a real msgid — outbound Tier 2 code
	 * may have already set a "pending:<label>" placeholder that we must preserve. */
	if (tags_data->msgid)
	{
		g_free (sess->current_msgid);
		sess->current_msgid = g_strdup (tags_data->msgid);
		sess->current_msgid_is_user_msg = TRUE;
	}

	/* Track msgid for deduplication - live messages use is_history=FALSE.
	 * If this was already tracked from chathistory, it's safely skipped. */
	if (tags_data->msgid)
		chathistory_track_msgid_ts (sess, tags_data->msgid, tags_data->timestamp, FALSE);

	if (sess != current_tab)
	{
		sess->tab_state |= TAB_STATE_NEW_MSG;
		lastact_update (sess);
	}

	user = userlist_find (sess, from);
	if (user)
	{
		if (user->account)
			id = TRUE;
		nickchar[0] = user->prefix[0];
		user->lasttalk = time (0);
		if (user->me)
			fromme = TRUE;
	}

	if (fromme)
	{
		if (prefs.hex_away_auto_unmark && serv->is_away && !tags_data->timestamp)
			sess->server->p_set_back (sess->server);
		EMIT_SIGNAL_TIMESTAMP (XP_TE_UCHANMSG, sess, from, text, nickchar, NULL,
									  0, tags_data->timestamp);
		goto check_reply;
	}

	inbound_make_idtext (serv, idtext, sizeof (idtext), id);

	if (is_hilight (from, text, sess, serv))
		hilight = TRUE;

	if (sess->type == SESS_DIALOG)
		EMIT_SIGNAL_TIMESTAMP (XP_TE_DPRIVMSG, sess, from, text, idtext, NULL, 0,
									  tags_data->timestamp);
	else if (hilight)
		EMIT_SIGNAL_TIMESTAMP (XP_TE_HCHANMSG, sess, from, text, nickchar, idtext,
									  0, tags_data->timestamp);
	else
		EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANMSG, sess, from, text, nickchar, idtext,
									  0, tags_data->timestamp);

check_reply:
	/* IRCv3 +draft/reply: attach reply context to the just-emitted message.
	 * EMIT_SIGNAL → PrintTextTimeStamp → fe_print_text clears current_msgid,
	 * but fe_reply_context_set needs it to find the correct entry (especially
	 * in sorted-insert mode where the entry may not be the last one).
	 * Restore it temporarily so the lookup succeeds.
	 */
	if (prefs.hex_irc_reply_show && tags_data->all_tags)
	{
		const char *reply_msgid = g_hash_table_lookup (tags_data->all_tags,
		                                                "+draft/reply");
		if (reply_msgid)
		{
			if (tags_data->msgid && !sess->current_msgid)
			{
				sess->current_msgid = g_strdup (tags_data->msgid);
				fe_reply_context_set (sess, reply_msgid);
				g_free (sess->current_msgid);
				sess->current_msgid = NULL;
			}
			else
				fe_reply_context_set (sess, reply_msgid);
		}
	}

	g_free (reply_arrow_text);
}

void
inbound_newnick (server *serv, char *nick, char *newnick, int quiet,
					  const message_tags_data *tags_data)
{
	int me = FALSE;
	session *sess;
	GSList *list = sess_list;

	if (!serv->p_cmp (nick, serv->nick))
	{
		me = TRUE;
		safe_strcpy (serv->nick, newnick, NICKLEN);
	}

	while (list)
	{
		sess = list->data;
		if (sess->server == serv)
		{
			if (userlist_change (sess, nick, newnick) || (me && sess->type == SESS_SERVER))
			{
				if (!quiet)
				{
					if (me)
						EMIT_SIGNAL_TIMESTAMP (XP_TE_UCHANGENICK, sess, nick, 
													  newnick, NULL, NULL, 0,
													  tags_data->timestamp);
					else
						EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANGENICK, sess, nick,
													  newnick, NULL, NULL, 0, tags_data->timestamp);
				}
			}
			if (sess->type == SESS_DIALOG && !serv->p_cmp (sess->channel, nick))
			{
				safe_strcpy (sess->channel, newnick, CHANLEN);
				fe_set_channel (sess);
			}
			fe_set_title (sess);
		}
		list = list->next;
	}

	dcc_change_nick (serv, nick, newnick);

	if (me)
		fe_set_nick (serv, newnick);
}

/* find a "<none>" tab */
static session *
find_unused_session (server *serv)
{
	session *sess;
	GSList *list = sess_list;
	while (list)
	{
		sess = (session *) list->data;
		if (sess->type == SESS_CHANNEL && sess->channel[0] == 0 &&
			 sess->server == serv)
		{
			if (sess->waitchannel[0] == 0)
				return sess;
		}
		list = list->next;
	}
	return NULL;
}

static session *
find_session_from_waitchannel (char *chan, struct server *serv)
{
	session *sess;
	GSList *list = sess_list;
	while (list)
	{
		sess = (session *) list->data;
		if (sess->server == serv && sess->channel[0] == 0 && sess->type == SESS_CHANNEL)
		{
			if (!serv->p_cmp (chan, sess->waitchannel))
				return sess;
		}
		list = list->next;
	}
	return NULL;
}

void
inbound_ujoin (server *serv, char *chan, char *nick, char *ip,
					const message_tags_data *tags_data)
{
	session *sess;
	int found_unused = FALSE;

	/* already joined? probably a bnc */
	sess = find_channel (serv, chan);

	/* Duplicate-msgid early-out: a bouncer auto-restoring channels can
	 * emit a JOIN burst at registration AND echo the client's later JOIN
	 * with the same msgid as the burst entry.  Without this guard the
	 * second JOIN ran the full ujoin flow — userlist_clear in particular
	 * wiped the userlist that the first JOIN's NAMES burst had filled. */
	if (sess && tags_data->msgid &&
	    chathistory_is_duplicate_msgid (sess, tags_data->msgid, tags_data->timestamp))
		return;

	if (!sess)
	{
		/* see if a window is waiting to join this channel */
		sess = find_session_from_waitchannel (chan, serv);
		if (!sess)
		{
			/* find a "<none>" tab and use that */
			sess = find_unused_session (serv);
			found_unused = sess != NULL;
			if (!sess)
				/* last resort, open a new tab/window */
				sess = new_ircwindow (serv, chan, SESS_CHANNEL, 1);
		}
	}

	safe_strcpy (sess->channel, chan, CHANLEN);
	if (found_unused)
	{
		chanopt_load (sess);
		scrollback_load (sess);
		if (sess->scrollwritten && sess->scrollback_replay_marklast)
			sess->scrollback_replay_marklast (sess);
	}

	fe_set_channel (sess);
	fe_set_title (sess);
	fe_set_nonchannel (sess, TRUE);
	userlist_clear (sess);

	log_open_or_close (sess);

	sess->waitchannel[0] = 0;
	sess->ignore_date = TRUE;
	sess->ignore_mode = TRUE;
	sess->ignore_names = TRUE;
	sess->end_of_names = FALSE;

	/* sends a MODE */
	serv->p_join_info (sess->server, chan);

	/* Track join msgid for chathistory dedup.  The duplicate case was
	 * handled at the top of this function. */
	if (tags_data->msgid)
	{
		chathistory_track_msgid_ts (sess, tags_data->msgid, tags_data->timestamp, FALSE);
		g_free (sess->join_msgid);
		sess->join_msgid = g_strdup (tags_data->msgid);
	}

	/* Show join banner.  Don't save to scrollback — chathistory provides the
	 * canonical JOIN record, and "Now talking on" looks inconsistent when
	 * replayed alongside chathistory's JOIN format.
	 * Use sorted insertion so it lands at the correct chronological position
	 * relative to chathistory messages that may arrive afterwards. */
	sess->display_only = TRUE;
	sess->history_insert_sorted_mode = TRUE;
	EMIT_SIGNAL_TIMESTAMP (XP_TE_UJOIN, sess, nick, chan, ip, NULL, 0,
								  tags_data->timestamp);
	sess->history_insert_sorted_mode = FALSE;
	sess->display_only = FALSE;  /* safety: clear if not consumed */

	/* Bouncer reconnect detection: if the JOIN timestamp predates our last
	 * disconnect, this is a replayed JOIN from the bouncer, not a fresh one.
	 * Emit a "Reconnected" marker at current time to pair with "Disconnected"
	 * and give the user a visual anchor for where live messages begin.
	 * Persisted to scrollback — the server has no knowledge of client
	 * disconnects, so only the client can maintain these markers.
	 *
	 * last_disconnect_time is ephemeral (in-memory), so after a client restart
	 * we fall back to scrollback_newest_time which reflects the "Disconnected"
	 * entry saved on exit. */
	{
		time_t disconnect_ref = serv->last_disconnect_time;
		if (disconnect_ref == 0)
			disconnect_ref = sess->scrollback_newest_time;

		if (tags_data->timestamp > 0 &&
		    disconnect_ref > 0 &&
		    tags_data->timestamp <= disconnect_ref)
		{
			EMIT_SIGNAL_TIMESTAMP (XP_TE_RECONNECT, sess, NULL, NULL, NULL, NULL, 0,
			                       time (NULL));

			/* Same signal that fires this branch tells us the server
			 * keeps channel state across our disconnects — record it so
			 * check_autojoin_channels suppresses the rejoin-on-reconnect
			 * path even without an explicit FLAG_PERSISTENT toggle.
			 * Reset on every server_disconnect (see server.c). */
			serv->bouncer_inferred = TRUE;
		}
	}

	/* Schedule deferred CHATHISTORY LATEST for all channels.
	 * Each 366 resets the 2-second timer so it fires after all JOINs settle,
	 * letting WHO/MODE drain from the send queue first. */
	chathistory_schedule_deferred (serv);

	/* Query server read position so we can position the marker line */
	if (serv->have_read_marker)
		tcp_sendf (serv, "MARKREAD %s\r\n", chan);

	if (prefs.hex_irc_who_join)
	{
		/* sends WHO #channel */
		serv->p_user_list (sess->server, chan);
		sess->doing_who = TRUE;
		sess->done_away_check = TRUE;	/* prevent away_check timer from re-WHOing */
	}
}

void
inbound_ukick (server *serv, char *chan, char *kicker, char *reason,
					const message_tags_data *tags_data)
{
	session *sess = find_channel (serv, chan);
	if (sess)
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_UKICK, sess, serv->nick, chan, kicker, 
									  reason, 0, tags_data->timestamp);
		clear_channel (sess);
		if (prefs.hex_irc_auto_rejoin)
		{
			serv->p_join (serv, chan, sess->channelkey);
			safe_strcpy (sess->waitchannel, chan, CHANLEN);
		}
	}
}

void
inbound_upart (server *serv, char *chan, char *ip, char *reason,
					const message_tags_data *tags_data)
{
	session *sess = find_channel (serv, chan);
	if (sess)
	{
		if (*reason)
			EMIT_SIGNAL_TIMESTAMP (XP_TE_UPARTREASON, sess, serv->nick, ip, chan,
										  reason, 0, tags_data->timestamp);
		else
			EMIT_SIGNAL_TIMESTAMP (XP_TE_UPART, sess, serv->nick, ip, chan, NULL,
										  0, tags_data->timestamp);
		clear_channel (sess);
	}
}

void
inbound_nameslist (server *serv, char *chan, char *names,
						 const message_tags_data *tags_data)
{
	session *sess;
	char **name_list;
	char *host, *nopre_name;
	char name[NICKLEN];
	int i;
	size_t offset;

	sess = find_channel (serv, chan);
	if (!sess)
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_USERSONCHAN, serv->server_session, chan,
									  names, NULL, NULL, 0, tags_data->timestamp);
		return;
	}
	if (!sess->ignore_names)
		EMIT_SIGNAL_TIMESTAMP (XP_TE_USERSONCHAN, sess, chan, names, NULL, NULL,
									  0, tags_data->timestamp);

	if (sess->end_of_names)
	{
		sess->end_of_names = FALSE;
		userlist_clear (sess);
	}

	name_list = g_strsplit (names, " ", -1);
	for (i = 0; name_list[i]; i++)
	{
		host = NULL;
		offset = sizeof(name);

		if (name_list[i][0] == 0)
			continue;

		if (serv->have_uhnames)
		{
			offset = 0;
			nopre_name = name_list[i];

			/* Ignore prefixes so '!' won't cause issues */
			while (strchr (serv->nick_prefixes, *nopre_name) != NULL)
			{
				nopre_name++;
				offset++;
			}

			offset += strcspn (nopre_name, "!");
			if (offset++ < strlen (name_list[i]))
				host = name_list[i] + offset;
		}

		g_strlcpy (name, name_list[i], MIN(offset, sizeof(name)));

		userlist_add (sess, name, host, NULL, NULL, tags_data);
	}
	g_strfreev (name_list);
}

void
inbound_topic (server *serv, char *chan, char *topic_text,
					const message_tags_data *tags_data)
{
	session *sess = find_channel (serv, chan);
	char *stripped_topic;

	if (sess)
	{
		stripped_topic = strip_color (topic_text, -1, STRIP_ALL);
		set_topic (sess, topic_text, stripped_topic);
		g_free (stripped_topic);
	} else
		sess = serv->server_session;

	EMIT_SIGNAL_TIMESTAMP (XP_TE_TOPIC, sess, chan, topic_text, NULL, NULL, 0,
								  tags_data->timestamp);
}

void
inbound_topicnew (server *serv, char *nick, char *chan, char *topic,
						const message_tags_data *tags_data)
{
	session *sess;
	char *stripped_topic;

	sess = find_channel (serv, chan);
	if (sess)
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_NEWTOPIC, sess, nick, topic, chan, NULL, 0,
									  tags_data->timestamp);
		stripped_topic = strip_color (topic, -1, STRIP_ALL);
		set_topic (sess, topic, stripped_topic);
		g_free (stripped_topic);
	}
}

void
inbound_join (server *serv, char *chan, char *user, char *ip, char *account,
				  char *realname, const message_tags_data *tags_data)
{
	session *sess = find_channel (serv, chan);
	if (sess)
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_JOIN, sess, user, chan, ip, account, 0,
									  tags_data->timestamp);
		userlist_add (sess, user, ip, account, realname, tags_data);
		notify_account_observed (serv, user, account, tags_data);
	}
}

void
inbound_kick (server *serv, char *chan, char *user, char *kicker, char *reason,
				  const message_tags_data *tags_data)
{
	session *sess = find_channel (serv, chan);
	if (sess)
	{
		char *kick_account = NULL;
		struct User *u = userlist_find (sess, user);
		if (u && u->account)
			kick_account = g_strdup (u->account);

		EMIT_SIGNAL_TIMESTAMP (XP_TE_KICK, sess, kicker, user, chan, reason, 0,
									  tags_data->timestamp);
		userlist_remove (sess, user);

		if (kick_account)
		{
			notify_account_cleared (serv, kick_account, tags_data);
			g_free (kick_account);
		}
	}
}

void
inbound_part (server *serv, char *chan, char *user, char *ip, char *reason,
				  const message_tags_data *tags_data)
{
	session *sess = find_channel (serv, chan);
	if (sess)
	{
		char *part_account = NULL;
		struct User *u = userlist_find (sess, user);
		if (u && u->account)
			part_account = g_strdup (u->account);

		if (*reason)
			EMIT_SIGNAL_TIMESTAMP (XP_TE_PARTREASON, sess, user, ip, chan, reason,
										  0, tags_data->timestamp);
		else
			EMIT_SIGNAL_TIMESTAMP (XP_TE_PART, sess, user, ip, chan, NULL, 0,
										  tags_data->timestamp);
		userlist_remove (sess, user);

		/* After the user leaves this channel, they may still be in other
		 * channels with us (still "online") — notify_account_cleared does
		 * the server-wide scan and only flips offline if nobody else on
		 * the server carries the account. */
		if (part_account)
		{
			notify_account_cleared (serv, part_account, tags_data);
			g_free (part_account);
		}
	}
}

void
inbound_topictime (server *serv, char *chan, char *nick, time_t stamp,
						 const message_tags_data *tags_data)
{
	char *tim = ctime (&stamp);
	session *sess = find_channel (serv, chan);

	if (!sess)
		sess = serv->server_session;

	if (tim != NULL)
		tim[24] = 0;	/* get rid of the \n */

	EMIT_SIGNAL_TIMESTAMP (XP_TE_TOPICDATE, sess, chan, nick, tim, NULL, 0,
								  tags_data->timestamp);
}

void
inbound_quit (server *serv, char *nick, char *ip, char *reason,
				  const message_tags_data *tags_data)
{
	GSList *list = sess_list;
	session *sess;
	struct User *user;
	int was_on_front_session = FALSE;
	char *quit_account = NULL;

	while (list)
	{
		sess = (session *) list->data;
		if (sess->server == serv)
		{
 			if (sess == current_sess)
 				was_on_front_session = TRUE;
			if ((user = userlist_find (sess, nick)))
			{
				if (!quit_account && user->account)
					quit_account = g_strdup (user->account);
				EMIT_SIGNAL_TIMESTAMP (XP_TE_QUIT, sess, nick, reason, ip, NULL, 0,
											  tags_data->timestamp);
				userlist_remove_user (sess, user);
			} else if (sess->type == SESS_DIALOG && !serv->p_cmp (sess->channel, nick))
			{
				EMIT_SIGNAL_TIMESTAMP (XP_TE_QUIT, sess, nick, reason, ip, NULL, 0,
											  tags_data->timestamp);
			}
		}
		list = list->next;
	}

	/* After the user is fully removed, their account may no longer be
	 * carried by anyone on the server — trigger the friend-offline
	 * transition (guarded against multi-nick-per-account). */
	if (quit_account)
	{
		notify_account_cleared (serv, quit_account, tags_data);
		g_free (quit_account);
	}

	notify_set_offline (serv, nick, was_on_front_session, tags_data);
}

void
inbound_account (server *serv, char *nick, char *account,
					  const message_tags_data *tags_data)
{
	session *sess = NULL;
	GSList *list;
	char *old_account = NULL;

	/* Snapshot the current account before propagation overwrites it, so
	 * we can detect logout / change transitions for friend presence.
	 * user->account is the same across channels for a given nick on a
	 * server, so the first match suffices. */
	list = sess_list;
	while (list)
	{
		sess = list->data;
		if (sess->server == serv)
		{
			struct User *user = userlist_find (sess, nick);
			if (user && user->account)
			{
				old_account = g_strdup (user->account);
				break;
			}
		}
		list = list->next;
	}

	list = sess_list;
	while (list)
	{
		sess = list->data;
		if (sess->server == serv)
			userlist_set_account (sess, nick, account);
		list = list->next;
	}

	notify_account_observed (serv, nick, account, tags_data);

	/* If the account just cleared or switched, the previous account may
	 * need to go offline — notify_account_cleared checks that no other
	 * nick on the server is still carrying it. */
	if (old_account
	    && (!account || !*account
	        || !strcmp (account, "*") || !strcmp (account, "0")
	        || strcmp (account, old_account) != 0))
		notify_account_cleared (serv, old_account, tags_data);

	g_free (old_account);
}

void
inbound_ping_reply (session *sess, char *timestring, char *from,
						  const message_tags_data *tags_data)
{
	unsigned long tim, nowtim, dif;
	int lag = 0;
	char outbuf[64];

	if (strncmp (timestring, "LAG", 3) == 0)
	{
		timestring += 3;
		lag = 1;
	}

	tim = strtoul (timestring, NULL, 10);
	nowtim = make_ping_time ();
	dif = nowtim - tim;

	sess->server->ping_recv = time (0);

	if (lag)
	{
		sess->server->lag_sent = 0;
		sess->server->lag = dif;
		fe_set_lag (sess->server, dif);
		return;
	}

	if (atol (timestring) == 0)
	{
		if (sess->server->lag_sent)
			sess->server->lag_sent = 0;
		else
			EMIT_SIGNAL_TIMESTAMP (XP_TE_PINGREP, sess, from, "?", NULL, NULL, 0,
										  tags_data->timestamp);
	} else
	{
		g_snprintf (outbuf, sizeof (outbuf), "%ld.%03ld", dif / 1000, dif % 1000);
		EMIT_SIGNAL_TIMESTAMP (XP_TE_PINGREP, sess, from, outbuf, NULL, NULL, 0,
									  tags_data->timestamp);
	}
}

static session *
find_session_from_type (int type, server *serv)
{
	session *sess;
	GSList *list = sess_list;
	while (list)
	{
		sess = list->data;
		if (sess->type == type && serv == sess->server)
			return sess;
		list = list->next;
	}
	return NULL;
}

void
inbound_notice (server *serv, char *to, char *nick, char *msg, char *ip, int id,
					 const message_tags_data *tags_data)
{
	char *ptr = to;
	session *sess = 0;
	int server_notice = FALSE;

	if (is_channel (serv, ptr))
		sess = find_channel (serv, ptr);

	/* /notice [mode-prefix]#channel should end up in that channel */
	if (!sess && ptr[0] && strchr(serv->nick_prefixes, ptr[0]) != NULL)
	{
		ptr++;
		sess = find_channel (serv, ptr);
	}

	if (strcmp (nick, ip) == 0)
		server_notice = TRUE;

	if (!sess)
	{
		ptr = 0;
		if (prefs.hex_irc_notice_pos == 0)
		{
											/* paranoia check */
			if (msg[0] == '[' && (!serv->have_idmsg || id))
			{
				/* guess where chanserv meant to post this -sigh- */
				if (!g_ascii_strcasecmp (nick, "ChanServ") && !find_dialog (serv, nick))
				{
					char *dest = g_strdup (msg + 1);
					char *end = strchr (dest, ']');
					if (end)
					{
						*end = 0;
						sess = find_channel (serv, dest);
					}
					g_free (dest);
				}
			}
			if (!sess)
				sess = find_session_from_nick (nick, serv);
		} else if (prefs.hex_irc_notice_pos == 1)
		{
			int stype = server_notice ? SESS_SNOTICES : SESS_NOTICES;
			sess = find_session_from_type (stype, serv);
			if (!sess)
			{
				if (stype == SESS_NOTICES)
					sess = new_ircwindow (serv, "(notices)", SESS_NOTICES, 0);
				else
					sess = new_ircwindow (serv, "(snotices)", SESS_SNOTICES, 0);
				fe_set_channel (sess);
				fe_set_title (sess);
				fe_set_nonchannel (sess, FALSE);
				userlist_clear (sess);
				log_open_or_close (sess);
			}
			/* Avoid redundancy with some Undernet notices */
			if (!strncmp (msg, "*** Notice -- ", 14))
				msg += 14;
		} else
		{
			sess = serv->front_session;
		}

		if (!sess)
		{
			if (server_notice)	
				sess = serv->server_session;
			else
				sess = serv->front_session;
		}
	}

	if (msg[0] == '\001')
	{
		size_t len;

		msg++;
		if (!strncmp (msg, "PING", 4))
		{
			inbound_ping_reply (sess, msg + 5, nick, tags_data);
			return;
		}

		len = strlen(msg);
		if (msg[len - 1] == '\001')
			msg[len - 1] = '\000';
	}

	/* Set current msgid for scrollback_save to capture.
	 * Only overwrite if tags_data provides a real msgid — outbound Tier 2 code
	 * may have already set a "pending:<label>" placeholder that we must preserve. */
	if (tags_data->msgid)
	{
		g_free (sess->current_msgid);
		sess->current_msgid = g_strdup (tags_data->msgid);
		sess->current_msgid_is_user_msg = TRUE;
		chathistory_track_msgid_ts (sess, tags_data->msgid, tags_data->timestamp, FALSE);
	}

	/* Self-echo of our own NOTICE (echo-message / bouncer) */
	if (serv->have_echo_message && !serv->p_cmp (nick, serv->nick))
	{
		EMIT_SIGNAL_TIMESTAMP (XP_TE_NOTICESEND, sess, to, msg, NULL, NULL, 0,
		                       tags_data->timestamp);
		return;
	}

	if (server_notice)
		EMIT_SIGNAL_TIMESTAMP (XP_TE_SERVNOTICE, sess, msg, nick, NULL, NULL, 0,
									  tags_data->timestamp);
	else if (ptr)
		EMIT_SIGNAL_TIMESTAMP (XP_TE_CHANNOTICE, sess, nick, to, msg, NULL, 0,
									  tags_data->timestamp);
	else
		EMIT_SIGNAL_TIMESTAMP (XP_TE_NOTICE, sess, nick, msg, NULL, NULL, 0,
									  tags_data->timestamp);
}

void
inbound_away (server *serv, char *nick, char *msg,
				  const message_tags_data *tags_data)
{
	struct away_msg *away = server_away_find_message (serv, nick);
	session *sess = NULL;
	GSList *list;

	if (away && !strcmp (msg, away->message))	/* Seen the msg before? */
	{
		if (prefs.hex_away_show_once && !serv->inside_whois)
			return;
	} else
	{
		server_away_save_message (serv, nick, msg);
	}

	if (prefs.hex_irc_whois_front)
		sess = serv->front_session;
	else
	{
		if (!serv->inside_whois)
			sess = find_session_from_nick (nick, serv);
		if (!sess)
			sess = serv->server_session;
	}

	/* possibly hide the output */
	if (!serv->inside_whois || !serv->skip_next_whois)
		EMIT_SIGNAL_TIMESTAMP (XP_TE_WHOIS5, sess, nick, msg, NULL, NULL, 0,
									  tags_data->timestamp);

	list = sess_list;
	while (list)
	{
		sess = list->data;
		if (sess->server == serv)
			userlist_set_away (sess, nick, TRUE);
		list = list->next;
	}
}

void
inbound_away_notify (server *serv, char *nick, char *reason,
							const message_tags_data *tags_data)
{
	session *sess = NULL;
	GSList *list;

	list = sess_list;
	while (list)
	{
		sess = list->data;
		if (sess->server == serv)
		{
			userlist_set_away (sess, nick, reason ? TRUE : FALSE);
			if (sess == serv->front_session)
			{
				struct User *user = userlist_find (sess, nick);
				const char *account = user ? user->account : NULL;
				if (notify_is_in_list (serv, nick, account))
				{
					if (reason)
						EMIT_SIGNAL_TIMESTAMP (XP_TE_NOTIFYAWAY, sess, nick, reason, NULL,
													  NULL, 0, tags_data->timestamp);
					else
						EMIT_SIGNAL_TIMESTAMP (XP_TE_NOTIFYBACK, sess, nick, NULL, NULL,
													  NULL, 0, tags_data->timestamp);
				}
			}
		}
		list = list->next;
	}
}

int
inbound_nameslist_end (server *serv, char *chan,
							  const message_tags_data *tags_data)
{
	session *sess;
	GSList *list;

	if (!strcmp (chan, "*"))
	{
		list = sess_list;
		while (list)
		{
			sess = list->data;
			if (sess->server == serv)
			{
				sess->end_of_names = TRUE;
				sess->ignore_names = FALSE;
				fe_userlist_numbers (sess);
			}
			list = list->next;
		}
		return TRUE;
	}
	sess = find_channel (serv, chan);
	if (sess)
	{
		sess->end_of_names = TRUE;
		sess->ignore_names = FALSE;
		fe_userlist_numbers (sess);
		return TRUE;
	}
	return FALSE;
}

static gboolean
check_autojoin_channels (server *serv)
{
	int i = 0;
	session *sess;
	GSList *list = sess_list;
	GSList *sess_channels = NULL;			/* joined channels that are not in the favorites list */
	favchannel *fav;

	/* shouldn't really happen, the io tag is destroyed in server.c */
	if (!is_server (serv))
	{
		return FALSE;
	}

	/* Suppress the rejoin-on-reconnect path entirely when the server is
	 * known to keep our channel state across the disconnect — either set
	 * explicitly via the network's "persistent server" toggle, or
	 * inferred at runtime when the server already replayed a self-JOIN
	 * with an @time predating our last disconnect.  In both cases the
	 * server will (re-)issue JOIN events for every channel we belong to;
	 * sending our own duplicates them, undoes the user's intervening
	 * PARTs from other clients, and races with the server-replayed
	 * JOIN's NAMES burst.  The favlist path below is intentionally not
	 * gated — a configured favourite is the user's explicit ask. */
	if (serv->persistent_server || serv->bouncer_inferred)
		list = NULL;

	/* If there's a session (i.e. this is a reconnect), autojoin to everything that was open previously. */
	while (list)
	{
		sess = list->data;

		if (sess->server == serv)
		{
			if (sess->willjoinchannel[0] != 0)
			{
				strcpy (sess->waitchannel, sess->willjoinchannel);
				sess->willjoinchannel[0] = 0;

				fav = servlist_favchan_find (serv->network, sess->waitchannel, NULL);	/* Is this channel in our favorites? */

				/* session->channelkey is initially unset for channels joined from the favorites. You have to fill them up manually from favorites settings. */
				if (fav)
				{
					/* session->channelkey is set if there was a key change during the session. In that case, use the session key, not the one from favorites. */
					if (fav->key && !strlen (sess->channelkey))
					{
						safe_strcpy (sess->channelkey, fav->key, sizeof (sess->channelkey));
					}
				}

				/* for easier checks, ensure that favchannel->key is just NULL when session->channelkey is empty i.e. '' */
				if (strlen (sess->channelkey))
				{
					sess_channels = servlist_favchan_listadd (sess_channels, sess->waitchannel, sess->channelkey);
				}
				else
				{
					sess_channels = servlist_favchan_listadd (sess_channels, sess->waitchannel, NULL);
				}
				i++;
			}
		}

		list = list->next;
	}

	if (sess_channels)
	{
		serv->p_join_list (serv, sess_channels);
		g_slist_free_full (sess_channels, (GDestroyNotify) servlist_favchan_free);
	}
	else
	{
		/* If there's no session, just autojoin to favorites. */
		if (serv->favlist)
		{
			serv->p_join_list (serv, serv->favlist);
			i++;

			/* FIXME this is not going to work and is not needed either. server_free() does the job already. */
			/* g_slist_free_full (serv->favlist, (GDestroyNotify) servlist_favchan_free); */
		}
	}

	serv->joindelay_tag = 0;
	fe_server_event (serv, FE_SE_LOGGEDIN, i);
	return FALSE;
}

void
inbound_next_nick (session *sess, char *nick, int error,
						 const message_tags_data *tags_data)
{
	char *newnick;
	server *serv = sess->server;
	ircnet *net;

	serv->nickcount++;

	switch (serv->nickcount)
	{
	case 2:
		newnick = prefs.hex_irc_nick2;
		net = serv->network;
		/* use network specific "Second choice"? */
		if (net && !(net->flags & FLAG_USE_GLOBAL) && net->nick2)
		{
			newnick = net->nick2;
		}
		serv->p_change_nick (serv, newnick);
		if (error)
		{
			EMIT_SIGNAL_TIMESTAMP (XP_TE_NICKERROR, sess, nick, newnick, NULL, NULL,
										  0, tags_data->timestamp);
		}
		else
		{
			EMIT_SIGNAL_TIMESTAMP (XP_TE_NICKCLASH, sess, nick, newnick, NULL, NULL,
										  0, tags_data->timestamp);
		}
		break;

	case 3:
		serv->p_change_nick (serv, prefs.hex_irc_nick3);
		if (error)
		{
			EMIT_SIGNAL_TIMESTAMP (XP_TE_NICKERROR, sess, nick, prefs.hex_irc_nick3,
										  NULL, NULL, 0, tags_data->timestamp);
		}
		else
		{
			EMIT_SIGNAL_TIMESTAMP (XP_TE_NICKCLASH, sess, nick, prefs.hex_irc_nick3,
										  NULL, NULL, 0, tags_data->timestamp);
		}
		break;

	default:
		EMIT_SIGNAL_TIMESTAMP (XP_TE_NICKFAIL, sess, NULL, NULL, NULL, NULL, 0,
									  tags_data->timestamp);
	}
}


static void
dns_addr_callback (GObject *obj, GAsyncResult *result, gpointer user_data)
{
	GResolver *resolver = G_RESOLVER(obj);
	session *sess = (session*)user_data;
	gchar *addr;

	g_return_if_fail (is_session(sess));

	addr = g_resolver_lookup_by_address_finish (resolver, result, NULL);
	if (addr)
		PrintTextf (sess, _("Resolved to %s"), addr);
	else
		PrintText (sess, _("Not found"));
}

static void
dns_name_callback (GObject *obj, GAsyncResult *result, gpointer user_data)
{
	GResolver *resolver = G_RESOLVER(obj);
	session *sess = (session*)user_data;
	GList* addrs;
	gchar* addr;
	GList* list;

	g_return_if_fail (is_session (sess));

	addrs = g_resolver_lookup_by_name_finish (resolver, result, NULL);
	if (addrs)
	{
		PrintText (sess, _("Resolved to:"));

		for (list = g_list_first (addrs); list; list = g_list_next (list))
		{
			addr = g_inet_address_to_string (list->data);
			PrintTextf (sess, "    %s", addr);
		}

		g_resolver_free_addresses (addrs);
	}
	else
		PrintText (sess, _("Not found"));
}

void
do_dns (session *sess, char *nick, char *host,
		const message_tags_data *tags_data)
{
	GResolver *res = g_resolver_get_default ();
	GInetAddress *addr;
	char *po;

	po = strrchr (host, '@');
	if (po)
		host = po + 1;

	if (nick)
		EMIT_SIGNAL_TIMESTAMP (XP_TE_RESOLVINGUSER, sess, nick, host, NULL, NULL, 0,
								tags_data->timestamp);

	PrintTextf (sess, _("Looking up %s..."), host);

	addr = g_inet_address_new_from_string (host);
	if (addr)
		g_resolver_lookup_by_address_async (res, addr, NULL, dns_addr_callback, sess);
	else
		g_resolver_lookup_by_name_async (res, host, NULL, dns_name_callback, sess);
}

static void
set_default_modes (server *serv)
{
	char modes[8];

	modes[0] = '+';
	modes[1] = '\0';

	if (prefs.hex_irc_wallops)
		strcat (modes, "w");
	if (prefs.hex_irc_servernotice)
		strcat (modes, "s");
	if (prefs.hex_irc_invisible)
		strcat (modes, "i");
	if (prefs.hex_irc_hidehost)
		strcat (modes, "x");

	if (modes[1] != '\0')
	{
		serv->p_mode (serv, serv->nick, modes);
	}
}

void
inbound_login_start (session *sess, char *nick, char *servname,
							const message_tags_data *tags_data)
{
	inbound_newnick (sess->server, sess->server->nick, nick, TRUE, tags_data);
	server_set_name (sess->server, servname);
	if (sess->type == SESS_SERVER)
		log_open_or_close (sess);
	/* reset our away status */
	if (sess->server->reconnect_away)
	{
		handle_command (sess->server->server_session, "away", FALSE);
		sess->server->reconnect_away = FALSE;
	}
}

static void
inbound_set_all_away_status (server *serv, char *nick, unsigned int status)
{
	GSList *list;
	session *sess;

	list = sess_list;
	while (list)
	{
		sess = list->data;
		if (sess->server == serv)
			userlist_set_away (sess, nick, status);
		list = list->next;
	}
}

void
inbound_uaway (server *serv, const message_tags_data *tags_data)
{
	serv->is_away = TRUE;
	serv->away_time = time (NULL);
	fe_set_away (serv);

	inbound_set_all_away_status (serv, serv->nick, 1);
}

void
inbound_uback (server *serv, const message_tags_data *tags_data)
{
	serv->is_away = FALSE;
	serv->reconnect_away = FALSE;
	fe_set_away (serv);

	inbound_set_all_away_status (serv, serv->nick, 0);
}

static void
foundip_resolved_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	GResolver *resolver = G_RESOLVER (source);
	server *serv = (server *) user_data;
	GList *addrs;
	GInetAddress *addr;

	if (!is_server (serv))
		return;

	addrs = g_resolver_lookup_by_name_finish (resolver, result, NULL);
	if (addrs)
	{
		addr = addrs->data;
		if (g_inet_address_get_family (addr) == G_SOCKET_FAMILY_IPV4)
		{
			memcpy (&serv->dcc_ip, g_inet_address_to_bytes (addr), 4);

			char *ip_str = g_inet_address_to_string (addr);
			EMIT_SIGNAL (XP_TE_FOUNDIP, serv->server_session,
						 ip_str, NULL, NULL, NULL, 0);
			g_free (ip_str);
		}
		g_resolver_free_addresses (addrs);
	}
}

void
inbound_foundip (session *sess, char *ip, const message_tags_data *tags_data)
{
	GInetAddress *addr;

	/* Try parsing as a numeric IP first — no DNS needed */
	addr = g_inet_address_new_from_string (ip);
	if (addr)
	{
		if (g_inet_address_get_family (addr) == G_SOCKET_FAMILY_IPV4)
		{
			memcpy (&sess->server->dcc_ip, g_inet_address_to_bytes (addr), 4);
			EMIT_SIGNAL_TIMESTAMP (XP_TE_FOUNDIP, sess->server->server_session,
								   ip, NULL, NULL, NULL, 0, tags_data->timestamp);
		}
		g_object_unref (addr);
	}
	else
	{
		/* Hostname — resolve asynchronously to avoid blocking the event loop.
		 * gethostbyname() on an IRC virtual host like "user.Users.Network" can
		 * stall for the full DNS timeout (~12 s on Windows), freezing the UI. */
		GResolver *res = g_resolver_get_default ();
		g_resolver_lookup_by_name_async (res, ip, NULL,
										 foundip_resolved_cb, sess->server);
	}
}

void
inbound_user_info_start (session *sess, char *nick,
								 const message_tags_data *tags_data)
{
	/* set away to FALSE now, 301 may turn it back on */
	inbound_set_all_away_status (sess->server, nick, 0);
}

/* reporting new information found about this user. chan may be NULL.
 * away may be 0xff to indicate UNKNOWN. */

void
inbound_user_info (session *sess, char *chan, char *user, char *host,
						 char *servname, char *nick, char *realname,
						 char *account, unsigned int away,
						 const message_tags_data *tags_data)
{
	server *serv = sess->server;
	session *who_sess;
	GSList *list;
	char *uhost = NULL;

	if (user && host)
	{
		uhost = g_strdup_printf ("%s@%s", user, host);
	}

	if (chan)
	{
		who_sess = find_channel (serv, chan);
		if (who_sess)
			userlist_add_hostname (who_sess, nick, uhost, realname, servname, account, away);
		else
		{
			if (serv->doing_dns && nick && host)
				do_dns (sess, nick, host, tags_data);
		}
	}
	else
	{
		/* came from WHOIS, not channel specific */
		for (list = sess_list; list; list = list->next)
		{
			sess = list->data;
			if (sess->server != serv)
				continue;

			if (sess->type == SESS_CHANNEL)
			{
				userlist_add_hostname (sess, nick, uhost, realname, servname, account, away);
			}
			else if (sess->type == SESS_DIALOG && uhost && !serv->p_cmp (sess->channel, nick))
			{
				set_topic (sess, uhost, uhost);
			}
		}
	}

	g_free (uhost);

	/* Single observation call at the end covers both the channel-specific
	 * and WHOIS-broadcast paths without duplicating the match walk. */
	if (account && *account)
		notify_account_observed (serv, nick, account, tags_data);
}

int
inbound_banlist (session *sess, time_t stamp, char *chan, char *mask,
					  char *banner, int rplcode, const message_tags_data *tags_data)
{
	char *time_str = ctime (&stamp);
	server *serv = sess->server;
	char *nl;

	if (stamp <= 0 || time_str == NULL)
	{
		time_str = "";
	}
	else
	{
		if ((nl = strchr (time_str, '\n')))
			*nl = 0;
	}

	sess = find_channel (serv, chan);
	if (!sess)
	{
		sess = serv->front_session;
		goto nowindow;
	}

	if (!fe_add_ban_list (sess, mask, banner, time_str, rplcode))
	{
nowindow:

		EMIT_SIGNAL_TIMESTAMP (XP_TE_BANLIST, sess, chan, mask, banner, time_str,
									  0, tags_data->timestamp);
		return TRUE;
	}

	return TRUE;
}

/* execute 1 end-of-motd command */

static int
inbound_exec_eom_cmd (char *str, void *sess)
{
	char *cmd;

	cmd = command_insert_vars ((session*)sess, (str[0] == '/') ? str + 1 : str);
	handle_command ((session*)sess, cmd, TRUE);
	g_free (cmd);

	return 1;
}

static int
inbound_nickserv_login (server *serv)
{
	/* this could grow ugly, but let's hope there won't be new NickServ types */
	switch (serv->loginmethod)
	{
		case LOGIN_MSG_NICKSERV:
		case LOGIN_NICKSERV:
		case LOGIN_CHALLENGEAUTH:
			return 1;
		default:
			return 0;
	}
}

void
inbound_login_end (session *sess, char *text, const message_tags_data *tags_data)
{
	GSList *cmdlist;
	commandentry *cmd;
	server *serv = sess->server;
	ircnet *net = serv->network;

	if (!serv->end_of_motd)
	{
		serv->end_of_motd = TRUE;

		if (prefs.hex_dcc_ip_from_server && serv->use_who)
		{
			serv->skip_next_userhost = TRUE;
			serv->p_get_ip_uh (serv, serv->nick);	/* sends USERHOST mynick */
		}
		set_default_modes (serv);

		if (net)
		{
			/* there may be more than 1, separated by \n */

			cmdlist = net->commandlist;
			while (cmdlist)
			{
				cmd = cmdlist->data;
				inbound_exec_eom_cmd (cmd->command, sess);
				cmdlist = cmdlist->next;
			}
		}
		/* The previously ran commands can alter the state of the server */
		if (serv->network != net)
			return;

		/* send nickserv password */
		if (net && net->pass && inbound_nickserv_login (serv))
		{
			serv->p_ns_identify (serv, net->pass);
		}

		/* wait for join if command or nickserv set */
		if (net && prefs.hex_irc_join_delay
			&& ((net->pass && inbound_nickserv_login (serv))
				|| net->commandlist))
		{
			serv->joindelay_tag = fe_timeout_add_seconds (prefs.hex_irc_join_delay, check_autojoin_channels, serv);
		}
		else
		{
			check_autojoin_channels (serv);
		}

		if (serv->supports_watch || serv->supports_monitor)
		{
			notify_send_watches (serv);
		}

		/* If we loaded an icon from cache but ISUPPORT didn't advertise ICON
		 * this connection, the server has stopped offering it — clear it. */
		if (!serv->network_icon_url && serv->network_icon)
		{
			g_object_unref (serv->network_icon);
			serv->network_icon = NULL;
			network_icon_clear_cache (serv);
		}

		/* Discover missed DMs via CHATHISTORY TARGETS on reconnect */
		chathistory_request_targets_on_reconnect (serv);
	}

	if (prefs.hex_irc_skip_motd && !serv->motd_skipped)
	{
		serv->motd_skipped = TRUE;
		EMIT_SIGNAL_TIMESTAMP (XP_TE_MOTDSKIP, serv->server_session, NULL, NULL,
									  NULL, NULL, 0, tags_data->timestamp);
		return;
	}

	EMIT_SIGNAL_TIMESTAMP (XP_TE_MOTD, serv->server_session, text, NULL, NULL,
								  NULL, 0, tags_data->timestamp);
}

/* IRCv3 Batch helper functions */

void
batch_message_free (batch_message *msg)
{
	if (!msg)
		return;

	g_free (msg->prefix);
	g_free (msg->command);
	g_strfreev (msg->params);
	if (msg->tags)
		g_hash_table_destroy (msg->tags);
	g_free (msg->msgid);
	g_free (msg);
}

static void
batch_info_free (batch_info *batch)
{
	if (!batch)
		return;

	g_free (batch->id);
	g_free (batch->type);
	g_strfreev (batch->params);
	g_free (batch->outer_batch);
	g_free (batch->label);
	g_free (batch->msgid);
	g_slist_free_full (batch->messages, (GDestroyNotify) batch_message_free);
	g_free (batch);
}

static batch_info *
batch_info_new (const char *id, const char *type, char *word[],
                const message_tags_data *tags_data)
{
	batch_info *batch;
	int i, param_count;

	batch = g_new0 (batch_info, 1);
	batch->id = g_strdup (id);
	batch->type = g_strdup (type);
	batch->started = time (NULL);
	batch->server_time = tags_data->timestamp;  /* server-time from BATCH START */

	/* Copy outer batch reference if this batch is nested */
	if (tags_data->batch_id)
		batch->outer_batch = g_strdup (tags_data->batch_id);

	/* Capture label tag for labeled-response correlation */
	if (tags_data->label)
		batch->label = g_strdup (tags_data->label);

	/* Capture msgid from BATCH START for echo confirmation */
	if (tags_data->msgid)
		batch->msgid = g_strdup (tags_data->msgid);

	/* IRCv3 draft/chathistory-end: server signals no more history available */
	if (tags_data->all_tags &&
	    g_hash_table_contains (tags_data->all_tags, "draft/chathistory-end"))
		batch->chathistory_end = TRUE;

	/* Count and copy parameters (starting from word[5]) */
	param_count = 0;
	for (i = 5; word[i] && *word[i]; i++)
		param_count++;

	if (param_count > 0)
	{
		batch->params = g_new0 (char *, param_count + 1);
		batch->param_count = param_count;
		for (i = 0; i < param_count; i++)
		{
			char *param = word[5 + i];
			if (*param == ':')
				param++;
			batch->params[i] = g_strdup (param);
		}
	}

	return batch;
}

void
inbound_batch_start (server *serv, const char *batch_id, const char *batch_type,
                     char *word[], const message_tags_data *tags_data)
{
	batch_info *batch;

	if (!serv->have_batch)
		return;

	/* Initialize active_batches hash table if needed */
	if (!serv->active_batches)
	{
		serv->active_batches = g_hash_table_new_full (g_str_hash, g_str_equal,
		                                              g_free,
		                                              (GDestroyNotify) batch_info_free);
	}

	/* Check if batch already exists (shouldn't happen, but be defensive) */
	if (g_hash_table_contains (serv->active_batches, batch_id))
	{
		g_warning ("Duplicate batch ID received: %s", batch_id);
		return;
	}

	/* Create and store the new batch */
	batch = batch_info_new (batch_id, batch_type, word, tags_data);
	g_hash_table_insert (serv->active_batches, g_strdup (batch_id), batch);
	server_ensure_stale_sweep_timer (serv);
}

/* Process a multiline batch: concatenate messages and display as single message */
static void
process_multiline_batch (server *serv, batch_info *batch)
{
	session *sess = NULL;
	GSList *iter;
	GString *combined_text;
	char *nick = NULL;
	char *host = NULL;
	message_tags_data tags_data;
	batch_message *first_msg = NULL;
	const char *target = NULL;
	gboolean first_line = TRUE;

	if (!batch || !batch->messages)
		return;

	/* Get target from batch params (batch->params[0] should be the target) */
	if (batch->param_count >= 1 && batch->params[0])
		target = batch->params[0];

	/* Find the session for this target */
	if (target)
	{
		/* Try as channel first */
		sess = find_channel (serv, (char *)target);
		if (!sess)
		{
			/* Try as dialog */
			sess = find_dialog (serv, (char *)target);
		}
	}

	if (!sess)
	{
		/* Use server session as fallback */
		sess = serv->server_session;
	}

	/* Build combined text from all messages */
	combined_text = g_string_new (NULL);

	for (iter = batch->messages; iter; iter = iter->next)
	{
		batch_message *msg = iter->data;

		if (!msg || !msg->command)
			continue;

		/* Only process PRIVMSG for multiline */
		if (g_ascii_strcasecmp (msg->command, "PRIVMSG") != 0)
			continue;

		/* Save first message for sender info and tags */
		if (!first_msg)
		{
			first_msg = msg;

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
		}

		/* Get the message text (params[1]) */
		if (msg->param_count >= 2 && msg->params[1])
		{
			char *text = msg->params[1];
			if (text[0] == ':')
				text++;

			/* Add newline separator between lines */
			if (!first_line)
				g_string_append_c (combined_text, '\n');
			first_line = FALSE;

			g_string_append (combined_text, text);
		}
	}

	/* Display combined message if we have content */
	if (combined_text->len > 0 && nick)
	{
		/* Initialize tags data from first message */
		memset (&tags_data, 0, sizeof (tags_data));
		if (first_msg)
		{
			tags_data.timestamp = first_msg->timestamp;
			/* Prefer batch-level msgid (from BATCH START) over per-message msgid */
			tags_data.msgid = batch->msgid ? batch->msgid : first_msg->msgid;
			if (first_msg->tags)
				tags_data.all_tags = first_msg->tags;
		}

		/* Group all entries from this multiline batch for unified hover highlight */
		fe_begin_multiline_group (sess);

		/* Check if it's a channel or private message */
		if (sess->type == SESS_CHANNEL)
		{
			inbound_chanmsg (serv, sess, sess->channel, nick, combined_text->str,
			                 FALSE, 0, &tags_data);
		}
		else
		{
			inbound_privmsg (serv, nick, host ? host : "", combined_text->str,
			                 0, &tags_data);
		}

		fe_end_multiline_group (sess);
	}

	g_string_free (combined_text, TRUE);
	g_free (nick);
	g_free (host);
}

/* Collapse a multiline batch into a single batch_message and add to parent batch.
 * Used when a draft/multiline batch is nested inside a chathistory batch so that
 * the assembled message gets processed in chronological order with other messages. */
static void
collapse_multiline_to_parent (batch_info *batch, batch_info *parent)
{
	GSList *iter;
	GString *combined_text;
	batch_message *first_msg = NULL;
	batch_message *result;
	gboolean first_line = TRUE;
	const char *target = NULL;

	if (!batch || !batch->messages)
		return;

	/* Get target from batch params */
	if (batch->param_count >= 1 && batch->params[0])
		target = batch->params[0];

	/* Build combined text from all PRIVMSG lines */
	combined_text = g_string_new (NULL);

	for (iter = batch->messages; iter; iter = iter->next)
	{
		batch_message *msg = iter->data;

		if (!msg || !msg->command)
			continue;

		if (g_ascii_strcasecmp (msg->command, "PRIVMSG") != 0)
			continue;

		if (!first_msg)
			first_msg = msg;

		/* Get message text (params[1]) */
		if (msg->param_count >= 2 && msg->params[1])
		{
			char *text = msg->params[1];
			if (text[0] == ':')
				text++;

			if (!first_line)
				g_string_append_c (combined_text, '\n');
			first_line = FALSE;

			g_string_append (combined_text, text);
		}
	}

	if (combined_text->len == 0 || !first_msg)
	{
		g_string_free (combined_text, TRUE);
		return;
	}

	/* Create a batch_message with the combined text.
	 * Per IRCv3 multiline spec, msgid and server-time belong on the
	 * BATCH START line, not on individual PRIVMSGs inside the batch. */
	result = g_new0 (batch_message, 1);
	result->prefix = g_strdup (first_msg->prefix);
	result->command = g_strdup ("PRIVMSG");
	result->timestamp = batch->server_time ? batch->server_time : first_msg->timestamp;
	result->msgid = g_strdup (batch->msgid ? batch->msgid : first_msg->msgid);

	/* Copy tags from first message */
	if (first_msg->tags)
	{
		GHashTableIter tag_iter;
		gpointer key, value;

		result->tags = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
		g_hash_table_iter_init (&tag_iter, first_msg->tags);
		while (g_hash_table_iter_next (&tag_iter, &key, &value))
		{
			g_hash_table_insert (result->tags, g_strdup (key), g_strdup (value));
		}
	}

	/* Set params: [target, :combined_text] */
	result->params = g_new0 (char *, 3);
	result->param_count = 2;
	result->params[0] = g_strdup (target ? target : "");
	result->params[1] = g_strdup_printf (":%s", combined_text->str);

	/* Add to parent batch's message list */
	parent->messages = g_slist_append (parent->messages, result);

	g_string_free (combined_text, TRUE);
}

void
inbound_batch_end (server *serv, const char *batch_id,
                   const message_tags_data *tags_data)
{
	batch_info *batch;

	if (!serv->have_batch || !serv->active_batches)
		return;

	/* Look up the batch */
	batch = g_hash_table_lookup (serv->active_batches, batch_id);
	if (!batch)
	{
		g_warning ("End of unknown batch: %s", batch_id);
		return;
	}

	/* Process the batch based on its type */
	if (batch->type)
	{
		if (g_ascii_strcasecmp (batch->type, "chathistory") == 0)
		{
			/* Process chat history batch */
			chathistory_process_batch (serv, batch);
		}
		else if (g_ascii_strcasecmp (batch->type, "draft/chathistory-targets") == 0)
		{
			/* Process CHATHISTORY TARGETS response for missed DMs */
			chathistory_process_targets_batch (serv, batch);
		}
		else if (g_ascii_strcasecmp (batch->type, "draft/multiline") == 0 ||
		         g_ascii_strcasecmp (batch->type, "multiline") == 0)
		{
			/* Check if this multiline batch is nested inside a parent batch */
			batch_info *parent = NULL;
			if (batch->outer_batch)
				parent = g_hash_table_lookup (serv->active_batches, batch->outer_batch);

			if (parent)
			{
				/* Nested inside parent batch (e.g., chathistory) - collapse into
				 * a single message and add to parent's list for chronological ordering */
				collapse_multiline_to_parent (batch, parent);
			}
			else
			{
				/* Standalone multiline batch — check if this is a Tier 2 echo
				 * of our own send (already displayed locally as pending). */
				gboolean echo_suppressed = FALSE;
				if (batch->label && serv->pending_labels)
				{
					pending_label_info *info = g_hash_table_lookup (serv->pending_labels,
					                                                batch->label);
					if (info && info->entry_id)
						echo_suppressed = TRUE;  /* confirmed in label cleanup below */
				}
				if (!echo_suppressed)
					process_multiline_batch (serv, batch);
			}
		}
		/* TODO: Handle other batch types:
		 * - "netjoin"/"netsplit": Collapse join/quit messages
		 */
	}

	/* Clean up pending label for any labeled batch response.
	 * The label tag appears on BATCH START (captured in batch->label),
	 * not on individual messages inside the batch. */
	if (batch->label && serv->pending_labels)
	{
		pending_label_info *info = g_hash_table_lookup (serv->pending_labels,
		                                                batch->label);
		if (info && info->entry_id && info->sess && is_session (info->sess))
		{
			/* Use msgid from BATCH START (where the server assigns it),
			 * falling back to the first message's msgid if not present. */
			const char *batch_msgid = batch->msgid;
			if (!batch_msgid && batch->messages)
			{
				batch_message *first = batch->messages->data;
				if (first)
					batch_msgid = first->msgid;
			}
			fe_confirm_entry (info->sess, info->entry_id, batch_msgid);
			if (batch_msgid && batch->label)
				scrollback_confirm_pending (info->sess, batch->label, batch_msgid);
		}

		g_hash_table_remove (serv->pending_labels, batch->label);
	}

	/* Remove the batch from active batches (this also frees it) */
	g_hash_table_remove (serv->active_batches, batch_id);
}

/* Check if a message belongs to an active batch and should be deferred */
gboolean
inbound_batch_is_active (server *serv, const message_tags_data *tags_data)
{
	if (!serv->have_batch || !tags_data->batch_id || !serv->active_batches)
		return FALSE;

	return g_hash_table_contains (serv->active_batches, tags_data->batch_id);
}

/* Add a message to an active batch
 * Returns TRUE if message was added to batch (and should not be processed immediately)
 * Returns FALSE if message is not part of a batch (process normally)
 */
gboolean
inbound_batch_add_message (server *serv, const char *prefix, const char *command,
                           char *word[], char *word_eol[], int word_count,
                           const message_tags_data *tags_data)
{
	batch_info *batch;
	batch_message *msg;
	int i, param_count;

	if (!serv->have_batch || !tags_data->batch_id || !serv->active_batches)
		return FALSE;

	batch = g_hash_table_lookup (serv->active_batches, tags_data->batch_id);
	if (!batch)
		return FALSE;

	/* Only collect messages for batch types that need deferred processing.
	 * Unknown types (including labeled-response) should let messages pass
	 * through for immediate processing — otherwise they'd be silently dropped. */
	if (!batch->type ||
	    (g_ascii_strcasecmp (batch->type, "chathistory") != 0 &&
	     g_ascii_strcasecmp (batch->type, "draft/chathistory-targets") != 0 &&
	     g_ascii_strcasecmp (batch->type, "draft/multiline") != 0 &&
	     g_ascii_strcasecmp (batch->type, "multiline") != 0))
		return FALSE;

	/* Create batch_message struct */
	msg = g_new0 (batch_message, 1);
	msg->prefix = g_strdup (prefix);
	msg->command = g_strdup (command);
	msg->timestamp = tags_data->timestamp;
	if (tags_data->msgid)
		msg->msgid = g_strdup (tags_data->msgid);

	/* Copy all_tags if available */
	if (tags_data->all_tags)
	{
		GHashTableIter iter;
		gpointer key, value;

		msg->tags = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
		g_hash_table_iter_init (&iter, tags_data->all_tags);
		while (g_hash_table_iter_next (&iter, &key, &value))
		{
			g_hash_table_insert (msg->tags, g_strdup (key), g_strdup (value));
		}
	}

	/* Count parameters (start from word[3] which is typically the first param).
	 * In IRC, a parameter starting with ':' is a trailing parameter that consumes
	 * the rest of the line. Stop counting when we hit one.
	 */
	param_count = 0;
	for (i = 3; i < word_count && word[i] && *word[i]; i++)
	{
		param_count++;
		if (word[i][0] == ':')
			break; /* Trailing parameter - everything after is part of this param */
	}

	if (param_count > 0)
	{
		msg->params = g_new0 (char *, param_count + 1);
		msg->param_count = param_count;
		for (i = 0; i < param_count; i++)
		{
			/* If this word starts with ':', it's a trailing parameter.
			 * Use word_eol to get the full text including spaces.
			 * Example: PRIVMSG #chan :hello world
			 *   word[4] = ":hello", word_eol[4] = ":hello world"
			 */
			if (word[3 + i][0] == ':')
			{
				msg->params[i] = g_strdup (word_eol[3 + i]);
				break; /* No more params after trailing */
			}
			else
			{
				msg->params[i] = g_strdup (word[3 + i]);
			}
		}
	}

	/* Add to batch's message list (appending maintains chronological order) */
	batch->messages = g_slist_append (batch->messages, msg);

	return TRUE;
}

/* IRCv3 +typing indicator helpers */

#define TYPING_EXPIRE_US       (6 * G_USEC_PER_SEC)
#define TYPING_SWEEP_INTERVAL  2  /* seconds */

static int typing_sweep_cb (void *userdata);

static void
typing_indicator_update (session *sess, const char *nick)
{
	GSList *list;
	typing_entry *entry;

	if (!prefs.hex_irc_typing_show)
		return;

	/* Search for existing entry (case-insensitive) */
	for (list = sess->typing_nicks; list; list = list->next)
	{
		entry = list->data;
		if (!sess->server->p_cmp (entry->nick, nick))
		{
			entry->last_seen = g_get_monotonic_time ();
			fe_typing_update (sess);
			return;
		}
	}

	/* New typer — add to list */
	entry = g_new0 (typing_entry, 1);
	safe_strcpy (entry->nick, nick, sizeof (entry->nick));
	entry->last_seen = g_get_monotonic_time ();
	sess->typing_nicks = g_slist_prepend (sess->typing_nicks, entry);

	/* Start sweep timer if not running */
	if (!sess->typing_sweep_timer)
		sess->typing_sweep_timer = fe_timeout_add_seconds (TYPING_SWEEP_INTERVAL,
		                                                    typing_sweep_cb, sess);

	fe_typing_update (sess);
}

static void
typing_indicator_remove (session *sess, const char *nick)
{
	GSList *list;
	typing_entry *entry;

	for (list = sess->typing_nicks; list; list = list->next)
	{
		entry = list->data;
		if (!sess->server->p_cmp (entry->nick, nick))
		{
			sess->typing_nicks = g_slist_remove (sess->typing_nicks, entry);
			g_free (entry);

			if (!sess->typing_nicks && sess->typing_sweep_timer)
			{
				fe_timeout_remove (sess->typing_sweep_timer);
				sess->typing_sweep_timer = 0;
			}

			fe_typing_update (sess);
			return;
		}
	}
}

static int
typing_sweep_cb (void *userdata)
{
	session *sess = userdata;
	gint64 now = g_get_monotonic_time ();
	GSList *list, *next;
	typing_entry *entry;
	int removed = 0;

	for (list = sess->typing_nicks; list; list = next)
	{
		next = list->next;
		entry = list->data;
		if (now - entry->last_seen > TYPING_EXPIRE_US)
		{
			sess->typing_nicks = g_slist_remove (sess->typing_nicks, entry);
			g_free (entry);
			removed++;
		}
	}

	if (removed)
		fe_typing_update (sess);

	if (!sess->typing_nicks)
	{
		sess->typing_sweep_timer = 0;
		return 0;
	}

	return 1;
}

/* IRCv3 TAGMSG - tag-only messages
 * Used for typing indicators (+typing), reactions (+react), replies (+reply), etc.
 * See https://ircv3.net/specs/extensions/message-tags#the-tagmsg-tag
 */
void
inbound_tagmsg (server *serv, char *to, char *nick, char *ip,
                const message_tags_data *tags_data)
{
	session *sess;
	const char *typing_value;
	const char *react_value;
	const char *unreact_value;
	const char *reply_msgid;
	gboolean is_self;

	if (!serv->have_message_tags)
		return;

	is_self = !serv->p_cmp (nick, serv->nick);

	/* Find the appropriate session — don't fall back to server console */
	if (is_channel (serv, to))
	{
		sess = find_channel (serv, to);
		if (!sess)
			return;
	}
	else
	{
		sess = find_dialog (serv, nick);
		if (!sess)
			return;
	}

	if (!tags_data->all_tags)
		return;

	/* Handle +typing tag for typing indicators
	 * Values: "active" (typing), "paused" (stopped briefly), "done" (finished/sent)
	 *
	 * Self-attributed TAGMSGs come from two sources:
	 *   1. THIS client's own echo (echo-message / bouncer round-trip).
	 *      Always suppressed — the input box already tells you that you're
	 *      typing.  Detected by the short echo window set when we sent.
	 *   2. ANOTHER client of ours (same nick) — useful signal that you're
	 *      composing on phone/another box.  Gated by hex_irc_typing_self.
	 * Only this filtering applies to typing, not reactions.
	 */
	typing_value = g_hash_table_lookup (tags_data->all_tags, "+typing");
	if (typing_value)
	{
		if (is_self)
		{
			gint64 now = g_get_monotonic_time ();
			if (sess->typing_self_echo_until != 0 && now < sess->typing_self_echo_until)
				goto skip_typing;  /* our own echo */
			if (!prefs.hex_irc_typing_self)
				goto skip_typing;  /* user opted out of other-client signal */
		}

		if (!strcmp (typing_value, "active") || !strcmp (typing_value, "paused"))
		{
			typing_indicator_update (sess, nick);
		}
		else if (!strcmp (typing_value, "done"))
		{
			typing_indicator_remove (sess, nick);
		}
	}

skip_typing:
	/* Handle +draft/react and +draft/unreact tags for reactions
	 * Reactions are sent as TAGMSG with both +draft/react=<text>
	 * and +draft/reply=<target_msgid> to identify the target message.
	 * See https://ircv3.net/specs/client-tags/react
	 */
	react_value = g_hash_table_lookup (tags_data->all_tags, "+draft/react");
	unreact_value = g_hash_table_lookup (tags_data->all_tags, "+draft/unreact");
	reply_msgid = g_hash_table_lookup (tags_data->all_tags, "+draft/reply");

	if (react_value && reply_msgid)
	{
		if (prefs.hex_irc_react_show)
			fe_reaction_received (sess, reply_msgid, react_value, nick, is_self);
		scrollback_save_reaction_for_session (sess, reply_msgid, react_value,
		                                      nick, is_self);
	}
	else if (unreact_value && reply_msgid)
	{
		if (prefs.hex_irc_react_show)
			fe_reaction_removed (sess, reply_msgid, unreact_value, nick);
		scrollback_remove_reaction_for_session (sess, reply_msgid,
		                                        unreact_value, nick);
	}
}

void
inbound_identified (server *serv)	/* 'MODE +e MYSELF' on freenode */
{
	if (serv->joindelay_tag)
	{
		/* stop waiting, just auto JOIN now */
		fe_timeout_remove (serv->joindelay_tag);
		serv->joindelay_tag = 0;
		check_autojoin_channels (serv);
	}
}

void
inbound_redact_fail (server *serv, session *sess, const char *code,
                     const char *text, const message_tags_data *tags_data)
{
	const char *msg;

	if (g_strcmp0 (code, "REDACT_FORBIDDEN") == 0)
		msg = _("You don't have permission to delete that message");
	else if (g_strcmp0 (code, "REDACT_WINDOW_EXPIRED") == 0)
		msg = _("Message is too old to delete");
	else if (g_strcmp0 (code, "UNKNOWN_MSGID") == 0)
		msg = _("Message not found on server");
	else if (g_strcmp0 (code, "INVALID_TARGET") == 0)
		msg = _("Invalid target for message deletion");
	else
		msg = text;

	EMIT_SIGNAL_TIMESTAMP (XP_TE_FAILCMD, sess, "REDACT", code, text, NULL,
	                        NULL, tags_data->timestamp);
	fe_toast_show (sess, msg, 5000, 5 /* TOAST_TYPE_ERROR */, 0);
}

static const char *sasl_mechanisms[] =
{
	"PLAIN",
	"EXTERNAL",
	"SCRAM-SHA-1",
	"SCRAM-SHA-256",
	"SCRAM-SHA-512",
	"OAUTHBEARER"
};

static void
inbound_toggle_caps (server *serv, const char *extensions_str, gboolean enable)
{
	char **extensions;
	gsize i;

	extensions = g_strsplit (extensions_str, " ", 0);

	for (i = 0; extensions[i]; i++)
	{
		const char *extension = extensions[i];

		if (!strcmp (extension, "solanum.chat/identify-msg"))
			serv->have_idmsg = enable;
		else if (!strcmp (extension, "multi-prefix"))
			serv->have_namesx = enable;
		else if (!strcmp (extension, "account-notify"))
			serv->have_accnotify = enable;
		else if (!strcmp (extension, "extended-join"))
			serv->have_extjoin = enable;
		else if (!strcmp (extension, "userhost-in-names"))
			serv->have_uhnames = enable;
		else if (!strcmp (extension, "server-time")
				|| !strcmp (extension, "znc.in/server-time")
				|| !strcmp (extension, "znc.in/server-time-iso"))
			serv->have_server_time = enable;
		else if (!strcmp (extension, "away-notify"))
			serv->have_awaynotify = enable;
		else if (!strcmp (extension, "account-tag"))
			serv->have_account_tag = enable;
		else if (!strcmp (extension, "batch"))
			serv->have_batch = enable;
		else if (!strcmp (extension, "message-tags"))
			serv->have_message_tags = enable;
		else if (!strcmp (extension, "echo-message"))
			serv->have_echo_message = enable;
		else if (!strcmp (extension, "labeled-response"))
			serv->have_labeled_response = enable;
		else if (!strcmp (extension, "draft/chathistory"))
			serv->have_chathistory = enable;
		else if (!strcmp (extension, "draft/multiline"))
			serv->have_multiline = enable;
		else if (!strcmp (extension, "draft/event-playback"))
			serv->have_event_playback = enable;
		else if (!strcmp (extension, "draft/read-marker"))
			serv->have_read_marker = enable;
		else if (!strcmp (extension, "draft/message-redaction"))
			serv->have_redact = enable;
		else if (!strcmp (extension, "draft/account-registration"))
			serv->have_account_registration = enable;
		else if (!strcmp (extension, "draft/metadata-2"))
			serv->have_metadata = enable;
		else if (!strcmp (extension, "draft/channel-rename"))
			serv->have_channel_rename = enable;
		else if (!strcmp (extension, "draft/pre-away"))
			serv->have_pre_away = enable;
		else if (!strcmp (extension, "draft/extended-isupport"))
		{
			serv->have_extended_isupport = enable;
			if (enable)
				tcp_send_len (serv, "ISUPPORT\r\n", 10);
		}
		else if (!strcmp (extension, "sasl"))
		{
			serv->have_sasl = enable;
			if (enable)
			{
#ifdef USE_OPENSSL
				if (serv->loginmethod == LOGIN_SASLEXTERNAL)
					serv->sasl_mech = MECH_EXTERNAL;
				else if (serv->loginmethod == LOGIN_SASL_SCRAM_SHA_1)
					serv->sasl_mech = MECH_SCRAM_SHA_1;
				else if (serv->loginmethod == LOGIN_SASL_SCRAM_SHA_256)
					serv->sasl_mech = MECH_SCRAM_SHA_256;
				else if (serv->loginmethod == LOGIN_SASL_SCRAM_SHA_512)
					serv->sasl_mech = MECH_SCRAM_SHA_512;
#endif
#ifdef USE_LIBWEBSOCKETS
				if (serv->loginmethod == LOGIN_SASL_OAUTHBEARER)
					serv->sasl_mech = MECH_OAUTHBEARER;
#endif
				/* Mechanism either defaulted to PLAIN or server gave us list */
				tcp_sendf (serv, "AUTHENTICATE %s\r\n", sasl_mechanisms[serv->sasl_mech]);
			}
		}
	}

	g_strfreev (extensions);
}

void
inbound_cap_ack (server *serv, char *nick, char *extensions,
					  const message_tags_data *tags_data)
{
	EMIT_SIGNAL_TIMESTAMP (XP_TE_CAPACK, serv->server_session, nick, extensions,
								  NULL, NULL, 0, tags_data->timestamp);

	inbound_toggle_caps (serv, extensions, TRUE);
}

void
inbound_cap_del (server *serv, char *nick, char *extensions,
					 const message_tags_data *tags_data)
{
	EMIT_SIGNAL_TIMESTAMP (XP_TE_CAPDEL, serv->server_session, nick, extensions,
								  NULL, NULL, 0, tags_data->timestamp);

	inbound_toggle_caps (serv, extensions, FALSE);
}

static const char * const supported_caps[] = {
	/* IRCv3.1 */
	"multi-prefix",
	"away-notify",
	"account-notify",
	"extended-join",
	/* "sasl", Handled manually */

	/* IRCv3.2 */
	"server-time",
	"userhost-in-names",
	"cap-notify",
	"chghost",
	"setname",
	"invite-notify",
	"account-tag",
	"extended-monitor",

	/* IRCv3 message-tags and batch */
	"batch",
	"message-tags",
	"echo-message",
	"labeled-response",

	/* IRCv3 chathistory, multiline, and related */
	"draft/chathistory",
	"draft/multiline",
	"draft/event-playback",
	"draft/read-marker",
	"draft/message-redaction",
	"draft/account-registration",
	"draft/metadata-2",
	"draft/channel-rename",
	"draft/pre-away",

	/* ZNC */
	"znc.in/server-time-iso",
	"znc.in/server-time",

	/* Twitch */
	"twitch.tv/membership",

	/* Solanum */
	"solanum.chat/identify-msg",

	/* Extended ISUPPORT (receive 005 tokens before registration) */
	"draft/extended-isupport",
};

static int
get_supported_mech (server *serv, const char *list)
{
	char **mechs = g_strsplit (list, ",", 0);
	gsize i;
	int ret = -1;

	for (i = 0; mechs[i]; ++i)
	{
#ifdef USE_OPENSSL
		if (serv->loginmethod == LOGIN_SASLEXTERNAL)
		{
			if (!strcmp (mechs[i], "EXTERNAL"))
			{
				ret = MECH_EXTERNAL;
				break;
			}
		}
		else if (serv->loginmethod == LOGIN_SASL_SCRAM_SHA_1)
		{
			if (!strcmp(mechs[i], "SCRAM-SHA-1"))
			{
				ret = MECH_SCRAM_SHA_1;
				break;
			}
		}
		else if (serv->loginmethod == LOGIN_SASL_SCRAM_SHA_256)
		{
			if (!strcmp(mechs[i], "SCRAM-SHA-256"))
			{
				ret = MECH_SCRAM_SHA_256;
				break;
			}
		}
		else if (serv->loginmethod == LOGIN_SASL_SCRAM_SHA_512)
		{
			if (!strcmp(mechs[i], "SCRAM-SHA-512"))
			{
				ret = MECH_SCRAM_SHA_512;
				break;
			}
		}
		else
#endif
		if (serv->loginmethod == LOGIN_SASL_OAUTHBEARER)
		{
			if (!strcmp (mechs[i], "OAUTHBEARER"))
			{
				ret = MECH_OAUTHBEARER;
				break;
			}
		}
		else if (!strcmp (mechs[i], "PLAIN"))
		{
			ret = MECH_PLAIN;
			break;
		}
	}

	g_strfreev (mechs);
	return ret;
}

void
inbound_cap_ls (server *serv, char *nick, char *extensions_str,
					 const message_tags_data *tags_data)
{
	char buffer[500];	/* buffer for requesting capabilities and emitting the signal */
	gboolean want_cap = FALSE; /* format the CAP REQ string based on previous capabilities being requested or not */
	char **extensions;
	int i;

	if (g_str_has_prefix (extensions_str, "* "))
	{
		serv->waiting_on_cap = TRUE;
		extensions_str += 2;
		extensions_str += extensions_str[0] == ':' ? 1 : 0;
	}
	else
	{
		serv->waiting_on_cap = FALSE;
	}

	EMIT_SIGNAL_TIMESTAMP (XP_TE_CAPLIST, serv->server_session, nick,
								  extensions_str, NULL, NULL, 0, tags_data->timestamp);

	extensions = g_strsplit (extensions_str, " ", 0);

	strcpy (buffer, "CAP REQ :");

	for (i=0; extensions[i]; i++)
	{
		char *extension = extensions[i];
		char *value;
		gsize x;

		/* CAP 3.2 can provide values */
		if ((value = strchr (extension, '=')))
		{
			*value = '\0';
			value++;
		}

		/* if the SASL password is set AND auth mode is set to SASL, request SASL auth */
		if (!g_strcmp0 (extension, "sasl") &&
			(((serv->loginmethod == LOGIN_SASL
				|| serv->loginmethod == LOGIN_SASL_SCRAM_SHA_1
				|| serv->loginmethod == LOGIN_SASL_SCRAM_SHA_256
				|| serv->loginmethod == LOGIN_SASL_SCRAM_SHA_512)
					&& strlen (serv->password) != 0)
				|| serv->loginmethod == LOGIN_SASLEXTERNAL
				|| serv->loginmethod == LOGIN_SASL_OAUTHBEARER))
		{
			if (value)
			{
				int sasl_mech = get_supported_mech (serv, value);
				if (sasl_mech == -1) /* No supported mech */
					continue;
				serv->sasl_mech = sasl_mech;
			}
			want_cap = TRUE;
			serv->waiting_on_sasl = TRUE;
			g_strlcat (buffer, "sasl ", sizeof(buffer));
			continue;
		}

		/* IRCv3 STS (Strict Transport Security) - parse but don't request
		 * Format: sts=port=6697,duration=2592000
		 */
		if (!g_strcmp0 (extension, "sts") && value)
		{
			int sts_port = 0;
			int sts_duration = 0;
			char **tokens = g_strsplit (value, ",", 0);
			int j;

			for (j = 0; tokens[j]; j++)
			{
				if (g_str_has_prefix (tokens[j], "port="))
					sts_port = atoi (tokens[j] + 5);
				else if (g_str_has_prefix (tokens[j], "duration="))
					sts_duration = atoi (tokens[j] + 9);
			}
			g_strfreev (tokens);

			if (sts_port > 0)
			{
				/* Store the STS policy */
				sts_policy_add (serv->hostname, sts_port, sts_duration);

				/* If we're on a non-TLS connection, we need to upgrade */
				if (!serv->ssl)
				{
					serv->sts_upgrade_port = sts_port;
					EMIT_SIGNAL_TIMESTAMP (XP_TE_SERVTEXT, serv->server_session,
						"STS policy received - upgrading to TLS", NULL, NULL, NULL,
						0, tags_data->timestamp);
				}
			}
			continue;
		}

		/* IRCv3 draft/account-registration - parse capability tokens
		 * Format: draft/account-registration=before-connect,email-required,custom-account-name
		 */
		if (!g_strcmp0 (extension, "draft/account-registration") && value)
		{
			char **tokens = g_strsplit (value, ",", 0);
			int j;

			for (j = 0; tokens[j]; j++)
			{
				if (!strcmp (tokens[j], "before-connect"))
					serv->accreg_before_connect = TRUE;
				else if (!strcmp (tokens[j], "email-required"))
					serv->accreg_email_required = TRUE;
				else if (!strcmp (tokens[j], "custom-account-name"))
					serv->accreg_custom_account = TRUE;
			}
			g_strfreev (tokens);
			/* Don't continue - still need to request the capability */
		}

		/* IRCv3 draft/multiline - parse capability tokens
		 * Format: draft/multiline=max-bytes=16384,max-lines=100
		 */
		if (!g_strcmp0 (extension, "draft/multiline") && value)
		{
			char **tokens = g_strsplit (value, ",", 0);
			int j;

			for (j = 0; tokens[j]; j++)
			{
				if (g_str_has_prefix (tokens[j], "max-bytes="))
					serv->multiline_max_bytes = atoi (tokens[j] + 10);
				else if (g_str_has_prefix (tokens[j], "max-lines="))
					serv->multiline_max_lines = atoi (tokens[j] + 10);
			}
			g_strfreev (tokens);
			/* Don't continue - still need to request the capability */
		}

		for (x = 0; x < G_N_ELEMENTS(supported_caps); ++x)
		{
			if (!g_strcmp0 (extension, supported_caps[x]))
			{
				g_strlcat (buffer, extension, sizeof(buffer));
				g_strlcat (buffer, " ", sizeof(buffer));
				want_cap = TRUE;
			}
		}
	}

	g_strfreev (extensions);

	if (want_cap)
	{
		/* buffer + 9 = emit buffer without "CAP REQ :" */
		EMIT_SIGNAL_TIMESTAMP (XP_TE_CAPREQ, serv->server_session,
									  buffer + 9, NULL, NULL, NULL, 0,
									  tags_data->timestamp);
		tcp_sendf (serv, "%s\r\n", g_strchomp (buffer));
	}
	if (!serv->waiting_on_sasl && !serv->waiting_on_cap)
	{
		/* Check if STS upgrade is needed before sending CAP END */
		if (serv->sts_upgrade_port > 0)
		{
			/* Need to reconnect with TLS - close this connection */
			int sts_port = serv->sts_upgrade_port;
			serv->sts_upgrade_port = 0;
			serv->sent_capend = TRUE; /* Prevent further processing */
			EMIT_SIGNAL_TIMESTAMP (XP_TE_SERVTEXT, serv->server_session,
				"Reconnecting with TLS due to STS policy...", NULL, NULL, NULL,
				0, tags_data->timestamp);
			/* Trigger reconnect with TLS */
			serv->use_ssl = TRUE;
			serv->accept_invalid_cert = FALSE;
			serv->port = sts_port;
			serv->disconnect (serv->server_session, FALSE, -1);
			serv->connect (serv, serv->hostname, sts_port, FALSE);
			return;
		}

		/* if we use SASL, CAP END is dealt via raw numerics */
		serv->sent_capend = TRUE;
		tcp_send_len (serv, "CAP END\r\n", 9);
	}
}

void
inbound_cap_nak (server *serv, char *extensions_str, const message_tags_data *tags_data)
{
	char **extensions;
	int i;

	extensions = g_strsplit (extensions_str, " ", 0);
	for (i=0; extensions[i]; i++)
	{
		if (!g_strcmp0 (extensions[i], "sasl"))
			serv->waiting_on_sasl = FALSE;
	}

	if (!serv->waiting_on_cap && !serv->waiting_on_sasl && !serv->sent_capend)
	{
		/* Check if STS upgrade is needed */
		if (serv->sts_upgrade_port > 0)
		{
			int sts_port = serv->sts_upgrade_port;
			serv->sts_upgrade_port = 0;
			serv->sent_capend = TRUE;
			serv->use_ssl = TRUE;
			serv->accept_invalid_cert = FALSE;
			serv->port = sts_port;
			serv->disconnect (serv->server_session, FALSE, -1);
			serv->connect (serv, serv->hostname, sts_port, FALSE);
			g_strfreev (extensions);
			return;
		}

		serv->sent_capend = TRUE;
		tcp_send_len (serv, "CAP END\r\n", 9);
	}

	g_strfreev (extensions);
}

void
inbound_cap_list (server *serv, char *nick, char *extensions,
						const message_tags_data *tags_data)
{
	if (g_str_has_prefix (extensions, "* "))
	{
		extensions += 2;
		extensions += extensions[0] == ':' ? 1 : 0;
	}
	EMIT_SIGNAL_TIMESTAMP (XP_TE_CAPACK, serv->server_session, nick, extensions,
								  NULL, NULL, 0, tags_data->timestamp);
}

/*
 * Send a base64-encoded SASL payload with 400-byte chunking.
 * Builds all AUTHENTICATE lines into a single buffer and sends at once
 * to avoid the outbound throttle delaying login handshake.
 */
static void
sasl_send_authenticate (server *serv, const char *encoded)
{
	GString *buf;
	size_t len, sent;

	len = strlen (encoded);
	/* Pre-allocate: each 400 bytes needs "AUTHENTICATE " (13) + chunk + "\r\n" (2) */
	buf = g_string_sized_new (len + (len / 400 + 1) * 15 + 20);

	sent = 0;
	while (sent < len)
	{
		size_t chunk_size = (len - sent > 400) ? 400 : (len - sent);
		g_string_append (buf, "AUTHENTICATE ");
		g_string_append_len (buf, encoded + sent, chunk_size);
		g_string_append (buf, "\r\n");
		sent += chunk_size;
	}

	/* If final chunk was exactly 400 bytes, send empty to signal end */
	if (len % 400 == 0)
		g_string_append (buf, "AUTHENTICATE +\r\n");

	tcp_send_len (serv, buf->str, (int) buf->len);
	g_string_free (buf, TRUE);
}

static void
plain_authenticate (server *serv, char *user, char *password)
{
	char *pass = encode_sasl_pass_plain (user, password);

	if (pass == NULL)
	{
		/* something went wrong abort */
		tcp_sendf (serv, "AUTHENTICATE *\r\n");
		return;
	}

	sasl_send_authenticate (serv, pass);
}

#ifdef USE_OPENSSL
/*
 * Sends AUTHENTICATE messages to log in via SCRAM.
 */
static void
scram_authenticate (server *serv, const char *data, const char *digest,
					const char *user, const char *password)
{
	char *encoded, *decoded, *output;
	scram_status status;
	size_t output_len;
	gsize decoded_len;

	if (serv->scram_session == NULL)
	{
		serv->scram_session = scram_session_create (digest, user, password);

		if (serv->scram_session == NULL)
		{
			PrintTextf (serv->server_session, _("Could not create SCRAM session with digest %s"), digest);
			g_warning ("Could not create SCRAM session with digest %s", digest);
			tcp_sendf (serv, "AUTHENTICATE *\r\n");
			return;
		}
	}

	decoded = g_base64_decode (data, &decoded_len);
	status = scram_process (serv->scram_session, decoded, &output, &output_len);
	g_free (decoded);

	if (status == SCRAM_IN_PROGRESS)
	{
		// Authentication is still in progress
		encoded = g_base64_encode ((guchar *) output, output_len);
		tcp_sendf (serv, "AUTHENTICATE %s\r\n", encoded);
		g_free (encoded);
		g_free (output);
	}
	else if (status == SCRAM_SUCCESS)
	{
		// Authentication succeeded
		tcp_sendf (serv, "AUTHENTICATE +\r\n");
		g_clear_pointer (&serv->scram_session, scram_session_free);
	}
	else if (status == SCRAM_ERROR)
	{
		// Authentication failed
		tcp_sendf (serv, "AUTHENTICATE *\r\n");

		if (serv->scram_session->error != NULL)
		{
			PrintTextf (serv->server_session, _("SASL SCRAM authentication failed: %s"), serv->scram_session->error);
			g_info ("SASL SCRAM authentication failed: %s", serv->scram_session->error);
		}

		g_clear_pointer (&serv->scram_session, scram_session_free);
	}
}
#endif

void
inbound_sasl_authenticate (server *serv, char *data)
{
		ircnet *net = (ircnet*)serv->network;
		char *user;
		const char *mech = sasl_mechanisms[serv->sasl_mech];

		/* Got a list of supported mechanisms from outdated inspircd
		 * just ignore it as it goes against spec */
		if (strchr (data, ',') != NULL)
			return;

		if (net->user && !(net->flags & FLAG_USE_GLOBAL))
			user = net->user;
		else
			user = prefs.hex_irc_user_name;

		switch (serv->sasl_mech)
		{
		case MECH_PLAIN:
			plain_authenticate(serv, user, serv->password);
			break;
#ifdef USE_OPENSSL
		case MECH_EXTERNAL:
			tcp_sendf (serv, "AUTHENTICATE +\r\n");
			break;
		case MECH_SCRAM_SHA_1:
			scram_authenticate(serv, data, "SHA1", user, serv->password);
			return;
		case MECH_SCRAM_SHA_256:
			scram_authenticate(serv, data, "SHA256", user, serv->password);
			return;
		case MECH_SCRAM_SHA_512:
			scram_authenticate(serv, data, "SHA512", user, serv->password);
			return;
#endif
#ifdef USE_LIBWEBSOCKETS
		case MECH_OAUTHBEARER:
			if (!serv->oauth_access_token || serv->oauth_access_token[0] == '\0')
			{
				/* No token available - abort SASL */
				PrintText (serv->server_session, _("OAuth: No access token available. Please authorize first in Network List."));
				tcp_sendf (serv, "AUTHENTICATE *\r\n");
				return;
			}
			/* Check if token is expired */
			if (serv->oauth_token_expires > 0 && time(NULL) >= serv->oauth_token_expires)
			{
				PrintText (serv->server_session, _("OAuth: Access token has expired. Please re-authorize in Network List."));
				tcp_sendf (serv, "AUTHENTICATE *\r\n");
				return;
			}
			{
				char *encoded = oauth_encode_sasl_oauthbearer(serv->oauth_access_token,
				                                              serv->servername,
				                                              serv->port);
				if (!encoded)
				{
					PrintText (serv->server_session, _("OAuth: Failed to encode OAUTHBEARER message."));
					tcp_sendf (serv, "AUTHENTICATE *\r\n");
					return;
				}
				sasl_send_authenticate (serv, encoded);
				g_free(encoded);
			}
			break;
#endif
		}

		EMIT_SIGNAL_TIMESTAMP (XP_TE_SASLAUTH, serv->server_session, user, (char*)mech,
								NULL,	NULL,	0,	0);
}

void
inbound_sasl_error (server *serv)
{
#ifdef USE_OPENSSL
    g_clear_pointer (&serv->scram_session, scram_session_free);
#endif
	/* Just abort, not much we can do */
	tcp_sendf (serv, "AUTHENTICATE *\r\n");
}

#ifdef USE_LIBWEBSOCKETS
/* Callback after reactive OAuth token refresh (triggered by SASL 904). */
static void
oauth_sasl_retry_cb (server *serv, oauth_token *token, const char *error)
{
	if (!is_server (serv))
		return;

	if (error || !token)
	{
		PrintTextf (serv->server_session,
			"OAuth: Token refresh failed: %s\n",
			error ? error : "unknown error");
		inbound_sasl_error (serv);
		serv->waiting_on_sasl = FALSE;
		if (!serv->sent_capend)
		{
			serv->sent_capend = TRUE;
			tcp_send_len (serv, "CAP END\r\n", 9);
		}
		return;
	}

	oauth_update_server_tokens (serv, token);
	PrintText (serv->server_session,
		_("OAuth: Token refreshed, retrying authentication...\n"));

	/* Re-attempt SASL OAUTHBEARER with the new token */
	inbound_sasl_authenticate (serv, "+");
}

/* Attempt to refresh the OAuth token after a SASL 904 failure.
 * Returns TRUE if refresh was initiated (caller should not abort SASL). */
gboolean
inbound_sasl_oauth_refresh (server *serv)
{
	ircnet *net = serv->network;

	if (!net || serv->oauth_refresh_attempted)
		return FALSE;

	serv->oauth_refresh_attempted = TRUE;

	PrintText (serv->server_session,
		_("OAuth: SASL authentication failed, attempting token refresh...\n"));

	return oauth_refresh_token (net, oauth_sasl_retry_cb, serv);
}
#endif
