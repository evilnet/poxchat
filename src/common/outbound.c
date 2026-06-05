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

#define _GNU_SOURCE	/* for memrchr */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>

#define WANTSOCKET
#define WANTARPA
#include "inet.h"

#ifndef WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "poxchat.h"
#include "plugin.h"
#include "ignore.h"
#include "util.h"
#include "fe.h"
#include "cfgfiles.h"			  /* poxchat_fopen_file() */
#include "network.h"				/* net_ip() */
#include "modes.h"
#include "notify.h"
#include "inbound.h"
#include "text.h"
#include "poxchatc.h"
#include "servlist.h"
#include "server.h"
#include "tree.h"
#include "outbound.h"
#include "chanopt.h"
#include "chathistory.h"
#include "proto-irc.h"

#define TBUFSIZE 4096

static void help (session *sess, char *tbuf, char *helpcmd, int quiet);
static int cmd_server (session *sess, char *tbuf, char *word[], char *word_eol[]);
static void handle_say (session *sess, char *text, int check_spch);

/* Tier 2 echo-message: after displaying a message and sending it (which sets
 * serv->last_sent_label via tcp_sendf_labeled_tracked), mark the just-created
 * xtext entry as PENDING and record its entry_id in the pending_label_info
 * so we can confirm it when the labeled echo arrives.
 * Must be called in the same synchronous call stack as the send+display. */
static void
mark_pending_echo (session *sess, server *serv)
{
	pending_label_info *info;
	guint64 eid;

	if (!serv->last_sent_label || !serv->pending_labels)
		return;

	info = g_hash_table_lookup (serv->pending_labels, serv->last_sent_label);
	if (!info)
		return;

	eid = fe_get_last_entry_id (sess);
	if (eid == 0)
		return;

	info->entry_id = eid;
	info->sess = sess;

	fe_set_entry_pending (sess, eid);
	serv->last_sent_label = NULL;
}

static void
notj_msg (struct session *sess)
{
	PrintText (sess, _("No channel joined. Try /join #<channel>\n"));
}

void
notc_msg (struct session *sess)
{
	PrintText (sess, _("Not connected. Try /server <host> [<port>]\n"));
}

static char *
random_line (char *file_name)
{
	FILE *fh;
	char buf[512];
	int lines, ran;

	if (!file_name[0])
		goto nofile;

	fh = poxchat_fopen_file (file_name, "r", 0);
	if (!fh)
	{
	 nofile:
		/* reason is not a file, an actual reason! */
		return g_strdup (file_name);
	}

	/* count number of lines in file */
	lines = 0;
	while (fgets (buf, sizeof (buf), fh))
		lines++;

	if (lines < 1)
		goto nofile;

	/* go down a random number */
	rewind (fh);
	ran = RAND_INT (lines);
	do
	{
		HC_IGNORE_RESULT (fgets (buf, sizeof (buf), fh));
		lines--;
	}
	while (lines > ran);
	fclose (fh);
	return g_strdup (buf);
}

void
server_sendpart (server * serv, char *channel, char *reason)
{
	if (!reason)
	{
		reason = random_line (prefs.hex_irc_part_reason);
		serv->p_part (serv, channel, reason);
		g_free (reason);
	} else
	{
		/* reason set by /quit, /close argument */
		serv->p_part (serv, channel, reason);
	}
}

void
server_sendquit (session * sess)
{
	char *rea, *colrea;

	if (!sess->quitreason)
	{
		colrea = g_strdup (prefs.hex_irc_quit_reason);
		check_special_chars (colrea, FALSE);
		rea = random_line (colrea);
		g_free (colrea);
		sess->server->p_quit (sess->server, rea);
		g_free (rea);
	} else
	{
		/* reason set by /quit, /close argument */
		sess->server->p_quit (sess->server, sess->quitreason);
	}
}

void
process_data_init (char *buf, char *cmd, char *word[],
						 char *word_eol[], gboolean handle_quotes,
						 gboolean allow_escape_quotes)
{
	int wordcount = 2;
	int space = FALSE;
	int quote = FALSE;
	int j = 0;
	int len;

	word[0] = "\000\000";
	word_eol[0] = "\000\000";
	word[1] = buf;
	word_eol[1] = cmd;

	while (1)
	{
		switch (*cmd)
		{
		case 0:
			buf[j] = 0;
			for (j = wordcount; j < PDIWORDS; j++)
			{
				word[j] = "\000\000";
				word_eol[j] = "\000\000";
			}
			return;
		case '\042':
			if (!handle_quotes)
				goto def;
			/* two quotes turn into 1 */
			if (allow_escape_quotes && cmd[1] == '\042')
			{
				cmd++;
				goto def;
			}
			if (quote)
			{
				quote = FALSE;
				space = FALSE;
			} else
				quote = TRUE;
			cmd++;
			break;
		case ' ':
			if (!quote)
			{
				if (!space)
				{
					buf[j] = 0;
					j++;

					if (wordcount < PDIWORDS)
					{
						word[wordcount] = &buf[j];
						word_eol[wordcount] = cmd + 1;
						wordcount++;
					}

					space = TRUE;
				}
				cmd++;
				break;
			}
		default:
def:
			space = FALSE;
			len = g_utf8_skip[((unsigned char *)cmd)[0]];
			if (len == 1)
			{
				buf[j] = *cmd;
				j++;
				cmd++;
			} else
			{
				/* skip past a multi-byte utf8 char */
				memcpy (buf + j, cmd, len);
				j += len;
				cmd += len;
			}
		}
	}
}

static int
cmd_addbutton (struct session *sess, char *tbuf, char *word[],
					char *word_eol[])
{
	if (*word[2] && *word_eol[3])
	{
		if (sess->type == SESS_DIALOG)
		{
			list_addentry (&dlgbutton_list, word_eol[3], word[2]);
			fe_dlgbuttons_update (sess);
		} else
		{
			list_addentry (&button_list, word_eol[3], word[2]);
			fe_buttons_update (sess);
		}
		return TRUE;
	}
	return FALSE;
}

/* ADDSERVER <networkname> <serveraddress>, add a new network and server to the network list */
static int
cmd_addserver (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	ircnet *network;

	/* do we have enough arguments given? */
	if (*word[2] && *word_eol[3])
	{
		network = servlist_net_find (word[2], NULL, strcmp);

		/* if the given network doesn't exist yet, add it */
		if (!network)
		{
			network = servlist_net_add (word[2], "", TRUE);
			network->encoding = g_strdup (IRC_DEFAULT_CHARSET);
		}
		/* if we had the network already, check if the given server already exists */
		else if (servlist_server_find (network, word_eol[3], NULL))
		{
			PrintTextf (sess, _("Server %s already exists on network %s.\n"), word_eol[3], word[2]);
			return TRUE;	/* unsuccessful, but the syntax was correct so we don't want to show the help */
		}

		/* server added to new or existing network, doesn't make any difference */
		servlist_server_add (network, word_eol[3]);
		PrintTextf (sess, _("Added server %s to network %s.\n"), word_eol[3], word[2]);
		return TRUE;		/* success */
	}
	else
	{
		return FALSE;		/* print help */
	}
}

static int
cmd_allchannels (session *sess, char *tbuf, char *word[], char *word_eol[])
{
	GSList *list = sess_list;

	if (!*word_eol[2])
		return FALSE;

	while (list)
	{
		sess = list->data;
		if (sess->type == SESS_CHANNEL && sess->channel[0] && sess->server->connected)
		{
			handle_command (sess, word_eol[2], FALSE);
		}
		list = list->next;
	}

	return TRUE;
}

static int
cmd_allchannelslocal (session *sess, char *tbuf, char *word[], char *word_eol[])
{
	GSList *list = sess_list;
	server *serv = sess->server;

	if (!*word_eol[2])
		return FALSE;

	while (list)
	{
		sess = list->data;
		if (sess->type == SESS_CHANNEL && sess->channel[0] &&
			 sess->server->connected && sess->server == serv)
		{
			handle_command (sess, word_eol[2], FALSE);
		}
		list = list->next;
	}

	return TRUE;
}

static int
cmd_allservers (struct session *sess, char *tbuf, char *word[],
					 char *word_eol[])
{
	GSList *list;
	server *serv;

	if (!*word_eol[2])
		return FALSE;

	list = serv_list;
	while (list)
	{
		serv = list->data;
		if (serv->connected)
			handle_command (serv->front_session, word_eol[2], FALSE);
		list = list->next;
	}

	return TRUE;
}

static int
cmd_away (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *reason = word_eol[2];

	if (!(*reason))
	{
		if (sess->server->is_away)
		{
			if (sess->server->last_away_reason)
				PrintTextf (sess, _("Already marked away: %s\n"), sess->server->last_away_reason);
			return FALSE;
		}

		if (sess->server->reconnect_away)
			reason = sess->server->last_away_reason;
		else
			/* must manage memory pointed to by random_line() */
			reason = random_line (prefs.hex_away_reason);
	}
	sess->server->p_set_away (sess->server, reason);

	if (sess->server->last_away_reason != reason)
	{
		g_free (sess->server->last_away_reason);

		if (reason == word_eol[2])
			sess->server->last_away_reason = g_strdup (reason);
		else
			sess->server->last_away_reason = reason;
	}

	if (!sess->server->connected)
		sess->server->reconnect_away = 1;

	return TRUE;
}

static int
cmd_back (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (sess->server->is_away)
	{
		sess->server->p_set_back (sess->server);
	}
	else
	{
		PrintText (sess, _("Already marked back.\n"));
	}

	g_free (sess->server->last_away_reason);
	sess->server->last_away_reason = NULL;

	return TRUE;
}

static char *
create_mask (session * sess, char *mask, char *mode, char *typestr, int deop)
{
	int type;
	struct User *user;
	char *at, *dot, *lastdot;
	char username[64], fullhost[128], domain[128], buf[512], *p2;

	user = userlist_find (sess, mask);
	if (user && user->hostname)  /* it's a nickname, let's find a proper ban mask */
	{
		if (deop)
			p2 = user->nick;
		else
			p2 = "";

		mask = user->hostname;

		at = strchr (mask, '@');	/* FIXME: utf8 */
		if (!at)
			return NULL;					  /* can't happen? */
		*at = 0;

		if (mask[0] == '~' || mask[0] == '+' ||
		    mask[0] == '=' || mask[0] == '^' || mask[0] == '-')
		{
			/* the ident is prefixed with something, we replace that sign with an * */
			safe_strcpy (username+1, mask+1, sizeof (username)-1);
			username[0] = '*';
		} else if (at - mask < USERNAMELEN)
		{
			/* we just add an * in the begining of the ident */
			safe_strcpy (username+1, mask, sizeof (username)-1);
			username[0] = '*';
		} else
		{
			/* ident might be too long, we just ban what it gives and add an * in the end */
			safe_strcpy (username, mask, sizeof (username));
		}
		*at = '@';
		safe_strcpy (fullhost, at + 1, sizeof (fullhost));

		dot = strchr (fullhost, '.');
		if (dot)
		{
			safe_strcpy (domain, dot, sizeof (domain));
		} else
		{
			safe_strcpy (domain, fullhost, sizeof (domain));
		}

		if (*typestr)
			type = atoi (typestr);
		else
			type = prefs.hex_irc_ban_type;

		buf[0] = 0;
		if (inet_addr (fullhost) != (guint32) -1)	/* "fullhost" is really a IP number */
		{
			lastdot = strrchr (fullhost, '.');
			if (!lastdot)
				return NULL;				  /* can't happen? */

			*lastdot = 0;
			strcpy (domain, fullhost);
			*lastdot = '.';

			switch (type)
			{
			case 0:
				g_snprintf (buf, sizeof (buf), "%s %s *!*@%s.*", mode, p2, domain);
				break;

			case 1:
				g_snprintf (buf, sizeof (buf), "%s %s *!*@%s", mode, p2, fullhost);
				break;

			case 2:
				g_snprintf (buf, sizeof (buf), "%s %s *!%s@%s.*", mode, p2, username, domain);
				break;

			case 3:
				g_snprintf (buf, sizeof (buf), "%s %s *!%s@%s", mode, p2, username, fullhost);
				break;
			}
		} else
		{
			switch (type)
			{
			case 0:
				g_snprintf (buf, sizeof (buf), "%s %s *!*@*%s", mode, p2, domain);
				break;

			case 1:
				g_snprintf (buf, sizeof (buf), "%s %s *!*@%s", mode, p2, fullhost);
				break;

			case 2:
				g_snprintf (buf, sizeof (buf), "%s %s *!%s@*%s", mode, p2, username, domain);
				break;

			case 3:
				g_snprintf (buf, sizeof (buf), "%s %s *!%s@%s", mode, p2, username, fullhost);
				break;
			}
		}

	} else
	{
		g_snprintf (buf, sizeof (buf), "%s %s", mode, mask);
	}
	
	return g_strdup (buf);
}

static void
ban (session * sess, char *tbuf, char *mask, char *bantypestr, int deop)
{
	char *banmask = create_mask (sess, mask, deop ? "-o+b" : "+b", bantypestr, deop);
	server *serv = sess->server;
	
	if (banmask)
	{
		serv->p_mode (serv, sess->channel, banmask);
		g_free (banmask);
	}
}

static int
cmd_ban (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *mask = word[2];

	if (*mask)
	{
		ban (sess, tbuf, mask, word[3], 0);
	} else
	{
		sess->server->p_mode (sess->server, sess->channel, "+b");	/* banlist */
	}

	return TRUE;
}

static int
cmd_unban (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	/* Allow more than one mask in /unban -- tvk */
	int i = 2;

	while (1)
	{
		if (!*word[i])
		{
			if (i == 2)
				return FALSE;
			send_channel_modes (sess, tbuf, word, 2, i, '-', 'b', 0);
			return TRUE;
		}
		i++;
	}
}

static int
cmd_chanopt (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int ret;
	
	/* chanopt.c */
	ret = chanopt_command (sess, tbuf, word, word_eol);
	chanopt_save_all (FALSE);
	
	return ret;
}

static int
cmd_charset (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	server *serv = sess->server;
	int offset = 0;

	if (strcmp (word[2], "-quiet") == 0)
		offset++;

	if (!word[2 + offset][0])
	{
		PrintTextf (sess, "Current charset: %s\n", serv->encoding);
		if (serv->utf8only)
			PrintText (sess, "Note: Server requires UTF-8 (UTF8ONLY), encoding cannot be changed.\n");
		return TRUE;
	}

	/* Server advertised UTF8ONLY - don't allow changing encoding */
	if (serv->utf8only)
	{
		PrintText (sess, "Cannot change encoding: server requires UTF-8 (UTF8ONLY).\n");
		return TRUE;
	}

	if (servlist_check_encoding (word[2 + offset]))
	{
		server_set_encoding (serv, word[2 + offset]);
		if (offset < 1)
			PrintTextf (sess, "Charset changed to: %s\n", word[2 + offset]);
	} else
	{
		PrintTextf (sess, "\0034Unknown charset:\017 %s\n", word[2 + offset]);
	}

	return TRUE;
}

static int
cmd_clear (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	GSList *list = sess_list;
	char *reason = word_eol[2];

	if (g_ascii_strcasecmp (reason, "HISTORY") == 0)
	{
		history_free (&sess->history);
		return TRUE;
	}

	if (g_ascii_strncasecmp (reason, "all", 3) == 0)
	{
		while (list)
		{
			sess = list->data;
			if (!(sess->tab_state & TAB_STATE_NEW_HILIGHT))
				fe_text_clear (list->data, 0);
			list = list->next;
		}
		return TRUE;
	}

	if (reason[0] != '-' && !isdigit (reason[0]) && reason[0] != 0)
		return FALSE;

	fe_text_clear (sess, atoi (reason));
	return TRUE;
}

static int
cmd_close (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	GSList *list;

	if (strcmp (word[2], "-m") == 0)
	{
		list = sess_list;
		while (list)
		{
			sess = list->data;
			list = list->next;
			if (sess->type == SESS_DIALOG)
				fe_close_window (sess);
		}
	} else
	{
		if (*word_eol[2])
			sess->quitreason = word_eol[2];
		fe_close_window (sess);
	}

	return TRUE;
}

static int
cmd_ctcp (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int mbl;
	char *to = word[2];
	if (*to)
	{
		char *msg = word_eol[3];
		if (*msg)
		{
			unsigned char *cmd = (unsigned char *)msg;

			/* make the first word upper case (as per RFC) */
			while (1)
			{
				if (*cmd == ' ' || *cmd == 0)
					break;
				mbl = g_utf8_skip[*cmd];
				if (mbl == 1)
					*cmd = toupper (*cmd);
				cmd += mbl;
			}

			sess->server->p_ctcp (sess->server, to, msg);

			if (sess->server->have_echo_message && sess->server->have_labeled_response)
			{
				EMIT_SIGNAL (XP_TE_CTCPSEND, sess, to, msg, NULL, NULL, 0);
				mark_pending_echo (sess, sess->server);
			}
			else if (!sess->server->have_echo_message)
			{
				EMIT_SIGNAL (XP_TE_CTCPSEND, sess, to, msg, NULL, NULL, 0);
			}

			return TRUE;
		}
	}
	return FALSE;
}

static int
cmd_country (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *code = word[2];
	if (*code)
	{
		/* search? */
		if (strcmp (code, "-s") == 0)
		{
			country_search (word[3], sess, (void *)PrintTextf);
			return TRUE;
		}

		/* search, but forgot the -s */
		if (strchr (code, '*'))
		{
			country_search (code, sess, (void *)PrintTextf);
			return TRUE;
		}

		sprintf (tbuf, "%s = %s\n", code, country (code));
		PrintText (sess, tbuf);
		return TRUE;
	}
	return FALSE;
}

static int
cmd_cycle (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *key = NULL;
	char *chan = word[2];
	session *chan_sess;

	if (!*chan)
		chan = sess->channel;

	if (chan)
	{
		chan_sess = find_channel (sess->server, chan);

		if (chan_sess && chan_sess->type == SESS_CHANNEL)
		{
			key = chan_sess->channelkey;
			sess->server->p_cycle (sess->server, chan, key);
			return TRUE;
		}
	}

	return FALSE;
}

static int
cmd_dcc (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int goodtype;
	struct DCC *dcc = 0;
	char *type = word[2];
	if (*type)
	{
		if (!g_ascii_strcasecmp (type, "HELP"))
			return FALSE;
		if (!g_ascii_strcasecmp (type, "CLOSE"))
		{
			if (*word[3] && *word[4])
			{
				goodtype = 0;
				if (!g_ascii_strcasecmp (word[3], "SEND"))
				{
					dcc = find_dcc (word[4], word[5], TYPE_SEND);
					dcc_abort (sess, dcc);
					goodtype = TRUE;
				}
				if (!g_ascii_strcasecmp (word[3], "GET"))
				{
					dcc = find_dcc (word[4], word[5], TYPE_RECV);
					dcc_abort (sess, dcc);
					goodtype = TRUE;
				}
				if (!g_ascii_strcasecmp (word[3], "CHAT"))
				{
					dcc = find_dcc (word[4], "", TYPE_CHATRECV);
					if (!dcc)
						dcc = find_dcc (word[4], "", TYPE_CHATSEND);
					dcc_abort (sess, dcc);
					goodtype = TRUE;
				}

				if (!goodtype)
					return FALSE;

				if (!dcc)
					EMIT_SIGNAL (XP_TE_NODCC, sess, NULL, NULL, NULL, NULL, 0);

				return TRUE;

			}
			return FALSE;
		}
		if ((!g_ascii_strcasecmp (type, "CHAT")) || (!g_ascii_strcasecmp (type, "PCHAT")))
		{
			char *nick = word[3];
			int passive = (!g_ascii_strcasecmp(type, "PCHAT")) ? 1 : 0;
			if (*nick)
				dcc_chat (sess, nick, passive);
			return TRUE;
		}
		if (!g_ascii_strcasecmp (type, "LIST"))
		{
			dcc_show_list (sess);
			return TRUE;
		}
		if (!g_ascii_strcasecmp (type, "GET"))
		{
			char *nick = word[3];
			char *file = word[4];
			if (!*file)
			{
				if (*nick)
					dcc_get_nick (sess, nick);
			} else
			{
				dcc = find_dcc (nick, file, TYPE_RECV);
				if (dcc)
					dcc_get (dcc);
				else
					EMIT_SIGNAL (XP_TE_NODCC, sess, NULL, NULL, NULL, NULL, 0);
			}
			return TRUE;
		}
		if ((!g_ascii_strcasecmp (type, "SEND")) || (!g_ascii_strcasecmp (type, "PSEND")))
		{
			int i = 3, maxcps;
			char *nick, *file;
			int passive = (!g_ascii_strcasecmp(type, "PSEND")) ? 1 : 0;

			nick = word[i];
			if (!*nick)
				return FALSE;

			maxcps = prefs.hex_dcc_max_send_cps;
			if (!g_ascii_strncasecmp(nick, "-maxcps=", 8))
			{
				maxcps = atoi(nick + 8);
				i++;
				nick = word[i];
				if (!*nick)
					return FALSE;
			}

			i++;

			file = word[i];
			if (!*file)
			{
				fe_dcc_send_filereq (sess, nick, maxcps, passive);
				return TRUE;
			}

			do
			{
				dcc_send (sess, nick, file, maxcps, passive);
				i++;
				file = word[i];
			}
			while (*file);

			return TRUE;
		}

		return FALSE;
	}

	dcc_show_list (sess);
	return TRUE;
}

static int
cmd_debug (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	struct session *s;
	struct server *v;
	GSList *list = sess_list;

	PrintText (sess, "Session   T Channel    WaitChan  WillChan  Server\n");
	while (list)
	{
		s = (struct session *) list->data;
		sprintf (tbuf, "%p %1x %-10.10s %-10.10s %-10.10s %p\n",
					s, s->type, s->channel, s->waitchannel,
					s->willjoinchannel, s->server);
		PrintText (sess, tbuf);
		list = list->next;
	}

	list = serv_list;
	PrintText (sess, "Server    Sock  Name\n");
	while (list)
	{
		v = (struct server *) list->data;
		sprintf (tbuf, "%p %-5d %s\n",
					v, v->sok, v->servername);
		PrintText (sess, tbuf);
		list = list->next;
	}

	sprintf (tbuf,
				"\nfront_session: %p\n"
				"current_tab: %p\n\n",
				sess->server->front_session, current_tab);
	PrintText (sess, tbuf);

	return TRUE;
}

static int
cmd_delbutton (struct session *sess, char *tbuf, char *word[],
					char *word_eol[])
{
	if (*word[2])
	{
		if (sess->type == SESS_DIALOG)
		{
			if (list_delentry (&dlgbutton_list, word[2]))
				fe_dlgbuttons_update (sess);
		} else
		{
			if (list_delentry (&button_list, word[2]))
				fe_buttons_update (sess);
		}
		return TRUE;
	}
	return FALSE;
}

static int
cmd_dehop (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int i = 2;

	while (1)
	{
		if (!*word[i])
		{
			if (i == 2)
				return FALSE;
			send_channel_modes (sess, tbuf, word, 2, i, '-', 'h', 0);
			return TRUE;
		}
		i++;
	}
}

static int
cmd_deop (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int i = 2;

	while (1)
	{
		if (!*word[i])
		{
			if (i == 2)
				return FALSE;
			send_channel_modes (sess, tbuf, word, 2, i, '-', 'o', 0);
			return TRUE;
		}
		i++;
	}
}

typedef struct
{
	char **nicks;
	int i;
	session *sess;
	char *reason;
	char *tbuf;
} multidata;

static int
mdehop_cb (struct User *user, multidata *data)
{
	if (user->hop && !user->me)
	{
		data->nicks[data->i] = user->nick;
		data->i++;
	}
	return TRUE;
}

static int
cmd_mdehop (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char **nicks = g_new0 (char *, sess->hops);
	multidata data;

	data.nicks = nicks;
	data.i = 0;
	tree_foreach (sess->usertree, (tree_traverse_func *)mdehop_cb, &data);
	send_channel_modes (sess, tbuf, nicks, 0, data.i, '-', 'h', 0);
	g_free (nicks);

	return TRUE;
}

static int
mdeop_cb (struct User *user, multidata *data)
{
	if (user->op && !user->me)
	{
		data->nicks[data->i] = user->nick;
		data->i++;
	}
	return TRUE;
}

static int
cmd_mdeop (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char **nicks = g_new0(char *, sess->ops);
	multidata data;

	data.nicks = nicks;
	data.i = 0;
	tree_foreach (sess->usertree, (tree_traverse_func *)mdeop_cb, &data);
	send_channel_modes (sess, tbuf, nicks, 0, data.i, '-', 'o', 0);
	g_free (nicks);

	return TRUE;
}

GSList *menu_list = NULL;

static void
menu_free (menu_entry *me)
{
	g_free (me->path);
	g_free (me->label);
	g_free (me->cmd);
	g_free (me->ucmd);
	g_free (me->group);
	g_free (me->icon);
	g_free (me);
}

/* strings equal? but ignore underscores */

int
menu_streq (const char *s1, const char *s2, int def)
{
	/* for separators */
	if (s1 == NULL && s2 == NULL)
		return 0;
	if (s1 == NULL || s2 == NULL)
		return 1;
	while (*s1)
	{
		if (*s1 == '_')
			s1++;
		if (*s2 == '_')
			s2++;
		if (*s1 != *s2)
			return 1;
		s1++;
		s2++;
	}
	if (!*s2)
		return 0;
	return def;
}

static menu_entry *
menu_entry_find (char *path, char *label)
{
	GSList *list;
	menu_entry *me;

	list = menu_list;
	while (list)
	{
		me = list->data;
		if (!strcmp (path, me->path))
		{
			if (me->label && label && !strcmp (label, me->label))
				return me;
		}
		list = list->next;
	}
	return NULL;
}

static void
menu_del_children (char *path, char *label)
{
	GSList *list, *next;
	menu_entry *me;
	char buf[512];

	if (!label)
		label = "";
	if (path[0])
		g_snprintf (buf, sizeof (buf), "%s/%s", path, label);
	else
		g_snprintf (buf, sizeof (buf), "%s", label);

	list = menu_list;
	while (list)
	{
		me = list->data;
		next = list->next;
		if (!menu_streq (buf, me->path, 0))
		{
			menu_list = g_slist_remove (menu_list, me);
			menu_free (me);
		}
		list = next;
	}
}

static int
menu_del (char *path, char *label)
{
	GSList *list;
	menu_entry *me;

	list = menu_list;
	while (list)
	{
		me = list->data;
		if (!menu_streq (me->label, label, 1) && !menu_streq (me->path, path, 1))
		{
			menu_list = g_slist_remove (menu_list, me);
			fe_menu_del (me);
			menu_free (me);
			/* delete this item's children, if any */
			menu_del_children (path, label);
			return 1;
		}
		list = list->next;
	}

	return 0;
}

static char
menu_is_mainmenu_root (char *path, gint16 *offset)
{
	static const char *menus[] = {"\x4$TAB","\x5$TRAY","\x4$URL","\x5$NICK","\x5$CHAN"};
	int i;

	for (i = 0; i < 5; i++)
	{
		if (!strncmp (path, menus[i] + 1, menus[i][0]))
		{
			*offset = menus[i][0];	/* number of bytes to offset the root */
			if (path[*offset] != '\0')
				*offset += 1;
			return 0;	/* is not main menu */
		}
	}

	*offset = 0;
	return 1;	/* is main menu */
}

static void
menu_add (char *path, char *label, char *cmd, char *ucmd, int pos, int state, int markup, int enable, int mod, int key, char *group, char *icon)
{
	menu_entry *me;

	/* already exists? */
	me = menu_entry_find (path, label);
	if (me)
	{
		/* update only */
		me->state = state;
		me->enable = enable;
		fe_menu_update (me);
		return;
	}

	me = g_new (menu_entry, 1);
	me->pos = pos;
	me->modifier = mod;
	me->is_main = menu_is_mainmenu_root (path, &me->root_offset);
	me->state = state;
	me->markup = markup;
	me->enable = enable;
	me->key = key;
	me->path = g_strdup (path);
	me->label = NULL;
	me->cmd = NULL;
	me->ucmd = NULL;
	me->group = NULL;
	me->icon = NULL;

	me->label = g_strdup (label);
	me->cmd = g_strdup (cmd);
	me->ucmd = g_strdup (ucmd);
	me->group = g_strdup (group);
	me->icon = g_strdup (icon);

	menu_list = g_slist_append (menu_list, me);
	label = fe_menu_add (me);
	if (label)
	{
		/* FE has given us a stripped label */
		g_free (me->label);
		me->label = g_strdup (label);
		g_free (label); /* this is from pango */
	}
}

static int
cmd_menu (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int idx = 2;
	int len;
	int pos = 0xffff;
	int state = 0;
	int toggle = FALSE;
	int enable = TRUE;
	int markup = FALSE;
	int key = 0;
	int mod = 0;
	char *label;
	char *group = NULL;
	char *icon = NULL;

	if (!word[2][0] || !word[3][0])
		return FALSE;

	/* -eX enabled or not? */
	if (word[idx][0] == '-' && word[idx][1] == 'e')
	{
		enable = atoi (word[idx] + 2);
		idx++;
	}

	/* -i<ICONFILE> */
	if (word[idx][0] == '-' && word[idx][1] == 'i')
	{
		icon = word[idx] + 2;
		idx++;
	}

	/* -k<mod>,<key> key binding */
	if (word[idx][0] == '-' && word[idx][1] == 'k')
	{
		char *comma = strchr (word[idx], ',');
		if (!comma)
			return FALSE;
		mod = atoi (word[idx] + 2);
		key = atoi (comma + 1);
		idx++;
	}

	/* -m to specify PangoMarkup language */
	if (word[idx][0] == '-' && word[idx][1] == 'm')
	{
		markup = TRUE;
		idx++;
	}

	/* -pX to specify menu position */
	if (word[idx][0] == '-' && word[idx][1] == 'p')
	{
		pos = atoi (word[idx] + 2);
		idx++;
	}

	/* -rSTATE,GROUP to specify a radio item */
	if (word[idx][0] == '-' && word[idx][1] == 'r')
	{
		state = atoi (word[idx] + 2);
		group = word[idx] + 4;
		idx++;
	}

	/* -tX to specify toggle item with default state */
	if (word[idx][0] == '-' && word[idx][1] == 't')
	{
		state = atoi (word[idx] + 2);
		idx++;
		toggle = TRUE;
	}

	if (word[idx+1][0] == 0)
		return FALSE;

	/* the path */
	path_part (word[idx+1], tbuf, 512);
	len = strlen (tbuf);
	if (len)
		tbuf[len - 1] = 0;

	/* the name of the item */
	label = file_part (word[idx + 1]);
	if (label[0] == '-' && label[1] == 0)
		label = NULL;	/* separator */

	if (markup)
	{
		char *p;	/* to force pango closing tags through */
		for (p = label; p && *p; p++)
			if (*p == 3)
				*p = '/';
	}

	if (!g_ascii_strcasecmp (word[idx], "ADD"))
	{
		if (toggle)
		{
			menu_add (tbuf, label, word[idx + 2], word[idx + 3], pos, state, markup, enable, mod, key, NULL, NULL);
		} else
		{
			if (word[idx + 2][0])
				menu_add (tbuf, label, word[idx + 2], NULL, pos, state, markup, enable, mod, key, group, icon);
			else
				menu_add (tbuf, label, NULL, NULL, pos, state, markup, enable, mod, key, group, icon);
		}
		return TRUE;
	}

	if (!g_ascii_strcasecmp (word[idx], "DEL"))
	{
		menu_del (tbuf, label);
		return TRUE;
	}

	return FALSE;
}

static int
mhop_cb (struct User *user, multidata *data)
{
	if (!user->hop)
	{
		data->nicks[data->i] = user->nick;
		data->i++;
	}
	return TRUE;
}

static int
cmd_mhop (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char **nicks = g_new0 (char *, sess->total - sess->hops);
	multidata data;

	data.nicks = nicks;
	data.i = 0;
	tree_foreach (sess->usertree, (tree_traverse_func *)mhop_cb, &data);
	send_channel_modes (sess, tbuf, nicks, 0, data.i, '+', 'h', 0);

	g_free (nicks);

	return TRUE;
}

static int
mkick_cb (struct User *user, multidata *data)
{
	if (!user->op && !user->me)
		data->sess->server->p_kick (data->sess->server, data->sess->channel, user->nick, data->reason);
	return TRUE;
}

static int
mkickops_cb (struct User *user, multidata *data)
{
	if (user->op && !user->me)
		data->sess->server->p_kick (data->sess->server, data->sess->channel, user->nick, data->reason);
	return TRUE;
}

static int
cmd_mkick (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	multidata data;

	data.sess = sess;
	data.reason = word_eol[2];
	tree_foreach (sess->usertree, (tree_traverse_func *)mkickops_cb, &data);
	tree_foreach (sess->usertree, (tree_traverse_func *)mkick_cb, &data);

	return TRUE;
}

static int
cmd_devoice (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int i = 2;

	while (1)
	{
		if (!*word[i])
		{
			if (i == 2)
				return FALSE;
			send_channel_modes (sess, tbuf, word, 2, i, '-', 'v', 0);
			return TRUE;
		}
		i++;
	}
}

static int
cmd_discon (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	sess->server->disconnect (sess, TRUE, -1);
	return TRUE;
}

static int
cmd_dns (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *nick = word[2];
	struct User *user;
	message_tags_data no_tags = MESSAGE_TAGS_DATA_INIT;

	if (*nick)
	{
		user = userlist_find (sess, nick);
		if (user)
		{
			if (user->hostname)
			{
				do_dns (sess, user->nick, user->hostname, &no_tags);
			} else
			{
				sess->server->p_get_ip (sess->server, nick);
				sess->server->doing_dns = TRUE;
			}
		} else
		{
			do_dns (sess, NULL, nick, &no_tags);
		}
		return TRUE;
	}
	return FALSE;
}

static int
cmd_doat (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	GStrv channels;
	guint i;

	if (!word[2] || !*word[2] || !word[3] || !*word[3])
		return FALSE;

	channels = g_strsplit (word[2], ",", -1);
	for (i = 0; channels[i] && *channels[i]; ++i)
	{
		char *chan = channels[i];
		char *serv;
		session *ctx;

		/* Split channel and network, either may be empty */
		if ((serv = strrchr (chan, '/')))
		{
			*serv = '\0';
			serv++;
			if (!strlen (serv))
				serv = NULL;
		}
		if (!strlen (chan))
			chan = NULL;

		if ((ctx = plugin_find_context (serv, chan, sess->server)))
			handle_command (ctx, word_eol[3], FALSE);
	}
	g_strfreev (channels);

	return TRUE;
}

static int
cmd_echo (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	PrintText (sess, word_eol[2]);
	return TRUE;
}

#ifndef WIN32

static void
exec_check_process (struct session *sess)
{
	int val;

	if (sess->running_exec == NULL)
		return;
	val = waitpid (sess->running_exec->childpid, NULL, WNOHANG);
	if (val == -1 || val > 0)
	{
		close (sess->running_exec->myfd);
		fe_input_remove (sess->running_exec->iotag);
		g_free (sess->running_exec);
		sess->running_exec = NULL;
	}
}

static int
cmd_execs (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int r;

	exec_check_process (sess);
	if (sess->running_exec == NULL)
	{
		EMIT_SIGNAL (XP_TE_NOCHILD, sess, NULL, NULL, NULL, NULL, 0);
		return FALSE;
	}
	r = kill (sess->running_exec->childpid, SIGSTOP);
	if (r == -1)
		PrintText (sess, "Error in kill(2)\n");

	return TRUE;
}

static int
cmd_execc (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int r;

	exec_check_process (sess);
	if (sess->running_exec == NULL)
	{
		EMIT_SIGNAL (XP_TE_NOCHILD, sess, NULL, NULL, NULL, NULL, 0);
		return FALSE;
	}
	r = kill (sess->running_exec->childpid, SIGCONT);
	if (r == -1)
		PrintText (sess, "Error in kill(2)\n");

	return TRUE;
}

static int
cmd_execk (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int r;

	exec_check_process (sess);
	if (sess->running_exec == NULL)
	{
		EMIT_SIGNAL (XP_TE_NOCHILD, sess, NULL, NULL, NULL, NULL, 0);
		return FALSE;
	}
	if (strcmp (word[2], "-9") == 0)
		r = kill (sess->running_exec->childpid, SIGKILL);
	else
		r = kill (sess->running_exec->childpid, SIGTERM);
	if (r == -1)
		PrintText (sess, "Error in kill(2)\n");

	return TRUE;
}

/* OS/2 Can't have the /EXECW command because it uses pipe(2) not socketpair
   and thus it is simplex --AGL */
static int
cmd_execw (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int len;
	char *temp;
	exec_check_process (sess);
	if (sess->running_exec == NULL)
	{
		EMIT_SIGNAL (XP_TE_NOCHILD, sess, NULL, NULL, NULL, NULL, 0);
		return FALSE;
	}
	if (strcmp (word[2], "--") == 0)
	{
		len = strlen(word_eol[3]);
		temp = g_strconcat (word_eol[3], "\n", NULL);
		PrintText(sess, temp);
	}
	else
	{
		if (strcmp (word[2], "-q") == 0)
		{
			len = strlen(word_eol[3]);
			temp = g_strconcat (word_eol[3], "\n", NULL);
		}
		else
		{
			len = strlen(word_eol[2]);
			temp = g_strconcat (word_eol[2], "\n", NULL);
			PrintText(sess, temp);
		}
	}
	HC_IGNORE_RESULT (write(sess->running_exec->myfd, temp, len + 1));
	g_free(temp);

	return TRUE;
}

/* convert ANSI escape color codes to mIRC codes */

static short escconv[] =
/* 0 1 2 3 4 5  6 7  0 1 2 3 4  5  6  7 */
{  1,4,3,5,2,10,6,1, 1,7,9,8,12,11,13,1 };

static void
exec_handle_colors (char *buf, int len)
{
	char numb[16];
	char *nbuf;
	int i = 0, j = 0, k = 0, firstn = 0, col, colf = 0, colb = 0;
	int esc = FALSE, backc = FALSE, bold = FALSE;

	/* any escape codes in this text? */
	if (strchr (buf, 27) == 0)
		return;

	nbuf = g_malloc (len + 1);

	while (i < len)
	{
		switch (buf[i])
		{
		case '\r':
			break;
		case 27:
			esc = TRUE;
			break;
		case ';':
			if (!esc)
				goto norm;
			backc = TRUE;
			numb[k] = 0;
			firstn = atoi (numb);
			k = 0;
			break;
		case '[':
			if (!esc)
				goto norm;
			break;
		default:
			if (esc)
			{
				if (buf[i] >= 'A' && buf[i] <= 'z')
				{
					if (buf[i] == 'm')
					{
						/* ^[[0m */
						if (k == 0 || (numb[0] == '0' && k == 1))
						{
							nbuf[j] = '\017';
							j++;
							bold = FALSE;
							goto cont;
						}

						numb[k] = 0;
						col = atoi (numb);
						backc = FALSE;

						if (firstn == 1)
							bold = TRUE;

						if (firstn >= 30 && firstn <= 37)
							colf = firstn - 30;

						if (col >= 40)
						{
							colb = col - 40;
							backc = TRUE;
						}

						if (col >= 30 && col <= 37)
							colf = col - 30;

						if (bold)
							colf += 8;

						if (backc)
						{
							colb = escconv[colb % 14];
							colf = escconv[colf % 14];
							j += sprintf (&nbuf[j], "\003%d,%02d", colf, colb);
						} else
						{
							colf = escconv[colf % 14];
							j += sprintf (&nbuf[j], "\003%02d", colf);
						}
					}
cont:				esc = FALSE;
					backc = FALSE;
					k = 0;
				} else
				{
					if (isdigit ((unsigned char) buf[i]) && k < (sizeof (numb) - 1))
					{
						numb[k] = buf[i];
						k++;
					}
				}
			} else
			{
norm:			nbuf[j] = buf[i];
				j++;
			}
		}
		i++;
	}

	nbuf[j] = 0;
	memcpy (buf, nbuf, j + 1);
	g_free (nbuf);
}

#ifndef HAVE_MEMRCHR
static void *
memrchr (const void *block, int c, size_t size)
{
	unsigned char *p;

	for (p = (unsigned char *)block + size; p != block; p--)
		if (*p == c)
			return p;
	return NULL;
}
#endif

static void
exec_print_line (session *sess, char *data, gssize len, gboolean tochannel)
{
	char *valid;
	exec_handle_colors (data, len);
	valid = text_fixup_invalid_utf8 (data, len, NULL);
	if (tochannel)
	{
		/* must turn off auto-completion temporarily */
		const unsigned int old = prefs.hex_completion_auto;
		prefs.hex_completion_auto = 0;
		handle_multiline (sess, valid, FALSE, TRUE);
		prefs.hex_completion_auto = old;
	}
	else
		PrintText (sess, valid);
	g_free (valid);
}

static gboolean
exec_data (GIOChannel *source, GIOCondition condition, struct nbexec *s)
{
	char *buf, *readpos, *rest;
	int rd, len;
	int sok = s->myfd;

	len = s->buffill;
	if (len) {
		/* append new data to buffered incomplete line */
		buf = g_malloc (len + 2050);
		memcpy(buf, s->linebuf, len);
		readpos = buf + len;
		g_free (s->linebuf);
		s->linebuf = NULL;
	}
	else
		readpos = buf = g_malloc (2050);

	rd = read (sok, readpos, 2048);
	if (rd < 1)
	{
		/* The process has died */
		kill(s->childpid, SIGKILL);
		if (len) {
			buf[len] = '\0';
			exec_print_line(s->sess, buf, len, s->tochannel);
		}
		g_free(buf);
		waitpid (s->childpid, NULL, 0);
		s->sess->running_exec = NULL;
		fe_input_remove (s->iotag);
		close (sok);
		g_free (s);
		return TRUE;
	}
	len += rd;
	buf[len] = '\0';

	rest = memrchr(buf, '\n', len);
	if (rest)
		rest++;
	else
		rest = buf;
	if (*rest) {
		s->buffill = len - (rest - buf); /* = strlen(rest) */
		s->linebuf = g_malloc (s->buffill + 1);
		memcpy(s->linebuf, rest, s->buffill);
		*rest = '\0';
		len -= s->buffill; /* possibly 0 */
	}
	else
		s->buffill = 0;

	if (len) {
		exec_print_line(s->sess, buf, len, s->tochannel);
	}

	g_free (buf);
	return TRUE;
}

static int
cmd_exec (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int tochannel = FALSE;
	char *cmd = word_eol[2];
	int fds[2], pid = 0;
	struct nbexec *s;
	int shell = TRUE;
	int fd;

	if (*cmd)
	{
		exec_check_process (sess);
		if (sess->running_exec != NULL)
		{
			EMIT_SIGNAL (XP_TE_ALREADYPROCESS, sess, NULL, NULL, NULL, NULL, 0);
			return TRUE;
		}

		if (!strcmp (word[2], "-d"))
		{
			if (!*word[3])
				return FALSE;
			cmd = word_eol[3];
			shell = FALSE;
		}
		else if (!strcmp (word[2], "-o"))
		{
			if (!*word[3])
				return FALSE;
			cmd = word_eol[3];
			tochannel = TRUE;
		}

		if (shell)
		{
			if (access ("/bin/sh", X_OK) != 0)
			{
				fe_message (_("I need /bin/sh to run!\n"), FE_MSG_ERROR);
				return TRUE;
			}
		}

		if (socketpair (PF_UNIX, SOCK_STREAM, 0, fds) == -1)
		{
			PrintText (sess, "socketpair(2) failed\n");
			return FALSE;
		}
		s = g_new0 (struct nbexec, 1);
		s->myfd = fds[0];
		s->tochannel = tochannel;
		s->sess = sess;

		pid = fork ();
		if (pid == 0)
		{
			/* This is the child's context */
			close (0);
			close (1);
			close (2);
			/* Close parent's end of pipe */
			close(s->myfd);
			/* Copy the child end of the pipe to stdout and stderr */
			dup2 (fds[1], 1);
			dup2 (fds[1], 2);
			/* Also copy it to stdin so we can write to it */
			dup2 (fds[1], 0);
			/* Now close all open file descriptors except stdin, stdout and stderr */
			for (fd = 3; fd < 1024; fd++) close(fd);
			/* Now we call /bin/sh to run our cmd ; made it more friendly -DC1 */
			if (shell)
			{
				execl ("/bin/sh", "sh", "-c", cmd, NULL);
			} else
			{
				char **argv;
				int argc;

				g_shell_parse_argv (cmd, &argc, &argv, NULL);
				execvp (argv[0], argv);

				g_strfreev (argv);
			}
			/* not reached unless error */
			/*printf("exec error\n");*/
			fflush (stdout);
			_exit (0);
		}
		if (pid == -1)
		{
			/* Parent context, fork() failed */

			PrintText (sess, "Error in fork(2)\n");
			close(fds[0]);
			close(fds[1]);
			g_free (s);
		}
		else
		{
			/* Parent path */
			close(fds[1]);
			s->childpid = pid;
			s->iotag = fe_input_add (s->myfd, FIA_READ|FIA_EX, exec_data, s);
			sess->running_exec = s;
			return TRUE;
		}
	}
	return FALSE;
}

#endif


static int
cmd_flushq (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	sprintf (tbuf, "Flushing server send queue, %d bytes.\n", sess->server->sendq_len);
	PrintText (sess, tbuf);
	sess->server->flush_queue (sess->server);
	return TRUE;
}

static int
cmd_quit (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (*word_eol[2])
		sess->quitreason = word_eol[2];
	sess->server->disconnect (sess, TRUE, -1);
	sess->quitreason = NULL;
	return 2;
}

static int
cmd_gate (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *server_name = word[2];
	server *serv = sess->server;
	if (*server_name)
	{
		char *port = word[3];
#ifdef USE_OPENSSL
		serv->use_ssl = FALSE;
#endif
		server_fill_her_up (serv);
		if (*port)
			serv->connect (serv, server_name, atoi (port), TRUE);
		else
			serv->connect (serv, server_name, 23, TRUE);
		return TRUE;
	}
	return FALSE;
}

typedef struct
{
	char *cmd;
	session *sess;
} getvalinfo;

static void
get_bool_cb (int val, getvalinfo *info)
{
	char buf[512];

	g_snprintf (buf, sizeof (buf), "%s %d", info->cmd, val);
	if (is_session (info->sess))
		handle_command (info->sess, buf, FALSE);

	g_free (info->cmd);
	g_free (info);
}

static int
cmd_getbool (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	getvalinfo *info;

	if (!word[4][0])
		return FALSE;

	info = g_new (getvalinfo, 1);
	info->cmd = g_strdup (word[2]);
	info->sess = sess;

	fe_get_bool (word[3], word_eol[4], get_bool_cb, info);

	return TRUE;
}

static void
get_int_cb (int cancel, int val, getvalinfo *info)
{
	char buf[512];

	if (!cancel)
	{
		g_snprintf (buf, sizeof (buf), "%s %d", info->cmd, val);
		if (is_session (info->sess))
			handle_command (info->sess, buf, FALSE);
	}

	g_free (info->cmd);
	g_free (info);
}

static int
cmd_getint (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	getvalinfo *info;

	if (!word[4][0])
		return FALSE;

	info = g_new (getvalinfo, 1);
	info->cmd = g_strdup (word[3]);
	info->sess = sess;

	fe_get_int (word[4], atoi (word[2]), get_int_cb, info);

	return TRUE;
}

static void
get_file_cb (char *cmd, char *file)
{
	char buf[1024 + 128];

	/* execute the command once per file, then once more with
      no args */
	if (file)
	{
		g_snprintf (buf, sizeof (buf), "%s %s", cmd, file);
		handle_command (current_sess, buf, FALSE);
	}
	else
	{
		handle_command (current_sess, cmd, FALSE);
		g_free (cmd);
	}
}

static int
cmd_getfile (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int idx = 2;
	int flags = 0;

	if (!word[3][0])
		return FALSE;

	if (!strcmp (word[2], "-folder"))
	{
		flags |= FRF_CHOOSEFOLDER;
		idx++;
	}

	if (!strcmp (word[idx], "-multi"))
	{
		flags |= FRF_MULTIPLE;
		idx++;
	}

	if (!strcmp (word[idx], "-save"))
	{
		flags |= FRF_WRITE;
		idx++;
	}

	fe_get_file (word[idx+1], word[idx+2], (void *)get_file_cb, g_strdup (word[idx]), flags);

	return TRUE;
}

static void
get_str_cb (int cancel, char *val, getvalinfo *info)
{
	char buf[512];

	if (!cancel)
	{
		g_snprintf (buf, sizeof (buf), "%s %s", info->cmd, val);
		if (is_session (info->sess))
			handle_command (info->sess, buf, FALSE);
	}

	g_free (info->cmd);
	g_free (info);
}

static int
cmd_getstr (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	getvalinfo *info;

	if (!word[4][0])
		return FALSE;

	info = g_new (getvalinfo, 1);
	info->cmd = g_strdup (word[3]);
	info->sess = sess;

	fe_get_str (word[4], word[2], get_str_cb, info);

	return TRUE;
}

static int
cmd_ghost (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (!word[2][0])
		return FALSE;

	sess->server->p_ns_ghost (sess->server, word[2], word[3]);
	return TRUE;
}

static int
cmd_gui (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	switch (str_ihash (word[2]))
	{
	case 0x058b836e: fe_ctrl_gui (sess, 8, 0); break; /* APPLY */
	case 0xac1eee45: fe_ctrl_gui (sess, 7, 2); break; /* ATTACH */
	case 0xb06a1793: fe_ctrl_gui (sess, 7, 1); break; /* DETACH */
	case 0x05cfeff0: fe_ctrl_gui (sess, 3, 0); break; /* FLASH */
	case 0x05d154d8: fe_ctrl_gui (sess, 2, 0); break; /* FOCUS */
	case 0x0030dd42: fe_ctrl_gui (sess, 0, 0); break; /* HIDE */
	case 0x61addbe3: fe_ctrl_gui (sess, 5, 0); break; /* ICONIFY */
	case 0xc0851aaa: fe_message (word[3], FE_MSG_INFO|FE_MSG_MARKUP); break; /* MSGBOX */
	case 0x0035dafd: fe_ctrl_gui (sess, 1, 0); break; /* SHOW */
	case 0x0033155f: /* MENU */
		if (!g_ascii_strcasecmp (word[3], "TOGGLE"))
			fe_ctrl_gui (sess, 6, 0);
		else
			return FALSE;
		break;
	case 0x05a72f63: /* COLOR */
		if (!g_ascii_strcasecmp (word[4], "-NOOVERRIDE"))
			fe_ctrl_gui (sess, 4, FE_COLOR_FLAG_NOOVERRIDE | atoi (word[3]));
		else
			fe_ctrl_gui (sess, 4, atoi (word[3]));
		break;
	default:
		return FALSE;
	}

	return TRUE;
}

typedef struct
{
	int longfmt;
	int i, t;
	char *buf;
} help_list;

static void
show_help_line (session *sess, help_list *hl, char *name, char *usage)
{
	int j, len, max;
	char *p;

	if (name[0] == '.')	/* hidden command? */
		return;

	if (hl->longfmt)	/* long format for /HELP -l */
	{
		if (!usage || usage[0] == 0)
			PrintTextf (sess, "   \0034%s\003 :\n", name);
		else
			PrintTextf (sess, "   \0034%s\003 : %s\n", name, _(usage));
		return;
	}

	/* append the name into buffer, but convert to uppercase */
	len = strlen (hl->buf);
	p = name;
	while (*p)
	{
		hl->buf[len] = toupper ((unsigned char) *p);
		len++;
		p++;
	}
	hl->buf[len] = 0;

	hl->t++;
	if (hl->t == 5)
	{
		hl->t = 0;
		strcat (hl->buf, "\n");
		PrintText (sess, hl->buf);
		hl->buf[0] = ' ';
		hl->buf[1] = ' ';
		hl->buf[2] = 0;
	} else
	{
		/* append some spaces after the command name */
		max = strlen (name);
		if (max < 10)
		{
			max = 10 - max;
			for (j = 0; j < max; j++)
			{
				hl->buf[len] = ' ';
				len++;
				hl->buf[len] = 0;
			}
		}
	}
}

static int
cmd_history (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	server *serv = sess->server;
	int limit = 0;
	const char *target = NULL;

	/* Check if chathistory is available */
	if (!serv->have_chathistory)
	{
		PrintText (sess, "Chat history is not available on this server.\n");
		return TRUE;
	}

	if (!serv->connected)
	{
		notc_msg (sess);
		return TRUE;
	}

	/* Parse arguments: /HISTORY [target] [limit] */
	if (word[2][0])
	{
		/* Check if first arg is a number (limit) or target */
		if (g_ascii_isdigit (word[2][0]))
		{
			limit = atoi (word[2]);
		}
		else
		{
			target = word[2];
			if (word[3][0])
				limit = atoi (word[3]);
		}
	}

	/* If no target specified, use current session */
	if (!target)
	{
		if (sess->type == SESS_CHANNEL || sess->type == SESS_DIALOG)
		{
			chathistory_request_latest (sess, NULL, limit);
		}
		else
		{
			PrintText (sess, "Use /HISTORY in a channel or query window, or specify a target.\n");
		}
	}
	else
	{
		/* Find or create session for target */
		session *target_sess = find_channel (serv, (char *)target);
		if (!target_sess)
			target_sess = find_dialog (serv, (char *)target);

		if (target_sess)
		{
			chathistory_request_latest (target_sess, NULL, limit);
		}
		else
		{
			PrintTextf (sess, "No open window for %s. Join the channel or open a query first.\n", target);
		}
	}

	return TRUE;
}

/* Format a time_t as ISO 8601 UTC (YYYY-MM-DDThh:mm:ss.000Z).
 * Buffer must be at least 32 bytes. */
static void
format_iso8601 (time_t t, char *buf, int bufsize)
{
	struct tm *tm = gmtime (&t);
	if (tm)
		g_snprintf (buf, bufsize, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
		            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		            tm->tm_hour, tm->tm_min, tm->tm_sec);
	else
		g_strlcpy (buf, "0", bufsize);
}

/* Send MARKREAD for a session using its newest message timestamp.
 * Called from auto-advance (focus/tab-switch) and /MARKREAD command. */
void
markread_send_for_session (session *sess)
{
	server *serv;
	time_t newest;
	char ts[64];

	if (!sess || !sess->server)
		return;

	serv = sess->server;
	if (!serv->have_read_marker || !serv->connected)
		return;
	if (sess->type != SESS_CHANNEL && sess->type != SESS_DIALOG)
		return;
	if (!sess->channel[0])
		return;

	/* Use the newest message timestamp from the buffer */
	newest = fe_get_newest_stamp (sess);
	if (newest == 0)
		return;

	/* Only send if we have messages newer than the current server marker */
	if (sess->markread_time >= newest)
		return;

	format_iso8601 (newest, ts, sizeof (ts));
	tcp_sendf (serv, "MARKREAD %s timestamp=%s\r\n", sess->channel, ts);

	/* Optimistically update local state */
	g_free (sess->markread_timestamp);
	sess->markread_timestamp = g_strdup (ts);
	sess->markread_time = newest;
}

static int
cmd_markread (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	server *serv = sess->server;

	/* Check if read-marker is available */
	if (!serv->have_read_marker)
	{
		PrintText (sess, "Read markers are not available on this server.\n");
		return TRUE;
	}

	if (!serv->connected)
	{
		notc_msg (sess);
		return TRUE;
	}

	/* /MARKREAD with no arguments — mark current session as read */
	if (!word[2][0])
	{
		if (sess->type == SESS_CHANNEL || sess->type == SESS_DIALOG)
		{
			markread_send_for_session (sess);
		}
		else
		{
			PrintText (sess, "Use /MARKREAD in a channel or query window, or specify a target.\n");
		}
		return TRUE;
	}

	/* /MARKREAD <target> — query current marker for target */
	/* /MARKREAD <target> <timestamp> — set marker explicitly */
	{
		const char *target = word[2];
		const char *ts = word[3][0] ? word[3] : NULL;

		if (ts)
			tcp_sendf (serv, "MARKREAD %s timestamp=%s\r\n", target, ts);
		else
			tcp_sendf (serv, "MARKREAD %s\r\n", target);
	}

	return TRUE;
}

static int
cmd_react (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	server *serv = sess->server;
	const char *reaction;
	const char *msgid;
	char *escaped;

	if (!serv->have_message_tags)
	{
		PrintText (sess, _("Message reactions require the message-tags capability.\n"));
		return TRUE;
	}

	if (!serv->connected)
	{
		notc_msg (sess);
		return TRUE;
	}

	if (!sess->channel[0])
	{
		notj_msg (sess);
		return TRUE;
	}

	/* /REACT <emoji> [msgid] */
	if (!word[2][0])
	{
		PrintText (sess, _("Usage: /REACT <emoji> [msgid]\n"));
		return TRUE;
	}

	reaction = word[2];
	msgid = word[3][0] ? word[3] : fe_get_last_msgid (sess);

	if (!msgid)
	{
		PrintText (sess, _("No message to react to.\n"));
		return TRUE;
	}

	escaped = escape_tag_value (reaction);
	{
		char *tags = g_strdup_printf ("+draft/react=%s;+draft/reply=%s", escaped, msgid);
		tcp_sendf_with_raw_tags (serv, "TAGMSG", sess->channel, tags,
		                         "TAGMSG %s\r\n", sess->channel);
		g_free (tags);
	}
	g_free (escaped);

	return TRUE;
}

static int
cmd_unreact (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	server *serv = sess->server;
	const char *reaction;
	const char *msgid;
	char *escaped;

	if (!serv->have_message_tags)
	{
		PrintText (sess, _("Message reactions require the message-tags capability.\n"));
		return TRUE;
	}

	if (!serv->connected)
	{
		notc_msg (sess);
		return TRUE;
	}

	if (!sess->channel[0])
	{
		notj_msg (sess);
		return TRUE;
	}

	/* /UNREACT <emoji> [msgid] */
	if (!word[2][0])
	{
		PrintText (sess, _("Usage: /UNREACT <emoji> [msgid]\n"));
		return TRUE;
	}

	reaction = word[2];
	msgid = word[3][0] ? word[3] : fe_get_last_msgid (sess);

	if (!msgid)
	{
		PrintText (sess, _("No message to unreact from.\n"));
		return TRUE;
	}

	escaped = escape_tag_value (reaction);
	{
		char *tags = g_strdup_printf ("+draft/unreact=%s;+draft/reply=%s", escaped, msgid);
		tcp_sendf_with_raw_tags (serv, "TAGMSG", sess->channel, tags,
		                         "TAGMSG %s\r\n", sess->channel);
		g_free (tags);
	}
	g_free (escaped);

	return TRUE;
}

static int
cmd_reply (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	server *serv = sess->server;
	const char *msgid;
	char nick_buf[NICKLEN + 1];

	if (!serv->have_message_tags)
	{
		PrintText (sess, _("Message replies require the message-tags capability.\n"));
		return TRUE;
	}

	if (!serv->connected)
	{
		notc_msg (sess);
		return TRUE;
	}

	if (!sess->channel[0])
	{
		notj_msg (sess);
		return TRUE;
	}

	/* /REPLY [msgid] <text>
	 * If first arg looks like a msgid (contains no spaces, and there's more text),
	 * use it as the target. Otherwise reply to the last non-self message. */
	if (!word_eol[2][0])
	{
		PrintText (sess, _("Usage: /REPLY [msgid] <text>\n"));
		return TRUE;
	}

	nick_buf[0] = '\0';

	if (word[3][0] && !strchr (word[2], ' '))
	{
		/* Explicit msgid provided: /REPLY <msgid> <text> */
		msgid = word[2];

		g_free (sess->reply_msgid);
		sess->reply_msgid = g_strdup (msgid);
		g_free (sess->reply_nick);
		sess->reply_nick = g_strdup (""); /* unknown nick when explicit msgid */

		handle_say (sess, word_eol[3], TRUE);
	}
	else
	{
		/* No msgid: reply to last non-self message */
		msgid = fe_get_last_nonself_msgid (sess, nick_buf, sizeof (nick_buf));
		if (!msgid)
		{
			PrintText (sess, _("No message to reply to.\n"));
			return TRUE;
		}

		g_free (sess->reply_msgid);
		sess->reply_msgid = g_strdup (msgid);
		g_free (sess->reply_nick);
		sess->reply_nick = g_strdup (nick_buf);

		handle_say (sess, word_eol[2], TRUE);
	}

	/* reply_msgid is consumed and cleared by irc_message()/irc_action() */
	return TRUE;
}

static int
cmd_redact (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	server *serv = sess->server;
	const char *target = NULL;
	const char *msgid = NULL;
	const char *reason = NULL;

	/* Parse arguments: /REDACT [target] [msgid] [reason] */
	if (word[2][0] && word[3][0])
	{
		/* Full form: /REDACT <target> <msgid> [reason] */
		target = word[2];
		msgid = word[3];
		if (word_eol[4][0])
			reason = word_eol[4];
	}
	else
	{
		/* Missing msgid — enter picker mode to click a message */
		target = word[2][0] ? word[2] : sess->channel;
		if (!target[0])
		{
			PrintText (sess, "Usage: /REDACT [target] [msgid] [reason]\n");
			return TRUE;
		}
		g_free (sess->picker_pending_cmd);
		sess->picker_pending_cmd = g_strdup_printf ("REDACT %s %%s", target);
		fe_reply_state_changed (sess);
		return TRUE;
	}

	/* Check if redaction is available */
	if (!serv->have_redact)
	{
		PrintText (sess, "Message redaction is not available on this server.\n");
		return TRUE;
	}

	if (!serv->connected)
	{
		notc_msg (sess);
		return TRUE;
	}

	/* Send REDACT command */
	if (reason && *reason)
	{
		tcp_sendf_labeled_tracked (serv, "REDACT", target,
		                           "REDACT %s %s :%s\r\n", target, msgid, reason);
	}
	else
	{
		tcp_sendf_labeled_tracked (serv, "REDACT", target,
		                           "REDACT %s %s\r\n", target, msgid);
	}

	return TRUE;
}

static int
cmd_help (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int i = 0, longfmt = 0;
	char *helpcmd = "";
	GSList *list;

	if (tbuf)
		helpcmd = word[2];
	if (*helpcmd && strcmp (helpcmd, "-l") == 0)
		longfmt = 1;

	if (*helpcmd && !longfmt)
	{
		help (sess, tbuf, helpcmd, FALSE);
	} else
	{
		struct popup *pop;
		char *buf = g_malloc (4096);
		help_list hl;

		hl.longfmt = longfmt;
		hl.buf = buf;

		PrintTextf (sess, "\n%s\n\n", _("Commands Available:"));
		buf[0] = ' ';
		buf[1] = ' ';
		buf[2] = 0;
		hl.t = 0;
		hl.i = 0;
		while (xc_cmds[i].name)
		{
			show_help_line (sess, &hl, xc_cmds[i].name, xc_cmds[i].help);
			i++;
		}
		strcat (buf, "\n");
		PrintText (sess, buf);

		PrintTextf (sess, "\n%s\n\n", _("User defined commands:"));
		buf[0] = ' ';
		buf[1] = ' ';
		buf[2] = 0;
		hl.t = 0;
		hl.i = 0;
		list = command_list;
		while (list)
		{
			pop = list->data;
			show_help_line (sess, &hl, pop->name, pop->cmd);
			list = list->next;
		}
		strcat (buf, "\n");
		PrintText (sess, buf);

		PrintTextf (sess, "\n%s\n\n", _("Plugin defined commands:"));
		buf[0] = ' ';
		buf[1] = ' ';
		buf[2] = 0;
		hl.t = 0;
		hl.i = 0;
		plugin_command_foreach (sess, &hl, (void *)show_help_line);
		strcat (buf, "\n");
		PrintText (sess, buf);
		g_free (buf);

		PrintTextf (sess, "\n%s\n\n", _("Type /HELP <command> for more information, or /HELP -l"));
	}
	return TRUE;
}

static int
cmd_id (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (word[2][0])
	{
		sess->server->p_ns_identify (sess->server, word[2]);
		return TRUE;
	}

	return FALSE;
}

static int
cmd_ignore (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int i;
	int type = 0;
	int quiet = 0;
	char *mask;

	if (!*word[2])
	{
		ignore_showlist (sess);
		return TRUE;
	}
	if (!*word[3])
		word[3] = "ALL";

	i = 3;
	while (1)
	{
		if (!*word[i])
		{
			if (type == 0)
				return FALSE;

			mask = word[2];
			/* If not a full mask or using wildcards, assume nick */
			if (strchr (mask, '!') == NULL &&
			    strchr (mask, '@') == NULL &&
			    strchr (mask, '?') == NULL &&
			    strchr (mask, '*') == NULL)
			{
				mask = tbuf;
				g_snprintf (tbuf, TBUFSIZE, "%s!*@*", word[2]);
			}

			i = ignore_add (mask, type, TRUE);
			if (quiet)
				return TRUE;
			switch (i)
			{
			case 1:
				EMIT_SIGNAL (XP_TE_IGNOREADD, sess, mask, NULL, NULL, NULL, 0);
				break;
			case 2:	/* old ignore changed */
				EMIT_SIGNAL (XP_TE_IGNORECHANGE, sess, mask, NULL, NULL, NULL, 0);
			}
			return TRUE;
		}
		if (!g_ascii_strcasecmp (word[i], "UNIGNORE"))
			type |= IG_UNIG;
		else if (!g_ascii_strcasecmp (word[i], "ALL"))
			type |= IG_PRIV | IG_NOTI | IG_CHAN | IG_CTCP | IG_INVI | IG_DCC;
		else if (!g_ascii_strcasecmp (word[i], "PRIV"))
			type |= IG_PRIV;
		else if (!g_ascii_strcasecmp (word[i], "NOTI"))
			type |= IG_NOTI;
		else if (!g_ascii_strcasecmp (word[i], "CHAN"))
			type |= IG_CHAN;
		else if (!g_ascii_strcasecmp (word[i], "CTCP"))
			type |= IG_CTCP;
		else if (!g_ascii_strcasecmp (word[i], "INVI"))
			type |= IG_INVI;
		else if (!g_ascii_strcasecmp (word[i], "QUIET"))
			quiet = 1;
		else if (!g_ascii_strcasecmp (word[i], "NOSAVE"))
			type |= IG_NOSAVE;
		else if (!g_ascii_strcasecmp (word[i], "DCC"))
			type |= IG_DCC;
		else
		{
			sprintf (tbuf, _("Unknown arg '%s' ignored."), word[i]);
			PrintText (sess, tbuf);
		}
		i++;
	}
}

static int
cmd_invite (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (!*word[2])
		return FALSE;
	if (*word[3])
		sess->server->p_invite (sess->server, word[3], word[2]);
	else
		sess->server->p_invite (sess->server, sess->channel, word[2]);
	return TRUE;
}

static int
cmd_join (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *chan = word[2];
	session *sess_find;
	if (*chan)
	{
		char *po, *pass = word[3];

		sess_find = find_channel (sess->server, chan);
		if (!sess_find)
		{
			sess->server->p_join (sess->server, chan, pass);
			if (sess->channel[0] == 0 && sess->waitchannel[0])
			{
				po = strchr (chan, ',');
				if (po)
					*po = 0;
				safe_strcpy (sess->waitchannel, chan, CHANLEN);
			}
		}
		else
			fe_ctrl_gui (sess_find, 2, 0);	/* bring-to-front */
		
		return TRUE;
	}
	return FALSE;
}

static int
cmd_kick (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *nick = word[2];
	char *reason = word_eol[3];
	if (*nick)
	{
		sess->server->p_kick (sess->server, sess->channel, nick, reason);
		return TRUE;
	}
	return FALSE;
}

static int
cmd_kickban (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *nick = word[2];
	char *reason = word_eol[3];
	struct User *user;

	if (*nick)
	{
		/* if the reason is a 1 digit number, treat it as a bantype */

		user = userlist_find (sess, nick);

		if (isdigit ((unsigned char) reason[0]) && reason[1] == 0)
		{
			ban (sess, tbuf, nick, reason, (user && user->op));
			reason[0] = 0;
		} else
			ban (sess, tbuf, nick, "", (user && user->op));

		sess->server->p_kick (sess->server, sess->channel, nick, reason);

		return TRUE;
	}
	return FALSE;
}

static int
cmd_killall (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	poxchat_exit();
	return 2;
}

static int
cmd_lagcheck (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	lag_check ();
	return TRUE;
}

static void
lastlog (session *sess, char *search, gtk_xtext_search_flags flags)
{
	session *lastlog_sess;

	if (!is_session (sess))
		return;

	lastlog_sess = find_dialog (sess->server, "(lastlog)");
	if (!lastlog_sess)
		lastlog_sess = new_ircwindow (sess->server, "(lastlog)", SESS_DIALOG, 0);

	lastlog_sess->lastlog_sess = sess;
	lastlog_sess->lastlog_flags = flags;

	fe_text_clear (lastlog_sess, 0);
	fe_lastlog (sess, lastlog_sess, search, flags);
}

static int
cmd_lastlog (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int j = 2;
	gtk_xtext_search_flags flags = 0;
	gboolean doublehyphen = FALSE;

	while (word_eol[j] != NULL && word_eol [j][0] == '-' && !doublehyphen)
	{
		switch (word_eol [j][1])
		{
			case 'r':
				flags |= regexp;
				break;
			case 'm':
				flags |= case_match;
				break;
			case 'h':
				flags |= highlight;
				break;
			case '-':
				doublehyphen = TRUE;
				break;
			default:
				break;
				/* O dear whatever shall we do here? */
		}
		j++;
	}
	if (word_eol[j] != NULL && *word_eol[j])
	{
		lastlog (sess, word_eol[j], flags);
		return TRUE;
	}
	else
	{	
		return FALSE;
	}
}

static int
cmd_list (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	fe_open_chan_list (sess->server, word_eol[2], TRUE);

	return TRUE;
}

gboolean
load_perform_file (session *sess, char *file)
{
	char tbuf[1024 + 4];
	char *nl;
	FILE *fp;

	fp = poxchat_fopen_file (file, "r", 0);		/* load files from config dir */
	if (!fp)
		return FALSE;

	tbuf[1024] = 0;
	while (fgets (tbuf, 1024, fp))
	{
		nl = strchr (tbuf, '\n');
		if (nl == tbuf) /* skip empty commands */
			continue;
		if (nl)
			*nl = 0;
		if (tbuf[0] == prefs.hex_input_command_char[0])
			handle_command (sess, tbuf + 1, TRUE);
		else
			handle_command (sess, tbuf, TRUE);
	}
	fclose (fp);
	return TRUE;
}

static int
cmd_load (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *file, *buf;
#ifdef USE_PLUGIN
	char *error, *arg;
#endif

	if (!word[2][0])
		return FALSE;

	if (strcmp (word[2], "-e") == 0)
	{
		file = expand_homedir (word[3]);
		if (!load_perform_file (sess, file))
		{
			buf = g_strdup_printf ("%s%c%s", get_xdir(), G_DIR_SEPARATOR, file);
			PrintTextf (sess, _("Cannot access %s\n"), buf);
			PrintText (sess, errorstring (errno));
			g_free (buf);
		}
		g_free (file);
		return TRUE;
	}

#ifdef USE_PLUGIN
	if (g_str_has_suffix (word[2], "."PLUGIN_SUFFIX))
	{
		arg = NULL;
		if (word_eol[3][0])
			arg = word_eol[3];

		file = expand_homedir (word[2]);
		error = plugin_load (sess, file, arg);
		g_free (file);

		if (error)
			PrintText (sess, error);

		return TRUE;
	}

	sprintf (tbuf, "Unknown file type %s. Maybe you need to install the Perl or Python plugin?\n", word[2]);
	PrintText (sess, tbuf);
#endif

	return FALSE;
}

char *
split_up_text(struct session *sess, char *text, int cmd_length, char *split_text)
{
	unsigned int max, space_offset;
	char *space;

	/* maximum allowed text */
	/* :nickname!username@host.com cmd_length */
	max = 512; /* rfc 2812 */
	max -= 3; /* :, !, @ */
	max -= cmd_length;
	max -= strlen (sess->server->nick);
	max -= strlen (sess->channel);
	if (sess->me && sess->me->hostname)
		max -= strlen (sess->me->hostname);
	else
	{
		max -= 9;	/* username */
		max -= 65;	/* max possible hostname and '@' */
	}

	if (strlen (text) > max)
	{
		unsigned int i = 0;
		int size;

		/* traverse the utf8 string and find the nearest cut point that
			doesn't split 1 char in half */
		while (1)
		{
			size = g_utf8_skip[((unsigned char *)text)[i]];
			if ((i + size) >= max)
				break;
			i += size;
		}
		max = i;

		/* Try splitting at last space */
		space = g_utf8_strrchr (text, max, ' ');
		if (space)
		{
			space_offset = g_utf8_pointer_to_offset (text, space);

			/* Only split if last word is of sane length */
			if (max != space_offset && max - space_offset < 20)
				max = space_offset + 1;
		}

		split_text = g_strdup_printf ("%.*s", max, text);

		return split_text;
	}

	return NULL;
}

static int
cmd_me (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *act = word_eol[2];
	char *split_text = NULL;
	int cmd_length = 22; /* " PRIVMSG ", " ", :, \001ACTION, " ", \001, \r, \n */
	int offset = 0;
	message_tags_data no_tags = MESSAGE_TAGS_DATA_INIT;
	/* See matching block in handle_say. */
	char *captured_reply_msgid = (sess->reply_msgid ? g_strdup (sess->reply_msgid) : NULL);
	GHashTable *self_reply_tags = NULL;
	message_tags_data reply_tags = MESSAGE_TAGS_DATA_INIT;
	message_tags_data *self_tags = &no_tags;
	int ret = TRUE;

	if (captured_reply_msgid)
	{
		self_reply_tags = g_hash_table_new (g_str_hash, g_str_equal);
		g_hash_table_insert (self_reply_tags, (gpointer)"+draft/reply",
		                     captured_reply_msgid);
		reply_tags.all_tags = self_reply_tags;
		self_tags = &reply_tags;
	}

	if (!(*act))
	{
		ret = FALSE;
		goto out;
	}

	if (sess->type == SESS_SERVER)
	{
		notj_msg (sess);
		goto out;
	}

	g_snprintf (tbuf, TBUFSIZE, "\001ACTION %s\001\r", act);
	/* first try through DCC CHAT */
	if (dcc_write_chat (sess->channel, tbuf))
	{
		/* print it to screen */
		inbound_action (sess, sess->channel, sess->server->nick, "", act, TRUE, FALSE,
							 self_tags);
	} else
	{
		/* DCC CHAT failed, try through server */
		if (sess->server->connected)
		{
			while ((split_text = split_up_text (sess, act + offset, cmd_length, split_text)))
			{
				sess->server->p_action (sess->server, sess->channel, split_text);
				if (sess->server->have_echo_message && sess->server->have_labeled_response)
				{
					g_free (sess->current_msgid);
					sess->current_msgid = g_strdup_printf ("pending:%s", sess->server->last_sent_label);
					inbound_action (sess, sess->channel, sess->server->nick, "",
										 split_text, TRUE, FALSE, self_tags);
					mark_pending_echo (sess, sess->server);
				}
				else if (!sess->server->have_echo_message)
				{
					inbound_action (sess, sess->channel, sess->server->nick, "",
										 split_text, TRUE, FALSE, self_tags);
				}

				if (*split_text)
					offset += strlen(split_text);

				g_free (split_text);
			}

			sess->server->p_action (sess->server, sess->channel, act + offset);
			if (sess->server->have_echo_message && sess->server->have_labeled_response)
			{
				g_free (sess->current_msgid);
				sess->current_msgid = g_strdup_printf ("pending:%s", sess->server->last_sent_label);
				inbound_action (sess, sess->channel, sess->server->nick, "",
									 act + offset, TRUE, FALSE, self_tags);
				mark_pending_echo (sess, sess->server);
			}
			else if (!sess->server->have_echo_message)
			{
				inbound_action (sess, sess->channel, sess->server->nick, "",
									 act + offset, TRUE, FALSE, self_tags);
			}
		} else
		{
			notc_msg (sess);
		}
	}

out:
	if (self_reply_tags)
		g_hash_table_destroy (self_reply_tags);
	g_free (captured_reply_msgid);
	return ret;
}

static int
cmd_metadata (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	server *serv = sess->server;
	char *target = word[2];
	char *subcmd = word[3];

	if (!serv->have_metadata)
	{
		PrintText (sess, "This server does not support METADATA.\n");
		return TRUE;
	}

	if (!*target)
	{
		PrintText (sess, "Usage: /METADATA <target> GET <key> [<key>...] - Get metadata\n");
		PrintText (sess, "       /METADATA <target> LIST - List all metadata\n");
		PrintText (sess, "       /METADATA <target> SET <key> [:<value>] - Set metadata\n");
		PrintText (sess, "       /METADATA <target> CLEAR - Clear all metadata\n");
		PrintText (sess, "       /METADATA * SUB <key> [<key>...] - Subscribe to updates\n");
		PrintText (sess, "       /METADATA * UNSUB <key> [<key>...] - Unsubscribe\n");
		PrintText (sess, "       /METADATA * SUBS - List subscriptions\n");
		PrintText (sess, "       /METADATA <target> SYNC - Sync metadata\n");
		PrintText (sess, "Target can be a nick, channel, or * for subscriptions.\n");
		return TRUE;
	}

	if (!*subcmd)
	{
		PrintText (sess, "Missing subcommand. Use /METADATA for help.\n");
		return TRUE;
	}

	/* Build and send METADATA command */
	if (!g_ascii_strcasecmp (subcmd, "GET") || !g_ascii_strcasecmp (subcmd, "LIST") ||
	    !g_ascii_strcasecmp (subcmd, "CLEAR") || !g_ascii_strcasecmp (subcmd, "SYNC") ||
	    !g_ascii_strcasecmp (subcmd, "SUBS"))
	{
		/* Commands with optional/no extra args */
		if (word[4][0])
			tcp_sendf_labeled_tracked (serv, "METADATA", target,
			                           "METADATA %s %s %s\r\n", target, subcmd, word_eol[4]);
		else
			tcp_sendf_labeled_tracked (serv, "METADATA", target,
			                           "METADATA %s %s\r\n", target, subcmd);
	}
	else if (!g_ascii_strcasecmp (subcmd, "SET"))
	{
		/* SET requires key and optional value */
		char *key = word[4];
		char *value = word_eol[5];
		if (!*key)
		{
			PrintText (sess, "Usage: /METADATA <target> SET <key> [:<value>]\n");
			return TRUE;
		}
		if (*value)
			tcp_sendf_labeled_tracked (serv, "METADATA", target,
			                           "METADATA %s SET %s :%s\r\n", target, key, value);
		else
			tcp_sendf_labeled_tracked (serv, "METADATA", target,
			                           "METADATA %s SET %s\r\n", target, key);
	}
	else if (!g_ascii_strcasecmp (subcmd, "SUB") || !g_ascii_strcasecmp (subcmd, "UNSUB"))
	{
		/* SUB/UNSUB require keys */
		if (!word[4][0])
		{
			PrintTextf (sess, "Usage: /METADATA * %s <key> [<key>...]\n", subcmd);
			return TRUE;
		}
		tcp_sendf_labeled_tracked (serv, "METADATA", target,
		                           "METADATA %s %s %s\r\n", target, subcmd, word_eol[4]);
	}
	else
	{
		PrintText (sess, "Unknown METADATA subcommand. Use /METADATA for help.\n");
	}

	return TRUE;
}

static int
cmd_mode (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	/* We allow omitting the target, so we have to figure it out:
	 * - Can only use info from channels or dialogs
	 * - Empty arg is always sess info
	 * - Assume + is mode not channel
	 * - We know valid channels and our nick
	 * - We cannot easily know if other nick or valid mode (Need to store 004)
	 */
	if ((sess->type != SESS_CHANNEL && sess->type != SESS_DIALOG)
	    || (!(*word[2] == '-' || *word[2] == '+' || *word[2] == '\0')
	        && (is_channel (sess->server, word[2]) || !rfc_casecmp (sess->server->nick, word[2])))
	   )
	{
		sess->server->p_mode (sess->server, word[2], word_eol[3]);
	}
	else
	{
		if(sess->channel[0] == 0)
			return FALSE;
		sess->server->p_mode (sess->server, sess->channel, word_eol[2]);
	}
	return TRUE;
}

static int
mop_cb (struct User *user, multidata *data)
{
	if (!user->op)
	{
		data->nicks[data->i] = user->nick;
		data->i++;
	}
	return TRUE;
}

static int
cmd_mop (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char **nicks = g_new0 (char *, sess->total - sess->ops);
	multidata data;

	data.nicks = nicks;
	data.i = 0;
	tree_foreach (sess->usertree, (tree_traverse_func *)mop_cb, &data);
	send_channel_modes (sess, tbuf, nicks, 0, data.i, '+', 'o', 0);

	g_free (nicks);

	return TRUE;
}

static int
cmd_monitor (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	server *serv = sess->server;
	char *subcmd = word[2];

	if (!serv->supports_monitor)
	{
		PrintText (sess, "This server does not support MONITOR.\n");
		return TRUE;
	}

	if (!*subcmd)
	{
		PrintText (sess, "Usage: /MONITOR +nick[,nick2,...] - Add nicks to monitor list\n");
		PrintText (sess, "       /MONITOR -nick[,nick2,...] - Remove nicks from monitor list\n");
		PrintText (sess, "       /MONITOR C - Clear monitor list\n");
		PrintText (sess, "       /MONITOR L - List monitored nicks\n");
		PrintText (sess, "       /MONITOR S - Show status of monitored nicks\n");
		return TRUE;
	}

	/* Check for single-letter commands */
	if ((subcmd[0] == 'C' || subcmd[0] == 'c' ||
	     subcmd[0] == 'L' || subcmd[0] == 'l' ||
	     subcmd[0] == 'S' || subcmd[0] == 's') && subcmd[1] == '\0')
	{
		tcp_sendf_labeled_tracked (serv, "MONITOR", NULL,
		                           "MONITOR %c\r\n", g_ascii_toupper (subcmd[0]));
		return TRUE;
	}

	/* +/-nick commands */
	if (subcmd[0] == '+' || subcmd[0] == '-')
	{
		tcp_sendf_labeled_tracked (serv, "MONITOR", NULL,
		                           "MONITOR %s\r\n", subcmd);
		return TRUE;
	}

	/* Invalid syntax */
	PrintText (sess, "Invalid MONITOR command. Use /MONITOR for help.\n");
	return TRUE;
}

static int
cmd_msg (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *nick = word[2];
	char *msg = word_eol[3];
	struct session *newsess;
	char *split_text = NULL;
	int cmd_length = 13; /* " PRIVMSG ", " ", :, \r, \n */
	int offset = 0;

	if (*nick)
	{
		if (*msg)
		{
			if (strcmp (nick, ".") == 0)
			{							  /* /msg the last nick /msg'ed */
				if (sess->lastnick[0])
					nick = sess->lastnick;
			} else
			{
				safe_strcpy (sess->lastnick, nick, NICKLEN);	/* prime the last nick memory */
			}

			if (*nick == '=')
			{
				nick++;
				if (!dcc_write_chat (nick, msg))
				{
					EMIT_SIGNAL (XP_TE_NODCC, sess, NULL, NULL, NULL, NULL, 0);
					return TRUE;
				}
			} else
			{
				if (!sess->server->connected)
				{
					notc_msg (sess);
					return TRUE;
				}

				if (sess->server->have_echo_message && sess->server->have_labeled_response)
				{
					/* Tier 2: interleave send+display+mark for correct label association */
					message_tags_data no_tags = MESSAGE_TAGS_DATA_INIT;
					session *target_sess = find_dialog (sess->server, nick);
					if (!target_sess)
						target_sess = find_channel (sess->server, nick);

					while ((split_text = split_up_text (sess, msg + offset, cmd_length, split_text)))
					{
						sess->server->p_message (sess->server, nick, split_text);
						if (target_sess)
						{
							inbound_chanmsg (target_sess->server, NULL, target_sess->channel,
							                 target_sess->server->nick, split_text, TRUE, FALSE,
							                 &no_tags);
							mark_pending_echo (target_sess, sess->server);
						}
						else
						{
							EMIT_SIGNAL (XP_TE_MSGSEND, sess, nick, split_text, NULL, NULL, 0);
							mark_pending_echo (sess, sess->server);
						}

						if (*split_text)
							offset += strlen(split_text);

						g_free (split_text);
					}
					sess->server->p_message (sess->server, nick, msg + offset);
					if (target_sess)
					{
						inbound_chanmsg (target_sess->server, NULL, target_sess->channel,
						                 target_sess->server->nick, msg + offset, TRUE, FALSE,
						                 &no_tags);
						mark_pending_echo (target_sess, sess->server);
					}
					else
					{
						char *dm = msg + offset;
						if (g_ascii_strcasecmp (nick, "nickserv") == 0)
						{
							if (g_ascii_strncasecmp (dm, "identify ", 9) == 0)
								dm = "identify ****";
							else if (g_ascii_strncasecmp (dm, "ghost ", 6) == 0)
								dm = "ghost ****";
						}
						EMIT_SIGNAL (XP_TE_MSGSEND, sess, nick, dm, NULL, NULL, 0);
						mark_pending_echo (sess, sess->server);
					}
				}
				else
				{
					while ((split_text = split_up_text (sess, msg + offset, cmd_length, split_text)))
					{
						sess->server->p_message (sess->server, nick, split_text);

						if (*split_text)
							offset += strlen(split_text);

						g_free (split_text);
					}
					sess->server->p_message (sess->server, nick, msg + offset);
					offset = 0;
				}
			}
			if (!sess->server->have_echo_message)
			{
				newsess = find_dialog (sess->server, nick);
				if (!newsess)
					newsess = find_channel (sess->server, nick);

				if (newsess)
				{
					message_tags_data no_tags = MESSAGE_TAGS_DATA_INIT;

					while ((split_text = split_up_text (sess, msg + offset, cmd_length, split_text)))
					{
						inbound_chanmsg (newsess->server, NULL, newsess->channel,
											  newsess->server->nick, split_text, TRUE, FALSE,
											  &no_tags);

						if (*split_text)
							offset += strlen(split_text);

						g_free (split_text);
					}
					inbound_chanmsg (newsess->server, NULL, newsess->channel,
										  newsess->server->nick, msg + offset, TRUE, FALSE,
										  &no_tags);
				}
				else
				{
					/* mask out passwords */
					if (g_ascii_strcasecmp (nick, "nickserv") == 0)
					{
						if (g_ascii_strncasecmp (msg, "identify ", 9) == 0)
							msg = "identify ****";
						else if (g_ascii_strncasecmp (msg, "ghost ", 6) == 0)
							msg = "ghost ****";
					}

					EMIT_SIGNAL (XP_TE_MSGSEND, sess, nick, msg, NULL, NULL, 0);
				}
			}

			return TRUE;
		}
	}
	return FALSE;
}

static int
cmd_names (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (*word[2])
	  	sess->server->p_names (sess->server, word[2]);
	else
		sess->server->p_names (sess->server, sess->channel);
	return TRUE;
}

static int
cmd_nctcp (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (*word_eol[3])
	{
		sess->server->p_nctcp (sess->server, word[2], word_eol[3]);
		return TRUE;
	}
	return FALSE;
}

static int
cmd_newserver (struct session *sess, char *tbuf, char *word[],
					char *word_eol[])
{
	if (strcmp (word[2], "-noconnect") == 0)
	{
		new_ircwindow (NULL, word[3], SESS_SERVER, 0);
		return TRUE;
	}
	
	sess = new_ircwindow (NULL, NULL, SESS_SERVER, 1);
	cmd_server (sess, tbuf, word, word_eol);
	return TRUE;
}

static int
cmd_nick (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *nick = word[2];
	if (*nick)
	{
		if (sess->server->connected)
			sess->server->p_change_nick (sess->server, nick);
		else
		{
			message_tags_data no_tags = MESSAGE_TAGS_DATA_INIT;
			inbound_newnick (sess->server, sess->server->nick, nick, TRUE,
								  &no_tags);
		}
		return TRUE;
	}
	return FALSE;
}

static int
cmd_notice (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *text = word_eol[3];
	char *split_text = NULL;
	int cmd_length = 12; /* " NOTICE ", " ", :, \r, \n */
	int offset = 0;

	if (*word[2] && *word_eol[3])
	{
		while ((split_text = split_up_text (sess, text + offset, cmd_length, split_text)))
		{
			sess->server->p_notice (sess->server, word[2], split_text);
			if (sess->server->have_echo_message && sess->server->have_labeled_response)
			{
				EMIT_SIGNAL (XP_TE_NOTICESEND, sess, word[2], split_text, NULL, NULL, 0);
				mark_pending_echo (sess, sess->server);
			}
			else if (!sess->server->have_echo_message)
			{
				EMIT_SIGNAL (XP_TE_NOTICESEND, sess, word[2], split_text, NULL, NULL, 0);
			}

			if (*split_text)
				offset += strlen(split_text);

			g_free (split_text);
		}

		sess->server->p_notice (sess->server, word[2], text + offset);
		if (sess->server->have_echo_message && sess->server->have_labeled_response)
		{
			EMIT_SIGNAL (XP_TE_NOTICESEND, sess, word[2], text + offset, NULL, NULL, 0);
			mark_pending_echo (sess, sess->server);
		}
		else if (!sess->server->have_echo_message)
		{
			EMIT_SIGNAL (XP_TE_NOTICESEND, sess, word[2], text + offset, NULL, NULL, 0);
		}

		return TRUE;
	}
	return FALSE;
}

static int
cmd_notify (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int i = 1;
	char *net = NULL;

	if (*word[2])
	{
		if (strcmp (word[2], "-n") == 0)	/* comma sep network list */
		{
			net = word[3];
			i += 2;
		}

		while (1)
		{
			i++;
			if (!*word[i])
				break;
			if (notify_deluser (word[i]))
			{
				EMIT_SIGNAL (XP_TE_DELNOTIFY, sess, word[i], NULL, NULL, NULL, 0);
				return TRUE;
			}

			if (net && strcmp (net, "ASK") == 0)
				fe_notify_ask (word[i], NULL);
			else
			{
				notify_adduser (word[i], NULL, net);
				EMIT_SIGNAL (XP_TE_ADDNOTIFY, sess, word[i], NULL, NULL, NULL, 0);
			}
		}
	} else
	{
		message_tags_data no_tags = MESSAGE_TAGS_DATA_INIT;
		notify_showlist (sess, &no_tags);
	}
	return TRUE;
}

static int
cmd_op (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int i = 2;

	while (1)
	{
		if (!*word[i])
		{
			if (i == 2)
				return FALSE;
			send_channel_modes (sess, tbuf, word, 2, i, '+', 'o', 0);
			return TRUE;
		}
		i++;
	}
}

static int
cmd_part (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *chan = word[2];
	char *reason = word_eol[3];
	if (!*chan)
		chan = sess->channel;
	if ((*chan) && is_channel (sess->server, chan))
	{
		if (reason[0] == 0)
			reason = NULL;
		server_sendpart (sess->server, chan, reason);
		return TRUE;
	}
	return FALSE;
}

static int
cmd_ping (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char timestring[64];
	unsigned long tim;
	char *to = word[2];

	tim = make_ping_time ();

	g_snprintf (timestring, sizeof (timestring), "%lu", tim);
	sess->server->p_ping (sess->server, to, timestring);

	return TRUE;
}

session *
open_query (server *serv, char *nick, gboolean focus_existing)
{
	session *sess;

	sess = find_dialog (serv, nick);
	if (!sess)
		sess = new_ircwindow (serv, nick, SESS_DIALOG, focus_existing);
	else if (focus_existing)
		fe_ctrl_gui (sess, 2, 0);	/* bring-to-front */

	return sess;
}

static int
cmd_query (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *nick = word[2];
	char *msg = word_eol[3];
	char *split_text = NULL;
	gboolean focus = TRUE;
	int cmd_length = 13; /* " PRIVMSG ", " ", :, \r, \n */
	int offset = 0;

	if (strcmp (word[2], "-nofocus") == 0)
	{
		nick = word[3];
		msg = word_eol[4];
		focus = FALSE;
	}

	if (*nick && !is_channel (sess->server, nick))
	{
		struct session *nick_sess;

		nick_sess = open_query (sess->server, nick, focus);

		if (*msg)
		{
			message_tags_data no_tags = MESSAGE_TAGS_DATA_INIT;

			if (!sess->server->connected)
			{
				notc_msg (sess);
				return TRUE;
			}

			while ((split_text = split_up_text (sess, msg + offset, cmd_length, split_text)))
			{
				sess->server->p_message (sess->server, nick, split_text);
				if (sess->server->have_echo_message && sess->server->have_labeled_response)
				{
					inbound_chanmsg (nick_sess->server, nick_sess, nick_sess->channel,
									 nick_sess->server->nick, split_text, TRUE, FALSE,
									 &no_tags);
					mark_pending_echo (nick_sess, sess->server);
				}
				else if (!sess->server->have_echo_message)
				{
					inbound_chanmsg (nick_sess->server, nick_sess, nick_sess->channel,
									 nick_sess->server->nick, split_text, TRUE, FALSE,
									 &no_tags);
				}

				if (*split_text)
					offset += strlen(split_text);

				g_free (split_text);
			}
			sess->server->p_message (sess->server, nick, msg + offset);
			if (sess->server->have_echo_message && sess->server->have_labeled_response)
			{
				inbound_chanmsg (nick_sess->server, nick_sess, nick_sess->channel,
								 nick_sess->server->nick, msg + offset, TRUE, FALSE,
								 &no_tags);
				mark_pending_echo (nick_sess, sess->server);
			}
			else if (!sess->server->have_echo_message)
			{
				inbound_chanmsg (nick_sess->server, nick_sess, nick_sess->channel,
								 nick_sess->server->nick, msg + offset, TRUE, FALSE,
								 &no_tags);
			}
		}

		return TRUE;
	}
	return FALSE;
}

static int
cmd_quiet (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *quietmask;
	server *serv = sess->server;

	if (strchr (serv->chanmodes, 'q') == NULL)
	{
		PrintText (sess, _("Quiet is not supported by this server."));
		return TRUE;
	}

	if (*word[2])
	{
		quietmask = create_mask (sess, word[2], "+q", word[3], 0);
	
		if (quietmask)
		{
			serv->p_mode (serv, sess->channel, quietmask);
			g_free (quietmask);
		}
	}
	else
	{
		serv->p_mode (serv, sess->channel, "+q");	/* quietlist */
	}

	return TRUE;
}

static int
cmd_unquiet (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	/* Allow more than one mask in /unban -- tvk */
	int i = 2;
	
	if (strchr (sess->server->chanmodes, 'q') == NULL)
	{
		PrintText (sess, _("Quiet is not supported by this server."));
		return TRUE;
	}

	while (1)
	{
		if (!*word[i])
		{
			if (i == 2)
				return FALSE;
			send_channel_modes (sess, tbuf, word, 2, i, '-', 'q', 0);
			return TRUE;
		}
		i++;
	}
}

static int
cmd_quote (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *raw = word_eol[2];

	return sess->server->p_raw (sess->server, raw);
}

static int
cmd_reconnect (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int tmp = prefs.hex_net_reconnect_delay;
	GSList *list;
	server *serv = sess->server;

	prefs.hex_net_reconnect_delay = 0;

	if (!g_ascii_strcasecmp (word[2], "ALL"))
	{
		list = serv_list;
		while (list)
		{
			serv = list->data;
			if (serv->connected)
				serv->auto_reconnect (serv, TRUE, -1);
			list = list->next;
		}
	}
	/* If it isn't "ALL" and there is something
	there it *should* be a server they are trying to connect to*/
	else if (*word[2])
	{
		int offset = 0;

#ifdef USE_OPENSSL
		int use_ssl = TRUE;
		int use_ssl_noverify = FALSE;
		if (g_strcmp0 (word[2], "-ssl") == 0)
		{
			use_ssl = TRUE;
			use_ssl_noverify = FALSE;
			offset++;	/* args move up by 1 word */
		} else if (g_strcmp0 (word[2], "-ssl-noverify") == 0)
		{
			use_ssl = TRUE;
			use_ssl_noverify = TRUE;
			offset++;	/* args move up by 1 word */
		} else if (g_strcmp0 (word[2], "-insecure") == 0)
		{
			use_ssl = FALSE;
			use_ssl_noverify = FALSE;
			offset++;	/* args move up by 1 word */
		}
		serv->use_ssl = use_ssl;
		serv->accept_invalid_cert = use_ssl_noverify;
#endif

		if (*word[4+offset])
			safe_strcpy (serv->password, word[4+offset], sizeof (serv->password));
		if (*word[3+offset])
			serv->port = atoi (word[3+offset]);
		safe_strcpy (serv->hostname, word[2+offset], sizeof (serv->hostname));
		serv->auto_reconnect (serv, TRUE, -1);
	}
	else
	{
		serv->auto_reconnect (serv, TRUE, -1);
	}
	prefs.hex_net_reconnect_delay = tmp;

	return TRUE;
}

static int
cmd_recv (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (*word_eol[2])
	{
		sess->server->p_inline (sess->server, word_eol[2], strlen (word_eol[2]));
		return TRUE;
	}

	return FALSE;
}

static int
cmd_register (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	server *serv = sess->server;
	char *account = word[2];
	char *email = word[3];
	char *password = word[4];

	if (!serv->have_account_registration)
	{
		PrintText (sess, "This server does not support account registration.\n");
		return TRUE;
	}

	/* Check for arguments */
	if (!*account)
	{
		PrintText (sess, "Usage: /REGISTER <account> [<email>|*] <password>\n");
		if (serv->accreg_email_required)
			PrintText (sess, "Note: This server requires an email address.\n");
		if (!serv->accreg_custom_account)
			PrintText (sess, "Note: Account name must match your current nickname (use * for account).\n");
		return TRUE;
	}

	/* If only account given, prompt for more */
	if (!*email || !*password)
	{
		if (!*email)
		{
			PrintText (sess, "Usage: /REGISTER <account> [<email>|*] <password>\n");
			return TRUE;
		}
		/* email given but not password - assume email is actually password and use * for email */
		password = email;
		email = "*";
	}

	/* Validate email requirement */
	if (serv->accreg_email_required && (!email || !strcmp (email, "*")))
	{
		PrintText (sess, "Error: This server requires a valid email address.\n");
		return TRUE;
	}

	/* Validate custom-account-name: if server doesn't support it, account must be * or match nick */
	if (!serv->accreg_custom_account && strcmp (account, "*") != 0)
	{
		if (serv->p_cmp (account, serv->nick) != 0)
		{
			PrintTextf (sess, "Error: This server requires account name to match your nickname.\n");
			PrintTextf (sess, "Use '*' for account or change your nick to '%s' first.\n", account);
			return TRUE;
		}
	}

	/* Send REGISTER command */
	tcp_sendf_labeled_tracked (serv, "REGISTER", NULL,
	                           "REGISTER %s %s %s\r\n", account, email, password);
	PrintTextf (sess, "Registering account '%s'...\n",
		strcmp (account, "*") == 0 ? serv->nick : account);

	return TRUE;
}

static int
cmd_verify (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	server *serv = sess->server;
	char *account = word[2];
	char *code = word[3];

	if (!serv->have_account_registration)
	{
		PrintText (sess, "This server does not support account registration.\n");
		return TRUE;
	}

	if (!*account || !*code)
	{
		PrintText (sess, "Usage: /VERIFY <account> <code>\n");
		PrintText (sess, "Use this command to complete account registration with the verification code.\n");
		return TRUE;
	}

	/* Send VERIFY command */
	tcp_sendf_labeled_tracked (serv, "VERIFY", NULL,
	                           "VERIFY %s %s\r\n", account, code);
	PrintTextf (sess, "Verifying account '%s'...\n", account);

	return TRUE;
}

static int
cmd_say (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	char *speech = word_eol[2];
	if (*speech)
	{
		handle_say (sess, speech, FALSE);
		return TRUE;
	}
	return FALSE;
}

static int
cmd_send (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	guint32 addr;
	socklen_t len;
	struct sockaddr_in SAddr;

	if (!word[2][0])
		return FALSE;

	addr = dcc_get_my_address (sess);
	if (addr == 0)
	{
		/* use the one from our connected server socket */
		memset (&SAddr, 0, sizeof (struct sockaddr_in));
		len = sizeof (SAddr);
		getsockname (sess->server->sok, (struct sockaddr *) &SAddr, &len);
		addr = SAddr.sin_addr.s_addr;
	}
	addr = ntohl (addr);

	if ((addr & 0xffff0000) == 0xc0a80000 ||	/* 192.168.x.x */
		 (addr & 0xff000000) == 0x0a000000)		/* 10.x.x.x */
		/* we got a private net address, let's PSEND or it'll fail */
		g_snprintf (tbuf, 512, "DCC PSEND %s", word_eol[2]);
	else
		g_snprintf (tbuf, 512, "DCC SEND %s", word_eol[2]);

	handle_command (sess, tbuf, FALSE);

	return TRUE;
}

static int
cmd_setcursor (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int delta = FALSE;

	if (*word[2])
	{
		if (word[2][0] == '-' || word[2][0] == '+')
			delta = TRUE;
		fe_set_inputbox_cursor (sess, delta, atoi (word[2]));
		return TRUE;
	}

	return FALSE;
}

static int
cmd_settab (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (*word_eol[2])
	{
		strcpy (tbuf, sess->channel);
		safe_strcpy (sess->channel, word_eol[2], CHANLEN);
		fe_set_channel (sess);
		strcpy (sess->channel, tbuf);
	}

	return TRUE;
}

static int
cmd_settext (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	fe_set_inputbox_contents (sess, word_eol[2]);
	return TRUE;
}

static int
cmd_splay (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (*word[2])
	{
		sound_play (word[2], FALSE);
		return TRUE;
	}

	return FALSE;
}

static int
parse_irc_url (char *url, char *server_name[], char *port[], char *channel[], char *key[], int *use_ssl)
{
	char *co;
#ifdef USE_OPENSSL
	if (g_ascii_strncasecmp ("ircs://", url, 7) == 0)
	{
		*use_ssl = TRUE;
		*server_name = url + 7;
		goto urlserv;
	}
#endif

	if (g_ascii_strncasecmp ("irc://", url, 6) == 0)
	{
		*server_name = url + 6;
#ifdef USE_OPENSSL
urlserv:
#endif
		/* check for port */
		co = strchr (*server_name, ':');
		if (co)
		{
			*port = co + 1;
			*co = 0;
		} else
			co = *server_name;
		/* check for channel - mirc style */
		co = strchr (co + 1, '/');
		if (co)
		{
			*co = 0;
			co++;
			if (*co == '#')
				*channel = co+1;
			else if (*co != '\0')
				*channel = co;
				
			/* check for key - mirc style */
			co = strchr (co + 1, '?');
			if (co)
			{
				*co = 0;
				co++;
				*key = co;
			}	
		}
			
		return TRUE;
	}
	return FALSE;
}

static int
cmd_server (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int offset = 0;
	char *server_name = NULL;
	char *port = NULL;
	char *pass = NULL;
	char *channel = NULL;
	char *key = NULL;
#ifdef USE_OPENSSL
	int use_ssl = TRUE;
	int use_ssl_noverify = FALSE;
#else
	int use_ssl = FALSE;
#endif
	int is_url = TRUE;
	server *serv = sess->server;
	ircnet *net = NULL;

#ifdef USE_OPENSSL
	/* BitchX uses -ssl, mIRC uses -e, let's support both */
	if (g_strcmp0 (word[2], "-ssl") == 0 || g_strcmp0 (word[2], "-e") == 0)
	{
		use_ssl = TRUE;
		offset++;	/* args move up by 1 word */
	}
	else if (g_strcmp0 (word[2], "-ssl-noverify") == 0)
	{
		use_ssl = TRUE;
		use_ssl_noverify = TRUE;
		offset++;	/* args move up by 1 word */
	}
	else if (g_strcmp0 (word[2], "-insecure") == 0)
	{
		use_ssl = FALSE;
		offset++;	/* args move up by 1 word */
	}
#endif

	if (!parse_irc_url (word[2 + offset], &server_name, &port, &channel, &key, &use_ssl))
	{
		is_url = FALSE;
		server_name = word[2 + offset];
	}
	if (port)
		pass = word[3 + offset];
	else
	{
		port = word[3 + offset];
		pass = word[4 + offset];
	}
	
	if (!(*server_name))
		return FALSE;

	sess->server->network = NULL;

	/* dont clear it for /servchan */
	if (g_ascii_strncasecmp (word_eol[1], "SERVCHAN ", 9))
		sess->willjoinchannel[0] = 0;

	if (channel)
	{
		sess->willjoinchannel[0] = '#';
		safe_strcpy ((sess->willjoinchannel + 1), channel, (CHANLEN - 1));
		if (key)
			safe_strcpy (sess->channelkey, key, 64);
	}

	/* support +7000 style ports like mIRC */
	if (port[0] == '+')
	{
		port++;
#ifdef USE_OPENSSL
		use_ssl = TRUE;
#endif
	}
	else if (port[0] == '-')
	{
		port++;
#ifdef USE_OPENSSL
		use_ssl = FALSE;
#endif
	}

	if (*pass)
	{
		safe_strcpy (serv->password, pass, sizeof (serv->password));
		serv->loginmethod = LOGIN_PASS;
	}
	else
	{
		/* If part of a known network, login like normal */
		net = servlist_net_find_from_server (server_name);
		if (net && net->pass && *net->pass)
		{
			safe_strcpy (serv->password, net->pass, sizeof (serv->password));
			serv->loginmethod = net->logintype;
		}
		else /* Otherwise ensure no password is sent or SASL started */
		{
			serv->loginmethod = LOGIN_DEFAULT;
			serv->password[0] = 0;
		}
	}

#ifdef USE_OPENSSL
	serv->use_ssl = use_ssl;
	serv->accept_invalid_cert = use_ssl_noverify;
#endif

	/* try to connect by Network name */
	if (servlist_connect_by_netname (sess, server_name, !is_url))
		return TRUE;

	if (*port)
	{
		serv->connect (serv, server_name, atoi (port), FALSE);
	} else
	{
		/* -1 for default port */
		serv->connect (serv, server_name, -1, FALSE);
	}

	/* try to associate this connection with a listed network */
	/* may return NULL, but that's OK */
	if ((serv->network = servlist_net_find_from_server (server_name)))
		server_set_encoding (serv, ((ircnet*)serv->network)->encoding);

	return TRUE;
}

static int
cmd_servchan (struct session *sess, char *tbuf, char *word[],
				  char *word_eol[])
{
	int offset = 0;

#ifdef USE_OPENSSL
	if (g_strcmp0 (word[2], "-ssl") == 0 || g_strcmp0 (word[2], "-ssl-noverify") == 0 || g_strcmp0 (word[2], "-insecure") == 0)
		offset++;
#endif

	if (*word[4 + offset])
	{
		safe_strcpy (sess->willjoinchannel, word[4 + offset], CHANLEN);
		return cmd_server (sess, tbuf, word, word_eol);
	}

	return FALSE;
}

static int
cmd_tagmsg (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	server *serv = sess->server;
	char *target;
	char *tags;

	if (!serv->have_message_tags)
	{
		PrintText (sess, "Server does not support message-tags capability.\n");
		return TRUE;
	}

	target = word[2];
	tags = word_eol[3];

	if (!*target || !*tags)
	{
		PrintText (sess, "Usage: /TAGMSG <target> <tags>\n");
		PrintText (sess, "Example: /TAGMSG #channel +typing=active\n");
		PrintText (sess, "Example: /TAGMSG #channel +draft/react=👍;+draft/reply=msgid123\n");
		return TRUE;
	}

	/* Send TAGMSG with the specified tags merged with label into one @ block */
	tcp_sendf_with_raw_tags (serv, "TAGMSG", target, tags,
	                         "TAGMSG %s\r\n", target);

	return TRUE;
}

static int
cmd_topic (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (word[2][0] && is_channel (sess->server, word[2]))
		sess->server->p_topic (sess->server, word[2], word_eol[3]);
	else
		sess->server->p_topic (sess->server, sess->channel, word_eol[2]);
	return TRUE;
}

static int
cmd_toast (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int type = 0;  /* TOAST_TYPE_INFO */
	unsigned int flags = 0;
	int arg = 2;

	/* /TOAST [-nick|-topic|-mode|-join|-error] [-sticky] <text> */
	while (word[arg][0] == '-')
	{
		if (strcmp (word[arg], "-nick") == 0)       type = 1;
		else if (strcmp (word[arg], "-topic") == 0) type = 2;
		else if (strcmp (word[arg], "-mode") == 0)  type = 3;
		else if (strcmp (word[arg], "-join") == 0)   type = 4;
		else if (strcmp (word[arg], "-error") == 0) type = 5;
		else if (strcmp (word[arg], "-sticky") == 0) flags |= 1; /* TOAST_FLAG_STICKY */
		else break;
		arg++;
	}

	if (word_eol[arg][0])
		fe_toast_show (sess, word_eol[arg], 4000, type, flags);
	else
		fe_toast_show (sess, "Test toast notification", 4000, type, flags);
	return TRUE;
}

static int
cmd_tray (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (strcmp (word[2], "-t") == 0)
	{
		fe_tray_set_tooltip (word[3][0] ? word[3] : NULL);
		return TRUE;
	}

	if (strcmp (word[2], "-i") == 0)
	{
		fe_tray_set_icon (atoi (word[3]));
		return TRUE;
	}

	if (strcmp (word[2], "-f") != 0)
		return FALSE;

	if (!word[3][0])
	{
		fe_tray_set_file (NULL);	/* default PoxChat icon */
		return TRUE;
	}

	if (!word[4][0])
	{
		fe_tray_set_file (word[3]);	/* fixed custom icon */
		return TRUE;
	}

	/* flash between 2 icons */
	fe_tray_set_flash (word[4], word[5][0] ? word[5] : NULL, atoi (word[3]));
	return TRUE;
}

static int
cmd_unignore (struct session *sess, char *tbuf, char *word[],
				  char *word_eol[])
{
	char *mask = word[2];
	char *arg = word[3];
	if (*mask)
	{
		if (strchr (mask, '?') == NULL && strchr (mask, '*') == NULL)
		{
			mask = tbuf;
			g_snprintf (tbuf, TBUFSIZE, "%s!*@*", word[2]);
		}
		
		if (ignore_del (mask, NULL))
		{
			if (g_ascii_strcasecmp (arg, "QUIET"))
				EMIT_SIGNAL (XP_TE_IGNOREREMOVE, sess, mask, NULL, NULL, NULL, 0);
		}
		return TRUE;
	}
	return FALSE;
}

static int
cmd_unload (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
#ifdef USE_PLUGIN
	gboolean by_file = FALSE;

	if (g_str_has_suffix (word[2], "."PLUGIN_SUFFIX))
		by_file = TRUE;

	switch (plugin_kill (word[2], by_file))
	{
	case 0:
			PrintText (sess, _("No such plugin found.\n"));
			break;
	case 1:
			return TRUE;
	case 2:
			PrintText (sess, _("That plugin is refusing to unload.\n"));
			break;
	}
#endif

	return FALSE;
}

static int
cmd_reload (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
#ifdef USE_PLUGIN
	gboolean by_file = FALSE;

	if (g_str_has_suffix (word[2], "."PLUGIN_SUFFIX))
		by_file = TRUE;

	switch (plugin_reload (sess, word[2], by_file))
	{
	case 0: /* error */
			PrintText (sess, _("No such plugin found.\n"));
			break;
	case 1: /* success */
			return TRUE;
	case 2: /* fake plugin, we know it exists but scripts should handle it. */
			return TRUE;
	}
#endif

	return FALSE;
}

static server *
find_server_from_hostname (char *hostname)
{
	GSList *list = serv_list;
	server *serv;

	while (list)
	{
		serv = list->data;
		if (!g_ascii_strcasecmp (hostname, serv->hostname) && serv->connected)
			return serv;
		list = list->next;
	}

	return NULL;
}

static server *
find_server_from_net (void *net)
{
	GSList *list = serv_list;
	server *serv;

	while (list)
	{
		serv = list->data;
		if (serv->network == net && serv->connected)
			return serv;
		list = list->next;
	}

	return NULL;
}

static void
url_join_only (server *serv, char *tbuf, char *channel, char *key)
{
	/* already connected, JOIN only. */
	if (channel == NULL)
		return;
	tbuf[0] = '#';
	/* tbuf is 4kb */
	safe_strcpy ((tbuf + 1), channel, 256);
	if (key)
		serv->p_join (serv, tbuf, key);
	else
		serv->p_join (serv, tbuf, "");
}

static int
cmd_url (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	if (word[2][0])
	{
		char *server_name = NULL;
		char *port = NULL;
		char *channel = NULL;
		char *key = NULL;
		char *url = g_strdup (word[2]);
		int use_ssl = FALSE;
		void *net;
		server *serv;

		if (parse_irc_url (url, &server_name, &port, &channel, &key, &use_ssl))
		{
			/* maybe we're already connected to this net */

			/* check for "FreeNode" */
			net = servlist_net_find (server_name, NULL, g_ascii_strcasecmp);
			/* check for "irc.eu.freenode.net" */
			if (!net)
				net = servlist_net_find_from_server (server_name);

			if (net)
			{
				/* found the network, but are we connected? */
				serv = find_server_from_net (net);
				if (serv)
				{
					url_join_only (serv, tbuf, channel, key);
					g_free (url);
					return TRUE;
				}
			}
			else
			{
				/* an un-listed connection */
				serv = find_server_from_hostname (server_name);
				if (serv)
				{
					url_join_only (serv, tbuf, channel, key);
					g_free (url);
					return TRUE;
				}
			}

			/* not connected to this net, open new window */
			cmd_newserver (sess, tbuf, word, word_eol);

		} else
			fe_open_url (word[2]);
		g_free (url);
		return TRUE;
	}

	return FALSE;
}

static int
userlist_cb (struct User *user, session *sess)
{
	time_t lt;

	if (!user->lasttalk)
		lt = 0;
	else
		lt = time (0) - user->lasttalk;
	PrintTextf (sess,
				"\00306%s\t\00314[\00310%-38s\00314] \017ov\0033=\017%d%d away=%u lt\0033=\017%ld\n",
				user->nick, user->hostname, user->op, user->voice, user->away, (long)lt);

	return TRUE;
}

static int
cmd_uselect (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int idx = 2;
	int clear = TRUE;
	int scroll = FALSE;

	if (strcmp (word[2], "-a") == 0)	/* ADD (don't clear selections) */
	{
		clear = FALSE;
		idx++;
	}
	if (strcmp (word[idx], "-s") == 0)	/* SCROLL TO */
	{
		scroll = TRUE;
		idx++;
	}
	/* always valid, no args means clear the selection list */
	fe_uselect (sess, word + idx, clear, scroll);
	return TRUE;
}

static int
cmd_userlist (struct session *sess, char *tbuf, char *word[],
				  char *word_eol[])
{
	tree_foreach (sess->usertree, (tree_traverse_func *)userlist_cb, sess);
	return TRUE;
}

static int
wallchop_cb (struct User *user, multidata *data)
{
	if (user->op)
	{
		if (data->i)
			strcat (data->tbuf, ",");
		strcat (data->tbuf, user->nick);
		data->i++;
	}
	if (data->i == 5)
	{
		data->i = 0;
		sprintf (data->tbuf + strlen (data->tbuf),
					" :[@%s] %s", data->sess->channel, data->reason);
		data->sess->server->p_raw (data->sess->server, data->tbuf);
		strcpy (data->tbuf, "NOTICE ");
	}

	return TRUE;
}

static int
cmd_wallchop (struct session *sess, char *tbuf, char *word[],
				  char *word_eol[])
{
	multidata data;

	if (!(*word_eol[2]))
		return FALSE;

	strcpy (tbuf, "NOTICE ");

	data.reason = word_eol[2];
	data.tbuf = tbuf;
	data.i = 0;
	data.sess = sess;
	tree_foreach (sess->usertree, (tree_traverse_func*)wallchop_cb, &data);

	if (data.i)
	{
		sprintf (tbuf + strlen (tbuf),
					" :[@%s] %s", sess->channel, word_eol[2]);
		sess->server->p_raw (sess->server, tbuf);
	}

	return TRUE;
}

static int
cmd_hop (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int i = 2;

	while (1)
	{
		if (!*word[i])
		{
			if (i == 2)
				return FALSE;
			send_channel_modes (sess, tbuf, word, 2, i, '+', 'h', 0);
			return TRUE;
		}
		i++;
	}
}

static int
cmd_voice (struct session *sess, char *tbuf, char *word[], char *word_eol[])
{
	int i = 2;

	while (1)
	{
		if (!*word[i])
		{
			if (i == 2)
				return FALSE;
			send_channel_modes (sess, tbuf, word, 2, i, '+', 'v', 0);
			return TRUE;
		}
		i++;
	}
}

/* *MUST* be kept perfectly sorted for the bsearch to work */
const struct commands xc_cmds[] = {
	{"ADDBUTTON", cmd_addbutton, 0, 0, 1,
	 N_("ADDBUTTON <name> <action>, adds a button under the user-list")},
	{"ADDSERVER", cmd_addserver, 0, 0, 1, N_("ADDSERVER <NewNetwork> <hostname/port>, adds a new network with a new server to the network list")},
	{"ALLCHAN", cmd_allchannels, 0, 0, 1,
	 N_("ALLCHAN <cmd>, sends a command to all channels you're in")},
	{"ALLCHANL", cmd_allchannelslocal, 0, 0, 1,
	 N_("ALLCHANL <cmd>, sends a command to all channels on the current server")},
	{"ALLSERV", cmd_allservers, 0, 0, 1,
	 N_("ALLSERV <cmd>, sends a command to all servers you're in")},
	{"AWAY", cmd_away, 1, 0, 1, N_("AWAY [<reason>], sets you away (use /BACK to unset)")},
	{"BACK", cmd_back, 1, 0, 1, N_("BACK, sets you back (not away)")},
	{"BAN", cmd_ban, 1, 1, 1,
	 N_("BAN <mask> [<bantype>], bans everyone matching the mask from the current channel. If they are already on the channel this doesn't kick them (needs chanop)")},
	{"CHANOPT", cmd_chanopt, 0, 0, 1, N_("CHANOPT [-quiet] <variable> [<value>]")},
	{"CHARSET", cmd_charset, 0, 0, 1, N_("CHARSET [<encoding>], get or set the encoding used for the current connection")},
	{"CLEAR", cmd_clear, 0, 0, 1, N_("CLEAR [ALL|HISTORY|[-]<amount>], Clears the current text window or command history")},
	{"CLOSE", cmd_close, 0, 0, 1, N_("CLOSE [-m], Closes the current tab, closing the window if this is the only open tab, or with the \"-m\" flag, closes all queries.")},

	{"COUNTRY", cmd_country, 0, 0, 1,
	 N_("COUNTRY [-s] <code|wildcard>, finds a country code, eg: au = australia")},
	{"CTCP", cmd_ctcp, 1, 0, 1,
	 N_("CTCP <nick> <message>, send the CTCP message to nick, common messages are VERSION and USERINFO")},
	{"CYCLE", cmd_cycle, 1, 1, 1,
	 N_("CYCLE [<channel>], parts the current or given channel and immediately rejoins")},
	{"DCC", cmd_dcc, 0, 0, 1,
	 N_("\n"
	 "DCC GET <nick>                      - accept an offered file\n"
	 "DCC SEND [-maxcps=#] <nick> [file]  - send a file to someone\n"
	 "DCC PSEND [-maxcps=#] <nick> [file] - send a file using passive mode\n"
	 "DCC LIST                            - show DCC list\n"
	 "DCC CHAT <nick>                     - offer DCC CHAT to someone\n"
	 "DCC PCHAT <nick>                    - offer DCC CHAT using passive mode\n"
	 "DCC CLOSE <type> <nick> <file>         example:\n"
	 "         /dcc close send johnsmith file.tar.gz")},
	{"DEBUG", cmd_debug, 0, 0, 1, 0},

	{"DEHOP", cmd_dehop, 1, 1, 1,
	 N_("DEHOP <nick>, removes chanhalf-op status from the nick on the current channel (needs chanop)")},
	{"DELBUTTON", cmd_delbutton, 0, 0, 1,
	 N_("DELBUTTON <name>, deletes a button from under the user-list")},
	{"DEOP", cmd_deop, 1, 1, 1,
	 N_("DEOP <nick>, removes chanop status from the nick on the current channel (needs chanop)")},
	{"DEVOICE", cmd_devoice, 1, 1, 1,
	 N_("DEVOICE <nick>, removes voice status from the nick on the current channel (needs chanop)")},
	{"DISCON", cmd_discon, 0, 0, 1, N_("DISCON, Disconnects from server")},
	{"DNS", cmd_dns, 0, 0, 1, N_("DNS <nick|host|ip>, Resolves an IP or hostname")},
	{"DOAT", cmd_doat, 0, 0, 1, N_("DOAT <channel,list,/network> <command>")},
	{"ECHO", cmd_echo, 0, 0, 1, N_("ECHO <text>, Prints text locally")},
#ifndef WIN32
	{"EXEC", cmd_exec, 0, 0, 1,
	 N_("EXEC [-o] <command>, runs the command. If -o flag is used then output is sent to current channel, else is printed to current text box")},
	{"EXECCONT", cmd_execc, 0, 0, 1, N_("EXECCONT, sends the process SIGCONT")},
	{"EXECKILL", cmd_execk, 0, 0, 1,
	 N_("EXECKILL [-9], kills a running exec in the current session. If -9 is given the process is SIGKILL'ed")},
	{"EXECSTOP", cmd_execs, 0, 0, 1, N_("EXECSTOP, sends the process SIGSTOP")},
	{"EXECWRITE", cmd_execw, 0, 0, 1, N_("EXECWRITE [-q|--], sends data to the processes stdin; use -q flag to quiet/suppress output at text box; use -- flag to stop interpreting arguments as flags, needed if -q itself would occur as data")},
#endif
	{"FLUSHQ", cmd_flushq, 0, 0, 1,
	 N_("FLUSHQ, flushes the current server's send queue")},
	{"GATE", cmd_gate, 0, 0, 1,
	 N_("GATE <host> [<port>], proxies through a host, port defaults to 23")},
	{"GETBOOL", cmd_getbool, 0, 0, 1, "GETBOOL <command> <title> <text>"},
	{"GETFILE", cmd_getfile, 0, 0, 1, "GETFILE [-folder] [-multi] [-save] <command> <title> [<initial>]"},
	{"GETINT", cmd_getint, 0, 0, 1, "GETINT <default> <command> <prompt>"},
	{"GETSTR", cmd_getstr, 0, 0, 1, "GETSTR <default> <command> <prompt>"},
	{"GHOST", cmd_ghost, 1, 0, 1, N_("GHOST <nick> [password], Kills a ghosted nickname")},
	{"GUI", cmd_gui, 0, 0, 1, "GUI [APPLY|ATTACH|DETACH|SHOW|HIDE|FOCUS|FLASH|ICONIFY]\n"
									  "       GUI [MSGBOX <text>|MENU TOGGLE]\n"
									  "       GUI COLOR <n> [-NOOVERRIDE]"},
	{"HELP", cmd_help, 0, 0, 1, 0},
	{"HISTORY", cmd_history, 1, 0, 1,
	 N_("HISTORY [target] [limit], requests chat history from the server (IRCv3)")},
	{"HOP", cmd_hop, 1, 1, 1,
	 N_("HOP <nick>, gives chanhalf-op status to the nick (needs chanop)")},
	{"ID", cmd_id, 1, 0, 1, N_("ID <password>, identifies yourself to nickserv")},
	{"IGNORE", cmd_ignore, 0, 0, 1,
	 N_("IGNORE <mask> <types..> <options..>\n"
	 "    mask - host mask to ignore, eg: *!*@*.aol.com\n"
	 "    types - types of data to ignore, one or all of:\n"
	 "            PRIV, CHAN, NOTI, CTCP, DCC, INVI, ALL\n"
	 "    options - NOSAVE, QUIET")},

	{"INVITE", cmd_invite, 1, 0, 1,
	 N_("INVITE <nick> [<channel>], invites someone to a channel, by default the current channel (needs chanop)")},
	{"JOIN", cmd_join, 1, 0, 0, N_("JOIN <channel>, joins the channel")},
	{"KICK", cmd_kick, 1, 1, 1,
	 N_("KICK <nick> [reason], kicks the nick from the current channel (needs chanop)")},
	{"KICKBAN", cmd_kickban, 1, 1, 1,
	 N_("KICKBAN <nick> [reason], bans then kicks the nick from the current channel (needs chanop)")},
	{"KILLALL", cmd_killall, 0, 0, 1, "KILLALL, immediately exit"},
	{"LAGCHECK", cmd_lagcheck, 0, 0, 1,
	 N_("LAGCHECK, forces a new lag check")},
	{"LASTLOG", cmd_lastlog, 0, 0, 1,
	 N_("LASTLOG [-h] [-m] [-r] [--] <string>, searches for a string in the buffer\n"
	 "    Use -h to highlight the found string(s)\n"
	 "    Use -m to match case\n"
	 "    Use -r when string is a Regular Expression\n"
	 "    Use -- (double hyphen) to end options when searching for, say, the string '-r'")},
	{"LIST", cmd_list, 1, 0, 1, 0},
	{"LOAD", cmd_load, 0, 0, 1, N_("LOAD [-e] <file>, loads a plugin or script")},
	{"MARKREAD", cmd_markread, 1, 0, 1,
	 N_("MARKREAD [target] [msgid], marks messages as read (IRCv3 read-marker)")},

	{"MDEHOP", cmd_mdehop, 1, 1, 1,
	 N_("MDEHOP, Mass deop's all chanhalf-ops in the current channel (needs chanop)")},
	{"MDEOP", cmd_mdeop, 1, 1, 1,
	 N_("MDEOP, Mass deop's all chanops in the current channel (needs chanop)")},
	{"ME", cmd_me, 0, 0, 1,
	 N_("ME <action>, sends the action to the current channel (actions are written in the 3rd person, like /me jumps)")},
	{"MENU", cmd_menu, 0, 0, 1, "MENU [-eX] [-i<ICONFILE>] [-k<mod>,<key>] [-m] [-pX] [-r<X,group>] [-tX] {ADD|DEL} <path> [command] [unselect command]\n"
										 "       See https://github.com/evilnet/poxchat for more details."},
	{"METADATA", cmd_metadata, 1, 0, 1, N_("METADATA <target> <GET|LIST|SET|CLEAR|SUB|UNSUB|SUBS|SYNC> [...], manage user/channel metadata")},
	{"MHOP", cmd_mhop, 1, 1, 1,
	 N_("MHOP, Mass hop's all users in the current channel (needs chanop)")},
	{"MKICK", cmd_mkick, 1, 1, 1,
	 N_("MKICK, Mass kicks everyone except you in the current channel (needs chanop)")},
	{"MODE", cmd_mode, 1, 0, 1, 0},
	{"MONITOR", cmd_monitor, 1, 0, 1, N_("MONITOR [+|-]nick[,nick,...] | C | L | S, manage the MONITOR list for nick online/offline notifications")},
	{"MOP", cmd_mop, 1, 1, 1,
	 N_("MOP, Mass op's all users in the current channel (needs chanop)")},
	{"MSG", cmd_msg, 0, 0, 1, N_("MSG <nick> <message>, sends a private message, message \".\" to send to last nick or prefix with \"=\" for dcc chat")},

	{"NAMES", cmd_names, 1, 0, 1,
	 N_("NAMES [channel], Lists the nicks on the channel")},
	{"NCTCP", cmd_nctcp, 1, 0, 1,
	 N_("NCTCP <nick> <message>, Sends a CTCP notice")},
	{"NEWSERVER", cmd_newserver, 0, 0, 1, N_("NEWSERVER [-noconnect] <hostname> [<port>]")},
	{"NICK", cmd_nick, 0, 0, 1, N_("NICK <nickname>, sets your nick")},

	{"NOTICE", cmd_notice, 1, 0, 1,
	 N_("NOTICE <nick/channel> <message>, sends a notice")},
	{"NOTIFY", cmd_notify, 0, 0, 1,
	 N_("NOTIFY [-n network1[,network2,...]] [<nick>], displays your notify list or adds someone to it")},
	{"OP", cmd_op, 1, 1, 1,
	 N_("OP <nick>, gives chanop status to the nick (needs chanop)")},
	{"PART", cmd_part, 1, 1, 0,
	 N_("PART [<channel>] [<reason>], leaves the channel, by default the current one")},
	{"PING", cmd_ping, 1, 0, 1,
	 N_("PING <nick | channel>, CTCP pings nick or channel")},
	{"QUERY", cmd_query, 0, 0, 1,
	 N_("QUERY [-nofocus] <nick> [message], opens up a new privmsg window to someone and optionally sends a message")},
	{"QUIET", cmd_quiet, 1, 1, 1,
	 N_("QUIET <mask> [<quiettype>], quiet everyone matching the mask in the current channel if supported by the server.")},
	{"QUIT", cmd_quit, 0, 0, 1,
	 N_("QUIT [<reason>], disconnects from the current server")},
	{"QUOTE", cmd_quote, 1, 0, 1,
	 N_("QUOTE <text>, sends the text in raw form to the server")},
	{"REACT", cmd_react, 1, 0, 1,
	 N_("REACT <emoji> [msgid], sends a reaction to a message (IRCv3 +draft/react)")},
#ifdef USE_OPENSSL
	{"RECONNECT", cmd_reconnect, 0, 0, 1,
	 N_("RECONNECT [-ssl|-ssl-noverify] [<host>] [<port>] [<password>], Can be called just as /RECONNECT to reconnect to the current server or with /RECONNECT ALL to reconnect to all the open servers")},
#else
	{"RECONNECT", cmd_reconnect, 0, 0, 1,
	 N_("RECONNECT [<host>] [<port>] [<password>], Can be called just as /RECONNECT to reconnect to the current server or with /RECONNECT ALL to reconnect to all the open servers")},
#endif
	{"RECV", cmd_recv, 1, 0, 1, N_("RECV <text>, send raw data to PoxChat, as if it was received from the IRC server")},
	{"REDACT", cmd_redact, 1, 0, 1,
	 N_("REDACT [target] [msgid] [reason], redacts/deletes a message (IRCv3)")},
	{"REGISTER", cmd_register, 1, 0, 1, N_("REGISTER <account> [<email>|*] <password>, register a new account on the server (requires draft/account-registration)")},
	{"RELOAD", cmd_reload, 0, 0, 1, N_("RELOAD <name>, reloads a plugin or script")},
	{"REPLY", cmd_reply, 1, 0, 1,
	 N_("REPLY [msgid] <text>, sends a reply to a message (IRCv3 +draft/reply)")},
	{"SAY", cmd_say, 0, 0, 1,
	 N_("SAY <text>, sends the text to the object in the current window")},
	{"SEND", cmd_send, 0, 0, 1, N_("SEND <nick> [<file>]")},
#ifdef USE_OPENSSL
	{"SERVCHAN", cmd_servchan, 0, 0, 1,
	 N_("SERVCHAN [-insecure|-ssl|-ssl-noverify] <host> <port> <channel>, connects and joins a channel using ssl unless otherwise specified")},
#else
	{"SERVCHAN", cmd_servchan, 0, 0, 1,
	 N_("SERVCHAN <host> <port> <channel>, connects and joins a channel")},
#endif
#ifdef USE_OPENSSL
	{"SERVER", cmd_server, 0, 0, 1,
	 N_("SERVER [-insecure|-ssl|-ssl-noverify] <host> [<port>] [<password>], connects to a server using ssl unless otherwise specified, the default port is 6697 for ssl connections and 6667 for insecure connections")},
#else
	{"SERVER", cmd_server, 0, 0, 1,
	 N_("SERVER <host> [<port>] [<password>], connects to a server, the default port is 6667")},
#endif
	{"SET", cmd_set, 0, 0, 1, N_("SET [-e] [-off|-on] [-quiet] <variable> [<value>]")},
	{"SETCURSOR", cmd_setcursor, 0, 0, 1, N_("SETCURSOR [-|+]<position>, reposition the cursor in the inputbox")},
	{"SETTAB", cmd_settab, 0, 0, 1, N_("SETTAB <new name>, change a tab's name, tab_trunc limit still applies")},
	{"SETTEXT", cmd_settext, 0, 0, 1, N_("SETTEXT <new text>, replace the text in the input box")},
	{"SPLAY", cmd_splay, 0, 0, 1, "SPLAY <soundfile>"},
	{"TAGMSG", cmd_tagmsg, 1, 0, 1,
	 N_("TAGMSG <target> <tags>, sends a tag-only message (e.g., typing indicators, reactions)")},
	{"TOAST", cmd_toast, 0, 0, 1,
	 N_("TOAST [<text>], show a test toast notification overlay")},
	{"TOPIC", cmd_topic, 1, 1, 1,
	 N_("TOPIC [<topic>], sets the topic if one is given, else shows the current topic")},
	{"TRAY", cmd_tray, 0, 0, 1,
	 N_("\nTRAY -f <timeout> <file1> [<file2>] Blink tray between two icons.\n"
		   "TRAY -f <filename>                  Set tray to a fixed icon.\n"
			"TRAY -i <number>                    Blink tray with an internal icon.\n"
			"TRAY -t <text>                      Set the tray tooltip.\n"
			"TRAY -b <title> <text>              Set the tray balloon."
			)},
	{"UNBAN", cmd_unban, 1, 1, 1,
	 N_("UNBAN <mask> [<mask>...], unbans the specified masks.")},
	{"UNIGNORE", cmd_unignore, 0, 0, 1, N_("UNIGNORE <mask> [QUIET]")},
	{"UNLOAD", cmd_unload, 0, 0, 1, N_("UNLOAD <name>, unloads a plugin or script")},
	{"UNQUIET", cmd_unquiet, 1, 1, 1,
	 N_("UNQUIET <mask> [<mask>...], unquiets the specified masks if supported by the server.")},
	{"UNREACT", cmd_unreact, 1, 0, 1,
	 N_("UNREACT <emoji> [msgid], removes a reaction from a message (IRCv3 +draft/unreact)")},
	{"URL", cmd_url, 0, 0, 1, N_("URL <url>, opens a URL in your browser")},
	{"USELECT", cmd_uselect, 0, 1, 0,
	 N_("USELECT [-a] [-s] <nick1> <nick2> etc, highlights nick(s) in channel userlist")},
	{"USERLIST", cmd_userlist, 1, 1, 1, 0},
	{"VERIFY", cmd_verify, 1, 0, 1, N_("VERIFY <account> <code>, complete account registration with a verification code")},
	{"VOICE", cmd_voice, 1, 1, 1,
	 N_("VOICE <nick>, gives voice status to someone (needs chanop)")},
	{"WALLCHOP", cmd_wallchop, 1, 1, 1,
	 N_("WALLCHOP <message>, sends the message to all chanops on the current channel")},
	{0, 0, 0, 0, 0, 0}
};


static int
command_compare (const void *a, const void *b)
{
	return g_ascii_strcasecmp (a, ((struct commands *)b)->name);
}

static struct commands *
find_internal_command (char *name)
{
	/* the "-1" is to skip the NULL terminator */
	return bsearch (name, xc_cmds, (sizeof (xc_cmds) /
				sizeof (xc_cmds[0])) - 1, sizeof (xc_cmds[0]), command_compare);
}

static gboolean
usercommand_show_help (session *sess, char *name)
{
	struct popup *pop;
	gboolean found = FALSE;
	char buf[1024];
	GSList *list;

	list = command_list;
	while (list)
	{
		pop = (struct popup *) list->data;
		if (!g_ascii_strcasecmp (pop->name, name))
		{
			g_snprintf (buf, sizeof(buf), _("User Command for: %s\n"), pop->cmd);
			PrintText (sess, buf);

			found = TRUE;
		}
		list = list->next;
	}

	return found;
}

static void
help (session *sess, char *tbuf, char *helpcmd, int quiet)
{
	struct commands *cmd;

	if (plugin_show_help (sess, helpcmd))
		return;

	if (usercommand_show_help (sess, helpcmd))
		return;

	cmd = find_internal_command (helpcmd);
	if (cmd)
	{
		if (cmd->help)
		{
			g_snprintf (tbuf, TBUFSIZE, _("Usage: %s\n"), _(cmd->help));
			PrintText (sess, tbuf);
		} else
		{
			if (!quiet)
				PrintText (sess, _("\nNo help available on that command.\n"));
		}
		return;
	}

	if (!quiet)
		PrintText (sess, _("No such command.\n"));
}

/* inserts %a, %c, %d etc into buffer. Also handles &x %x for word/word_eol. *
 *   returns 2 on buffer overflow
 *   returns 1 on success                                                    *
 *   returns 0 on bad-args-for-user-command                                  *
 * - word/word_eol args might be NULL                                        *
 * - this beast is used for UserCommands, UserlistButtons and CTCP replies   */

int
auto_insert (char *dest, gsize destlen, unsigned char *src, char *word[],
				 char *word_eol[], char *a, char *c, char *d, char *e, char *h,
				 char *n, char *s, char *u)
{
	int num;
	char buf[32];
	time_t now;
	struct tm *tm_ptr;
	char *utf;
	gsize utf_len;
	char *orig = dest;

	destlen--;

	while (src[0])
	{
		if (src[0] == '%' || src[0] == '&')
		{
			if (isdigit ((unsigned char) src[1]))
			{
				if (isdigit ((unsigned char) src[2]) && isdigit ((unsigned char) src[3]))
				{
					buf[0] = src[1];
					buf[1] = src[2];
					buf[2] = src[3];
					buf[3] = 0;
					dest[0] = atoi (buf);
					utf = g_locale_to_utf8 (dest, 1, 0, &utf_len, 0);
					if (utf)
					{
						if ((dest - orig) + utf_len >= destlen)
						{
							g_free (utf);
							return 2;
						}

						memcpy (dest, utf, utf_len);
						g_free (utf);
						dest += utf_len;
					}
					src += 3;
				} else
				{
					if (word)
					{
						src++;
						num = src[0] - '0';	/* ascii to decimal */
						if (*word[num] == 0)
							return 0;

						if (src[-1] == '%')
							utf = word[num];
						else
							utf = word_eol[num];

						/* avoid recusive usercommand overflow */
						if ((dest - orig) + strlen (utf) >= destlen)
							return 2;

						strcpy (dest, utf);
						dest += strlen (dest);
					}
				}
			} else
			{
				if (src[0] == '&')
					goto lamecode;
				src++;
				utf = NULL;
				switch (src[0])
				{
				case '%':
					if ((gsize)(dest - orig) + 2u >= destlen)
						return 2;
					dest[0] = '%';
					dest[1] = 0;
					dest++;
					break;
				case 'a':
					utf = a; break;
				case 'c':
					utf = c; break;
				case 'd':
					utf = d; break;
				case 'e':
					utf = e; break;
				case 'h':
					utf = h; break;
				case 'm':
					utf = get_sys_str (1); break;
				case 'n':
					utf = n; break;
				case 's':
					utf = s; break;
				case 't':
					now = time (0);
					utf = ctime (&now);
					utf[19] = 0;
					break;
				case 'u':
					utf = u; break;
				case 'v':
					utf = PACKAGE_VERSION; break;
					break;
				case 'y':
					now = time (0);
					tm_ptr = localtime (&now);
					g_snprintf (buf, sizeof (buf), "%4d%02d%02d", 1900 +
								 tm_ptr->tm_year, 1 + tm_ptr->tm_mon, tm_ptr->tm_mday);
					utf = buf;
					break;
				default:
					src--;
					goto lamecode;
				}

				if (utf)
				{
					if ((dest - orig) + strlen (utf) >= destlen)
						return 2;
					strcpy (dest, utf);
					dest += strlen (dest);
				}

			}
			src++;
		} else
		{
			utf_len = g_utf8_skip[src[0]];

			if ((dest - orig) + utf_len >= destlen)
				return 2;

			if (utf_len == 1)
			{
		 lamecode:
				dest[0] = src[0];
				dest++;
				src++;
			} else
			{
				memcpy (dest, src, utf_len);
				dest += utf_len;
				src += utf_len;
			}
		}
	}

	dest[0] = 0;

	return 1;
}

void
check_special_chars (char *cmd, int do_ascii) /* check for %X */
{
	int occur = 0;
	int len = strlen (cmd);
	char *buf, *utf;
	char tbuf[4];
	int i = 0, j = 0;
	gsize utf_len;

	if (!len)
		return;

	buf = g_malloc (len + 1);

	while (cmd[j])
	{
		switch (cmd[j])
		{
		case '%':
			occur++;
			if (	do_ascii &&
					j + 3 < len &&
					(isdigit ((unsigned char) cmd[j + 1]) && isdigit ((unsigned char) cmd[j + 2]) &&
					isdigit ((unsigned char) cmd[j + 3])))
			{
				tbuf[0] = cmd[j + 1];
				tbuf[1] = cmd[j + 2];
				tbuf[2] = cmd[j + 3];
				tbuf[3] = 0;
				buf[i] = atoi (tbuf);
				utf = g_locale_to_utf8 (buf + i, 1, 0, &utf_len, 0);
				if (utf)
				{
					memcpy (buf + i, utf, utf_len);
					g_free (utf);
					i += (int)(utf_len - 1);
				}
				j += 3;
			} else
			{
				switch (cmd[j + 1])
				{
				case 'R':
					buf[i] = '\026';
					break;
				case 'U':
					buf[i] = '\037';
					break;
				case 'B':
					buf[i] = '\002';
					break;
				case 'I':
					buf[i] = '\035';
					break;
				case 'S':
					buf[i] = '\036';
					break;
				case 'C':
					buf[i] = '\003';
					break;
				case 'O':
					buf[i] = '\017';
					break;
				case 'H':	/* CL: invisible text code */
					buf[i] = HIDDEN_CHAR;
					break;
				case '%':
					buf[i] = '%';
					break;
				default:
					buf[i] = '%';
					j--;
					break;
				}
				j++;
				break;
		default:
				buf[i] = cmd[j];
			}
		}
		j++;
		i++;
	}
	buf[i] = 0;
	if (occur)
		strcpy (cmd, buf);
	g_free (buf);
}

typedef struct
{
	char *nick;
	int len;
	struct User *best;
	int bestlen;
	char *space;
	char *tbuf;
} nickdata;

static int
nick_comp_cb (struct User *user, nickdata *data)
{
	int lenu;

	if (!rfc_ncasecmp (user->nick, data->nick, data->len))
	{
		lenu = strlen (user->nick);
		if (lenu == data->len)
		{
			g_snprintf (data->tbuf, TBUFSIZE, "%s%s", user->nick, data->space);
			data->len = -1;
			return FALSE;
		} else if (lenu < data->bestlen)
		{
			data->bestlen = lenu;
			data->best = user;
		}
	}

	return TRUE;
}

static void
perform_nick_completion (struct session *sess, char *cmd, char *tbuf)
{
	int len;
	char *space = strchr (cmd, ' ');
	if (space && space != cmd)
	{
		if (space[-1] == prefs.hex_completion_suffix[0] && space - 1 != cmd)
		{
			len = (int)(space - cmd - 1);
			if (len < NICKLEN)
			{
				char nick[NICKLEN];
				nickdata data;

				memcpy (nick, cmd, len);
				nick[len] = 0;

				data.nick = nick;
				data.len = len;
				data.bestlen = INT_MAX;
				data.best = NULL;
				data.tbuf = tbuf;
				data.space = space - 1;
				tree_foreach (sess->usertree, (tree_traverse_func *)nick_comp_cb, &data);

				if (data.len == -1)
					return;

				if (data.best)
				{
					g_snprintf (tbuf, TBUFSIZE, "%s%s", data.best->nick, space - 1);
					return;
				}
			}
		}
	}

	strcpy (tbuf, cmd);
}

static void
user_command (session * sess, char *tbuf, char *cmd, char *word[],
				  char *word_eol[])
{
	if (!auto_insert (tbuf, 2048, cmd, word, word_eol, "", sess->channel, "",
							server_get_network (sess->server, TRUE), "",
							sess->server->nick, "", ""))
	{
		PrintText (sess, _("Bad arguments for user command.\n"));
		return;
	}

	handle_command (sess, tbuf, TRUE);
}

/* handle text entered without a hex_input_command_char prefix */

static void
handle_say (session *sess, char *text, int check_spch)
{
	struct DCC *dcc;
	char *word[PDIWORDS+1];
	char *word_eol[PDIWORDS+1];
	char *pdibuf;
	char *newcmd;
	int len;
	int newcmdlen;
	message_tags_data no_tags = MESSAGE_TAGS_DATA_INIT;
	/* Synthesize +draft/reply tag for self-echo when sending a reply so
	 * inbound_chanmsg prepends the reply sentinel (U+FDD0) on local display.
	 * p_message (irc_message) consumes sess->reply_msgid, so capture it
	 * up front and free at function exit. */
	char *captured_reply_msgid = (sess->reply_msgid ? g_strdup (sess->reply_msgid) : NULL);
	GHashTable *self_reply_tags = NULL;
	message_tags_data reply_tags = MESSAGE_TAGS_DATA_INIT;
	message_tags_data *self_tags = &no_tags;
	if (captured_reply_msgid)
	{
		self_reply_tags = g_hash_table_new (g_str_hash, g_str_equal);
		g_hash_table_insert (self_reply_tags, (gpointer)"+draft/reply",
		                     captured_reply_msgid);
		reply_tags.all_tags = self_reply_tags;
		self_tags = &reply_tags;
	}

	if (strcmp (sess->channel, "(lastlog)") == 0)
	{
		lastlog (sess->lastlog_sess, text, sess->lastlog_flags);
		return;
	}

	/* IRCv3 react-with-text: if react state is active, send TAGMSG instead */
	if (sess->react_target_msgid && sess->server && sess->server->connected &&
	    sess->server->have_message_tags && sess->channel[0])
	{
		char *escaped = escape_tag_value (text);
		char *tags = g_strdup_printf ("+draft/react=%s;+draft/reply=%s",
		                               escaped, sess->react_target_msgid);
		tcp_sendf_with_raw_tags (sess->server, "TAGMSG", sess->channel, tags,
		                         "TAGMSG %s\r\n", sess->channel);
		g_free (tags);
		g_free (escaped);
		g_clear_pointer (&sess->react_target_msgid, g_free);
		g_clear_pointer (&sess->react_target_nick, g_free);
		fe_reply_state_changed (sess);
		return;
	}

	len = strlen (text);
	pdibuf = g_malloc (len + 1);
	newcmdlen = MAX(len + NICKLEN + 1, TBUFSIZE);
	newcmd = g_malloc (newcmdlen);

	if (check_spch && prefs.hex_input_perc_color)
		check_special_chars (text, prefs.hex_input_perc_ascii);

	/* Python relies on this */
	word[PDIWORDS] = NULL;
	word_eol[PDIWORDS] = NULL;

	/* split the text into words and word_eol */
	process_data_init (pdibuf, text, word, word_eol, TRUE, FALSE);

	/* a command of "" can be hooked for non-commands */
	if (plugin_emit_command (sess, "", word, word_eol))
		goto xit;

	/* incase a plugin did /close */
	if (!is_session (sess))
		goto xit;

	if (!sess->channel[0] || sess->type == SESS_SERVER || sess->type == SESS_NOTICES || sess->type == SESS_SNOTICES)
	{
		notj_msg (sess);
		goto xit;
	}

	if (prefs.hex_completion_auto)
		perform_nick_completion (sess, text, newcmd);
	else
		safe_strcpy (newcmd, text, newcmdlen);

	text = newcmd;

	if (sess->type == SESS_DIALOG)
	{
		/* try it via dcc, if possible */
		dcc = dcc_write_chat (sess->channel, text);
		if (dcc)
		{
			inbound_chanmsg (sess->server, NULL, sess->channel,
								  sess->server->nick, text, TRUE, FALSE, self_tags);
			set_topic (sess, net_ip (dcc->addr), net_ip (dcc->addr));
			goto xit;
		}
	}

	if (sess->server->connected)
	{
		char *split_text = NULL;
		int cmd_length = 13; /* " PRIVMSG ", " ", :, \r, \n */
		int offset = 0;

		while ((split_text = split_up_text (sess, text + offset, cmd_length, split_text)))
		{
			if (sess->server->have_echo_message && sess->server->have_labeled_response)
			{
				/* Tier 2: send, display as pending, record entry.
				 * Save to scrollback with placeholder msgid "pending:<label>" so the
				 * text is preserved correctly. Echo_confirmed path updates to real msgid. */
				sess->server->p_message (sess->server, sess->channel, split_text);
				g_free (sess->current_msgid);
				sess->current_msgid = g_strdup_printf ("pending:%s", sess->server->last_sent_label);
				inbound_chanmsg (sess->server, sess, sess->channel, sess->server->nick,
									  split_text, TRUE, FALSE, self_tags);
				mark_pending_echo (sess, sess->server);
			}
			else if (!sess->server->have_echo_message)
			{
				inbound_chanmsg (sess->server, sess, sess->channel, sess->server->nick,
									  split_text, TRUE, FALSE, self_tags);
				sess->server->p_message (sess->server, sess->channel, split_text);
			}
			else
			{
				/* Tier 1: suppress display, server echo handles it */
				sess->server->p_message (sess->server, sess->channel, split_text);
			}

			if (*split_text)
				offset += strlen(split_text);

			g_free (split_text);
		}

		if (sess->server->have_echo_message && sess->server->have_labeled_response)
		{
			sess->server->p_message (sess->server, sess->channel, text + offset);
			g_free (sess->current_msgid);
			sess->current_msgid = g_strdup_printf ("pending:%s", sess->server->last_sent_label);
			inbound_chanmsg (sess->server, sess, sess->channel, sess->server->nick,
								  text + offset, TRUE, FALSE, self_tags);
			mark_pending_echo (sess, sess->server);
		}
		else if (!sess->server->have_echo_message)
		{
			inbound_chanmsg (sess->server, sess, sess->channel, sess->server->nick,
								  text + offset, TRUE, FALSE, self_tags);
			sess->server->p_message (sess->server, sess->channel, text + offset);
		}
		else
		{
			sess->server->p_message (sess->server, sess->channel, text + offset);
		}
	} else
	{
		notc_msg (sess);
	}

xit:
	g_free (pdibuf);

	g_free (newcmd);

	if (self_reply_tags)
		g_hash_table_destroy (self_reply_tags);
	g_free (captured_reply_msgid);
}

char *
command_insert_vars (session *sess, char *cmd)
{
	int pos;
	GString *expanded;
	ircnet *mynet = (ircnet *) sess->server->network;

	if (!mynet)										/* shouldn't really happen */
	{
		return g_strdup (cmd);						/* the return value will be freed so we must srtdup() it */
	}

	expanded = g_string_new (NULL);

	while (strchr (cmd, '%') != NULL)
	{
		pos = (int) (strchr (cmd, '%') - cmd);		/* offset to the first '%' */
		g_string_append_len (expanded, cmd, pos);	/* copy contents till the '%' */
		cmd += pos + 1;								/* jump to the char after the '%' */

		switch (cmd[0])
		{
			case 'n':
				if (mynet->nick)
				{
					g_string_append (expanded, mynet->nick);
				}
				else
				{
					g_string_append (expanded, prefs.hex_irc_nick1);
				}
				cmd++;
				break;

			case 'p':
				if (mynet->pass)
				{
					g_string_append (expanded, mynet->pass);
				}
				cmd++;
				break;

			case 'r':
				if (mynet->real)
				{
					g_string_append (expanded, mynet->real);
				}
				else
				{
					g_string_append (expanded, prefs.hex_irc_real_name);
				}
				cmd++;
				break;

			case 'u':
				if (mynet->user)
				{
					g_string_append (expanded, mynet->user);
				}
				else
				{
					g_string_append (expanded, prefs.hex_irc_user_name);
				}
				cmd++;
				break;

			default:								/* unsupported character? copy it along with the '%'! */
				cmd--;
				g_string_append_len (expanded, cmd, 2);
				cmd += 2;
				break;
		}
	}

	g_string_append (expanded, cmd);				/* copy any remaining string after the last '%' */

	return g_string_free (expanded, FALSE);
}

/* handle a command, without the '/' prefix */

int
handle_command (session *sess, char *cmd, int check_spch)
{
	struct popup *pop;
	int user_cmd = FALSE;
	GSList *list;
	char *word[PDIWORDS+1];
	char *word_eol[PDIWORDS+1];
	static int command_level = 0;
	struct commands *int_cmd;
	char *pdibuf;
	char *tbuf;
	int len;
	int ret = TRUE;

	if (command_level > 99)
	{
		fe_message (_("Too many recursive usercommands, aborting."), FE_MSG_ERROR);
		return TRUE;
	}
	command_level++;
	/* anything below MUST DEC command_level before returning */

	len = strlen (cmd);
	pdibuf = g_malloc (len + 1);
	tbuf = g_malloc (MAX(TBUFSIZE, (len * 2) + 1));

	/* split the text into words and word_eol */
	process_data_init (pdibuf, cmd, word, word_eol, TRUE, TRUE);

	/* ensure an empty string at index 32 for cmd_deop etc */
	/* (internal use only, plugins can still only read 1-31). */
	word[PDIWORDS] = "\000\000";
	word_eol[PDIWORDS] = "\000\000";

	int_cmd = find_internal_command (word[1]);
	/* redo it without quotes processing, for some commands like /JOIN */
	if (int_cmd && !int_cmd->handle_quotes)
	{
		process_data_init (pdibuf, cmd, word, word_eol, FALSE, FALSE);
	}

	if (check_spch && prefs.hex_input_perc_color)
	{
		check_special_chars (cmd, prefs.hex_input_perc_ascii);
	}

	if (plugin_emit_command (sess, word[1], word, word_eol))
	{
		goto xit;
	}

	/* incase a plugin did /close */
	if (!is_session (sess))
	{
		goto xit;
	}

	/* first see if it's a userCommand */
	list = command_list;
	while (list)
	{
		pop = (struct popup *) list->data;
		if (!g_ascii_strcasecmp (pop->name, word[1]))
		{
			user_command (sess, tbuf, pop->cmd, word, word_eol);
			user_cmd = TRUE;
		}
		list = list->next;
	}

	if (user_cmd)
	{
		goto xit;
	}

	/* now check internal commands */
	int_cmd = find_internal_command (word[1]);

	if (int_cmd)
	{
		if (int_cmd->needserver && !sess->server->connected)
		{
			notc_msg (sess);
		}
		else if (int_cmd->needchannel && !sess->channel[0])
		{
			notj_msg (sess);
		}
		else
		{
			switch (int_cmd->callback (sess, tbuf, word, word_eol))
			{
				case FALSE:
					help (sess, tbuf, int_cmd->name, TRUE);
					break;
				case 2:
					ret = FALSE;
					goto xit;
			}
		}
	}
	else
	{
		if (!sess->server->connected)
		{
			PrintTextf (sess, _("Unknown Command %s. Try /help\n"), word[1]);
		}
		else
		{
			/* unknown command, just send it to the server and hope */
			sess->server->p_raw (sess->server, cmd);
		}
	}

xit:
	command_level--;

	g_free (pdibuf);
	g_free (tbuf);

	return ret;
}

/* IRCv3 +typing outbound state machine */

#define TYPING_SEND_INTERVAL_MS  3000
#define TYPING_SEND_EXPIRE_US    (6 * G_USEC_PER_SEC)
/* How long after we send a typing TAGMSG to treat an inbound is_self +typing as
 * our own echo (echo-message / bouncer round-trip).  Beyond this window an
 * is_self +typing is attributed to another connected client of ours. */
#define TYPING_SELF_ECHO_WINDOW_US  (5 * G_USEC_PER_SEC)

static int typing_send_timer_cb (void *userdata);

static void
typing_mark_self_echo_window (session *sess)
{
	sess->typing_self_echo_until = g_get_monotonic_time () + TYPING_SELF_ECHO_WINDOW_US;
}

static void
typing_send_active (session *sess)
{
	tcp_sendf (sess->server, "@+typing=active TAGMSG %s\r\n", sess->channel);
	sess->typing_last_sent = g_get_monotonic_time ();
	typing_mark_self_echo_window (sess);

	/* Restart the 3s re-send timer */
	if (sess->typing_send_timer)
		fe_timeout_remove (sess->typing_send_timer);
	sess->typing_send_timer = fe_timeout_add (TYPING_SEND_INTERVAL_MS,
	                                           typing_send_timer_cb, sess);
}

static int
typing_send_timer_cb (void *userdata)
{
	session *sess = userdata;

	/* Did the user type since our last send? */
	if (sess->typing_last_keystroke > sess->typing_last_sent)
	{
		typing_send_active (sess);
		return 0; /* typing_send_active started a new timer */
	}

	/* User stopped typing but input still has text — send paused */
	tcp_sendf (sess->server, "@+typing=paused TAGMSG %s\r\n", sess->channel);
	typing_mark_self_echo_window (sess);
	sess->typing_last_sent = 0;
	sess->typing_send_timer = 0;
	return 0;
}

void
typing_indicator_cancel (session *sess)
{
	if (sess->typing_send_timer)
	{
		fe_timeout_remove (sess->typing_send_timer);
		sess->typing_send_timer = 0;
	}
	/* Send done if we previously sent active/paused */
	if (sess->typing_last_sent != 0 && sess->server && sess->server->connected
	    && sess->server->have_message_tags && sess->channel[0])
	{
		tcp_sendf (sess->server, "@+typing=done TAGMSG %s\r\n", sess->channel);
		typing_mark_self_echo_window (sess);
	}
	sess->typing_last_sent = 0;
}

void
typing_indicator_keystroke (session *sess)
{
	gint64 now;

	if (!prefs.hex_irc_typing_send)
		return;
	if (!sess->server || !sess->server->connected)
		return;
	if (!sess->server->have_message_tags)
		return;
	if (sess->type != SESS_CHANNEL && sess->type != SESS_DIALOG)
		return;
	if (!sess->channel[0])
		return;

	/* Check if input box is empty — if so, cancel */
	{
		const char *text = fe_get_inputbox_contents (sess);
		if (!text || !*text)
		{
			typing_indicator_cancel (sess);
			return;
		}
	}

	now = g_get_monotonic_time ();
	sess->typing_last_keystroke = now;

	/* Send active if never sent or expired server-side */
	if (sess->typing_last_sent == 0 ||
	    now - sess->typing_last_sent >= TYPING_SEND_EXPIRE_US)
	{
		typing_send_active (sess);
	}
}

/* handle one line entered into the input box */

static int
handle_user_input (session *sess, char *text, int history, int nocommand)
{
	if (*text == '\0')
		return 1;

	typing_indicator_cancel (sess);

	if (history)
		history_add (&sess->history, text);

	/* is it NOT a command, just text? */
	if (nocommand || text[0] != prefs.hex_input_command_char[0])
	{
		handle_say (sess, text, TRUE);
		return 1;
	}

	/* check for // */
	if (text[0] == prefs.hex_input_command_char[0] && text[1] == prefs.hex_input_command_char[0])
	{
		handle_say (sess, text + 1, TRUE);
		return 1;
	}


	return handle_command (sess, text + 1, TRUE);
}

/* Send collected plain-text lines as a draft/multiline BATCH.
 * Each line is individually split if too long for one PRIVMSG.
 * If total would exceed server limits, closes batch and opens a new one.
 * Returns TRUE if lines were sent as a batch, FALSE if caller should
 * fall back to per-line sending. */
static int
handle_multiline_batch (session *sess, char **lines, int n_lines)
{
	server *serv = sess->server;
	char *batch_tag;
	int cmd_length = 13; /* " PRIVMSG ", " ", :, \r, \n */
	int i;
	int total_bytes = 0;
	int total_lines = 0;
	message_tags_data no_tags = MESSAGE_TAGS_DATA_INIT;

	if (!serv->connected || !sess->channel[0])
		return FALSE;

	batch_tag = tcp_batch_open_multiline (serv, sess->channel);

	/* Tier 2 local echo: display the combined message as pending immediately,
	 * so the user sees it without waiting for the server echo round-trip. */
	if (serv->have_echo_message && serv->have_labeled_response)
	{
		GString *combined = g_string_new (lines[0]);
		for (i = 1; i < n_lines; i++)
		{
			g_string_append_c (combined, '\n');
			g_string_append (combined, lines[i]);
		}
		/* Set pending msgid so scrollback_save captures it for later confirmation */
		g_free (sess->current_msgid);
		sess->current_msgid = serv->last_sent_label
			? g_strdup_printf ("pending:%s", serv->last_sent_label) : NULL;

		fe_begin_multiline_group (sess);
		inbound_chanmsg (serv, sess, sess->channel, serv->nick,
		                 combined->str, TRUE, FALSE, &no_tags);
		fe_end_multiline_group (sess);
		mark_pending_echo (sess, serv);
		g_string_free (combined, TRUE);
	}

	for (i = 0; i < n_lines; i++)
	{
		char *text = lines[i];
		char *split_text = NULL;
		int offset = 0;
		int line_len;

		/* Handle lines that need splitting due to per-message length */
		while ((split_text = split_up_text (sess, text + offset, cmd_length, split_text)))
		{
			line_len = strlen (split_text);
			total_bytes += line_len;
			total_lines++;

			/* Check batch limits — close and reopen if needed */
			if ((serv->multiline_max_bytes > 0 && total_bytes > serv->multiline_max_bytes) ||
			    (serv->multiline_max_lines > 0 && total_lines > serv->multiline_max_lines))
			{
				tcp_batch_close (serv, batch_tag);
				g_free (batch_tag);
				batch_tag = tcp_batch_open_multiline (serv, sess->channel);
				total_bytes = line_len;
				total_lines = 1;
			}

			tcp_batch_privmsg (serv, batch_tag, sess->channel, split_text);

			/* Local display */
			if (!serv->have_echo_message)
			{
				inbound_chanmsg (serv, sess, sess->channel, serv->nick,
				                 split_text, TRUE, FALSE, &no_tags);
			}

			if (*split_text)
				offset += strlen (split_text);
			g_free (split_text);
		}

		/* Remainder (or whole line if no splitting needed) */
		line_len = strlen (text + offset);
		total_bytes += line_len;
		total_lines++;

		if ((serv->multiline_max_bytes > 0 && total_bytes > serv->multiline_max_bytes) ||
		    (serv->multiline_max_lines > 0 && total_lines > serv->multiline_max_lines))
		{
			tcp_batch_close (serv, batch_tag);
			g_free (batch_tag);
			batch_tag = tcp_batch_open_multiline (serv, sess->channel);
			total_bytes = line_len;
			total_lines = 1;
		}

		tcp_batch_privmsg (serv, batch_tag, sess->channel, text + offset);

		if (!serv->have_echo_message)
		{
			inbound_chanmsg (serv, sess, sess->channel, serv->nick,
			                 text + offset, TRUE, FALSE, &no_tags);
		}
	}

	tcp_batch_close (serv, batch_tag);
	g_free (batch_tag);

	return TRUE;
}

/* changed by Steve Green. Macs sometimes paste with imbedded \r */
void
handle_multiline (session *sess, char *cmd, int history, int nocommand)
{
	server *serv = sess->server;

	/* Try draft/multiline BATCH if the server supports it and the input
	 * contains multiple lines of plain text (no commands). */
	if (serv && serv->have_multiline && serv->multiline_max_lines > 1 &&
	    strchr (cmd, '\n'))
	{
		char *p;
		int all_text = TRUE;
		char cmd_char = prefs.hex_input_command_char[0];

		/* Non-destructive scan: check if all lines are plain text */
		p = cmd;
		while (*p)
		{
			if (*p == cmd_char && p[1] != cmd_char)
			{
				all_text = FALSE;
				break;
			}
			/* Skip to next line */
			p += strcspn (p, "\n\r");
			if (*p)
				p++;
		}

		if (all_text)
		{
			/* Save full text for history before destructive split,
			 * which null-terminates newlines in the cmd buffer */
			char *history_text = history ? g_strdup (cmd) : NULL;

			/* Now destructively split and collect lines */
			GPtrArray *lines = g_ptr_array_new ();
			p = cmd;
			while (1)
			{
				char *cr = p + strcspn (p, "\n\r");
				int end_of_string = (*cr == 0);
				if (!end_of_string)
					*cr = 0;

				if (*p == cmd_char && p[1] == cmd_char)
					g_ptr_array_add (lines, p + 1);
				else if (*p)
					g_ptr_array_add (lines, p);

				if (end_of_string)
					break;
				p = cr + 1;
			}

			if (lines->len >= 2)
			{
				if (history_text)
					history_add (&sess->history, history_text);
				g_free (history_text);
				typing_indicator_cancel (sess);
				handle_multiline_batch (sess, (char **)lines->pdata, lines->len);
				g_ptr_array_free (lines, TRUE);
				return;
			}

			/* Only 1 line after splitting — fall through to per-line.
			 * The string is already null-terminated at the right place. */
			g_free (history_text);
			g_ptr_array_free (lines, TRUE);
		}
	}

	/* Per-line fallback */
	while (*cmd)
	{
		char *cr = cmd + strcspn (cmd, "\n\r");
		int end_of_string = *cr == 0;
		*cr = 0;
		if (!handle_user_input (sess, cmd, history, nocommand))
			return;
		if (end_of_string)
			break;
		cmd = cr + 1;
	}
}

/*void
handle_multiline (session *sess, char *cmd, int history, int nocommand)
{
	char *cr;

	cr = strchr (cmd, '\n');
	if (cr)
	{
		while (1)
		{
			if (cr)
				*cr = 0;
			if (!handle_user_input (sess, cmd, history, nocommand))
				return;
			if (!cr)
				break;
			cmd = cr + 1;
			if (*cmd == 0)
				break;
			cr = strchr (cmd, '\n');
		}
	} else
	{
		handle_user_input (sess, cmd, history, nocommand);
	}
}*/
