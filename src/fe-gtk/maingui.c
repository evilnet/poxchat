/* X-Chat
 * Copyright (C) 1998-2005 Peter Zelezny.
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include <gdk/gdkkeysyms.h>

#include "../common/poxchat.h"
#include "../common/fe.h"
#include "../common/server.h"
#include "../common/poxchatc.h"
#include "../common/outbound.h"
#include "../common/inbound.h"
#include "../common/plugin.h"
#include "../common/modes.h"
#include "../common/url.h"
#include "../common/util.h"
#include "../common/chathistory.h"
#include "../common/text.h"
#include "../common/chanopt.h"
#include "../common/cfgfiles.h"
#include "../common/servlist.h"
#include "../common/chathistory.h"

#include "fe-gtk.h"
#include "hex-input-edit.h"
#include "banlist.h"
#include "gtkutil.h"
#include "joind.h"
#include "palette.h"
#include "maingui.h"
#include "menu.h"
#include "fkeys.h"
#include "userlistgui.h"
#include "hex-emoji-chooser.h"
#include "chanview.h"
#include "pixmaps.h"
#include "plugin-tray.h"
#include "servlistgui.h"
#include "wm-hints.h"
#include "xtext.h"

#ifdef G_OS_WIN32
#include <windows.h>
#endif

#define GUI_SPACING (3)
#define GUI_BORDER (0)
#define GUI_PANE_MARGIN (6)
/* Horizontal paned handle width. CSS in fe-gtk.c gives the separator
 * `margin: 0 6px`, so its allocation is 2 * GUI_PANE_MARGIN regardless of
 * which paned we're measuring. Used to correct right-side position math:
 * on the right pane, `pane_w - position` includes the handle, so the
 * actual end-child allocation is 12px less. */
#define MG_PANED_HANDLE_W (GUI_PANE_MARGIN * 2)

enum
{
	POS_INVALID = 0,
	POS_TOPLEFT = 1,
	POS_BOTTOMLEFT = 2,
	POS_TOPRIGHT = 3,
	POS_BOTTOMRIGHT = 4,
	POS_TOP = 5,	/* for tabs only */
	POS_BOTTOM = 6,
	POS_HIDDEN = 7
};

/* two different types of tabs */
#define TAG_IRC 0		/* server, channel, dialog */
#define TAG_UTIL 1	/* dcc, notify, chanlist */

static void mg_create_entry (session *sess, GtkWidget *box);
static void mg_create_search (session *sess, GtkWidget *box);
static GtkWidget *mg_create_drop_strip (int pos);
static void mg_link_irctab (session *sess, int focus);
void mg_update_window_minimum (session_gui *gui);

static session_gui static_mg_gui;
static session_gui *mg_gui = NULL;	/* the shared irc tab */
static int ignore_chanmode = FALSE;
static const char chan_flags[] = { 'c', 'n', 't', 'i', 'm', 'l', 'k' };

static chan *active_tab = NULL;	/* active tab */
GtkWidget *parent_window = NULL;			/* the master window */

InputStyle *input_style;

static PangoAttrList *away_list;
static PangoAttrList *newdata_list;
static PangoAttrList *nickseen_list;
static PangoAttrList *newmsg_list;
static PangoAttrList *plain_list = NULL;

static PangoAttrList *
mg_attr_list_create (GdkRGBA *col, int size)
{
	PangoAttribute *attr;
	PangoAttrList *list;

	list = pango_attr_list_new ();

	if (col)
	{
		/* Convert GdkRGBA floats (0.0-1.0) to 16-bit integers (0-65535) for pango */
		attr = pango_attr_foreground_new ((guint16)(col->red * 65535),
		                                  (guint16)(col->green * 65535),
		                                  (guint16)(col->blue * 65535));
		attr->start_index = 0;
		attr->end_index = 0xffff;
		pango_attr_list_insert (list, attr);
	}

	if (size > 0)
	{
		attr = pango_attr_scale_new (size == 1 ? PANGO_SCALE_SMALL : PANGO_SCALE_X_SMALL);
		attr->start_index = 0;
		attr->end_index = 0xffff;
		pango_attr_list_insert (list, attr);
	}

	return list;
}

static void
mg_create_tab_colors (void)
{
	if (plain_list)
	{
		pango_attr_list_unref (plain_list);
		pango_attr_list_unref (newmsg_list);
		pango_attr_list_unref (newdata_list);
		pango_attr_list_unref (nickseen_list);
		pango_attr_list_unref (away_list);
	}

	plain_list = mg_attr_list_create (NULL, prefs.hex_gui_tab_small);
	newdata_list = mg_attr_list_create (&colors[COL_NEW_DATA], prefs.hex_gui_tab_small);
	nickseen_list = mg_attr_list_create (&colors[COL_HILIGHT], prefs.hex_gui_tab_small);
	newmsg_list = mg_attr_list_create (&colors[COL_NEW_MSG], prefs.hex_gui_tab_small);
	away_list = mg_attr_list_create (&colors[COL_AWAY], FALSE);
}

static void
set_window_urgency (GtkWidget *win, gboolean set)
{
	/* GTK4 removed urgency hints; no direct equivalent */
	(void)win;
	(void)set;
}

static void
flash_window (GtkWidget *win)
{
#ifdef HAVE_GTK_MAC
	gtkosx_application_attention_request (osx_app, INFO_REQUEST);
#endif
	set_window_urgency (win, TRUE);
}

static void
unflash_window (GtkWidget *win)
{
	set_window_urgency (win, FALSE);
}

/* flash the taskbar button */

void
fe_flash_window (session *sess)
{
	if (fe_gui_info (sess, 0) != 1)	/* only do it if not focused */
		flash_window (sess->gui->window);
}

/* set a tab plain, red, light-red, or blue */

void
fe_set_tab_color (struct session *sess, tabcolor col)
{
	struct session *server_sess = sess->server->server_session;
	int col_noflags = (col & ~FE_COLOR_ALLFLAGS);
	int col_shouldoverride = !(col & FE_COLOR_FLAG_NOOVERRIDE);

	if (sess->res->tab && sess->gui->is_tab && (col == 0 || sess != current_tab))
	{
		switch (col_noflags)
		{
		case 0:	/* no particular color (theme default) */
			sess->tab_state = TAB_STATE_NONE;
			chan_set_color (sess->res->tab, plain_list);
			break;
		case 1:	/* new data has been displayed (dark red) */
			if (col_shouldoverride || !((sess->tab_state & TAB_STATE_NEW_MSG)
										|| (sess->tab_state & TAB_STATE_NEW_HILIGHT))) {
				sess->tab_state = TAB_STATE_NEW_DATA;
				chan_set_color (sess->res->tab, newdata_list);
			}

			if (chan_is_collapsed (sess->res->tab)
				&& !((server_sess->tab_state & TAB_STATE_NEW_MSG)
					 || (server_sess->tab_state & TAB_STATE_NEW_HILIGHT))
				&& !(server_sess == current_tab))
			{
				server_sess->tab_state = TAB_STATE_NEW_DATA;
				chan_set_color (chan_get_parent (sess->res->tab), newdata_list);
			}

			break;
		case 2:	/* new message arrived in channel (light red) */
			if (col_shouldoverride || !(sess->tab_state & TAB_STATE_NEW_HILIGHT)) {
				sess->tab_state = TAB_STATE_NEW_MSG;
				chan_set_color (sess->res->tab, newmsg_list);
			}

			if (chan_is_collapsed (sess->res->tab)
				&& !(server_sess->tab_state & TAB_STATE_NEW_HILIGHT)
				&& !(server_sess == current_tab))
			{
				server_sess->tab_state = TAB_STATE_NEW_MSG;
				chan_set_color (chan_get_parent (sess->res->tab), newmsg_list);
			}

			break;
		case 3:	/* your nick has been seen (blue) */
			sess->tab_state = TAB_STATE_NEW_HILIGHT;
			chan_set_color (sess->res->tab, nickseen_list);

			if (chan_is_collapsed (sess->res->tab) && !(server_sess == current_tab))
			{
				server_sess->tab_state = TAB_STATE_NEW_MSG;
				chan_set_color (chan_get_parent (sess->res->tab), nickseen_list);
			}

			break;
		}
		lastact_update (sess);
		sess->last_tab_state = sess->tab_state; /* For plugins handling future prints */
	}
}

static void
mg_set_myself_away (session_gui *gui, gboolean away)
{
	GtkWidget *label = g_object_get_data (G_OBJECT (gui->nick_label), "nick-label");
	if (label && GTK_IS_LABEL (label))
		gtk_label_set_attributes (GTK_LABEL (label), away ? away_list : NULL);
}

/* change the little icon to the left of your nickname */

void
mg_set_access_icon (session_gui *gui, GdkPixbuf *pix, gboolean away)
{
	GtkWidget *btn_box;

	if (gui->op_xpm)
	{
		/* GTK4: GtkImage no longer exposes its pixbuf; compare via stashed pointer */
		if (pix == g_object_get_data (G_OBJECT (gui->op_xpm), "source-pixbuf"))
		{
			mg_set_myself_away (gui, away);
			return;
		}

		hc_widget_destroy_impl (gui->op_xpm);
		gui->op_xpm = NULL;
	}

	btn_box = gtk_button_get_child (GTK_BUTTON (gui->nick_label));
	if (pix && prefs.hex_gui_input_icon && btn_box)
	{
		gui->op_xpm = gtk_image_new_from_paintable (GDK_PAINTABLE (gdk_texture_new_for_pixbuf (pix)));
		g_object_set_data (G_OBJECT (gui->op_xpm), "source-pixbuf", pix);
		gtk_box_prepend (GTK_BOX (btn_box), gui->op_xpm);
	}

	mg_set_myself_away (gui, away);
}

static void
mg_inputbox_focus (GtkEventControllerFocus *controller, session_gui *gui)
{
	GSList *list;
	session *sess;

	if (gui->is_tab)
		return;

	list = sess_list;
	while (list)
	{
		sess = list->data;
		if (sess->gui == gui)
		{
			current_sess = sess;
			if (!sess->server->server_session)
				sess->server->server_session = sess;
			break;
		}
		list = list->next;
	}
}

void
mg_inputbox_cb (GtkWidget *igad, session_gui *gui)
{
	char *cmd;
	static int ignore = FALSE;
	GSList *list;
	session *sess = NULL;

	if (ignore)
		return;

	cmd = SPELL_ENTRY_GET_TEXT (igad);
	if (cmd[0] == 0)
		return;

	cmd = g_strdup (cmd);

	/* avoid recursive loop */
	ignore = TRUE;
	SPELL_ENTRY_SET_TEXT (igad, "");
	ignore = FALSE;

	/* where did this event come from? */
	if (gui->is_tab)
	{
		sess = current_tab;
	} else
	{
		list = sess_list;
		while (list)
		{
			sess = list->data;
			if (sess->gui == gui)
				break;
			list = list->next;
		}
		if (!list)
			sess = NULL;
	}

	if (sess)
		handle_multiline (sess, cmd, TRUE, FALSE);

	g_free (cmd);
}


void
fe_set_title (session *sess)
{
	char tbuf[512];
	int type;

	if (sess->gui->is_tab && sess != current_tab)
		return;

	type = sess->type;

	if (sess->server->connected == FALSE && sess->type != SESS_DIALOG)
		goto def;

	switch (type)
	{
	case SESS_DIALOG:
		g_snprintf (tbuf, sizeof (tbuf), "%s %s @ %s - %s",
					 _("Dialog with"), sess->channel, server_get_network (sess->server, TRUE),
					 _(DISPLAY_NAME));
		break;
	case SESS_SERVER:
		g_snprintf (tbuf, sizeof (tbuf), "%s%s%s - %s",
					 prefs.hex_gui_win_nick ? sess->server->nick : "",
					 prefs.hex_gui_win_nick ? " @ " : "", server_get_network (sess->server, TRUE),
					 _(DISPLAY_NAME));
		break;
	case SESS_CHANNEL:
		/* don't display keys in the titlebar */
			g_snprintf (tbuf, sizeof (tbuf),
					 "%s%s%s / %s%s%s%s - %s",
					 prefs.hex_gui_win_nick ? sess->server->nick : "",
					 prefs.hex_gui_win_nick ? " @ " : "",
					 server_get_network (sess->server, TRUE), sess->channel,
					 prefs.hex_gui_win_modes && sess->current_modes ? " (" : "",
					 prefs.hex_gui_win_modes && sess->current_modes ? sess->current_modes : "",
					 prefs.hex_gui_win_modes && sess->current_modes ? ")" : "",
					 _(DISPLAY_NAME));
		if (prefs.hex_gui_win_ucount)
		{
			g_snprintf (tbuf + strlen (tbuf), 9, " (%d)", sess->total);
		}
		break;
	case SESS_NOTICES:
	case SESS_SNOTICES:
		g_snprintf (tbuf, sizeof (tbuf), "%s%s%s (notices) - %s",
					 prefs.hex_gui_win_nick ? sess->server->nick : "",
					 prefs.hex_gui_win_nick ? " @ " : "", server_get_network (sess->server, TRUE),
					 _(DISPLAY_NAME));
		break;
	default:
	def:
		g_snprintf (tbuf, sizeof (tbuf), _(DISPLAY_NAME));
		gtk_window_set_title (GTK_WINDOW (sess->gui->window), tbuf);
		return;
	}

	gtk_window_set_title (GTK_WINDOW (sess->gui->window), tbuf);
}

/* GTK4: Surface state callback - handles minimize detection */
static void
mg_surface_state_cb (GObject *gobject, GParamSpec *pspec, gpointer userdata)
{
	GdkToplevel *toplevel = GDK_TOPLEVEL (gobject);
	GdkToplevelState state = gdk_toplevel_get_state (toplevel);
	GtkWindow *wid = GTK_WINDOW (userdata);

	/* Minimize to tray: intercept minimize and hide to tray instead */
	if (state & GDK_TOPLEVEL_STATE_MINIMIZED)
	{
		if (prefs.hex_gui_tray_minimize && gtkutil_tray_icon_supported (wid))
		{
			/* Unminimize first, then hide to tray */
			gtk_window_unminimize (wid);
			tray_toggle_visibility (TRUE);
		}
	}
}

/* GTK4: Window state changes are monitored differently - track via property notifications */
static void
mg_windowstate_cb (GObject *gobject, GParamSpec *pspec, gpointer userdata)
{
	GtkWindow *wid = GTK_WINDOW (gobject);
	GdkToplevelState state = 0;
	GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (wid));

	if (surface && GDK_IS_TOPLEVEL (surface))
		state = gdk_toplevel_get_state (GDK_TOPLEVEL (surface));

	prefs.hex_gui_win_state = 0;
	if (state & GDK_TOPLEVEL_STATE_MAXIMIZED)
		prefs.hex_gui_win_state = 1;

	prefs.hex_gui_win_fullscreen = 0;
	if (state & GDK_TOPLEVEL_STATE_FULLSCREEN)
		prefs.hex_gui_win_fullscreen = 1;

	/* current_sess can be NULL during early window construction — libenchant
	 * init inside hex_input_edit_new triggers a Windows surface-state event
	 * before the session is fully wired up. */
	if (current_sess && current_sess->gui)
		menu_set_fullscreen (current_sess->gui, prefs.hex_gui_win_fullscreen);
}

/* GTK4: Connect to surface state after window is realized */
static void
mg_realize_cb (GtkWidget *widget, gpointer userdata)
{
	GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));

	if (surface && GDK_IS_TOPLEVEL (surface))
	{
		g_signal_connect (G_OBJECT (surface), "notify::state",
						  G_CALLBACK (mg_surface_state_cb), widget);
	}
}

/* GTK4: Use "notify::default-width/height" signal - position isn't available */
static void
mg_configure_cb (GtkWidget *wid, GParamSpec *pspec, session *sess)
{
	if (sess == NULL)			/* for the main_window */
	{
		if (mg_gui)
		{
			if (prefs.hex_gui_win_save && !prefs.hex_gui_win_state && !prefs.hex_gui_win_fullscreen)
			{
				sess = current_sess;
				/* GTK4: Can't get position, only size */
				prefs.hex_gui_win_width = gtk_widget_get_width (wid);
				prefs.hex_gui_win_height = gtk_widget_get_height (wid);
			}
		}
	}

	if (sess)
	{
		if (sess->type == SESS_DIALOG && prefs.hex_gui_win_save)
		{
			/* GTK4: Can't get position, only size */
			prefs.hex_gui_dialog_width = gtk_widget_get_width (wid);
			prefs.hex_gui_dialog_height = gtk_widget_get_height (wid);
		}
	}
}

/* move to a non-irc tab */

static void
mg_show_generic_tab (GtkWidget *box)
{
	int num;
	GtkWidget *f = NULL;

	if (current_sess && gtk_widget_has_focus (current_sess->gui->input_box))
		f = current_sess->gui->input_box;

	num = hc_page_container_get_page_num (mg_gui->note_book, box);
	hc_page_container_set_current_page (mg_gui->note_book, num);
	gtk_column_view_set_model (GTK_COLUMN_VIEW (mg_gui->user_tree), NULL);
	gtk_window_set_title (GTK_WINDOW (mg_gui->window),
								 g_object_get_data (G_OBJECT (box), "title"));

	if (f)
		gtk_widget_grab_focus (f);
}

/* a channel has been focused */

static void
mg_focus (session *sess)
{
	if (sess->gui->is_tab)
		current_tab = sess;
	current_sess = sess;

	/* dirty trick to avoid auto-selection */
	SPELL_ENTRY_SET_EDITABLE (sess->gui->input_box, FALSE);
	gtk_widget_grab_focus (sess->gui->input_box);
	SPELL_ENTRY_SET_EDITABLE (sess->gui->input_box, TRUE);

	sess->server->front_session = sess;

	if (sess->server->server_session != NULL)
	{
		if (sess->server->server_session->type != SESS_SERVER)
			sess->server->server_session = sess;
	} else
	{
		sess->server->server_session = sess;
	}

	/* when called via mg_changui_new, is_tab might be true, but
		sess->res->tab is still NULL. */
	if (sess->res->tab)
		fe_set_tab_color (sess, FE_COLOR_NONE);

	/* Auto-advance server read marker when tab gains focus */
	markread_send_for_session (sess);
}

static int
mg_progressbar_update (GtkWidget *bar)
{
	static int type = 0;
	static gdouble pos = 0;

	pos += 0.05;
	if (pos >= 0.99)
	{
		if (type == 0)
		{
			type = 1;
			/* GTK3: Use gtk_progress_bar_set_inverted instead of deprecated set_orientation */
			gtk_progress_bar_set_inverted ((GtkProgressBar *) bar, TRUE);
		} else
		{
			type = 0;
			gtk_progress_bar_set_inverted ((GtkProgressBar *) bar, FALSE);
		}
		pos = 0.05;
	}
	gtk_progress_bar_set_fraction ((GtkProgressBar *) bar, pos);
	return 1;
}

void
mg_progressbar_create (session_gui *gui)
{
	gui->bar = gtk_progress_bar_new ();
	gtk_box_append (GTK_BOX (gui->nick_box), gui->bar);
	gui->bartag = fe_timeout_add (50, mg_progressbar_update, gui->bar);
}

void
mg_progressbar_destroy (session_gui *gui)
{
	fe_timeout_remove (gui->bartag);
	hc_widget_destroy_impl (gui->bar);
	gui->bar = 0;
	gui->bartag = 0;
}

/* switching tabs away from this one, so remember some info about it! */

static void
mg_unpopulate (session *sess)
{
	restore_gui *res;
	session_gui *gui;
	int i;

	gui = sess->gui;
	res = sess->res;

	if (!gui || !res)
		return;

	res->input_text = g_strdup (SPELL_ENTRY_GET_TEXT (gui->input_box));
	res->topic_text = g_strdup (hc_entry_get_text (gui->topic_entry));
	res->limit_text = g_strdup (hc_entry_get_text (gui->limit_entry));
	res->key_text = g_strdup (hc_entry_get_text (gui->key_entry));
	if (gui->laginfo)
		res->lag_text = g_strdup (gtk_label_get_text (GTK_LABEL (gui->laginfo)));
	if (gui->throttleinfo)
		res->queue_text = g_strdup (gtk_label_get_text (GTK_LABEL (gui->throttleinfo)));

	for (i = 0; i < NUM_FLAG_WIDS - 1; i++)
		res->flag_wid_state[i] = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (gui->flag_wid[i]));

	res->old_ul_value = userlist_get_value (gui->user_tree);
	if (gui->lagometer)
		res->lag_value = gtk_progress_bar_get_fraction (
													GTK_PROGRESS_BAR (gui->lagometer));
	if (gui->throttlemeter)
		res->queue_value = gtk_progress_bar_get_fraction (
													GTK_PROGRESS_BAR (gui->throttlemeter));

	if (gui->bar)
	{
		res->c_graph = TRUE;	/* still have a graph, just not visible now */
		mg_progressbar_destroy (gui);
	}
}

static void
mg_restore_label (GtkWidget *label, char **text)
{
	if (!label)
		return;

	if (*text)
	{
		gtk_label_set_text (GTK_LABEL (label), *text);
		g_free (*text);
		*text = NULL;
	} else
	{
		gtk_label_set_text (GTK_LABEL (label), "");
	}
}

static void
mg_restore_entry (GtkWidget *entry, char **text)
{
	if (*text)
	{
		hc_entry_set_text (entry, *text);
		g_free (*text);
		*text = NULL;
	} else
	{
		hc_entry_set_text (entry, "");
	}
	if (HEX_IS_INPUT_EDIT (entry))
		hex_input_edit_set_position (HEX_INPUT_EDIT (entry), -1);
	else
		gtk_editable_set_position (GTK_EDITABLE (entry), -1);
}

static void
mg_restore_speller (GtkWidget *entry, char **text)
{
	if (*text)
	{
		SPELL_ENTRY_SET_TEXT (entry, *text);
		g_free (*text);
		*text = NULL;
	} else
	{
		SPELL_ENTRY_SET_TEXT (entry, "");
	}
	SPELL_ENTRY_SET_POS (entry, -1);
}

void
mg_set_topic_tip (session *sess)
{
	char *text;

	switch (sess->type)
	{
	case SESS_CHANNEL:
		if (sess->topic)
		{
			text = g_strdup_printf (_("Topic for %s is: %s"), sess->channel,
						 sess->topic);
			gtk_widget_set_tooltip_text (sess->gui->topic_entry, text);
			g_free (text);
		} else
			gtk_widget_set_tooltip_text (sess->gui->topic_entry, _("No topic is set"));
		break;
	default:
		if (hc_entry_get_text (sess->gui->topic_entry) &&
			 hc_entry_get_text (sess->gui->topic_entry)[0])
			gtk_widget_set_tooltip_text (sess->gui->topic_entry, (char *)hc_entry_get_text (sess->gui->topic_entry));
		else
			gtk_widget_set_tooltip_text (sess->gui->topic_entry, NULL);
	}
}

static void
mg_hide_empty_pane (GtkPaned *pane)
{
	GtkWidget *child1 = gtk_paned_get_start_child (pane);
	GtkWidget *child2 = gtk_paned_get_end_child (pane);
	gboolean child1_visible = (child1 != NULL && gtk_widget_get_visible (child1));
	gboolean child2_visible = (child2 != NULL && gtk_widget_get_visible (child2));

	if (!child1_visible && !child2_visible)
	{
		gtk_widget_set_visible (GTK_WIDGET (pane), FALSE);
		return;
	}

	gtk_widget_set_visible (GTK_WIDGET (pane), TRUE);
}

static void
mg_hide_empty_boxes (session_gui *gui)
{
	/* hide empty vpanes - so the handle is not shown */
	mg_hide_empty_pane ((GtkPaned*)gui->vpane_right);
	mg_hide_empty_pane ((GtkPaned*)gui->vpane_left);

	/* The wraps (GtkOverlay around the vpane) follow the vpane's
	 * visibility so the hpane doesn't reserve space for a hidden pane. */
	if (gui->vpane_left_wrap)
		gtk_widget_set_visible (gui->vpane_left_wrap,
			gtk_widget_get_visible (gui->vpane_left));
	if (gui->vpane_right_wrap)
		gtk_widget_set_visible (gui->vpane_right_wrap,
			gtk_widget_get_visible (gui->vpane_right));
}

/* Deferred right pane position restoration - called after window is mapped */
static gboolean
mg_restore_right_pane_position (gpointer user_data)
{
	session_gui *gui = (session_gui *)user_data;
	int pane_width, right_size;

	if (!gui || !gui->hpane_right)
		return G_SOURCE_REMOVE;

	pane_width = gtk_widget_get_width (gui->hpane_right);
	right_size = MAX (prefs.hex_gui_pane_right_size, prefs.hex_gui_pane_right_size_min);

	hc_debug_log ("mg_restore_right_pane_position: pane_width=%d right_size=%d",
	              pane_width, right_size);

	if (pane_width > 0 && right_size > 0)
	{
		gtk_paned_set_position (GTK_PANED (gui->hpane_right), pane_width - right_size);
	}
	else if (pane_width <= 0)
	{
		/* Window not yet fully laid out, try again */
		return G_SOURCE_CONTINUE;
	}

	return G_SOURCE_REMOVE;
}

/* Deferred vpane position restoration - called after window is mapped */
static gboolean
mg_restore_vpane_position (gpointer user_data)
{
	session_gui *gui = (session_gui *)user_data;
	int position = prefs.hex_gui_pane_divider_position;
	int tab_pos = prefs.hex_gui_tab_pos;
	int ulist_pos = prefs.hex_gui_ulist_pos;

	hc_debug_log ("mg_restore_vpane_position: position=%d tab_pos=%d ulist_pos=%d",
	              position, tab_pos, ulist_pos);

	if (!gui)
		return G_SOURCE_REMOVE;

	/* Restore vpane_left if both chanview and userlist are on the left */
	if ((tab_pos == POS_TOPLEFT || tab_pos == POS_BOTTOMLEFT) &&
	    (ulist_pos == POS_TOPLEFT || ulist_pos == POS_BOTTOMLEFT))
	{
		int height = gtk_widget_get_height (gui->vpane_left);
		hc_debug_log ("  vpane_left: height=%d, setting position=%d", height, position);
		if (height <= 0)
			return G_SOURCE_CONTINUE;
		if (position > 0)
			gtk_paned_set_position (GTK_PANED (gui->vpane_left), position);
	}

	/* Restore vpane_right if both chanview and userlist are on the right */
	if ((tab_pos == POS_TOPRIGHT || tab_pos == POS_BOTTOMRIGHT) &&
	    (ulist_pos == POS_TOPRIGHT || ulist_pos == POS_BOTTOMRIGHT))
	{
		int height = gtk_widget_get_height (gui->vpane_right);
		hc_debug_log ("  vpane_right: height=%d, setting position=%d", height, position);
		if (height <= 0)
			return G_SOURCE_CONTINUE;
		if (position > 0)
			gtk_paned_set_position (GTK_PANED (gui->vpane_right), position);
	}

	/* Mark vpane as restored so callbacks can now save position changes */
	gui->vpane_restored = 1;
	hc_debug_log ("  vpane_restored set to 1");

	return G_SOURCE_REMOVE;
}

static void mg_rightpane_cb (GtkPaned *pane, GParamSpec *param, session_gui *gui);

/* Idle callback: restore right pane position and unblock the notify::position
 * signal that was blocked in mg_userlist_showhide.  Runs after layout has
 * settled so pane_width reflects the actual (possibly resized) window. */
static gboolean
mg_showhide_restore_idle (gpointer user_data)
{
	session_gui *gui = (session_gui *)user_data;
	int pane_width, right_size;

	if (!gui || !gui->hpane_right)
		return G_SOURCE_REMOVE;

	pane_width = gtk_widget_get_width (gui->hpane_right);
	right_size = MAX (prefs.hex_gui_pane_right_size, prefs.hex_gui_pane_right_size_min);

	if (pane_width <= 0)
		return G_SOURCE_CONTINUE;  /* window not yet laid out, retry */

	/* Block signal around set_position so the restore itself doesn't
	 * get picked up as a user-initiated pane drag. */
	if (!gui->ul_hidden && pane_width > 0 && right_size > 0)
		gtk_paned_set_position (GTK_PANED (gui->hpane_right), pane_width - right_size);

	g_signal_handlers_unblock_by_func (gui->hpane_right, mg_rightpane_cb, gui);
	return G_SOURCE_REMOVE;
}

static void
mg_userlist_showhide (session *sess, int show)
{
	session_gui *gui = sess->gui;

	/* Block notify::position during show/hide to prevent spurious saves.
	 * GTK4 internally adjusts the pane position when children are shown/hidden
	 * and when the window resizes to meet minimum size — those intermediate
	 * positions must not overwrite the user's preferred right_size.
	 * The signal stays blocked until the idle callback restores the position. */
	g_signal_handlers_block_by_func (gui->hpane_right, mg_rightpane_cb, gui);

	if (show)
	{
		gtk_widget_set_visible (gui->user_box, TRUE);
		gui->ul_hidden = 0;
	}
	else
	{
		gui->ul_hidden = 1;
		gtk_widget_set_visible (gui->user_box, FALSE);
	}

	mg_hide_empty_boxes (gui);
	mg_update_window_minimum (gui);

	/* Defer position restore + signal unblock to idle so layout settles first.
	 * For hide, the idle still runs to unblock the signal (position is moot). */
	g_idle_add (mg_showhide_restore_idle, gui);
}

static gboolean
mg_is_userlist_and_tree_combined (void)
{
	if (prefs.hex_gui_tab_pos == POS_TOPLEFT && prefs.hex_gui_ulist_pos == POS_BOTTOMLEFT)
		return TRUE;
	if (prefs.hex_gui_tab_pos == POS_BOTTOMLEFT && prefs.hex_gui_ulist_pos == POS_TOPLEFT)
		return TRUE;

	if (prefs.hex_gui_tab_pos == POS_TOPRIGHT && prefs.hex_gui_ulist_pos == POS_BOTTOMRIGHT)
		return TRUE;
	if (prefs.hex_gui_tab_pos == POS_BOTTOMRIGHT && prefs.hex_gui_ulist_pos == POS_TOPRIGHT)
		return TRUE;

	return FALSE;
}

/* decide if the userlist should be shown or hidden for this tab */

void
mg_decide_userlist (session *sess, gboolean switch_to_current)
{
	/* when called from menu.c we need this */
	if (sess->gui == mg_gui && switch_to_current)
		sess = current_tab;

	if (prefs.hex_gui_ulist_hide)
	{
		mg_userlist_showhide (sess, FALSE);
		return;
	}

	switch (sess->type)
	{
	case SESS_SERVER:
	case SESS_DIALOG:
	case SESS_NOTICES:
	case SESS_SNOTICES:
		if (mg_is_userlist_and_tree_combined ())
			mg_userlist_showhide (sess, TRUE);	/* show */
		else
			mg_userlist_showhide (sess, FALSE);	/* hide */
		break;
	default:		
		mg_userlist_showhide (sess, TRUE);	/* show */
	}
}

static int ul_tag = 0;

static int mg_userlist_pane_size (session_gui *gui);
static void mg_update_userlist_columns (session *sess, int pane_size);
static gboolean mg_userlist_update_columns_idle (gpointer user_data);
static void mg_pane_apply_detent (GtkPaned *pane, GtkWidget *shrinking_child,
                                  gboolean shrinking_side_is_end,
                                  GCallback handler, gpointer data);

static gboolean
mg_populate_userlist (session *sess)
{
	if (!sess)
		sess = current_tab;

	if (is_session (sess) && sess->gui && sess->res)
	{
		int pane_size;

		if (sess->type == SESS_DIALOG)
			mg_set_access_icon (sess->gui, NULL, sess->server->is_away);
		else
			mg_set_access_icon (sess->gui, get_user_icon (sess->server, sess->me), sess->server->is_away);

		userlist_show (sess);
		userlist_set_value (sess->gui->user_tree, sess->res->old_ul_value);

		/* Apply the new channel's column collapse synchronously — both
		 * set-model and set-visible queue a single coalesced layout, so
		 * the ColumnView paints once with the correct visibility instead
		 * of painting wide first and reshuffling (which flickers the
		 * whole pane). max_nick is measured from sess->res->user_model
		 * via Pango so it doesn't require the ListView to have bound
		 * row labels yet. */
		pane_size = mg_userlist_pane_size (sess->gui);
		if (pane_size > 0)
			mg_update_userlist_columns (sess, pane_size);

		/* Safety net: re-run after layout completes, in case pane_size
		 * was 0 (window not yet allocated). Normally a no-op. */
		g_idle_add (mg_userlist_update_columns_idle, sess);

		/* Re-apply right pane position after model connect triggers layout reflow */
		if (sess->gui->hpane_right)
		{
			int pane_width = gtk_widget_get_width (sess->gui->hpane_right);
			int right_size = MAX (prefs.hex_gui_pane_right_size,
			                      prefs.hex_gui_pane_right_size_min);
			if (pane_width > 0 && right_size > 0)
				gtk_paned_set_position (GTK_PANED (sess->gui->hpane_right),
				                        pane_width - right_size);
		}
	}

	ul_tag = 0;
	return 0;
}

/* fill the irc tab with a new channel */

static void
mg_populate (session *sess)
{
	session_gui *gui = sess->gui;
	restore_gui *res = sess->res;
	int i, render = TRUE;
	guint16 vis;

	if (!gui || !res)
		return;

	vis = gui->ul_hidden;

	switch (sess->type)
	{
	case SESS_DIALOG:
		/* show the dialog buttons */
		gtk_widget_set_visible (gui->dialogbutton_box, TRUE);
		/* hide the chan-mode buttons */
		gtk_widget_set_visible (gui->topicbutton_box, FALSE);
		/* hide the userlist */
		mg_decide_userlist (sess, FALSE);
		/* shouldn't edit the topic */
		hex_input_edit_set_editable (HEX_INPUT_EDIT (gui->topic_entry), FALSE);
		/* might be hidden from server tab */
		if (prefs.hex_gui_topicbar)
			gtk_widget_set_visible (gui->topic_bar, TRUE);
		break;
	case SESS_SERVER:
		if (prefs.hex_gui_mode_buttons)
			gtk_widget_set_visible (gui->topicbutton_box, TRUE);
		/* hide the dialog buttons */
		gtk_widget_set_visible (gui->dialogbutton_box, FALSE);
		/* hide the userlist */
		mg_decide_userlist (sess, FALSE);
		/* servers don't have topics */
		gtk_widget_set_visible (gui->topic_bar, FALSE);
		break;
	default:
		/* hide the dialog buttons */
		gtk_widget_set_visible (gui->dialogbutton_box, FALSE);
		/* show the userlist */
		mg_decide_userlist (sess, FALSE);
		/* let the topic be editted */
		hex_input_edit_set_editable (HEX_INPUT_EDIT (gui->topic_entry), TRUE);
		if (prefs.hex_gui_topicbar)
			gtk_widget_set_visible (gui->topic_bar, TRUE);
		/* Show mode buttons after topic_bar is visible */
		if (prefs.hex_gui_mode_buttons)
		{
			/* GTK4: Force visibility state change by hiding then showing */
			gtk_widget_set_visible (gui->topicbutton_box, FALSE);
			gtk_widget_set_visible (gui->topicbutton_box, TRUE);
			/* Also force visibility of all children */
			{
				GtkWidget *child;
				for (child = gtk_widget_get_first_child (gui->topicbutton_box);
				     child != NULL;
				     child = gtk_widget_get_next_sibling (child))
				{
					gtk_widget_set_visible (child, TRUE);
				}
			}
		}
	}

	/* move to THE irc tab */
	if (gui->is_tab)
		hc_page_container_set_current_page (gui->note_book, 0);

	/* xtext size change? Then don't render, wait for the expose caused
      by showing/hidding the userlist */
	if (vis != gui->ul_hidden && gtk_widget_get_width (gui->user_box) > 1)
		render = FALSE;

	gtk_xtext_buffer_show (GTK_XTEXT (gui->xtext), res->buffer, render);

	/* Update typing indicator strip and reply state for this tab */
	fe_typing_update (sess);
	fe_reply_state_changed (sess);

	/* restore all the GtkEntry's */
	mg_restore_entry (gui->topic_entry, &res->topic_text);
	mg_restore_speller (gui->input_box, &res->input_text);
	mg_restore_entry (gui->key_entry, &res->key_text);
	mg_restore_entry (gui->limit_entry, &res->limit_text);
	mg_restore_label (gui->laginfo, &res->lag_text);
	mg_restore_label (gui->throttleinfo, &res->queue_text);

	mg_focus (sess);
	fe_set_title (sess);

	if (gui->xtext)
	{
		GtkXText *xt = GTK_XTEXT (gui->xtext);
		xt->redact_button_visible = (sess->server && sess->server->have_redact);
	}

	/* this one flickers, so only change if necessary */
	{
		GtkWidget *lbl = g_object_get_data (G_OBJECT (gui->nick_label), "nick-label");
		if (lbl && strcmp (sess->server->nick, gtk_label_get_text (GTK_LABEL (lbl))) != 0)
			gtk_label_set_text (GTK_LABEL (lbl), sess->server->nick);
	}

	if (ul_tag != 0)
	{
		g_source_remove (ul_tag);
		ul_tag = 0;
	}
	if (!gui->is_tab)
	{
		mg_populate_userlist (sess);
	}
	else
	{
		/* Short timeout so the pane position restore idle settles before
		 * the model connects and triggers a layout reflow. */
		ul_tag = g_timeout_add (50, (GSourceFunc)mg_populate_userlist, NULL);
	}

	fe_userlist_numbers (sess);

	/* restore all the channel mode buttons */
	ignore_chanmode = TRUE;
	for (i = 0; i < NUM_FLAG_WIDS - 1; i++)
	{
		/* Hide if mode not supported */
		if (sess->server && strchr (sess->server->chanmodes, chan_flags[i]) == NULL)
			gtk_widget_set_visible (sess->gui->flag_wid[i], FALSE);
		else
			gtk_widget_set_visible (sess->gui->flag_wid[i], TRUE);

		/* Update state */
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (gui->flag_wid[i]),
									res->flag_wid_state[i]);
	}
	ignore_chanmode = FALSE;

	if (gui->lagometer)
	{
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (gui->lagometer),
												 res->lag_value);
		if (res->lag_tip)
			gtk_widget_set_tooltip_text (gtk_widget_get_parent (sess->gui->lagometer), res->lag_tip);
	}
	if (gui->throttlemeter)
	{
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (gui->throttlemeter),
												 res->queue_value);
		if (res->queue_tip)
			gtk_widget_set_tooltip_text (gtk_widget_get_parent (sess->gui->throttlemeter), res->queue_tip);
	}

	/* did this tab have a connecting graph? restore it.. */
	if (res->c_graph)
	{
		res->c_graph = FALSE;
		mg_progressbar_create (gui);
	}

	/* menu items */
	menu_set_away (gui, sess->server->is_away);
	menu_set_action_sensitive (gui, MENU_ID_AWAY, sess->server->connected);
	menu_set_action_sensitive (gui, MENU_ID_JOIN, sess->server->end_of_motd);
	menu_set_action_sensitive (gui, MENU_ID_DISCONNECT,
									  sess->server->connected || sess->server->recondelay_tag);

	mg_set_topic_tip (sess);

	plugin_emit_dummy_print (sess, "Focus Tab");
}

void
mg_bring_tofront_sess (session *sess)	/* IRC tab or window */
{
	if (sess->gui->is_tab)
		chan_focus (sess->res->tab);
	else
		gtk_window_present (GTK_WINDOW (sess->gui->window));
}

void
mg_bring_tofront (GtkWidget *vbox)	/* non-IRC tab or window */
{
	chan *ch;

	ch = g_object_get_data (G_OBJECT (vbox), "ch");
	if (ch)
		chan_focus (ch);
	else
		gtk_window_present (GTK_WINDOW (gtk_widget_get_root (vbox)));
}

void
mg_switch_page (int relative, int num)
{
	if (mg_gui)
		chanview_move_focus (mg_gui->chanview, relative, num);
}

/* a toplevel IRC window was destroyed */

static void
mg_topdestroy_cb (GtkWidget *win, session *sess)
{
	session_free (sess);	/* tell poxchat.c about it */
}

/* cleanup an IRC tab */

static void
mg_ircdestroy (session *sess)
{
	GSList *list;

	session_free (sess);	/* tell poxchat.c about it */

	if (mg_gui == NULL)
	{
/*		puts("-> mg_gui is already NULL");*/
		return;
	}

	list = sess_list;
	while (list)
	{
		sess = list->data;
		if (sess->gui->is_tab)
		{
/*			puts("-> some tabs still remain");*/
			return;
		}
		list = list->next;
	}

/*	puts("-> no tabs left, killing main tabwindow");*/
	hc_window_destroy_fn (GTK_WINDOW (mg_gui->window));
	active_tab = NULL;
	mg_gui = NULL;
	parent_window = NULL;
}

static void
mg_tab_close_cb (GObject *source, GAsyncResult *result, gpointer data)
{
	session *sess = data;
	GSList *list, *next;
	int button = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), result, NULL);

	if (button == 1 && is_session (sess)) /* OK */
	{
		/* force it NOT to send individual PARTs */
		sess->server->sent_quit = TRUE;

		for (list = sess_list; list;)
		{
			next = list->next;
			if (((session *)list->data)->server == sess->server &&
				 ((session *)list->data) != sess)
				fe_close_window ((session *)list->data);
			list = next;
		}

		/* just send one QUIT - better for BNCs */
		sess->server->sent_quit = FALSE;
		fe_close_window (sess);
	}
}

void
mg_tab_close (session *sess)
{
	GSList *list;
	int i;

	if (chan_remove (sess->res->tab, FALSE))
	{
		sess->res->tab = NULL;
		mg_ircdestroy (sess);
	}
	else
	{
		for (i = 0, list = sess_list; list; list = list->next)
		{
			session *s = (session*)list->data;
			if (s->server == sess->server && (s->type == SESS_CHANNEL || s->type == SESS_DIALOG))
				i++;
		}
		{
			GtkAlertDialog *alert;
			const char *buttons[] = { _("_Cancel"), _("_OK"), NULL };

			alert = gtk_alert_dialog_new (_("This server still has %d channels or dialogs associated with it. "
				"Close them all?"), i);
			gtk_alert_dialog_set_buttons (alert, buttons);
			gtk_alert_dialog_set_cancel_button (alert, 0);
			gtk_alert_dialog_set_default_button (alert, 1);

			gtk_alert_dialog_choose (alert, GTK_WINDOW (parent_window),
				NULL, mg_tab_close_cb, sess);
			g_object_unref (alert);
		}
	}
}

static int
mg_count_networks (void)
{
	int cons = 0;
	GSList *list;

	for (list = serv_list; list; list = list->next)
	{
		if (((server *)list->data)->connected)
			cons++;
	}
	return cons;
}

static int
mg_count_dccs (void)
{
	GSList *list;
	struct DCC *dcc;
	int dccs = 0;

	list = dcc_list;
	while (list)
	{
		dcc = list->data;
		if ((dcc->type == TYPE_SEND || dcc->type == TYPE_RECV) &&
			 dcc->dccstat == STAT_ACTIVE)
			dccs++;
		list = list->next;
	}

	return dccs;
}

/*
 * GTK4 version of quit dialog - uses async response handling.
 * gtk_dialog_run(), gtk_dialog_get_action_area(), gtk_button_box_set_layout(),
 * gtk_container_set_border_width(), GTK_ICON_SIZE_DIALOG all removed in GTK4.
 */
static GtkWidget *quit_dialog = NULL;
static GtkWidget *quit_dialog_checkbox = NULL;

static void
mg_quit_dialog_quit_cb (GtkWidget *button, gpointer user_data)
{
	if (gtk_check_button_get_active (GTK_CHECK_BUTTON (quit_dialog_checkbox)))
		prefs.hex_gui_quit_dialog = 0;
	hc_window_destroy_fn (GTK_WINDOW (quit_dialog));
	quit_dialog = NULL;
	quit_dialog_checkbox = NULL;
	poxchat_exit ();
}

static void
mg_quit_dialog_tray_cb (GtkWidget *button, gpointer user_data)
{
	if (gtk_check_button_get_active (GTK_CHECK_BUTTON (quit_dialog_checkbox)))
	{
		prefs.hex_gui_tray_close = 1;
	}
	/* force tray icon ON, if not already */
	if (!prefs.hex_gui_tray)
	{
		prefs.hex_gui_tray = 1;
		tray_apply_setup ();
	}
	tray_toggle_visibility (TRUE);
	hc_window_destroy_fn (GTK_WINDOW (quit_dialog));
	quit_dialog = NULL;
	quit_dialog_checkbox = NULL;
}

static void
mg_quit_dialog_cancel_cb (GtkWidget *button, gpointer user_data)
{
	hc_window_destroy_fn (GTK_WINDOW (quit_dialog));
	quit_dialog = NULL;
	quit_dialog_checkbox = NULL;
}

void
mg_open_quit_dialog (gboolean minimize_button)
{
	GtkWidget *dialog_vbox1;
	GtkWidget *table1;
	GtkWidget *image;
	GtkWidget *label;
	GtkWidget *button;
	char *text, *connecttext;
	int cons;
	int dccs;

	if (quit_dialog)
	{
		gtk_window_present (GTK_WINDOW (quit_dialog));
		return;
	}

	dccs = mg_count_dccs ();
	cons = mg_count_networks ();
	if (dccs + cons == 0 || !prefs.hex_gui_quit_dialog)
	{
		poxchat_exit ();
		return;
	}

	quit_dialog = gtk_window_new ();
	gtk_window_set_title (GTK_WINDOW (quit_dialog), _("Quit PoxChat?"));
	gtk_window_set_transient_for (GTK_WINDOW (quit_dialog), GTK_WINDOW (parent_window));
	gtk_window_set_resizable (GTK_WINDOW (quit_dialog), FALSE);
	gtk_window_set_destroy_with_parent (GTK_WINDOW (quit_dialog), TRUE);

	dialog_vbox1 = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	hc_widget_set_margin_all (dialog_vbox1, 12);
	gtk_window_set_child (GTK_WINDOW (quit_dialog), dialog_vbox1);

	table1 = gtk_grid_new ();
	gtk_box_append (GTK_BOX (dialog_vbox1), table1);
	gtk_grid_set_row_spacing (GTK_GRID (table1), 12);
	gtk_grid_set_column_spacing (GTK_GRID (table1), 12);

	/* GTK4: gtk_image_new_from_icon_name no longer takes icon size */
	image = gtk_image_new_from_icon_name ("dialog-warning");
	gtk_image_set_icon_size (GTK_IMAGE (image), GTK_ICON_SIZE_LARGE);
	gtk_grid_attach (GTK_GRID (table1), image, 0, 0, 1, 1);

	quit_dialog_checkbox = gtk_check_button_new_with_mnemonic (_("Don't ask next time."));
	gtk_widget_set_hexpand (quit_dialog_checkbox, TRUE);
	gtk_widget_set_margin_top (quit_dialog_checkbox, 4);
	gtk_grid_attach (GTK_GRID (table1), quit_dialog_checkbox, 0, 1, 2, 1);

	connecttext = g_strdup_printf (_("You are connected to %i IRC networks."), cons);
	text = g_strdup_printf ("<span weight=\"bold\" size=\"larger\">%s</span>\n\n%s\n%s",
								_("Are you sure you want to quit?"),
								cons ? connecttext : "",
								dccs ? _("Some file transfers are still active.") : "");
	g_free (connecttext);
	label = gtk_label_new (text);
	g_free (text);
	gtk_widget_set_hexpand (label, TRUE);
	gtk_widget_set_vexpand (label, TRUE);
	gtk_grid_attach (GTK_GRID (table1), label, 1, 0, 1, 1);
	gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
	gtk_widget_set_halign (label, GTK_ALIGN_START);
	gtk_widget_set_valign (label, GTK_ALIGN_CENTER);

	/* Button row */
	{
		GtkWidget *button_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
		gtk_widget_set_halign (button_box, GTK_ALIGN_END);
		gtk_widget_set_margin_top (button_box, 12);

		if (minimize_button && gtkutil_tray_icon_supported (GTK_WINDOW(quit_dialog)))
		{
			button = gtk_button_new_with_mnemonic (_("_Minimize to Tray"));
			g_signal_connect (button, "clicked", G_CALLBACK (mg_quit_dialog_tray_cb), NULL);
			gtk_box_append (GTK_BOX (button_box), button);
		}

		button = gtk_button_new_with_mnemonic (_("_Cancel"));
		g_signal_connect (button, "clicked", G_CALLBACK (mg_quit_dialog_cancel_cb), NULL);
		gtk_box_append (GTK_BOX (button_box), button);
		gtk_widget_grab_focus (button);

		button = gtk_button_new_with_mnemonic (_("_Quit"));
		g_signal_connect (button, "clicked", G_CALLBACK (mg_quit_dialog_quit_cb), NULL);
		gtk_box_append (GTK_BOX (button_box), button);

		gtk_box_append (GTK_BOX (dialog_vbox1), button_box);
	}

	gtk_window_present (GTK_WINDOW (quit_dialog));
}

void
mg_close_sess (session *sess)
{
	if (sess_list->next == NULL)
	{
		mg_open_quit_dialog (FALSE);
		return;
	}

	fe_close_window (sess);
}

static int
mg_chan_remove (chan *ch)
{
	/* remove the tab from chanview */
	chan_remove (ch, TRUE);
	/* any tabs left? */
	if (chanview_get_size (mg_gui->chanview) < 1)
	{
		/* if not, destroy the main tab window */
		hc_window_destroy_fn (GTK_WINDOW (mg_gui->window));
		current_tab = NULL;
		active_tab = NULL;
		mg_gui = NULL;
		parent_window = NULL;
		return TRUE;
	}
	return FALSE;
}

/* destroy non-irc tab/window */

static void
mg_close_gen (chan *ch, GtkWidget *box)
{
	if (!ch)
		ch = g_object_get_data (G_OBJECT (box), "ch");
	if (ch)
	{
		/* remove from notebook */
		hc_widget_destroy_impl (box);
		/* remove the tab from chanview */
		mg_chan_remove (ch);
	} else
	{
		hc_window_destroy_fn (GTK_WINDOW (gtk_widget_get_root (box)));
	}
}

/* the "X" close button has been pressed (tab-view) */

static void
mg_xbutton_cb (chanview *cv, chan *ch, int tag, gpointer userdata)
{
	if (tag == TAG_IRC)	/* irc tab */
		mg_close_sess (userdata);
	else						/* non-irc utility tab */
		mg_close_gen (ch, userdata);
}

static void
mg_link_gentab (chan *ch, GtkWidget *box)
{
	int num;
	GtkWidget *win;

	g_object_ref (box);

	num = hc_page_container_get_page_num (mg_gui->note_book, box);
	hc_page_container_remove_page (mg_gui->note_book, num);
	mg_chan_remove (ch);

	win = gtkutil_window_new (g_object_get_data (G_OBJECT (box), "title"), "",
									  GPOINTER_TO_INT (g_object_get_data (G_OBJECT (box), "w")),
									  GPOINTER_TO_INT (g_object_get_data (G_OBJECT (box), "h")),
									  2);
	/* so it doesn't try to chan_remove (there's no tab anymore) */
	g_object_steal_data (G_OBJECT (box), "ch");
	hc_widget_set_margin_all (box, 0);
	gtk_window_set_child (GTK_WINDOW (win), box);
	gtk_window_present (GTK_WINDOW (win));

	g_object_unref (box);
}

/* GTK4: Tab menu action callbacks and context state */
static session *tab_menu_sess = NULL;
static chan *tab_menu_ch = NULL;

static void
tab_action_detach (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	(void)action; (void)parameter; (void)user_data;
	if (tab_menu_ch)
	{
		if (chan_get_tag (tab_menu_ch) == TAG_IRC)
			mg_link_irctab (chan_get_userdata (tab_menu_ch), 1);
		else
			mg_link_gentab (tab_menu_ch, chan_get_userdata (tab_menu_ch));
	}
}

static void
tab_action_close (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	(void)action; (void)parameter; (void)user_data;
	if (tab_menu_ch)
		mg_xbutton_cb (mg_gui->chanview, tab_menu_ch, chan_get_tag (tab_menu_ch),
						   chan_get_userdata (tab_menu_ch));
}

/* Alert toggle actions - use stateful actions with boolean state */
static void
tab_action_alert_balloon (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	(void)parameter; (void)user_data;
	if (tab_menu_sess)
	{
		GVariant *state = g_action_get_state (G_ACTION (action));
		gboolean new_state = !g_variant_get_boolean (state);
		g_simple_action_set_state (action, g_variant_new_boolean (new_state));
		tab_menu_sess->alert_balloon = new_state ? SET_ON : SET_OFF;
		chanopt_save (tab_menu_sess);
		chanopt_save_all (FALSE);
		g_variant_unref (state);
	}
}

static void
tab_action_alert_beep (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	(void)parameter; (void)user_data;
	if (tab_menu_sess)
	{
		GVariant *state = g_action_get_state (G_ACTION (action));
		gboolean new_state = !g_variant_get_boolean (state);
		g_simple_action_set_state (action, g_variant_new_boolean (new_state));
		tab_menu_sess->alert_beep = new_state ? SET_ON : SET_OFF;
		chanopt_save (tab_menu_sess);
		chanopt_save_all (FALSE);
		g_variant_unref (state);
	}
}

static void
tab_action_alert_tray (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	(void)parameter; (void)user_data;
	if (tab_menu_sess)
	{
		GVariant *state = g_action_get_state (G_ACTION (action));
		gboolean new_state = !g_variant_get_boolean (state);
		g_simple_action_set_state (action, g_variant_new_boolean (new_state));
		tab_menu_sess->alert_tray = new_state ? SET_ON : SET_OFF;
		chanopt_save (tab_menu_sess);
		chanopt_save_all (FALSE);
		g_variant_unref (state);
	}
}

static void
tab_action_alert_taskbar (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	(void)parameter; (void)user_data;
	if (tab_menu_sess)
	{
		GVariant *state = g_action_get_state (G_ACTION (action));
		gboolean new_state = !g_variant_get_boolean (state);
		g_simple_action_set_state (action, g_variant_new_boolean (new_state));
		tab_menu_sess->alert_taskbar = new_state ? SET_ON : SET_OFF;
		chanopt_save (tab_menu_sess);
		chanopt_save_all (FALSE);
		g_variant_unref (state);
	}
}

/* Settings toggle actions */
static void
tab_action_logging (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	(void)parameter; (void)user_data;
	if (tab_menu_sess)
	{
		GVariant *state = g_action_get_state (G_ACTION (action));
		gboolean new_state = !g_variant_get_boolean (state);
		guint8 old_logging = tab_menu_sess->text_logging;
		g_simple_action_set_state (action, g_variant_new_boolean (new_state));
		tab_menu_sess->text_logging = new_state ? SET_ON : SET_OFF;
		if (old_logging != tab_menu_sess->text_logging)
			log_open_or_close (tab_menu_sess);
		chanopt_save (tab_menu_sess);
		chanopt_save_all (FALSE);
		g_variant_unref (state);
	}
}

static void
tab_action_scrollback (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	(void)parameter; (void)user_data;
	if (tab_menu_sess)
	{
		GVariant *state = g_action_get_state (G_ACTION (action));
		gboolean new_state = !g_variant_get_boolean (state);
		g_simple_action_set_state (action, g_variant_new_boolean (new_state));
		tab_menu_sess->text_scrollback = new_state ? SET_ON : SET_OFF;
		chanopt_save (tab_menu_sess);
		chanopt_save_all (FALSE);
		g_variant_unref (state);
	}
}

static void
tab_action_strip_colors (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	(void)parameter; (void)user_data;
	if (tab_menu_sess)
	{
		GVariant *state = g_action_get_state (G_ACTION (action));
		gboolean new_state = !g_variant_get_boolean (state);
		g_simple_action_set_state (action, g_variant_new_boolean (new_state));
		tab_menu_sess->text_strip = new_state ? SET_ON : SET_OFF;
		chanopt_save (tab_menu_sess);
		chanopt_save_all (FALSE);
		g_variant_unref (state);
	}
}

static void
tab_action_hide_joinpart (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	(void)parameter; (void)user_data;
	if (tab_menu_sess)
	{
		GVariant *state = g_action_get_state (G_ACTION (action));
		gboolean new_state = !g_variant_get_boolean (state);
		g_simple_action_set_state (action, g_variant_new_boolean (new_state));
		tab_menu_sess->text_hidejoinpart = new_state ? SET_ON : SET_OFF;
		chanopt_save (tab_menu_sess);
		chanopt_save_all (FALSE);
		g_variant_unref (state);
	}
}

/* Autojoin toggle action */
static void
tab_action_autojoin (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	(void)parameter; (void)user_data;
	if (tab_menu_sess && tab_menu_sess->server && tab_menu_sess->server->network)
	{
		GVariant *state = g_action_get_state (G_ACTION (action));
		gboolean new_state = !g_variant_get_boolean (state);
		g_simple_action_set_state (action, g_variant_new_boolean (new_state));
		servlist_autojoinedit (tab_menu_sess->server->network, tab_menu_sess->channel, new_state);
		g_variant_unref (state);
	}
}

/* Auto-connect toggle action (for server tabs) */
static void
tab_action_autoconnect (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	(void)parameter; (void)user_data;
	if (tab_menu_sess && tab_menu_sess->server && tab_menu_sess->server->network)
	{
		GVariant *state = g_action_get_state (G_ACTION (action));
		gboolean new_state = !g_variant_get_boolean (state);
		g_simple_action_set_state (action, g_variant_new_boolean (new_state));
		if (new_state)
			((ircnet*)tab_menu_sess->server->network)->flags |= FLAG_AUTO_CONNECT;
		else
			((ircnet*)tab_menu_sess->server->network)->flags &= ~FLAG_AUTO_CONNECT;
		servlist_save ();
		g_variant_unref (state);
	}
}

typedef struct {
	GtkWidget *popover;
	GSimpleActionGroup *action_group;
} TabMenuCleanupData;

static void
tab_menu_popover_weak_notify (gpointer data, GObject *where_the_object_was)
{
	TabMenuCleanupData *cleanup = data;
	(void)where_the_object_was;
	cleanup->popover = NULL;
}

static gboolean
tab_menu_popover_cleanup_idle (gpointer user_data)
{
	TabMenuCleanupData *cleanup = user_data;

	if (cleanup->action_group)
		g_object_unref (cleanup->action_group);

	/* Unparent the popover if still alive with a parent — without this,
	 * stale popovers accumulate on the parent widget and eventually prevent
	 * new ones from opening. The parent check guards against window-teardown
	 * races where the hierarchy is already being dismantled. */
	if (cleanup->popover != NULL)
	{
		g_object_weak_unref (G_OBJECT (cleanup->popover), tab_menu_popover_weak_notify, cleanup);
		if (gtk_widget_get_parent (cleanup->popover))
			gtk_widget_unparent (cleanup->popover);
	}

	g_free (cleanup);
	return G_SOURCE_REMOVE;
}

static void
tab_menu_popover_closed_cb (GtkPopover *popover, gpointer user_data)
{
	GSimpleActionGroup *action_group = G_SIMPLE_ACTION_GROUP (user_data);
	TabMenuCleanupData *cleanup;
	chanview *cv;

	cv = g_object_get_data (G_OBJECT (popover), "chanview");
	if (cv)
		chanview_restore_focus_selection (cv);

	/* Defer action-group unref and popover unparent to an idle callback so
	 * that action activations triggered by the menu item click have time to
	 * run before the muxer/action group goes away. Synchronous cleanup here
	 * kills action resolution mid-flight, making every menu item a no-op. */
	cleanup = g_new0 (TabMenuCleanupData, 1);
	cleanup->popover = GTK_WIDGET (popover);
	cleanup->action_group = action_group;

	g_object_weak_ref (G_OBJECT (popover), tab_menu_popover_weak_notify, cleanup);
	g_idle_add (tab_menu_popover_cleanup_idle, cleanup);
}

/* Helper to get the effective boolean state for per-channel settings */
static gboolean
tab_get_setting_state (guint8 setting, guint global_default)
{
	if (setting == SET_DEFAULT)
		return global_default ? TRUE : FALSE;
	return (setting == SET_ON);
}

static void
mg_create_tabmenu (chanview *cv, session *sess, chan *ch, GtkWidget *parent, double x, double y)
{
	GMenu *gmenu;
	GMenu *alerts_submenu;
	GMenu *settings_submenu;
	GtkWidget *popover;
	GtkWidget *parent_widget;
	GSimpleActionGroup *action_group;
	GSimpleAction *action;
	char buf[256];

	tab_menu_sess = sess;
	tab_menu_ch = ch;

	/* Use provided parent widget, or find one */
	parent_widget = parent;
	if (!parent_widget)
		parent_widget = chan_get_impl_widget (ch);
	if (!parent_widget)
	{
		/* Fallback to chanview box if no tab widget */
		if (mg_gui && mg_gui->chanview)
			parent_widget = chanview_get_box (mg_gui->chanview);
	}
	if (!parent_widget)
		return;

	/* For toggle buttons (tabs), parent the popover to the container instead
	 * of the button itself — otherwise popover events propagate to the toggle
	 * button and corrupt its :active CSS state */
	if (GTK_IS_TOGGLE_BUTTON (parent_widget))
	{
		GtkWidget *container = gtk_widget_get_parent (parent_widget);
		if (container)
		{
			double cx, cy;
			gtk_widget_translate_coordinates (parent_widget, container, x, y, &cx, &cy);
			parent_widget = container;
			x = cx;
			y = cy;
		}
	}

	/* Create action group */
	action_group = g_simple_action_group_new ();

	/* Basic actions */
	action = g_simple_action_new ("detach", NULL);
	g_signal_connect (action, "activate", G_CALLBACK (tab_action_detach), NULL);
	g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
	g_object_unref (action);

	action = g_simple_action_new ("close", NULL);
	g_signal_connect (action, "activate", G_CALLBACK (tab_action_close), NULL);
	g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
	g_object_unref (action);

	gmenu = g_menu_new ();

	/* Channel name header (in its own section for visual separator) */
	if (sess)
	{
		GMenu *header_section = g_menu_new ();
		GMenu *body_section = g_menu_new ();
		char *name = g_markup_escape_text (sess->channel[0] ? sess->channel : _("<none>"), -1);
		g_snprintf (buf, sizeof (buf), "%s", name);
		g_free (name);
		g_menu_append (header_section, buf, NULL);
		g_menu_append_section (gmenu, NULL, G_MENU_MODEL (header_section));
		g_object_unref (header_section);

		/* Get default values based on session type */
		int hex_balloon, hex_beep, hex_tray, hex_flash;
		switch (sess->type) {
			case SESS_DIALOG:
				hex_balloon = prefs.hex_input_balloon_priv;
				hex_beep = prefs.hex_input_beep_priv;
				hex_tray = prefs.hex_input_tray_priv;
				hex_flash = prefs.hex_input_flash_priv;
				break;
			default:
				hex_balloon = prefs.hex_input_balloon_chans;
				hex_beep = prefs.hex_input_beep_chans;
				hex_tray = prefs.hex_input_tray_chans;
				hex_flash = prefs.hex_input_flash_chans;
		}

		/* Extra Alerts submenu */
		alerts_submenu = g_menu_new ();

		action = g_simple_action_new_stateful ("alert_balloon", NULL,
			g_variant_new_boolean (tab_get_setting_state (sess->alert_balloon, hex_balloon)));
		g_signal_connect (action, "activate", G_CALLBACK (tab_action_alert_balloon), NULL);
		g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
		g_object_unref (action);
		g_menu_append (alerts_submenu, _("Show Notifications"), "tab.alert_balloon");

		action = g_simple_action_new_stateful ("alert_beep", NULL,
			g_variant_new_boolean (tab_get_setting_state (sess->alert_beep, hex_beep)));
		g_signal_connect (action, "activate", G_CALLBACK (tab_action_alert_beep), NULL);
		g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
		g_object_unref (action);
		g_menu_append (alerts_submenu, _("Beep on _Message"), "tab.alert_beep");

		action = g_simple_action_new_stateful ("alert_tray", NULL,
			g_variant_new_boolean (tab_get_setting_state (sess->alert_tray, hex_tray)));
		g_signal_connect (action, "activate", G_CALLBACK (tab_action_alert_tray), NULL);
		g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
		g_object_unref (action);
		g_menu_append (alerts_submenu, _("Blink Tray _Icon"), "tab.alert_tray");

		action = g_simple_action_new_stateful ("alert_taskbar", NULL,
			g_variant_new_boolean (tab_get_setting_state (sess->alert_taskbar, hex_flash)));
		g_signal_connect (action, "activate", G_CALLBACK (tab_action_alert_taskbar), NULL);
		g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
		g_object_unref (action);
		g_menu_append (alerts_submenu, _("Blink Task _Bar"), "tab.alert_taskbar");

		g_menu_append_submenu (body_section, _("_Extra Alerts"), G_MENU_MODEL (alerts_submenu));
		g_object_unref (alerts_submenu);

		/* Per-channel Settings submenu */
		settings_submenu = g_menu_new ();

		action = g_simple_action_new_stateful ("logging", NULL,
			g_variant_new_boolean (tab_get_setting_state (sess->text_logging, prefs.hex_irc_logging)));
		g_signal_connect (action, "activate", G_CALLBACK (tab_action_logging), NULL);
		g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
		g_object_unref (action);
		g_menu_append (settings_submenu, _("_Log to Disk"), "tab.logging");

		action = g_simple_action_new_stateful ("scrollback", NULL,
			g_variant_new_boolean (tab_get_setting_state (sess->text_scrollback, prefs.hex_text_replay)));
		g_signal_connect (action, "activate", G_CALLBACK (tab_action_scrollback), NULL);
		g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
		g_object_unref (action);
		g_menu_append (settings_submenu, _("_Reload Scrollback"), "tab.scrollback");

		if (sess->type == SESS_CHANNEL)
		{
			action = g_simple_action_new_stateful ("strip_colors", NULL,
				g_variant_new_boolean (tab_get_setting_state (sess->text_strip, prefs.hex_text_stripcolor_msg)));
			g_signal_connect (action, "activate", G_CALLBACK (tab_action_strip_colors), NULL);
			g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
			g_object_unref (action);
			g_menu_append (settings_submenu, _("Strip _Colors"), "tab.strip_colors");

			action = g_simple_action_new_stateful ("hide_joinpart", NULL,
				g_variant_new_boolean (tab_get_setting_state (sess->text_hidejoinpart, prefs.hex_irc_conf_mode)));
			g_signal_connect (action, "activate", G_CALLBACK (tab_action_hide_joinpart), NULL);
			g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
			g_object_unref (action);
			g_menu_append (settings_submenu, _("_Hide Join/Part Messages"), "tab.hide_joinpart");
		}

		g_menu_append_submenu (body_section, _("_Settings"), G_MENU_MODEL (settings_submenu));
		g_object_unref (settings_submenu);

		/* Autojoin for channels */
		if (sess->type == SESS_CHANNEL && sess->server && sess->server->network)
		{
			gboolean is_autojoin = joinlist_is_in_list (sess->server, sess->channel);
			action = g_simple_action_new_stateful ("autojoin", NULL,
				g_variant_new_boolean (is_autojoin));
			g_signal_connect (action, "activate", G_CALLBACK (tab_action_autojoin), NULL);
			g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
			g_object_unref (action);
			g_menu_append (body_section, _("_Autojoin"), "tab.autojoin");
		}
		/* Auto-connect for server tabs */
		else if (sess->type == SESS_SERVER && sess->server && sess->server->network)
		{
			gboolean is_autoconnect = (((ircnet*)sess->server->network)->flags & FLAG_AUTO_CONNECT) != 0;
			action = g_simple_action_new_stateful ("autoconnect", NULL,
				g_variant_new_boolean (is_autoconnect));
			g_signal_connect (action, "activate", G_CALLBACK (tab_action_autoconnect), NULL);
			g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
			g_object_unref (action);
			g_menu_append (body_section, _("_Auto-Connect"), "tab.autoconnect");
		}

		g_menu_append_section (gmenu, NULL, G_MENU_MODEL (body_section));
		g_object_unref (body_section);
	}

	/* Main actions */
	g_menu_append (gmenu, _("_Detach"), "tab.detach");
	g_menu_append (gmenu, _("_Close"), "tab.close");

	/* Create and configure the popover */
	popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (gmenu));
	gtk_widget_insert_action_group (popover, "tab", G_ACTION_GROUP (action_group));
	gtk_widget_set_parent (popover, parent_widget);
	gtk_popover_set_pointing_to (GTK_POPOVER (popover),
								 &(GdkRectangle){ (int)x, (int)y, 1, 1 });
	gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);

	/* Restore chanview focus selection and clean up when popover is closed */
	if (cv)
		g_object_set_data (G_OBJECT (popover), "chanview", cv);
	g_signal_connect (popover, "closed", G_CALLBACK (tab_menu_popover_closed_cb), action_group);

	gtk_popover_popup (GTK_POPOVER (popover));
	g_object_unref (gmenu);
}

static gboolean
mg_tab_contextmenu_cb (chanview *cv, chan *ch, int tag, gpointer ud, GtkWidget *parent, double x, double y)
{
	if (tag == TAG_IRC)
		mg_create_tabmenu (cv, ud, ch, parent, x, y);
	else
		mg_create_tabmenu (cv, NULL, ch, parent, x, y);
	return TRUE;
}

void
mg_dnd_drop_file (session *sess, char *target, char *uri)
{
	char *p, *data, *next, *fname;

	p = data = g_strdup (uri);
	while (*p)
	{
		next = strchr (p, '\r');
		if (g_ascii_strncasecmp ("file:", p, 5) == 0)
		{
			if (next)
				*next = 0;
			fname = g_filename_from_uri (p, NULL, NULL);
			if (fname)
			{
				/* dcc_send() expects utf-8 */
				p = g_filename_from_utf8 (fname, -1, 0, 0, 0);
				if (p)
				{
					dcc_send (sess, target, p, prefs.hex_dcc_max_send_cps, 0);
					g_free (p);
				}
				g_free (fname);
			}
		}
		if (!next)
			break;
		p = next + 1;
		if (*p == '\n')
			p++;
	}
	g_free (data);

}

/* add a tabbed channel */

static void
mg_add_chan (session *sess)
{
	GdkPixbuf *icon;
	char *name = _("<none>");

	if (sess->channel[0])
		name = sess->channel;

	switch (sess->type)
	{
	case SESS_CHANNEL:
		icon = pix_tree_channel;
		break;
	case SESS_SERVER:
		icon = (sess->server->network_icon && prefs.hex_gui_network_icons)
		       ? (GdkPixbuf *)sess->server->network_icon : pix_tree_server;
		break;
	default:
		icon = pix_tree_dialog;
	}

	sess->res->tab = chanview_add (sess->gui->chanview, name, sess->server, sess,
											 sess->type == SESS_SERVER ? FALSE : TRUE,
											 TAG_IRC, icon);
	if (plain_list == NULL)
		mg_create_tab_colors ();

	chan_set_color (sess->res->tab, plain_list);

	if (sess->res->buffer == NULL)
	{
		sess->res->buffer = gtk_xtext_buffer_new (GTK_XTEXT (sess->gui->xtext));
		gtk_xtext_set_time_stamp (sess->res->buffer, prefs.hex_stamp_text);
		sess->res->user_model = userlist_create_model (sess);
	}
}

static void
mg_userlist_button (GtkWidget * box, char *label, char *cmd,
						  int a, int b, int c, int d)
{
	GtkWidget *wid = gtk_button_new_with_label (label);
	GtkWidget *label_widget;

	g_signal_connect (G_OBJECT (wid), "clicked",
							G_CALLBACK (userlist_button_cb), cmd);
	gtk_widget_add_css_class (wid, "poxchat-userlistbutton");
	gtk_button_set_can_shrink (GTK_BUTTON (wid), TRUE);

	/* Explicitly enable ellipsizing on the button label */
	label_widget = gtk_button_get_child (GTK_BUTTON (wid));
	if (GTK_IS_LABEL (label_widget))
	{
		gtk_label_set_ellipsize (GTK_LABEL (label_widget), PANGO_ELLIPSIZE_END);
		gtk_label_set_wrap (GTK_LABEL (label_widget), FALSE);
	}

	gtk_widget_set_hexpand (wid, TRUE);
	gtk_widget_set_vexpand (wid, FALSE);
	gtk_grid_attach (GTK_GRID (box), wid, a, c, b - a, d - c);
	show_and_unfocus (wid);
}

/* Detent hint for the userlist button grid. Buttons ellipsize to "...",
 * two per row. Use Pango's approximate_char_width (digit-average, what GTK
 * uses for width_chars) rather than 'n' width — 'n' over-predicts, and the
 * overshoot grows at small fonts due to hinting rounding. */
static int
mg_ulist_buttons_detent_min (GtkWidget *grid)
{
	int approx_cw = hc_widget_approx_char_width (grid);
	/* 2 buttons/row, each ~= 2 char widths of label + ~12px button padding */
	return 2 * (2 * approx_cw + 12);
}

static GtkWidget *
mg_create_userlistbuttons (GtkWidget *box)
{
	struct popup *pop;
	GSList *list = button_list;
	int a = 0, b = 0;
	GtkWidget *tab;

	tab = gtk_grid_new ();
	gtk_grid_set_column_spacing (GTK_GRID (tab), GUI_PANE_MARGIN);
	gtk_grid_set_column_homogeneous (GTK_GRID (tab), TRUE);
	/* Allow userlist panel to shrink below button grid's natural width */
	gtk_widget_set_size_request (tab, 1, -1);
	/* Scrolled window (meters) above already supplies the 6px universal gap
	 * via its own bottom padding; no extra margin needed here. */
	/* pack_end places buttons at bottom, matching GTK2 behavior */
	gtk_box_append (GTK_BOX (box), tab);
	mg_set_detent_min_func (tab, mg_ulist_buttons_detent_min);

	while (list)
	{
		pop = list->data;
		if (pop->cmd[0])
		{
			mg_userlist_button (tab, pop->name, pop->cmd, a, a + 1, b, b + 1);
			a++;
			if (a == 2)
			{
				a = 0;
				b++;
			}
		}
		list = list->next;
	}

	return tab;
}

static void
mg_topic_cb (GtkWidget *entry, gpointer userdata)
{
	session *sess = current_sess;
	char *text;

	if (sess->channel[0] && sess->server->connected && sess->type == SESS_CHANNEL)
	{
		text = (char *)hc_entry_get_text (entry);
		if (text[0] == 0)
			text = NULL;
		sess->server->p_topic (sess->server, sess->channel, text);
	} else
		hc_entry_set_text (entry, "");
	/* restore focus to the input widget, where the next input will most
likely be */
	gtk_widget_grab_focus (sess->gui->input_box);
}

/* Handle special keys in topic entry - Down/Escape return focus to input box */
static gboolean
mg_topic_key_press (GtkEventControllerKey *controller, guint keyval,
                    guint keycode, GdkModifierType state, gpointer userdata)
{
	session *sess = current_sess;

	(void)controller;
	(void)keycode;
	(void)state;
	(void)userdata;

	if (keyval == GDK_KEY_Down || keyval == GDK_KEY_Escape)
	{
		if (sess && sess->gui && sess->gui->input_box)
		{
			gtk_widget_grab_focus (sess->gui->input_box);
			return TRUE;
		}
	}

	return FALSE;
}

static void
mg_tabwindow_kill_cb (GtkWidget *win, gpointer userdata)
{
	GSList *list, *next;
	session *sess;

	poxchat_is_quitting = TRUE;

	/* see if there's any non-tab windows left */
	list = sess_list;
	while (list)
	{
		sess = list->data;
		next = list->next;
		if (!sess->gui->is_tab)
		{
			poxchat_is_quitting = FALSE;
/*			puts("-> will not exit, some toplevel windows left");*/
		} else
		{
			mg_ircdestroy (sess);
		}
		list = next;
	}

	current_tab = NULL;
	active_tab = NULL;
	mg_gui = NULL;
	parent_window = NULL;
}

static GtkWidget *
mg_changui_destroy (session *sess)
{
	GtkWidget *ret = NULL;

	if (sess->gui->is_tab)
	{
		/* avoid calling the "destroy" callback */
		g_signal_handlers_disconnect_by_func (G_OBJECT (sess->gui->window),
														  mg_tabwindow_kill_cb, 0);
		/* remove the tab from the chanview */
		if (!mg_chan_remove (sess->res->tab))
			/* if the window still exists, restore the signal handler */
			g_signal_connect (G_OBJECT (sess->gui->window), "destroy",
									G_CALLBACK (mg_tabwindow_kill_cb), 0);
	} else
	{
		/* avoid calling the "destroy" callback */
		g_signal_handlers_disconnect_by_func (G_OBJECT (sess->gui->window),
														  mg_topdestroy_cb, sess);
			/* don't destroy until the new one is created. Not sure why, but */
		/* it fixes: Gdk-CRITICAL **: gdk_colormap_get_screen: */
		/*           assertion `GDK_IS_COLORMAP (cmap)' failed */
		ret = sess->gui->window;
		g_free (sess->gui);
		sess->gui = NULL;
	}
	return ret;
}

static void
mg_link_irctab (session *sess, int focus)
{
	GtkWidget *win;

	if (sess->gui->is_tab)
	{
		win = mg_changui_destroy (sess);
		mg_changui_new (sess, sess->res, 0, focus);
		mg_populate (sess);
		poxchat_is_quitting = FALSE;
		if (win)
			hc_window_destroy_fn (GTK_WINDOW (win));
		return;
	}

	mg_unpopulate (sess);
	win = mg_changui_destroy (sess);
	mg_changui_new (sess, sess->res, 1, focus);
	/* the buffer is now attached to a different widget */
	((xtext_buffer *)sess->res->buffer)->xtext = (GtkXText *)sess->gui->xtext;
	if (win)
		hc_window_destroy_fn (GTK_WINDOW (win));
}

void
mg_detach (session *sess, int mode)
{
	switch (mode)
	{
	/* detach only */
	case 1:
		if (sess->gui->is_tab)
			mg_link_irctab (sess, 1);
		break;
	/* attach only */
	case 2:
		if (!sess->gui->is_tab)
			mg_link_irctab (sess, 1);
		break;
	/* toggle */
	default:
		mg_link_irctab (sess, 1);
	}
}

static int
check_is_number (char *t)
{
	while (*t)
	{
		if (*t < '0' || *t > '9')
			return FALSE;
		t++;
	}
	return TRUE;
}

static void
mg_change_flag (GtkWidget * wid, session *sess, char flag)
{
	server *serv = sess->server;
	char mode[3];

	mode[1] = flag;
	mode[2] = '\0';
	if (serv->connected && sess->channel[0])
	{
		if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (wid)))
			mode[0] = '+';
		else
			mode[0] = '-';
		serv->p_mode (serv, sess->channel, mode);
		serv->p_join_info (serv, sess->channel);
		sess->ignore_mode = TRUE;
		sess->ignore_date = TRUE;
	}
}

static void
flagl_hit (GtkWidget * wid, struct session *sess)
{
	char modes[512];
	const char *limit_str;
	server *serv = sess->server;

	if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (wid)))
	{
		if (serv->connected && sess->channel[0])
		{
			limit_str = hc_entry_get_text (sess->gui->limit_entry);
			if (check_is_number ((char *)limit_str) == FALSE)
			{
				fe_message (_("User limit must be a number!\n"), FE_MSG_ERROR);
				hc_entry_set_text (sess->gui->limit_entry, "");
				gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (wid), FALSE);
				return;
			}
			g_snprintf (modes, sizeof (modes), "+l %d", atoi (limit_str));
			serv->p_mode (serv, sess->channel, modes);
			serv->p_join_info (serv, sess->channel);
		}
	} else
		mg_change_flag (wid, sess, 'l');
}

static void
flagk_hit (GtkWidget * wid, struct session *sess)
{
	char modes[512];
	server *serv = sess->server;

	if (serv->connected && sess->channel[0])
	{
		g_snprintf (modes, sizeof (modes), "-k %s", 
			  hc_entry_get_text (sess->gui->key_entry));

		if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (wid)))
			modes[0] = '+';

		serv->p_mode (serv, sess->channel, modes);
	}
}

static void
mg_flagbutton_cb (GtkWidget *but, char *flag)
{
	session *sess;
	char mode;

	if (ignore_chanmode)
		return;

	sess = current_sess;
	mode = tolower ((unsigned char) flag[0]);

	switch (mode)
	{
	case 'l':
		flagl_hit (but, sess);
		break;
	case 'k':
		flagk_hit (but, sess);
		break;
	case 'b':
		ignore_chanmode = TRUE;
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (sess->gui->flag_b), FALSE);
		ignore_chanmode = FALSE;
		banlist_opengui (sess);
		break;
	default:
		mg_change_flag (but, sess, mode);
	}

	/* Return focus to main input box after clicking mode button */
	if (sess && sess->gui && sess->gui->input_box)
		gtk_widget_grab_focus (sess->gui->input_box);
}

static GtkWidget *
mg_create_flagbutton (char *tip, GtkWidget *box, char *face)
{
	GtkWidget *btn, *lbl;
	char label_markup[16];

	g_snprintf (label_markup, sizeof(label_markup), "<tt>%s</tt>", face);
	lbl = gtk_label_new (NULL);
	gtk_label_set_markup (GTK_LABEL(lbl), label_markup);

	btn = gtk_toggle_button_new ();
	/* GTK4: Add CSS class for compact styling */
	gtk_widget_add_css_class (btn, "poxchat-modebutton");
	gtk_widget_set_tooltip_text (btn, tip);
	gtk_button_set_child (GTK_BUTTON (btn), lbl);

	gtk_box_append (GTK_BOX (box), btn);
	g_signal_connect (G_OBJECT (btn), "toggled",
							G_CALLBACK (mg_flagbutton_cb), face);
	show_and_unfocus (btn);

	return btn;
}

static void
mg_key_entry_cb (GtkWidget * igad, gpointer userdata)
{
	char modes[512];
	session *sess = current_sess;
	server *serv = sess->server;

	(void)userdata;

	if (serv->connected && sess->channel[0])
	{
		g_snprintf (modes, sizeof (modes), "+k %s",
				hc_entry_get_text (igad));
		serv->p_mode (serv, sess->channel, modes);
		serv->p_join_info (serv, sess->channel);
	}

	/* Return focus to main input box */
	if (sess->gui && sess->gui->input_box)
		gtk_widget_grab_focus (sess->gui->input_box);
}

static void
mg_limit_entry_cb (GtkWidget * igad, gpointer userdata)
{
	char modes[512];
	session *sess = current_sess;
	server *serv = sess->server;

	(void)userdata;

	if (serv->connected && sess->channel[0])
	{
		if (check_is_number ((char *)hc_entry_get_text (igad)) == FALSE)
		{
			hc_entry_set_text (igad, "");
			fe_message (_("User limit must be a number!\n"), FE_MSG_ERROR);
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (sess->gui->flag_l), FALSE);
			/* Return focus to main input box */
			if (sess->gui && sess->gui->input_box)
				gtk_widget_grab_focus (sess->gui->input_box);
			return;
		}
		g_snprintf (modes, sizeof(modes), "+l %d",
				atoi (hc_entry_get_text (igad)));
		serv->p_mode (serv, sess->channel, modes);
		serv->p_join_info (serv, sess->channel);
	}

	/* Return focus to main input box */
	if (sess->gui && sess->gui->input_box)
		gtk_widget_grab_focus (sess->gui->input_box);
}

static void
mg_create_chanmodebuttons (session_gui *gui, GtkWidget *box)
{
	gui->flag_c = mg_create_flagbutton (_("Filter Colors"), box, "c");
	gui->flag_n = mg_create_flagbutton (_("No outside messages"), box, "n");
	gui->flag_t = mg_create_flagbutton (_("Topic Protection"), box, "t");
	gui->flag_i = mg_create_flagbutton (_("Invite Only"), box, "i");
	gui->flag_m = mg_create_flagbutton (_("Moderated"), box, "m");
	gui->flag_b = mg_create_flagbutton (_("Ban List"), box, "b");

	gui->flag_k = mg_create_flagbutton (_("Keyword"), box, "k");
	gui->key_entry = hex_input_edit_new ();
	hex_input_edit_set_multiline (HEX_INPUT_EDIT (gui->key_entry), FALSE);
	hex_input_edit_set_checked (HEX_INPUT_EDIT (gui->key_entry), FALSE);
	if (input_style && input_style->font_desc)
		hex_input_edit_set_font (HEX_INPUT_EDIT (gui->key_entry), input_style->font_desc);
	hex_input_edit_set_max_chars (HEX_INPUT_EDIT (gui->key_entry), 23);
	hex_input_edit_set_width_chars (HEX_INPUT_EDIT (gui->key_entry), 4);
	hex_input_edit_set_max_width_chars (HEX_INPUT_EDIT (gui->key_entry), 8);
	gtk_widget_set_name (gui->key_entry, "poxchat-inputbox");
	gtk_widget_set_hexpand (gui->key_entry, FALSE);
	gtk_box_append (GTK_BOX (box), gui->key_entry);
	g_signal_connect (G_OBJECT (gui->key_entry), "activate",
							G_CALLBACK (mg_key_entry_cb), NULL);
	/* Return focus to main input box on Down/Escape key */
	{
		GtkEventController *key_controller = gtk_event_controller_key_new ();
		g_signal_connect (key_controller, "key-pressed",
								G_CALLBACK (mg_topic_key_press), NULL);
		gtk_widget_add_controller (gui->key_entry, key_controller);
	}

	gui->flag_l = mg_create_flagbutton (_("User Limit"), box, "l");
	gui->limit_entry = hex_input_edit_new ();
	hex_input_edit_set_multiline (HEX_INPUT_EDIT (gui->limit_entry), FALSE);
	hex_input_edit_set_checked (HEX_INPUT_EDIT (gui->limit_entry), FALSE);
	if (input_style && input_style->font_desc)
		hex_input_edit_set_font (HEX_INPUT_EDIT (gui->limit_entry), input_style->font_desc);
	hex_input_edit_set_max_chars (HEX_INPUT_EDIT (gui->limit_entry), 10);
	hex_input_edit_set_width_chars (HEX_INPUT_EDIT (gui->limit_entry), 2);
	hex_input_edit_set_max_width_chars (HEX_INPUT_EDIT (gui->limit_entry), 3);
	gtk_widget_set_name (gui->limit_entry, "poxchat-inputbox");
	gtk_widget_set_hexpand (gui->limit_entry, FALSE);
	gtk_box_append (GTK_BOX (box), gui->limit_entry);
	g_signal_connect (G_OBJECT (gui->limit_entry), "activate",
							G_CALLBACK (mg_limit_entry_cb), NULL);
	/* Return focus to main input box on Down/Escape key */
	{
		GtkEventController *key_controller = gtk_event_controller_key_new ();
		g_signal_connect (key_controller, "key-pressed",
								G_CALLBACK (mg_topic_key_press), NULL);
		gtk_widget_add_controller (gui->limit_entry, key_controller);
	}

}


static void
mg_dialog_button_cb (GtkWidget *wid, char *cmd)
{
	/* the longest cmd is 12, and the longest nickname is 64 */
	char buf[128];
	char *host = "";
	char *topic;

	if (!current_sess)
		return;

	topic = (char *)(hc_entry_get_text (current_sess->gui->topic_entry));
	topic = strrchr (topic, '@');
	if (topic)
		host = topic + 1;

	auto_insert (buf, sizeof (buf), cmd, 0, 0, "", "", "",
					 server_get_network (current_sess->server, TRUE), host, "",
					 current_sess->channel, "");

	handle_command (current_sess, buf, TRUE);

	/* dirty trick to avoid auto-selection */
	SPELL_ENTRY_SET_EDITABLE (current_sess->gui->input_box, FALSE);
	gtk_widget_grab_focus (current_sess->gui->input_box);
	SPELL_ENTRY_SET_EDITABLE (current_sess->gui->input_box, TRUE);
}

static void
mg_dialog_button (GtkWidget *box, char *name, char *cmd)
{
	GtkWidget *wid;

	wid = gtk_button_new_with_label (name);
	gtk_box_append (GTK_BOX (box), wid);
	g_signal_connect (G_OBJECT (wid), "clicked",
							G_CALLBACK (mg_dialog_button_cb), cmd);
}

static void
mg_create_dialogbuttons (GtkWidget *box)
{
	struct popup *pop;
	GSList *list = dlgbutton_list;

	while (list)
	{
		pop = list->data;
		if (pop->cmd[0])
			mg_dialog_button (box, pop->name, pop->cmd);
		list = list->next;
	}
}

static void
mg_create_topicbar (session *sess, GtkWidget *box)
{
	GtkWidget *hbox, *topic, *bbox;
	session_gui *gui = sess->gui;

	gui->topic_bar = hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_margin_top (hbox, GUI_PANE_MARGIN);
	gtk_box_append (GTK_BOX (box), hbox);

	if (!gui->is_tab)
		sess->res->tab = NULL;

	gui->topic_entry = topic = hex_input_edit_new ();
	hex_input_edit_set_multiline (HEX_INPUT_EDIT (topic), FALSE);
	hex_input_edit_set_checked (HEX_INPUT_EDIT (topic), FALSE);
	if (input_style && input_style->font_desc)
		hex_input_edit_set_font (HEX_INPUT_EDIT (topic), input_style->font_desc);
	gtk_widget_set_name (topic, "poxchat-inputbox");
	gtk_widget_set_hexpand (topic, TRUE);
	gtk_box_append (GTK_BOX (hbox), topic);
	g_signal_connect (G_OBJECT (topic), "activate",
							G_CALLBACK (mg_topic_cb), 0);

	/* Return focus to main input box on Down/Escape key */
	{
		GtkEventController *key_controller = gtk_event_controller_key_new ();
		g_signal_connect (key_controller, "key-pressed",
								G_CALLBACK (mg_topic_key_press), NULL);
		gtk_widget_add_controller (topic, key_controller);
	}

	gui->topicbutton_box = bbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, GUI_PANE_MARGIN);
	gtk_widget_set_margin_start (bbox, GUI_PANE_MARGIN);
	gtk_box_append (GTK_BOX (hbox), bbox);
	mg_create_chanmodebuttons (gui, bbox);

	gui->dialogbutton_box = bbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, GUI_PANE_MARGIN);
	gtk_widget_set_margin_start (bbox, GUI_PANE_MARGIN);
	gtk_box_append (GTK_BOX (hbox), bbox);
	mg_create_dialogbuttons (bbox);
}

/* check if a word is clickable */

static int
mg_word_check (GtkWidget * xtext, char *word)
{
	session *sess = current_sess;
	int ret;

	ret = url_check_word (word);
	if (ret == 0 && sess->type == SESS_DIALOG)
		return WORD_DIALOG;

	return ret;
}

/* Callback for scroll-to-top: request older history (chathistory BEFORE) */
static void
mg_scroll_to_top_cb (GtkXText *xtext, gpointer userdata)
{
	session *sess = current_sess;
	(void)xtext;
	(void)userdata;

	if (sess && prefs.hex_irc_chathistory_scroll)
	{
		chathistory_request_older (sess);
	}
}

/* Clear react-with-text state */
static void
mg_clear_react_state (session *sess)
{
	g_clear_pointer (&sess->react_target_msgid, g_free);
	g_clear_pointer (&sess->react_target_nick, g_free);
}

/* Clear reply state */
static void
mg_clear_reply_state (session *sess)
{
	g_clear_pointer (&sess->reply_msgid, g_free);
	g_clear_pointer (&sess->reply_nick, g_free);
}

/* Hover reply button clicked — set reply state on session */
static void
mg_reply_button_cb (GtkXText *xtext, const char *msgid, const char *nick, gpointer userdata)
{
	session *sess = current_sess;
	(void)xtext;
	(void)userdata;

	if (!sess)
		return;

	mg_clear_react_state (sess);  /* mutually exclusive */
	g_free (sess->reply_msgid);
	sess->reply_msgid = g_strdup (msgid);
	g_free (sess->reply_nick);
	sess->reply_nick = g_strdup (nick);
	fe_reply_state_changed (sess);

	if (sess->gui && sess->gui->input_box)
		gtk_widget_grab_focus (sess->gui->input_box);
}

/* Hover react-text button clicked — set react-with-text state on session */
static void
mg_react_text_button_cb (GtkXText *xtext, const char *msgid, const char *nick, gpointer userdata)
{
	session *sess = current_sess;
	(void)xtext;
	(void)userdata;

	if (!sess)
		return;

	mg_clear_reply_state (sess);  /* mutually exclusive */
	g_free (sess->react_target_msgid);
	sess->react_target_msgid = g_strdup (msgid);
	g_free (sess->react_target_nick);
	sess->react_target_nick = g_strdup (nick);
	fe_reply_state_changed (sess);

	if (sess->gui && sess->gui->input_box)
		gtk_widget_grab_focus (sess->gui->input_box);
}

/* Emoji picked from hover react-emoji button */
static void
mg_react_emoji_picked_cb (GtkWidget *chooser, const char *emoji, gpointer user_data)
{
	session *sess = current_sess;
	(void)user_data;

	gtk_popover_popdown (GTK_POPOVER (chooser));

	if (sess && sess->react_target_msgid && sess->server && sess->server->connected)
	{
		char *cmd = g_strdup_printf ("REACT %s %s", emoji, sess->react_target_msgid);
		handle_command (sess, cmd, FALSE);
		g_free (cmd);
	}

	if (sess)
	{
		g_clear_pointer (&sess->react_target_msgid, g_free);
		mg_focus (sess);
	}
}

/* Hover react-emoji button clicked — open emoji picker */
static void
mg_react_emoji_button_cb (GtkXText *xtext, const char *msgid, const char *nick, gpointer userdata)
{
	session *sess = current_sess;
	GtkWidget *chooser;
	(void)userdata;
	(void)nick;

	if (!sess || !sess->server || !sess->server->have_message_tags)
		return;

	g_free (sess->react_target_msgid);
	sess->react_target_msgid = g_strdup (msgid);

	chooser = hex_emoji_chooser_new ();
	if (xtext->emoji_cache)
		hex_emoji_chooser_set_emoji_cache (HEX_EMOJI_CHOOSER (chooser), xtext->emoji_cache);
	gtk_widget_set_parent (chooser, GTK_WIDGET (xtext));
	gtk_popover_set_pointing_to (GTK_POPOVER (chooser),
	                             &(GdkRectangle){ xtext->react_emoji_btn_x, xtext->hover_btn_y, 1, 1 });
	g_signal_connect (chooser, "emoji-picked", G_CALLBACK (mg_react_emoji_picked_cb), NULL);
	gtk_popover_popup (GTK_POPOVER (chooser));
}

/* Hover redact button clicked — two-click confirmation then send REDACT */
static void
mg_redact_button_cb (GtkXText *xtext, const char *msgid, const char *nick, gpointer userdata)
{
	session *sess = current_sess;
	(void)nick;
	(void)userdata;

	if (!sess || !sess->server || !sess->server->have_redact ||
	    !sess->server->connected || !sess->channel[0] || !msgid)
		return;

	/* Two-click: first click sets confirm state, second click sends */
	if (xtext->redact_confirm_ent == xtext->hover_ent &&
	    (g_get_monotonic_time () - xtext->redact_confirm_time) < 3 * G_USEC_PER_SEC)
	{
		/* Entry still awaiting echo/labeled-response — msgid is a
		 * "pending:<label>" placeholder the server doesn't know.  Mark
		 * the entry and let fe_confirm_entry fire REDACT once the real
		 * msgid binds. */
		if (gtk_xtext_entry_get_state (xtext->hover_ent) == XTEXT_STATE_PENDING)
		{
			gtk_xtext_entry_set_pending_redact (xtext->hover_ent, TRUE);
			fe_toast_show (sess, _("Will delete once the message is confirmed by the server"),
			               3000, TOAST_TYPE_INFO, 0);
		}
		else
		{
			char *cmd = g_strdup_printf ("REDACT %s %s", sess->channel, msgid);
			handle_command (sess, cmd, FALSE);
			g_free (cmd);
		}
		xtext->redact_confirm_ent = NULL;
	}
	else
	{
		xtext->redact_confirm_ent = xtext->hover_ent;
		xtext->redact_confirm_time = g_get_monotonic_time ();
		fe_toast_show (sess, _("Click \xc3\x97 again to delete this message"), 3000,
		               TOAST_TYPE_ERROR, 0);
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
}

/* Reaction badge clicked — toggle reaction on/off */
static void
mg_reaction_click_cb (GtkXText *xtext, const char *msgid, const char *reaction_text,
                      gboolean is_self, gpointer userdata)
{
	session *sess = current_sess;
	(void)xtext;
	(void)userdata;

	if (!sess || !sess->server || !sess->server->connected ||
	    !sess->server->have_message_tags || !sess->channel[0])
		return;

	if (is_self)
	{
		char *cmd = g_strdup_printf ("UNREACT %s %s", reaction_text, msgid);
		handle_command (sess, cmd, FALSE);
		g_free (cmd);
	}
	else
	{
		char *cmd = g_strdup_printf ("REACT %s %s", reaction_text, msgid);
		handle_command (sess, cmd, FALSE);
		g_free (cmd);
	}

	mg_focus (sess);
}

/* mouse click inside text area */

static void
mg_word_clicked (GtkWidget *xtext_widget, char *word, gpointer event_unused)
{
	session *sess = current_sess;
	int word_type = 0, start, end;
	char *tmp;
	GtkXText *xtext = GTK_XTEXT (xtext_widget);

	/* GTK4: Get click info from xtext structure (stored before signal emission) */
	guint button = xtext->last_click_button;
	GdkModifierType state = xtext->last_click_state;
	int n_press = xtext->last_click_n_press;
	double x = xtext->last_click_x;
	double y = xtext->last_click_y;

	(void)event_unused;  /* Unused in GTK4 */

	if (word)
	{
		word_type = mg_word_check (xtext_widget, word);
		url_last (&start, &end);
	}

	if (button == 1)			/* left button */
	{
		if (word == NULL)
		{
			mg_focus (sess);
			return;
		}

		if ((state & 13) == (GdkModifierType)prefs.hex_gui_url_mod)
		{
			switch (word_type)
			{
			case WORD_URL:
			case WORD_HOST6:
			case WORD_HOST:
				word[end] = 0;
				fe_open_url (word + start);
			}
		}
		/* Always return focus to input box after clicking in text area */
		mg_focus (sess);
		return;
	}

	if (button == 2)
	{
		if (sess->type == SESS_DIALOG)
			menu_middlemenu (sess, xtext_widget, x, y);
		else if (n_press == 2)
			userlist_select (sess, word);
		return;
	}
	if (word == NULL)
		return;

	switch (word_type)
	{
	case 0:
	case WORD_PATH:
		menu_middlemenu (sess, xtext_widget, x, y);
		break;
	case WORD_URL:
	case WORD_HOST6:
	case WORD_HOST:
		word[end] = 0;
		word += start;
		menu_urlmenu (xtext_widget, x, y, word);
		break;
	case WORD_NICK:
		word[end] = 0;
		word += start;
		menu_nickmenu (sess, xtext_widget, x, y, word, FALSE);
		break;
	case WORD_CHANNEL:
		word[end] = 0;
		word += start;
		menu_chanmenu (sess, xtext_widget, x, y, word);
		break;
	case WORD_EMAIL:
		word[end] = 0;
		word += start;
		tmp = g_strdup_printf ("mailto:%s", word + (ispunct (*word) ? 1 : 0));
		menu_urlmenu (xtext_widget, x, y, tmp);
		g_free (tmp);
		break;
	case WORD_DIALOG:
		menu_nickmenu (sess, xtext_widget, x, y, sess->channel, FALSE);
		break;
	}
}

void
mg_update_xtext (GtkWidget *wid)
{
	GtkXText *xtext = GTK_XTEXT (wid);

	gtk_xtext_set_palette (xtext, colors);
	gtk_xtext_set_max_lines (xtext, prefs.hex_text_max_lines);
	gtk_xtext_set_background (xtext, channelwin_pix);
	gtk_xtext_set_wordwrap (xtext, prefs.hex_text_wordwrap);
	gtk_xtext_set_show_marker (xtext, prefs.hex_text_show_marker);
	gtk_xtext_set_show_separator (xtext, prefs.hex_text_indent ? prefs.hex_text_show_sep : 0);
	gtk_xtext_set_indent (xtext, prefs.hex_text_indent);
	if (!gtk_xtext_set_font (xtext, prefs.hex_text_font))
	{
		fe_message ("Failed to open any font. I'm out of here!", FE_MSG_WAIT | FE_MSG_ERROR);
		exit (1);
	}

	gtk_xtext_refresh (xtext);
}

static void
mg_create_textarea (session *sess, GtkWidget *box)
{
	GtkWidget *vbox, *frame;
	GtkXText *xtext;
	session_gui *gui = sess->gui;

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_vexpand (vbox, TRUE);
	gtk_box_append (GTK_BOX (box), vbox);

	frame = gtk_frame_new (NULL);
	gtk_widget_set_halign (frame, GTK_ALIGN_FILL);
	gtk_widget_set_valign (frame, GTK_ALIGN_FILL);
	gtk_widget_set_hexpand (frame, TRUE);
	gtk_widget_set_vexpand (frame, TRUE);
	gtk_widget_set_margin_top (frame, GUI_PANE_MARGIN);
	gtk_widget_set_margin_bottom (frame, GUI_PANE_MARGIN);
	gtk_box_append (GTK_BOX (vbox), frame);

	gui->xtext = gtk_xtext_new (colors, TRUE);
	gtk_widget_set_halign (gui->xtext, GTK_ALIGN_FILL);
	gtk_widget_set_valign (gui->xtext, GTK_ALIGN_FILL);
	xtext = GTK_XTEXT (gui->xtext);

	/* Wrap xtext in a GtkOverlay so layout-swap drops on xtext can show
	 * the same hover-indicator UI the vpanes have. When both sides have
	 * a pane, the drop handler rejects xtext drops entirely and the
	 * indicators stay hidden — they only light up on the paneless side
	 * during a drag. Indicator widgets are stashed on the xtext for the
	 * motion handler to find. */
	{
		GtkWidget *overlay = gtk_overlay_new ();
		GtkWidget *top_ind = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
		GtkWidget *bot_ind = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

		gtk_overlay_set_child (GTK_OVERLAY (overlay), GTK_WIDGET (xtext));

		gtk_widget_add_css_class (top_ind, "poxchat-drop-indicator");
		gtk_widget_set_can_target (top_ind, FALSE);
		gtk_widget_set_valign (top_ind, GTK_ALIGN_START);
		gtk_widget_set_halign (top_ind, GTK_ALIGN_START);
		gtk_widget_set_visible (top_ind, FALSE);
		gtk_overlay_add_overlay (GTK_OVERLAY (overlay), top_ind);

		gtk_widget_add_css_class (bot_ind, "poxchat-drop-indicator");
		gtk_widget_set_can_target (bot_ind, FALSE);
		gtk_widget_set_valign (bot_ind, GTK_ALIGN_END);
		gtk_widget_set_halign (bot_ind, GTK_ALIGN_START);
		gtk_widget_set_visible (bot_ind, FALSE);
		gtk_overlay_add_overlay (GTK_OVERLAY (overlay), bot_ind);

		g_object_set_data (G_OBJECT (gui->xtext), "hc-drop-top", top_ind);
		g_object_set_data (G_OBJECT (gui->xtext), "hc-drop-bottom", bot_ind);

		gtk_frame_set_child (GTK_FRAME (frame), overlay);
	}
	gtk_xtext_set_max_indent (xtext, prefs.hex_text_max_indent);
	gtk_xtext_set_thin_separator (xtext, prefs.hex_text_thin_sep);
	gtk_xtext_set_urlcheck_function (xtext, mg_word_check);
	gtk_xtext_set_max_lines (xtext, prefs.hex_text_max_lines);
	gtk_xtext_set_scroll_to_top_callback (xtext, mg_scroll_to_top_cb, NULL);
	gtk_xtext_set_reply_button_callback (xtext, mg_reply_button_cb, NULL);
	gtk_xtext_set_react_text_button_callback (xtext, mg_react_text_button_cb, NULL);
	gtk_xtext_set_react_emoji_button_callback (xtext, mg_react_emoji_button_cb, NULL);
	gtk_xtext_set_redact_button_callback (xtext, mg_redact_button_cb, NULL);
	gtk_xtext_set_reaction_click_callback (xtext, mg_reaction_click_cb, NULL);
	/* xtext is parented into the drop-indicator overlay above; the frame's
	 * child was set to that overlay already. */

	mg_update_xtext (GTK_WIDGET (xtext));

	g_signal_connect (G_OBJECT (xtext), "word_click",
							G_CALLBACK (mg_word_clicked), NULL);

	/* Layout-swap drops land on the two vpanes (handled where they're
	 * created) or on xtext for the paneless-side case. File drops for
	 * DCC also live on xtext. */
	mg_setup_xtext_dnd (gui->xtext);
}

static GtkWidget *
mg_create_infoframe (GtkWidget *box)
{
	GtkWidget *frame, *label, *hbox;

	frame = gtk_frame_new (0);
	gtk_widget_set_hexpand (frame, TRUE);
	gtk_widget_set_size_request (frame, 1, -1);
	gtk_box_append (GTK_BOX (box), frame);

	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_size_request (hbox, 1, -1);
	gtk_frame_set_child (GTK_FRAME (frame), hbox);

	label = gtk_label_new (NULL);
	gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand (label, TRUE);
	gtk_widget_set_halign (label, GTK_ALIGN_CENTER);
	gtk_widget_set_size_request (label, 1, -1);
	gtk_box_append (GTK_BOX (hbox), label);

	return label;
}

static void
mg_create_meters (session_gui *gui, GtkWidget *parent_box)
{
	GtkWidget *infbox, *wid, *box;

	infbox = box = gui->meter_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
	gtk_widget_set_size_request (box, 1, -1);
	gtk_box_append (GTK_BOX (parent_box), box);

	if ((prefs.hex_gui_lagometer & 2) || (prefs.hex_gui_throttlemeter & 2))
	{
		infbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, GUI_PANE_MARGIN);
		gtk_box_set_homogeneous (GTK_BOX (infbox), TRUE);
		gtk_widget_set_margin_bottom (infbox, GUI_PANE_MARGIN);
		gtk_widget_set_size_request (infbox, 1, -1);
		gtk_box_append (GTK_BOX (box), infbox);
	}

	if (prefs.hex_gui_lagometer & 1)
	{
		gui->lagometer = wid = gtk_progress_bar_new ();
#ifdef WIN32
		gtk_widget_set_size_request (wid, 1, 10);
#else
		gtk_widget_set_size_request (wid, 1, 8);
#endif

		wid = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
		gtk_widget_set_size_request (wid, 1, -1);
		gtk_box_append (GTK_BOX (wid), gui->lagometer);
		gtk_box_append (GTK_BOX (box), wid);
	}
	if (prefs.hex_gui_lagometer & 2)
	{
		gui->laginfo = wid = mg_create_infoframe (infbox);
		gtk_label_set_text ((GtkLabel *) wid, "Lag");
	}

	if (prefs.hex_gui_throttlemeter & 1)
	{
		gui->throttlemeter = wid = gtk_progress_bar_new ();
#ifdef WIN32
		gtk_widget_set_size_request (wid, 1, 10);
#else
		gtk_widget_set_size_request (wid, 1, 8);
#endif

		wid = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
		gtk_widget_set_size_request (wid, 1, -1);
		gtk_box_append (GTK_BOX (wid), gui->throttlemeter);
		gtk_box_append (GTK_BOX (box), wid);
	}
	if (prefs.hex_gui_throttlemeter & 2)
	{
		gui->throttleinfo = wid = mg_create_infoframe (infbox);
		gtk_label_set_text ((GtkLabel *) wid, "Throttle");
	}
}

void
mg_update_meters (session_gui *gui)
{
	GtkWidget *sibling;

	/* Remember the widget before meter_box so we can re-insert at the
	 * same position.  If meter_box is NULL (first call) or already
	 * removed, fall back to inserting before the button_box. */
	sibling = gui->meter_box
		? gtk_widget_get_prev_sibling (gui->meter_box)
		: gtk_widget_get_prev_sibling (gui->button_box);

	hc_widget_destroy_impl (gui->meter_box);
	gui->lagometer = NULL;
	gui->laginfo = NULL;
	gui->throttlemeter = NULL;
	gui->throttleinfo = NULL;

	mg_create_meters (gui, gui->button_box_parent);

	/* gtk_box_append put the new meter_box at the end — move it back
	 * to just after the original predecessor (i.e. before button_box). */
	if (gui->meter_box)
		gtk_box_reorder_child_after (GTK_BOX (gui->button_box_parent),
		                             gui->meter_box, sibling);
}

static void
mg_create_userlist (session_gui *gui, GtkWidget *box)
{
	GtkWidget *frame, *ulist, *vbox;

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_vexpand (vbox, TRUE);
	gtk_widget_set_margin_top (vbox, GUI_PANE_MARGIN);
	gtk_widget_set_margin_bottom (vbox, GUI_PANE_MARGIN);
	/* Allow userlist panel to shrink below natural minimum */
	gtk_widget_set_size_request (vbox, 1, -1);
	gtk_box_append (GTK_BOX (box), vbox);

	frame = gtk_frame_new (NULL);
	/* Count frame sits above the tree. The tree widget's own top row padding
	 * adds ~2px, so 4 here yields the 6px universal gap. */
	gtk_widget_set_margin_bottom (frame, 4);
	gtk_box_append (GTK_BOX (vbox), frame);
	gtk_widget_set_visible (frame, prefs.hex_gui_ulist_count);

	gui->namelistinfo = gtk_label_new (NULL);
	gtk_label_set_ellipsize (GTK_LABEL (gui->namelistinfo), PANGO_ELLIPSIZE_END);
	gtk_frame_set_child (GTK_FRAME (frame), gui->namelistinfo);

	gui->user_tree = ulist = userlist_create (vbox);

	mg_create_meters (gui, vbox);

	gui->button_box_parent = vbox;
	gui->button_box = mg_create_userlistbuttons (vbox);
}

/* Helper to validate pane size preference - returns 0 if invalid/corrupted */
static int
mg_validate_pane_size (int size)
{
	/* Reasonable range for pane sizes: 50-1000 pixels */
	if (size > 0 && size < 1000)
		return size;
	return 0;
}

/* Update the window's minimum size based on visible elements.
 * This prevents content from being pushed off-screen when resizing.
 * The minimum accounts for: topic bar buttons + chanview (if on left/right) + userlist (if visible)
 */
void
mg_update_window_minimum (session_gui *gui)
{
	int min_width = 0;
	int topicbar_min = 0;
	GtkWidget *win;
	int tab_pos;
	gboolean chanview_on_left = FALSE;
	gboolean chanview_on_right = FALSE;

	if (!gui || !gui->window)
		return;

	win = gui->window;
	tab_pos = prefs.hex_gui_tab_pos;

	/* Get the minimum width of the topic bar buttons (mode buttons, etc.)
	 * This is the primary constraint that can't shrink */
	if (gui->topicbutton_box && gtk_widget_get_visible (gui->topicbutton_box))
	{
		GtkRequisition min_req;
		gtk_widget_get_preferred_size (gui->topicbutton_box, &min_req, NULL);
		topicbar_min = min_req.width;
	}

	/* Also check dialog buttons if visible */
	if (gui->dialogbutton_box && gtk_widget_get_visible (gui->dialogbutton_box))
	{
		GtkRequisition min_req;
		gtk_widget_get_preferred_size (gui->dialogbutton_box, &min_req, NULL);
		if (min_req.width > topicbar_min)
			topicbar_min = min_req.width;
	}

	/* Base minimum: topic buttons + some space for topic entry + padding */
	min_width = topicbar_min + 100 + 30;

	/* Determine chanview position */
	if (gui->chanview && tab_pos != POS_HIDDEN)
	{
		if (tab_pos == POS_TOPLEFT || tab_pos == POS_BOTTOMLEFT)
			chanview_on_left = TRUE;
		else if (tab_pos == POS_TOPRIGHT || tab_pos == POS_BOTTOMRIGHT)
			chanview_on_right = TRUE;
		/* POS_TOP, POS_BOTTOM don't add horizontal width */
	}

	/* Add left pane width (chanview on left) */
	if (chanview_on_left)
	{
		int left_size = mg_validate_pane_size (prefs.hex_gui_pane_left_size);
		if (left_size > 0)
			min_width += left_size;
		else
			min_width += 100;  /* Default chanview width */
	}

	/* Add right pane width (userlist and/or chanview on right) */
	if (!gui->ul_hidden || chanview_on_right)
	{
		int right_size = mg_validate_pane_size (
			MAX (prefs.hex_gui_pane_right_size, prefs.hex_gui_pane_right_size_min));
		if (right_size > 0)
			min_width += right_size;
		else
			min_width += 100;  /* Default right pane width */
	}

	hc_debug_log ("mg_update_window_minimum: min_width=%d (topicbar=%d, left=%d, right=%d, ul_hidden=%d)",
	              min_width, topicbar_min,
	              chanview_on_left ? prefs.hex_gui_pane_left_size : 0,
	              (!gui->ul_hidden || chanview_on_right) ? prefs.hex_gui_pane_right_size : 0,
	              gui->ul_hidden);

	gtk_widget_set_size_request (win, min_width, 200);
}

static void
mg_vpane_cb (GtkPaned *pane, GParamSpec *param, session_gui *gui)
{
	int height = gtk_widget_get_height (GTK_WIDGET (pane));
	int position = gtk_paned_get_position (pane);

	hc_debug_log ("mg_vpane_cb: height=%d position=%d vpane_restored=%d",
	              height, position, gui ? gui->vpane_restored : -1);

	/* Don't save until the vpane has been restored from config */
	if (!gui || !gui->vpane_restored)
		return;

	/* Only save if pane has valid dimensions and position is reasonable */
	if (height > 0 && position > 0 && position < height)
	{
		hc_debug_log ("  -> saving position=%d", position);
		prefs.hex_gui_pane_divider_position = position;
	}
}

/* Data structure for deferred pane size calculation */
typedef struct {
	GtkPaned *pane;
	session_gui *gui;
	gboolean is_left_pane;
} PaneUpdateData;

static gboolean
mg_pane_idle_cb (gpointer user_data)
{
	PaneUpdateData *data = (PaneUpdateData *)user_data;
	session_gui *gui = data->gui;

	/* Now that layout is complete, update the window minimum */
	hc_debug_log ("mg_pane_idle_cb: is_left=%d, calling mg_update_window_minimum",
	              data->is_left_pane);
	mg_update_window_minimum (gui);

	if (data->is_left_pane)
	{
		int left_size = gtk_paned_get_position (data->pane);

		/* Update userlist column visibility if userlist is on the left pane */
		if (prefs.hex_gui_ulist_pos == POS_TOPLEFT ||
			prefs.hex_gui_ulist_pos == POS_BOTTOMLEFT)
			mg_update_userlist_columns (current_sess, left_size);

		/* Update chanview tree compact mode if chanview is on the left pane */
		if (gui->chanview &&
			(prefs.hex_gui_tab_pos == POS_TOPLEFT ||
			 prefs.hex_gui_tab_pos == POS_BOTTOMLEFT))
			chanview_update_pane_size (gui->chanview, left_size);
	}

	g_free (data);
	return G_SOURCE_REMOVE;
}

static void
mg_leftpane_cb (GtkPaned *pane, GParamSpec *param, session_gui *gui)
{
	mg_pane_apply_detent (pane, gtk_paned_get_start_child (pane), FALSE,
	                      G_CALLBACK (mg_leftpane_cb), gui);
	prefs.hex_gui_pane_left_size = gtk_paned_get_position (pane);
	hc_debug_log ("mg_leftpane_cb: position=%d -> left_size=%d",
	              gtk_paned_get_position (pane), prefs.hex_gui_pane_left_size);
	/* Defer window minimum update to avoid layout issues */
	{
		PaneUpdateData *data = g_new (PaneUpdateData, 1);
		data->pane = pane;
		data->gui = gui;
		data->is_left_pane = TRUE;
		g_idle_add (mg_pane_idle_cb, data);
	}
}

/* Approximate width of one character in the widget's current font.
 * Used to scale progressive-collapse thresholds so they still make
 * sense when the user runs a larger or smaller font. */
static int
mg_approx_char_width (GtkWidget *widget)
{
	return hc_widget_char_width (widget);
}

/* Progressive column collapse as userlist pane shrinks:
 * 1. Host column hides (nick starts ellipsizing)
 * 2. Icon column hides (nick gets the extra space)
 * Nick column stays visible and clips against the viewport at narrow widths.
 * max_nick is measured synchronously from sess's user model via Pango,
 * so the correct column visibility is set before the ColumnView first
 * paints the new channel — avoids the full-pane flicker that happens
 * when columns reshuffle after the initial paint. Additive offsets
 * scale with font char width. */
static void
mg_update_userlist_columns (session *sess, int pane_size)
{
	session_gui *gui;
	GtkColumnViewColumn *host_col, *nick_col, *icon_col;
	int icon_width, max_nick, char_w;
	gboolean show_host, show_nick, show_icon;

	if (!sess || !sess->gui || !sess->gui->user_tree)
		return;
	gui = sess->gui;

	host_col = g_object_get_data (G_OBJECT (gui->user_tree), "host-column");
	nick_col = g_object_get_data (G_OBJECT (gui->user_tree), "nick-column");
	icon_col = g_object_get_data (G_OBJECT (gui->user_tree), "icon-column");
	icon_width = (icon_col && prefs.hex_gui_ulist_icons) ? 20 : 0;
	char_w = mg_approx_char_width (gui->user_tree);
	max_nick = userlist_measure_max_nick_width (gui->user_tree, sess);

	/* Host hides when there isn't room for icon + full nick + ~8 chars
	 * of host content. */
	show_host = (prefs.hex_gui_ulist_show_hosts &&
		pane_size > icon_width + max_nick + 8 * char_w);
	/* Keep the mode icon visible until the pane can't fit icon + ~8
	 * chars of nick room. Previously this was tied to the full nick
	 * fitting (`max_nick + 20`), which hid the icon aggressively on
	 * narrow panes — unnecessary now that nick has width_chars=3 and
	 * just clips. */
	show_icon = show_host || (pane_size > icon_width + 8 * char_w);
	/* Nick column stays visible at all widths — the label has width_chars=3,
	 * so its content clips against the viewport when the pane is narrower
	 * than ~3 chars. This also keeps column view full_width > 0 so the
	 * internal listview never hits the bounds.y assertion at 0 width. */
	show_nick = TRUE;

	/* Apply visibility */
	if (icon_col && prefs.hex_gui_ulist_icons)
	{
		gboolean was_visible = gtk_column_view_column_get_visible (icon_col);
		gtk_column_view_column_set_visible (icon_col, show_icon);
		/* GtkColumnView doesn't rebind cached row children on column
		 * visibility change, so refresh already-bound nick labels to
		 * add/strip the prefix char immediately. */
		if (was_visible != show_icon)
			userlist_refresh_nick_labels (gui->user_tree);
	}
	if (nick_col)
		gtk_column_view_column_set_visible (nick_col, show_nick);
	if (host_col && prefs.hex_gui_ulist_show_hosts)
		gtk_column_view_column_set_visible (host_col, show_host);

	/* Nick only ellipsizes after host is hidden */
	userlist_set_nick_ellipsize (gui->user_tree, show_nick && !show_host);
}

/* Return the width currently allocated to the userlist pane based on where
 * the userlist lives. Returns 0 if it can't be determined yet. */
static int
mg_userlist_pane_size (session_gui *gui)
{
	if (!gui)
		return 0;

	if (prefs.hex_gui_ulist_pos == POS_TOPLEFT ||
	    prefs.hex_gui_ulist_pos == POS_BOTTOMLEFT)
	{
		return gui->hpane_left ? gtk_paned_get_position (GTK_PANED (gui->hpane_left)) : 0;
	}
	else
	{
		int pane_width, position, size;
		if (!gui->hpane_right)
			return 0;
		pane_width = gtk_widget_get_width (gui->hpane_right);
		position = gtk_paned_get_position (GTK_PANED (gui->hpane_right));
		if (pane_width <= 0)
			return 0;
		/* `pane_width - position` includes the handle; subtract it so
		 * this matches the left-side semantics (vpane allocation only). */
		size = pane_width - position - MG_PANED_HANDLE_W;
		return size > 0 ? size : 0;
	}
}

/* Safety-net idle after mg_populate_userlist's synchronous column update:
 * verifies that after the ColumnView binds its first batch of row labels
 * the collapse thresholds still hold. Normally a no-op.
 *
 * The scheduling session is ignored: column visibility is a property of
 * the shared ColumnView, driven by the currently-displayed channel's
 * user_model. A background-channel WHO burst queuing us would otherwise
 * measure that channel's widest nick and apply the result to the
 * foreground view — exactly the host-column flash observed during
 * remote channel WHO responses. */
static gboolean
mg_userlist_update_columns_idle (gpointer user_data)
{
	session *queued_sess = (session *)user_data;
	session *sess;
	int pane_size;

	if (!is_session (queued_sess) || !queued_sess->gui)
		return G_SOURCE_REMOVE;

	sess = current_sess;
	if (!sess || !sess->gui || !sess->gui->user_tree)
		return G_SOURCE_REMOVE;

	/* Clear the pending flag on both widgets — in tabbed mode they're
	 * the same widget so this is idempotent, but a background channel
	 * with its own toplevel would otherwise have its flag stuck set
	 * and never queue another update. */
	g_object_set_data (G_OBJECT (queued_sess->gui->user_tree),
		"hc-userlist-update-id", GUINT_TO_POINTER (0));
	g_object_set_data (G_OBJECT (sess->gui->user_tree),
		"hc-userlist-update-id", GUINT_TO_POINTER (0));

	pane_size = mg_userlist_pane_size (sess->gui);
	if (pane_size > 0)
		mg_update_userlist_columns (sess, pane_size);

	return G_SOURCE_REMOVE;
}

void
mg_queue_userlist_update (session *sess)
{
	guint id;

	if (!sess || !sess->gui || !sess->gui->user_tree)
		return;

	/* Coalesce: during a WHO burst (dozens of fe_userlist_update calls in
	 * rapid succession) we only need one column-recompute pass, not one
	 * per user. Without this the host column flickers as each response
	 * races the pane allocation around the collapse threshold. */
	id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (sess->gui->user_tree),
	                                           "hc-userlist-update-id"));
	if (id != 0)
		return;

	id = g_idle_add (mg_userlist_update_columns_idle, sess);
	g_object_set_data (G_OBJECT (sess->gui->user_tree),
		"hc-userlist-update-id", GUINT_TO_POINTER (id));
}

/* Fire the pane-dependent UI updates (userlist columns, chanview compact
 * mode) against the currently-allocated pane sizes. Needed after layout
 * rebuilds and after font/setup changes, since those paths don't move
 * the pane separators so the notify::position chain never fires. */
static gboolean
mg_layout_update_idle_cb (gpointer user_data)
{
	session_gui *gui = (session_gui *)user_data;
	int left_size = 0, right_size = 0;

	if (!gui)
		return G_SOURCE_REMOVE;

	if (gui->hpane_left)
		left_size = gtk_paned_get_position (GTK_PANED (gui->hpane_left));

	if (gui->hpane_right)
	{
		int pane_width = gtk_widget_get_width (gui->hpane_right);
		int position = gtk_paned_get_position (GTK_PANED (gui->hpane_right));
		/* vpane allocation only, to match left-side `position` semantics. */
		right_size = (pane_width > 0) ? pane_width - position - MG_PANED_HANDLE_W : 0;
	}

	if (current_sess)
	{
		if (prefs.hex_gui_ulist_pos == POS_TOPLEFT ||
		    prefs.hex_gui_ulist_pos == POS_BOTTOMLEFT)
		{
			if (left_size > 0)
				mg_update_userlist_columns (current_sess, left_size);
		}
		else
		{
			if (right_size > 0)
				mg_update_userlist_columns (current_sess, right_size);
		}
	}

	if (gui->chanview)
	{
		if ((prefs.hex_gui_tab_pos == POS_TOPLEFT ||
		     prefs.hex_gui_tab_pos == POS_BOTTOMLEFT) && left_size > 0)
			chanview_update_pane_size (gui->chanview, left_size);
		else if ((prefs.hex_gui_tab_pos == POS_TOPRIGHT ||
		          prefs.hex_gui_tab_pos == POS_BOTTOMRIGHT) && right_size > 0)
			chanview_update_pane_size (gui->chanview, right_size);
	}

	return G_SOURCE_REMOVE;
}

/* Deferred right pane size save — only fires from user-initiated pane drags
 * (the signal is blocked during programmatic show/hide/restore). */
static gboolean
mg_rightpane_idle_cb (gpointer user_data)
{
	session_gui *gui = (session_gui *)user_data;
	GtkPaned *pane = GTK_PANED (gui->hpane_right);
	int pane_width, position, right_size, vpane_size;

	/* Get dimensions after layout is complete */
	pane_width = gtk_widget_get_width (GTK_WIDGET (pane));
	position = gtk_paned_get_position (pane);
	right_size = pane_width - position;
	/* vpane allocation only — matches left-side `position` semantics, so
	 * the collapse/column update funcs see symmetric values on both sides. */
	vpane_size = right_size - MG_PANED_HANDLE_W;

	if (pane_width > 0 && right_size > 0 && right_size < 2000)
	{
		prefs.hex_gui_pane_right_size = right_size;
		mg_update_window_minimum (gui);
	}

	/* Only update userlist columns if userlist is actually on the right pane.
	 * If the user has the userlist on the left, dragging the right separator
	 * would otherwise pass a nonsensical width (vpane_right's allocation, not
	 * userlist's), clobbering whatever the left-pane callback just set. */
	if (prefs.hex_gui_ulist_pos == POS_TOPRIGHT ||
		prefs.hex_gui_ulist_pos == POS_BOTTOMRIGHT)
		mg_update_userlist_columns (current_sess, vpane_size);

	/* Update chanview tree compact mode if chanview is on the right pane */
	if (gui->chanview &&
		(prefs.hex_gui_tab_pos == POS_TOPRIGHT ||
		 prefs.hex_gui_tab_pos == POS_BOTTOMRIGHT))
		chanview_update_pane_size (gui->chanview, vpane_size);

	return G_SOURCE_REMOVE;
}

/* Pixels of "overshoot" past the content's natural minimum before the
 * detent releases. Within this window the pane visually sticks at the
 * minimum while the cursor continues moving; past it, the paned follows
 * the cursor normally. Gives the user a chance to stop before their drag
 * starts clipping the content. */
#define MG_PANE_DETENT_STICKY_PX 25

#define MG_DETENT_MIN_KEY "poxchat-detent-min-func"

void
mg_set_detent_min_func (GtkWidget *widget, mg_detent_min_func func)
{
	if (widget)
		g_object_set_data (G_OBJECT (widget), MG_DETENT_MIN_KEY, (gpointer) func);
}

/* Walk the widget subtree; for each widget that declares a detent-min
 * via mg_set_detent_min_func, call the callback and take the max value.
 * Registered widgets are treated as leaves — we don't recurse into them. */
static int
mg_widget_detent_min (GtkWidget *widget)
{
	mg_detent_min_func func;
	GtkWidget *child;
	int max_min = 0, m;

	if (!widget)
		return 0;

	func = (mg_detent_min_func) g_object_get_data (G_OBJECT (widget), MG_DETENT_MIN_KEY);
	if (func)
		return func (widget);

	for (child = gtk_widget_get_first_child (widget); child;
	     child = gtk_widget_get_next_sibling (child))
	{
		m = mg_widget_detent_min (child);
		if (m > max_min)
			max_min = m;
	}

	return max_min;
}

/* Magnetic detent on pane shrink. child is the paned child that shrinks
 * as `position` approaches `threshold_side` (START / END). We compute
 * the child's effective minimum width (from widget-declared hints, or
 * fall back to gtk_widget_measure) and, when the cursor has driven the
 * paned within the sticky window on the shrinking side of that minimum,
 * clamp the position back to hold the child at its minimum. */
static void
mg_pane_apply_detent (GtkPaned *pane, GtkWidget *shrinking_child,
                      gboolean shrinking_side_is_end, GCallback handler, gpointer data)
{
	int pane_w, pos, child_min, threshold;

	if (!shrinking_child)
		return;

	pane_w = gtk_widget_get_width (GTK_WIDGET (pane));
	if (pane_w <= 0)
		return;

	child_min = mg_widget_detent_min (shrinking_child);
	if (child_min > 0)
	{
		/* Declared hints report content-only widths. Add the shrinking
		 * child's own margins since the paned allocates them on top. */
		child_min += gtk_widget_get_margin_start (shrinking_child)
		           + gtk_widget_get_margin_end (shrinking_child);
	}
	else
	{
		/* No widget-declared hint found; fall back to measured natural
		 * min. gtk_widget_measure already includes the widget's margins. */
		gtk_widget_measure (shrinking_child, GTK_ORIENTATION_HORIZONTAL, -1,
		                    &child_min, NULL, NULL, NULL);
		if (child_min <= 0)
			return;
	}

	pos = gtk_paned_get_position (pane);

	if (shrinking_side_is_end)
	{
		/* End child shrinks as position grows. At snap we want the end
		 * child allocated child_min pixels; GTK4 gives the end child
		 * `pane_w - position - handle_w`, so subtract the handle here
		 * or the end child ends up handle_w pixels narrower than the
		 * symmetric left case (which allocates `position` directly). */
		threshold = pane_w - child_min - MG_PANED_HANDLE_W;
		if (threshold <= 0)
			return;
		if (pos > threshold && pos <= threshold + MG_PANE_DETENT_STICKY_PX)
		{
			g_signal_handlers_block_by_func (pane, handler, data);
			gtk_paned_set_position (pane, threshold);
			g_signal_handlers_unblock_by_func (pane, handler, data);
		}
	}
	else
	{
		/* Start child shrinks as position drops. Threshold = child_min. */
		threshold = child_min;
		if (pos < threshold && pos >= threshold - MG_PANE_DETENT_STICKY_PX)
		{
			g_signal_handlers_block_by_func (pane, handler, data);
			gtk_paned_set_position (pane, threshold);
			g_signal_handlers_unblock_by_func (pane, handler, data);
		}
	}
}

static void
mg_rightpane_cb (GtkPaned *pane, GParamSpec *param, session_gui *gui)
{
	mg_pane_apply_detent (pane, gtk_paned_get_end_child (pane), TRUE,
	                      G_CALLBACK (mg_rightpane_cb), gui);
	/* GTK4: Defer the size calculation to an idle callback because
	 * gtk_widget_get_width() returns invalid values during notify::position. */
	g_idle_add (mg_rightpane_idle_cb, gui);
}

static gboolean
mg_add_pane_signals (session_gui *gui)
{
	g_signal_connect (G_OBJECT (gui->hpane_right), "notify::position",
							G_CALLBACK (mg_rightpane_cb), gui);
	g_signal_connect (G_OBJECT (gui->hpane_left), "notify::position",
							G_CALLBACK (mg_leftpane_cb), gui);
	g_signal_connect (G_OBJECT (gui->vpane_left), "notify::position",
							G_CALLBACK (mg_vpane_cb), gui);
	g_signal_connect (G_OBJECT (gui->vpane_right), "notify::position",
							G_CALLBACK (mg_vpane_cb), gui);
	return FALSE;
}

static void
mg_paned_update_collapsed (GtkPaned *paned)
{
	int pos = gtk_paned_get_position (paned);
	int max = 0;
	gboolean collapsed;
	g_object_get (paned, "max-position", &max, NULL);
	collapsed = (max > 0) && (pos <= 0 || pos >= max);
	if (collapsed)
		gtk_widget_add_css_class (GTK_WIDGET (paned), "collapsed");
	else
		gtk_widget_remove_css_class (GTK_WIDGET (paned), "collapsed");
}

static void
mg_paned_notify_position_cb (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
	mg_paned_update_collapsed (GTK_PANED (obj));
}

static void
mg_paned_track_collapse (GtkPaned *paned)
{
	g_signal_connect (paned, "notify::position", G_CALLBACK (mg_paned_notify_position_cb), NULL);
	g_signal_connect (paned, "notify::max-position", G_CALLBACK (mg_paned_notify_position_cb), NULL);
}

static void
mg_create_center (session *sess, session_gui *gui, GtkWidget *box)
{
	GtkWidget *vbox, *hbox, *book;

	/* sep between top and bottom of left side */
	gui->vpane_left = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
	/* GTK4: Hide empty paned widgets initially to prevent crashes during
	 * size computation. They will be shown by mg_hide_empty_pane when
	 * children are added by mg_place_userlist_and_chanview. */
	gtk_widget_set_visible (gui->vpane_left, FALSE);
	gui->vpane_left_wrap = mg_setup_pane_layout_dnd (gui->vpane_left, TRUE);

	/* sep between top and bottom of right side */
	gui->vpane_right = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
	gtk_widget_set_visible (gui->vpane_right, FALSE);
	gui->vpane_right_wrap = mg_setup_pane_layout_dnd (gui->vpane_right, FALSE);

	/* sep between left and xtext */
	gui->hpane_left = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_set_position (GTK_PANED (gui->hpane_left), prefs.hex_gui_pane_left_size);

	/* sep between xtext and right side — wide handle so the separator
	 * gets its own allocated space instead of overlaying the scrollbar. */
	gui->hpane_right = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);

	/* Track pane collapse so the .collapsed CSS class can zero the handle
	 * margin when a child hits 0 size. */
	mg_paned_track_collapse (GTK_PANED (gui->vpane_left));
	mg_paned_track_collapse (GTK_PANED (gui->vpane_right));
	mg_paned_track_collapse (GTK_PANED (gui->hpane_left));
	mg_paned_track_collapse (GTK_PANED (gui->hpane_right));

	if (prefs.hex_gui_win_swap)
	{
		gtk_paned_set_end_child (GTK_PANED (gui->hpane_left), gui->vpane_left_wrap);
		gtk_paned_set_resize_end_child (GTK_PANED (gui->hpane_left), FALSE);
		gtk_paned_set_shrink_end_child (GTK_PANED (gui->hpane_left), TRUE);
		gtk_paned_set_start_child (GTK_PANED (gui->hpane_left), gui->hpane_right);
		gtk_paned_set_resize_start_child (GTK_PANED (gui->hpane_left), TRUE);
		gtk_paned_set_shrink_start_child (GTK_PANED (gui->hpane_left), TRUE);
		/* Window-edge margin on the wrap so the drop-indicator strips
		 * are offset with the vpane. */
		gtk_widget_set_margin_start (gui->vpane_left_wrap, GUI_PANE_MARGIN);
	}
	else
	{
		gtk_paned_set_start_child (GTK_PANED (gui->hpane_left), gui->vpane_left_wrap);
		gtk_paned_set_resize_start_child (GTK_PANED (gui->hpane_left), FALSE);
		gtk_paned_set_shrink_start_child (GTK_PANED (gui->hpane_left), TRUE);
		gtk_paned_set_end_child (GTK_PANED (gui->hpane_left), gui->hpane_right);
		gtk_paned_set_resize_end_child (GTK_PANED (gui->hpane_left), TRUE);
		gtk_paned_set_shrink_end_child (GTK_PANED (gui->hpane_left), TRUE);
		gtk_widget_set_margin_start (gui->vpane_left_wrap, GUI_PANE_MARGIN);
	}
	/* shrink=TRUE allows userlist panel to shrink below its natural minimum */
	gtk_paned_set_end_child (GTK_PANED (gui->hpane_right), gui->vpane_right_wrap);
	gtk_paned_set_resize_end_child (GTK_PANED (gui->hpane_right), FALSE);
	gtk_paned_set_shrink_end_child (GTK_PANED (gui->hpane_right), TRUE);

	/* Window-edge margin on the wrap so the drop-indicator strips
	 * are offset with the vpane. */
	gtk_widget_set_margin_end (gui->vpane_right_wrap, GUI_PANE_MARGIN);

	/* GTK3: Ensure main paned fills its container for proper anchoring */
	gtk_widget_set_halign (gui->hpane_left, GTK_ALIGN_FILL);
	gtk_widget_set_valign (gui->hpane_left, GTK_ALIGN_FILL);
	gtk_widget_set_hexpand (gui->hpane_left, TRUE);
	gtk_widget_set_vexpand (gui->hpane_left, TRUE);
	gtk_box_append (GTK_BOX (box), gui->hpane_left);

	gui->note_book = book = hc_page_container_new ();
	/* GTK4: Ensure page container fills its container and content is anchored at top.
	 * Let child minimum sizes propagate naturally (no size_request override). */
	gtk_widget_set_halign (book, GTK_ALIGN_FILL);
	gtk_widget_set_valign (book, GTK_ALIGN_FILL);
	gtk_widget_set_hexpand (book, TRUE);
	gtk_widget_set_vexpand (book, TRUE);
	gtk_widget_set_margin_start (book, 0);
	gtk_widget_set_margin_end (book, 0);
	gtk_paned_set_start_child (GTK_PANED (gui->hpane_right), book);
	gtk_paned_set_resize_start_child (GTK_PANED (gui->hpane_right), TRUE);
	gtk_paned_set_shrink_start_child (GTK_PANED (gui->hpane_right), TRUE);

	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	/* Allow userlist panel to clip content when shrunk below natural size */
	gtk_widget_set_overflow (hbox, GTK_OVERFLOW_HIDDEN);
	mg_create_userlist (gui, hbox);

	gui->user_box = hbox;

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	/* GTK4: Ensure vbox fills page container and is anchored at top-left */
	gtk_widget_set_halign (vbox, GTK_ALIGN_FILL);
	gtk_widget_set_valign (vbox, GTK_ALIGN_FILL);
	gtk_widget_set_hexpand (vbox, TRUE);
	gtk_widget_set_vexpand (vbox, TRUE);
	hc_page_container_append (book, vbox);
	mg_create_topicbar (sess, vbox);

	if (prefs.hex_gui_search_pos)
	{
		mg_create_search (sess, vbox);
		mg_create_textarea (sess, vbox);
	}
	else
	{
		mg_create_textarea (sess, vbox);
		mg_create_search (sess, vbox);
	}

	mg_create_entry (sess, vbox);

	mg_add_pane_signals (gui);
}

static void
mg_change_nick (int cancel, char *text, gpointer userdata)
{
	char buf[256];

	if (!cancel)
	{
		g_snprintf (buf, sizeof (buf), "nick %s", text);
		handle_command (current_sess, buf, FALSE);
	}
}

static void
mg_nick_dialog_destroyed (GtkWidget *w, GtkWidget *button)
{
	gtk_widget_set_sensitive (button, TRUE);
}

static void
mg_nickclick_cb (GtkWidget *button, gpointer userdata)
{
	GtkWidget *dialog;

	gtk_widget_set_sensitive (button, FALSE);
	dialog = fe_get_str (_("Enter new nickname:"), current_sess->server->nick,
					mg_change_nick, (void *) 1);
	if (dialog)
		g_signal_connect (dialog, "destroy",
						  G_CALLBACK (mg_nick_dialog_destroyed), button);
	else
		gtk_widget_set_sensitive (button, TRUE);
}

/* make sure chanview and userlist positions are sane */

static void
mg_sanitize_positions (int *cv, int *ul)
{
	if (prefs.hex_gui_tab_layout == 2)
	{
		/* treeview can't be on TOP or BOTTOM */
		if (*cv == POS_TOP || *cv == POS_BOTTOM)
			*cv = POS_TOPLEFT;
	}

	/* userlist can't be on TOP or BOTTOM */
	if (*ul == POS_TOP || *ul == POS_BOTTOM)
		*ul = POS_TOPRIGHT;

	/* can't have both in the same place */
	if (*cv == *ul)
	{
		*cv = POS_TOPRIGHT;
		if (*ul == POS_TOPRIGHT)
			*cv = POS_BOTTOMRIGHT;
	}
}

static void
mg_place_userlist_and_chanview_real (session_gui *gui, GtkWidget *userlist, GtkWidget *chanview)
{
	int unref_userlist = FALSE;
	int unref_chanview = FALSE;
	gboolean ulist_visible = !prefs.hex_gui_ulist_hide;
	gboolean cv_left = (prefs.hex_gui_tab_pos == POS_TOPLEFT || prefs.hex_gui_tab_pos == POS_BOTTOMLEFT);
	gboolean cv_right = (prefs.hex_gui_tab_pos == POS_TOPRIGHT || prefs.hex_gui_tab_pos == POS_BOTTOMRIGHT);
	gboolean ul_left = (prefs.hex_gui_ulist_pos == POS_TOPLEFT || prefs.hex_gui_ulist_pos == POS_BOTTOMLEFT);
	gboolean ul_right = (prefs.hex_gui_ulist_pos == POS_TOPRIGHT || prefs.hex_gui_ulist_pos == POS_BOTTOMRIGHT);
	gboolean shares_left = ulist_visible && cv_left && ul_left;
	gboolean shares_right = ulist_visible && cv_right && ul_right;

	/* first, remove userlist/treeview from their containers */
	if (userlist && gtk_widget_get_parent (userlist))
	{
		g_object_ref (userlist);
		hc_container_remove (gtk_widget_get_parent (userlist), userlist);
		unref_userlist = TRUE;
	}

	if (chanview && gtk_widget_get_parent (chanview))
	{
		g_object_ref (chanview);
		hc_container_remove (gtk_widget_get_parent (chanview), chanview);
		unref_chanview = TRUE;
	}

	if (chanview)
	{
		/* incase the previous pos was POS_HIDDEN */
		gtk_widget_set_visible (chanview, TRUE);

		/* reset margins that may have been set by previous position */
		gtk_widget_set_margin_top (chanview, 0);
		gtk_widget_set_margin_bottom (chanview, 0);

		/* then place them back in their new positions */
		/* shrink=TRUE matches original GTK2 behavior */
		switch (prefs.hex_gui_tab_pos)
		{
		/* 6px on outer sides; handle-adjacent side is 0 when sharing a vpane
		 * (handle margin supplies the 12px junction). When alone in the
		 * vpane, both sides get 6. */
		case POS_TOPLEFT:
			gtk_widget_set_vexpand (chanview, FALSE);
			gtk_widget_set_margin_top (chanview, GUI_PANE_MARGIN);
			gtk_widget_set_margin_bottom (chanview, shares_left ? 0 : GUI_PANE_MARGIN);
			gtk_paned_set_start_child (GTK_PANED (gui->vpane_left), chanview);
			gtk_paned_set_resize_start_child (GTK_PANED (gui->vpane_left), FALSE);
			gtk_paned_set_shrink_start_child (GTK_PANED (gui->vpane_left), TRUE);
			break;
		case POS_BOTTOMLEFT:
			gtk_widget_set_vexpand (chanview, FALSE);
			gtk_widget_set_margin_top (chanview, shares_left ? 0 : GUI_PANE_MARGIN);
			gtk_widget_set_margin_bottom (chanview, GUI_PANE_MARGIN);
			gtk_paned_set_end_child (GTK_PANED (gui->vpane_left), chanview);
			gtk_paned_set_resize_end_child (GTK_PANED (gui->vpane_left), FALSE);
			gtk_paned_set_shrink_end_child (GTK_PANED (gui->vpane_left), TRUE);
			break;
		case POS_TOPRIGHT:
			gtk_widget_set_vexpand (chanview, FALSE);
			gtk_widget_set_margin_top (chanview, GUI_PANE_MARGIN);
			gtk_widget_set_margin_bottom (chanview, shares_right ? 0 : GUI_PANE_MARGIN);
			gtk_paned_set_start_child (GTK_PANED (gui->vpane_right), chanview);
			gtk_paned_set_resize_start_child (GTK_PANED (gui->vpane_right), FALSE);
			gtk_paned_set_shrink_start_child (GTK_PANED (gui->vpane_right), TRUE);
			break;
		case POS_BOTTOMRIGHT:
			gtk_widget_set_vexpand (chanview, FALSE);
			gtk_widget_set_margin_top (chanview, shares_right ? 0 : GUI_PANE_MARGIN);
			gtk_widget_set_margin_bottom (chanview, GUI_PANE_MARGIN);
			gtk_paned_set_end_child (GTK_PANED (gui->vpane_right), chanview);
			gtk_paned_set_resize_end_child (GTK_PANED (gui->vpane_right), FALSE);
			gtk_paned_set_shrink_end_child (GTK_PANED (gui->vpane_right), TRUE);
			break;
		case POS_TOP:
			/* topic_bar's margin_top=6 supplies the gap below */
			gtk_widget_set_margin_bottom (chanview, 0);
			gtk_widget_set_hexpand (chanview, TRUE);
			gtk_widget_set_vexpand (chanview, FALSE);
			gtk_grid_attach (GTK_GRID (gui->main_table), chanview, 1, 1, 1, 1);
			break;
		case POS_HIDDEN:
			gtk_widget_set_visible (chanview, FALSE);
			gtk_widget_set_hexpand (chanview, TRUE);
			gtk_widget_set_vexpand (chanview, FALSE);
			/* always attach it to something to avoid ref_count=0 */
			if (prefs.hex_gui_ulist_pos == POS_TOP)
				gtk_grid_attach (GTK_GRID (gui->main_table), chanview, 1, 3, 1, 1);
			else
				gtk_grid_attach (GTK_GRID (gui->main_table), chanview, 1, 1, 1, 1);
			break;
		default:/* POS_BOTTOM */
			/* input's margin_bottom=6 supplies the gap above */
			gtk_widget_set_margin_top (chanview, 0);
			gtk_widget_set_hexpand (chanview, TRUE);
			gtk_widget_set_vexpand (chanview, FALSE);
			gtk_grid_attach (GTK_GRID (gui->main_table), chanview, 1, 3, 1, 1);
		}
	}

	if (userlist)
	{
		/* reset margins that may have been set by previous position */
		gtk_widget_set_margin_top (userlist, 0);
		gtk_widget_set_margin_bottom (userlist, 0);

		/* shrink=TRUE allows userlist to shrink below natural minimum.
		 * Handle-adjacent side is 0 when sharing a vpane; handle margin
		 * supplies the junction gap. */
		switch (prefs.hex_gui_ulist_pos)
		{
		case POS_TOPLEFT:
			gtk_widget_set_margin_top (gui->button_box_parent, GUI_PANE_MARGIN);
			gtk_widget_set_margin_bottom (gui->button_box_parent, shares_left ? 0 : GUI_PANE_MARGIN);
			gtk_paned_set_start_child (GTK_PANED (gui->vpane_left), userlist);
			gtk_paned_set_resize_start_child (GTK_PANED (gui->vpane_left), FALSE);
			gtk_paned_set_shrink_start_child (GTK_PANED (gui->vpane_left), TRUE);
			break;
		case POS_BOTTOMLEFT:
			gtk_widget_set_margin_top (gui->button_box_parent, shares_left ? 0 : GUI_PANE_MARGIN);
			gtk_widget_set_margin_bottom (gui->button_box_parent, GUI_PANE_MARGIN);
			gtk_paned_set_end_child (GTK_PANED (gui->vpane_left), userlist);
			gtk_paned_set_resize_end_child (GTK_PANED (gui->vpane_left), FALSE);
			gtk_paned_set_shrink_end_child (GTK_PANED (gui->vpane_left), TRUE);
			break;
		case POS_BOTTOMRIGHT:
			gtk_widget_set_margin_top (gui->button_box_parent, shares_right ? 0 : GUI_PANE_MARGIN);
			gtk_widget_set_margin_bottom (gui->button_box_parent, GUI_PANE_MARGIN);
			gtk_paned_set_end_child (GTK_PANED (gui->vpane_right), userlist);
			gtk_paned_set_resize_end_child (GTK_PANED (gui->vpane_right), FALSE);
			gtk_paned_set_shrink_end_child (GTK_PANED (gui->vpane_right), TRUE);
			break;
		/*case POS_HIDDEN:
			break;*/	/* Hide using the VIEW menu instead */
		default:/* POS_TOPRIGHT */
			gtk_widget_set_margin_top (gui->button_box_parent, GUI_PANE_MARGIN);
			gtk_widget_set_margin_bottom (gui->button_box_parent, shares_right ? 0 : GUI_PANE_MARGIN);
			gtk_paned_set_start_child (GTK_PANED (gui->vpane_right), userlist);
			gtk_paned_set_resize_start_child (GTK_PANED (gui->vpane_right), FALSE);
			gtk_paned_set_shrink_start_child (GTK_PANED (gui->vpane_right), TRUE);
		}
	}

	/* Restore vertical pane divider positions (deferred until window is mapped) */
	if (prefs.hex_gui_pane_divider_position != 0)
	{
		g_idle_add (mg_restore_vpane_position, gui);
	}

	/* Restore horizontal right pane position if chanview is on the right */
	if (prefs.hex_gui_tab_pos == POS_TOPRIGHT || prefs.hex_gui_tab_pos == POS_BOTTOMRIGHT)
	{
		g_idle_add (mg_restore_right_pane_position, gui);
	}

	if (unref_chanview)
		g_object_unref (chanview);
	if (unref_userlist)
		g_object_unref (userlist);

	mg_hide_empty_boxes (gui);
	mg_update_window_minimum (gui);

	/* Layout rebuilds don't move the pane separators, so the notify::position
	 * chain that normally drives collapse-threshold updates won't fire. Run
	 * the update explicitly against the now-settled pane sizes. */
	g_idle_add (mg_layout_update_idle_cb, gui);
}

static void
mg_place_userlist_and_chanview (session_gui *gui)
{
	GtkOrientation orientation;
	GtkWidget *chanviewbox = NULL;
	int pos;
	int tab_pos, ulist_pos;
	gboolean left_has_content, right_has_content;

	mg_sanitize_positions (&prefs.hex_gui_tab_pos, &prefs.hex_gui_ulist_pos);

	tab_pos = prefs.hex_gui_tab_pos;
	ulist_pos = prefs.hex_gui_ulist_pos;

	gtk_widget_set_margin_bottom (gui->main_table, 0);

	/* Window-edge margin is supplied by vpane_left/right when they are
	 * present; main_table fills in on sides without a pane. */
	left_has_content = (tab_pos == POS_TOPLEFT || tab_pos == POS_BOTTOMLEFT);
	right_has_content = (tab_pos == POS_TOPRIGHT || tab_pos == POS_BOTTOMRIGHT);

	if (!prefs.hex_gui_ulist_hide)
	{
		if (ulist_pos == POS_TOPLEFT || ulist_pos == POS_BOTTOMLEFT)
			left_has_content = TRUE;
		if (ulist_pos == POS_TOPRIGHT || ulist_pos == POS_BOTTOMRIGHT)
			right_has_content = TRUE;
	}

	gtk_widget_set_margin_start (gui->main_table, left_has_content ? 0 : GUI_PANE_MARGIN);
	gtk_widget_set_margin_end (gui->main_table, right_has_content ? 0 : GUI_PANE_MARGIN);

	/* App menubar is rendered by GtkApplicationWindow and spans the
	 * window width; no per-side margin compensation needed here. */

	if (gui->chanview)
	{
		pos = prefs.hex_gui_tab_pos;

		orientation = chanview_get_orientation (gui->chanview);
		if ((pos == POS_BOTTOM || pos == POS_TOP) && orientation == GTK_ORIENTATION_VERTICAL)
			chanview_set_orientation (gui->chanview, FALSE);
		else if ((pos == POS_TOPLEFT || pos == POS_BOTTOMLEFT || pos == POS_TOPRIGHT || pos == POS_BOTTOMRIGHT) && orientation == GTK_ORIENTATION_HORIZONTAL)
			chanview_set_orientation (gui->chanview, TRUE);
		chanviewbox = chanview_get_box (gui->chanview);
	}

	mg_place_userlist_and_chanview_real (gui, gui->user_box, chanviewbox);
}

void
mg_change_layout (int type)
{
	if (mg_gui)
	{
		mg_place_userlist_and_chanview (mg_gui);
		chanview_set_impl (mg_gui->chanview, type);
	}
}

/* Search bar adapted from Conspire's by William Pitcock */

#define SEARCH_CHANGE		1
#define SEARCH_NEXT			2
#define SEARCH_PREVIOUS		3
#define SEARCH_REFRESH		4

static void
search_handle_event(int search_type, session *sess)
{
	textentry *last;
	const gchar *text = NULL;
	gtk_xtext_search_flags flags;
	GError *err = NULL;
	gboolean backwards = FALSE;

	/* When just typing show most recent first */
	if (search_type == SEARCH_PREVIOUS || search_type == SEARCH_CHANGE)
		backwards = TRUE;

	flags = ((prefs.hex_text_search_case_match == 1? case_match: 0) |
				(backwards? backward: 0) |
				(prefs.hex_text_search_highlight_all == 1? highlight: 0) |
				(prefs.hex_text_search_follow == 1? follow: 0) |
				(prefs.hex_text_search_regexp == 1? regexp: 0));

	if (search_type != SEARCH_REFRESH)
		text = hc_entry_get_text (sess->gui->shentry);
	last = gtk_xtext_search (GTK_XTEXT (sess->gui->xtext), text, flags, &err);

	if (err)
	{
		gtk_entry_set_icon_from_icon_name (GTK_ENTRY (sess->gui->shentry), GTK_ENTRY_ICON_SECONDARY, "dialog-error");
		gtk_entry_set_icon_tooltip_text (GTK_ENTRY (sess->gui->shentry), GTK_ENTRY_ICON_SECONDARY, _(err->message));
		g_error_free (err);
	}
	else if (!last)
	{
		if (text && text[0] == 0) /* empty string, no error */
		{
			gtk_entry_set_icon_from_icon_name (GTK_ENTRY (sess->gui->shentry), GTK_ENTRY_ICON_SECONDARY, NULL);
		}
		else
		{
			/* Either end of search or not found, try again to wrap if only end */
			last = gtk_xtext_search (GTK_XTEXT (sess->gui->xtext), text, flags, &err);
			if (!last) /* Not found error */
			{
				gtk_entry_set_icon_from_icon_name (GTK_ENTRY (sess->gui->shentry), GTK_ENTRY_ICON_SECONDARY, "dialog-error");
				gtk_entry_set_icon_tooltip_text (GTK_ENTRY (sess->gui->shentry), GTK_ENTRY_ICON_SECONDARY, _("No results found."));
			}
		}
	}
	else
	{
		gtk_entry_set_icon_from_icon_name (GTK_ENTRY (sess->gui->shentry), GTK_ENTRY_ICON_SECONDARY, NULL);
	}
}

static void
search_handle_change(GtkWidget *wid, session *sess)
{
	search_handle_event(SEARCH_CHANGE, sess);
}

static void
search_handle_refresh(GtkWidget *wid, session *sess)
{
	search_handle_event(SEARCH_REFRESH, sess);
}

void
mg_search_handle_previous(GtkWidget *wid, session *sess)
{
	search_handle_event(SEARCH_PREVIOUS, sess);
}

void
mg_search_handle_next(GtkWidget *wid, session *sess)
{
	search_handle_event(SEARCH_NEXT, sess);
}

static void
search_set_option (GtkCheckButton *but, guint *pref)
{
	*pref = gtk_check_button_get_active(but);
	save_config();
}

void
mg_search_toggle(session *sess)
{
	if (gtk_widget_get_visible(sess->gui->shbox))
	{
		gtk_widget_set_visible(sess->gui->shbox, FALSE);
		gtk_widget_grab_focus(sess->gui->input_box);
		hc_entry_set_text(sess->gui->shentry, "");
	}
	else
	{
		/* Reset search state */
		gtk_entry_set_icon_from_icon_name (GTK_ENTRY (sess->gui->shentry), GTK_ENTRY_ICON_SECONDARY, NULL);

		/* Show and focus */
		gtk_widget_set_visible(sess->gui->shbox, TRUE);
		gtk_widget_grab_focus(sess->gui->shentry);
	}
}

static gboolean
search_handle_esc (GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, session *sess)
{
	(void)controller; (void)keycode; (void)state;
	if (keyval == GDK_KEY_Escape)
		mg_search_toggle(sess);
	return FALSE;
}

static void
mg_create_search(session *sess, GtkWidget *box)
{
	GtkWidget *entry, *label, *next, *previous, *highlight, *matchcase, *regex, *close;
	session_gui *gui = sess->gui;

	gui->shbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 5);
	gtk_widget_set_margin_bottom (gui->shbox, GUI_PANE_MARGIN);
	gtk_box_append (GTK_BOX (box), gui->shbox);

	close = gtk_button_new ();
	gtk_button_set_child (GTK_BUTTON (close), hc_image_new_from_icon_name ("window-close", GTK_ICON_SIZE_MENU));
	gtk_widget_add_css_class(close, "flat");
	gtk_widget_set_can_focus (close, FALSE);
	gtk_box_append (GTK_BOX (gui->shbox), close);
	g_signal_connect_swapped(G_OBJECT(close), "clicked", G_CALLBACK(mg_search_toggle), sess);

	label = gtk_label_new(_("Find:"));
	gtk_box_append (GTK_BOX (gui->shbox), label);

	gui->shentry = entry = gtk_entry_new();
	gtk_box_append (GTK_BOX (gui->shbox), entry);
	gtk_widget_set_size_request (gui->shentry, 180, -1);
	gui->search_changed_signal = g_signal_connect(G_OBJECT(entry), "changed", G_CALLBACK(search_handle_change), sess);
	{
		GtkEventController *key_controller = gtk_event_controller_key_new ();
		g_signal_connect (key_controller, "key-pressed", G_CALLBACK (search_handle_esc), sess);
		gtk_widget_add_controller (entry, key_controller);
	}
	g_signal_connect(G_OBJECT(entry), "activate", G_CALLBACK(mg_search_handle_next), sess);
	gtk_entry_set_icon_activatable (GTK_ENTRY (entry), GTK_ENTRY_ICON_SECONDARY, FALSE);
	gtk_entry_set_icon_tooltip_text (GTK_ENTRY (sess->gui->shentry), GTK_ENTRY_ICON_SECONDARY, _("Search hit end or not found."));

	previous = gtk_button_new ();
	gtk_button_set_child (GTK_BUTTON (previous), hc_image_new_from_icon_name ("go-previous", GTK_ICON_SIZE_MENU));
	gtk_widget_add_css_class(previous, "flat");
	gtk_widget_set_can_focus (previous, FALSE);
	gtk_box_append (GTK_BOX (gui->shbox), previous);
	g_signal_connect(G_OBJECT(previous), "clicked", G_CALLBACK(mg_search_handle_previous), sess);

	next = gtk_button_new ();
	gtk_button_set_child (GTK_BUTTON (next), hc_image_new_from_icon_name ("go-next", GTK_ICON_SIZE_MENU));
	gtk_widget_add_css_class(next, "flat");
	gtk_widget_set_can_focus (next, FALSE);
	gtk_box_append (GTK_BOX (gui->shbox), next);
	g_signal_connect(G_OBJECT(next), "clicked", G_CALLBACK(mg_search_handle_next), sess);

	highlight = gtk_check_button_new_with_mnemonic (_("_Highlight all"));
	gtk_check_button_set_active (GTK_CHECK_BUTTON (highlight), prefs.hex_text_search_highlight_all);
	gtk_widget_set_can_focus (highlight, FALSE);
	g_signal_connect (G_OBJECT (highlight), "toggled", G_CALLBACK (search_set_option), &prefs.hex_text_search_highlight_all);
	g_signal_connect (G_OBJECT (highlight), "toggled", G_CALLBACK (search_handle_refresh), sess);
	gtk_box_append (GTK_BOX (gui->shbox), highlight);
	gtk_widget_set_tooltip_text (highlight, _("Highlight all occurrences, and underline the current occurrence."));

	matchcase = gtk_check_button_new_with_mnemonic (_("Mat_ch case"));
	gtk_check_button_set_active (GTK_CHECK_BUTTON (matchcase), prefs.hex_text_search_case_match);
	gtk_widget_set_can_focus (matchcase, FALSE);
	g_signal_connect (G_OBJECT (matchcase), "toggled", G_CALLBACK (search_set_option), &prefs.hex_text_search_case_match);
	gtk_box_append (GTK_BOX (gui->shbox), matchcase);
	gtk_widget_set_tooltip_text (matchcase, _("Perform a case-sensitive search."));

	regex = gtk_check_button_new_with_mnemonic (_("_Regex"));
	gtk_check_button_set_active (GTK_CHECK_BUTTON (regex), prefs.hex_text_search_regexp);
	gtk_widget_set_can_focus (regex, FALSE);
	g_signal_connect (G_OBJECT (regex), "toggled", G_CALLBACK (search_set_option), &prefs.hex_text_search_regexp);
	gtk_box_append (GTK_BOX (gui->shbox), regex);
	gtk_widget_set_tooltip_text (regex, _("Regard search string as a regular expression."));
}

static void
mg_emoji_picked (HexEmojiChooser *chooser, const char *text, session_gui *gui)
{
	int pos;
	(void) chooser;

	if (!gui || !gui->input_box || !text || !*text)
		return;

	pos = SPELL_ENTRY_GET_POS (gui->input_box);
	SPELL_ENTRY_INSERT (gui->input_box, text, strlen (text), &pos);
	SPELL_ENTRY_SET_POS (gui->input_box, pos);
}

static void
reply_bar_close_cb (GtkButton *button, gpointer user_data)
{
	session *sess = current_sess;

	mg_clear_reply_state (sess);
	mg_clear_react_state (sess);
	fe_reply_state_changed (sess);

	/* Return focus to input box */
	if (sess->gui && sess->gui->input_box)
		gtk_widget_grab_focus (sess->gui->input_box);
}

static void
mg_create_entry (session *sess, GtkWidget *box)
{
	GtkWidget *hbox, *but, *entry;
	session_gui *gui = sess->gui;

	/* IRCv3 reply bar: "Replying to <nick>" indicator above input.
	 * Hidden by default, shown when user activates Reply from context menu. */
	{
		GtkWidget *reply_hbox, *reply_label, *reply_close;

		reply_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
		gtk_widget_add_css_class (reply_hbox, "reply-bar");

		reply_label = gtk_label_new ("");
		gtk_label_set_ellipsize (GTK_LABEL (reply_label), PANGO_ELLIPSIZE_END);
		gtk_widget_set_hexpand (reply_label, TRUE);
		gtk_label_set_xalign (GTK_LABEL (reply_label), 0.0);
		gtk_box_append (GTK_BOX (reply_hbox), reply_label);

		reply_close = gtk_button_new_from_icon_name ("window-close-symbolic");
		gtk_button_set_has_frame (GTK_BUTTON (reply_close), FALSE);
		gtk_widget_set_can_focus (reply_close, FALSE);
		gtk_widget_set_tooltip_text (reply_close, _("Cancel reply"));
		gtk_box_append (GTK_BOX (reply_hbox), reply_close);

		g_object_set_data (G_OBJECT (reply_hbox), "reply-label", reply_label);
		g_object_set_data (G_OBJECT (reply_hbox), "reply-close", reply_close);
		g_signal_connect (reply_close, "clicked", G_CALLBACK (reply_bar_close_cb), NULL);
		gui->reply_bar = reply_hbox;

		gtk_widget_set_visible (reply_hbox, FALSE);
		gtk_box_append (GTK_BOX (box), reply_hbox);
	}

	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, GUI_PANE_MARGIN);
	gtk_widget_set_margin_bottom (hbox, GUI_PANE_MARGIN);
	gtk_box_append (GTK_BOX (box), hbox);

	gui->nick_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append (GTK_BOX (hbox), gui->nick_box);

	gui->nick_label = but = gtk_button_new ();
	gtk_widget_add_css_class (but, "flat");
	gtk_widget_set_name (but, "poxchat-nickbutton");
	gtk_widget_set_can_focus (but, FALSE);
	{
		GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
		GtkWidget *label = gtk_label_new (sess->server->nick);
		gtk_box_append (GTK_BOX (btn_box), label);
		gtk_button_set_child (GTK_BUTTON (but), btn_box);
		g_object_set_data (G_OBJECT (but), "nick-label", label);
	}
	gtk_box_append (GTK_BOX (gui->nick_box), but);
	g_signal_connect (G_OBJECT (but), "clicked",
							G_CALLBACK (mg_nickclick_cb), NULL);

	gui->input_box = entry = hex_input_edit_new ();
	hex_input_edit_set_max_lines (HEX_INPUT_EDIT (entry), prefs.hex_gui_input_lines);
	if (input_style && input_style->font_desc)
		hex_input_edit_set_font (HEX_INPUT_EDIT (entry), input_style->font_desc);

	g_signal_connect (G_OBJECT (entry), "activate",
							G_CALLBACK (mg_inputbox_cb), gui);

	/* Add directly — measure vfunc handles max height clamping */
	gtk_widget_set_hexpand (entry, TRUE);
	gtk_widget_set_valign (entry, GTK_ALIGN_FILL);
	gtk_box_append (GTK_BOX (hbox), entry);

	gtk_widget_set_name (entry, "poxchat-inputbox");
	{
		GtkEventController *key_controller = gtk_event_controller_key_new ();
		/* Use capture phase to handle keys before focus navigation */
		gtk_event_controller_set_propagation_phase (key_controller, GTK_PHASE_CAPTURE);
		g_signal_connect (key_controller, "key-pressed",
								G_CALLBACK (key_handle_key_press), NULL);
		gtk_widget_add_controller (entry, key_controller);
	}
	{
		GtkEventController *focus_controller = gtk_event_controller_focus_new ();
		g_signal_connect (focus_controller, "enter",
								G_CALLBACK (mg_inputbox_focus), gui);
		gtk_widget_add_controller (entry, focus_controller);
	}

	/* Share xtext's emoji cache and palette with the input box */
	if (gui->xtext && GTK_XTEXT (gui->xtext)->emoji_cache)
		hex_input_edit_set_emoji_cache (HEX_INPUT_EDIT (entry),
		                                GTK_XTEXT (gui->xtext)->emoji_cache);
	if (gui->xtext)
		hex_input_edit_set_palette (HEX_INPUT_EDIT (entry),
		                            GTK_XTEXT (gui->xtext)->palette);

	/* Emoji picker button — GtkMenuButton owns the popover lifecycle */
	{
		GtkWidget *emoji_btn, *emoji_chooser;

		emoji_btn = gtk_menu_button_new ();
		gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (emoji_btn), "face-smile-symbolic");
		gtk_menu_button_set_has_frame (GTK_MENU_BUTTON (emoji_btn), FALSE);
		/* Leave can-focus at its default (TRUE). Setting it FALSE here
		 * breaks the popover's search entry: autohide popovers in GTK4
		 * can't transfer keyboard focus to their content when the
		 * trigger widget isn't focusable. */
		gtk_widget_set_tooltip_text (emoji_btn, _("Insert Emoji"));
		gtk_widget_set_valign (emoji_btn, GTK_ALIGN_FILL);
		gtk_widget_set_name (emoji_btn, "poxchat-emojibtn");
		gtk_box_append (GTK_BOX (hbox), emoji_btn);

		emoji_chooser = hex_emoji_chooser_new ();
		gtk_menu_button_set_popover (GTK_MENU_BUTTON (emoji_btn), emoji_chooser);

		if (gui->xtext && GTK_XTEXT (gui->xtext)->emoji_cache)
			hex_emoji_chooser_set_emoji_cache (HEX_EMOJI_CHOOSER (emoji_chooser),
			                                    GTK_XTEXT (gui->xtext)->emoji_cache);

		g_signal_connect (emoji_chooser, "emoji-picked",
		                  G_CALLBACK (mg_emoji_picked), gui);
	}

	gtk_widget_grab_focus (entry);
}

static void
mg_switch_tab_cb (chanview *cv, chan *ch, int tag, gpointer ud)
{
	chan *old;
	session *sess = ud;

	old = active_tab;
	active_tab = ch;

	if (tag == TAG_IRC)
	{
		/* Validate session is still in the session list before using it */
		if (!is_session (sess))
			return;

		if (active_tab != old)
		{
			if (old && current_tab)
				mg_unpopulate (current_tab);
			mg_populate (sess);
			chathistory_notify_tab_switch (sess);
			/* Reset scroll-to-top backoff for the new tab */
			fe_reset_scroll_top_backoff (sess);
		}
	} else if (old != active_tab)
	{
		/* userdata for non-irc tabs is actually the GtkBox */
		mg_show_generic_tab (ud);
		if (!mg_is_userlist_and_tree_combined ())
			mg_userlist_showhide (current_sess, FALSE);	/* hide */
	}
}

/* compare two tabs (for tab sorting function) */

static int
mg_tabs_compare (session *a, session *b)
{
	/* server tabs always go first */
	if (a->type == SESS_SERVER)
		return -1;

	/* then channels */
	if (a->type == SESS_CHANNEL && b->type != SESS_CHANNEL)
		return -1;
	if (a->type != SESS_CHANNEL && b->type == SESS_CHANNEL)
		return 1;

	return g_ascii_strcasecmp (a->channel, b->channel);
}

static void
mg_create_tabs (session_gui *gui)
{
	gboolean use_icons = FALSE;

	/* if any one of these PNGs exist, the chanview will create
	 * the extra column for icons. */
	if (prefs.hex_gui_tab_icons && (pix_tree_channel || pix_tree_dialog || pix_tree_server || pix_tree_util))
	{
		use_icons = TRUE;
	}

	gui->chanview = chanview_new (prefs.hex_gui_tab_layout, prefs.hex_gui_tab_trunc,
											prefs.hex_gui_tab_sort, use_icons);
	chanview_set_callbacks (gui->chanview, mg_switch_tab_cb, mg_xbutton_cb,
									mg_tab_contextmenu_cb, (void *)mg_tabs_compare);
	mg_place_userlist_and_chanview (gui);
}

static void
mg_tabwin_focus_cb (GtkEventControllerFocus *controller, gpointer userdata)
{
	GtkWidget *win = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
	(void)userdata;
	current_sess = current_tab;
	if (current_sess)
	{
		gtk_xtext_check_marker_visibility (GTK_XTEXT (current_sess->gui->xtext));
		markread_send_for_session (current_sess);
		plugin_emit_dummy_print (current_sess, "Focus Window");
	}
	unflash_window (win);
}

static void
mg_topwin_focus_cb (GtkEventControllerFocus *controller, session *sess)
{
	GtkWidget *win = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
	current_sess = sess;
	if (!sess->server->server_session)
		sess->server->server_session = sess;
	gtk_xtext_check_marker_visibility(GTK_XTEXT (current_sess->gui->xtext));
	markread_send_for_session (sess);
	unflash_window (win);
	plugin_emit_dummy_print (sess, "Focus Window");
}

static void
mg_create_menu (session *sess, GtkWidget *table, int away_state)
{
	session_gui *gui = sess->gui;
	(void)table;

	/* GtkApplicationWindow renders the application menubar (installed at
	 * startup via gtk_application_set_menubar) when show-menubar is TRUE.
	 * Register the per-window action map so win.* references resolve. */
	if (gui->window)
	{
		GAction *action;

		menu_setup_window (gui->window, away_state, gui->menu_item);

		/* Detached windows have a single session; store it so window-scoped
		 * actions (detach, attach, etc.) operate on the correct session
		 * regardless of global focus state. Tabbed windows use current_tab. */
		if (!gui->is_tab)
			g_object_set_data (G_OBJECT (gui->window), "hc-sess", sess);

		/* Only one of {detach, attach} applies per window; disable the other
		 * so it renders greyed out in the menu. */
		action = g_action_map_lookup_action (G_ACTION_MAP (gui->window), "detach");
		if (action)
			g_simple_action_set_enabled (G_SIMPLE_ACTION (action), gui->is_tab);
		action = g_action_map_lookup_action (G_ACTION_MAP (gui->window), "attach");
		if (action)
			g_simple_action_set_enabled (G_SIMPLE_ACTION (action), !gui->is_tab);

		if (GTK_IS_APPLICATION_WINDOW (gui->window))
			gtk_application_window_set_show_menubar (
				GTK_APPLICATION_WINDOW (gui->window), !prefs.hex_gui_hide_menu);
	}
	gui->menu = NULL;
}

static void
mg_create_irctab (session *sess, GtkWidget *table)
{
	GtkWidget *vbox;
	session_gui *gui = sess->gui;

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_hexpand (vbox, TRUE);
	gtk_widget_set_vexpand (vbox, TRUE);
	gtk_grid_attach (GTK_GRID (table), vbox, 1, 2, 1, 1);
	mg_create_center (sess, gui, vbox);
}

static void
mg_create_topwindow (session *sess)
{
	GtkWidget *win;
	GtkWidget *table;

	if (sess->type == SESS_DIALOG)
		win = gtkutil_window_new ("PoxChat", NULL,
										  prefs.hex_gui_dialog_width, prefs.hex_gui_dialog_height, 0);
	else
		win = gtkutil_window_new ("PoxChat", NULL,
										  prefs.hex_gui_win_width,
										  prefs.hex_gui_win_height, 0);
	sess->gui->window = win;
	hc_widget_set_margin_all (win, GUI_BORDER);
	/* GTK4: gtk_window_set_opacity removed; use CSS opacity if needed */

	{
		GtkEventController *focus_controller = gtk_event_controller_focus_new ();
		g_signal_connect (focus_controller, "enter",
								G_CALLBACK (mg_topwin_focus_cb), sess);
		gtk_widget_add_controller (win, focus_controller);
	}
	g_signal_connect (G_OBJECT (win), "destroy",
							G_CALLBACK (mg_topdestroy_cb), sess);
	g_signal_connect (G_OBJECT (win), "notify::default-width",
							G_CALLBACK (mg_configure_cb), sess);
	g_signal_connect (G_OBJECT (win), "notify::default-height",
							G_CALLBACK (mg_configure_cb), sess);

	palette_alloc (win);

	table = gtk_grid_new ();
	/* spacing under the menubar */
	gtk_grid_set_row_spacing (GTK_GRID (table), GUI_SPACING);
	/* left and right borders */
	gtk_grid_set_column_spacing (GTK_GRID (table), 1);
	gtk_window_set_child (GTK_WINDOW (win), table);

	mg_create_irctab (sess, table);
	mg_create_menu (sess, table, sess->server->is_away);

	/* Set up keyboard shortcuts for menu actions */
	menu_add_shortcuts (win);

	if (sess->res->buffer == NULL)
	{
		sess->res->buffer = gtk_xtext_buffer_new (GTK_XTEXT (sess->gui->xtext));
		gtk_xtext_buffer_show (GTK_XTEXT (sess->gui->xtext), sess->res->buffer, TRUE);
		gtk_xtext_set_time_stamp (sess->res->buffer, prefs.hex_stamp_text);
		sess->res->user_model = userlist_create_model (sess);
	}

	userlist_show (sess);

	/* Will be shown when needed */
	gtk_widget_set_visible (sess->gui->topic_bar, FALSE);

	if (!prefs.hex_gui_ulist_buttons)
		gtk_widget_set_visible (sess->gui->button_box, FALSE);

	if (!prefs.hex_gui_input_nick)
		gtk_widget_set_visible (sess->gui->nick_box, FALSE);

	gtk_widget_set_visible(sess->gui->shbox, FALSE);

	mg_decide_userlist (sess, FALSE);

	if (sess->type == SESS_DIALOG)
	{
		/* hide the chan-mode buttons */
		gtk_widget_set_visible (sess->gui->topicbutton_box, FALSE);
	} else
	{
		gtk_widget_set_visible (sess->gui->dialogbutton_box, FALSE);

		if (!prefs.hex_gui_mode_buttons)
			gtk_widget_set_visible (sess->gui->topicbutton_box, FALSE);
	}

	mg_place_userlist_and_chanview (sess->gui);

	gtk_window_present (GTK_WINDOW (win));
}

/* Capture WM state (sticky + outer geometry) into prefs. Called from
 * close-request and from fe_cleanup so quit-via-menu also persists state.
 * Skipped while maximized/fullscreen so we don't overwrite the un-maximized
 * geometry the user actually wants restored. */
void
mg_capture_window_state (void)
{
	GtkWindow *win;
	int x, y, w, h;

	if (!mg_gui || !mg_gui->window)
		return;

	win = GTK_WINDOW (mg_gui->window);
	if (wm_hints_supports_sticky (win))
		prefs.hex_gui_win_sticky = wm_hints_get_sticky (win) ? 1 : 0;

	if (!prefs.hex_gui_win_state && !prefs.hex_gui_win_fullscreen
	    && wm_hints_get_geometry (win, &x, &y, &w, &h))
	{
		prefs.hex_gui_win_left = x;
		prefs.hex_gui_win_top = y;
		prefs.hex_gui_win_width = w;
		prefs.hex_gui_win_height = h;
	}
}

static gboolean
mg_tabwindow_de_cb (GtkWindow *win)
{
	GSList *list;
	session *sess;

	mg_capture_window_state ();

	if (prefs.hex_gui_tray_close && gtkutil_tray_icon_supported (win) && tray_toggle_visibility (FALSE))
		return TRUE;

	/* check for remaining toplevel windows */
	list = sess_list;
	while (list)
	{
		sess = list->data;
		if (!sess->gui->is_tab)
		{
			return FALSE;
		}
		list = list->next;
	}

	mg_open_quit_dialog (TRUE);
	return TRUE;
}

static void
mg_create_tabwindow (session *sess)
{
	GtkWidget *win;
	GtkWidget *table;

	win = gtkutil_window_new ("PoxChat", NULL, prefs.hex_gui_win_width,
									  prefs.hex_gui_win_height, 0);
	sess->gui->window = win;

#ifdef HAVE_GTK_MAC
	/* macOS: GtkHeaderBar with use-native-controls draws the native
	 * traffic-light close/minimize/maximize buttons integrated with
	 * GTK's custom titlebar, so the window chrome looks Mac-native
	 * while still letting us put custom content in the titlebar.
	 * Windows GTK doesn't implement this yet, so skip there. */
	{
		GtkWidget *header = gtk_header_bar_new ();
		g_object_set (header, "use-native-controls", TRUE, NULL);
		gtk_header_bar_set_show_title_buttons (GTK_HEADER_BAR (header), TRUE);
		gtk_window_set_titlebar (GTK_WINDOW (win), header);
	}
#endif

	if (prefs.hex_gui_win_state)
		gtk_window_maximize (GTK_WINDOW (win));
	if (prefs.hex_gui_win_fullscreen)
		gtk_window_fullscreen (GTK_WINDOW (win));
	if (prefs.hex_gui_win_sticky)
		wm_hints_set_sticky (GTK_WINDOW (win), TRUE);

	/* Restore last-known outer position if it still lands on a connected
	 * monitor (e.g. don't strand the window off-screen after a second
	 * display has been unplugged). Only meaningful on X11 — GTK4 itself
	 * doesn't expose window positioning. */
	if (prefs.hex_gui_win_save
	    && !prefs.hex_gui_win_state && !prefs.hex_gui_win_fullscreen
	    && (prefs.hex_gui_win_left || prefs.hex_gui_win_top)
	    && wm_hints_position_on_screen (GTK_WINDOW (win),
	                                    prefs.hex_gui_win_left,
	                                    prefs.hex_gui_win_top))
	{
		wm_hints_set_position (GTK_WINDOW (win),
		                       prefs.hex_gui_win_left,
		                       prefs.hex_gui_win_top);
	}
	/* GTK4: gtk_window_set_opacity removed; use CSS opacity if needed */
	hc_widget_set_margin_all (win, GUI_BORDER);

	g_signal_connect (G_OBJECT (win), "close-request",
						   G_CALLBACK (mg_tabwindow_de_cb), 0);
	g_signal_connect (G_OBJECT (win), "destroy",
						   G_CALLBACK (mg_tabwindow_kill_cb), 0);
	{
		GtkEventController *focus_controller = gtk_event_controller_focus_new ();
		g_signal_connect (focus_controller, "enter",
								G_CALLBACK (mg_tabwin_focus_cb), NULL);
		gtk_widget_add_controller (win, focus_controller);
	}
	g_signal_connect (G_OBJECT (win), "notify::default-width",
							G_CALLBACK (mg_configure_cb), NULL);
	g_signal_connect (G_OBJECT (win), "notify::default-height",
							G_CALLBACK (mg_configure_cb), NULL);
	g_signal_connect (G_OBJECT (win), "notify::maximized",
							G_CALLBACK (mg_windowstate_cb), NULL);
	g_signal_connect (G_OBJECT (win), "notify::fullscreened",
							G_CALLBACK (mg_windowstate_cb), NULL);
	/* Connect to realize to hook surface state for minimize-to-tray */
	g_signal_connect (G_OBJECT (win), "realize",
							G_CALLBACK (mg_realize_cb), NULL);

	palette_alloc (win);

	sess->gui->main_table = table = gtk_grid_new ();
	/* Row/column spacing at 0 — gaps come from per-widget margins so we can
	 * keep a consistent 6px universal margin (12px at pane junctions). */
	gtk_grid_set_row_spacing (GTK_GRID (table), 0);
	gtk_grid_set_column_spacing (GTK_GRID (table), 0);
	/* GTK4: Add bottom margin so input box doesn't touch window edge,
	 * but skip when tabs are at bottom (they provide their own spacing) */
	if (prefs.hex_gui_tab_pos != POS_BOTTOM)
		gtk_widget_set_margin_bottom (table, 0);
	gtk_window_set_child (GTK_WINDOW (win), table);

	/* Tab-bar drop strips at the very top and very bottom of the grid.
	 * Hidden by default; revealed on chanview drag-begin so the user
	 * can drop the tab bar onto either strip to move it to POS_TOP or
	 * POS_BOTTOM. Rows 0 and 4 are normally unused (irctab vbox at 2,
	 * chanview POS_TOP at 1, chanview POS_BOTTOM at 3). */
	sess->gui->top_drop_strip = mg_create_drop_strip (POS_TOP);
	gtk_grid_attach (GTK_GRID (table), sess->gui->top_drop_strip, 0, 0, 3, 1);

	sess->gui->bottom_drop_strip = mg_create_drop_strip (POS_BOTTOM);
	gtk_grid_attach (GTK_GRID (table), sess->gui->bottom_drop_strip, 0, 4, 3, 1);

	mg_create_irctab (sess, table);
	mg_create_tabs (sess->gui);
	mg_create_menu (sess, table, sess->server->is_away);

	/* Set up keyboard shortcuts for menu actions */
	menu_add_shortcuts (win);

	mg_focus (sess);

	mg_decide_userlist (sess, FALSE);

	/* Will be shown when needed */
	gtk_widget_set_visible (sess->gui->topic_bar, FALSE);

	if (!prefs.hex_gui_mode_buttons)
		gtk_widget_set_visible (sess->gui->topicbutton_box, FALSE);

	if (!prefs.hex_gui_ulist_buttons)
		gtk_widget_set_visible (sess->gui->button_box, FALSE);

	if (!prefs.hex_gui_input_nick)
		gtk_widget_set_visible (sess->gui->nick_box, FALSE);

	gtk_widget_set_visible (sess->gui->shbox, FALSE);

	mg_place_userlist_and_chanview (sess->gui);

	/* GTK4: Ensure topic bar children are visible before presenting window */
	if (prefs.hex_gui_mode_buttons)
	{
		gtk_widget_set_visible (sess->gui->topicbutton_box, TRUE);
	}
	gtk_window_present (GTK_WINDOW (win));
}

void
mg_apply_setup (void)
{
	GSList *list = sess_list;
	session *sess;
	int done_main = FALSE;

	mg_create_tab_colors ();

	while (list)
	{
		sess = list->data;
		/* Re-apply tab colors so "Smaller Text" takes effect immediately */
		if (sess->res->tab)
			chan_set_color (sess->res->tab, plain_list);
		gtk_xtext_set_time_stamp (sess->res->buffer, prefs.hex_stamp_text);
		gtk_xtext_recalc_day_boundaries ((xtext_buffer *)sess->res->buffer);
		((xtext_buffer *)sess->res->buffer)->needs_recalc = TRUE;
		if (!sess->gui->is_tab || !done_main)
			mg_place_userlist_and_chanview (sess->gui);
		if (sess->gui->is_tab)
			done_main = TRUE;
		list = list->next;
	}

	/* Re-apply xtext settings AFTER all buffers are updated, so the
	 * active buffer's recalc sees the correct time_stamp/indent state. */
	if (mg_gui)
	{
		mg_update_xtext (mg_gui->xtext);
		gtk_widget_queue_draw (mg_gui->xtext);
		/* Font-size changes alter the collapse thresholds but don't move
		 * any pane, so the notify::position chain won't fire. Trigger
		 * the update explicitly. */
		g_idle_add (mg_layout_update_idle_cb, mg_gui);
	}
}

static chan *
mg_add_generic_tab (char *name, char *title, void *family, GtkWidget *box)
{
	chan *ch;

	hc_page_container_append (mg_gui->note_book, box);

	ch = chanview_add (mg_gui->chanview, name, NULL, box, TRUE, TAG_UTIL, pix_tree_util);
	chan_set_color (ch, plain_list);

	g_object_set_data_full (G_OBJECT (box), "title", g_strdup (title), g_free);
	g_object_set_data (G_OBJECT (box), "ch", ch);

	if (prefs.hex_gui_tab_newtofront)
		chan_focus (ch);

	return ch;
}

void
fe_buttons_update (session *sess)
{
	session_gui *gui = sess->gui;

	hc_widget_destroy_impl (gui->button_box);
	gui->button_box = mg_create_userlistbuttons (gui->button_box_parent);

	if (prefs.hex_gui_ulist_buttons)
		gtk_widget_set_visible (sess->gui->button_box, TRUE);
	else
		gtk_widget_set_visible (sess->gui->button_box, FALSE);
}

void
fe_clear_channel (session *sess)
{
	char tbuf[CHANLEN+6];
	session_gui *gui = sess->gui;

	if (sess->gui->is_tab)
	{
		if (sess->waitchannel[0])
		{
			if (prefs.hex_gui_tab_trunc > 2 && g_utf8_strlen (sess->waitchannel, -1) > prefs.hex_gui_tab_trunc)
			{
				/* truncate long channel names */
				tbuf[0] = '(';
				strcpy (tbuf + 1, sess->waitchannel);
				g_utf8_offset_to_pointer(tbuf, prefs.hex_gui_tab_trunc)[0] = 0;
				strcat (tbuf, "..)");
			} else
			{
				sprintf (tbuf, "(%s)", sess->waitchannel);
			}
		}
		else
			strcpy (tbuf, _("<none>"));
		chan_rename (sess->res->tab, tbuf, prefs.hex_gui_tab_trunc);
	}

	if (!sess->gui->is_tab || sess == current_tab)
	{
		hc_entry_set_text (gui->topic_entry, "");

		if (gui->op_xpm)
		{
			hc_widget_destroy_impl (gui->op_xpm);
			gui->op_xpm = 0;
		}
	} else
	{
		if (sess->res->topic_text)
		{
			g_free (sess->res->topic_text);
			sess->res->topic_text = NULL;
		}
	}
}

void
fe_set_nonchannel (session *sess, int state)
{
}

void
fe_dlgbuttons_update (session *sess)
{
	GtkWidget *box;
	session_gui *gui = sess->gui;

	hc_widget_destroy_impl (gui->dialogbutton_box);

	gui->dialogbutton_box = box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, GUI_PANE_MARGIN);
	gtk_widget_set_margin_start (box, GUI_PANE_MARGIN);
	gtk_box_append (GTK_BOX (gui->topic_bar), box);
	hc_box_reorder_child (GTK_BOX (gui->topic_bar), box, 3);
	mg_create_dialogbuttons (box);

	if (current_tab && current_tab->type != SESS_DIALOG)
		gtk_widget_set_visible (current_tab->gui->dialogbutton_box, FALSE);
}

void
fe_update_mode_buttons (session *sess, char mode, char sign)
{
	int state, i;

	if (sign == '+')
		state = TRUE;
	else
		state = FALSE;

	for (i = 0; i < NUM_FLAG_WIDS - 1; i++)
	{
		if (chan_flags[i] == mode)
		{
			if (!sess->gui->is_tab || sess == current_tab)
			{
				ignore_chanmode = TRUE;
				if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (sess->gui->flag_wid[i])) != state)
					gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (sess->gui->flag_wid[i]), state);
				ignore_chanmode = FALSE;
			} else
			{
				sess->res->flag_wid_state[i] = state;
			}
			return;
		}
	}
}

void
fe_set_nick (server *serv, char *newnick)
{
	GSList *list = sess_list;
	session *sess;

	while (list)
	{
		sess = list->data;
		if (sess->server == serv)
		{
			if (current_tab == sess || !sess->gui->is_tab)
			{
				GtkWidget *lbl = g_object_get_data (G_OBJECT (sess->gui->nick_label), "nick-label");
				if (lbl)
					gtk_label_set_text (GTK_LABEL (lbl), newnick);
			}
		}
		list = list->next;
	}
}

void
fe_set_away (server *serv)
{
	GSList *list = sess_list;
	session *sess;

	while (list)
	{
		sess = list->data;
		if (sess->server == serv)
		{
			if (!sess->gui->is_tab || sess == current_tab)
			{
				menu_set_away (sess->gui, serv->is_away);
				/* gray out my nickname */
				mg_set_myself_away (sess->gui, serv->is_away);
			}
		}
		list = list->next;
	}
}

void
fe_set_channel (session *sess)
{
	if (sess->res->tab != NULL)
		chan_rename (sess->res->tab, sess->channel, prefs.hex_gui_tab_trunc);
}

void
mg_changui_new (session *sess, restore_gui *res, int tab, int focus)
{
	int first_run = FALSE;
	session_gui *gui;

	if (res == NULL)
	{
		res = g_new0 (restore_gui, 1);
	}

	sess->res = res;

	if (sess->server->front_session == NULL)
	{
		sess->server->front_session = sess;
	}

	if (!tab)
	{
		gui = g_new0 (session_gui, 1);
		gui->is_tab = FALSE;
		sess->gui = gui;
		mg_create_topwindow (sess);
		fe_set_title (sess);
		return;
	}

	if (mg_gui == NULL)
	{
		first_run = TRUE;
		gui = &static_mg_gui;
		memset (gui, 0, sizeof (session_gui));
		gui->is_tab = TRUE;
		sess->gui = gui;
		mg_create_tabwindow (sess);
		mg_gui = gui;
		parent_window = gui->window;
	} else
	{
		sess->gui = gui = mg_gui;
		gui->is_tab = TRUE;
	}

	mg_add_chan (sess);

	if (first_run || (prefs.hex_gui_tab_newtofront == FOCUS_NEW_ONLY_ASKED && focus)
			|| prefs.hex_gui_tab_newtofront == FOCUS_NEW_ALL )
		chan_focus (res->tab);
}

GtkWidget *
mg_create_generic_tab (char *name, char *title, int force_toplevel,
							  int link_buttons,
							  void *close_callback, void *userdata,
							  int width, int height, GtkWidget **vbox_ret,
							  void *family)
{
	GtkWidget *vbox, *win;

	if (prefs.hex_gui_tab_pos == POS_HIDDEN && prefs.hex_gui_tab_utils)
		prefs.hex_gui_tab_utils = 0;

	if (force_toplevel || !prefs.hex_gui_tab_utils)
	{
		win = gtkutil_window_new (title, name, width, height, 2);
		vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
		hc_widget_set_margin_all (vbox, 12);
		*vbox_ret = vbox;
		gtk_window_set_child (GTK_WINDOW (win), vbox);
		if (close_callback)
			g_signal_connect (G_OBJECT (win), "destroy",
									G_CALLBACK (close_callback), userdata);
		return win;
	}

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
	g_object_set_data (G_OBJECT (vbox), "w", GINT_TO_POINTER (width));
	g_object_set_data (G_OBJECT (vbox), "h", GINT_TO_POINTER (height));
	hc_widget_set_margin_all (vbox, 12);
	*vbox_ret = vbox;

	if (close_callback)
		g_signal_connect (G_OBJECT (vbox), "destroy",
								G_CALLBACK (close_callback), userdata);

	mg_add_generic_tab (name, title, family, vbox);


	return vbox;
}

void
mg_move_tab (session *sess, int delta)
{
	if (sess->gui->is_tab)
		chan_move (sess->res->tab, delta);
}

void
mg_move_tab_family (session *sess, int delta)
{
	if (sess->gui->is_tab)
		chan_move_family (sess->res->tab, delta);
}

void
mg_set_title (GtkWidget *vbox, char *title) /* for non-irc tab/window only */
{
	char *old;

	old = g_object_get_data (G_OBJECT (vbox), "title");
	if (old)
	{
		g_object_set_data_full (G_OBJECT (vbox), "title", g_strdup (title), g_free);
	} else
	{
		gtk_window_set_title (GTK_WINDOW (vbox), title);
	}
}

void
fe_server_callback (server *serv)
{
	joind_close (serv);

	if (serv->gui->chanlist_window)
		mg_close_gen (NULL, serv->gui->chanlist_window);

	if (serv->gui->rawlog_window)
		mg_close_gen (NULL, serv->gui->rawlog_window);

	g_free (serv->gui);
}

/* called when a session is being killed */

void
fe_session_callback (session *sess)
{
	/* Remove the tab from the channel view FIRST, before freeing anything.
	 * This prevents the tab from being clicked while the session is being destroyed. */
	if (sess->res->tab)
	{
		/* Clear current_tab if it points to this session, so mg_switch_tab_cb
		 * doesn't try to mg_unpopulate() a session that's being destroyed */
		if (current_tab == sess)
			current_tab = NULL;

		chan_remove (sess->res->tab, TRUE);
		sess->res->tab = NULL;
	}

	gtk_xtext_buffer_free (sess->res->buffer);
	g_object_unref (G_OBJECT (sess->res->user_model));

	if (sess->res->banlist && sess->res->banlist->window)
		mg_close_gen (NULL, sess->res->banlist->window);

	g_free (sess->res->input_text);
	g_free (sess->res->topic_text);
	g_free (sess->res->limit_text);
	g_free (sess->res->key_text);
	g_free (sess->res->queue_text);
	g_free (sess->res->queue_tip);
	g_free (sess->res->lag_text);
	g_free (sess->res->lag_tip);

	if (sess->gui->bartag)
		fe_timeout_remove (sess->gui->bartag);

	if (sess->gui != &static_mg_gui)
		g_free (sess->gui);
	g_free (sess->res);
}

/* ===== DRAG AND DROP STUFF ===== */
/*
 * GTK4 DND Implementation
 *
 * Uses GtkDropTarget for receiving drops and GtkDragSource for initiating drags.
 * Two main use cases:
 * 1. File drops on xtext/userlist for DCC file transfers
 * 2. Layout swapping by dragging userlist/chanview to scrollbar positions
 */

/* File drop handler for xtext (DCC send to current channel/dialog) */
static gboolean
mg_xtext_file_drop_cb (GtkDropTarget *target, const GValue *value,
                       double x, double y, gpointer user_data)
{
	GFile *file;
	char *uri;

	(void)target; (void)x; (void)y; (void)user_data;

	if (!G_VALUE_HOLDS (value, G_TYPE_FILE))
		return FALSE;

	file = g_value_get_object (value);
	if (!file)
		return FALSE;

	uri = g_file_get_uri (file);
	if (uri)
	{
		if (current_sess->type == SESS_DIALOG)
		{
			/* sess->channel is really the nickname of dialogs */
			mg_dnd_drop_file (current_sess, current_sess->channel, uri);
		}
		else if (current_sess->type == SESS_CHANNEL)
		{
			/* For channels, need to select a user first - just show a message */
			/* This matches GTK3 behavior - file drops on channel require selecting a user */
		}
		g_free (uri);
	}

	return TRUE;
}

/* Internal layout swapping target types */
#define DND_TARGET_CHANVIEW "POXCHAT_CHANVIEW"
#define DND_TARGET_USERLIST "POXCHAT_USERLIST"

/* Apply a layout drop: put the dragged panel (chanview or userlist) in
 * slot `target_pos` (1 = TL, 2 = BL, 3 = TR, 4 = BR) and bump the other
 * panel to a different slot on the same side if they'd collide. Called
 * from both the pane and xtext drop handlers with the pos resolved
 * beforehand — see mg_pane_layout_drop_cb / mg_xtext_layout_drop_cb. */
/* Deferred layout rebuild after a layout-swap drop. The drop handler
 * calls this from g_idle_add so the drag lifecycle (drag-end, widget-
 * hierarchy settlement) completes BEFORE we reparent the drag source
 * widget. Doing the reparent inline from the drop handler can wedge
 * the drag source into a state where subsequent drags don't initiate. */
static gboolean
mg_layout_drop_idle_cb (gpointer user_data)
{
	session_gui *gui = user_data;
	if (gui && current_sess && current_sess->gui == gui)
		mg_place_userlist_and_chanview (gui);
	return G_SOURCE_REMOVE;
}

static void
mg_apply_layout_drop (const char *drop_type, int target_pos)
{
	int *this_pos;
	int *other_pos;
	session_gui *gui;

	if (!current_sess || !current_sess->gui)
		return;
	gui = current_sess->gui;

	if (g_strcmp0 (drop_type, DND_TARGET_USERLIST) == 0)
	{
		/* Userlist can only live in the four vpane slots (1-4);
		 * top/bottom strip positions don't have a userlist layout. */
		if (target_pos < POS_TOPLEFT || target_pos > POS_BOTTOMRIGHT)
			return;
		this_pos = &prefs.hex_gui_ulist_pos;
		other_pos = &prefs.hex_gui_tab_pos;
	}
	else if (g_strcmp0 (drop_type, DND_TARGET_CHANVIEW) == 0)
	{
		/* Chanview can land in any of the 6 positions: vpane slots
		 * 1-4 (tree-style placement) or POS_TOP/POS_BOTTOM (horizontal
		 * bar). The layout handles both regardless of current
		 * hex_gui_tab_layout — if a wide tab bar ends up in a vpane
		 * slot, it looks cramped but remains functional until the
		 * user flips View→Channel Tree mode. (A future pref can
		 * auto-convert mode on drop.) */
		this_pos = &prefs.hex_gui_tab_pos;
		other_pos = &prefs.hex_gui_ulist_pos;
	}
	else
	{
		return;
	}

	*this_pos = target_pos;

	/* Collision: bump the other panel to the opposite vertical slot on
	 * whichever side it was on. */
	if (*this_pos == *other_pos)
	{
		switch (*other_pos)
		{
		case 1: *other_pos = 2; break;
		case 2: *other_pos = 1; break;
		case 3: *other_pos = 4; break;
		case 4: *other_pos = 3; break;
		}
	}

	/* Defer the actual layout rebuild: reparenting the drag source's
	 * widget from the drop handler wedges subsequent drags. Scheduling
	 * via idle lets drag-end fire and the gesture machinery reset before
	 * the widget hierarchy changes underneath it. */
	g_idle_add (mg_layout_drop_idle_cb, gui);
}

/* Drop handler for a top/bottom strip in tabs-mode: user_data is
 * GINT_TO_POINTER(POS_TOP) or GINT_TO_POINTER(POS_BOTTOM). Only accepts
 * chanview payloads (POS_TOP/BOTTOM don't apply to userlist). */
static gboolean
mg_strip_layout_drop_cb (GtkDropTarget *target, const GValue *value,
                         double x, double y, gpointer user_data)
{
	GtkWidget *widget;
	const char *drop_type;
	int pos = GPOINTER_TO_INT (user_data);

	(void) x; (void) y;

	/* Clear our own hover class up front — some drop completions don't
	 * emit a trailing leave event, which leaves the strip tinted. */
	widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
	if (widget)
		gtk_widget_remove_css_class (widget, "poxchat-drop-hover");

	/* Hide both strips now too. The layout rebuild triggered by
	 * mg_apply_layout_drop can reparent chanview / re-run pane placement,
	 * which sometimes leaves drag-end without a clean ancestor walk from
	 * the source back to the gui — relying on drag-end alone can leave
	 * the strips visible (and their min-height reserving space). */
	if (current_sess && current_sess->gui)
	{
		session_gui *gui = current_sess->gui;
		if (gui->top_drop_strip)
			gtk_widget_set_visible (gui->top_drop_strip, FALSE);
		if (gui->bottom_drop_strip)
			gtk_widget_set_visible (gui->bottom_drop_strip, FALSE);
	}

	if (!G_VALUE_HOLDS (value, G_TYPE_STRING))
		return FALSE;
	drop_type = g_value_get_string (value);
	if (g_strcmp0 (drop_type, DND_TARGET_CHANVIEW) != 0)
		return FALSE;

	mg_apply_layout_drop (drop_type, pos);
	return TRUE;
}

static GdkDragAction
mg_strip_layout_motion_cb (GtkDropTarget *target, double x, double y,
                           gpointer user_data)
{
	GtkWidget *widget;

	(void) x; (void) y; (void) user_data;

	widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
	gtk_widget_add_css_class (widget, "poxchat-drop-hover");
	return GDK_ACTION_MOVE;
}

static void
mg_strip_layout_leave_cb (GtkDropTarget *target, gpointer user_data)
{
	GtkWidget *widget;

	(void) user_data;

	widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
	gtk_widget_remove_css_class (widget, "poxchat-drop-hover");
}

/* Create a tab-bar drop strip for POS_TOP (pos == 5) or POS_BOTTOM
 * (pos == 6). Hidden until a chanview drag begins. */
static GtkWidget *
mg_create_drop_strip (int pos)
{
	GtkWidget *strip;
	GtkDropTarget *target;

	strip = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class (strip, "poxchat-drop-strip");
	gtk_widget_set_hexpand (strip, TRUE);
	gtk_widget_set_visible (strip, FALSE);

	target = gtk_drop_target_new (G_TYPE_STRING, GDK_ACTION_MOVE);
	g_signal_connect (target, "drop", G_CALLBACK (mg_strip_layout_drop_cb),
	                  GINT_TO_POINTER (pos));
	g_signal_connect (target, "motion", G_CALLBACK (mg_strip_layout_motion_cb), NULL);
	g_signal_connect (target, "leave", G_CALLBACK (mg_strip_layout_leave_cb), NULL);
	gtk_widget_add_controller (strip, GTK_EVENT_CONTROLLER (target));

	return strip;
}

/* Drop handler for vpane_left / vpane_right. user_data is TRUE for the
 * left pane (slots 1/2) and FALSE for the right (slots 3/4); y within
 * the pane picks top vs bottom. */
static gboolean
mg_pane_layout_drop_cb (GtkDropTarget *target, const GValue *value,
                        double x, double y, gpointer user_data)
{
	GtkWidget *widget;
	GtkWidget *top_ind;
	GtkWidget *bot_ind;
	const char *drop_type;
	gboolean is_left = GPOINTER_TO_INT (user_data);
	int height;
	int pos;

	(void) x;

	widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));

	/* Clear hover state on the vpane's own indicators up front. Drop
	 * completion doesn't always emit a matching leave event, and the
	 * subsequent layout rebuild in mg_apply_layout_drop reparents
	 * widgets in a way that drag-end's ancestor walk can miss. */
	top_ind = widget ? g_object_get_data (G_OBJECT (widget), "hc-drop-top") : NULL;
	bot_ind = widget ? g_object_get_data (G_OBJECT (widget), "hc-drop-bottom") : NULL;
	if (top_ind) gtk_widget_remove_css_class (top_ind, "poxchat-drop-hover");
	if (bot_ind) gtk_widget_remove_css_class (bot_ind, "poxchat-drop-hover");

	/* Hide the tabs-mode strips too — if this drop is a chanview drag,
	 * the drag-begin revealed them and we need to hide them now before
	 * the layout rebuild that may prevent drag-end from finding them. */
	if (current_sess && current_sess->gui)
	{
		session_gui *gui = current_sess->gui;
		if (gui->top_drop_strip)
			gtk_widget_set_visible (gui->top_drop_strip, FALSE);
		if (gui->bottom_drop_strip)
			gtk_widget_set_visible (gui->bottom_drop_strip, FALSE);
	}

	if (!G_VALUE_HOLDS (value, G_TYPE_STRING))
		return FALSE;
	drop_type = g_value_get_string (value);
	if (!drop_type)
		return FALSE;

	height = gtk_widget_get_height (widget);

	if (y < height / 2)
		pos = is_left ? 1 : 3;
	else
		pos = is_left ? 2 : 4;

	mg_apply_layout_drop (drop_type, pos);
	return TRUE;
}

/* Helpers for xtext drop indicators: two strips overlaid on the xtext,
 * dynamically sized and positioned to cover the top and bottom halves
 * of the paneless side (the half of xtext where the drop would land).
 * When both sides already have panels, the drop handler rejects and the
 * indicators stay hidden. */

/* Returns TRUE iff a drop on xtext would be applied; also fills
 * paneless_left with the side on which the drop would land. */
static gboolean
mg_xtext_drop_is_active (gboolean *paneless_left_out)
{
	/* A side is paneless when nothing is placed there. Check each pref
	 * against the four pane-slot values explicitly — POS_TOP (5) and
	 * POS_BOTTOM (6) are NOT on either side, so a naive >=3 / <=2 test
	 * miscategorises tabs-mode chanview positions. Userlist contributes
	 * to side occupancy only when it's visible. */
	gboolean left_has_panel =
		(prefs.hex_gui_tab_pos == POS_TOPLEFT
		 || prefs.hex_gui_tab_pos == POS_BOTTOMLEFT)
		|| (!prefs.hex_gui_ulist_hide
		    && (prefs.hex_gui_ulist_pos == POS_TOPLEFT
		        || prefs.hex_gui_ulist_pos == POS_BOTTOMLEFT));
	gboolean right_has_panel =
		(prefs.hex_gui_tab_pos == POS_TOPRIGHT
		 || prefs.hex_gui_tab_pos == POS_BOTTOMRIGHT)
		|| (!prefs.hex_gui_ulist_hide
		    && (prefs.hex_gui_ulist_pos == POS_TOPRIGHT
		        || prefs.hex_gui_ulist_pos == POS_BOTTOMRIGHT));
	gboolean left_paneless = !left_has_panel;
	gboolean right_paneless = !right_has_panel;

	if (!left_paneless && !right_paneless)
		return FALSE;

	if (paneless_left_out)
		*paneless_left_out = left_paneless;
	return TRUE;
}

static GdkDragAction
mg_xtext_layout_motion_cb (GtkDropTarget *target, double x, double y,
                           gpointer user_data)
{
	GtkWidget *widget;
	GtkWidget *top_ind;
	GtkWidget *bot_ind;
	int width, height;
	gboolean paneless_left;

	(void) x; (void) user_data;

	widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
	top_ind = g_object_get_data (G_OBJECT (widget), "hc-drop-top");
	bot_ind = g_object_get_data (G_OBJECT (widget), "hc-drop-bottom");
	width = gtk_widget_get_width (widget);
	height = gtk_widget_get_height (widget);

	if (top_ind) gtk_widget_remove_css_class (top_ind, "poxchat-drop-hover");
	if (bot_ind) gtk_widget_remove_css_class (bot_ind, "poxchat-drop-hover");

	if (!mg_xtext_drop_is_active (&paneless_left))
	{
		if (top_ind) gtk_widget_set_visible (top_ind, FALSE);
		if (bot_ind) gtk_widget_set_visible (bot_ind, FALSE);
		return 0;
	}

	/* Size the strips to cover a quarter of xtext (one half vertically,
	 * one half horizontally on the paneless side). Positioned via halign
	 * = START or END. */
	if (top_ind)
	{
		gtk_widget_set_halign (top_ind,
			paneless_left ? GTK_ALIGN_START : GTK_ALIGN_END);
		gtk_widget_set_size_request (top_ind, width / 2, height / 2);
		gtk_widget_set_visible (top_ind, TRUE);
	}
	if (bot_ind)
	{
		gtk_widget_set_halign (bot_ind,
			paneless_left ? GTK_ALIGN_START : GTK_ALIGN_END);
		gtk_widget_set_size_request (bot_ind, width / 2, height / 2);
		gtk_widget_set_visible (bot_ind, TRUE);
	}

	if (y < height / 2)
	{
		if (top_ind) gtk_widget_add_css_class (top_ind, "poxchat-drop-hover");
	}
	else
	{
		if (bot_ind) gtk_widget_add_css_class (bot_ind, "poxchat-drop-hover");
	}

	return GDK_ACTION_MOVE;
}

static void
mg_xtext_layout_leave_cb (GtkDropTarget *target, gpointer user_data)
{
	GtkWidget *widget;
	GtkWidget *top_ind;
	GtkWidget *bot_ind;

	(void) user_data;

	widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
	top_ind = g_object_get_data (G_OBJECT (widget), "hc-drop-top");
	bot_ind = g_object_get_data (G_OBJECT (widget), "hc-drop-bottom");
	if (top_ind)
	{
		gtk_widget_remove_css_class (top_ind, "poxchat-drop-hover");
		gtk_widget_set_visible (top_ind, FALSE);
	}
	if (bot_ind)
	{
		gtk_widget_remove_css_class (bot_ind, "poxchat-drop-hover");
		gtk_widget_set_visible (bot_ind, FALSE);
	}
}

/* Drop handler for the xtext widget (layout-swap strings only — file
 * drops go through mg_xtext_file_drop_cb). Only active when exactly one
 * side is paneless: the drop then means "put this panel on the empty
 * side", with y picking top vs bottom within that side. When both
 * sides already have a panel the drop is ignored — the vpane drop
 * targets handle that case unambiguously. */
static gboolean
mg_xtext_layout_drop_cb (GtkDropTarget *target, const GValue *value,
                         double x, double y, gpointer user_data)
{
	GtkWidget *widget;
	const char *drop_type;
	int height;
	int pos;
	gboolean paneless_left = FALSE;

	(void) x; (void) user_data;

	if (!G_VALUE_HOLDS (value, G_TYPE_STRING))
		return FALSE;
	drop_type = g_value_get_string (value);
	if (!drop_type)
		return FALSE;

	/* Use the same paneless detector as the motion handler so drops
	 * land on the side the user saw highlighted. */
	if (!mg_xtext_drop_is_active (&paneless_left))
		return FALSE;

	widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
	height = gtk_widget_get_height (widget);

	if (paneless_left)
		pos = (y < height / 2) ? 1 : 2;
	else
		pos = (y < height / 2) ? 3 : 4;

	mg_apply_layout_drop (drop_type, pos);
	return TRUE;
}

/* Build a scaled-down static snapshot of widget for use as a drag icon.
 * Without an explicit icon, GTK4 falls back to rendering the drag payload
 * (a G_TYPE_STRING) as text, which looks ugly and has been seen to leave
 * stuck ghosts on Windows when the drag ends. An explicit paintable is
 * owned by the drag operation and cleaned up with it. */
static GdkPaintable *
mg_drag_snapshot_icon (GtkWidget *widget, double scale)
{
	int w = gtk_widget_get_width (widget);
	int h = gtk_widget_get_height (widget);
	if (w <= 0 || h <= 0)
		return NULL;

	int sw = MAX (1, (int) (w * scale));
	int sh = MAX (1, (int) (h * scale));

	GdkPaintable *live = gtk_widget_paintable_new (widget);
	if (!live)
		return NULL;

	/* Render the live paintable into a snapshot at the scaled target size,
	 * then detach into a static paintable so nothing references the widget
	 * once the drag is underway. */
	GtkSnapshot *snap = gtk_snapshot_new ();
	gdk_paintable_snapshot (live, GDK_SNAPSHOT (snap), sw, sh);
	g_object_unref (live);

	graphene_size_t size = GRAPHENE_SIZE_INIT ((float) sw, (float) sh);
	return gtk_snapshot_free_to_paintable (snap, &size);
}

/* Set a scaled-down widget snapshot as the drag icon on source, with the
 * hotspot placed at the user's click coordinates (scaled to match).
 * If the drag-source widget has a stashed "hc-drag-snapshot-target",
 * use that widget for the snapshot instead — the chanview tab bar wants
 * to snapshot just the tabs container, not the outer box (which is
 * often wider than the tabs themselves and leaves dead space / cursor
 * hovering in void around the icon). */
static void
mg_drag_set_snapshot_icon (GtkDragSource *source, double x, double y, double scale)
{
	GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (source));
	GtkWidget *snap_target;
	GdkPaintable *icon;
	double hotspot_x = x, hotspot_y = y;

	if (!widget)
		return;

	snap_target = g_object_get_data (G_OBJECT (widget), "hc-drag-snapshot-target");
	if (snap_target && gtk_widget_get_width (snap_target) > 0
	                && gtk_widget_get_height (snap_target) > 0)
	{
		/* Translate the click point from the source widget's coordinate
		 * space into the snapshot target's space for a sensible hotspot. */
		graphene_point_t src_pt = GRAPHENE_POINT_INIT ((float) x, (float) y);
		graphene_point_t dst_pt;
		if (gtk_widget_compute_point (widget, snap_target, &src_pt, &dst_pt))
		{
			hotspot_x = dst_pt.x;
			hotspot_y = dst_pt.y;
		}
		widget = snap_target;
	}

	icon = mg_drag_snapshot_icon (widget, scale);
	if (!icon)
		return;

	gtk_drag_source_set_icon (source, icon,
	                          (int) (hotspot_x * scale), (int) (hotspot_y * scale));
	g_object_unref (icon);
}

#define MG_DRAG_ICON_SCALE 0.5

/* Prepare callback for userlist drag source */
static GdkContentProvider *
mg_userlist_drag_prepare_cb (GtkDragSource *source, double x, double y, gpointer user_data)
{
	(void)user_data;

	mg_drag_set_snapshot_icon (source, x, y, MG_DRAG_ICON_SCALE);
	return gdk_content_provider_new_typed (G_TYPE_STRING, DND_TARGET_USERLIST);
}

/* Prepare callback for chanview drag source */
static GdkContentProvider *
mg_chanview_drag_prepare_cb (GtkDragSource *source, double x, double y, gpointer user_data)
{
	(void)user_data;

	mg_drag_set_snapshot_icon (source, x, y, MG_DRAG_ICON_SCALE);
	return gdk_content_provider_new_typed (G_TYPE_STRING, DND_TARGET_CHANVIEW);
}

/* Set up drop targets on the xtext widget: G_TYPE_FILE for DCC file
 * drops and G_TYPE_STRING for layout-swap drops when one side is
 * paneless. Two separate GtkDropTarget controllers because a single
 * target can't accept multiple unrelated types. */
void
mg_setup_xtext_dnd (GtkWidget *xtext)
{
	GtkDropTarget *target;

	target = gtk_drop_target_new (G_TYPE_FILE, GDK_ACTION_COPY | GDK_ACTION_MOVE);
	g_signal_connect (target, "drop", G_CALLBACK (mg_xtext_file_drop_cb), NULL);
	gtk_widget_add_controller (xtext, GTK_EVENT_CONTROLLER (target));

	target = gtk_drop_target_new (G_TYPE_STRING, GDK_ACTION_MOVE);
	g_signal_connect (target, "drop", G_CALLBACK (mg_xtext_layout_drop_cb), NULL);
	g_signal_connect (target, "motion", G_CALLBACK (mg_xtext_layout_motion_cb), NULL);
	g_signal_connect (target, "leave", G_CALLBACK (mg_xtext_layout_leave_cb), NULL);
	gtk_widget_add_controller (xtext, GTK_EVENT_CONTROLLER (target));
}

/* Motion handler: toggle hover CSS classes on the stashed top/bottom
 * indicator widgets based on y-position within the vpane. */
static GdkDragAction
mg_pane_layout_motion_cb (GtkDropTarget *target, double x, double y,
                          gpointer user_data)
{
	GtkWidget *widget;
	GtkWidget *top_ind;
	GtkWidget *bot_ind;
	int height;

	(void) x; (void) user_data;

	widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
	top_ind = g_object_get_data (G_OBJECT (widget), "hc-drop-top");
	bot_ind = g_object_get_data (G_OBJECT (widget), "hc-drop-bottom");
	height = gtk_widget_get_height (widget);

	if (top_ind)
		gtk_widget_remove_css_class (top_ind, "poxchat-drop-hover");
	if (bot_ind)
		gtk_widget_remove_css_class (bot_ind, "poxchat-drop-hover");

	if (y < height / 2)
	{
		if (top_ind)
			gtk_widget_add_css_class (top_ind, "poxchat-drop-hover");
	}
	else
	{
		if (bot_ind)
			gtk_widget_add_css_class (bot_ind, "poxchat-drop-hover");
	}

	return GDK_ACTION_MOVE;
}

static void
mg_pane_layout_leave_cb (GtkDropTarget *target, gpointer user_data)
{
	GtkWidget *widget;
	GtkWidget *top_ind;
	GtkWidget *bot_ind;

	(void) user_data;

	widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
	top_ind = g_object_get_data (G_OBJECT (widget), "hc-drop-top");
	bot_ind = g_object_get_data (G_OBJECT (widget), "hc-drop-bottom");
	if (top_ind)
		gtk_widget_remove_css_class (top_ind, "poxchat-drop-hover");
	if (bot_ind)
		gtk_widget_remove_css_class (bot_ind, "poxchat-drop-hover");
}

/* Install a layout-swap drop target on a vpane. `is_left` distinguishes
 * vpane_left (slots 1/2) from vpane_right (slots 3/4); y within the
 * pane picks top vs bottom. Returns a GtkOverlay that wraps the vpane
 * with two transparent hover-indicator strips (top half, bottom half)
 * so the user can see which slot their drop will land in. */
GtkWidget *
mg_setup_pane_layout_dnd (GtkWidget *vpane, gboolean is_left)
{
	GtkWidget *overlay;
	GtkWidget *top_ind;
	GtkWidget *bot_ind;
	GtkDropTarget *target;

	overlay = gtk_overlay_new ();
	gtk_overlay_set_child (GTK_OVERLAY (overlay), vpane);

	/* Two transparent strips, one for each half. They don't consume
	 * input (can-target = FALSE) so drop events reach the vpane. CSS
	 * makes them invisible by default; a .poxchat-drop-hover class on
	 * the one under the cursor lights it up during a drag. */
	top_ind = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class (top_ind, "poxchat-drop-indicator");
	gtk_widget_add_css_class (top_ind, "poxchat-drop-indicator-top");
	gtk_widget_set_can_target (top_ind, FALSE);
	gtk_widget_set_halign (top_ind, GTK_ALIGN_FILL);
	gtk_widget_set_valign (top_ind, GTK_ALIGN_START);
	gtk_widget_set_hexpand (top_ind, TRUE);
	gtk_overlay_add_overlay (GTK_OVERLAY (overlay), top_ind);

	bot_ind = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class (bot_ind, "poxchat-drop-indicator");
	gtk_widget_add_css_class (bot_ind, "poxchat-drop-indicator-bottom");
	gtk_widget_set_can_target (bot_ind, FALSE);
	gtk_widget_set_halign (bot_ind, GTK_ALIGN_FILL);
	gtk_widget_set_valign (bot_ind, GTK_ALIGN_END);
	gtk_widget_set_hexpand (bot_ind, TRUE);
	gtk_overlay_add_overlay (GTK_OVERLAY (overlay), bot_ind);

	g_object_set_data (G_OBJECT (vpane), "hc-drop-top", top_ind);
	g_object_set_data (G_OBJECT (vpane), "hc-drop-bottom", bot_ind);

	target = gtk_drop_target_new (G_TYPE_STRING, GDK_ACTION_MOVE);
	g_signal_connect (target, "drop", G_CALLBACK (mg_pane_layout_drop_cb),
	                  GINT_TO_POINTER (is_left));
	g_signal_connect (target, "motion", G_CALLBACK (mg_pane_layout_motion_cb), NULL);
	g_signal_connect (target, "leave", G_CALLBACK (mg_pane_layout_leave_cb), NULL);
	gtk_widget_add_controller (vpane, GTK_EVENT_CONTROLLER (target));

	return overlay;
}

/* Forward declaration — defined alongside the chanview drag-end below. */
static void mg_userlist_drag_end_cb (GtkDragSource *source, GdkDrag *drag,
                                     gboolean delete_data, gpointer user_data);

/* Set up userlist as drag source for layout swapping */
void
mg_setup_userlist_drag_source (GtkWidget *treeview)
{
	GtkDragSource *source;

	source = gtk_drag_source_new ();
	gtk_drag_source_set_actions (source, GDK_ACTION_MOVE);
	g_signal_connect (source, "prepare", G_CALLBACK (mg_userlist_drag_prepare_cb), NULL);
	g_signal_connect (source, "drag-end", G_CALLBACK (mg_userlist_drag_end_cb), NULL);
	gtk_widget_add_controller (treeview, GTK_EVENT_CONTROLLER (source));
}

/* Reset all drop-indicator visual state across every gui — the tabs-mode
 * top/bottom drop strips, the xtext overlay indicators, and the vpane
 * overlay indicators.  Called from drag-end and from each drop handler.
 *
 * The leave callbacks on individual drop targets are normally where this
 * cleanup happens, but GTK4 doesn't always emit a trailing leave after a
 * successful drop or a drag-cancel — and our layout-swap drops reparent
 * widgets in ways that break ancestor walks from the source.  Walking
 * every gui is harmless because at most one drag is active at a time. */
static void
mg_clear_all_drop_indicators (void)
{
	GSList *list;

	for (list = sess_list; list; list = list->next)
	{
		session *sess = list->data;
		session_gui *gui = sess->gui;
		GtkWidget *ind;

		if (!gui)
			continue;

		if (gui->top_drop_strip)
		{
			gtk_widget_remove_css_class (gui->top_drop_strip, "poxchat-drop-hover");
			gtk_widget_set_visible (gui->top_drop_strip, FALSE);
		}
		if (gui->bottom_drop_strip)
		{
			gtk_widget_remove_css_class (gui->bottom_drop_strip, "poxchat-drop-hover");
			gtk_widget_set_visible (gui->bottom_drop_strip, FALSE);
		}

		/* xtext indicators: explicit visibility + hover class. */
		if (gui->xtext)
		{
			ind = g_object_get_data (G_OBJECT (gui->xtext), "hc-drop-top");
			if (ind)
			{
				gtk_widget_remove_css_class (ind, "poxchat-drop-hover");
				gtk_widget_set_visible (ind, FALSE);
			}
			ind = g_object_get_data (G_OBJECT (gui->xtext), "hc-drop-bottom");
			if (ind)
			{
				gtk_widget_remove_css_class (ind, "poxchat-drop-hover");
				gtk_widget_set_visible (ind, FALSE);
			}
		}

		/* vpane indicators: always-visible-but-transparent, hover class only. */
		if (gui->vpane_left)
		{
			ind = g_object_get_data (G_OBJECT (gui->vpane_left), "hc-drop-top");
			if (ind) gtk_widget_remove_css_class (ind, "poxchat-drop-hover");
			ind = g_object_get_data (G_OBJECT (gui->vpane_left), "hc-drop-bottom");
			if (ind) gtk_widget_remove_css_class (ind, "poxchat-drop-hover");
		}
		if (gui->vpane_right)
		{
			ind = g_object_get_data (G_OBJECT (gui->vpane_right), "hc-drop-top");
			if (ind) gtk_widget_remove_css_class (ind, "poxchat-drop-hover");
			ind = g_object_get_data (G_OBJECT (gui->vpane_right), "hc-drop-bottom");
			if (ind) gtk_widget_remove_css_class (ind, "poxchat-drop-hover");
		}
	}
}

/* Walk up from the drag-source widget to find the session_gui that
 * owns it, so we can toggle its top/bottom drop strips during drag. */
static session_gui *
mg_gui_for_widget (GtkWidget *w)
{
	GSList *list;

	for (list = sess_list; list; list = list->next)
	{
		session *sess = list->data;
		if (sess->gui && sess->gui->main_table
		    && gtk_widget_is_ancestor (w, sess->gui->main_table))
			return sess->gui;
	}
	return NULL;
}

static void
mg_chanview_drag_begin_cb (GtkDragSource *source, GdkDrag *drag,
                           gpointer user_data)
{
	GtkWidget *src_widget;
	session_gui *gui;

	(void) drag; (void) user_data;

	src_widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (source));
	gui = src_widget ? mg_gui_for_widget (src_widget) : NULL;
	if (!gui)
		return;

	if (gui->top_drop_strip)
		gtk_widget_set_visible (gui->top_drop_strip, TRUE);
	if (gui->bottom_drop_strip)
		gtk_widget_set_visible (gui->bottom_drop_strip, TRUE);
}

static void
mg_chanview_drag_end_cb (GtkDragSource *source, GdkDrag *drag,
                         gboolean delete_data, gpointer user_data)
{
	(void) drag; (void) delete_data; (void) user_data;

	/* Reset the drag source gesture so its internal sequence state is
	 * clean for the next drag. Without this, in-app drops that reparent
	 * the drag source widget (our layout swap does exactly that) can
	 * leave the gesture in a state where the next press doesn't start
	 * a drag — clicks and right-click menus still work, just drag. */
	if (source)
		gtk_event_controller_reset (GTK_EVENT_CONTROLLER (source));

	mg_clear_all_drop_indicators ();
}

static void
mg_userlist_drag_end_cb (GtkDragSource *source, GdkDrag *drag,
                         gboolean delete_data, gpointer user_data)
{
	(void) drag; (void) delete_data; (void) user_data;

	if (source)
		gtk_event_controller_reset (GTK_EVENT_CONTROLLER (source));

	mg_clear_all_drop_indicators ();
}

/* Set up chanview as drag source for layout swapping */
void
mg_setup_chanview_drag_source (GtkWidget *widget)
{
	GtkDragSource *source;

	source = gtk_drag_source_new ();
	gtk_drag_source_set_actions (source, GDK_ACTION_MOVE);
	g_signal_connect (source, "prepare", G_CALLBACK (mg_chanview_drag_prepare_cb), NULL);
	/* Reveal tabs-mode drop strips during the drag so the user can
	 * target POS_TOP / POS_BOTTOM. Hidden again on drag-end. */
	g_signal_connect (source, "drag-begin", G_CALLBACK (mg_chanview_drag_begin_cb), NULL);
	g_signal_connect (source, "drag-end", G_CALLBACK (mg_chanview_drag_end_cb), NULL);
	gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (source));
}
