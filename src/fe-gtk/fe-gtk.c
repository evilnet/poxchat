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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "fe-gtk.h"
#include "hex-input-edit.h"

#ifdef WIN32
#include <gdk/win32/gdkwin32.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "../common/poxchat.h"
#include "../common/fe.h"
#include "../common/util.h"
#include "../common/text.h"
#include "../common/cfgfiles.h"
#include "../common/poxchatc.h"
#include "../common/plugin.h"
#include "../common/server.h"
#include "../common/scrollback.h"
#include "../common/outbound.h"
#include "../common/url.h"
#include "gtkutil.h"
#include "maingui.h"
#include "pixmaps.h"
#include "chanlist.h"
#include "joind.h"
#include "xtext.h"
#include "palette.h"
#include "menu.h"
#include "notifygui.h"
#include "textgui.h"
#include "userlistgui.h"
#include "fkeys.h"
#include "plugin-tray.h"
#include "urlgrab.h"
#include "setup.h"
#include "chanview.h"
#include "../common/network-icon.h"
#include "plugin-notification.h"

#ifdef USE_LIBCANBERRA
#include <canberra.h>
#endif

cairo_surface_t *channelwin_pix;

static GtkApplication *poxchat_app = NULL;

GtkApplication *
fe_get_application (void)
{
	return poxchat_app;
}

#ifdef USE_LIBCANBERRA
static ca_context *ca_con;
#endif

#ifdef HAVE_GTK_MAC
GtkosxApplication *osx_app;
#endif

/* === command-line parameter parsing : requires glib 2.6 === */

static char *arg_cfgdir = NULL;
static gint arg_show_autoload = 0;
static gint arg_show_config = 0;
static gint arg_show_version = 0;
static gint arg_minimize = 0;

static const GOptionEntry gopt_entries[] = 
{
 {"no-auto",	'a', 0, G_OPTION_ARG_NONE,	&arg_dont_autoconnect, N_("Don't auto connect to servers"), NULL},
 {"cfgdir",	'd', 0, G_OPTION_ARG_STRING,	&arg_cfgdir, N_("Use a different config directory"), "PATH"},
 {"no-plugins",	'n', 0, G_OPTION_ARG_NONE,	&arg_skip_plugins, N_("Don't auto load any plugins"), NULL},
 {"plugindir",	'p', 0, G_OPTION_ARG_NONE,	&arg_show_autoload, N_("Show plugin/script auto-load directory"), NULL},
 {"configdir",	'u', 0, G_OPTION_ARG_NONE,	&arg_show_config, N_("Show user config directory"), NULL},
 {"url",	 0,  G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &arg_url, N_("Open an irc://server:port/channel?key URL"), "URL"},
 {"command",	'c', 0, G_OPTION_ARG_STRING,	&arg_command, N_("Execute command:"), "COMMAND"},
#ifdef USE_DBUS
 {"existing",	'e', 0, G_OPTION_ARG_NONE,	&arg_existing, N_("Open URL or execute command in an existing PoxChat"), NULL},
#endif
 {"minimize",	 0,  0, G_OPTION_ARG_INT,	&arg_minimize, N_("Begin minimized. Level 0=Normal 1=Iconified 2=Tray"), N_("level")},
 {"version",	'v', 0, G_OPTION_ARG_NONE,	&arg_show_version, N_("Show version information"), NULL},
 {G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_STRING_ARRAY, &arg_urls, N_("Open an irc://server:port/channel?key URL"), "URL"},
 {NULL}
};

#ifdef WIN32
static void
create_msg_dialog_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	gboolean *done = (gboolean *)user_data;
	gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), result, NULL);
	*done = TRUE;
}

static void
create_msg_dialog (gchar *title, gchar *message)
{
	GtkAlertDialog *dialog;
	gboolean done = FALSE;

	dialog = gtk_alert_dialog_new ("%s", message);
	gtk_alert_dialog_set_detail (dialog, title);
	gtk_alert_dialog_set_modal (dialog, TRUE);

	gtk_alert_dialog_choose (dialog,
		parent_window ? GTK_WINDOW (parent_window) : NULL,
		NULL, create_msg_dialog_cb, &done);
	g_object_unref (dialog);

	/* Run a mini event loop until the dialog is closed */
	while (!done)
		g_main_context_iteration (NULL, TRUE);
}
#endif /* WIN32 */

/* ── GtkApplication signal handlers ──────────────────────────────────── */

static void
on_app_startup (GApplication *app, gpointer user_data)
{
	static const GActionEntry app_actions[] = {
		{ "quit", menu_action_quit, NULL, NULL, NULL },
	};

	(void) user_data;

	/* One-time GUI setup — palette, pixmaps, CSS, key bindings */
	fe_init ();

	/* Register application-scoped actions. Quit lives here (not win.quit)
	 * so macOS's auto-provided Quit menu item (app.quit by convention)
	 * resolves correctly and isn't rendered disabled. */
	g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions,
	                                 G_N_ELEMENTS (app_actions), NULL);

	/* Install the application menubar. Each GtkApplicationWindow resolves
	 * win.* action references against its own action map. On macOS this
	 * renders in the system menu bar; elsewhere GtkApplicationWindow
	 * shows it at the top of the window (controlled via show-menubar). */
	gtk_application_set_menubar (GTK_APPLICATION (app), menu_get_menubar_model ());

	/* Check config dir writability (needs GTK for fe_message dialog) */
	if (g_access (get_xdir (), W_OK) != 0)
	{
		char buf[2048];

		g_snprintf (buf, sizeof (buf),
			_("You do not have write access to %s. Nothing from this session can be saved."),
			get_xdir ());
		fe_message (buf, FE_MSG_ERROR);
	}

#ifndef WIN32
	if (getuid () == 0)
		fe_message (_("* Running IRC as root is stupid! You should\n"
		              "  create a User Account and use that to login.\n"), FE_MSG_WARN|FE_MSG_WAIT);
#endif
}

static void
on_app_activate (GApplication *app, gpointer user_data)
{
	static gboolean activated = FALSE;
	(void) app; (void) user_data;

	/* Guard against re-entrance — activate can fire more than once */
	if (activated)
		return;
	activated = TRUE;

	xchat_init ();
}

/* ─────────────────────────────────────────────────────────────────────── */

int
fe_args (int argc, char *argv[])
{
	GError *error = NULL;
	GOptionContext *context;
	char *buffer;

#ifdef ENABLE_NLS
	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);
#endif

	context = g_option_context_new (NULL);
#ifdef WIN32
	g_option_context_set_help_enabled (context, FALSE);	/* disable stdout help as stdout is unavailable for subsystem:windows */
#endif
	g_option_context_add_main_entries (context, gopt_entries, GETTEXT_PACKAGE);
	g_option_context_parse (context, &argc, &argv, &error);

#ifdef WIN32
	if (error)											/* workaround for argv not being available when using subsystem:windows */
	{
		if (error->message)								/* the error message contains argv so search for patterns in that */
		{
			if (strstr (error->message, "--help-all") != NULL)
			{
				buffer = g_option_context_get_help (context, FALSE, NULL);
				gtk_init ();
				create_msg_dialog ("Long Help", buffer);
				g_free (buffer);
				return 0;
			}
			else if (strstr (error->message, "--help") != NULL || strstr (error->message, "-?") != NULL)
			{
				buffer = g_option_context_get_help (context, TRUE, NULL);
				gtk_init ();
				create_msg_dialog ("Help", buffer);
				g_free (buffer);
				return 0;
			}
			else
			{
				buffer = g_strdup_printf ("%s\n", error->message);
				gtk_init ();
				create_msg_dialog ("Error", buffer);
				g_free (buffer);
				return 1;
			}
		}
	}
#else
	if (error)
	{
		if (error->message)
			printf ("%s\n", error->message);
		return 1;
	}
#endif

	g_option_context_free (context);

	if (arg_show_version)
	{
		buffer = g_strdup_printf ("%s %s", PACKAGE_NAME, PACKAGE_VERSION);
#ifdef WIN32
		gtk_init ();
		create_msg_dialog ("Version Information", buffer);
#else
		puts (buffer);
#endif
		g_free (buffer);

		return 0;
	}

	if (arg_show_autoload)
	{
		buffer = g_strdup_printf ("%s%caddons%c", get_xdir(), G_DIR_SEPARATOR, G_DIR_SEPARATOR);
#ifdef WIN32
		gtk_init ();
		create_msg_dialog ("Plugin/Script Auto-load Directory", buffer);
#else
		puts (buffer);
#endif
		g_free (buffer);

		return 0;
	}

	if (arg_show_config)
	{
		buffer = g_strdup_printf ("%s%c", get_xdir(), G_DIR_SEPARATOR);
#ifdef WIN32
		gtk_init ();
		create_msg_dialog ("User Config Directory", buffer);
#else
		puts (buffer);
#endif
		g_free (buffer);

		return 0;
	}

#ifdef WIN32
	/* this is mainly for irc:// URL handling. When windows calls us from */
	/* I.E, it doesn't give an option of "Start in" directory, like short */
	/* cuts can. So we have to set the current dir manually, to the path  */
	/* of the exe. */
	{
		char *tmp = g_strdup (argv[0]);
		char *sl;

		sl = strrchr (tmp, G_DIR_SEPARATOR);
		if (sl)
		{
			*sl = 0;
			chdir (tmp);
		}
		g_free (tmp);
	}
#endif

	gtk_init ();

	/* Create GtkApplication — g_application_run() in fe_main() will
	 * handle registration, startup/activate signals, and the main loop.
	 * NON_UNIQUE: allow multiple instances (PoxChat tradition). */
	poxchat_app = gtk_application_new ("io.github.evilnet.PoxChat",
	                                   G_APPLICATION_NON_UNIQUE);
	g_signal_connect (poxchat_app, "startup",
	                  G_CALLBACK (on_app_startup), NULL);
	g_signal_connect (poxchat_app, "activate",
	                  G_CALLBACK (on_app_activate), NULL);

#ifdef HAVE_GTK_MAC
	osx_app = g_object_new(GTKOSX_TYPE_APPLICATION, NULL);
#endif

	return -1;
}

/* GTK3: Use CSS provider for input box styling instead of deprecated gtk_rc_parse_string */
static GtkCssProvider *input_css_provider = NULL;
static GtkCssProvider *tree_css_provider = NULL;

InputStyle *
create_input_style (InputStyle *style)
{
	char buf[256];
	char css_buf[2048];
	const char *font_family;
	int font_size;

	if (style == NULL)
	{
		style = g_new0 (InputStyle, 1);
	}
	else if (style->font_desc)
	{
		pango_font_description_free (style->font_desc);
	}

	style->font_desc = pango_font_description_from_string (prefs.hex_text_font);

	/* hex_text_font is the combined main+alternative font string. On startup
	 * the size in it may be stale (from a prior session's xtext merge).
	 * Override with the size from hex_text_font_main which is authoritative. */
	{
		PangoFontDescription *main_desc = pango_font_description_from_string (prefs.hex_text_font_main);
		int main_size = pango_font_description_get_size (main_desc);
		if (main_size > 0)
			pango_font_description_set_size (style->font_desc, main_size);
		pango_font_description_free (main_desc);
	}

	/* fall back */
	if (pango_font_description_get_size (style->font_desc) == 0)
	{
		g_snprintf (buf, sizeof (buf), _("Failed to open font:\n\n%s"), prefs.hex_text_font);
		fe_message (buf, FE_MSG_ERROR);
		pango_font_description_free (style->font_desc);
		style->font_desc = pango_font_description_from_string ("sans 11");
	}

	/* Create CSS provider once, reload CSS every time (font/colors may change) */
	if (!input_css_provider)
	{
		input_css_provider = gtk_css_provider_new ();
		gtk_style_context_add_provider_for_display (
			gdk_display_get_default (),
			GTK_STYLE_PROVIDER (input_css_provider),
			GTK_STYLE_PROVIDER_PRIORITY_USER);
	}

	if (prefs.hex_gui_input_style)
	{
		font_family = pango_font_description_get_family (style->font_desc);
		font_size = pango_font_description_get_size (style->font_desc) / PANGO_SCALE;

		g_snprintf (css_buf, sizeof (css_buf),
			/* Main input box — full theme (colors + font) */
			"#poxchat-inputbox { "
			"  caret-color: rgb(%d, %d, %d); "
			"  background: rgb(%d, %d, %d); "
			"  color: rgb(%d, %d, %d); "
			"  font-family: \"%s\"; "
			"  font-size: %dpt; "
			"  min-height: 0; "
			"  padding: 0; "
			"} "
			"#poxchat-inputbox selection { "
			"  background-color: rgb(%d, %d, %d); "
			"  color: rgb(%d, %d, %d); "
			"} "
			/* All other entries and spinbuttons — colors only, no font override */
			"entry, spinbutton { "
			"  background-color: rgb(%d, %d, %d); "
			"  color: rgb(%d, %d, %d); "
			"  caret-color: rgb(%d, %d, %d); "
			"} "
			"entry selection, spinbutton selection { "
			"  background-color: rgb(%d, %d, %d); "
			"  color: rgb(%d, %d, %d); "
			"}",
			/* #poxchat-inputbox foreground */
			(int)(colors[COL_FG].red * 255),
			(int)(colors[COL_FG].green * 255),
			(int)(colors[COL_FG].blue * 255),
			/* #poxchat-inputbox background */
			(int)(colors[COL_BG].red * 255),
			(int)(colors[COL_BG].green * 255),
			(int)(colors[COL_BG].blue * 255),
			/* #poxchat-inputbox color */
			(int)(colors[COL_FG].red * 255),
			(int)(colors[COL_FG].green * 255),
			(int)(colors[COL_FG].blue * 255),
			/* #poxchat-inputbox font */
			font_family ? font_family : "sans",
			font_size > 0 ? font_size : 11,
			/* #poxchat-inputbox selection */
			(int)(colors[COL_MARK_BG].red * 255),
			(int)(colors[COL_MARK_BG].green * 255),
			(int)(colors[COL_MARK_BG].blue * 255),
			(int)(colors[COL_MARK_FG].red * 255),
			(int)(colors[COL_MARK_FG].green * 255),
			(int)(colors[COL_MARK_FG].blue * 255),
			/* generic entry/spinbutton background */
			(int)(colors[COL_BG].red * 255),
			(int)(colors[COL_BG].green * 255),
			(int)(colors[COL_BG].blue * 255),
			/* generic entry/spinbutton foreground */
			(int)(colors[COL_FG].red * 255),
			(int)(colors[COL_FG].green * 255),
			(int)(colors[COL_FG].blue * 255),
			/* generic entry/spinbutton caret */
			(int)(colors[COL_FG].red * 255),
			(int)(colors[COL_FG].green * 255),
			(int)(colors[COL_FG].blue * 255),
			/* generic entry/spinbutton selection */
			(int)(colors[COL_MARK_BG].red * 255),
			(int)(colors[COL_MARK_BG].green * 255),
			(int)(colors[COL_MARK_BG].blue * 255),
			(int)(colors[COL_MARK_FG].red * 255),
			(int)(colors[COL_MARK_FG].green * 255),
			(int)(colors[COL_MARK_FG].blue * 255));
	}
	else
	{
		css_buf[0] = '\0';
	}

	gtk_css_provider_load_from_string (input_css_provider, css_buf);

	return style;
}

void
apply_tree_css (void)
{
	char css_buf[2048];
	char *font_family;
	int font_size;
	const char *font_style;
	const char *font_weight;

	if (!tree_css_provider)
	{
		tree_css_provider = gtk_css_provider_new ();
		gtk_style_context_add_provider_for_display (
			gdk_display_get_default (),
			GTK_STYLE_PROVIDER (tree_css_provider),
			GTK_STYLE_PROVIDER_PRIORITY_USER);
	}

	/* Extract font family from hex_text_font_main (not the combined string,
	 * which contains fallback fonts that CSS would treat as a single name).
	 * Size comes from input_style which already has the corrected value. */
	{
		PangoFontDescription *main_desc = pango_font_description_from_string (prefs.hex_text_font_main);
		font_family = g_strdup (pango_font_description_get_family (main_desc));
		font_size = pango_font_description_get_size (input_style->font_desc) / PANGO_SCALE;

		switch (pango_font_description_get_style (main_desc))
		{
		case PANGO_STYLE_ITALIC:  font_style = "italic"; break;
		case PANGO_STYLE_OBLIQUE: font_style = "oblique"; break;
		default:                  font_style = "normal"; break;
		}

		if (pango_font_description_get_weight (main_desc) >= PANGO_WEIGHT_BOLD)
			font_weight = "bold";
		else
			font_weight = "normal";

		pango_font_description_free (main_desc);
	}


	/* Apply theme colors to chanview tree, userlist, and all .poxchat-list views.
	 * ID selectors for chanview/userlist; class selector for dialog list views. */
	g_snprintf (css_buf, sizeof (css_buf),
		/* Chanview tree (GtkListView) */
		"#poxchat-tree { "
		"  border-radius: 6px; "
		"  border: 1px solid @borders; "
		"} "
		"#poxchat-tree, "
		"#poxchat-tree row { "
		"  background-color: rgb(%d, %d, %d); "
		"  color: rgb(%d, %d, %d); "
		"  font-family: \"%s\"; "
		"  font-size: %dpt; "
		"  font-style: %s; "
		"  font-weight: %s; "
		"} "
		"#poxchat-tree row:selected { "
		"  background-color: rgb(%d, %d, %d); "
		"} "
		"#poxchat-tree row:selected label { "
		"  color: rgb(%d, %d, %d); "
		"} "
		"#poxchat-tree treeexpander { "
		"  color: rgb(%d, %d, %d); "
		"} "
		/* Userlist (GtkColumnView) */
		"#poxchat-userlist { "
		"  border-radius: 6px; "
		"  border: 1px solid @borders; "
		"  margin-top: 2px; "
		"  margin-bottom: 6px; "
		"} "
		"#poxchat-userlist, "
		"#poxchat-userlist listview, "
		"#poxchat-userlist row { "
		"  background-color: rgb(%d, %d, %d); "
		"  color: rgb(%d, %d, %d); "
		"  font-family: \"%s\"; "
		"  font-size: %dpt; "
		"  font-style: %s; "
		"  font-weight: %s; "
		"} "
		"#poxchat-userlist row:selected { "
		"  background-color: rgb(%d, %d, %d); "
		"} "
		"#poxchat-userlist row:selected label { "
		"  color: rgb(%d, %d, %d); "
		"} "
		/* Dialog list/column views named poxchat-list */
		"#poxchat-list { "
		"  border-radius: 6px; "
		"  border: 1px solid @borders; "
		"} "
		"#poxchat-list, "
		"#poxchat-list listview, "
		"#poxchat-list row { "
		"  background-color: rgb(%d, %d, %d); "
		"  color: rgb(%d, %d, %d); "
		"} "
		"#poxchat-list row:selected { "
		"  background-color: rgb(%d, %d, %d); "
		"} "
		"#poxchat-list row:selected label { "
		"  color: rgb(%d, %d, %d); "
		"} "
		"#poxchat-list cell { "
		"  padding: 0; "
		"} "
		"#poxchat-list .favorite { "
		"  font-weight: 800; "
		"} "
		"#poxchat-list editablelabel { "
		"  padding: 4px 8px; "
		"} "
		"#poxchat-editable text { "
		"  background: rgb(%d, %d, %d); "
		"  color: rgb(%d, %d, %d); "
		"  caret-color: rgb(%d, %d, %d); "
		"  outline: 1px solid rgb(%d, %d, %d); "
		"  outline-offset: -1px; "
		"} "
		"#poxchat-editable text selection { "
		"  background-color: rgb(%d, %d, %d); "
		"  color: rgb(%d, %d, %d); "
		"}",
		/* #poxchat-tree bg */
		(int)(colors[COL_BG].red * 255),
		(int)(colors[COL_BG].green * 255),
		(int)(colors[COL_BG].blue * 255),
		/* #poxchat-tree fg */
		(int)(colors[COL_FG].red * 255),
		(int)(colors[COL_FG].green * 255),
		(int)(colors[COL_FG].blue * 255),
		/* #poxchat-tree font */
		font_family ? font_family : "sans",
		font_size > 0 ? font_size : 11,
		font_style,
		font_weight,
		/* #poxchat-tree row:selected bg */
		(int)(colors[COL_MARK_BG].red * 255),
		(int)(colors[COL_MARK_BG].green * 255),
		(int)(colors[COL_MARK_BG].blue * 255),
		/* #poxchat-tree row:selected label fg */
		(int)(colors[COL_MARK_FG].red * 255),
		(int)(colors[COL_MARK_FG].green * 255),
		(int)(colors[COL_MARK_FG].blue * 255),
		/* #poxchat-tree treeexpander fg */
		(int)(colors[COL_FG].red * 255),
		(int)(colors[COL_FG].green * 255),
		(int)(colors[COL_FG].blue * 255),
		/* #poxchat-userlist bg */
		(int)(colors[COL_BG].red * 255),
		(int)(colors[COL_BG].green * 255),
		(int)(colors[COL_BG].blue * 255),
		/* #poxchat-userlist fg */
		(int)(colors[COL_FG].red * 255),
		(int)(colors[COL_FG].green * 255),
		(int)(colors[COL_FG].blue * 255),
		/* #poxchat-userlist font */
		font_family ? font_family : "sans",
		font_size > 0 ? font_size : 11,
		font_style,
		font_weight,
		/* #poxchat-userlist row:selected bg */
		(int)(colors[COL_MARK_BG].red * 255),
		(int)(colors[COL_MARK_BG].green * 255),
		(int)(colors[COL_MARK_BG].blue * 255),
		/* #poxchat-userlist row:selected label fg */
		(int)(colors[COL_MARK_FG].red * 255),
		(int)(colors[COL_MARK_FG].green * 255),
		(int)(colors[COL_MARK_FG].blue * 255),
		/* #poxchat-list bg */
		(int)(colors[COL_BG].red * 255),
		(int)(colors[COL_BG].green * 255),
		(int)(colors[COL_BG].blue * 255),
		/* #poxchat-list fg */
		(int)(colors[COL_FG].red * 255),
		(int)(colors[COL_FG].green * 255),
		(int)(colors[COL_FG].blue * 255),
		/* #poxchat-list row:selected bg */
		(int)(colors[COL_MARK_BG].red * 255),
		(int)(colors[COL_MARK_BG].green * 255),
		(int)(colors[COL_MARK_BG].blue * 255),
		/* #poxchat-list row:selected label fg */
		(int)(colors[COL_MARK_FG].red * 255),
		(int)(colors[COL_MARK_FG].green * 255),
		(int)(colors[COL_MARK_FG].blue * 255),
		/* #poxchat-list editablelabel text bg */
		(int)(colors[COL_BG].red * 255),
		(int)(colors[COL_BG].green * 255),
		(int)(colors[COL_BG].blue * 255),
		/* #poxchat-list editablelabel text fg */
		(int)(colors[COL_FG].red * 255),
		(int)(colors[COL_FG].green * 255),
		(int)(colors[COL_FG].blue * 255),
		/* #poxchat-list editablelabel text caret */
		(int)(colors[COL_FG].red * 255),
		(int)(colors[COL_FG].green * 255),
		(int)(colors[COL_FG].blue * 255),
		/* #poxchat-editable text outline */
		(int)(colors[COL_MARK_FG].red * 255),
		(int)(colors[COL_MARK_FG].green * 255),
		(int)(colors[COL_MARK_FG].blue * 255),
		/* #poxchat-editable text selection bg */
		(int)(colors[COL_MARK_BG].red * 255),
		(int)(colors[COL_MARK_BG].green * 255),
		(int)(colors[COL_MARK_BG].blue * 255),
		/* #poxchat-editable text selection fg */
		(int)(colors[COL_MARK_FG].red * 255),
		(int)(colors[COL_MARK_FG].green * 255),
		(int)(colors[COL_MARK_FG].blue * 255));

	/* Compact mode: add row padding only when NOT compact */
	if (prefs.hex_gui_compact)
	{
		g_strlcat (css_buf,
			"#poxchat-userlist row, "
			"#poxchat-userlist row cell, "
			"#poxchat-tree row, "
			"#poxchat-tree row cell { "
			"  padding: 0; "
			"  min-height: 0; "
			"}", sizeof (css_buf));
	}
	else
	{
		g_strlcat (css_buf,
			"#poxchat-userlist row, "
			"#poxchat-userlist row cell, "
			"#poxchat-tree row, "
			"#poxchat-tree row cell { "
			"  padding-top: 1px; "
			"  padding-bottom: 1px; "
			"}", sizeof (css_buf));
	}

	gtk_css_provider_load_from_string (tree_css_provider, css_buf);
	g_free (font_family);
}

void
fe_init (void)
{
	palette_load ();
	key_init ();
	pixmaps_init ();

#ifdef HAVE_GTK_MAC
	gtkosx_application_set_dock_icon_pixbuf (osx_app, pix_poxchat);
#endif
	channelwin_pix = pixmap_load_from_file (prefs.hex_text_background);

	/* Merge hex_text_font_main + hex_text_font_alternative into hex_text_font
	 * so all font consumers see the correct combined string on startup.
	 * This also runs in setup_apply() when preferences change. */
	if (prefs.hex_text_font_main[0])
	{
		PangoFontDescription *old_desc = pango_font_description_from_string (prefs.hex_text_font_main);
		char buffer[4 * FONTNAMELEN + 1];
		sprintf (buffer, "%s,%s", pango_font_description_get_family (old_desc), prefs.hex_text_font_alternative);
		PangoFontDescription *new_desc = pango_font_description_from_string (buffer);
		pango_font_description_set_weight (new_desc, pango_font_description_get_weight (old_desc));
		pango_font_description_set_style (new_desc, pango_font_description_get_style (old_desc));
		pango_font_description_set_size (new_desc, pango_font_description_get_size (old_desc));
		sprintf (prefs.hex_text_font, "%s", pango_font_description_to_string (new_desc));
		pango_font_description_free (old_desc);
		pango_font_description_free (new_desc);
	}

	input_style = create_input_style (NULL);
	apply_tree_css ();

	/* GTK4: Apply CSS for various UI adjustments */
	{
		static GtkCssProvider *layout_css = NULL;
		if (!layout_css)
		{
			layout_css = gtk_css_provider_new ();
			gtk_css_provider_load_from_string (layout_css,
				/* Handle reserves the 12px junction gap via its own margin (6
				 * each side). Content on handle-adjacent sides carries 0 so
				 * the cursor-change region matches the click region.
				 * The .collapsed class (set by mg_paned_update_collapsed when
				 * a child hits 0 size) halves the margin: keeps a 3px hit
				 * zone on each side of the handle so the user can still grab
				 * and uncollapse, while preserving the window-edge margin. */
				"paned.horizontal > separator { min-width: 0; min-height: 0; background: none; margin: 0 6px; padding: 0; } "
				"paned.vertical > separator { min-width: 0; min-height: 0; background: none; margin: 6px 0; padding: 0; } "
				"paned.collapsed.horizontal > separator { margin: 0 3px; } "
				"paned.collapsed.vertical > separator { margin: 3px 0; } "
				/* Nick button — let input box drive the row height */
				"#poxchat-nickbutton { min-height: 0; padding-top: 0; padding-bottom: 0; } "
				"#poxchat-emojibtn { min-height: 0; padding-top: 0; padding-bottom: 0; } "
				"#poxchat-emojibtn > button { min-height: 0; padding-top: 0; padding-bottom: 0; } "
				/* GtkStack (used as page container) styling */
				"stack { padding: 0; margin: 0; } "
				/* Mode buttons in topic bar - compact padding */
				".poxchat-modebutton { min-height: 0; padding: 0 0; padding-top: 0; padding-bottom: 0; } "
				/* Horizontal rule between friends and non-friends in the
				 * userlist. Every row is tagged .poxchat-friend or
				 * .poxchat-nonfriend in the bind callback; the CSS
				 * adjacent-sibling selector below draws the rule at every
				 * friend→non-friend transition without needing C to
				 * compute neighbor state per bind. */
				"row.poxchat-friend + row.poxchat-nonfriend { "
				"  border-top: 1px solid alpha(currentColor, 0.4); "
				"} "
				/* Layout-swap drop indicators: two transparent strips
				 * overlaid on each vpane, highlighted when the drop
				 * cursor is hovering over their half. */
				".poxchat-drop-indicator { "
				"  min-height: 48px; "
				"  background-color: transparent; "
				"  transition: background-color 120ms ease; "
				"} "
				".poxchat-drop-indicator.poxchat-drop-hover { "
				"  background-color: alpha(@theme_selected_bg_color, 0.35); "
				"} "
				/* Tabs-mode drop strips at the very top and bottom of the
				 * window. Hidden via visible=FALSE by default; revealed
				 * during a chanview drag. Subtle default tint so the user
				 * can see where to aim; stronger tint on hover. */
				".poxchat-drop-strip { "
				"  min-height: 24px; "
				"  background-color: alpha(@theme_selected_bg_color, 0.12); "
				"  transition: background-color 120ms ease; "
				"} "
				".poxchat-drop-strip.poxchat-drop-hover { "
				"  background-color: alpha(@theme_selected_bg_color, 0.45); "
				"} "
				/* Channel tabs - reduce horizontal padding for compact appearance */
				"#poxchat-tab { "
				"  padding: 2px 4px; "
				"} "
				/* HexInputEdit — entry-like appearance without Adwaita min-height */
			"hexinput { "
			"  border: 1px solid @borders; "
			"  border-radius: 6px; "
			"  padding: 0; "
			"  background-color: @theme_base_color; "
			"  transition: all 200ms ease; "
			"} "
			"hexinput:focus-within { "
			"  outline: 2px solid @theme_selected_bg_color; "
			"  outline-offset: -2px; "
			"} "
			/* Userlist buttons - minimal padding for shrinkable panel */
				".poxchat-userlistbutton { "
				"  padding: 1px 2px; "
				"  margin: 0; "
				"  min-width: 0; "
				"  min-height: 0; "
				"}"
				"progressbar trough { min-width: 1px; }");
			gtk_style_context_add_provider_for_display (
				gdk_display_get_default (),
				GTK_STYLE_PROVIDER (layout_css),
				GTK_STYLE_PROVIDER_PRIORITY_USER);
		}

		/* Compact mode row padding is now in apply_tree_css() for live updates */
	}
}

#ifdef HAVE_GTK_MAC
static void
gtkosx_application_terminate (GtkosxApplication *app, gpointer userdata)
{
	poxchat_exit();
}
#endif

void
fe_main (void)
{
#ifdef HAVE_GTK_MAC
	gtkosx_application_ready(osx_app);
	g_signal_connect (G_OBJECT(osx_app), "NSApplicationWillTerminate",
					G_CALLBACK(gtkosx_application_terminate), NULL);
#endif
	g_application_run (G_APPLICATION (poxchat_app), 0, NULL);

	/* sleep for 2 seconds so any QUIT messages are not lost. The  */
	/* GUI is closed at this point, so the user doesn't even know! */
	if (prefs.wait_on_exit)
		sleep (2);
}

void
fe_cleanup (void)
{
	/* it's saved when pressing OK in setup.c */
	/*palette_save ();*/
}

void
fe_exit (void)
{
	if (poxchat_app)
		g_application_quit (G_APPLICATION (poxchat_app));
}

int
fe_timeout_add (int interval, void *callback, void *userdata)
{
	return g_timeout_add (interval, (GSourceFunc) callback, userdata);
}

int
fe_timeout_add_seconds (int interval, void *callback, void *userdata)
{
	return g_timeout_add_seconds (interval, (GSourceFunc) callback, userdata);
}

void
fe_timeout_remove (int tag)
{
	g_source_remove (tag);
}

#ifdef WIN32

static void
log_handler (const gchar   *log_domain,
		       GLogLevelFlags log_level,
		       const gchar   *message,
		       gpointer	      unused_data)
{
	session *sess;

	/* if (getenv ("POXCHAT_WARNING_IGNORE")) this gets ignored sometimes, so simply just disable all warnings */
		return;

	sess = find_dialog (serv_list->data, "(warnings)");
	if (!sess)
		sess = new_ircwindow (serv_list->data, "(warnings)", SESS_DIALOG, 0);

	PrintTextf (sess, "%s\t%s\n", log_domain, message);
	if (getenv ("POXCHAT_WARNING_ABORT"))
		abort ();
}

#endif

/* install tray stuff */

static int
fe_idle (gpointer data)
{
	session *sess = sess_list->data;

	plugin_add (sess, NULL, NULL, notification_plugin_init, notification_plugin_deinit, NULL, FALSE);

	plugin_add (sess, NULL, NULL, tray_plugin_init, tray_plugin_deinit, NULL, FALSE);

	if (arg_minimize == 1)
		gtk_window_minimize (GTK_WINDOW (sess->gui->window));
	else if (arg_minimize == 2)
		tray_toggle_visibility (FALSE);

	return 0;
}

void
fe_new_window (session *sess, int focus)
{
	int tab = FALSE;

	if (sess->type == SESS_DIALOG)
	{
		if (prefs.hex_gui_tab_dialogs)
			tab = TRUE;
	} else
	{
		if (prefs.hex_gui_tab_chans)
			tab = TRUE;
	}

	mg_changui_new (sess, NULL, tab, focus);

#ifdef WIN32
	g_log_set_handler ("GLib", G_LOG_LEVEL_CRITICAL|G_LOG_LEVEL_WARNING, (GLogFunc)log_handler, 0);
	g_log_set_handler ("GLib-GObject", G_LOG_LEVEL_CRITICAL|G_LOG_LEVEL_WARNING, (GLogFunc)log_handler, 0);
	g_log_set_handler ("Gdk", G_LOG_LEVEL_CRITICAL|G_LOG_LEVEL_WARNING, (GLogFunc)log_handler, 0);
	g_log_set_handler ("Gtk", G_LOG_LEVEL_CRITICAL|G_LOG_LEVEL_WARNING, (GLogFunc)log_handler, 0);
#endif

	if (!sess_list->next)
		g_idle_add (fe_idle, NULL);

	sess->scrollback_replay_marklast = gtk_xtext_set_marker_last;
}

void
fe_new_server (struct server *serv)
{
	serv->gui = g_new0 (struct server_gui, 1);
}

static void
fe_message_wait_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	gboolean *done = (gboolean *)user_data;
	gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), result, NULL);
	if (done)
		*done = TRUE;
}

void
fe_message (char *msg, int flags)
{
	GtkAlertDialog *dialog;
	gboolean done = FALSE;

	dialog = gtk_alert_dialog_new ("%s", msg);

	if (flags & FE_MSG_WAIT)
	{
		gtk_alert_dialog_set_modal (dialog, TRUE);
		gtk_alert_dialog_choose (dialog,
			parent_window ? GTK_WINDOW (parent_window) : NULL,
			NULL, fe_message_wait_cb, &done);
		g_object_unref (dialog);
		while (!done)
			g_main_context_iteration (NULL, TRUE);
	}
	else
	{
		gtk_alert_dialog_show (dialog,
			parent_window ? GTK_WINDOW (parent_window) : NULL);
		g_object_unref (dialog);
	}
}

void
fe_idle_add (void *func, void *data)
{
	g_idle_add (func, data);
}

void
fe_input_remove (int tag)
{
	g_source_remove (tag);
}

int
fe_input_add (int sok, int flags, void *func, void *data)
{
	int tag, type = 0;
	GIOChannel *channel;

#ifdef WIN32
	if (flags & FIA_FD)
		channel = g_io_channel_win32_new_fd (sok);
	else
		channel = g_io_channel_win32_new_socket (sok);
#else
	channel = g_io_channel_unix_new (sok);
#endif

	if (flags & FIA_READ)
		type |= G_IO_IN | G_IO_HUP | G_IO_ERR;
	if (flags & FIA_WRITE)
		type |= G_IO_OUT | G_IO_ERR;
	if (flags & FIA_EX)
		type |= G_IO_PRI;

	tag = g_io_add_watch (channel, type, (GIOFunc) func, data);
	g_io_channel_unref (channel);

	return tag;
}

void
fe_set_topic (session *sess, char *topic, char *stripped_topic)
{
	if (!sess->gui->is_tab || sess == current_tab)
	{
		if (prefs.hex_text_stripcolor_topic)
		{
			hc_entry_set_text (sess->gui->topic_entry, stripped_topic);
		}
		else
		{
			hc_entry_set_text (sess->gui->topic_entry, topic);
		}
		mg_set_topic_tip (sess);
	}
	else
	{
		g_free (sess->res->topic_text);

		if (prefs.hex_text_stripcolor_topic)
		{
			sess->res->topic_text = g_strdup (stripped_topic);
		}
		else
		{
			sess->res->topic_text = g_strdup (topic);
		}
	}
}

static void
fe_update_mode_entry (session *sess, GtkWidget *entry, char **text, char *new_text)
{
	if (!sess->gui->is_tab || sess == current_tab)
	{
		if (sess->gui->flag_wid[0])	/* channel mode buttons enabled? */
			hc_entry_set_text (entry, new_text);
	} else
	{
		if (sess->gui->is_tab)
		{
			g_free (*text);
			*text = g_strdup (new_text);
		}
	}
}

void
fe_update_channel_key (struct session *sess)
{
	fe_update_mode_entry (sess, sess->gui->key_entry,
								 &sess->res->key_text, sess->channelkey);
	fe_set_title (sess);
}

void
fe_update_channel_limit (struct session *sess)
{
	char tmp[16];

	sprintf (tmp, "%d", sess->limit);
	fe_update_mode_entry (sess, sess->gui->limit_entry,
								 &sess->res->limit_text, tmp);
	fe_set_title (sess);
}

int
fe_is_chanwindow (struct server *serv)
{
	if (!serv->gui->chanlist_window)
		return 0;
	return 1;
}

void
fe_notify_update (char *name)
{
	if (!name)
		notify_gui_update ();
}

static guint fe_notify_friends_idle_id = 0;

static gboolean
fe_notify_friends_changed_idle (gpointer user_data)
{
	GSList *list;

	(void) user_data;
	fe_notify_friends_idle_id = 0;

	/* Two updates per view:
	 *   1. Kick the sorter so friend-first ordering re-runs.
	 *   2. Re-apply .poxchat-friend / .poxchat-nonfriend on every
	 *      bound row — GtkListView optimizes away bind-cb calls when
	 *      an item just moves positions, so rows whose underlying
	 *      user flipped friend-ness (e.g. account-observed) keep
	 *      their stale class otherwise.
	 *
	 * Previously this did a full column_view model null-and-restore
	 * to force rebinds. That caused GtkColumnView to re-measure its
	 * internal layout, which (combined with the paned min-size
	 * coupling) manifested as the host column hiding and reshowing
	 * multiple times during a WHO burst. Refreshing classes directly
	 * keeps the ColumnView size allocation stable — no re-measure. */
	for (list = sess_list; list; list = list->next)
	{
		session *sess = list->data;
		GtkWidget *view;
		GtkSorter *sorter;

		if (!sess->gui || !sess->gui->user_tree)
			continue;
		view = sess->gui->user_tree;

		sorter = g_object_get_data (G_OBJECT (view), "hc-userlist-sorter");
		if (sorter)
			gtk_sorter_changed (sorter, GTK_SORTER_CHANGE_DIFFERENT);

		userlist_refresh_friend_classes (view);
	}

	return G_SOURCE_REMOVE;
}

void
fe_notify_friends_changed (void)
{
	/* Coalesce bursts. During a WHO response, notify_account_observed
	 * fires once per friend in the channel. The full sorter-kick +
	 * model null-restore is expensive — each round rebinds every row
	 * and triggers mg_update_userlist_columns, which shows up to the
	 * user as the host column flashing hide/show through the burst.
	 * Collapse all calls within one mainloop iteration into a single
	 * refresh that runs after the burst settles. */
	if (fe_notify_friends_idle_id != 0)
		return;
	fe_notify_friends_idle_id =
		g_idle_add (fe_notify_friends_changed_idle, NULL);
}

void
fe_text_clear (struct session *sess, int lines)
{
	gtk_xtext_clear (sess->res->buffer, lines);
}

void
fe_close_window (struct session *sess)
{
	if (sess->gui->is_tab)
		mg_tab_close (sess);
	else
		hc_window_destroy_fn (GTK_WINDOW (sess->gui->window));
}

void
fe_progressbar_start (session *sess)
{
	if (!sess->gui->is_tab || current_tab == sess)
	/* if it's the focused tab, create it for real! */
		mg_progressbar_create (sess->gui);
	else
	/* otherwise just remember to create on when it gets focused */
		sess->res->c_graph = TRUE;
}

void
fe_progressbar_end (server *serv)
{
	GSList *list = sess_list;
	session *sess;

	while (list)				  /* check all windows that use this server and  *
									   * remove the connecting graph, if it has one. */
	{
		sess = list->data;
		if (sess->server == serv)
		{
			if (sess->gui->bar)
				mg_progressbar_destroy (sess->gui);
			sess->res->c_graph = FALSE;
		}
		list = list->next;
	}
}

void
fe_print_text (struct session *sess, char *text, time_t stamp,
			   gboolean no_activity)
{
	textentry *first_new_entry;
	textentry *new_entry;

	/* IRCv3 modernization: check if we should prepend (Phase 3)
	 * For CHATHISTORY BEFORE requests processed in reverse order,
	 * we prepend each message to maintain chronological order.
	 */
	if (sess->history_request_is_before && sess->history_prepend_mode)
	{
		if (!gtk_xtext_virt_should_materialize (sess->res->buffer, stamp, LINK_HEAD))
		{
			((xtext_buffer *)sess->res->buffer)->pending_db_rowid = 0;
			return;
		}

		/* For prepend, track the entry at the head before we add */
		first_new_entry = gtk_xtext_buffer_get_first (sess->res->buffer);

		PrintTextRawPrepend (sess->res->buffer, (unsigned char *)text, prefs.hex_text_indent, stamp);

		/* After prepend, find the new first entry */
		new_entry = gtk_xtext_buffer_get_first (sess->res->buffer);

		/* Associate msgid with the new entry if it's different from old first */
		if (sess->current_msgid && new_entry && new_entry != first_new_entry)
		{
			gtk_xtext_set_msgid (sess->res->buffer, new_entry, sess->current_msgid);
		}

		/* Don't update tab colors for historical messages */
		return;
	}

	/* IRCv3 modernization: check if we should insert sorted (Phase 3)
	 * For CHATHISTORY AFTER requests, we insert at the correct timestamp position.
	 * This places catch-up messages between scrollback and the join banner.
	 */
	if (sess->history_insert_sorted_mode)
	{
		if (!gtk_xtext_virt_should_materialize (sess->res->buffer, stamp, LINK_BEFORE))
		{
			((xtext_buffer *)sess->res->buffer)->pending_db_rowid = 0;
			return;
		}

		PrintTextRawInsertSorted (sess->res->buffer, (unsigned char *)text, prefs.hex_text_indent, stamp);

		/* Associate msgid with the newly inserted entry.  It may have been
		 * inserted anywhere (before or after old_last) depending on its
		 * timestamp.  Walk backwards from the tail; the first entry
		 * without a msgid is the one we just inserted. */
		if (sess->current_msgid)
		{
			textentry *ent;
			for (ent = gtk_xtext_buffer_get_last (sess->res->buffer); ent;
			     ent = gtk_xtext_entry_get_prev (ent))
			{
				if (!gtk_xtext_get_msgid (ent))
				{
					gtk_xtext_set_msgid (sess->res->buffer, ent, sess->current_msgid);
					break;
				}
			}
		}

		/* Don't update tab colors for historical messages */
		return;
	}

	/* Normal append path */

	/* Virtual scrollback: if user is scrolled up and the materialization
	 * window is full, skip materializing this entry.  It's already in
	 * the DB (saved by PrintTextTimeStamp above); ensure_range will load
	 * it when the user scrolls down. */
	if (!gtk_xtext_virt_should_materialize (sess->res->buffer, stamp, LINK_TAIL))
	{
		((xtext_buffer *)sess->res->buffer)->pending_db_rowid = 0;
		goto skip_materialize;
	}

	/* IRCv3 modernization: track first entry for msgid association (Phase 1)
	 * Important: A single IRC message may create multiple xtext entries
	 * (e.g., multiline batches, text with embedded newlines). We associate
	 * the msgid with the FIRST entry, which is the logical "start" of the message.
	 */
	first_new_entry = gtk_xtext_buffer_get_last (sess->res->buffer);  /* Entry before our new ones */

	PrintTextRaw (sess->res->buffer, (unsigned char *)text, prefs.hex_text_indent, stamp);

	/* Find the first entry we just created */
	if (first_new_entry)
		first_new_entry = gtk_xtext_entry_get_next (first_new_entry);  /* First new entry is after the old last */
	else
		first_new_entry = gtk_xtext_buffer_get_first (sess->res->buffer);  /* Buffer was empty */

	/* Associate msgid with the first entry of this message */
	if (sess->current_msgid && first_new_entry)
	{
		gtk_xtext_set_msgid (sess->res->buffer, first_new_entry,
		                     sess->current_msgid);
	}

skip_materialize:
	if (no_activity || !sess->gui->is_tab)
		return;

	if (sess == current_tab)
		fe_set_tab_color (sess, FE_COLOR_NONE);
	else if (sess->tab_state & TAB_STATE_NEW_HILIGHT)
		fe_set_tab_color (sess, FE_COLOR_NEW_HILIGHT);
	else if (sess->tab_state & TAB_STATE_NEW_MSG)
		fe_set_tab_color (sess, FE_COLOR_NEW_MSG);
	else
		fe_set_tab_color (sess, FE_COLOR_NEW_DATA);
}

/* IRCv3 modernization: prepend text for chathistory BEFORE requests (Phase 3)
 * Inserts at head of buffer to maintain chronological order.
 * For BEFORE requests, messages come oldest-to-newest - we process in reverse
 * and prepend each, resulting in oldest at top.
 */
void
fe_print_text_prepend (struct session *sess, char *text, time_t stamp)
{
	textentry *first_new_entry;

	/* For prepend, the "first" entry will be at the head after prepending */
	first_new_entry = gtk_xtext_buffer_get_first (sess->res->buffer);

	PrintTextRawPrepend (sess->res->buffer, (unsigned char *)text, prefs.hex_text_indent, stamp);

	/* After prepend, the new first entry is at the head */
	textentry *new_first = gtk_xtext_buffer_get_first (sess->res->buffer);

	/* Associate msgid with the new entry if different from old first */
	if (sess->current_msgid && new_first && new_first != first_new_entry)
	{
		gtk_xtext_set_msgid (sess->res->buffer, new_first, sess->current_msgid);
	}

	/* Don't update tab colors for historical messages - they're not "new" activity */
}

void
fe_redact_message (session *sess, const char *msgid,
                   const char *redacted_by, const char *reason,
                   time_t redact_time)
{
	textentry *ent;
	xtext_buffer *buf = sess->res->buffer;

	ent = gtk_xtext_find_by_msgid (buf, msgid);
	if (!ent)
	{
		/* Original message not loaded — insert a notice at the correct
		 * chronological position (not appended at the end). */
		char *notice;
		textentry *notice_ent;
		if (reason && *reason)
			notice = g_strdup_printf ("\017[Message redacted by %s: %s]",
			                          redacted_by, reason);
		else
			notice = g_strdup_printf ("\017[Message redacted by %s]",
			                          redacted_by);
		notice_ent = gtk_xtext_insert_sorted_indent (buf,
		                                (unsigned char *)"*", 1,
		                                (unsigned char *)notice, -1,
		                                redact_time);
		g_free (notice);
		/* Set reply context so it visually references the target message */
		if (notice_ent)
			gtk_xtext_entry_set_reply (buf, notice_ent, msgid, NULL, NULL, 0);
		return;
	}
	if (gtk_xtext_entry_get_state (ent) == XTEXT_STATE_REDACTED)
		return;

	/* Preserve original content for accountability */
	gtk_xtext_entry_set_redaction_info (buf, ent,
		(const char *)gtk_xtext_entry_get_str (ent),
		gtk_xtext_entry_get_str_len (ent),
		redacted_by, reason, redact_time);

	/* Build replacement: preserve nick prefix, replace message body */
	{
		int left_len = gtk_xtext_entry_get_left_len (ent);
		char *placeholder;
		unsigned char *new_str;
		int new_len;

		if (reason && *reason)
			placeholder = g_strdup_printf ("\017[Message deleted by %s: %s]",
			                               redacted_by, reason);
		else
			placeholder = g_strdup_printf ("\017[Message deleted by %s]",
			                               redacted_by);

		if (left_len >= 0)
		{
			/* Indented: preserve left portion (nick + separator) */
			const unsigned char *old_str = gtk_xtext_entry_get_str (ent);
			int plen = strlen (placeholder);
			new_len = left_len + 1 + plen;
			new_str = g_malloc (new_len + 1);
			memcpy (new_str, old_str, left_len + 1);
			memcpy (new_str + left_len + 1, placeholder, plen);
			new_str[new_len] = '\0';
		}
		else
		{
			new_str = (unsigned char *)g_strdup (placeholder);
			new_len = strlen (placeholder);
		}

		g_free (placeholder);
		gtk_xtext_entry_set_text (buf, ent, new_str, new_len);
		g_free (new_str);
	}

	gtk_xtext_entry_set_state (buf, ent, XTEXT_STATE_REDACTED);
}

guint64
fe_get_last_entry_id (session *sess)
{
	textentry *ent = gtk_xtext_buffer_get_last (sess->res->buffer);
	return ent ? gtk_xtext_get_entry_id (ent) : 0;
}

const char *
fe_get_last_msgid (session *sess)
{
	textentry *ent = gtk_xtext_buffer_get_last (sess->res->buffer);
	return ent ? gtk_xtext_get_msgid (ent) : NULL;
}

const char *
fe_get_last_nonself_msgid (session *sess, char *nick_out, int nick_out_size)
{
	textentry *ent;
	const char *msgid;

	/* Walk backwards from the last entry to find a non-self message with a msgid */
	for (ent = gtk_xtext_buffer_get_last (sess->res->buffer);
	     ent != NULL;
	     ent = gtk_xtext_entry_get_prev (ent))
	{
		int left_len;

		msgid = gtk_xtext_get_msgid (ent);
		if (!msgid)
			continue;

		left_len = gtk_xtext_entry_get_left_len (ent);

		/* Extract nick from the left portion of str (before the tab separator).
		 * The nick is embedded in format codes, so strip them. */
		if (left_len > 0 && sess->server)
		{
			const unsigned char *str = gtk_xtext_entry_get_str (ent);
			char *nick_raw = g_strndup ((const char *)str, left_len);
			char *nick_clean = strip_color (nick_raw, -1, STRIP_ALL);
			g_free (nick_raw);

			/* Skip if this is our own message */
			if (sess->server->p_cmp (nick_clean, sess->server->nick) == 0)
			{
				g_free (nick_clean);
				continue;
			}

			if (nick_out && nick_out_size > 0)
				g_strlcpy (nick_out, nick_clean, nick_out_size);

			g_free (nick_clean);
		}

		return msgid;
	}

	return NULL;
}

void
fe_set_entry_pending (session *sess, guint64 entry_id)
{
	textentry *ent = gtk_xtext_find_by_id (sess->res->buffer, entry_id);
	if (ent)
		gtk_xtext_entry_set_state (sess->res->buffer, ent, XTEXT_STATE_PENDING);
}

void
fe_confirm_entry (session *sess, guint64 entry_id, const char *msgid)
{
	textentry *ent = gtk_xtext_find_by_id (sess->res->buffer, entry_id);
	if (ent && gtk_xtext_entry_get_state (ent) == XTEXT_STATE_PENDING)
	{
		gtk_xtext_entry_set_state (sess->res->buffer, ent, XTEXT_STATE_NORMAL);
		if (msgid)
			gtk_xtext_set_msgid (sess->res->buffer, ent, msgid);
	}
	/* Virtual scrollback: if the entry was evicted (find_by_id returns NULL),
	 * the DB-side msgid update is already handled by scrollback_confirm_pending
	 * in inbound.c.  When the entry is rematerialized from the DB later, it
	 * will have the correct msgid and render as NORMAL (PENDING is transient
	 * and not persisted to the DB). */
}

void
fe_clear_all_pending (session *sess)
{
	xtext_buffer *buf = sess->res->buffer;
	textentry *ent;

	for (ent = gtk_xtext_buffer_get_first (buf); ent;
	     ent = gtk_xtext_entry_get_next (ent))
	{
		if (gtk_xtext_entry_get_state (ent) == XTEXT_STATE_PENDING)
			gtk_xtext_entry_set_state (buf, ent, XTEXT_STATE_NORMAL);
	}
}

time_t
fe_get_newest_stamp (session *sess)
{
	xtext_buffer *buf;
	textentry *last;

	if (!sess->res || !sess->res->buffer)
		return 0;

	buf = sess->res->buffer;
	last = gtk_xtext_buffer_get_last (buf);
	if (!last)
		return 0;
	return gtk_xtext_entry_get_stamp (last);
}

void
fe_network_icon_ready (server *serv, const guint8 *data, gsize len)
{
	GSList *list;
	session *sess;
	GdkPixbufLoader *loader;
	GdkPixbuf *pixbuf, *scaled;
	int w, h;

	if (!data || len == 0)
		return;

	/* Decode raw image bytes into a pixbuf */
	loader = gdk_pixbuf_loader_new ();
	if (!gdk_pixbuf_loader_write (loader, data, len, NULL))
	{
		gdk_pixbuf_loader_close (loader, NULL);
		g_object_unref (loader);
		return;
	}
	gdk_pixbuf_loader_close (loader, NULL);

	pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
	if (!pixbuf)
	{
		g_object_unref (loader);
		return;
	}

	/* Reject unreasonably large source images */
	w = gdk_pixbuf_get_width (pixbuf);
	h = gdk_pixbuf_get_height (pixbuf);
	if (w > 256 || h > 256)
	{
		g_object_unref (loader);
		return;
	}

	/* Scale to tree icon size */
	if (w != NETWORK_ICON_SIZE || h != NETWORK_ICON_SIZE)
	{
		scaled = gdk_pixbuf_scale_simple (pixbuf, NETWORK_ICON_SIZE,
		                                   NETWORK_ICON_SIZE,
		                                   GDK_INTERP_BILINEAR);
		g_object_unref (loader);
		pixbuf = scaled;
	}
	else
	{
		g_object_ref (pixbuf);
		g_object_unref (loader);
	}

	if (!pixbuf)
		return;

	/* Store on server struct (replace previous) */
	if (serv->network_icon)
		g_object_unref (serv->network_icon);
	serv->network_icon = pixbuf;

	/* Update all server tabs for this connection */
	list = sess_list;
	while (list)
	{
		sess = list->data;
		if (sess->server == serv && sess->type == SESS_SERVER &&
		    sess->res && sess->res->tab)
		{
			chan_set_icon (sess->res->tab, pixbuf);
		}
		list = list->next;
	}
}

void
fe_typing_update (session *sess)
{
	GtkXText *xtext;

	if (!sess->gui || !sess->gui->xtext)
		return;
	if (sess->gui->is_tab && sess != current_tab)
		return;

	xtext = GTK_XTEXT (sess->gui->xtext);

	if (!sess->typing_nicks)
	{
		/* Don't yank the strip out from under the scrollback the moment
		 * typing stops — turn the item into a placeholder so the reserved
		 * space stays until the next appended line sweeps it away. */
		gtk_xtext_status_set_placeholder (xtext, "typing");
	}
	else
	{
		GString *str = g_string_new ("+typing: ");
		GSList *list;
		int first = 1;

		for (list = sess->typing_nicks; list; list = list->next)
		{
			typing_entry *entry = list->data;
			if (!first)
				g_string_append (str, ", ");
			g_string_append (str, entry->nick);
			first = 0;
		}

		gtk_xtext_status_set (xtext, "typing", str->str, 100, 0);
		g_string_free (str, TRUE);
	}
}

void
fe_status_update (session *sess, const char *key, const char *text,
                  int priority, int timeout_ms)
{
	GtkXText *xtext;

	if (!sess->gui || !sess->gui->xtext)
		return;
	if (sess->gui->is_tab && sess != current_tab)
		return;

	xtext = GTK_XTEXT (sess->gui->xtext);

	if (text)
		gtk_xtext_status_set (xtext, key, text, priority, timeout_ms);
	else
		gtk_xtext_status_remove (xtext, key);
}

void
fe_toast_show (session *sess, const char *text, int linger_ms, int type,
               unsigned int flags)
{
	GtkXText *xtext;

	if (!sess->gui || !sess->gui->xtext)
		return;
	if (sess->gui->is_tab && sess != current_tab)
		return;

	xtext = GTK_XTEXT (sess->gui->xtext);
	gtk_xtext_toast_show (xtext, text, linger_ms, (xtext_toast_type)type, flags);
}

void
fe_set_marker_from_timestamp (session *sess, time_t timestamp)
{
	xtext_buffer *buf;

	if (!sess->res || !sess->res->buffer)
		return;

	buf = sess->res->buffer;
	gtk_xtext_set_marker_from_timestamp (buf, timestamp);
}

void
fe_clear_server_read_marker (session *sess)
{
	if (sess->res && sess->res->buffer)
		((xtext_buffer *)sess->res->buffer)->server_read_marker = FALSE;
}

void
fe_beep (session *sess)
{
#ifdef WIN32
	/* Play the "Instant Message Notification" system sound
	 */
	if (!PlaySoundW (L"Notification.IM", NULL, SND_ALIAS | SND_ASYNC))
	{
		/* The user does not have the "Instant Message Notification" sound set. Fall back to system beep.
		 */
		Beep (1000, 50);
	}
#else
#ifdef USE_LIBCANBERRA
	if (ca_con == NULL)
	{
		ca_context_create (&ca_con);
		ca_context_change_props (ca_con,
										CA_PROP_APPLICATION_ID, "poxchat",
										CA_PROP_APPLICATION_NAME, DISPLAY_NAME,
										CA_PROP_APPLICATION_ICON_NAME, "poxchat", NULL);
	}

	ca_context_play (ca_con, 0, CA_PROP_EVENT_ID, "message-new-instant", NULL);
#endif
#endif
}

void
fe_lastlog (session *sess, session *lastlog_sess, char *sstr, gtk_xtext_search_flags flags)
{
	GError *err = NULL;
	xtext_buffer *buf, *lbuf;

	buf = sess->res->buffer;

	if (gtk_xtext_is_empty (buf))
	{
		PrintText (lastlog_sess, _("Search buffer is empty.\n"));
		return;
	}

	lbuf = lastlog_sess->res->buffer;
	if (flags & regexp)
	{
		GRegexCompileFlags gcf = (flags & case_match)? 0: G_REGEX_CASELESS;

		lbuf->search_re = g_regex_new (sstr, gcf, 0, &err);
		if (err)
		{
			PrintText (lastlog_sess, _(err->message));
			g_error_free (err);
			return;
		}
	}
	else
	{
		if (flags & case_match)
		{
			lbuf->search_nee = g_strdup (sstr);
		}
		else
		{
			lbuf->search_nee = g_utf8_casefold (sstr, strlen (sstr));
		}
		lbuf->search_lnee = strlen (lbuf->search_nee);
	}
	lbuf->search_flags = flags;
	lbuf->search_text = g_strdup (sstr);
	gtk_xtext_lastlog (lbuf, buf);
}

void
fe_set_lag (server *serv, long lag)
{
	GSList *list = sess_list;
	session *sess;
	gdouble per;
	char lagtext[64];
	char lagtip[128];
	unsigned long nowtim;

	if (lag == -1)
	{
		if (!serv->lag_sent)
			return;
		nowtim = make_ping_time ();
		lag = nowtim - serv->lag_sent;
	}

	/* if there is no pong for >30s report the lag as +30s */
	if (lag > 30000 && serv->lag_sent)
		lag=30000;

	per = ((double)lag) / 1000.0;
	if (per > 1.0)
		per = 1.0;

	g_snprintf (lagtext, sizeof (lagtext) - 1, "%s%ld.%lds",
			  serv->lag_sent ? "+" : "", lag / 1000, (lag/100) % 10);
	g_snprintf (lagtip, sizeof (lagtip) - 1, "Lag: %s%ld.%ld seconds",
				 serv->lag_sent ? "+" : "", lag / 1000, (lag/100) % 10);

	while (list)
	{
		sess = list->data;
		if (sess->server == serv)
		{
			g_free (sess->res->lag_tip);
			sess->res->lag_tip = g_strdup (lagtip);

			if (!sess->gui->is_tab || current_tab == sess)
			{
				if (sess->gui->lagometer)
				{
					gtk_progress_bar_set_fraction ((GtkProgressBar *) sess->gui->lagometer, per);
					gtk_widget_set_tooltip_text (gtk_widget_get_parent (sess->gui->lagometer), lagtip);
				}
				if (sess->gui->laginfo)
					gtk_label_set_text ((GtkLabel *) sess->gui->laginfo, lagtext);
			} else
			{
				sess->res->lag_value = per;
				g_free (sess->res->lag_text);
				sess->res->lag_text = g_strdup (lagtext);
			}
		}
		list = list->next;
	}
}

void
fe_set_throttle (server *serv)
{
	GSList *list = sess_list;
	struct session *sess;
	float per;
	char tbuf[96];
	char tip[160];

	per = (float) serv->sendq_len / 1024.0;
	if (per > 1.0)
		per = 1.0;

	while (list)
	{
		sess = list->data;
		if (sess->server == serv)
		{
			g_snprintf (tbuf, sizeof (tbuf) - 1, _("%d bytes"), serv->sendq_len);
			g_snprintf (tip, sizeof (tip) - 1, _("Network send queue: %d bytes"), serv->sendq_len);

			g_free (sess->res->queue_tip);
			sess->res->queue_tip = g_strdup (tip);

			if (!sess->gui->is_tab || current_tab == sess)
			{
				if (sess->gui->throttlemeter)
				{
					gtk_progress_bar_set_fraction ((GtkProgressBar *) sess->gui->throttlemeter, per);
					gtk_widget_set_tooltip_text (gtk_widget_get_parent (sess->gui->throttlemeter), tip);
				}
				if (sess->gui->throttleinfo)
					gtk_label_set_text ((GtkLabel *) sess->gui->throttleinfo, tbuf);
			} else
			{
				sess->res->queue_value = per;
				g_free (sess->res->queue_text);
				sess->res->queue_text = g_strdup (tbuf);
			}
		}
		list = list->next;
	}
}

void
fe_ctrl_gui (session *sess, fe_gui_action action, int arg)
{
	switch (action)
	{
	case FE_GUI_HIDE:
		gtk_widget_set_visible (sess->gui->window, FALSE); break;
	case FE_GUI_SHOW:
		gtk_window_present (GTK_WINDOW (sess->gui->window));
		break;
	case FE_GUI_FOCUS:
		mg_bring_tofront_sess (sess); break;
	case FE_GUI_FLASH:
		fe_flash_window (sess); break;
	case FE_GUI_COLOR:
		fe_set_tab_color (sess, arg); break;
	case FE_GUI_ICONIFY:
		gtk_window_minimize (GTK_WINDOW (sess->gui->window)); break;
	case FE_GUI_MENU:
		menu_bar_toggle ();	/* toggle menubar on/off */
		break;
	case FE_GUI_ATTACH:
		mg_detach (sess, arg);	/* arg: 0=toggle 1=detach 2=attach */
		break;
	case FE_GUI_APPLY:
		setup_apply_real (TRUE, TRUE, TRUE, FALSE);
	}
}

static void
dcc_saveas_cb (struct DCC *dcc, char *file)
{
	if (is_dcc (dcc))
	{
		if (dcc->dccstat == STAT_QUEUED)
		{
			if (file)
				dcc_get_with_destfile (dcc, file);
			else if (dcc->resume_sent == 0)
				dcc_abort (dcc->serv->front_session, dcc);
		}
	}
}

void
fe_confirm (const char *message, void (*yesproc)(void *), void (*noproc)(void *), void *ud)
{
	/* warning, assuming fe_confirm is used by DCC only! */
	struct DCC *dcc = ud;

	if (dcc->file)
	{
		char *filepath = g_build_filename (prefs.hex_dcc_dir, dcc->file, NULL);
		gtkutil_file_req (NULL, message, dcc_saveas_cb, ud, filepath, NULL,
								FRF_WRITE|FRF_NOASKOVERWRITE|FRF_FILTERISINITIAL);
		g_free (filepath);
	}
}

int
fe_gui_info (session *sess, int info_type)
{
	switch (info_type)
	{
	case 0:	/* window status */
		if (!gtk_widget_get_visible (GTK_WIDGET (sess->gui->window)))
		{
			return 2;	/* hidden (iconified or systray) */
		}

		if (gtk_window_is_active (GTK_WINDOW (sess->gui->window)))
		{
			return 1;	/* active/focused */
		}

		return 0;		/* normal (no keyboard focus or behind a window) */
	}

	return -1;
}

void *
fe_gui_info_ptr (session *sess, int info_type)
{
	switch (info_type)
	{
	case 0:	/* native window pointer (for plugins) */
#ifdef WIN32
		{
			GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (sess->gui->window));
			if (surface)
				return gdk_win32_surface_get_handle (surface);
			return NULL;
		}
#else
		return sess->gui->window;
#endif
		break;

	case 1:	/* GtkWindow * (for plugins) */
		return sess->gui->window;
	}
	return NULL;
}

char *
fe_get_inputbox_contents (session *sess)
{
	/* not the current tab */
	if (sess->res->input_text)
		return sess->res->input_text;

	/* current focused tab */
	return SPELL_ENTRY_GET_TEXT (sess->gui->input_box);
}

int
fe_get_inputbox_cursor (session *sess)
{
	/* not the current tab (we don't remember the cursor pos) */
	if (sess->res->input_text)
		return 0;

	/* current focused tab */
	return SPELL_ENTRY_GET_POS (sess->gui->input_box);
}

void
fe_set_inputbox_cursor (session *sess, int delta, int pos)
{
	if (!sess->gui->is_tab || sess == current_tab)
	{
		if (delta)
			pos += SPELL_ENTRY_GET_POS (sess->gui->input_box);
		SPELL_ENTRY_SET_POS (sess->gui->input_box, pos);
	} else
	{
		/* we don't support changing non-front tabs yet */
	}
}

void
fe_set_inputbox_contents (session *sess, char *text)
{
	if (!sess->gui->is_tab || sess == current_tab)
	{
		SPELL_ENTRY_SET_TEXT (sess->gui->input_box, text);
	} else
	{
		g_free (sess->res->input_text);
		sess->res->input_text = g_strdup (text);
	}
}

#ifdef __APPLE__
static char *
url_escape_hostname (const char *url)
{
    char *host_start, *host_end, *ret, *hostname;

    host_start = strstr (url, "://");
    if (host_start != NULL)
    {
        *host_start = '\0';
        host_start += 3;
        host_end = strchr (host_start, '/');

        if (host_end != NULL)
        {
            *host_end = '\0';
            host_end++;
        }

        hostname = g_hostname_to_ascii (host_start);
        if (host_end != NULL)
            ret = g_strdup_printf ("%s://%s/%s", url, hostname, host_end);
        else
            ret = g_strdup_printf ("%s://%s", url, hostname);

        g_free (hostname);
        return ret;
    }

    return g_strdup (url);
}

static void
osx_show_uri (const char *url)
{
    char *escaped_url, *encoded_url, *open, *cmd;

    escaped_url = url_escape_hostname (url);
    encoded_url = g_filename_from_utf8 (escaped_url, -1, NULL, NULL, NULL);
    if (encoded_url)
    {
        open = g_find_program_in_path ("open");
        cmd = g_strjoin (" ", open, encoded_url, NULL);

        poxchat_exec (cmd);

        g_free (encoded_url);
        g_free (cmd);
    }

    g_free (escaped_url);
}

#endif

static inline char *
escape_uri (const char *uri)
{
	return g_uri_escape_string(uri, G_URI_RESERVED_CHARS_GENERIC_DELIMITERS G_URI_RESERVED_CHARS_SUBCOMPONENT_DELIMITERS, FALSE);
}

static inline gboolean
uri_contains_forbidden_characters (const char *uri)
{
	while (*uri)
	{
		if (!g_ascii_isalnum (*uri) && !strchr ("-._~:/?#[]@!$&'()*+,;=", *uri))
			return TRUE;
		uri++;
	}

	return FALSE;
}

static char *
maybe_escape_uri (const char *uri)
{
	/* The only way to know if a string has already been escaped or not
	 * is by fulling parsing each segement but we can try some more simple heuristics. */

	/* If we find characters that should clearly be escaped. */
	if (uri_contains_forbidden_characters (uri))
		return escape_uri (uri);

	/* If it fails to be unescaped then it was not escaped. */
	char *unescaped = g_uri_unescape_string (uri, NULL);
	if (!unescaped)
		return escape_uri (uri);
	g_free (unescaped);

	/* At this point it is probably safe to pass through as-is. */
	return g_strdup (uri);
}

static void
fe_open_url_inner (const char *url)
{
#ifdef WIN32
	gunichar2 *url_utf16 = g_utf8_to_utf16 (url, -1, NULL, NULL, NULL);

	if (url_utf16 == NULL)
	{
		return;
	}

	ShellExecuteW (0, L"open", url_utf16, NULL, NULL, SW_SHOWNORMAL);

	g_free (url_utf16);
#elif defined(__APPLE__)
    osx_show_uri (url);
#else
	char *escaped_url = maybe_escape_uri (url);
	GError *error = NULL;
	if (!g_app_info_launch_default_for_uri (escaped_url, NULL, &error))
	{
		g_printerr ("Failed to open URL \"%s\": %s\n",
		            escaped_url, error ? error->message : "unknown error");
		g_clear_error (&error);
	}
	g_free (escaped_url);
#endif
}

void
fe_open_url (const char *url)
{
	int url_type = url_check_word (url);
	char *uri;

	/* gvfs likes file:// */
	if (url_type == WORD_PATH)
	{
#ifndef WIN32
		uri = g_strconcat ("file://", url, NULL);
		fe_open_url_inner (uri);
		g_free (uri);
#else
		fe_open_url_inner (url);
#endif
	}
	/* IPv6 addr. Add http:// */
	else if (url_type == WORD_HOST6)
	{
		/* IPv6 addrs in urls should be enclosed in [ ] */
		if (*url != '[')
			uri = g_strdup_printf ("http://[%s]", url);
		else
			uri = g_strdup_printf ("http://%s", url);

		fe_open_url_inner (uri);
		g_free (uri);
	}
	/* the http:// part's missing, prepend it, otherwise it won't always work */
	else if (strchr (url, ':') == NULL)
	{
		uri = g_strdup_printf ("http://%s", url);
		fe_open_url_inner (uri);
		g_free (uri);
	}
	/* we have a sane URL, send it to the browser untouched */
	else
	{
		fe_open_url_inner (url);
	}
}

void
fe_server_event (server *serv, int type, int arg)
{
	GSList *list = sess_list;
	session *sess;

	while (list)
	{
		sess = list->data;
		if (sess->server == serv && (current_tab == sess || !sess->gui->is_tab))
		{
			session_gui *gui = sess->gui;

			switch (type)
			{
			case FE_SE_CONNECTING:	/* connecting in progress */
			case FE_SE_RECONDELAY:	/* reconnect delay begun */
				/* enable Disconnect item */
				menu_set_action_sensitive (gui, MENU_ID_DISCONNECT, 1);
				break;

			case FE_SE_CONNECT:
				/* enable Disconnect and Away menu items */
				menu_set_action_sensitive (gui, MENU_ID_AWAY, 1);
				menu_set_action_sensitive (gui, MENU_ID_DISCONNECT, 1);
				break;

			case FE_SE_LOGGEDIN:	/* end of MOTD */
				menu_set_action_sensitive (gui, MENU_ID_JOIN, 1);
				/* if number of auto-join channels is zero, open joind */
				if (arg == 0)
					joind_open (serv);
				break;

			case FE_SE_DISCONNECT:
				/* disable Disconnect and Away menu items */
				menu_set_action_sensitive (gui, MENU_ID_AWAY, 0);
				menu_set_action_sensitive (gui, MENU_ID_DISCONNECT, 0);
				menu_set_action_sensitive (gui, MENU_ID_JOIN, 0);
				/* close the join-dialog, if one exists */
				joind_close (serv);
			}
		}
		list = list->next;
	}
}

void
fe_get_file (const char *title, char *initial,
				 void (*callback) (void *userdata, char *file), void *userdata,
				 int flags)
				
{
	/* OK: Call callback once per file, then once more with file=NULL. */
	/* CANCEL: Call callback once with file=NULL. */
	gtkutil_file_req (NULL, title, callback, userdata, initial, NULL, flags | FRF_FILTERISINITIAL);
}

void
fe_open_chan_list (server *serv, char *filter, int do_refresh)
{
	chanlist_opengui (serv, do_refresh);
}

const char *
fe_get_default_font (void)
{
#ifdef WIN32
	if (gtkutil_find_font ("Consolas"))
		return "Consolas 10";
	else
#else
#ifdef __APPLE__
	if (gtkutil_find_font ("Menlo"))
		return "Menlo 13";
	else
#endif
#endif
		return NULL;
}

void
fe_reset_scroll_top_backoff (session *sess)
{
	if (sess && sess->gui && sess->gui->xtext)
	{
		gtk_xtext_reset_scroll_top_backoff (GTK_XTEXT (sess->gui->xtext));
	}
}

void
fe_reaction_received (session *sess, const char *target_msgid,
                      const char *reaction_text, const char *nick, int is_self)
{
	xtext_buffer *buf;
	textentry *target;

	if (!sess || !sess->res || !sess->res->buffer)
		return;

	buf = sess->res->buffer;
	target = gtk_xtext_find_by_msgid (buf, target_msgid);
	if (!target)
		return;

	gtk_xtext_entry_add_reaction (buf, target, reaction_text, nick, is_self);
}

void
fe_reaction_removed (session *sess, const char *target_msgid,
                     const char *reaction_text, const char *nick)
{
	xtext_buffer *buf;
	textentry *target;

	if (!sess || !sess->res || !sess->res->buffer)
		return;

	buf = sess->res->buffer;
	target = gtk_xtext_find_by_msgid (buf, target_msgid);
	if (!target)
		return;

	gtk_xtext_entry_remove_reaction (buf, target, reaction_text, nick);
}

void
fe_reply_context_set (session *sess, const char *reply_msgid)
{
	xtext_buffer *buf;
	textentry *new_ent;
	textentry *orig;
	const char *orig_nick = NULL;
	char *stripped_nick = NULL;
	char *stripped_preview = NULL;
	char preview[81] = "";
	guint64 orig_id = 0;

	if (!sess || !sess->res || !sess->res->buffer || !reply_msgid)
		return;

	buf = sess->res->buffer;

	/* Find the entry to attach reply context to.
	 * For echo-confirmed messages, the entry has its msgid set already (from fe_confirm_entry).
	 * For non-echo messages, the entry was just appended and is the last one.
	 * Try by current_msgid first (set by inbound_chanmsg), then fall back to last. */
	new_ent = NULL;
	if (sess->current_msgid)
		new_ent = gtk_xtext_find_by_msgid (buf, sess->current_msgid);
	if (!new_ent)
		new_ent = gtk_xtext_buffer_get_last (buf);
	if (!new_ent)
		return;

	/* Try to resolve the referenced message */
	orig = gtk_xtext_find_by_msgid (buf, reply_msgid);
	if (orig)
	{
		const unsigned char *str = gtk_xtext_entry_get_str (orig);
		int str_len = gtk_xtext_entry_get_str_len (orig);
		int left_len = gtk_xtext_entry_get_left_len (orig);

		orig_id = gtk_xtext_get_entry_id (orig);

		/* Extract nick from left portion, stripping all format codes */
		if (left_len > 0 && str)
		{
			char *raw_nick = g_strndup ((const char *)str, left_len);
			char *p;
			stripped_nick = strip_color (raw_nick, -1, STRIP_ALL);
			g_free (raw_nick);
			/* Trim surrounding brackets/punctuation like <nick> or «nick» */
			p = stripped_nick;
			while (*p && (*p == '<' || *p == '\xc2'))  /* skip < or « (UTF-8: C2 AB) */
			{
				if (*p == '<') { p++; break; }
				if (*p == '\xc2' && *(p+1) == '\xab') { p += 2; break; }
				break;
			}
			orig_nick = p;
			/* Trim trailing > or » and whitespace */
			{
				int len = strlen (orig_nick);
				while (len > 0)
				{
					char c = orig_nick[len - 1];
					if (c == '>' || c == ' ' || c == '\t')
						len--;
					else if (len >= 2 && (unsigned char)orig_nick[len - 2] == 0xc2 &&
					         (unsigned char)orig_nick[len - 1] == 0xbb)
						len -= 2;  /* » */
					else
						break;
				}
				/* Write NUL into stripped_nick (which we own) */
				((char *)orig_nick)[len] = '\0';
			}
		}

		/* Build preview from the right portion (message text after separator) */
		if (str && str_len > left_len)
		{
			char *raw_preview = g_strndup ((const char *)(str + left_len), MIN (str_len - left_len, 120));
			stripped_preview = strip_color (raw_preview, -1, STRIP_ALL);
			g_free (raw_preview);
			g_strlcpy (preview, stripped_preview, sizeof (preview));
		}
	}

	gtk_xtext_entry_set_reply (buf, new_ent,
	                           reply_msgid, orig_nick, preview, orig_id);

	/* Persist reply context to scrollback (if this entry has a msgid) */
	{
		const char *new_msgid = gtk_xtext_get_msgid (new_ent);
		if (new_msgid)
			scrollback_save_reply_for_session (sess, new_msgid,
			                                   reply_msgid, orig_nick, preview);
	}

	g_free (stripped_nick);
	g_free (stripped_preview);
}

void fe_reply_state_changed (session *sess);

static void
fe_reply_dismiss_cb (GtkXText *xtext, const char *key, gpointer userdata)
{
	session *sess = current_sess;
	(void)xtext;
	(void)key;
	(void)userdata;

	if (!sess)
		return;

	g_clear_pointer (&sess->reply_msgid, g_free);
	g_clear_pointer (&sess->reply_nick, g_free);
	g_clear_pointer (&sess->react_target_msgid, g_free);
	g_clear_pointer (&sess->react_target_nick, g_free);
	g_clear_pointer (&sess->picker_pending_cmd, g_free);
	fe_reply_state_changed (sess);
}

static void
fe_picker_click_cb (GtkXText *xtext, const char *msgid, gpointer userdata)
{
	session *sess = current_sess;
	(void)xtext;
	(void)userdata;

	if (!sess || !sess->picker_pending_cmd)
		return;

	if (!msgid || !*msgid)
	{
		PrintText (sess, _("This message has no message ID.\n"));
		return;
	}

	{
		char *cmd = g_strdup_printf (sess->picker_pending_cmd, msgid);
		g_clear_pointer (&sess->picker_pending_cmd, g_free);
		fe_reply_state_changed (sess);
		handle_command (sess, cmd, FALSE);
		g_free (cmd);
	}
}

void
fe_reply_state_changed (session *sess)
{
	GtkXText *xtext;

	if (!sess || !sess->gui || !sess->gui->xtext)
		return;

	xtext = GTK_XTEXT (sess->gui->xtext);

	if (sess->picker_pending_cmd)
	{
		gtk_xtext_status_set (xtext, "reply",
		                       _("\xf0\x9f\x94\x8d Click a message to select its ID"),
		                       10, 0);
		gtk_xtext_status_set_dismiss (xtext, "reply", fe_reply_dismiss_cb, NULL);
		gtk_xtext_set_picker_click_callback (xtext, fe_picker_click_cb, NULL);
	}
	else
	{
		gtk_xtext_set_picker_click_callback (xtext, NULL, NULL);
		if (sess->react_target_msgid && sess->react_target_nick)
		{
			char *text = g_strdup_printf (_("\xf0\x9f\x92\xac Reacting to %s"), sess->react_target_nick);
			gtk_xtext_status_set (xtext, "reply", text, 10, 0);
			gtk_xtext_status_set_dismiss (xtext, "reply", fe_reply_dismiss_cb, NULL);
			g_free (text);
		}
		else if (sess->reply_msgid && sess->reply_nick)
		{
			char *text = g_strdup_printf (_("\xe2\x86\xa9 Replying to %s"), sess->reply_nick);
			gtk_xtext_status_set (xtext, "reply", text, 10, 0);
			gtk_xtext_status_set_dismiss (xtext, "reply", fe_reply_dismiss_cb, NULL);
			g_free (text);
		}
		else
		{
			gtk_xtext_status_remove (xtext, "reply");
		}
	}
}

/* Re-attach a persisted reply context to a specific entry during scrollback load.
 * Unlike fe_reply_context_set (which targets the last entry), this targets
 * a specific entry identified by its msgid.
 */
void
fe_scrollback_reply_attach (session *sess, const char *entry_msgid,
                            const char *target_msgid, const char *target_nick,
                            const char *target_preview)
{
	xtext_buffer *buf;
	textentry *ent;
	textentry *orig;
	guint64 orig_id = 0;

	if (!sess || !sess->res || !sess->res->buffer || !entry_msgid || !target_msgid)
		return;

	buf = sess->res->buffer;

	/* Find the entry this reply info belongs to */
	ent = gtk_xtext_find_by_msgid (buf, entry_msgid);
	if (!ent)
		return;

	/* Try to resolve the target entry for click-to-scroll */
	orig = gtk_xtext_find_by_msgid (buf, target_msgid);
	if (orig)
		orig_id = gtk_xtext_get_entry_id (orig);

	gtk_xtext_entry_set_reply (buf, ent,
	                           target_msgid, target_nick, target_preview, orig_id);
}

void
fe_begin_multiline_group (session *sess)
{
	if (!sess || !sess->res || !sess->res->buffer)
		return;

	gtk_xtext_begin_group (sess->res->buffer);
}

void
fe_end_multiline_group (session *sess)
{
	if (!sess || !sess->res || !sess->res->buffer)
		return;

	gtk_xtext_end_group (sess->res->buffer);
}

void
fe_scrollback_extras_done (session *sess)
{
	if (!sess || !sess->res || !sess->res->buffer)
		return;

	/* Recalculate total line count after reactions/replies added extra lines.
	 * This corrects num_lines and scroll position after bulk attachment. */
	gtk_xtext_calc_lines (sess->res->buffer, FALSE);
}

void
fe_scrollback_set_virtual (session *sess, void *db, const char *channel,
                           int total_entries, gint64 max_rowid)
{
	if (!sess || !sess->res || !sess->res->buffer)
		return;

	/* Use the session's own buffer, not xtext->buffer which points to
	 * whichever tab is currently active.  During startup, multiple
	 * sessions load scrollback concurrently but only one is displayed. */
	gtk_xtext_buffer_set_virtual (sess->res->buffer, db, channel, total_entries, max_rowid);
}

void
fe_set_pending_db_rowid (session *sess, gint64 rowid)
{
	if (sess && sess->res && sess->res->buffer)
		((xtext_buffer *) sess->res->buffer)->pending_db_rowid = rowid;
}

void
fe_set_batch_mode (session *sess, gboolean on)
{
	xtext_buffer *buf;

	if (!sess || !sess->res || !sess->res->buffer)
		return;

	buf = sess->res->buffer;

	if (on)
	{
		/* Save scroll anchor before batch modifies the buffer */
		if (buf->xtext && buf->xtext->buffer == buf)
			gtk_xtext_save_scroll_anchor_top (buf, &buf->batch_anchor);
	}

	buf->batch_mode = on ? 1 : 0;

	if (!on)
	{
		buf->insert_hint = NULL;
		buf->insert_hint_lines = 0;
	}

	/* When batch ends, enforce the materialization window, recalculate
	 * line counts (total_entries synced from DB by calc_lines), restore
	 * scroll position, and redraw. */
	if (!on && buf->xtext && buf->xtext->buffer == buf)
	{
		buf->pagetop_ent = NULL;

		if (HAS_VIRT_DB (buf))
			gtk_xtext_enforce_mat_window (buf);

		gtk_xtext_calc_lines (buf, FALSE);
		gtk_xtext_restore_scroll_anchor_top (buf, &buf->batch_anchor);

		gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
	}
}
