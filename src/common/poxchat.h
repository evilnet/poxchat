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

#include "config.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include <time.h>			/* need time_t */

#ifndef POXCHAT_H
#define POXCHAT_H

#ifdef USE_OPENSSL
#ifdef __APPLE__
#define __AVAILABILITYMACROS__
#define DEPRECATED_IN_MAC_OS_X_VERSION_10_7_AND_LATER
#endif
#endif

/* Suppress -Wunused-result for fire-and-forget I/O */
#define HC_IGNORE_RESULT(expr) do { if (expr) {} } while (0)

#include "history.h"
#include "tree.h"

#ifdef USE_OPENSSL
#include <openssl/ssl.h>		  /* SSL_() */
#include "scram.h"
#endif


/* force a 32bit CMP.L */
#define WORDL(c0, c1, c2, c3) (guint32)(c0 | (c1 << 8) | (c2 << 16) | (c3 << 24))

#ifdef WIN32						/* for win32 */
#define OFLAGS O_BINARY
#define sleep(t) Sleep(t*1000)
#include <direct.h>
#define	F_OK	0
#define	X_OK	1
#define	W_OK	2
#define	R_OK	4
#ifndef S_ISDIR
#define	S_ISDIR(m)	((m) & _S_IFDIR)
#endif
#define NETWORK_PRIVATE
#else									/* for unix */
#define OFLAGS 0
#endif

#define FONTNAMELEN	127
#define PATHLEN		255
#define DOMAINLEN	100
#define NICKLEN		64				/* including the NULL, so 63 really */
#define CHANLEN		300
#define PDIWORDS	32
#define USERNAMELEN 10
#define HIDDEN_CHAR	8			/* invisible character for xtext */

struct nbexec
{
	int myfd;
	int childpid;
	int tochannel;						/* making this int keeps the struct 4-byte aligned */
	int iotag;
	char *linebuf;
	int buffill;
	struct session *sess;
};

/* IRCv3 STS (Strict Transport Security) policy */
typedef struct sts_policy
{
	char *host;			/* hostname this policy applies to */
	int port;			/* TLS port to use */
	time_t expires;		/* when this policy expires (0 = session-only) */
} sts_policy;

struct poxchatprefs
{
	/* these are the rebranded, consistent, sorted poxchat variables */

	/* BOOLEANS */
	unsigned int hex_away_auto_unmark;
	unsigned int hex_away_omit_alerts;
	unsigned int hex_away_show_once;
	unsigned int hex_away_track;
	unsigned int hex_completion_auto;
	unsigned int hex_dcc_auto_chat;
	unsigned int hex_dcc_auto_resume;
	unsigned int hex_dcc_fast_send;
	unsigned int hex_dcc_ip_from_server;
	unsigned int hex_dcc_remove;
	unsigned int hex_dcc_save_nick;
	unsigned int hex_dcc_send_fillspaces;
	unsigned int hex_gui_autoopen_chat;
	unsigned int hex_gui_autoopen_dialog;
	unsigned int hex_gui_autoopen_recv;
	unsigned int hex_gui_autoopen_send;
	unsigned int hex_gui_collapse_multiline;
	unsigned int hex_gui_collapse_threshold;
	unsigned int hex_gui_collapse_page_divisor;
	unsigned int hex_gui_compact;
	unsigned int hex_gui_day_separator;
	unsigned int hex_gui_emoji_sprites;
	unsigned int hex_gui_filesize_iec;
	unsigned int hex_gui_focus_omitalerts;
	unsigned int hex_gui_hide_menu;
	unsigned int hex_gui_input_attr;
	unsigned int hex_gui_input_icon;
	unsigned int hex_gui_input_nick;
	unsigned int hex_gui_input_spell;
	unsigned int hex_gui_input_lines;
	unsigned int hex_gui_input_style;
	unsigned int hex_gui_join_dialog;
	unsigned int hex_gui_mode_buttons;
	unsigned int hex_gui_network_icons;
	unsigned int hex_gui_quit_dialog;
	/* unsigned int hex_gui_single; */
	unsigned int hex_gui_slist_fav;
	unsigned int hex_gui_slist_skip;
	unsigned int hex_gui_scroll_lines;
	unsigned int hex_gui_tab_chans;
	unsigned int hex_gui_tab_dialogs;
	unsigned int hex_gui_tab_dots;
	unsigned int hex_gui_tab_icons;
	unsigned int hex_gui_tab_scrollchans;
	unsigned int hex_gui_tab_server;
	unsigned int hex_gui_tab_sort;
	unsigned int hex_gui_tab_utils;
	unsigned int hex_gui_topicbar;
	unsigned int hex_gui_tray;
	unsigned int hex_gui_tray_away;
	unsigned int hex_gui_tray_blink;
	unsigned int hex_gui_tray_close;
	unsigned int hex_gui_tray_minimize;
	unsigned int hex_gui_tray_quiet;
	unsigned int hex_gui_ulist_buttons;
	unsigned int hex_gui_ulist_color;
	unsigned int hex_gui_ulist_count;
	unsigned int hex_gui_ulist_hide;
	unsigned int hex_gui_ulist_icons;
	unsigned int hex_gui_ulist_show_hosts;
	unsigned int hex_gui_ulist_style;
	unsigned int hex_gui_usermenu;
	unsigned int hex_gui_win_modes;
	unsigned int hex_gui_win_nick;
	unsigned int hex_gui_win_save;
	unsigned int hex_gui_win_sticky;
	unsigned int hex_gui_win_swap;
	unsigned int hex_gui_win_ucount;
	unsigned int hex_identd_server;
	unsigned int hex_input_balloon_chans;
	unsigned int hex_input_balloon_hilight;
	unsigned int hex_input_balloon_priv;
	unsigned int hex_input_beep_chans;
	unsigned int hex_input_beep_hilight;
	unsigned int hex_input_beep_priv;
	unsigned int hex_input_filter_beep;
	unsigned int hex_input_flash_chans;
	unsigned int hex_input_flash_hilight;
	unsigned int hex_input_flash_priv;
	unsigned int hex_input_perc_ascii;
	unsigned int hex_input_perc_color;
	unsigned int hex_input_tray_chans;
	unsigned int hex_input_tray_hilight;
	unsigned int hex_input_tray_priv;
	unsigned int hex_irc_auto_rejoin;
	unsigned int hex_irc_reconnect_rejoin;
	unsigned int hex_irc_conf_mode;
	unsigned int hex_irc_hidehost;
	unsigned int hex_irc_hide_nickchange;
	unsigned int hex_irc_hide_version;
	unsigned int hex_irc_invisible;
	unsigned int hex_irc_logging;
	unsigned int hex_irc_raw_modes;
	unsigned int hex_irc_react_show;
	unsigned int hex_irc_reply_show;
	unsigned int hex_irc_servernotice;
	unsigned int hex_irc_skip_motd;
	unsigned int hex_irc_typing_self;
	unsigned int hex_irc_typing_send;
	unsigned int hex_irc_typing_show;
	unsigned int hex_irc_wallops;
	unsigned int hex_irc_who_join;
	unsigned int hex_irc_whois_front;
	unsigned int hex_irc_cap_server_time;
	unsigned int hex_irc_chathistory_auto;  /* auto-fetch history on join */
	unsigned int hex_irc_chathistory_scroll; /* load more history on scroll-to-top */
	unsigned int hex_irc_chathistory_background; /* background fetch older history */
	unsigned int hex_irc_chathistory_background_delay; /* seconds between background fetches */
	unsigned int hex_irc_chathistory_background_max_age; /* max hours to fetch in background (0 = unlimited) */
	unsigned int hex_net_auto_reconnect;
	unsigned int hex_net_auto_reconnectonfail;
	unsigned int hex_net_proxy_auth;
	unsigned int hex_net_throttle;
	unsigned int hex_notify_whois_online;
	unsigned int hex_perl_warnings;
	unsigned int hex_stamp_log;
	unsigned int hex_stamp_text;
	unsigned int hex_text_autocopy_color;
	unsigned int hex_text_autocopy_stamp;
	unsigned int hex_text_autocopy_text;
	unsigned int hex_text_color_nicks;
	unsigned int hex_text_indent;
	unsigned int hex_text_replay;
	unsigned int hex_text_search_case_match;
	unsigned int hex_text_search_highlight_all;
	unsigned int hex_text_search_follow;
	unsigned int hex_text_search_regexp;
	unsigned int hex_text_show_marker;
	unsigned int hex_text_show_sep;
	unsigned int hex_text_stripcolor_msg;
	unsigned int hex_text_stripcolor_replay;
	unsigned int hex_text_stripcolor_topic;
	unsigned int hex_text_thin_sep;
	unsigned int hex_text_transparent;
	unsigned int hex_text_wordwrap;
	unsigned int hex_url_grabber;
	unsigned int hex_url_image_upload_enable;
	unsigned int hex_url_image_strip_exif;
	unsigned int hex_url_logging;

	/* NUMBERS */
	int hex_away_size_max;
	int hex_away_timeout;
	int hex_completion_amount;
	int hex_completion_sort;
	int hex_dcc_auto_recv;
	int hex_dcc_blocksize;
	int hex_dcc_global_max_get_cps;
	int hex_dcc_global_max_send_cps;
	int hex_dcc_max_get_cps;
	int hex_dcc_max_send_cps;
	int hex_dcc_permissions;
	int hex_dcc_port_first;
	int hex_dcc_port_last;
	int hex_dcc_stall_timeout;
	int hex_dcc_timeout;
	int hex_flood_ctcp_num;				/* flood */
	int hex_flood_ctcp_time;			/* seconds of floods */
	int hex_flood_msg_num;				/* same deal */
	int hex_flood_msg_time;
	int hex_gui_chanlist_maxusers;
	int hex_gui_chanlist_minusers;
	int hex_gui_dialog_height;
	int hex_gui_dialog_left;
	int hex_gui_dialog_top;
	int hex_gui_dialog_width;
	int hex_gui_lagometer;
	int hex_gui_lang;
	int hex_gui_pane_divider_position;
	int hex_gui_pane_left_size;
	int hex_gui_pane_right_size;
	int hex_gui_pane_right_size_min;
	int hex_gui_search_pos;
	int hex_gui_slist_select;
	int hex_gui_tab_layout;
	int hex_gui_tab_middleclose;
	int hex_gui_tab_newtofront;
	int hex_gui_tab_pos;
	int hex_gui_tab_small;
	int hex_gui_tab_trunc;
	int hex_gui_transparency;
	int hex_gui_throttlemeter;
	int hex_gui_ulist_pos;
	int hex_gui_ulist_sort;
	int hex_gui_url_mod;
	int hex_gui_win_desktop;
	int hex_gui_win_height;
	int hex_gui_win_fullscreen;
	int hex_gui_win_left;
	int hex_gui_win_state;
	int hex_gui_win_top;
	int hex_gui_win_width;
	int hex_identd_port;
	int hex_irc_ban_type;
	int hex_irc_chathistory_lines;  /* lines to fetch per request */
	int hex_irc_join_delay;
	int hex_irc_notice_pos;
	int hex_net_ping_timeout;
	int hex_net_proxy_port;
	int hex_net_proxy_type;				/* 0=disabled, 1=wingate 2=socks4, 3=socks5, 4=http */
	int hex_net_proxy_use;				/* 0=all 1=IRC_ONLY 2=DCC_ONLY */
	int hex_net_reconnect_delay;
	int hex_notify_timeout;
	int hex_text_max_indent;
	int hex_text_max_lines;
	int hex_url_grabber_limit;

	/* STRINGS */
	char hex_away_reason[256];
	char hex_completion_suffix[4];		/* Only ever holds a one-character string. */
	char hex_dcc_completed_dir[PATHLEN + 1];
	char hex_dcc_dir[PATHLEN + 1];
	char hex_dcc_ip[DOMAINLEN + 1];
	char hex_gui_ulist_doubleclick[256];
	char hex_input_command_char[4];
	char hex_irc_extra_hilight[300];
	char hex_irc_id_ntext[64];
	char hex_irc_id_ytext[64];
	char hex_irc_logmask[256];
	char hex_irc_nick1[NICKLEN];
	char hex_irc_nick2[NICKLEN];
	char hex_irc_nick3[NICKLEN];
	char hex_irc_nick_hilight[300];
	char hex_irc_no_hilight[300];
	char hex_irc_part_reason[256];
	char hex_irc_quit_reason[256];
	char hex_irc_real_name[127];
	char hex_irc_user_name[127];
	char hex_net_bind_host[127];
	char hex_net_proxy_host[64];
	char hex_net_proxy_pass[256];
	char hex_net_proxy_user[256];
	char hex_stamp_log_format[64];
	char hex_stamp_text_format[64];
	char hex_text_background[PATHLEN + 1];
	char hex_text_font[4 * FONTNAMELEN + 1];
	char hex_text_font_main[FONTNAMELEN + 1];
	char hex_text_font_alternative[3 * FONTNAMELEN + 1];
	char hex_text_spell_langs[64];
	char hex_url_image_upload[256];

	/* these are the private variables */
	guint32 local_ip;

	unsigned int wait_on_exit;	/* wait for logs to be flushed to disk IF we're connected */

	/* Tells us if we need to save, only when they've been edited.
		This is so that we continue using internal defaults (which can
		change in the next release) until the user edits them. */
	unsigned int save_pevents:1;
};

/* Session types */
#define SESS_SERVER	1
#define SESS_CHANNEL	2
#define SESS_DIALOG	3
#define SESS_NOTICES	4
#define SESS_SNOTICES	5

/* Per-Channel Settings */
#define SET_OFF 0
#define SET_ON 1
#define SET_DEFAULT 2 /* use global setting */

/* Priorities in the "interesting sessions" priority queue
 * (see xchat.c:sess_list_by_lastact) */
#define LACT_NONE		-1		/* no queues */
#define LACT_QUERY_HI	0		/* query with hilight */
#define LACT_QUERY		1		/* query with messages */
#define LACT_CHAN_HI	2		/* channel with hilight */
#define LACT_CHAN		3		/* channel with messages */
#define LACT_CHAN_DATA	4		/* channel with other data */

/* Moved from fe-gtk for use in outbound.c as well -- */
typedef enum gtk_xtext_search_flags_e {
	case_match = 1,
	backward = 2,
	highlight = 4,
	follow = 8,
	regexp = 16
} gtk_xtext_search_flags;

typedef enum {
	TAB_STATE_NONE = 0,
	TAB_STATE_NEW_DATA = (1 << 0),
	TAB_STATE_NEW_MSG = (1 << 1),
	TAB_STATE_NEW_HILIGHT = (1 << 2),
} tab_state_flags;

typedef struct {
	char nick[NICKLEN];
	gint64 last_seen;		/* g_get_monotonic_time() */
} typing_entry;

typedef struct session
{
	/* Per-Channel Alerts */
	/* use a byte, because we need a pointer to each element */
	guint8 alert_balloon;
	guint8 alert_beep;
	guint8 alert_taskbar;
	guint8 alert_tray;

	/* Per-Channel Settings */
	guint8 text_hidejoinpart;
	guint8 text_logging;
	guint8 text_scrollback;
	guint8 text_strip;

	struct server *server;
	tree *usertree;					/* alphabetical tree */
	struct User *me;					/* points to myself in the usertree */
	char channel[CHANLEN];
	char waitchannel[CHANLEN];		  /* waiting to join channel (/join sent) */
	char willjoinchannel[CHANLEN];	  /* will issue /join for this channel */
	char session_name[CHANLEN];		 /* the name of the session, should not modified */
	char channelkey[64];			  /* XXX correct max length? */
	int limit;						  /* channel user limit */
	int logfd;

	GFile *scrollfile;							/* scrollback file */
	int scrollwritten;					/* number of lines written */

	char lastnick[NICKLEN];			  /* last nick you /msg'ed */

	struct history history;

	int ops;								/* num. of ops in channel */
	int hops;						  /* num. of half-oped users */
	int voices;							/* num. of voiced people */
	int total;							/* num. of users in channel */

	char *quitreason;
	char *topic;
	char *current_modes;					/* free() me */

	int mode_timeout_tag;

	struct session *lastlog_sess;
	struct nbexec *running_exec;

	struct session_gui *gui;		/* initialized by fe_new_window */
	struct restore_gui *res;

	int type;					/* SESS_* */

	int lastact_idx;		/* the sess_list_by_lastact[] index of the list we're in.
							 * For valid values, see defines of LACT_*. */

	int ignore_date:1;
	int ignore_mode:1;
	int ignore_names:1;
	int end_of_names:1;
	int doing_who:1;		/* /who sent on this channel */
	int done_away_check:1;	/* done checking for away status changes */
	int history_loading:1;	/* chathistory request in progress (derived: ch_active != NULL) */
	int history_request_is_before:1; /* current request is BEFORE (needs prepend) */
	int history_request_is_after:1;  /* current request is AFTER (needs insert_sorted) */
	int history_prepend_mode:1; /* currently processing BEFORE batch with prepend */
	int history_insert_sorted_mode:1; /* currently processing AFTER batch with timestamp sort */
	int history_exhausted:1; /* server has no more history for this target */
	int history_request_used_msgid:1; /* current request used msgid reference (vs timestamp) */
	struct chreq_tag *ch_active;	/* in-flight chathistory request (NULL = idle) */
	struct chreq_tag *ch_pending;	/* queued chathistory request (NULL = nothing waiting) */
	int catchup_in_progress:1; /* catch-up loop active (join or TARGETS) */
	int catchup_is_before:1; /* currently in BEFORE pagination phase (not initial LATEST) */
	int display_only:1; /* next text output skips scrollback/log save */
	int background_history_active:1; /* background history fetch enabled for this session */
	guint background_history_timer;	/* timer for next background history fetch */
	time_t catchup_lower_bound;		/* timestamp stop condition for BEFORE pagination */
	int history_catchup_stale_count;	/* consecutive all-dupe BEFORE responses */
	int history_catchup_retrieved;	/* total messages retrieved in BEFORE catch-up */
	void *chunk_state;				/* chathistory_chunk_state* during async batch processing */
	tab_state_flags tab_state;
	tab_state_flags last_tab_state; /* before event is handled */
	gtk_xtext_search_flags lastlog_flags;
	void (*scrollback_replay_marklast) (struct session *sess);
	char *join_msgid;		/* msgid of our current JOIN (suppress from chathistory) */
	char *oldest_msgid;		/* oldest message in buffer (for BEFORE requests) */
	char *newest_msgid;		/* newest message in buffer (for AFTER requests) */
	char *markread_timestamp;	/* server MARKREAD timestamp string (ISO 8601, for round-trip) */
	time_t markread_time;		/* parsed MARKREAD timestamp as time_t (for local comparison) */
	time_t scrollback_newest_time;	/* newest timestamp from loaded scrollback */
	unsigned int scrollback_virtual_set:1;	/* virtual mode already enabled for this session */
	char *scrollback_oldest_msgid;	/* oldest msgid from loaded scrollback (for BEFORE) */
	char *scrollback_newest_msgid;	/* newest msgid from loaded scrollback (for AFTER) */
	char *current_msgid;	/* temporary: msgid of message being processed (owned copy) */
	gboolean current_msgid_is_user_msg;	/* true when current_msgid belongs to a PRIVMSG/NOTICE/ACTION,
								 * false for JOIN/PART/QUIT/TOPIC/etc.  Drives whether the
								 * entry gets reply/react hover buttons. */
	GHashTable *known_msgids;	/* hash set of msgids already displayed (for deduplication) */

	/* Typing indicators (IRCv3 +typing) */
	GSList *typing_nicks;			/* GSList of typing_entry* — who's typing here */
	int typing_sweep_timer;			/* timer to expire stale typers (fires every 2s) */
	gint64 typing_last_sent;		/* monotonic time of last +typing=active we sent */
	int typing_send_timer;			/* timer for periodic re-sends (fires every 3s) */
	gint64 typing_last_keystroke;	/* monotonic time of last keystroke in input box */
	gint64 typing_self_echo_until;	/* deadline (monotonic) within which an inbound is_self
									   +typing is treated as this client's own echo; outside
									   this window an is_self +typing means another client of
									   ours (same nick) is typing */

	/* IRCv3 reply and react compose state */
	char *reply_msgid;				/* msgid we're replying to (NULL = not replying) */
	char *reply_nick;				/* nick of the message we're replying to */
	char *react_target_msgid;		/* msgid for pending reaction (cleared after send) */
	char *react_target_nick;		/* nick of the message we're reacting to */
	char *picker_pending_cmd;		/* command template with %s for msgid (NULL = no picker active) */
} session;

/* SASL Mechanisms */
#define MECH_PLAIN 0
#define MECH_EXTERNAL 1
#define MECH_SCRAM_SHA_1 2
#define MECH_SCRAM_SHA_256 3
#define MECH_SCRAM_SHA_512 4
#define MECH_OAUTHBEARER 5

/* IRCv3 Batch information
 * Used to collect messages belonging to a batch for processing as a unit
 * See https://ircv3.net/specs/extensions/batch
 */
typedef struct batch_info
{
	char *id;            /* Unique batch identifier */
	char *type;          /* Batch type: chathistory, multiline, netjoin, netsplit, etc. */
	char **params;       /* Additional parameters after the type */
	int param_count;     /* Number of parameters */
	char *outer_batch;   /* For nested batches: reference to parent batch ID */
	char *label;         /* labeled-response: label tag from BATCH START */
	char *msgid;         /* msgid tag from BATCH START (for echo confirmation) */
	unsigned int chathistory_end:1; /* draft/chathistory-end tag on BATCH START */
	GSList *messages;    /* Collected messages (each element is batch_message) */
	time_t started;      /* When the batch was opened (wall clock, for stale detection) */
	time_t server_time;  /* server-time from BATCH START tags (for nested batch timestamps) */
} batch_info;

/* IRCv3 labeled-response: pending label tracking */
typedef struct {
	char *command;       /* "PRIVMSG", "NOTICE", "TAGMSG", "CHATHISTORY", etc. */
	char *target;        /* Channel/nick target */
	session *sess;       /* Session that initiated the command — MUST validate
	                      * with is_session() before dereferencing, as the tab
	                      * may have been closed while a label was pending */
	time_t sent_time;    /* When we sent it */
	guint64 entry_id;    /* xtext entry ID for pending state confirmation (0 = none) */
} pending_label_info;

/* A single message collected within a batch */
typedef struct batch_message
{
	char *prefix;        /* Message source (nick!user@host) */
	char *command;       /* IRC command (PRIVMSG, NOTICE, JOIN, etc.) */
	char **params;       /* Command parameters */
	int param_count;     /* Number of parameters */
	GHashTable *tags;    /* Message tags (copied from message_tags_data.all_tags) */
	time_t timestamp;    /* Server time if available */
	char *msgid;         /* Message ID if available */
} batch_message;

void batch_message_free (batch_message *msg);

typedef struct server
{
	/*  server control operations (in server*.c) */
	void (*connect)(struct server *, char *hostname, int port, int no_login);
	void (*disconnect)(struct session *, int sendquit, int err);
	int  (*cleanup)(struct server *);
	void (*flush_queue)(struct server *);
	void (*auto_reconnect)(struct server *, int send_quit, int err);
	/* irc protocol functions (in proto*.c) */
	void (*p_inline)(struct server *, char *buf, int len);
	void (*p_invite)(struct server *, char *channel, char *nick);
	void (*p_cycle)(struct server *, char *channel, char *key);
	void (*p_ctcp)(struct server *, char *to, char *msg);
	void (*p_nctcp)(struct server *, char *to, char *msg);
	void (*p_quit)(struct server *, char *reason);
	void (*p_kick)(struct server *, char *channel, char *nick, char *reason);
	void (*p_part)(struct server *, char *channel, char *reason);
	void (*p_ns_identify)(struct server *, char *pass);
	void (*p_ns_ghost)(struct server *, char *usname, char *pass);
	void (*p_join)(struct server *, char *channel, char *key);
	void (*p_join_list)(struct server *, GSList *favorites);
	void (*p_login)(struct server *, char *user, char *realname);
	void (*p_join_info)(struct server *, char *channel);
	void (*p_mode)(struct server *, char *target, char *mode);
	void (*p_user_list)(struct server *, char *channel);
	void (*p_away_status)(struct server *, char *channel);
	void (*p_whois)(struct server *, char *nicks);
	void (*p_get_ip)(struct server *, char *nick);
	void (*p_get_ip_uh)(struct server *, char *nick);
	void (*p_set_back)(struct server *);
	void (*p_set_away)(struct server *, char *reason);
	void (*p_message)(struct server *, char *channel, char *text);
	void (*p_action)(struct server *, char *channel, char *act);
	void (*p_notice)(struct server *, char *channel, char *text);
	void (*p_topic)(struct server *, char *channel, char *topic);
	void (*p_list_channels)(struct server *, char *arg, int min_users);
	void (*p_change_nick)(struct server *, char *new_nick);
	void (*p_names)(struct server *, char *channel);
	void (*p_ping)(struct server *, char *to, char *timestring);
/*	void (*p_set_away)(struct server *);*/
	int (*p_raw)(struct server *, char *raw);
	int (*p_cmp)(const char *s1, const char *s2);

	int port;
	int sok;					/* is equal to sok4 or sok6 (the one we are using) */
	int sok4;					/* tcp4 socket */
	int sok6;					/* tcp6 socket */
	int proxy_type;
	int proxy_sok;				/* Additional information for MS Proxy beast */
	int proxy_sok4;
	int proxy_sok6;
	int id;					/* unique ID number (for plugin API) */

	/* dcc_ip moved from poxchatprefs to make it per-server */
	guint32 dcc_ip;

#ifdef USE_OPENSSL
	SSL_CTX *ctx;
	SSL *ssl;
	int ssl_do_connect_tag;
#else
	void *ssl;
#endif
	int childread;
	int childwrite;
	int childpid;
	int iotag;
	int recondelay_tag;				/* reconnect delay timeout */
	int joindelay_tag;				/* waiting before we send JOIN */
	char hostname[128];				/* real ip number */
	char servername[128];			/* what the server says is its name */
	char password[1024];
	char nick[NICKLEN];
	char linebuf[8704];				/* RFC says 512 chars including \r\n, IRCv3 message tags add 8191, plus the NUL byte */
	char *last_away_reason;
	int pos;								/* current position in linebuf */
	int nickcount;
	int loginmethod;					/* see login_types[] */

	char *chantypes;					/* for 005 numeric - free me */
	char *chanmodes;					/* for 005 numeric - free me */
	char *nick_prefixes;				/* e.g. "*@%+" */
	char *nick_modes;					/* e.g. "aohv" */
	char *bad_nick_prefixes;		/* for ircd that doesn't give the modes */
	int modes_per_line;				/* 6 on undernet, 4 on efnet etc... */
	int chathistory_limit;			/* max messages per CHATHISTORY request (from ISUPPORT) */
	guint chathistory_start_timer;	/* deferred LATEST: 2s after last 366 */
	guint chathistory_before_timer;	/* delay between BEFORE catch-up requests */
	int chathistory_latest_pending;	/* count of sessions awaiting LATEST response */
	struct session *chathistory_before_sess;	/* session doing active BEFORE catch-up (NULL = none) */
	int multiline_max_bytes;		/* max bytes in multiline batch (from ISUPPORT) */
	int multiline_max_lines;		/* max lines in multiline batch (from ISUPPORT) */
	int sts_upgrade_port;			/* STS TLS port to upgrade to (0 = no upgrade needed) */
	char *network_icon_url;			/* Network icon URL from ISUPPORT draft/ICON */
	void *network_icon;				/* Cached network icon pixbuf (GdkPixbuf *, NULL if not fetched) */
	void *network_icon_cancel;		/* Opaque handle to cancel in-flight icon fetch */

	void *network;						/* points to entry in servlist.c or NULL! */

	GSList *outbound_queue;
	time_t next_send;						/* cptr->since in ircu */
	time_t prev_now;					/* previous now-time */
	int sendq_len;						/* queue size */
	unsigned int batch_burst:1;			/* skip throttle queue during multiline batch */
	int batch_burst_bytes;				/* total bytes sent during burst (for post-batch penalty) */
	int lag;								/* milliseconds */

	struct session *front_session;	/* front-most window/tab */
	struct session *server_session;	/* server window/tab */

	struct server_gui *gui;		  /* initialized by fe_new_server */

	unsigned int ctcp_counter;	  /*flood */
	time_t ctcp_last_time;

	unsigned int msg_counter;	  /*counts the msg tab opened in a certain time */
	time_t msg_last_time;

	/*time_t connect_time;*/				/* when did it connect? */
	unsigned long lag_sent;   /* we are still waiting for this ping response*/
	time_t ping_recv;					/* when we last got a ping reply */
	time_t away_time;					/* when we were marked away */
	time_t last_disconnect_time;		/* when we last disconnected (for TARGETS lower bound) */

	char *encoding;
	GIConv read_converter;  /* iconv converter for converting from server encoding to UTF-8. */
	GIConv write_converter; /* iconv converter for converting from UTF-8 to server encoding. */

	GSList *favlist;			/* list of channels & keys to join */

	unsigned int motd_skipped:1;
	unsigned int connected:1;
	unsigned int connecting:1;
	unsigned int no_login:1;
	unsigned int skip_next_userhost:1;/* used for "get my ip from server" */
	unsigned int skip_next_whois:1;	/* hide whois output */
	unsigned int inside_whois:1;
	unsigned int doing_dns:1;			/* /dns has been done */
	unsigned int end_of_motd:1;		/* end of motd reached (logged in) */
	unsigned int sent_quit:1;			/* sent a QUIT already? */
	unsigned int use_listargs:1;		/* undernet and dalnet need /list >0,<10000 */
	unsigned int is_away:1;
	unsigned int reconnect_away:1;	/* whether to reconnect in is_away state */
	unsigned int dont_use_proxy:1;	/* to proxy or not to proxy */
	unsigned int supports_watch:1;	/* supports the WATCH command */
	unsigned int supports_monitor:1;	/* supports the MONITOR command */
	unsigned int bad_prefix:1;			/* gave us a bad PREFIX= 005 number */
	unsigned int have_namesx:1;		/* 005 tokens NAMESX and UHNAMES */
	unsigned int have_awaynotify:1;
	unsigned int have_uhnames:1;
	unsigned int have_whox:1;		/* have undernet's WHOX features */
	unsigned int have_idmsg:1;		/* cap solanum.chat/identify-msg */
	unsigned int have_accnotify:1; /* cap account-notify */
	unsigned int have_extjoin:1;	/* cap extended-join */
	unsigned int have_account_tag:1;	/* cap account-tag */
	unsigned int have_server_time:1;	/* cap server-time */
	unsigned int have_sasl:1;		/* SASL capability */
	unsigned int have_batch:1;		/* IRCv3 batch capability */
	unsigned int have_message_tags:1;	/* IRCv3 message-tags capability */
	unsigned int have_echo_message:1;	/* IRCv3 echo-message capability */
	unsigned int have_labeled_response:1; /* IRCv3 labeled-response capability */
	unsigned int have_chathistory:1;	/* IRCv3 draft/chathistory capability */
	unsigned int have_multiline:1;		/* IRCv3 draft/multiline capability */
	unsigned int have_event_playback:1;	/* IRCv3 draft/event-playback capability */
	unsigned int have_read_marker:1;	/* IRCv3 draft/read-marker capability */
	unsigned int have_redact:1;		/* IRCv3 draft/message-redaction capability */
	unsigned int have_account_registration:1; /* IRCv3 draft/account-registration capability */
	unsigned int accreg_before_connect:1;     /* account-registration: before-connect supported */
	unsigned int accreg_email_required:1;     /* account-registration: email-required */
	unsigned int accreg_custom_account:1;     /* account-registration: custom-account-name supported */
	unsigned int have_metadata:1;	/* IRCv3 draft/metadata-2 capability */
	unsigned int have_channel_rename:1; /* IRCv3 draft/channel-rename capability */
	unsigned int have_pre_away:1;	/* IRCv3 draft/pre-away capability */
	unsigned int have_extended_isupport:1; /* IRCv3 draft/extended-isupport capability */
	unsigned int utf8only:1;		/* Server requires UTF-8 encoding (ISUPPORT UTF8ONLY) */
	unsigned int have_except:1;	/* ban exemptions +e */
	unsigned int have_invite:1;	/* invite exemptions +I */
	unsigned int have_cert:1;	/* have loaded a cert */
	unsigned int use_who:1;			/* whether to use WHO command to get dcc_ip */
	unsigned int sasl_mech;			/* mechanism for sasl auth */
	unsigned int sent_capend:1;	/* have sent CAP END yet */
	unsigned int waiting_on_cap:1;	/* waiting on another line of CAP LS */
	unsigned int waiting_on_sasl:1; /* waiting on sasl */
	unsigned int persistent_server:1;	/* network config flag — bouncer/persistent server, suppresses our auto-rejoin on reconnect */
	unsigned int bouncer_inferred:1;	/* heuristic — saw a server-replayed self-JOIN whose @time predates our last_disconnect_time */
#ifdef USE_OPENSSL
	unsigned int use_ssl:1;				  /* is server SSL capable? */
	unsigned int accept_invalid_cert:1;/* ignore result of server's cert. verify */
	scram_session *scram_session; /* session for SASL SCRAM authentication */
#endif
#ifdef USE_LIBWEBSOCKETS
	/* OAuth2/OIDC state */
	struct oauth_session *oauth_session;	/* Active OAuth flow, if any */
	char *oauth_access_token;				/* Current access token for SASL OAUTHBEARER */
	time_t oauth_token_expires;				/* When current token expires */
	unsigned int oauth_refresh_attempted:1;	/* Prevent infinite refresh loop on 904 */
	char *oauth_connect_host;				/* Stashed hostname for deferred connect after refresh */
	int oauth_connect_port;					/* Stashed port for deferred connect after refresh */
#endif

	/* IRCv3 batch state */
	GHashTable *active_batches;	/* batch_id (string) -> batch_info* */
	guint32 label_counter;		/* Counter for generating unique labels */
	GHashTable *pending_labels;	/* label (string) -> pending command info */
	char *last_sent_label;		/* Borrowed pointer — valid only within synchronous outbound call */
	int stale_sweep_timer;		/* periodic timer for cleaning stale labels/batches */
} server;

typedef int (*cmd_callback) (struct session * sess, char *tbuf, char *word[],
									  char *word_eol[]);

struct commands
{
	char *name;
	cmd_callback callback;
	char needserver;
	char needchannel;
	gint16 handle_quotes;
	char *help;
};

struct away_msg
{
	struct server *server;
	char nick[NICKLEN];
	char *message;
};

/* not just for popups, but used for usercommands, ctcp replies,
   userlist buttons etc */

struct popup
{
	char *cmd;
	char *name;
};

/* CL: get a random int in the range [0..n-1]. DON'T use rand() % n, it gives terrible results. */
#define RAND_INT(n) ((int)(rand() / (RAND_MAX + 1.0) * (n)))

#endif
