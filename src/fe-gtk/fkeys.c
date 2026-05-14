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
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>

#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "fe-gtk.h"
#include "hex-input-edit.h"
#include "xtext.h"

#include "../common/poxchat.h"
#include "../common/poxchatc.h"
#include "../common/cfgfiles.h"
#include "../common/fe.h"
#include "../common/userlist.h"
#include "../common/outbound.h"
#include "../common/util.h"
#include "../common/text.h"
#include "../common/plugin.h"
#include "../common/typedef.h"
#include <gdk/gdkkeysyms.h>
#include "gtkutil.h"
#include "gtk-helpers.h"
#include "menu.h"
#include "xtext.h"
#include "palette.h"
#include "maingui.h"
#include "textgui.h"
#include "fkeys.h"

static void replace_handle (GtkWidget * wid);
void key_action_tab_clean (void);

/* Replacement for deprecated GCompletion (removed in GLib 2.26) */
typedef gchar* (*HcCompletionFunc)(gpointer);
typedef gint (*HcCompletionStrncmpFunc)(const gchar*, const gchar*, gsize);

typedef struct {
	GList *items;
	HcCompletionFunc func;
	HcCompletionStrncmpFunc strncmp_func;
} HcCompletion;

static HcCompletion *
hc_completion_new (HcCompletionFunc func)
{
	HcCompletion *comp = g_new0 (HcCompletion, 1);
	comp->func = func;
	comp->strncmp_func = strncmp;
	return comp;
}

static void
hc_completion_add_items (HcCompletion *comp, GList *items)
{
	comp->items = g_list_concat (comp->items, g_list_copy (items));
}

static void
hc_completion_set_compare (HcCompletion *comp, HcCompletionStrncmpFunc cmp)
{
	comp->strncmp_func = cmp;
}

static GList *
hc_completion_complete_utf8 (HcCompletion *comp, const char *prefix, char **new_prefix)
{
	GList *matches = NULL;
	GList *l;
	gsize prefix_len = strlen (prefix);
	const char *first_match = NULL;
	gsize common_len = 0;

	for (l = comp->items; l; l = l->next)
	{
		const char *str = comp->func ? comp->func (l->data) : (const char *)l->data;
		if (str && comp->strncmp_func (str, prefix, prefix_len) == 0)
		{
			matches = g_list_prepend (matches, l->data);
			if (!first_match)
			{
				first_match = str;
				common_len = strlen (str);
			}
			else
			{
				gsize i;
				gsize slen = strlen (str);
				if (slen < common_len)
					common_len = slen;
				for (i = 0; i < common_len; i++)
				{
					if (comp->strncmp_func (str + i, first_match + i, 1) != 0)
					{
						common_len = i;
						break;
					}
				}
			}
		}
	}

	if (new_prefix)
	{
		if (first_match && common_len > 0)
			*new_prefix = g_strndup (first_match, common_len);
		else
			*new_prefix = NULL;
	}

	return g_list_reverse (matches);
}

static void
hc_completion_free (HcCompletion *comp)
{
	g_list_free (comp->items);
	g_free (comp);
}

/***************** Key Binding Code ******************/

/* NOTES:

   To add a new action:
   1) inc KEY_MAX_ACTIONS
   2) write the function at the bottom of this file (with all the others)
   FIXME: Write about calling and returning
   3) Add it to key_actions

   --AGL

 */

/* Remember that the *number* of actions is this *plus* 1 --AGL */
#define KEY_MAX_ACTIONS 16

struct key_binding
{
	guint keyval;					  /* keyval from gdk */
	GdkModifierType mod;			  /* Modifier, always ran through key_modifier_get_valid() */
	int action;						  /* Index into key_actions */
	char *data1;					  /* Pointer to string, must be freed */
};

/*
 * Key action handler type - GTK4 doesn't have GdkEventKey, so we use void*
 * The event parameter is not actually used by most handlers anyway
 */
typedef int (*key_action_handler) (GtkWidget *wid, void *evt, char *d1, char *d2,
                                   struct session *sess);
/* Macro for function implementation signatures - allows a single definition to work for both */
#define KEY_EVENT_PARAM void *evt

struct key_action
{
	key_action_handler handler;
	char *name;
	char *help;
};

struct gcomp_data
{
	char data[CHANLEN];
	int elen;
};

static int key_load_kbs (void);
static int key_save_kbs (void);

/* Key action function declarations - use void* for evt in GTK4 */
static int key_action_handle_command (GtkWidget *wid, void *evt,
                                      char *d1, char *d2, struct session *sess);
static int key_action_page_switch (GtkWidget *wid, void *evt,
                                   char *d1, char *d2, struct session *sess);
int key_action_insert (GtkWidget *wid, void *evt, char *d1, char *d2,
                       struct session *sess);
static int key_action_scroll_page (GtkWidget *wid, void *evt,
                                   char *d1, char *d2, struct session *sess);
static int key_action_set_buffer (GtkWidget *wid, void *evt,
                                  char *d1, char *d2, struct session *sess);
static int key_action_history_up (GtkWidget *wid, void *evt,
                                  char *d1, char *d2, struct session *sess);
static int key_action_history_down (GtkWidget *wid, void *evt,
                                    char *d1, char *d2, struct session *sess);
static int key_action_tab_comp (GtkWidget *wid, void *evt, char *d1,
                                char *d2, struct session *sess);
static int key_action_comp_chng (GtkWidget *wid, void *evt, char *d1,
                                 char *d2, struct session *sess);
static int key_action_replace (GtkWidget *wid, void *evt, char *d1,
                               char *d2, struct session *sess);
static int key_action_move_tab_left (GtkWidget *wid, void *evt,
                                     char *d1, char *d2, struct session *sess);
static int key_action_move_tab_right (GtkWidget *wid, void *evt,
                                      char *d1, char *d2, struct session *sess);
static int key_action_move_tab_family_left (GtkWidget *wid, void *evt,
                                            char *d1, char *d2, struct session *sess);
static int key_action_move_tab_family_right (GtkWidget *wid, void *evt,
                                             char *d1, char *d2, struct session *sess);
static int key_action_put_history (GtkWidget *wid, void *evt,
                                   char *d1, char *d2, struct session *sess);
static int key_action_next_tab (GtkWidget *wid, void *evt,
                                char *d1, char *d2, struct session *sess);
static int key_action_prev_tab (GtkWidget *wid, void *evt,
                                char *d1, char *d2, struct session *sess);

static GSList *keybind_list = NULL;

static const struct key_action key_actions[KEY_MAX_ACTIONS + 1] = {

	{key_action_handle_command, "Run Command",
	 N_("The \002Run Command\002 action runs the data in Data 1 as if it had been typed into the entry box where you pressed the key sequence. Thus it can contain text (which will be sent to the channel/person), commands or user commands. When run all \002\\n\002 characters in Data 1 are used to deliminate separate commands so it is possible to run more than one command. If you want a \002\\\002 in the actual text run then enter \002\\\\\002")},
	{key_action_page_switch, "Change Tab",
	 N_("The \002Change Tab\002 command switches between tabs. Set Data 1 to an absolute tab number (e.g. 3), or to a signed offset (e.g. +1, -1) for a relative jump. Set Data 1 to auto to switch to the tab with the most recent and important activity (queries first, then channels with hilight, channels with dialogue, channels with other data). For plain previous/next tab, use the dedicated \002Previous Tab\002 / \002Next Tab\002 actions instead.")},
	{key_action_insert, "Insert in Buffer",
	 N_("The \002Insert in Buffer\002 command will insert the contents of Data 1 into the entry where the key sequence was pressed at the current cursor position")},
	{key_action_scroll_page, "Scroll Page",
	 N_("The \002Scroll Page\002 command scrolls the text widget up or down one page or one line. Set Data 1 to either Top, Bottom, Up, Down, +1 or -1.")},
	{key_action_set_buffer, "Set Buffer",
	 N_("The \002Set Buffer\002 command sets the entry where the key sequence was entered to the contents of Data 1")},
	{key_action_history_up, "Last Command",
	 N_("The \002Last Command\002 command sets the entry to contain the last command entered - the same as pressing up in a shell")},
	{key_action_history_down, "Next Command",
	 N_("The \002Next Command\002 command sets the entry to contain the next command entered - the same as pressing down in a shell")},
	{key_action_tab_comp, "Complete nick/command",
	 N_("This command changes the text in the entry to finish an incomplete nickname or command. If Data 1 is set then double-tabbing in a string will select the last nick, not the next")},
	{key_action_comp_chng, "Change Selected Nick",
	 N_("This command scrolls up and down through the list of nicks. If Data 1 is set to anything it will scroll up, else it scrolls down")},
	{key_action_replace, "Check For Replace",
	 N_("This command checks the last word entered in the entry against the replace list and replaces it if it finds a match")},
	{key_action_move_tab_left, "Move front tab left",
	 N_("This command moves the front tab left by one")},
	{key_action_move_tab_right, "Move front tab right",
	 N_("This command moves the front tab right by one")},
	{key_action_move_tab_family_left, "Move tab family left",
	 N_("This command moves the current tab family to the left")},
	{key_action_move_tab_family_right, "Move tab family right",
	 N_("This command moves the current tab family to the right")},
	{key_action_put_history, "Push input line into history",
	 N_("Push input line into history but doesn't send to server")},
	{key_action_next_tab, "Next Tab",
	 N_("Switch to the next tab. No data fields needed.")},
	{key_action_prev_tab, "Previous Tab",
	 N_("Switch to the previous tab. No data fields needed.")},
};

#define default_kb_cfg \
	"ACCEL=<Primary>Page_Up\nPrevious Tab\nD1!\n\n"\
	"ACCEL=<Primary>Page_Down\nNext Tab\nD1!\n\n"\
	"ACCEL=<Alt>9\nChange Tab\nD1:9\n\n"\
	"ACCEL=<Alt>8\nChange Tab\nD1:8\n\n"\
	"ACCEL=<Alt>7\nChange Tab\nD1:7\n\n"\
	"ACCEL=<Alt>6\nChange Tab\nD1:6\n\n"\
	"ACCEL=<Alt>5\nChange Tab\nD1:5\n\n"\
	"ACCEL=<Alt>4\nChange Tab\nD1:4\n\n"\
	"ACCEL=<Alt>3\nChange Tab\nD1:3\n\n"\
	"ACCEL=<Alt>2\nChange Tab\nD1:2\n\n"\
	"ACCEL=<Alt>1\nChange Tab\nD1:1\n\n"\
	"ACCEL=<Alt>grave\nChange Tab\nD1:auto\n\n"\
	"ACCEL=<Primary>o\nInsert in Buffer\nD1:\017\n\n"\
	"ACCEL=<Primary>b\nInsert in Buffer\nD1:\002\n\n"\
	"ACCEL=<Primary>k\nInsert in Buffer\nD1:\003\n\n"\
	"ACCEL=<Primary>i\nInsert in Buffer\nD1:\035\n\n"\
	"ACCEL=<Primary>u\nInsert in Buffer\nD1:\037\n\n"\
	"ACCEL=<Primary>r\nInsert in Buffer\nD1:\026\n\n"\
	"ACCEL=<Shift>Page_Down\nChange Selected Nick\nD1!\n\n"\
	"ACCEL=<Shift>Page_Up\nChange Selected Nick\nD1:Up\n\n"\
	"ACCEL=Page_Down\nScroll Page\nD1:Down\n\n"\
	"ACCEL=<Primary>Home\nScroll Page\nD1:Top\n\n"\
	"ACCEL=<Primary>End\nScroll Page\nD1:Bottom\n\n"\
	"ACCEL=Page_Up\nScroll Page\nD1:Up\n\n"\
	"ACCEL=<Shift>Down\nScroll Page\nD1:+1\n\n"\
	"ACCEL=<Shift>Up\nScroll Page\nD1:-1\n\n"\
	"ACCEL=Down\nNext Command\nD1!\n\n"\
	"ACCEL=Up\nLast Command\nD1!\n\n"\
	"ACCEL=Tab\nComplete nick/command\nD1!\n\n"\
	"ACCEL=<Shift>ISO_Left_Tab\nComplete nick/command\nD1:Previous\n\n"\
	"ACCEL=space\nCheck For Replace\nD1!\n\n"\
	"ACCEL=Return\nCheck For Replace\nD1!\n\n"\
	"ACCEL=KP_Enter\nCheck For Replace\nD1!\n\n"\
	"ACCEL=<Primary>Tab\nComplete nick/command\nD1:Up\n\n"\
	"ACCEL=<Alt>Left\nMove front tab left\nD1!\n\n"\
	"ACCEL=<Alt>Right\nMove front tab right\nD1!\n\n"\
	"ACCEL=<Primary><Shift>Page_Up\nMove tab family left\nD1!\n\n"\
	"ACCEL=<Primary><Shift>Page_Down\nMove tab family right\nD1!\n\n"\
	"ACCEL=F9\nRun Command\nD1:/GUI MENU TOGGLE\n\n"

void
key_init ()
{
	if (key_load_kbs () == 1)
	{
		fe_message (_("There was an error loading key"
							" bindings configuration"), FE_MSG_ERROR);
	}
}

static inline int
key_get_action_from_string (char *text)
{
	int i;

	/* Legacy alias: "Change Page" was renamed to "Change Tab". */
	if (strcmp (text, "Change Page") == 0)
		text = "Change Tab";

	for (i = 0; i < KEY_MAX_ACTIONS + 1; i++)
	{
		if (strcmp (key_actions[i].name, text) == 0)
		{
			return i;
		}
	}

	return 0;
}

static void
key_free (gpointer data)
{
	struct key_binding *kb = (struct key_binding*)data;

	g_return_if_fail (kb != NULL);

	g_free (kb->data1);
	g_free (kb);
}

/* Ok, here are the NOTES

   key_handle_key_press now handles all the key presses and history_keypress is
   now defunct. It goes thru the linked list keys_root and finds a matching
   key. It runs the action func and switches on these values:
   0) Return
   1) Find next
   2) stop signal and return

   * history_keypress is now dead (and gone)
   * key_handle_key_press now takes its role
   * All the possible actions are in a struct called key_actions (in fkeys.c)
   * it is made of {function, name, desc}
   * key bindings can pass 2 *text* strings to the handler. If more options are nee
   ded a format can be put on one of these options
   * key actions are passed {
   the entry widget
   the Gdk event
   data 1
   data 2
   session struct
   }
   * key bindings are stored in a linked list of key_binding structs
   * which looks like {
   guint keyval;  GDK keynumber
   int action;  Index into key_actions
   GdkModiferType mod; modifier, only ones from key_modifer_get_valid()
   char *data1, *data2;  Pointers to strings, these must be freed
   struct key_binding *next;
   }
   * remember that is (data1 || data2) != NULL then they need to be free()'ed

   --AGL

 */

static inline GdkModifierType
key_modifier_get_valid (GdkModifierType mod)
{
	GdkModifierType ret;

#ifdef __APPLE__
	ret = mod & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_META_MASK);
#else
	/* These masks work on both Windows and Unix */
	ret = mod & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK);
#endif

	return ret;
}

/*
 * Key press handler for input box
 * GTK3: Uses GdkEventKey from "key-press-event" signal
 * GTK4: Uses GtkEventControllerKey with different signature
 */
gboolean
key_handle_key_press (GtkEventControllerKey *controller, guint keyval,
                      guint keycode, GdkModifierType state, session *sess)
{
	struct key_binding *kb;
	int n;
	GSList *list;
	GtkWidget *wid = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));

	/* where did this event come from? */
	list = sess_list;
	while (list)
	{
		sess = list->data;
		if (sess->gui->input_box == wid)
		{
			if (sess->gui->is_tab)
				sess = current_tab;
			break;
		}
		list = list->next;
	}
	if (!list)
		return FALSE;
	current_sess = sess;

	if (plugin_emit_keypress (sess, state, keyval, gdk_keyval_to_unicode (keyval)))
		return 1;

	/* maybe the plugin closed this tab? */
	if (!is_session (sess))
		return 1;

	list = keybind_list;
	while (list)
	{
		kb = (struct key_binding*)list->data;

		if (kb->keyval == keyval && kb->mod == key_modifier_get_valid (state))
		{
			if (kb->action < 0 || kb->action > KEY_MAX_ACTIONS)
				return 0;

			/* Run the function - pass NULL for evt in GTK4 since handlers don't use it.
			 * d2 is no longer stored; pass NULL for back-compat with the shared signature. */
			n = key_actions[kb->action].handler (wid, NULL, kb->data1,
															 NULL, sess);
			switch (n)
			{
			case 0:
				return 1;
			case 2:
				/* GTK4: Event propagation is handled by returning TRUE */
				return 1;
			}
		}
		list = g_slist_next (list);
	}

	switch (keyval)
	{
	case GDK_KEY_space:
		key_action_tab_clean ();
		break;

	case GDK_KEY_Escape:
		/* Cancel active reply, react-with-text, or picker state */
		if (sess->reply_msgid || sess->react_target_msgid || sess->picker_pending_cmd)
		{
			g_clear_pointer (&sess->reply_msgid, g_free);
			g_clear_pointer (&sess->reply_nick, g_free);
			g_clear_pointer (&sess->react_target_msgid, g_free);
			g_clear_pointer (&sess->react_target_nick, g_free);
			g_clear_pointer (&sess->picker_pending_cmd, g_free);
			fe_reply_state_changed (sess);
			return 1; /* consume the key */
		}
		break;
	}

	typing_indicator_keystroke (sess);

	return 0;
}


/* ***** GUI code here ******************* */

static GtkWidget *key_dialog = NULL;
static GListStore *key_store = NULL;
static GtkWidget *key_column_view = NULL;

/* Key binding item for GtkColumnView */
#define HC_TYPE_KEY_BINDING_ITEM (hc_key_binding_item_get_type())
G_DECLARE_FINAL_TYPE (HcKeyBindingItem, hc_key_binding_item, HC, KEY_BINDING_ITEM, GObject)

struct _HcKeyBindingItem {
	GObject parent;
	guint keyval;
	GdkModifierType mod;
	int action;
	char *data1;
};

G_DEFINE_TYPE (HcKeyBindingItem, hc_key_binding_item, G_TYPE_OBJECT)

static void
hc_key_binding_item_finalize (GObject *obj)
{
	HcKeyBindingItem *item = HC_KEY_BINDING_ITEM (obj);
	g_free (item->data1);
	G_OBJECT_CLASS (hc_key_binding_item_parent_class)->finalize (obj);
}

static void hc_key_binding_item_class_init (HcKeyBindingItemClass *klass) { G_OBJECT_CLASS (klass)->finalize = hc_key_binding_item_finalize; }
static void hc_key_binding_item_init (HcKeyBindingItem *item) { }

static HcKeyBindingItem *
hc_key_binding_item_new (guint keyval, GdkModifierType mod, int action, const char *data1)
{
	HcKeyBindingItem *item = g_object_new (HC_TYPE_KEY_BINDING_ITEM, NULL);
	item->keyval = keyval;
	item->mod = mod;
	item->action = action;
	item->data1 = g_strdup (data1 ? data1 : "");
	return item;
}

static void
key_dialog_print_text (GtkXText *xtext, char *text)
{
	unsigned int old = prefs.hex_stamp_text;
	prefs.hex_stamp_text = 0;	/* temporarily disable stamps */
	gtk_xtext_clear (GTK_XTEXT (xtext)->buffer, 0);
	PrintTextRaw (GTK_XTEXT (xtext)->buffer, text, 0, 0);
	prefs.hex_stamp_text = old;
}

/* --- Accel button: click to capture a key --- */

static GtkWidget *accel_capturing_button = NULL;  /* currently capturing button, or NULL */

static void
accel_button_update_label (GtkButton *button, guint keyval, GdkModifierType mod)
{
	if (keyval == 0 && mod == 0)
	{
		gtk_button_set_label (button, _("(none)"));
	}
	else
	{
		char *label = gtk_accelerator_get_label (keyval, mod);
		gtk_button_set_label (button, label);
		g_free (label);
	}
}

static gboolean
accel_button_key_cb (GtkEventControllerKey *controller, guint keyval,
                     guint keycode, GdkModifierType state, gpointer user_data)
{
	GtkButton *button = GTK_BUTTON (user_data);
	HcKeyBindingItem *item;

	if (accel_capturing_button != GTK_WIDGET (button))
		return FALSE;

	item = g_object_get_data (G_OBJECT (button), "kb-item");

	/* Escape cancels capture */
	if (keyval == GDK_KEY_Escape)
	{
		accel_button_update_label (button, item->keyval, item->mod);
		gtk_widget_remove_css_class (GTK_WIDGET (button), "capturing");
		accel_capturing_button = NULL;
		return TRUE;
	}

	/* Delete/Backspace clears the binding */
	if (keyval == GDK_KEY_Delete || keyval == GDK_KEY_BackSpace)
	{
		item->keyval = 0;
		item->mod = 0;
		accel_button_update_label (button, 0, 0);
		gtk_widget_remove_css_class (GTK_WIDGET (button), "capturing");
		accel_capturing_button = NULL;
		return TRUE;
	}

	/* Ignore bare modifier presses — wait for the actual key.
	 * Without this, pressing Alt or Ctrl on its own immediately ends
	 * capture (with mod=0, since the modifier mask isn't set on the
	 * keypress that *is* the modifier). */
	switch (keyval)
	{
	case GDK_KEY_Shift_L: case GDK_KEY_Shift_R:
	case GDK_KEY_Control_L: case GDK_KEY_Control_R:
	case GDK_KEY_Alt_L: case GDK_KEY_Alt_R:
	case GDK_KEY_Meta_L: case GDK_KEY_Meta_R:
	case GDK_KEY_Super_L: case GDK_KEY_Super_R:
	case GDK_KEY_Hyper_L: case GDK_KEY_Hyper_R:
	case GDK_KEY_ISO_Level3_Shift: case GDK_KEY_ISO_Level5_Shift:
	case GDK_KEY_Caps_Lock: case GDK_KEY_Shift_Lock:
	case GDK_KEY_Num_Lock: case GDK_KEY_Scroll_Lock:
		return TRUE;
	}

	/* Shift tab requires an exception */
	if (keyval == GDK_KEY_Tab && state & GDK_SHIFT_MASK)
		keyval = GDK_KEY_ISO_Left_Tab;

	item->keyval = keyval;
	item->mod = key_modifier_get_valid (state);

	accel_button_update_label (button, item->keyval, item->mod);
	gtk_widget_remove_css_class (GTK_WIDGET (button), "capturing");
	accel_capturing_button = NULL;
	return TRUE;
}

static void
accel_button_clicked_cb (GtkButton *button, gpointer user_data)
{
	/* If another button is capturing, cancel it */
	if (accel_capturing_button && accel_capturing_button != GTK_WIDGET (button))
	{
		HcKeyBindingItem *old_item = g_object_get_data (G_OBJECT (accel_capturing_button), "kb-item");
		if (old_item)
			accel_button_update_label (GTK_BUTTON (accel_capturing_button), old_item->keyval, old_item->mod);
		gtk_widget_remove_css_class (accel_capturing_button, "capturing");
	}

	accel_capturing_button = GTK_WIDGET (button);
	gtk_button_set_label (button, _("Press a key..."));
	gtk_widget_add_css_class (GTK_WIDGET (button), "capturing");
}

/* --- Key column factory --- */

static void
key_col_setup_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *button = gtk_button_new_with_label ("");
	GtkEventController *key_ctrl;

	gtk_widget_set_hexpand (button, TRUE);

	key_ctrl = gtk_event_controller_key_new ();
	g_signal_connect (key_ctrl, "key-pressed", G_CALLBACK (accel_button_key_cb), button);
	gtk_widget_add_controller (button, key_ctrl);

	g_signal_connect (button, "clicked", G_CALLBACK (accel_button_clicked_cb), NULL);

	gtk_list_item_set_child (list_item, button);
}

static void
key_col_bind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *button = gtk_list_item_get_child (list_item);
	HcKeyBindingItem *item = gtk_list_item_get_item (list_item);

	g_object_set_data (G_OBJECT (button), "kb-item", item);
	accel_button_update_label (GTK_BUTTON (button), item->keyval, item->mod);

	/* If this button was capturing, cancel */
	if (accel_capturing_button == button)
	{
		gtk_widget_remove_css_class (button, "capturing");
		accel_capturing_button = NULL;
	}
}

/* --- Action column factory (GtkDropDown) --- */

static GtkStringList *action_string_list = NULL;

static void
action_col_setup_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *dropdown;

	if (!action_string_list)
	{
		action_string_list = gtk_string_list_new (NULL);
		for (int i = 0; i <= KEY_MAX_ACTIONS; i++)
		{
			if (key_actions[i].name[0])
				gtk_string_list_append (action_string_list, key_actions[i].name);
		}
	}

	dropdown = gtk_drop_down_new (G_LIST_MODEL (g_object_ref (action_string_list)), NULL);
	gtk_widget_set_hexpand (dropdown, TRUE);
	gtk_list_item_set_child (list_item, dropdown);
}

static void
action_col_changed_cb (GObject *dropdown, GParamSpec *pspec, gpointer user_data)
{
	GtkListItem *list_item = GTK_LIST_ITEM (user_data);
	HcKeyBindingItem *item = gtk_list_item_get_item (list_item);
	GtkXText *xtext;
	guint sel;

	if (!item)
		return;

	sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (dropdown));
	if (sel != GTK_INVALID_LIST_POSITION && (int)sel != item->action)
	{
		item->action = sel;

		/* Update help text */
		xtext = GTK_XTEXT (g_object_get_data (G_OBJECT (key_dialog), "xtext"));
		if (xtext && item->action >= 0 && item->action <= KEY_MAX_ACTIONS)
			key_dialog_print_text (xtext, key_actions[item->action].help);
	}
}

static void
action_col_bind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *dropdown = gtk_list_item_get_child (list_item);
	HcKeyBindingItem *item = gtk_list_item_get_item (list_item);

	g_signal_handlers_disconnect_by_func (dropdown, action_col_changed_cb, list_item);

	if (item->action >= 0 && item->action <= KEY_MAX_ACTIONS)
		gtk_drop_down_set_selected (GTK_DROP_DOWN (dropdown), item->action);
	else
		gtk_drop_down_set_selected (GTK_DROP_DOWN (dropdown), 0);

	g_signal_connect (dropdown, "notify::selected", G_CALLBACK (action_col_changed_cb), list_item);
}

static void
action_col_unbind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *dropdown = gtk_list_item_get_child (list_item);
	g_signal_handlers_disconnect_by_func (dropdown, action_col_changed_cb, list_item);
}

/* --- Data1 column factory --- */

static GtkEditableLabel *keydlg_editing_label = NULL;

static void
data1_changed_cb (GtkEditableLabel *label, GParamSpec *pspec, gpointer user_data)
{
	GtkListItem *list_item = GTK_LIST_ITEM (user_data);
	HcKeyBindingItem *item = gtk_list_item_get_item (list_item);
	if (item && gtk_editable_label_get_editing (GTK_EDITABLE_LABEL (label)))
	{
		g_free (item->data1);
		item->data1 = g_strdup (gtk_editable_get_text (GTK_EDITABLE (label)));
	}
}

static void
data1_setup_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = hc_editable_label_new (list_item, &keydlg_editing_label);
	gtk_list_item_set_child (list_item, label);
}

static void
data1_bind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	HcKeyBindingItem *item = gtk_list_item_get_item (list_item);
	gtk_editable_set_text (GTK_EDITABLE (label), item->data1 ? item->data1 : "");
	g_signal_connect (label, "notify::text", G_CALLBACK (data1_changed_cb), list_item);
}

static void
data1_unbind_cb (GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
	GtkWidget *label = gtk_list_item_get_child (list_item);
	g_signal_handlers_disconnect_by_func (label, data1_changed_cb, list_item);
}

/* --- Selection changed: update help text --- */

static void
key_dialog_selection_changed (GtkSelectionModel *sel_model, guint position, guint n_items, gpointer user_data)
{
	GtkXText *xtext;
	HcKeyBindingItem *item;

	(void)position; (void)n_items;

	xtext = GTK_XTEXT (g_object_get_data (G_OBJECT (key_dialog), "xtext"));

	item = hc_selection_model_get_selected_item (sel_model);
	if (item)
	{
		if (item->action >= 0 && item->action <= KEY_MAX_ACTIONS)
			key_dialog_print_text (xtext, key_actions[item->action].help);
		g_object_unref (item);
	}
	else
	{
		key_dialog_print_text (xtext, _("Select a row to get help information on its Action."));
	}
}

/* --- Shift+Up/Down reorder --- */

static gboolean
key_dialog_keypress (GtkEventControllerKey *controller, guint keyval,
                     guint keycode, GdkModifierType state, gpointer userdata)
{
	GtkSelectionModel *sel_model;
	guint pos, n;
	int delta;
	gboolean handled = FALSE;

	if (state & GDK_SHIFT_MASK)
	{
		if (keyval == GDK_KEY_Up)
		{
			handled = TRUE;
			delta = -1;
		}
		else if (keyval == GDK_KEY_Down)
		{
			handled = TRUE;
			delta = 1;
		}
	}

	if (handled && key_store)
	{
		sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (key_column_view));
		pos = hc_selection_model_get_selected_position (sel_model);
		n = g_list_model_get_n_items (G_LIST_MODEL (key_store));

		if (pos != GTK_INVALID_LIST_POSITION)
		{
			guint new_pos = pos + delta;
			if (new_pos < n)
			{
				HcKeyBindingItem *item = g_list_model_get_item (G_LIST_MODEL (key_store), pos);
				g_object_ref (item);
				g_list_store_remove (key_store, pos);
				g_list_store_insert (key_store, new_pos, item);
				g_object_unref (item);
				g_object_unref (item);
				gtk_selection_model_select_item (sel_model, new_pos, TRUE);
			}
		}
	}

	return handled;
}

/* --- Close / Save / Add / Delete --- */

static void
key_dialog_close (GtkWidget *wid, gpointer userdata)
{
	accel_capturing_button = NULL;
	keydlg_editing_label = NULL;
	hc_window_destroy_fn (GTK_WINDOW (key_dialog));
	key_dialog = NULL;
	key_store = NULL;
	key_column_view = NULL;
	action_string_list = NULL;
}

static void
key_dialog_save (GtkWidget *wid, gpointer userdata)
{
	guint i, n;
	struct key_binding *kb;

	if (keybind_list)
	{
		g_slist_free_full (keybind_list, key_free);
		keybind_list = NULL;
	}

	n = g_list_model_get_n_items (G_LIST_MODEL (key_store));
	for (i = 0; i < n; i++)
	{
		HcKeyBindingItem *item = g_list_model_get_item (G_LIST_MODEL (key_store), i);
		if (!item)
			continue;

		/* Skip entries with no key set */
		if (item->keyval == 0 && item->mod == 0)
		{
			g_object_unref (item);
			continue;
		}

		kb = g_new0 (struct key_binding, 1);
		kb->keyval = item->keyval;
		kb->mod = item->mod;
		kb->action = item->action;
		kb->data1 = g_strdup (item->data1 ? item->data1 : "");

		keybind_list = g_slist_append (keybind_list, kb);
		g_object_unref (item);
	}

	if (key_save_kbs () == 0)
		key_dialog_close (wid, NULL);
}

static void
key_dialog_add (GtkWidget *wid, gpointer userdata)
{
	GtkSelectionModel *sel_model;
	HcKeyBindingItem *item;
	guint n;

	item = hc_key_binding_item_new (0, 0, 0, "");
	g_list_store_append (key_store, item);
	g_object_unref (item);

	/* Select the new row */
	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (key_column_view));
	n = g_list_model_get_n_items (G_LIST_MODEL (key_store));
	if (n > 0)
		gtk_selection_model_select_item (sel_model, n - 1, TRUE);
}

static void
key_dialog_delete (GtkWidget *wid, gpointer userdata)
{
	GtkSelectionModel *sel_model;
	guint pos, n;

	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (key_column_view));
	pos = hc_selection_model_get_selected_position (sel_model);
	if (pos == GTK_INVALID_LIST_POSITION)
		return;

	g_list_store_remove (key_store, pos);

	/* Select next row if available */
	n = g_list_model_get_n_items (G_LIST_MODEL (key_store));
	if (n > 0)
	{
		if (pos >= n)
			pos = n - 1;
		gtk_selection_model_select_item (sel_model, pos, TRUE);
	}
}

/* --- Build the column view --- */

static GtkWidget *
key_dialog_treeview_new (GtkWidget *box)
{
	GtkWidget *scroll, *view;
	GtkListItemFactory *factory;
	GtkColumnViewColumn *col;
	GtkSelectionModel *sel_model;

	scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

	key_store = g_list_store_new (HC_TYPE_KEY_BINDING_ITEM);
	view = hc_column_view_new_simple (G_LIST_MODEL (key_store), GTK_SELECTION_SINGLE);
	key_column_view = view;

	/* Key column */
	col = hc_column_view_add_column (GTK_COLUMN_VIEW (view), _("Key"),
	                                  G_CALLBACK (key_col_setup_cb),
	                                  G_CALLBACK (key_col_bind_cb), NULL, NULL);
	gtk_column_view_column_set_fixed_width (col, 200);
	gtk_column_view_column_set_resizable (col, TRUE);

	/* Action column */
	col = hc_column_view_add_column (GTK_COLUMN_VIEW (view), _("Action"),
	                                  G_CALLBACK (action_col_setup_cb),
	                                  G_CALLBACK (action_col_bind_cb),
	                                  G_CALLBACK (action_col_unbind_cb), NULL);
	gtk_column_view_column_set_fixed_width (col, 200);
	gtk_column_view_column_set_resizable (col, TRUE);

	/* Data column */
	col = hc_column_view_add_column (GTK_COLUMN_VIEW (view), _("Data"),
	                                  G_CALLBACK (data1_setup_cb),
	                                  G_CALLBACK (data1_bind_cb),
	                                  G_CALLBACK (data1_unbind_cb), NULL);
	gtk_column_view_column_set_expand (col, TRUE);
	gtk_column_view_column_set_resizable (col, TRUE);

	/* Key controller for Shift+Up/Down reordering */
	hc_add_key_controller (view, G_CALLBACK (key_dialog_keypress), NULL, NULL);

	/* Selection changed */
	sel_model = gtk_column_view_get_model (GTK_COLUMN_VIEW (view));
	g_signal_connect (sel_model, "selection-changed",
	                  G_CALLBACK (key_dialog_selection_changed), NULL);

	gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), view);
	gtk_widget_set_vexpand (scroll, TRUE);
	gtk_box_append (GTK_BOX (box), scroll);

	return view;
}

/* --- Load / Show --- */

static void
key_dialog_load (void)
{
	GSList *list = keybind_list;

	g_list_store_remove_all (key_store);

	while (list)
	{
		struct key_binding *kb = (struct key_binding *)list->data;
		HcKeyBindingItem *item = hc_key_binding_item_new (
			kb->keyval, kb->mod, kb->action,
			kb->data1);
		g_list_store_append (key_store, item);
		g_object_unref (item);
		list = g_slist_next (list);
	}
}

void
key_dialog_show ()
{
	GtkWidget *vbox, *box;
	GtkWidget *view, *xtext;
	char buf[128];

	if (key_dialog)
	{
		mg_bring_tofront (key_dialog);
		return;
	}

	g_snprintf(buf, sizeof(buf), _("Keyboard Shortcuts - %s"), _(DISPLAY_NAME));
	key_dialog = mg_create_generic_tab ("editkeys", buf, TRUE, FALSE, key_dialog_close,
									NULL, 600, 360, &vbox, 0);

	view = key_dialog_treeview_new (vbox);
	xtext = gtk_xtext_new (colors, 0);
	gtk_box_append (GTK_BOX (vbox), xtext);
	gtk_xtext_set_font (GTK_XTEXT (xtext), prefs.hex_text_font);

	g_object_set_data (G_OBJECT (key_dialog), "view", view);
	g_object_set_data (G_OBJECT (key_dialog), "xtext", xtext);

	box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_margin_top (box, 12);
	gtk_box_append (GTK_BOX (vbox), box);

	gtkutil_button (box, "document-new", NULL, key_dialog_add,
					NULL, _("Add"));
	gtkutil_button (box, "edit-delete", NULL, key_dialog_delete,
					NULL, _("Delete"));

	{
		GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_set_hexpand (spacer, TRUE);
		gtk_box_append (GTK_BOX (box), spacer);
	}

	gtkutil_button (box, "process-stop", NULL, key_dialog_close,
					NULL, _("Cancel"));
	gtkutil_button (box, "document-save", NULL, key_dialog_save,
					NULL, _("Save"));

	key_dialog_load ();

	gtk_widget_set_visible (key_dialog, TRUE);
}


static int
key_save_kbs (void)
{
	int fd;
	char buf[512];
	char *accel_text;
	GSList *list = keybind_list;
	struct key_binding *kb;

	fd = poxchat_open_file ("keybindings.conf", O_CREAT | O_TRUNC | O_WRONLY,
									 0x180, XOF_DOMODE);
	if (fd < 0)
		return 1;
	HC_IGNORE_RESULT (write (fd, buf, g_snprintf (buf, 510, "# PoxChat key bindings config file\n\n")));

	while (list)
	{
		kb = list->data;

		accel_text = gtk_accelerator_name (kb->keyval, kb->mod);

		g_snprintf (buf, 510, "ACCEL=%s\n%s\n", accel_text, key_actions[kb->action].name);
		HC_IGNORE_RESULT (write (fd, buf, strlen (buf)));
		g_free (accel_text);

		if (kb->data1 && kb->data1[0])
			HC_IGNORE_RESULT (write (fd, buf, g_snprintf (buf, 510, "D1:%s\n", kb->data1)));
		else
			HC_IGNORE_RESULT (write (fd, "D1!\n", 4));

		HC_IGNORE_RESULT (write (fd, "\n", 1));

		list = g_slist_next (list);
	}

	close (fd);
	return 0;
}

#define KBSTATE_MOD 0
#define KBSTATE_KEY 1
#define KBSTATE_ACT 2
#define KBSTATE_DT1 3
/* Legacy state: silently consume the D2 line if old configs still have one,
 * and migrate Change Tab + non-empty D2 (= "Relative") to a signed D1 value. */
#define KBSTATE_DT2_LEGACY 4

#define STRIP_WHITESPACE \
	while (buf[0] == ' ' || buf[0] == '\t') \
		buf++; \
	len = strlen (buf); \
	while (buf[len] == ' ' || buf[len] == '\t') \
	{ \
		buf[len] = 0; \
		len--; \
	} \

static inline int
key_load_kbs_helper_mod (char *buf, GdkModifierType *out)
{
	int n, len, mod = 0;

	/* First strip off the fluff */
	STRIP_WHITESPACE

	if (strcmp (buf, "None") == 0)
	{
		*out = 0;
		return 0;
	}
	for (n = 0; n < len; n++)
	{
		switch (buf[n])
		{
		case 'C':
			mod |= GDK_CONTROL_MASK;
			break;
		case 'A':
			mod |= GDK_MOD1_MASK;
			break;
		case 'S':
			mod |= GDK_SHIFT_MASK;
			break;
		default:
			return 1;
		}
	}

	*out = mod;
	return 0;
}

static int
key_load_kbs (void)
{
	char *buf, *ibuf;
	struct stat st;
	struct key_binding *kb = NULL;
	struct key_binding *last_kb = NULL;
	int fd, len, state = 0, pnt = 0;
	int change_tab_idx;
	gboolean reprocess = FALSE;
	guint keyval;
	GdkModifierType mod = 0;
	off_t size;

	change_tab_idx = key_get_action_from_string ("Change Tab");

	fd = poxchat_open_file ("keybindings.conf", O_RDONLY, 0, 0);
	if (fd < 0)
	{
		ibuf = g_strdup (default_kb_cfg);
		size = strlen (default_kb_cfg);
	}
	else
	{
		if (fstat (fd, &st) != 0)
		{
			close (fd);
			return 1;
		}

		ibuf = g_malloc(st.st_size);
		HC_IGNORE_RESULT (read (fd, ibuf, st.st_size));
		size = st.st_size;
		close (fd);
	}

	if (keybind_list)
	{
		g_slist_free_full (keybind_list, key_free);
		keybind_list = NULL;
	}

	while (reprocess || buf_get_line (ibuf, &buf, &pnt, size))
	{
		reprocess = FALSE;

		if (buf[0] == '#')
			continue;
		if (strlen (buf) == 0)
			continue;

		switch (state)
		{
		case KBSTATE_MOD:
			kb = g_new0 (struct key_binding, 1);

			/* New format */
			if (strncmp (buf, "ACCEL=", 6) == 0)
			{
				buf += 6;

				gtk_accelerator_parse (buf, &keyval, &mod);


				kb->keyval = keyval;
				kb->mod = key_modifier_get_valid (mod);

				state = KBSTATE_ACT;
				continue;
			}

			if (key_load_kbs_helper_mod (buf, &mod))
				goto corrupt_file;

			kb->mod = mod;

			state = KBSTATE_KEY;
			continue;

		case KBSTATE_KEY:
			STRIP_WHITESPACE

			keyval = gdk_keyval_from_name (buf);
			if (keyval == 0)
			{
				g_free (ibuf);
				return 2;
			}

			kb->keyval = keyval;

			state = KBSTATE_ACT;
			continue;

		case KBSTATE_ACT:
			STRIP_WHITESPACE

			kb->action = key_get_action_from_string (buf);

			if (kb->action == KEY_MAX_ACTIONS + 1)
			{
				g_free (ibuf);
				return 3;
			}

			state = KBSTATE_DT1;
			continue;

		case KBSTATE_DT1:
			kb->data1 = NULL;

			while (buf[0] == ' ' || buf[0] == '\t')
				buf++;

			if (buf[0] != 'D' || buf[1] != '1')
			{
				g_free (ibuf);
				return 4;
			}

			if (buf[2] == ':')
			{
				len = strlen (buf);
				/* Add one for the NULL, subtract 3 for the "D1:" */
				len++;
				len -= 3;
				kb->data1 = g_strndup (&buf[3], len);
			}
			/* "D1!" leaves data1 as NULL */

			keybind_list = g_slist_append (keybind_list, kb);
			last_kb = kb;
			kb = NULL;
			state = KBSTATE_DT2_LEGACY;
			continue;

		case KBSTATE_DT2_LEGACY:
			while (buf[0] == ' ' || buf[0] == '\t')
				buf++;

			if (buf[0] == 'D' && buf[1] == '2')
			{
				/* Legacy D2 line. Migrate Change Tab entries: a non-empty
				 * D2 (historically "Relative") meant the D1 number was a
				 * relative offset. Express that as a signed D1 instead. */
				if (last_kb && last_kb->action == change_tab_idx
				    && buf[2] == ':' && buf[3] != 0
				    && last_kb->data1 && last_kb->data1[0]
				    && last_kb->data1[0] != '+'
				    && last_kb->data1[0] != '-'
				    && last_kb->data1[0] >= '0' && last_kb->data1[0] <= '9')
				{
					char *new_d1 = g_strdup_printf ("+%s", last_kb->data1);
					g_free (last_kb->data1);
					last_kb->data1 = new_d1;
				}
				state = KBSTATE_MOD;
				continue;
			}

			/* No D2 line — current line belongs to the next entry. */
			state = KBSTATE_MOD;
			reprocess = TRUE;
			continue;
		}
	}
	g_free (ibuf);
	return 0;

corrupt_file:
	g_free (ibuf);
	g_free (kb);
	return 5;
}

/* ***** Key actions start here *********** */

/* See the NOTES above --AGL */

/* "Run command" */
static int
key_action_handle_command (GtkWidget * wid, KEY_EVENT_PARAM, char *d1,
									char *d2, struct session *sess)
{
	int ii, oi, len;
	char out[2048], d = 0;

	if (!d1)
		return 0;

	len = strlen (d1);

	/* Replace each "\n" substring with '\n' */
	for (ii = oi = 0; ii < len; ii++)
	{
		d = d1[ii];
		if (d == '\\')
		{
			ii++;
			d = d1[ii];
			if (d == 'n')
				out[oi++] = '\n';
			else if (d == '\\')
				out[oi++] = '\\';
			else
			{
				out[oi++] = '\\';
				out[oi++] = d;
			}
			continue;
		}
		out[oi++] = d;
	}
	out[oi] = 0;

	handle_multiline (sess, out, 0, 0);
	return 0;
}

/*
 * Check if the given session is inside the main window. This predicate
 * is passed to lastact_getfirst() as a way to filter out detached sessions.
 * XXX: Consider moving this in a different file?
 */
static int
session_check_is_tab(session *sess)
{
	if (!sess || !sess->gui)
		return FALSE;

	return (sess->gui->is_tab);
}

static int
key_action_page_switch (GtkWidget * wid, KEY_EVENT_PARAM, char *d1,
								char *d2, struct session *sess)
{
	session *newsess;
	int len, i, num;
	gboolean relative;

	if (!d1)
		return 1;

	len = strlen (d1);
	if (!len)
		return 1;

	if (strcasecmp(d1, "auto") == 0)
	{
		/* Auto switch makes no sense in detached sessions */
		if (!sess->gui->is_tab)
			return 1;

		/* Obtain a session with recent activity */
		newsess = lastact_getfirst(session_check_is_tab);

		if (newsess)
		{
			/*
			 * Only sessions in the current window should be considered (i.e.
			 * we don't want to move the focus on a different window). This
			 * call could, in theory, do this, but we checked before that
			 * newsess->gui->is_tab and sess->gui->is_tab.
			 */
			mg_bring_tofront_sess(newsess);
			return 0;
		}
		else
			return 1;
	}

	for (i = 0; i < len; i++)
	{
		if (d1[i] < '0' || d1[i] > '9')
		{
			if (i == 0 && (d1[i] == '+' || d1[i] == '-'))
				continue;
			else
				return 1;
		}
	}

	/* Sign prefix (+/-) means relative; bare number means absolute. */
	relative = (d1[0] == '+' || d1[0] == '-');
	num = atoi (d1);
	if (!relative)
		num--;
	mg_switch_page (relative, num);
	return 0;
}

int
key_action_insert (GtkWidget * wid, KEY_EVENT_PARAM, char *d1, char *d2,
						 struct session *sess)
{
	int tmp_pos;

	if (!d1)
		return 1;

	tmp_pos = SPELL_ENTRY_GET_POS (wid);
	SPELL_ENTRY_INSERT (wid, d1, strlen (d1), &tmp_pos);
	SPELL_ENTRY_SET_POS (wid, tmp_pos);
	return 2;
}

/* handles PageUp/Down keys */
static int
key_action_scroll_page (GtkWidget * wid, KEY_EVENT_PARAM, char *d1,
								char *d2, struct session *sess)
{
	int value, end;
	GtkAdjustment *adj;
	enum scroll_type { PAGE_TOP, PAGE_BOTTOM, PAGE_UP, PAGE_DOWN, LINE_UP, LINE_DOWN };
	int type = PAGE_DOWN;

	if (d1)
	{
		if (!g_ascii_strcasecmp (d1, "top"))
			type = PAGE_TOP;
		else if (!g_ascii_strcasecmp (d1, "bottom"))
			type = PAGE_BOTTOM;
		else if (!g_ascii_strcasecmp (d1, "up"))
			type = PAGE_UP;
		else if (!g_ascii_strcasecmp (d1, "down"))
			type = PAGE_DOWN;
		else if (!g_ascii_strcasecmp (d1, "+1"))
			type = LINE_DOWN;
		else if (!g_ascii_strcasecmp (d1, "-1"))
			type = LINE_UP;
	}

	if (!sess)
		return 0;

	adj = GTK_XTEXT (sess->gui->xtext)->adj;
	end = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_lower (adj) - gtk_adjustment_get_page_size (adj);

	switch (type)
	{
	case PAGE_TOP:
		value = 0;
		break;

	case PAGE_BOTTOM:
		value = end;
		break;

	case PAGE_UP:
		value = gtk_adjustment_get_value (adj) - (gtk_adjustment_get_page_size (adj) - 1);
		break;

	case PAGE_DOWN:
		value = gtk_adjustment_get_value (adj) + (gtk_adjustment_get_page_size (adj) - 1);
		break;

	case LINE_UP:
		value = gtk_adjustment_get_value (adj) - 1.0;
		break;

	case LINE_DOWN:
		value = gtk_adjustment_get_value (adj) + 1.0;
		break;
	}

	if (value < 0)
		value = 0;
	if (value > end)
		value = end;

	gtk_adjustment_set_value (adj, value);

	return 0;
}

static int
key_action_set_buffer (GtkWidget * wid, KEY_EVENT_PARAM, char *d1, char *d2,
							  struct session *sess)
{
	if (!d1)
		return 1;
	if (d1[0] == 0)
		return 1;

	SPELL_ENTRY_SET_TEXT (wid, d1);
	SPELL_ENTRY_SET_POS (wid, -1);

	return 2;
}

static int
key_action_history_up (GtkWidget * wid, KEY_EVENT_PARAM, char *d1, char *d2,
							  struct session *sess)
{
	char *new_line;

	/* In multi-line mode, only trigger history when cursor is on the first line */
	if (HEX_IS_INPUT_EDIT (wid))
	{
		if (hex_input_edit_get_cursor_line (HEX_INPUT_EDIT (wid)) > 0)
		{
			hex_input_edit_move_cursor_up (HEX_INPUT_EDIT (wid));
			return 2;
		}
	}

	new_line = history_up (&sess->history, SPELL_ENTRY_GET_TEXT (wid));
	if (new_line)
	{
		SPELL_ENTRY_SET_TEXT (wid, new_line);
		SPELL_ENTRY_SET_POS (wid, -1);
	}

	return 2;
}

static int
key_action_history_down (GtkWidget * wid, KEY_EVENT_PARAM, char *d1,
								 char *d2, struct session *sess)
{
	char *new_line;

	/* In multi-line mode, only trigger history when cursor is on the last line */
	if (HEX_IS_INPUT_EDIT (wid))
	{
		HexInputEdit *ie = HEX_INPUT_EDIT (wid);
		if (hex_input_edit_get_cursor_line (ie) < hex_input_edit_get_line_count (ie) - 1)
		{
			hex_input_edit_move_cursor_down (ie);
			return 2;
		}
	}

	new_line = history_down (&sess->history);
	if (new_line)
	{
		SPELL_ENTRY_SET_TEXT (wid, new_line);
		SPELL_ENTRY_SET_POS (wid, 0);
	}

	return 2;
}

/* old data that we reuse */
static struct gcomp_data old_gcomp;

/* work on the data, ie return only channels */
static int
double_chan_cb (session *lsess, GList **list)
{
	if (lsess->type == SESS_CHANNEL)
		*list = g_list_prepend(*list, lsess->channel);
	return TRUE;
}

/* convert a slist -> list. */
static GList *
chanlist_double_list (GSList *inlist)
{
	GList *list = NULL;
	g_slist_foreach(inlist, (GFunc)double_chan_cb, &list);
	return list;
}

/* handle commands */
static int
double_cmd_cb (struct popup *pop, GList **list)
{
	*list = g_list_prepend(*list, pop->name);
	return TRUE;
}

/* convert a slist -> list. */
static GList *
cmdlist_double_list (GSList *inlist)
{
	GList *list = NULL;
	g_slist_foreach (inlist, (GFunc)double_cmd_cb, &list);
	return list;
}

static char *
gcomp_nick_func (char *data)
{
	if (data)
		return ((struct User *)data)->nick;
	return "";
}

void
key_action_tab_clean(void)
{
	if (old_gcomp.elen)
	{
		old_gcomp.data[0] = 0;
		old_gcomp.elen = 0;
	}
}

/* For use in sorting the user list for completion

This sorts everyone by the last talked time except your own nick
which is forced to the bottom of the list to avoid completing your
own name, which is very unlikely.
*/
static int
talked_recent_cmp (struct User *a, struct User *b)
{
	if (a->me)
		return -1;

	if (b->me)
		return 1;

	if (a->lasttalk < b->lasttalk)
		return -1;

	if (a->lasttalk > b->lasttalk)
		return 1;

	return 0;
}

#define COMP_BUF 2048

static inline glong
len_to_offset (const char *str, glong len)
{
	return g_utf8_pointer_to_offset (str, str + len);
}

static inline glong
offset_to_len (const char *str, glong offset)
{
	return g_utf8_offset_to_pointer (str, offset) - str;
}

static int
key_action_tab_comp (GtkWidget *t, KEY_EVENT_PARAM, char *d1, char *d2,
							struct session *sess)
{
	int len = 0, elen = 0, i = 0, cursor_pos, ent_start = 0, comp = 0, prefix_len, skip_len = 0;
	gboolean is_nick = FALSE, is_cmd = FALSE, found = FALSE, has_nick_prefix = FALSE;
	char ent[CHANLEN], *postfix = NULL, *result, *ch;
	GList *list = NULL, *tmp_list = NULL;
	const char *text;
	HcCompletion *gcomp = NULL;
	GString *buf;

	/* force the IM Context to reset */
	SPELL_ENTRY_SET_EDITABLE (t, FALSE);
	SPELL_ENTRY_SET_EDITABLE (t, TRUE);

	text = SPELL_ENTRY_GET_TEXT (t);
	if (text[0] == 0)
		return 1;

	len = g_utf8_strlen (text, -1); /* must be null terminated */

	cursor_pos = SPELL_ENTRY_GET_POS (t);

	/* handle "nick: " or "nick " or "#channel "*/
	ch = g_utf8_find_prev_char(text, g_utf8_offset_to_pointer(text,cursor_pos));
	if (ch && ch[0] == ' ')
	{
		skip_len++;
		ch = g_utf8_find_prev_char(text, ch);
		if (!ch)
			return 2;

		cursor_pos = g_utf8_pointer_to_offset(text, ch);
		if (cursor_pos && (g_utf8_get_char_validated(ch, -1) == ':' ||
					g_utf8_get_char_validated(ch, -1) == ',' ||
					g_utf8_get_char_validated (ch, -1) == g_utf8_get_char_validated (prefs.hex_completion_suffix, -1)))
		{
			skip_len++;
		}
		else
			cursor_pos = g_utf8_pointer_to_offset(text, g_utf8_offset_to_pointer(ch, 1));
	}

	comp = skip_len;

	/* store the text following the cursor for reinsertion later */
	if ((cursor_pos + skip_len) < len)
		postfix = g_utf8_offset_to_pointer(text, cursor_pos + skip_len);

	for (ent_start = cursor_pos; ; --ent_start)
	{
		if (ent_start == 0)
			break;
		ch = g_utf8_offset_to_pointer(text, ent_start - 1);
		if (ch && ch[0] == ' ')
			break;
	}

	if (ent_start == 0 && text[0] == prefs.hex_input_command_char[0])
	{
		ent_start++;
		is_cmd = TRUE;
	}
	else if (strchr (sess->server->chantypes, text[ent_start]) == NULL)
	{
		is_nick = TRUE;
		if (strchr (sess->server->nick_prefixes, text[ent_start]) != NULL)
		{
			if (ent_start == 0)
				has_nick_prefix = TRUE;
			ent_start++;
		}
	}

	prefix_len = ent_start;
	elen = cursor_pos - ent_start;

	g_utf8_strncpy (ent, g_utf8_offset_to_pointer (text, prefix_len), elen);

	if (sess->type == SESS_DIALOG && is_nick)
	{
		/* tab in a dialog completes the other person's name */
		if (rfc_ncasecmp (sess->channel, ent, elen) == 0)
		{
			result =  sess->channel;
			is_nick = FALSE;
		}
		else
			return 2;
	}
	else
	{
		if (is_nick)
		{
			gcomp = hc_completion_new((HcCompletionFunc)gcomp_nick_func);
			tmp_list = userlist_double_list(sess); /* create a temp list so we can free the memory */
			if (prefs.hex_completion_sort == 1)	/* sort in last-talk order? */
				tmp_list = g_list_sort (tmp_list, (void *)talked_recent_cmp);
		}
		else
		{
			gcomp = hc_completion_new (NULL);
			if (is_cmd)
			{
				tmp_list = cmdlist_double_list (command_list);
				for(i = 0; xc_cmds[i].name != NULL ; i++)
				{
					tmp_list = g_list_prepend (tmp_list, xc_cmds[i].name);
				}
				tmp_list = plugin_command_list(tmp_list);
			}
			else
				tmp_list = chanlist_double_list (sess_list);
		}
		tmp_list = g_list_reverse(tmp_list); /* make the comp entries turn up in the right order */
		hc_completion_set_compare (gcomp, (HcCompletionStrncmpFunc)rfc_ncasecmp);
		if (tmp_list)
		{
			hc_completion_add_items (gcomp, tmp_list);
			g_list_free (tmp_list);
		}

		if (comp && !(rfc_ncasecmp(old_gcomp.data, ent, old_gcomp.elen) == 0))
		{
			key_action_tab_clean ();
			comp = 0;
		}

		list = hc_completion_complete_utf8 (gcomp, comp ? old_gcomp.data : ent, &result);

		if (result == NULL) /* No matches found */
		{
			hc_completion_free(gcomp);
			return 2;
		}

		if (comp) /* existing completion */
		{
			while(list) /* find the current entry */
			{
				if(rfc_ncasecmp(list->data, ent, elen) == 0)
				{
					found = TRUE;
					break;
				}
				list = list->next;
			}

			if (found)
			{
				if (!(d1 && d1[0])) /* not holding down shift */
				{
					if (g_list_next(list) == NULL)
						list = g_list_first(list);
					else
						list = g_list_next(list);
				}
				else
				{
					if (g_list_previous(list) == NULL)
						list = g_list_last(list);
					else
						list = g_list_previous(list);
				}
				g_free(result);
				result = (char*)list->data;
			}
			else
			{
				g_free(result);
				hc_completion_free(gcomp);
				return 2;
			}
		}
		else
		{
			g_strlcpy(old_gcomp.data, ent, sizeof(old_gcomp.data));
			old_gcomp.elen = elen;

			/* Get the first nick and put out the data for future nickcompletes */
			if (prefs.hex_completion_amount > 0 && g_list_length (list) <= (guint) prefs.hex_completion_amount)
			{
				g_free(result);
				result = (char*)list->data;
			}
			else
			{
				/* bash style completion */
				if (g_list_next(list) != NULL)
				{
					buf = g_string_sized_new (MAX(COMP_BUF, len + NICKLEN));
					if (strlen (result) > elen) /* the largest common prefix is larger than nick, change the data */
					{
						if (prefix_len)
							g_string_append_len (buf, text, offset_to_len (text, prefix_len));
						g_string_append (buf, result);
						cursor_pos = buf->len;
						g_free(result);
						if (postfix)
						{
							g_string_append_c (buf, ' ');
							g_string_append (buf, postfix);
						}
						SPELL_ENTRY_SET_TEXT (t, buf->str);
						SPELL_ENTRY_SET_POS (t, len_to_offset (buf->str, cursor_pos));
						g_string_erase (buf, 0, -1);
					}
					else
						g_free(result);

					while (list)
					{
						len = buf->len;
						elen = strlen (list->data);	/* next item to add */
						if (len + elen + 2 >= COMP_BUF) /* +2 is space + null */
						{
							PrintText (sess, buf->str);
							g_string_erase (buf, 0, -1);
						}
						g_string_append (buf, (char*)list->data);
						g_string_append_c (buf, ' ');
						list = list->next;
					}
					PrintText (sess, buf->str);
					hc_completion_free(gcomp);
					g_string_free (buf, TRUE);
					return 2;
				}
				/* Only one matching entry */
				g_free(result);
				result = list->data;
			}
		}
	}

	if(result)
	{
		buf = g_string_sized_new (len + NICKLEN);
		if (prefix_len)
			g_string_append_len (buf, text, offset_to_len (text, prefix_len));
		g_string_append (buf, result);
		if((!prefix_len || has_nick_prefix) && is_nick && prefs.hex_completion_suffix[0] != '\0')
			g_string_append_unichar (buf, g_utf8_get_char_validated (prefs.hex_completion_suffix, -1));
		g_string_append_c (buf, ' ');
		cursor_pos = buf->len;
		if (postfix)
			g_string_append (buf, postfix);
		SPELL_ENTRY_SET_TEXT (t, buf->str);
		SPELL_ENTRY_SET_POS (t, len_to_offset (buf->str, cursor_pos));
		g_string_free (buf, TRUE);
	}
	if (gcomp)
		hc_completion_free(gcomp);
	return 2;
}
#undef COMP_BUF

static int
key_action_comp_chng (GtkWidget * wid, KEY_EVENT_PARAM, char *d1, char *d2,
		struct session *sess)
{
	key_action_tab_comp(wid, evt, d1, d2, sess);
	return 2;
}


static int
key_action_replace (GtkWidget * wid, KEY_EVENT_PARAM, char *d1, char *d2,
						  struct session *sess)
{
	replace_handle (wid);
	return 1;
}


static int
key_action_move_tab_left (GtkWidget * wid, KEY_EVENT_PARAM, char *d1,
								  char *d2, struct session *sess)
{
	mg_move_tab (sess, +1);
	return 2;						  /* don't allow default action */
}

static int
key_action_move_tab_right (GtkWidget * wid, KEY_EVENT_PARAM, char *d1,
									char *d2, struct session *sess)
{
	mg_move_tab (sess, -1);
	return 2;						  /* -''- */
}

static int
key_action_move_tab_family_left (GtkWidget * wid, KEY_EVENT_PARAM, char *d1,
								  char *d2, struct session *sess)
{
	mg_move_tab_family (sess, +1);
	return 2;						  /* don't allow default action */
}

static int
key_action_move_tab_family_right (GtkWidget * wid, KEY_EVENT_PARAM, char *d1,
									char *d2, struct session *sess)
{
	mg_move_tab_family (sess, -1);
	return 2;						  /* -''- */
}

static int
key_action_put_history (GtkWidget * wid, KEY_EVENT_PARAM, char *d1,
									char *d2, struct session *sess)
{
	history_add (&sess->history, SPELL_ENTRY_GET_TEXT (wid));
	SPELL_ENTRY_SET_TEXT (wid, "");
	return 2;						  /* -''- */
}

static int
key_action_next_tab (GtkWidget * wid, KEY_EVENT_PARAM, char *d1,
                     char *d2, struct session *sess)
{
	mg_switch_page (TRUE, 1);
	return 0;
}

static int
key_action_prev_tab (GtkWidget * wid, KEY_EVENT_PARAM, char *d1,
                     char *d2, struct session *sess)
{
	mg_switch_page (TRUE, -1);
	return 0;
}


/* -------- */

static void
replace_handle (GtkWidget *t)
{
	const char *text, *postfix_pnt;
	struct popup *pop;
	GSList *list = replace_list;
	char word[256];
	char postfix[256];
	char outbuf[4096];
	int c, len, xlen;

	text = SPELL_ENTRY_GET_TEXT (t);

	len = strlen (text);
	if (len < 1)
		return;

	for (c = len - 1; c > 0; c--)
	{
		if (text[c] == ' ')
			break;
	}
	if (text[c] == ' ')
		c++;
	xlen = c;
	if (len - c >= (sizeof (word) - 12))
		return;
	if (len - c < 1)
		return;
	memcpy (word, &text[c], len - c);
	word[len - c] = 0;
	len = strlen (word);
	if (word[0] == '\'' && word[len] == '\'')
		return;
	postfix_pnt = NULL;
	for (c = 0; c < len; c++)
	{
		if (word[c] == '\'')
		{
			postfix_pnt = &word[c + 1];
			word[c] = 0;
			break;
		}
	}

	if (postfix_pnt != NULL)
	{
		if (strlen (postfix_pnt) > sizeof (postfix) - 12)
			return;
		strcpy (postfix, postfix_pnt);
	}
	while (list)
	{
		pop = (struct popup *) list->data;
		if (strcmp (pop->name, word) == 0)
		{
			memcpy (outbuf, text, xlen);
			outbuf[xlen] = 0;
			if (postfix_pnt == NULL)
				g_snprintf (word, sizeof (word), "%s", pop->cmd);
			else
				g_snprintf (word, sizeof (word), "%s%s", pop->cmd, postfix);
			g_strlcat (outbuf, word, sizeof(outbuf));
			SPELL_ENTRY_SET_TEXT (t, outbuf);
			SPELL_ENTRY_SET_POS (t, -1);
			return;
		}
		list = list->next;
	}
}
