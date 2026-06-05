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

/* hex-input-edit.c — Custom editable input widget.
 *
 * Uses xtext's rendering pipeline (xtext-render.c) for mIRC format codes
 * and emoji sprite rendering, with a simple GString buffer + cursor model.
 */

#include <string.h>
#include <ctype.h>
#include <math.h>

#include "../common/cfgfiles.h"
#include "../common/poxchatc.h"
#include "../common/url.h"
#include "../common/fe.h"

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <pango/pango.h>
#include <cairo.h>
#include <glib/gi18n.h>

#include "hex-input-edit.h"
#include "xtext-render.h"
#include "xtext-emoji.h"
#include "palette.h"

#ifdef WIN32
#include <windows.h>
#include "../common/typedef.h"
#endif

/* ── Enchant runtime loading ────────────────────────────────────────── */
/* File-scoped copies via g_module_open — loaded at runtime. */

struct EnchantDict;
struct EnchantBroker;

typedef void (*EnchantDictDescribeFn) (const char * const lang_tag,
                                       const char * const provider_name,
                                       const char * const provider_desc,
                                       const char * const provider_file,
                                       void * user_data);

static struct EnchantBroker * (*enchant_broker_init) (void);
static void (*enchant_broker_free) (struct EnchantBroker * broker);
static void (*enchant_broker_free_dict) (struct EnchantBroker * broker, struct EnchantDict * dict);
static void (*enchant_broker_list_dicts) (struct EnchantBroker * broker, EnchantDictDescribeFn fn, void * user_data);
static struct EnchantDict * (*enchant_broker_request_dict) (struct EnchantBroker * broker, const char *const tag);

static int (*enchant_dict_check) (struct EnchantDict * dict, const char *const word, ssize_t len);
static char ** (*enchant_dict_suggest) (struct EnchantDict * dict, const char *const word, ssize_t len, size_t * out_n_suggs);
static void (*enchant_dict_free_suggestions) (struct EnchantDict * dict, char **suggestions);
static void (*enchant_dict_describe) (struct EnchantDict * dict, EnchantDictDescribeFn fn, void * user_data);
static void (*enchant_dict_add_to_personal) (struct EnchantDict * dict, const char *const word, ssize_t len);

static gboolean hie_have_enchant = FALSE;

static void
hie_initialize_enchant (void)
{
	GModule *enchant;
	gpointer funcptr;
	gsize i;
	static gboolean initialized = FALSE;
	const char * const libnames[] = {
#ifdef G_OS_WIN32
		"libenchant.dll",
#endif
#ifdef G_OS_UNIX
		"libenchant.so.1",
		"libenchant.so.2",
		"libenchant-2.so.2",
#endif
#ifdef __APPLE__
		"libenchant.dylib",
#endif
	};

	if (initialized)
		return;
	initialized = TRUE;

	for (i = 0; i < G_N_ELEMENTS (libnames); ++i)
	{
		enchant = g_module_open (libnames[i], 0);
		if (enchant)
		{
			hie_have_enchant = TRUE;
			break;
		}
	}

	if (!hie_have_enchant)
		return;

#define LOAD_SYMBOL(name, func) G_STMT_START { \
	if (!g_module_symbol (enchant, name, &funcptr)) { \
		hie_have_enchant = FALSE; \
		return; \
	} \
	(func) = funcptr; \
} G_STMT_END

	LOAD_SYMBOL ("enchant_broker_init", enchant_broker_init);
	LOAD_SYMBOL ("enchant_broker_free", enchant_broker_free);
	LOAD_SYMBOL ("enchant_broker_free_dict", enchant_broker_free_dict);
	LOAD_SYMBOL ("enchant_broker_list_dicts", enchant_broker_list_dicts);
	LOAD_SYMBOL ("enchant_broker_request_dict", enchant_broker_request_dict);
	LOAD_SYMBOL ("enchant_dict_check", enchant_dict_check);
	LOAD_SYMBOL ("enchant_dict_suggest", enchant_dict_suggest);
	LOAD_SYMBOL ("enchant_dict_free_suggestions", enchant_dict_free_suggestions);
	LOAD_SYMBOL ("enchant_dict_describe", enchant_dict_describe);
	LOAD_SYMBOL ("enchant_dict_add_to_personal", enchant_dict_add_to_personal);

#undef LOAD_SYMBOL
}

/* --- Undo snapshot --- */
typedef struct {
	char *text;
	int cursor_byte;
} UndoSnapshot;

#define UNDO_MAX 50

static void
undo_snapshot_free (gpointer data)
{
	UndoSnapshot *snap = data;
	g_free (snap->text);
	g_free (snap);
}

/* --- Margins/padding --- */
#define HPAD 6		/* horizontal padding inside widget */
#define VPAD 4		/* vertical padding inside widget */

/* --- Caret blink interval (ms) --- */
#define BLINK_INTERVAL 500

/* --- Signals --- */
enum {
	SIGNAL_ACTIVATE,
	SIGNAL_WORD_CHECK,
	SIGNAL_IMAGE_PASTE,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static gboolean
spell_accumulator (GSignalInvocationHint *hint, GValue *return_accu,
                   const GValue *handler_return, gpointer data)
{
	(void) hint; (void) data;
	gboolean ret = g_value_get_boolean (handler_return);
	g_value_set_boolean (return_accu, ret);
	return ret;
}

/* --- Private data --- */
struct _HexInputEditPriv
{
	/* Text content */
	GString *text;			/* raw text (may contain mIRC format codes) */
	char *cached_text;		/* returned by get_text (owned) */

	/* Parsed rendering data (rebuilt on text change) */
	unsigned char *stripped_str;
	int stripped_len;
	xtext_fmt_span *fmt_spans;
	int fmt_span_count;
	xtext_emoji_info *emoji_list;
	int emoji_count;
	guint16 *raw_to_stripped_map;

	/* Cursor and selection (byte offsets in raw text) */
	int cursor_byte;		/* byte offset of cursor in text->str */
	int sel_anchor_byte;	/* selection anchor byte, -1 if no selection */

	/* Display */
	PangoLayout *layout;
	PangoFontDescription *font_desc;
	int ascent;				/* font ascent in pixels */
	int descent;			/* font descent in pixels */
	int fontsize;			/* ascent + descent */
	int line_height;		/* line height for multi-line sizing */

	/* Configuration */
	gboolean multiline;
	gboolean editable;		/* whether text can be modified */
	int max_lines;
	int max_chars;			/* max character count (0 = unlimited) */
	int width_chars;		/* preferred width in characters (0 = default) */
	int max_width_chars;	/* max width in characters (0 = default) */
	int alloc_width;		/* last allocated width, for detecting resize */

	/* Caret blink */
	guint blink_timer;
	gboolean caret_visible;
	gboolean has_focus;

	/* Input method */
	GtkIMContext *im_context;
	char *preedit_str;
	PangoAttrList *preedit_attrs;
	int preedit_cursor;

	/* Shared resources */
	xtext_emoji_cache *emoji_cache;
	const GdkRGBA *palette;	/* points to external palette, not owned */

	/* Scrolling */
	int scroll_x;			/* pixel offset scrolled left (single-line horiz scroll) */
	int scroll_y;			/* pixel offset scrolled up (0 = top visible) */

	/* Dirty flags */
	gboolean parse_dirty;	/* text changed, need re-parse */

	/* Undo/redo */
	GPtrArray *undo_stack;		/* array of UndoSnapshot* */
	int undo_pos;				/* current position in stack (-1 = nothing) */
	gboolean undo_frozen;		/* prevent recording during undo/redo */

	/* Spell checking (Enchant) */
	struct EnchantBroker *broker;
	GHashTable           *dict_hash;	/* lang tag → EnchantDict* */
	GSList               *dict_list;	/* list of active EnchantDict* */
	gboolean              spell_checked;	/* spell checking enabled */

	/* Spell context menu state (valid while popover is open) */
	char                 *spell_word;		/* misspelled word (owned) */
	int                   spell_word_start;	/* raw byte offset of word start */
	int                   spell_word_end;	/* raw byte offset of word end */
	char                **spell_suggestions;/* suggestions array (owned by enchant) */
	size_t                spell_n_suggs;
	struct EnchantDict   *spell_dict;		/* dict used for suggestions (not owned) */

	/* URL hover tracking (Ctrl+hover cursor feedback) */
	double last_mouse_x;
	double last_mouse_y;
	gboolean mouse_in_widget;
	GtkEventController *root_key_ctrl;	/* capture-phase key ctrl on toplevel */
};

G_DEFINE_TYPE (HexInputEdit, hex_input_edit, GTK_TYPE_WIDGET)

static void ensure_cursor_visible (HexInputEdit *edit);
static void update_url_cursor (HexInputEdit *edit, double x, double y, gboolean ctrl_held);

/* ── Spell checking helpers ──────────────────────────────────────────── */

static gboolean
default_word_check (HexInputEdit *edit, const gchar *word)
{
	gboolean result = TRUE;
	GSList *li;

	if (!hie_have_enchant)
		return result;

	if (g_unichar_isalpha (*word) == FALSE)
		return FALSE;

	for (li = edit->priv->dict_list; li; li = g_slist_next (li))
	{
		struct EnchantDict *dict = (struct EnchantDict *) li->data;
		if (enchant_dict_check (dict, word, strlen (word)) == 0)
		{
			result = FALSE;
			break;
		}
	}
	return result;
}

static gboolean
word_misspelled (HexInputEdit *edit, const char *word)
{
	gboolean ret = FALSE;

	if (!word || !*word)
		return FALSE;

	g_signal_emit (edit, signals[SIGNAL_WORD_CHECK], 0, word, &ret);
	return ret;
}

/* Append PANGO_UNDERLINE_ERROR attrs for misspelled words in stripped text.
 * Operates on the stripped string (no mIRC codes), whose byte offsets
 * map directly to the PangoLayout indices. */
static void
spell_check_attrs (HexInputEdit *edit, PangoAttrList *attrs)
{
	HexInputEditPriv *priv = edit->priv;
	const char *text;
	int text_len, n_chars, n_attrs;
	PangoLogAttr *log_attrs;
	const GdkRGBA *pal;

	if (!hie_have_enchant || !priv->spell_checked)
		return;
	if (!priv->dict_list)
		return;
	if (!priv->stripped_str || priv->stripped_len <= 0)
		return;

	text = (const char *) priv->stripped_str;
	text_len = priv->stripped_len;
	n_chars = g_utf8_strlen (text, text_len);
	n_attrs = n_chars + 1;
	log_attrs = g_new0 (PangoLogAttr, n_attrs);

	pango_get_log_attrs (text, text_len, -1,
	                     pango_language_get_default (),
	                     log_attrs, n_attrs);

	pal = priv->palette ? priv->palette : colors;

	for (int i = 0; i < n_attrs; i++)
	{
		if (log_attrs[i].is_word_start && log_attrs[i].is_word_boundary)
		{
			int word_end;
			for (word_end = i; word_end < n_attrs; word_end++)
			{
				if (log_attrs[word_end].is_word_end &&
				    log_attrs[word_end].is_word_boundary)
					break;
			}

			if (word_end > i)
			{
				char *ws = g_utf8_offset_to_pointer (text, i);
				char *we = g_utf8_offset_to_pointer (text, word_end);
				char *word = g_strndup (ws, we - ws);

				if (word_misspelled (edit, word))
				{
					int byte_start = (int)(ws - text);
					int byte_end = (int)(we - text);

					PangoAttribute *unline =
						pango_attr_underline_new (PANGO_UNDERLINE_ERROR);
					unline->start_index = byte_start;
					unline->end_index = byte_end;
					pango_attr_list_insert (attrs, unline);

					PangoAttribute *ucolor =
						pango_attr_underline_color_new (
							(guint16)(pal[COL_SPELL].red * 65535),
							(guint16)(pal[COL_SPELL].green * 65535),
							(guint16)(pal[COL_SPELL].blue * 65535));
					ucolor->start_index = byte_start;
					ucolor->end_index = byte_end;
					pango_attr_list_insert (attrs, ucolor);
				}

				g_free (word);
			}
		}
	}

	g_free (log_attrs);
}

/* =============================== */
/* === Helpers                  === */
/* =============================== */

/* Convert a character offset to a byte offset in a UTF-8 string.
 * Returns byte length if char_off >= char count. */
static int
char_to_byte (const char *str, int len, int char_off)
{
	const char *p = str;
	const char *end = str + len;
	int i = 0;
	if (char_off < 0)
		return len;
	while (p < end && i < char_off)
	{
		p = g_utf8_next_char (p);
		i++;
	}
	return (int)(p - str);
}

/* Convert a byte offset to a character offset. */
static int
byte_to_char (const char *str, int byte_off)
{
	return (int) g_utf8_pointer_to_offset (str, str + byte_off);
}

/* Get the stripped-text byte offset for the cursor position. */
static int
cursor_stripped_offset (HexInputEditPriv *priv)
{
	if (!priv->raw_to_stripped_map)
		return 0;
	return xtext_raw_to_stripped (priv->raw_to_stripped_map,
	                              priv->text->len, priv->cursor_byte);
}

/* Selection range in raw bytes (ordered). Returns FALSE if no selection. */
static gboolean
get_selection_bytes (HexInputEditPriv *priv, int *start_out, int *end_out)
{
	if (priv->sel_anchor_byte < 0)
		return FALSE;
	int a = priv->sel_anchor_byte;
	int b = priv->cursor_byte;
	if (a == b)
		return FALSE;
	*start_out = MIN (a, b);
	*end_out = MAX (a, b);
	return TRUE;
}

/* Push an undo snapshot (call before mutating text).
 * The stack stores states such that undo_pos points to the entry
 * representing the current text. On mutation, we save the current state
 * and undo_pos advances to that new entry. Undo decrements, redo increments. */
static void
push_undo_snapshot (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;

	if (priv->undo_frozen)
		return;

	if (!priv->undo_stack)
		priv->undo_stack = g_ptr_array_new_with_free_func (undo_snapshot_free);

	/* Discard any redo entries beyond current position */
	while ((int) priv->undo_stack->len > priv->undo_pos + 1)
		g_ptr_array_remove_index (priv->undo_stack,
		                          priv->undo_stack->len - 1);

	/* Avoid duplicate: skip if top of stack matches current text */
	if (priv->undo_stack->len > 0)
	{
		UndoSnapshot *top = g_ptr_array_index (priv->undo_stack,
		                                       priv->undo_stack->len - 1);
		if (strcmp (top->text, priv->text->str) == 0)
			return;
	}

	/* Cap at UNDO_MAX — remove oldest */
	if ((int) priv->undo_stack->len >= UNDO_MAX)
		g_ptr_array_remove_index (priv->undo_stack, 0);

	UndoSnapshot *snap = g_new (UndoSnapshot, 1);
	snap->text = g_strdup (priv->text->str);
	snap->cursor_byte = priv->cursor_byte;
	g_ptr_array_add (priv->undo_stack, snap);
	priv->undo_pos = (int) priv->undo_stack->len - 1;
}

/* =============================== */
/* === Text parsing             === */
/* =============================== */

static void
reparse_text (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;

	/* Free old data */
	g_free (priv->stripped_str);
	g_free (priv->fmt_spans);
	g_free (priv->emoji_list);
	g_free (priv->raw_to_stripped_map);

	priv->stripped_str = NULL;
	priv->stripped_len = 0;
	priv->fmt_spans = NULL;
	priv->fmt_span_count = 0;
	priv->emoji_list = NULL;
	priv->emoji_count = 0;
	priv->raw_to_stripped_map = NULL;

	if (priv->text->len > 0)
	{
		int slen, scnt, ecnt;
		xtext_parse_formats ((const unsigned char *) priv->text->str,
		                     (int) priv->text->len,
		                     &priv->stripped_str, &slen,
		                     &priv->fmt_spans, &scnt,
		                     priv->emoji_cache ? &priv->emoji_list : NULL,
		                     priv->emoji_cache ? &ecnt : NULL,
		                     &priv->raw_to_stripped_map,
		                     priv->emoji_cache != NULL);
		priv->stripped_len = slen;
		priv->fmt_span_count = scnt;
		priv->emoji_count = priv->emoji_cache ? ecnt : 0;
	}

	priv->parse_dirty = FALSE;
}

/* Update the PangoLayout for rendering. */
static void
update_layout (HexInputEdit *edit, int width)
{
	HexInputEditPriv *priv = edit->priv;

	if (priv->parse_dirty)
		reparse_text (edit);

	if (!priv->layout)
	{
		PangoContext *pctx = gtk_widget_get_pango_context (GTK_WIDGET (edit));
		priv->layout = pango_layout_new (pctx);
	}

	/* Set text */
	if (priv->stripped_str && priv->stripped_len > 0)
		pango_layout_set_text (priv->layout,
		                       (const char *) priv->stripped_str,
		                       priv->stripped_len);
	else
		pango_layout_set_text (priv->layout, "", 0);

	/* Build attrlist from format spans */
	PangoAttrList *attrs = NULL;
	if (priv->fmt_spans && priv->stripped_len > 0)
	{
		xtext_format_data fd = {
			.stripped_str = priv->stripped_str,
			.stripped_len = priv->stripped_len,
			.fmt_spans = priv->fmt_spans,
			.fmt_span_count = priv->fmt_span_count,
			.emoji_list = priv->emoji_list,
			.emoji_count = priv->emoji_count
		};
		attrs = xtext_build_attrlist (&fd, 0, priv->stripped_len,
		                              priv->palette ? priv->palette : colors,
		                              priv->fontsize, priv->ascent);
	}
	else
	{
		attrs = pango_attr_list_new ();
	}

	/* Spell-check: append underline-error attrs for misspelled words */
	spell_check_attrs (edit, attrs);

	/* Insert preedit string and attributes */
	if (priv->preedit_str && priv->preedit_str[0])
	{
		int stripped_cursor = cursor_stripped_offset (priv);
		const char *base = pango_layout_get_text (priv->layout);
		int base_len = (int) strlen (base);
		int pe_len = (int) strlen (priv->preedit_str);

		/* Build new text with preedit inserted */
		GString *full = g_string_sized_new (base_len + pe_len);
		g_string_append_len (full, base, stripped_cursor);
		g_string_append_len (full, priv->preedit_str, pe_len);
		g_string_append (full, base + stripped_cursor);
		pango_layout_set_text (priv->layout, full->str, full->len);
		g_string_free (full, TRUE);

		/* Shift existing attributes past preedit */
		if (priv->preedit_attrs)
		{
			PangoAttrList *pe_attrs = pango_attr_list_copy (priv->preedit_attrs);
			PangoAttrIterator *iter = pango_attr_list_get_iterator (pe_attrs);
			do {
				int start, end;
				pango_attr_iterator_range (iter, &start, &end);
				GSList *a_list = pango_attr_iterator_get_attrs (iter);
				for (GSList *l = a_list; l; l = l->next)
				{
					PangoAttribute *a = l->data;
					a->start_index += stripped_cursor;
					a->end_index += stripped_cursor;
					pango_attr_list_insert (attrs, pango_attribute_copy (a));
				}
				g_slist_free_full (a_list, (GDestroyNotify) pango_attribute_destroy);
			} while (pango_attr_iterator_next (iter));
			pango_attr_iterator_destroy (iter);
			pango_attr_list_unref (pe_attrs);
		}
	}

	pango_layout_set_attributes (priv->layout, attrs);
	pango_attr_list_unref (attrs);

	/* Font */
	if (priv->font_desc)
		pango_layout_set_font_description (priv->layout, priv->font_desc);

	/* Wrapping for multiline */
	if (priv->multiline && width > 2 * HPAD)
	{
		pango_layout_set_width (priv->layout, (width - 2 * HPAD) * PANGO_SCALE);
		pango_layout_set_wrap (priv->layout, PANGO_WRAP_WORD_CHAR);
	}
	else
	{
		pango_layout_set_width (priv->layout, -1);
	}
}

/* =============================== */
/* === Font metrics              === */
/* =============================== */

static void
update_font_metrics (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;
	PangoContext *pctx = gtk_widget_get_pango_context (GTK_WIDGET (edit));
	if (priv->font_desc)
		pango_context_set_font_description (pctx, priv->font_desc);
	PangoFontMetrics *metrics = pango_context_get_metrics (pctx, NULL, NULL);

	priv->ascent = pango_font_metrics_get_ascent (metrics) / PANGO_SCALE;
	priv->descent = pango_font_metrics_get_descent (metrics) / PANGO_SCALE;
	priv->fontsize = priv->ascent + priv->descent;

	priv->line_height = priv->fontsize;

	pango_font_metrics_unref (metrics);
}

/* =============================== */
/* === Text mutation            === */
/* =============================== */

static void
mark_dirty (HexInputEdit *edit)
{
	edit->priv->parse_dirty = TRUE;
	g_free (edit->priv->cached_text);
	edit->priv->cached_text = NULL;
	ensure_cursor_visible (edit);
	gtk_widget_queue_draw (GTK_WIDGET (edit));
	gtk_widget_queue_resize (GTK_WIDGET (edit));
}

/* Adjust scroll offsets so the cursor is within the visible area. */
static void
ensure_cursor_visible (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;
	int widget_h = gtk_widget_get_height (GTK_WIDGET (edit));
	int widget_w = gtk_widget_get_width (GTK_WIDGET (edit));

	if (!priv->layout || widget_h <= 0 || widget_w <= 0)
		return;

	update_layout (edit, widget_w);

	/* Get cursor position in layout coordinates */
	int stripped_cursor = cursor_stripped_offset (priv);
	PangoRectangle strong;
	pango_layout_get_cursor_pos (priv->layout, stripped_cursor, &strong, NULL);

	/* --- Vertical scrolling (multiline) --- */
	int cursor_top = PANGO_PIXELS (strong.y);
	int cursor_bot = cursor_top + PANGO_PIXELS (strong.height);

	int content_h = widget_h - 2 * VPAD;
	if (content_h > 0)
	{
		if (cursor_bot > priv->scroll_y + content_h)
			priv->scroll_y = cursor_bot - content_h;
		if (cursor_top < priv->scroll_y)
			priv->scroll_y = cursor_top;

		int layout_h = 0;
		pango_layout_get_pixel_size (priv->layout, NULL, &layout_h);
		int max_scroll = layout_h - content_h;
		if (max_scroll < 0) max_scroll = 0;
		if (priv->scroll_y > max_scroll) priv->scroll_y = max_scroll;
		if (priv->scroll_y < 0) priv->scroll_y = 0;
	}

	/* --- Horizontal scrolling (single-line) --- */
	if (!priv->multiline)
	{
		int content_w = widget_w - 2 * HPAD;
		if (content_w > 0)
		{
			int cursor_x = PANGO_PIXELS (strong.x);

			/* Scroll right if cursor is past the visible area */
			if (cursor_x > priv->scroll_x + content_w)
				priv->scroll_x = cursor_x - content_w;

			/* Scroll left if cursor is before the visible area */
			if (cursor_x < priv->scroll_x)
				priv->scroll_x = cursor_x;

			/* Clamp */
			int layout_w = 0;
			pango_layout_get_pixel_size (priv->layout, &layout_w, NULL);
			int max_scroll_x = layout_w - content_w;
			if (max_scroll_x < 0) max_scroll_x = 0;
			if (priv->scroll_x > max_scroll_x) priv->scroll_x = max_scroll_x;
			if (priv->scroll_x < 0) priv->scroll_x = 0;
		}
	}
	else
	{
		priv->scroll_x = 0;
	}
}

/* Delete the current selection. Returns TRUE if something was deleted. */
static gboolean
delete_selection (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;
	int sel_start, sel_end;

	if (!priv->editable)
		return FALSE;
	if (!get_selection_bytes (priv, &sel_start, &sel_end))
		return FALSE;

	push_undo_snapshot (edit);
	g_string_erase (priv->text, sel_start, sel_end - sel_start);
	priv->cursor_byte = sel_start;
	priv->sel_anchor_byte = -1;
	mark_dirty (edit);
	return TRUE;
}

/* Insert text at the current cursor, replacing any selection. */
static void
insert_at_cursor (HexInputEdit *edit, const char *str, int len)
{
	HexInputEditPriv *priv = edit->priv;
	char *filtered = NULL;

	if (!priv->editable)
		return;

	if (len < 0)
		len = (int) strlen (str);

	/* Single-line: strip newlines from pasted/committed text */
	if (!priv->multiline)
	{
		const char *p;
		gboolean has_nl = FALSE;
		for (p = str; p < str + len; p++)
		{
			if (*p == '\n' || *p == '\r')
			{
				has_nl = TRUE;
				break;
			}
		}
		if (has_nl)
		{
			GString *f = g_string_sized_new (len);
			for (p = str; p < str + len; p++)
			{
				if (*p != '\n' && *p != '\r')
					g_string_append_c (f, *p);
			}
			filtered = g_string_free (f, FALSE);
			str = filtered;
			len = (int) strlen (str);
		}
	}

	/* Enforce max_chars limit */
	if (priv->max_chars > 0)
	{
		int cur_chars = (int) g_utf8_strlen (priv->text->str, priv->text->len);
		int sel_chars = 0;
		int sel_s, sel_e;
		if (get_selection_bytes (priv, &sel_s, &sel_e))
			sel_chars = (int) g_utf8_strlen (priv->text->str + sel_s,
			                                  sel_e - sel_s);
		int avail = priv->max_chars - (cur_chars - sel_chars);
		if (avail <= 0)
		{
			g_free (filtered);
			return;
		}
		int insert_chars = (int) g_utf8_strlen (str, len);
		if (insert_chars > avail)
		{
			/* Truncate to avail characters */
			const char *end = g_utf8_offset_to_pointer (str, avail);
			len = (int)(end - str);
		}
	}

	/* push_undo_snapshot before delete_selection would double-push if
	 * there's a selection, so only push here if no selection active. */
	if (priv->sel_anchor_byte < 0 ||
	    priv->sel_anchor_byte == priv->cursor_byte)
		push_undo_snapshot (edit);

	delete_selection (edit);

	g_string_insert_len (priv->text, priv->cursor_byte, str, len);
	priv->cursor_byte += len;
	priv->sel_anchor_byte = -1;
	mark_dirty (edit);
	g_free (filtered);
}

/* =============================== */
/* === Caret blink              === */
/* =============================== */

static gboolean
blink_cb (gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	edit->priv->caret_visible = !edit->priv->caret_visible;
	gtk_widget_queue_draw (GTK_WIDGET (edit));
	return G_SOURCE_CONTINUE;
}

static void
reset_blink (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;
	priv->caret_visible = TRUE;
	if (priv->blink_timer)
		g_source_remove (priv->blink_timer);
	priv->blink_timer = 0;
	if (priv->has_focus)
		priv->blink_timer = g_timeout_add (BLINK_INTERVAL, blink_cb, edit);
	ensure_cursor_visible (edit);
	gtk_widget_queue_draw (GTK_WIDGET (edit));
}

/* =============================== */
/* === IMContext callbacks       === */
/* =============================== */

static void
im_commit_cb (GtkIMContext *ctx, const char *str, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	insert_at_cursor (edit, str, -1);
	reset_blink (edit);
}

static void
im_preedit_changed_cb (GtkIMContext *ctx, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;

	g_free (priv->preedit_str);
	if (priv->preedit_attrs)
		pango_attr_list_unref (priv->preedit_attrs);

	gtk_im_context_get_preedit_string (ctx,
	                                   &priv->preedit_str,
	                                   &priv->preedit_attrs,
	                                   &priv->preedit_cursor);
	gtk_widget_queue_draw (GTK_WIDGET (edit));
}

static gboolean
im_retrieve_surrounding_cb (GtkIMContext *ctx, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;

	gtk_im_context_set_surrounding_with_selection (ctx,
	                                priv->text->str,
	                                (int) priv->text->len,
	                                priv->cursor_byte,
	                                priv->cursor_byte);
	return TRUE;
}

static gboolean
im_delete_surrounding_cb (GtkIMContext *ctx, int offset, int n_chars, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;

	/* Convert char offset/count to byte range */
	int cursor_char = byte_to_char (priv->text->str, priv->cursor_byte);
	int start_char = cursor_char + offset;
	int end_char = start_char + n_chars;

	if (start_char < 0) start_char = 0;

	int start_byte = char_to_byte (priv->text->str, priv->text->len, start_char);
	int end_byte = char_to_byte (priv->text->str, priv->text->len, end_char);

	g_string_erase (priv->text, start_byte, end_byte - start_byte);
	if (priv->cursor_byte > start_byte)
		priv->cursor_byte = start_byte;
	priv->sel_anchor_byte = -1;
	mark_dirty (edit);
	return TRUE;
}

/* =============================== */
/* === Keyboard handling        === */
/* =============================== */

/* Find the previous word boundary (byte offset) scanning backwards from pos. */
static int
find_word_boundary_left (const char *str, int len, int pos)
{
	const char *p;

	if (pos <= 0)
		return 0;

	/* Start one char before pos */
	p = g_utf8_find_prev_char (str, str + pos);
	if (!p)
		return 0;

	/* Skip whitespace backwards */
	while (p > str)
	{
		gunichar ch = g_utf8_get_char (p);
		if (!g_unichar_isspace (ch))
			break;
		p = g_utf8_find_prev_char (str, p);
		if (!p)
			return 0;
	}

	/* Skip non-whitespace backwards */
	while (p > str)
	{
		const char *prev = g_utf8_find_prev_char (str, p);
		if (!prev)
			break;
		gunichar ch = g_utf8_get_char (prev);
		if (g_unichar_isspace (ch))
			break;
		p = prev;
	}

	return (int)(p - str);
}

/* Find the next word boundary (byte offset) scanning forwards from pos. */
static int
find_word_boundary_right (const char *str, int len, int pos)
{
	const char *p = str + pos;
	const char *end = str + len;

	if (pos >= len)
		return len;

	/* Skip non-whitespace forward */
	while (p < end)
	{
		gunichar ch = g_utf8_get_char (p);
		if (g_unichar_isspace (ch))
			break;
		p = g_utf8_next_char (p);
	}

	/* Skip whitespace forward */
	while (p < end)
	{
		gunichar ch = g_utf8_get_char (p);
		if (!g_unichar_isspace (ch))
			break;
		p = g_utf8_next_char (p);
	}

	return (int)(p - str);
}

/* Async paste callback */
static void
paste_text_ready_cb (GObject *source, GAsyncResult *result, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	GdkClipboard *clip = GDK_CLIPBOARD (source);
	char *text = gdk_clipboard_read_text_finish (clip, result, NULL);

	if (text && text[0])
	{
		insert_at_cursor (edit, text, -1);
		reset_blink (edit);
	}
	g_free (text);
	g_object_unref (edit);
}

/* =============================== */
/* === Edit operations          === */
/* =============================== */

/* Mirror the current selection into the PRIMARY clipboard so other apps
 * can middle-click-paste from us. No-op when there is no selection — we
 * don't want to clobber PRIMARY when the user just deselects. */
static void
push_selection_to_primary (HexInputEdit *edit)
{
	int sel_start, sel_end;
	char *sel_text;

	if (!get_selection_bytes (edit->priv, &sel_start, &sel_end))
		return;

	sel_text = g_strndup (edit->priv->text->str + sel_start,
	                      sel_end - sel_start);
	gdk_clipboard_set_text (gtk_widget_get_primary_clipboard (GTK_WIDGET (edit)),
	                        sel_text);
	g_free (sel_text);
}

static void
do_select_all (HexInputEdit *edit)
{
	edit->priv->sel_anchor_byte = 0;
	edit->priv->cursor_byte = (int) edit->priv->text->len;
	push_selection_to_primary (edit);
	reset_blink (edit);
}

static void
do_copy (HexInputEdit *edit)
{
	int sel_start, sel_end;
	if (get_selection_bytes (edit->priv, &sel_start, &sel_end))
	{
		char *sel_text = g_strndup (edit->priv->text->str + sel_start,
		                            sel_end - sel_start);
		GdkClipboard *clip = gdk_display_get_clipboard (
		    gdk_display_get_default ());
		gdk_clipboard_set_text (clip, sel_text);
		g_free (sel_text);
	}
}

static void
do_cut (HexInputEdit *edit)
{
	int sel_start, sel_end;
	if (get_selection_bytes (edit->priv, &sel_start, &sel_end))
	{
		char *sel_text = g_strndup (edit->priv->text->str + sel_start,
		                            sel_end - sel_start);
		GdkClipboard *clip = gdk_display_get_clipboard (
		    gdk_display_get_default ());
		gdk_clipboard_set_text (clip, sel_text);
		g_free (sel_text);
		delete_selection (edit);
		reset_blink (edit);
	}
}

/* Async image-paste callback: encode the pasted texture as PNG and hand it to
 * the embedder via "image-paste" (which uploads it and inserts a link). */
static void
paste_texture_ready_cb (GObject *source, GAsyncResult *result, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	GdkClipboard *clip = GDK_CLIPBOARD (source);
	GdkTexture *texture = gdk_clipboard_read_texture_finish (clip, result, NULL);

	if (texture)
	{
		GBytes *png = gdk_texture_save_to_png_bytes (texture);
		if (png)
		{
			g_signal_emit (edit, signals[SIGNAL_IMAGE_PASTE], 0, png);
			g_bytes_unref (png);
		}
		g_object_unref (texture);
	}
	g_object_unref (edit);
}

static void
do_paste (HexInputEdit *edit, GdkClipboard *clip)
{
	GdkContentFormats *formats = gdk_clipboard_get_formats (clip);

	/* If the clipboard offers an image and uploading is enabled, divert it to
	 * the image-paste path instead of pasting text. */
	if (prefs.hex_url_image_upload_enable && formats &&
	    gdk_content_formats_contain_gtype (formats, GDK_TYPE_TEXTURE))
	{
		g_object_ref (edit);
		gdk_clipboard_read_texture_async (clip, NULL,
		                                  paste_texture_ready_cb, edit);
		return;
	}

	g_object_ref (edit);
	gdk_clipboard_read_text_async (clip, NULL,
	                               paste_text_ready_cb, edit);
}

static void
do_undo (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;
	if (!priv->undo_stack || priv->undo_stack->len == 0)
		return;

	priv->undo_frozen = TRUE;

	/* Save current state for redo if at the tip and text differs */
	if (priv->undo_pos == (int) priv->undo_stack->len - 1)
	{
		UndoSnapshot *top = g_ptr_array_index (priv->undo_stack,
		                                       priv->undo_pos);
		if (strcmp (priv->text->str, top->text) != 0)
		{
			UndoSnapshot *cur = g_new (UndoSnapshot, 1);
			cur->text = g_strdup (priv->text->str);
			cur->cursor_byte = priv->cursor_byte;
			g_ptr_array_add (priv->undo_stack, cur);
			priv->undo_pos = (int) priv->undo_stack->len - 1;
		}
	}

	if (priv->undo_pos > 0)
	{
		priv->undo_pos--;
		UndoSnapshot *snap = g_ptr_array_index (priv->undo_stack,
		                                        priv->undo_pos);
		g_string_assign (priv->text, snap->text);
		priv->cursor_byte = CLAMP (snap->cursor_byte, 0,
		                           (int) priv->text->len);
		priv->sel_anchor_byte = -1;
		mark_dirty (edit);
		reset_blink (edit);
	}
	priv->undo_frozen = FALSE;
}

static void
do_redo (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;
	if (priv->undo_stack &&
	    priv->undo_pos + 1 < (int) priv->undo_stack->len)
	{
		priv->undo_pos++;
		UndoSnapshot *snap = g_ptr_array_index (priv->undo_stack,
		                                        priv->undo_pos);
		priv->undo_frozen = TRUE;
		g_string_assign (priv->text, snap->text);
		priv->cursor_byte = CLAMP (snap->cursor_byte, 0,
		                           (int) priv->text->len);
		priv->sel_anchor_byte = -1;
		priv->undo_frozen = FALSE;
		mark_dirty (edit);
		reset_blink (edit);
	}
}

/* =============================== */
/* === Context menu (popover)   === */
/* =============================== */

static gboolean
popover_cleanup_idle (gpointer data)
{
	GtkWidget *popover = GTK_WIDGET (data);
	if (gtk_widget_get_parent (popover))
		gtk_widget_unparent (popover);
	g_object_unref (popover);
	return G_SOURCE_REMOVE;
}

static void
popover_closed_cb (GtkPopover *popover, gpointer data)
{
	g_object_ref (popover);
	g_idle_add (popover_cleanup_idle, popover);
}

static void
action_cut (GtkWidget *widget, const char *name, GVariant *param)
{
	do_cut (HEX_INPUT_EDIT (widget));
}

static void
action_copy (GtkWidget *widget, const char *name, GVariant *param)
{
	do_copy (HEX_INPUT_EDIT (widget));
}

static void
action_paste (GtkWidget *widget, const char *name, GVariant *param)
{
	do_paste (HEX_INPUT_EDIT (widget), gtk_widget_get_clipboard (widget));
}

static void
action_select_all (GtkWidget *widget, const char *name, GVariant *param)
{
	do_select_all (HEX_INPUT_EDIT (widget));
}

static void
action_undo (GtkWidget *widget, const char *name, GVariant *param)
{
	do_undo (HEX_INPUT_EDIT (widget));
}

static void
action_redo (GtkWidget *widget, const char *name, GVariant *param)
{
	do_redo (HEX_INPUT_EDIT (widget));
}

/* Free spell suggestion state from a previous context menu. */
static void
spell_menu_cleanup (HexInputEdit *edit)
{
	HexInputEditPriv *priv = edit->priv;

	if (priv->spell_suggestions && priv->spell_dict)
		enchant_dict_free_suggestions (priv->spell_dict, priv->spell_suggestions);
	priv->spell_suggestions = NULL;
	priv->spell_n_suggs = 0;
	priv->spell_dict = NULL;
	g_free (priv->spell_word);
	priv->spell_word = NULL;
}

/* Find the misspelled word at widget coordinates (x, y).
 * If found, populates priv->spell_word/suggestions/dict and returns TRUE. */
static gboolean
spell_word_at_xy (HexInputEdit *edit, double x, double y)
{
	HexInputEditPriv *priv = edit->priv;
	const char *text;
	int text_len, n_chars, n_attrs;
	PangoLogAttr *log_attrs;
	int click_stripped;

	spell_menu_cleanup (edit);

	if (!hie_have_enchant || !priv->spell_checked || !priv->dict_list)
		return FALSE;
	if (!priv->stripped_str || priv->stripped_len <= 0)
		return FALSE;
	if (!priv->layout)
		return FALSE;

	/* Get stripped-text byte offset at click position */
	{
		int layout_h = 0;
		int widget_h = gtk_widget_get_height (GTK_WIDGET (edit));
		pango_layout_get_pixel_size (priv->layout, NULL, &layout_h);
		int content_h = widget_h - 2 * VPAD;
		double text_y;
		if (layout_h <= content_h)
		{
			text_y = (widget_h - layout_h) * 0.53;
			if (text_y < VPAD) text_y = VPAD;
		}
		else
		{
			text_y = VPAD - priv->scroll_y;
		}

		int lx = (int)((x - HPAD + priv->scroll_x) * PANGO_SCALE);
		int ly = (int)((y - text_y) * PANGO_SCALE);
		int index, trailing;
		pango_layout_xy_to_index (priv->layout, lx, ly, &index, &trailing);
		click_stripped = index;  /* byte offset in stripped text */
	}

	text = (const char *) priv->stripped_str;
	text_len = priv->stripped_len;
	n_chars = g_utf8_strlen (text, text_len);
	n_attrs = n_chars + 1;
	log_attrs = g_new0 (PangoLogAttr, n_attrs);

	pango_get_log_attrs (text, text_len, -1,
	                     pango_language_get_default (),
	                     log_attrs, n_attrs);

	/* Convert click byte offset to char offset */
	int click_char = (int) g_utf8_pointer_to_offset (text, text + click_stripped);
	if (click_char < 0) click_char = 0;
	if (click_char >= n_attrs) click_char = n_attrs - 1;

	/* Find the word containing this character */
	int ws_char = click_char, we_char = click_char;

	/* Scan back to word start */
	while (ws_char > 0 &&
	       !(log_attrs[ws_char].is_word_start && log_attrs[ws_char].is_word_boundary))
		ws_char--;

	/* Scan forward to word end */
	while (we_char < n_attrs &&
	       !(log_attrs[we_char].is_word_end && log_attrs[we_char].is_word_boundary))
		we_char++;

	gboolean found = FALSE;

	if (we_char > ws_char)
	{
		char *word_start = g_utf8_offset_to_pointer (text, ws_char);
		char *word_end = g_utf8_offset_to_pointer (text, we_char);
		char *word = g_strndup (word_start, word_end - word_start);

		if (word_misspelled (edit, word))
		{
			int stripped_byte_start = (int)(word_start - text);
			int stripped_byte_end = (int)(word_end - text);

			/* Convert stripped byte offsets to raw byte offsets */
			int raw_start = xtext_stripped_to_raw (priv->raw_to_stripped_map,
			                                       (int) priv->text->len,
			                                       stripped_byte_start);
			int raw_end = xtext_stripped_to_raw (priv->raw_to_stripped_map,
			                                     (int) priv->text->len,
			                                     stripped_byte_end);

			priv->spell_word = word;
			priv->spell_word_start = raw_start;
			priv->spell_word_end = raw_end;
			priv->spell_dict = (struct EnchantDict *) priv->dict_list->data;
			priv->spell_suggestions = enchant_dict_suggest (
				priv->spell_dict, word, -1, &priv->spell_n_suggs);
			found = TRUE;
		}
		else
		{
			g_free (word);
		}
	}

	g_free (log_attrs);
	return found;
}

/* Replace misspelled word with a suggestion */
static void
action_spell_replace (GtkWidget *widget, const char *name, GVariant *param)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (widget);
	HexInputEditPriv *priv = edit->priv;
	const char *replacement;

	if (!param || !priv->spell_word)
		return;

	replacement = g_variant_get_string (param, NULL);
	if (!replacement || !*replacement)
		return;

	push_undo_snapshot (edit);
	int rlen = (int) strlen (replacement);
	int old_len = priv->spell_word_end - priv->spell_word_start;
	g_string_erase (priv->text, priv->spell_word_start, old_len);
	g_string_insert_len (priv->text, priv->spell_word_start, replacement, rlen);
	priv->cursor_byte = priv->spell_word_start + rlen;
	priv->sel_anchor_byte = -1;
	mark_dirty (edit);
}

/* Add word to personal dictionary */
static void
action_spell_add (GtkWidget *widget, const char *name, GVariant *param)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (widget);
	HexInputEditPriv *priv = edit->priv;

	if (!hie_have_enchant || !priv->spell_word || !priv->spell_dict)
		return;

	enchant_dict_add_to_personal (priv->spell_dict, priv->spell_word, -1);
	mark_dirty (edit);  /* recheck to remove underline */
}

static void
show_context_menu (HexInputEdit *edit, double x, double y)
{
	HexInputEditPriv *priv = edit->priv;
	gboolean has_sel = (priv->sel_anchor_byte >= 0 &&
	                    priv->sel_anchor_byte != priv->cursor_byte);
	gboolean can_undo = (priv->undo_stack && priv->undo_pos > 0);
	gboolean can_redo = (priv->undo_stack &&
	                     priv->undo_pos + 1 < (int) priv->undo_stack->len);

	/* Update action enabled states */
	gtk_widget_action_set_enabled (GTK_WIDGET (edit), "edit.undo", can_undo);
	gtk_widget_action_set_enabled (GTK_WIDGET (edit), "edit.redo", can_redo);
	gtk_widget_action_set_enabled (GTK_WIDGET (edit), "edit.cut", has_sel);
	gtk_widget_action_set_enabled (GTK_WIDGET (edit), "edit.copy", has_sel);
	gtk_widget_action_set_enabled (GTK_WIDGET (edit), "edit.select-all",
	                               priv->text->len > 0);

	/* Check for misspelled word under click */
	gboolean has_spell = spell_word_at_xy (edit, x, y);

	/* Menu model */
	GMenu *menu = g_menu_new ();

	/* Spell suggestions section (at the top) */
	if (has_spell)
	{
		GMenu *spell_section = g_menu_new ();

		if (priv->spell_suggestions && priv->spell_n_suggs > 0)
		{
			size_t i;
			for (i = 0; i < priv->spell_n_suggs && i < 10; i++)
			{
				GMenuItem *item = g_menu_item_new (
					priv->spell_suggestions[i], NULL);
				g_menu_item_set_action_and_target (item, "spell.replace",
					"s", priv->spell_suggestions[i]);
				g_menu_append_item (spell_section, item);
				g_object_unref (item);
			}
		}
		else
		{
			g_menu_append (spell_section, _("(no suggestions)"), NULL);
		}

		g_menu_append_section (menu, NULL, G_MENU_MODEL (spell_section));
		g_object_unref (spell_section);

		/* Add to dictionary */
		GMenu *add_section = g_menu_new ();
		char *add_label = g_strdup_printf (_("Add \"%s\" to Dictionary"),
		                                    priv->spell_word);
		g_menu_append (add_section, add_label, "spell.add");
		g_free (add_label);
		g_menu_append_section (menu, NULL, G_MENU_MODEL (add_section));
		g_object_unref (add_section);
	}

	GMenu *section1 = g_menu_new ();
	g_menu_append (section1, "_Undo", "edit.undo");
	g_menu_append (section1, "_Redo", "edit.redo");
	g_menu_append_section (menu, NULL, G_MENU_MODEL (section1));
	g_object_unref (section1);

	GMenu *section2 = g_menu_new ();
	g_menu_append (section2, "Cu_t", "edit.cut");
	g_menu_append (section2, "_Copy", "edit.copy");
	g_menu_append (section2, "_Paste", "edit.paste");
	g_menu_append_section (menu, NULL, G_MENU_MODEL (section2));
	g_object_unref (section2);

	GMenu *section3 = g_menu_new ();
	g_menu_append (section3, "Select _All", "edit.select-all");
	g_menu_append_section (menu, NULL, G_MENU_MODEL (section3));
	g_object_unref (section3);

	/* Popover */
	GtkWidget *popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
	g_object_unref (menu);

	gtk_widget_set_parent (popover, GTK_WIDGET (edit));
	GdkRectangle rect = { (int) x, (int) y, 1, 1 };
	gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
	gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);

	g_signal_connect (popover, "closed",
	                  G_CALLBACK (popover_closed_cb), edit);
	gtk_popover_popup (GTK_POPOVER (popover));
}

static gboolean
key_pressed_cb (GtkEventControllerKey *controller, guint keyval,
                guint keycode, GdkModifierType state, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;
	gboolean shift = (state & GDK_SHIFT_MASK) != 0;
	gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;

	/* Let IMContext handle first */
	if (gtk_im_context_filter_keypress (priv->im_context,
	        gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (controller))))
		return TRUE;

	/* --- Ctrl shortcuts --- */
	if (ctrl)
	{
		switch (keyval)
		{
		case GDK_KEY_a:
		case GDK_KEY_A:
			do_select_all (edit);
			return TRUE;

		case GDK_KEY_c:
		case GDK_KEY_C:
			do_copy (edit);
			return TRUE;

		case GDK_KEY_x:
		case GDK_KEY_X:
			do_cut (edit);
			return TRUE;

		case GDK_KEY_v:
		case GDK_KEY_V:
			do_paste (edit, gtk_widget_get_clipboard (GTK_WIDGET (edit)));
			return TRUE;

		case GDK_KEY_z:
		case GDK_KEY_Z:
			if (shift)
				do_redo (edit);
			else
				do_undo (edit);
			return TRUE;

		case GDK_KEY_y:
		case GDK_KEY_Y:
			do_redo (edit);
			return TRUE;

		case GDK_KEY_Left:
		case GDK_KEY_KP_Left:
		{
			/* Ctrl+Left: word movement */
			if (shift && priv->sel_anchor_byte < 0)
				priv->sel_anchor_byte = priv->cursor_byte;
			priv->cursor_byte = find_word_boundary_left (
			    priv->text->str, (int) priv->text->len, priv->cursor_byte);
			if (!shift)
				priv->sel_anchor_byte = -1;
			if (shift)
				push_selection_to_primary (edit);
			reset_blink (edit);
			return TRUE;
		}

		case GDK_KEY_Right:
		case GDK_KEY_KP_Right:
		{
			/* Ctrl+Right: word movement */
			if (shift && priv->sel_anchor_byte < 0)
				priv->sel_anchor_byte = priv->cursor_byte;
			priv->cursor_byte = find_word_boundary_right (
			    priv->text->str, (int) priv->text->len, priv->cursor_byte);
			if (!shift)
				priv->sel_anchor_byte = -1;
			if (shift)
				push_selection_to_primary (edit);
			reset_blink (edit);
			return TRUE;
		}

		default:
			break;
		}
	}

	switch (keyval)
	{
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		if (priv->multiline && shift)
		{
			/* Insert newline */
			insert_at_cursor (edit, "\n", 1);
			reset_blink (edit);
			return TRUE;
		}
		/* Activate (send message) */
		g_signal_emit (edit, signals[SIGNAL_ACTIVATE], 0);
		return TRUE;

	case GDK_KEY_BackSpace:
	{
		if (delete_selection (edit))
		{
			reset_blink (edit);
			return TRUE;
		}
		if (priv->cursor_byte > 0)
		{
			const char *prev = g_utf8_find_prev_char (priv->text->str,
			                                           priv->text->str + priv->cursor_byte);
			if (prev)
			{
				int prev_byte = (int)(prev - priv->text->str);
				push_undo_snapshot (edit);
				g_string_erase (priv->text, prev_byte,
				                priv->cursor_byte - prev_byte);
				priv->cursor_byte = prev_byte;
				priv->sel_anchor_byte = -1;
				mark_dirty (edit);
			}
		}
		reset_blink (edit);
		return TRUE;
	}

	case GDK_KEY_Delete:
	case GDK_KEY_KP_Delete:
	{
		if (delete_selection (edit))
		{
			reset_blink (edit);
			return TRUE;
		}
		if (priv->cursor_byte < (int) priv->text->len)
		{
			const char *next = g_utf8_next_char (priv->text->str + priv->cursor_byte);
			int next_byte = (int)(next - priv->text->str);
			push_undo_snapshot (edit);
			g_string_erase (priv->text, priv->cursor_byte,
			                next_byte - priv->cursor_byte);
			priv->sel_anchor_byte = -1;
			mark_dirty (edit);
		}
		reset_blink (edit);
		return TRUE;
	}

	case GDK_KEY_Left:
	case GDK_KEY_KP_Left:
	{
		if (!shift && priv->sel_anchor_byte >= 0)
		{
			/* Collapse selection to left edge */
			int sel_start, sel_end;
			if (get_selection_bytes (priv, &sel_start, &sel_end))
				priv->cursor_byte = sel_start;
			priv->sel_anchor_byte = -1;
		}
		else if (priv->cursor_byte > 0)
		{
			if (shift && priv->sel_anchor_byte < 0)
				priv->sel_anchor_byte = priv->cursor_byte;
			const char *prev = g_utf8_find_prev_char (priv->text->str,
			                                           priv->text->str + priv->cursor_byte);
			if (prev)
				priv->cursor_byte = (int)(prev - priv->text->str);
			if (!shift)
				priv->sel_anchor_byte = -1;
		}
		if (shift)
			push_selection_to_primary (edit);
		reset_blink (edit);
		return TRUE;
	}

	case GDK_KEY_Right:
	case GDK_KEY_KP_Right:
	{
		if (!shift && priv->sel_anchor_byte >= 0)
		{
			/* Collapse selection to right edge */
			int sel_start, sel_end;
			if (get_selection_bytes (priv, &sel_start, &sel_end))
				priv->cursor_byte = sel_end;
			priv->sel_anchor_byte = -1;
		}
		else if (priv->cursor_byte < (int) priv->text->len)
		{
			if (shift && priv->sel_anchor_byte < 0)
				priv->sel_anchor_byte = priv->cursor_byte;
			const char *next = g_utf8_next_char (priv->text->str + priv->cursor_byte);
			priv->cursor_byte = (int)(next - priv->text->str);
			if (!shift)
				priv->sel_anchor_byte = -1;
		}
		if (shift)
			push_selection_to_primary (edit);
		reset_blink (edit);
		return TRUE;
	}

	case GDK_KEY_Home:
	case GDK_KEY_KP_Home:
		if (shift && priv->sel_anchor_byte < 0)
			priv->sel_anchor_byte = priv->cursor_byte;
		priv->cursor_byte = 0;
		if (!shift)
			priv->sel_anchor_byte = -1;
		if (shift)
			push_selection_to_primary (edit);
		reset_blink (edit);
		return TRUE;

	case GDK_KEY_End:
	case GDK_KEY_KP_End:
		if (shift && priv->sel_anchor_byte < 0)
			priv->sel_anchor_byte = priv->cursor_byte;
		priv->cursor_byte = (int) priv->text->len;
		if (!shift)
			priv->sel_anchor_byte = -1;
		if (shift)
			push_selection_to_primary (edit);
		reset_blink (edit);
		return TRUE;

	case GDK_KEY_Tab:
	case GDK_KEY_ISO_Left_Tab:
		/* Let tab completion in fkeys.c handle this */
		return FALSE;

	case GDK_KEY_Up:
	case GDK_KEY_Down:
		/* Let history navigation in fkeys.c handle this */
		return FALSE;

	default:
		break;
	}

	return FALSE;
}

static void
key_released_cb (GtkEventControllerKey *controller, guint keyval,
                 guint keycode, GdkModifierType state, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	gtk_im_context_filter_keypress (edit->priv->im_context,
	    gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (controller)));
}

/* =============================== */
/* === Mouse handling           === */
/* =============================== */

static int
xy_to_byte_offset (HexInputEdit *edit, double x, double y)
{
	HexInputEditPriv *priv = edit->priv;

	if (!priv->layout)
		return 0;

	/* Compute text_y to match snapshot centering */
	int layout_h = 0;
	int widget_h = gtk_widget_get_height (GTK_WIDGET (edit));
	pango_layout_get_pixel_size (priv->layout, NULL, &layout_h);
	int content_h = widget_h - 2 * VPAD;
	double text_y;
	if (layout_h <= content_h)
	{
		text_y = (widget_h - layout_h) * 0.53;
		if (text_y < VPAD) text_y = VPAD;
	}
	else
	{
		text_y = VPAD - priv->scroll_y;
	}

	/* Adjust for padding and scroll offset */
	int lx = (int)((x - HPAD + priv->scroll_x) * PANGO_SCALE);
	int ly = (int)((y - text_y) * PANGO_SCALE);

	int index, trailing;
	pango_layout_xy_to_index (priv->layout, lx, ly, &index, &trailing);

	/* Pango returns `trailing` as a count of CHARACTERS past `index`, not
	 * bytes — walk forward with g_utf8_next_char so we land on a real UTF-8
	 * boundary in the stripped text. */
	int stripped_off = index;
	if (trailing > 0 && priv->stripped_str && priv->stripped_len > 0)
	{
		const char *p = (const char *) priv->stripped_str + index;
		const char *end = (const char *) priv->stripped_str + priv->stripped_len;
		for (int t = 0; t < trailing && p < end; t++)
			p = g_utf8_next_char (p);
		stripped_off = (int) (p - (const char *) priv->stripped_str);
	}

	int raw_off = 0;
	if (priv->raw_to_stripped_map && priv->stripped_len > 0)
		raw_off = xtext_stripped_to_raw (priv->raw_to_stripped_map,
		                                 (int) priv->text->len, stripped_off);
	else
		raw_off = stripped_off;

	return CLAMP (raw_off, 0, (int) priv->text->len);
}

/* Extract the whitespace-delimited word at a byte offset in raw text.
 * Returns a newly allocated string, or NULL if no word found. */
static char *
word_at_byte (const char *str, int len, int byte_off)
{
	int ws = byte_off, we = byte_off;

	while (ws > 0)
	{
		const char *prev = g_utf8_find_prev_char (str, str + ws);
		if (!prev)
			break;
		gunichar ch = g_utf8_get_char (prev);
		if (g_unichar_isspace (ch))
			break;
		ws = (int)(prev - str);
	}

	{
		const char *p = str + we;
		const char *end = str + len;
		while (p < end)
		{
			gunichar ch = g_utf8_get_char (p);
			if (g_unichar_isspace (ch))
				break;
			p = g_utf8_next_char (p);
		}
		we = (int)(p - str);
	}

	if (we <= ws)
		return NULL;
	return g_strndup (str + ws, we - ws);
}

static void
click_pressed_cb (GtkGestureClick *gesture, int n_press, double x, double y,
                  gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;

	/* Ensure we have focus */
	gtk_widget_grab_focus (GTK_WIDGET (edit));

	int byte_off = xy_to_byte_offset (edit, x, y);

	/* Ctrl+click: open URL at click position */
	{
		GdkModifierType state = gtk_event_controller_get_current_event_state (
			GTK_EVENT_CONTROLLER (gesture));
		if ((state & GDK_CONTROL_MASK) && n_press == 1)
		{
			char *word = word_at_byte (priv->text->str, priv->text->len, byte_off);
			if (word)
			{
				int url_type = url_check_word (word);
				if (url_type == WORD_URL || url_type == WORD_EMAIL ||
				    url_type == WORD_HOST || url_type == WORD_HOST6 ||
				    url_type == WORD_PATH)
				{
					fe_open_url (word);
					g_free (word);
					return;
				}
				g_free (word);
			}
		}
	}

	if (n_press == 1)
	{
		priv->cursor_byte = byte_off;
		priv->sel_anchor_byte = byte_off;  /* start potential drag */
	}
	else if (n_press == 2)
	{
		/* Select word at click position */
		const char *str = priv->text->str;
		int len = (int) priv->text->len;
		int word_start = byte_off;
		int word_end = byte_off;

		/* Scan backward to find word start */
		while (word_start > 0)
		{
			const char *prev = g_utf8_find_prev_char (str, str + word_start);
			if (!prev)
				break;
			gunichar ch = g_utf8_get_char (prev);
			if (g_unichar_isspace (ch))
				break;
			word_start = (int)(prev - str);
		}

		/* Scan forward to find word end */
		{
			const char *p = str + word_end;
			const char *end = str + len;
			while (p < end)
			{
				gunichar ch = g_utf8_get_char (p);
				if (g_unichar_isspace (ch))
					break;
				p = g_utf8_next_char (p);
			}
			word_end = (int)(p - str);
		}

		priv->sel_anchor_byte = word_start;
		priv->cursor_byte = word_end;
		push_selection_to_primary (edit);
	}
	else if (n_press == 3)
	{
		/* Select all */
		priv->sel_anchor_byte = 0;
		priv->cursor_byte = (int) priv->text->len;
		push_selection_to_primary (edit);
	}

	reset_blink (edit);
}

static void
click_released_cb (GtkGestureClick *gesture, int n_press, double x, double y,
                   gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;

	/* If anchor == cursor, no selection was made */
	if (priv->sel_anchor_byte == priv->cursor_byte)
		priv->sel_anchor_byte = -1;
}

static void
rclick_pressed_cb (GtkGestureClick *gesture, int n_press, double x, double y,
                   gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	gtk_widget_grab_focus (GTK_WIDGET (edit));
	show_context_menu (edit, x, y);
}

static void
middle_pressed_cb (GtkGestureClick *gesture, int n_press, double x, double y,
                   gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;

	gtk_widget_grab_focus (GTK_WIDGET (edit));

	priv->cursor_byte = xy_to_byte_offset (edit, x, y);
	priv->sel_anchor_byte = -1;
	reset_blink (edit);
	gtk_widget_queue_draw (GTK_WIDGET (edit));

	do_paste (edit, gtk_widget_get_primary_clipboard (GTK_WIDGET (edit)));
}

static void
drag_update_cb (GtkGestureDrag *gesture, double offset_x, double offset_y,
                gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;
	double start_x, start_y;

	gtk_gesture_drag_get_start_point (gesture, &start_x, &start_y);

	int byte_off = xy_to_byte_offset (edit, start_x + offset_x, start_y + offset_y);
	priv->cursor_byte = byte_off;

	push_selection_to_primary (edit);
	ensure_cursor_visible (edit);
	gtk_widget_queue_draw (GTK_WIDGET (edit));
}

/* =============================== */
/* === Focus handling           === */
/* =============================== */

static void
focus_enter_cb (GtkEventControllerFocus *controller, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	edit->priv->has_focus = TRUE;
	gtk_im_context_focus_in (edit->priv->im_context);
	reset_blink (edit);
}

static void
focus_leave_cb (GtkEventControllerFocus *controller, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	edit->priv->has_focus = FALSE;
	edit->priv->caret_visible = FALSE;
	gtk_im_context_focus_out (edit->priv->im_context);
	if (edit->priv->blink_timer)
	{
		g_source_remove (edit->priv->blink_timer);
		edit->priv->blink_timer = 0;
	}
	gtk_widget_queue_draw (GTK_WIDGET (edit));
}

/* =============================== */
/* === Widget vfuncs            === */
/* =============================== */

static void
hex_input_edit_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (widget);
	HexInputEditPriv *priv = edit->priv;
	int width = gtk_widget_get_width (widget);
	int height = gtk_widget_get_height (widget);
	const GdkRGBA *pal = priv->palette ? priv->palette : colors;

	update_layout (edit, width);

	/* Compute vertical offset: center when content fits, scroll when it doesn't */
	int layout_h = 0;
	if (priv->layout)
		pango_layout_get_pixel_size (priv->layout, NULL, &layout_h);
	int content_h = height - 2 * VPAD;
	double text_y;
	if (layout_h <= content_h)
	{
		/* Content fits — center vertically */
		text_y = (height - layout_h) * 0.53;
		if (text_y < VPAD) text_y = VPAD;
	}
	else
	{
		/* Content overflows — apply scroll offset */
		text_y = VPAD - priv->scroll_y;
	}

	/* Get Cairo context */
	graphene_rect_t bounds;
	graphene_rect_init (&bounds, 0, 0, width, height);
	cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &bounds);

	/* Background — rounded rect to match CSS border-radius */
	{
		double r = 5.0;  /* slightly less than CSS 6px to stay inside border */
		double x0 = 0, y0 = 0, x1 = width, y1 = height;
		cairo_new_sub_path (cr);
		cairo_arc (cr, x1 - r, y0 + r, r, -G_PI_2, 0);
		cairo_arc (cr, x1 - r, y1 - r, r, 0, G_PI_2);
		cairo_arc (cr, x0 + r, y1 - r, r, G_PI_2, G_PI);
		cairo_arc (cr, x0 + r, y0 + r, r, G_PI, 3 * G_PI_2);
		cairo_close_path (cr);
	}
	gdk_cairo_set_source_rgba (cr, &pal[XTEXT_BG]);
	cairo_fill (cr);

	/* Clip content to inside border (prevents text overflow when scrolling) */
	cairo_save (cr);
	{
		double r = 5.0;
		double x0 = 1, y0 = 1, x1 = width - 1, y1 = height - 1;
		cairo_new_sub_path (cr);
		cairo_arc (cr, x1 - r, y0 + r, r, -G_PI_2, 0);
		cairo_arc (cr, x1 - r, y1 - r, r, 0, G_PI_2);
		cairo_arc (cr, x0 + r, y1 - r, r, G_PI_2, G_PI);
		cairo_arc (cr, x0 + r, y0 + r, r, G_PI, 3 * G_PI_2);
		cairo_close_path (cr);
	}
	cairo_clip (cr);

	/* Horizontal text origin (HPAD with single-line scroll offset) */
	double text_x = HPAD - priv->scroll_x;

	/* Selection highlight */
	int sel_start_byte, sel_end_byte;
	if (get_selection_bytes (priv, &sel_start_byte, &sel_end_byte) &&
	    priv->raw_to_stripped_map)
	{
		int sel_s = xtext_raw_to_stripped (priv->raw_to_stripped_map,
		                                   priv->text->len, sel_start_byte);
		int sel_e = xtext_raw_to_stripped (priv->raw_to_stripped_map,
		                                   priv->text->len, sel_end_byte);

		/* Get pixel ranges from Pango for each line */
		PangoLayoutIter *iter = pango_layout_get_iter (priv->layout);
		do {
			PangoRectangle line_ext;
			pango_layout_iter_get_line_extents (iter, NULL, &line_ext);
			int line_y = PANGO_PIXELS (line_ext.y) + (int)text_y;
			int line_h = PANGO_PIXELS (line_ext.height);

			int line_start = pango_layout_iter_get_index (iter);
			PangoLayoutLine *pline = pango_layout_iter_get_line_readonly (iter);
			int line_end = line_start + pline->length;

			int s = MAX (sel_s, line_start);
			int e = MIN (sel_e, line_end);

			if (s < e)
			{
				int sx, ex;
				pango_layout_line_index_to_x (pline, s, FALSE, &sx);
				pango_layout_line_index_to_x (pline, e, FALSE, &ex);
				if (sx > ex) { int t = sx; sx = ex; ex = t; }

				gdk_cairo_set_source_rgba (cr, &pal[XTEXT_MARK_BG]);
				cairo_rectangle (cr,
				                 text_x + PANGO_PIXELS (sx), line_y,
				                 PANGO_PIXELS (ex - sx), line_h);
				cairo_fill (cr);
			}
		} while (pango_layout_iter_next_line (iter));
		pango_layout_iter_free (iter);
	}

	/* Text */
	cairo_move_to (cr, text_x, text_y);
	gdk_cairo_set_source_rgba (cr, &pal[XTEXT_FG]);
	pango_cairo_show_layout (cr, priv->layout);

	/* Wrap continuation indicators — draw a small "↳" for wrapped lines
	 * (lines that don't start after a newline). Actual newlines from
	 * Shift+Enter get no indicator. */
	if (priv->multiline && priv->layout &&
	    pango_layout_get_line_count (priv->layout) > 1)
	{
		PangoLayoutIter *wi = pango_layout_get_iter (priv->layout);
		int line_idx = 0;
		do {
			if (line_idx > 0)
			{
				int byte_idx = pango_layout_iter_get_index (wi);
				const char *ltxt = pango_layout_get_text (priv->layout);
				/* Wrapped line: previous char is NOT a newline */
				if (byte_idx > 0 && ltxt[byte_idx - 1] != '\n')
				{
					PangoRectangle ext;
					pango_layout_iter_get_line_extents (wi, NULL, &ext);
					double iy = text_y + PANGO_PIXELS (ext.y);
					double ih = PANGO_PIXELS (ext.height);
					/* Draw a small right-arrow in the left padding area */
					GdkRGBA dim = pal[XTEXT_FG];
					dim.alpha *= 0.35f;
					gdk_cairo_set_source_rgba (cr, &dim);
					/* Simple ">" chevron, vertically centered */
					double cx = 3.0;
					double cy = iy + ih * 0.3;
					double bx = cx + 5.0;
					double by = iy + ih * 0.5;
					double dx = cx;
					double dy = iy + ih * 0.7;
					cairo_move_to (cr, cx, cy);
					cairo_line_to (cr, bx, by);
					cairo_line_to (cr, dx, dy);
					cairo_set_line_width (cr, 1.2);
					cairo_stroke (cr);
				}
			}
			line_idx++;
		} while (pango_layout_iter_next_line (wi));
		pango_layout_iter_free (wi);
	}

	/* Emoji sprites */
	if (priv->emoji_cache && priv->emoji_list)
	{
		/* Compute selection range in stripped coords for emoji highlight */
		int em_sel_s = -1, em_sel_e = -1;
		int em_sel_sb, em_sel_eb;
		if (get_selection_bytes (priv, &em_sel_sb, &em_sel_eb) &&
		    priv->raw_to_stripped_map)
		{
			em_sel_s = xtext_raw_to_stripped (priv->raw_to_stripped_map,
			                                  priv->text->len, em_sel_sb);
			em_sel_e = xtext_raw_to_stripped (priv->raw_to_stripped_map,
			                                  priv->text->len, em_sel_eb);
		}

		for (int ei = 0; ei < priv->emoji_count; ei++)
		{
			xtext_emoji_info *em = &priv->emoji_list[ei];
			cairo_surface_t *surf = xtext_emoji_cache_get (priv->emoji_cache,
			                                                em->filename);
			if (!surf)
				continue;

			PangoRectangle pos;
			pango_layout_index_to_pos (priv->layout, em->stripped_off, &pos);

			double ex = text_x + PANGO_PIXELS (pos.x);
			double ey = text_y + PANGO_PIXELS (pos.y);
			double ew = PANGO_PIXELS (pos.width);
			double eh = PANGO_PIXELS (pos.height);

			/* Center sprite in cell */
			int sw = cairo_image_surface_get_width (surf);
			int sh = cairo_image_surface_get_height (surf);
			double sx = ex + (ew - sw) / 2.0;
			double sy = ey + (eh - sh) / 2.0;

			/* Background behind emoji — use selection BG if within selection */
			gboolean in_sel = (em_sel_s >= 0 &&
			                   em->stripped_off >= em_sel_s &&
			                   em->stripped_off < em_sel_e);
			gdk_cairo_set_source_rgba (cr,
				in_sel ? &pal[XTEXT_MARK_BG] : &pal[XTEXT_BG]);
			cairo_rectangle (cr, ex, ey, ew, eh);
			cairo_fill (cr);

			cairo_set_source_surface (cr, surf, sx, sy);
			cairo_paint_with_alpha (cr, in_sel ? 0.6 : 1.0);
		}
	}

	/* Caret */
	if (priv->has_focus && priv->caret_visible && priv->layout)
	{
		int stripped_cursor = cursor_stripped_offset (priv);

		/* Account for preedit insertion offset */
		if (priv->preedit_str && priv->preedit_str[0])
			stripped_cursor += priv->preedit_cursor;

		PangoRectangle strong;
		pango_layout_get_cursor_pos (priv->layout, stripped_cursor, &strong, NULL);

		double cx = text_x + PANGO_PIXELS (strong.x);
		double cy = text_y + PANGO_PIXELS (strong.y);
		double ch = PANGO_PIXELS (strong.height);

		gdk_cairo_set_source_rgba (cr, &pal[XTEXT_FG]);
		cairo_rectangle (cr, cx, cy, 1.5, ch);
		cairo_fill (cr);
	}

	cairo_restore (cr);  /* undo content clip */
	cairo_destroy (cr);
}

static GtkSizeRequestMode
hex_input_edit_get_request_mode (GtkWidget *widget)
{
	(void)widget;
	return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
hex_input_edit_measure (GtkWidget *widget, GtkOrientation orientation,
                        int for_size, int *minimum, int *natural,
                        int *minimum_baseline, int *natural_baseline)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (widget);
	HexInputEditPriv *priv = edit->priv;

	update_font_metrics (edit);

	if (orientation == GTK_ORIENTATION_HORIZONTAL)
	{
		/* Compute character width from a real 'n' glyph — approximate
		 * metrics under-report for many fonts. */
		int char_w = 0;
		if (priv->width_chars > 0 || priv->max_width_chars > 0)
		{
			PangoLayout *layout = gtk_widget_create_pango_layout (widget, "n");
			if (layout)
			{
				if (priv->font_desc)
					pango_layout_set_font_description (layout, priv->font_desc);
				pango_layout_get_pixel_size (layout, &char_w, NULL);
				g_object_unref (layout);
			}
			if (char_w <= 0)
				char_w = 8;
		}

		if (priv->width_chars > 0)
			*minimum = priv->width_chars * char_w + 2 * HPAD;
		else
			*minimum = 50;

		if (priv->max_width_chars > 0)
			*natural = priv->max_width_chars * char_w + 2 * HPAD;
		else
			*natural = MAX (*minimum, 200);
	}
	else
	{
		/* Vertical: compute based on actual content lines, clamped to max_lines.
		 * for_size is the allocated width; if unconstrained (-1), report single-line
		 * height since we can't know the wrap width yet. */
		int line_count = 1;
		if (priv->multiline && for_size > 0)
		{
			update_layout (edit, for_size);
			if (priv->layout)
				line_count = pango_layout_get_line_count (priv->layout);
		}

		int max_lines = priv->max_lines > 0 ? priv->max_lines : 5;
		int one_line = priv->line_height + 2 * VPAD;
		int nat_lines = MIN (line_count, max_lines);

		*minimum = MAX (one_line, 23);
		*natural = MAX (priv->line_height * nat_lines + 2 * VPAD, 23);
	}

	if (minimum_baseline) *minimum_baseline = -1;
	if (natural_baseline) *natural_baseline = -1;
}

static void
hex_input_edit_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (widget);
	HexInputEditPriv *priv = edit->priv;

	/* If width changed, re-layout with new width and check if line count
	 * changed (wrapping). If so, request height recalculation. */
	if (priv->alloc_width != width)
	{
		int old_lines = priv->layout ? pango_layout_get_line_count (priv->layout) : 1;
		priv->alloc_width = width;
		update_layout (edit, width);
		int new_lines = priv->layout ? pango_layout_get_line_count (priv->layout) : 1;
		if (old_lines != new_lines)
			gtk_widget_queue_resize (widget);
	}
	/* Re-evaluate scroll with final dimensions — force layout update
	 * since text may have changed and we now know the real height. */
	update_layout (edit, width);
	ensure_cursor_visible (edit);
	gtk_widget_queue_draw (widget);
}

static void
hex_input_edit_realize (GtkWidget *widget)
{
	GTK_WIDGET_CLASS (hex_input_edit_parent_class)->realize (widget);

	HexInputEdit *edit = HEX_INPUT_EDIT (widget);
	HexInputEditPriv *priv = edit->priv;

	update_font_metrics (edit);

	/* Connect IM context to this widget's surface */
	gtk_im_context_set_client_widget (priv->im_context, widget);
}

static void
hex_input_edit_unrealize (GtkWidget *widget)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (widget);
	gtk_im_context_set_client_widget (edit->priv->im_context, NULL);

	GTK_WIDGET_CLASS (hex_input_edit_parent_class)->unrealize (widget);
}

/* =============================== */
/* === Init / Finalize          === */
/* =============================== */

static void
hex_input_edit_dispose (GObject *obj)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (obj);
	HexInputEditPriv *priv = edit->priv;

	/* Release im_context during dispose while signal disconnection is
	 * still safe — finalize runs after the class pointer is gone. */
	g_clear_object (&priv->im_context);

	G_OBJECT_CLASS (hex_input_edit_parent_class)->dispose (obj);
}

static void
hex_input_edit_finalize (GObject *obj)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (obj);
	HexInputEditPriv *priv = edit->priv;

	if (priv->blink_timer)
		g_source_remove (priv->blink_timer);
	/* root_key_ctrl is owned by the toplevel — removed via on_root_notify
	 * when widget is unrooted, or destroyed with the toplevel. */
	if (priv->undo_stack)
		g_ptr_array_free (priv->undo_stack, TRUE);

	/* Spell menu state cleanup */
	if (priv->spell_suggestions && priv->spell_dict)
		enchant_dict_free_suggestions (priv->spell_dict, priv->spell_suggestions);
	g_free (priv->spell_word);

	/* Spell checking cleanup */
	if (priv->dict_hash)
		g_hash_table_destroy (priv->dict_hash);
	if (hie_have_enchant && priv->broker)
	{
		GSList *li;
		for (li = priv->dict_list; li; li = g_slist_next (li))
			enchant_broker_free_dict (priv->broker, (struct EnchantDict *) li->data);
		g_slist_free (priv->dict_list);
		enchant_broker_free (priv->broker);
	}

	g_string_free (priv->text, TRUE);
	g_free (priv->cached_text);
	g_free (priv->stripped_str);
	g_free (priv->fmt_spans);
	g_free (priv->emoji_list);
	g_free (priv->raw_to_stripped_map);
	g_free (priv->preedit_str);
	if (priv->preedit_attrs)
		pango_attr_list_unref (priv->preedit_attrs);
	g_clear_object (&priv->layout);
	if (priv->font_desc)
		pango_font_description_free (priv->font_desc);
	g_free (priv);

	G_OBJECT_CLASS (hex_input_edit_parent_class)->finalize (obj);
}

static gboolean
scroll_cb (GtkEventControllerScroll *controller, double dx, double dy,
           gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	HexInputEditPriv *priv = edit->priv;
	int widget_h = gtk_widget_get_height (GTK_WIDGET (edit));
	int layout_h = 0;

	if (!priv->layout)
		return FALSE;

	pango_layout_get_pixel_size (priv->layout, NULL, &layout_h);
	int content_h = widget_h - 2 * VPAD;
	if (layout_h <= content_h)
		return FALSE;  /* no scrolling needed */

	int max_scroll = layout_h - content_h;
	priv->scroll_y += (int)(dy * priv->line_height);
	if (priv->scroll_y < 0) priv->scroll_y = 0;
	if (priv->scroll_y > max_scroll) priv->scroll_y = max_scroll;

	gtk_widget_queue_draw (GTK_WIDGET (edit));
	return TRUE;
}

/* Update mouse cursor: pointer over URLs when Ctrl is held, otherwise text cursor.
 * Called from motion_cb and key press/release to keep cursor in sync with Ctrl state. */
static void
update_url_cursor (HexInputEdit *edit, double x, double y, gboolean ctrl_held)
{
	HexInputEditPriv *priv = edit->priv;

	if (ctrl_held && priv->text->len > 0)
	{
		int byte_off = xy_to_byte_offset (edit, x, y);
		char *word = word_at_byte (priv->text->str, priv->text->len, byte_off);
		if (word)
		{
			int url_type = url_check_word (word);
			g_free (word);
			if (url_type == WORD_URL || url_type == WORD_EMAIL ||
			    url_type == WORD_HOST || url_type == WORD_HOST6 ||
			    url_type == WORD_PATH)
			{
				gtk_widget_set_cursor_from_name (GTK_WIDGET (edit), "pointer");
				return;
			}
		}
	}
	gtk_widget_set_cursor_from_name (GTK_WIDGET (edit), "text");
}

static void
motion_cb (GtkEventControllerMotion *controller, double x, double y, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	edit->priv->last_mouse_x = x;
	edit->priv->last_mouse_y = y;

	GdkModifierType state = gtk_event_controller_get_current_event_state (
		GTK_EVENT_CONTROLLER (controller));
	update_url_cursor (edit, x, y, (state & GDK_CONTROL_MASK) != 0);
}

static void
motion_enter_cb (GtkEventControllerMotion *controller, double x, double y, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	edit->priv->mouse_in_widget = TRUE;
	edit->priv->last_mouse_x = x;
	edit->priv->last_mouse_y = y;
}

static void
motion_leave_cb (GtkEventControllerMotion *controller, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	edit->priv->mouse_in_widget = FALSE;
	gtk_widget_set_cursor_from_name (GTK_WIDGET (edit), "text");
}

/* Capture-phase key handler on toplevel: catches Ctrl press/release regardless
 * of which child has focus, so we can update the URL cursor immediately. */
static gboolean
root_key_pressed_cb (GtkEventControllerKey *controller, guint keyval,
                     guint keycode, GdkModifierType state, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	if ((keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R) &&
	    edit->priv->mouse_in_widget)
	{
		update_url_cursor (edit, edit->priv->last_mouse_x,
		                   edit->priv->last_mouse_y, TRUE);
	}
	return FALSE;
}

static void
root_key_released_cb (GtkEventControllerKey *controller, guint keyval,
                      guint keycode, GdkModifierType state, gpointer data)
{
	HexInputEdit *edit = HEX_INPUT_EDIT (data);
	if ((keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R) &&
	    edit->priv->mouse_in_widget)
	{
		update_url_cursor (edit, edit->priv->last_mouse_x,
		                   edit->priv->last_mouse_y, FALSE);
	}
}

static void
on_root_notify (GObject *object, GParamSpec *pspec, gpointer data)
{
	(void) pspec; (void) data;
	HexInputEdit *edit = HEX_INPUT_EDIT (object);
	HexInputEditPriv *priv = edit->priv;

	/* Remove old controller from previous root */
	if (priv->root_key_ctrl)
	{
		GtkWidget *old_root = gtk_event_controller_get_widget (priv->root_key_ctrl);
		if (old_root)
			gtk_widget_remove_controller (old_root, priv->root_key_ctrl);
		priv->root_key_ctrl = NULL;
	}

	/* Attach to new root */
	GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (edit));
	if (root && GTK_IS_WIDGET (root))
	{
		GtkEventController *ctrl = gtk_event_controller_key_new ();
		gtk_event_controller_set_propagation_phase (ctrl, GTK_PHASE_CAPTURE);
		g_signal_connect (ctrl, "key-pressed",
		                  G_CALLBACK (root_key_pressed_cb), edit);
		g_signal_connect (ctrl, "key-released",
		                  G_CALLBACK (root_key_released_cb), edit);
		gtk_widget_add_controller (GTK_WIDGET (root), ctrl);
		priv->root_key_ctrl = ctrl;
	}
}

static void
hex_input_edit_init (HexInputEdit *edit)
{
	HexInputEditPriv *priv = g_new0 (HexInputEditPriv, 1);
	edit->priv = priv;

	priv->text = g_string_new ("");
	priv->cursor_byte = 0;
	priv->sel_anchor_byte = -1;
	priv->multiline = TRUE;
	priv->editable = TRUE;
	priv->max_lines = 5;
	priv->parse_dirty = TRUE;
	priv->caret_visible = TRUE;
	priv->undo_stack = NULL;
	priv->undo_pos = -1;
	priv->undo_frozen = FALSE;

	/* Spell checking */
	priv->dict_hash = g_hash_table_new_full (g_str_hash, g_str_equal,
	                                          g_free, NULL);
	hie_initialize_enchant ();
	if (hie_have_enchant)
	{
		hex_input_edit_activate_default_languages (edit);
		priv->spell_checked = prefs.hex_gui_input_spell;
	}

	/* Make focusable */
	gtk_widget_set_focusable (GTK_WIDGET (edit), TRUE);
	gtk_widget_set_can_focus (GTK_WIDGET (edit), TRUE);

	/* IM Context */
	priv->im_context = gtk_im_multicontext_new ();
	g_signal_connect (priv->im_context, "commit",
	                  G_CALLBACK (im_commit_cb), edit);
	g_signal_connect (priv->im_context, "preedit-changed",
	                  G_CALLBACK (im_preedit_changed_cb), edit);
	g_signal_connect (priv->im_context, "retrieve-surrounding",
	                  G_CALLBACK (im_retrieve_surrounding_cb), edit);
	g_signal_connect (priv->im_context, "delete-surrounding",
	                  G_CALLBACK (im_delete_surrounding_cb), edit);

	/* Key event controller (BUBBLE phase — fkeys.c uses CAPTURE) */
	GtkEventController *key_ctrl = gtk_event_controller_key_new ();
	gtk_event_controller_set_propagation_phase (key_ctrl, GTK_PHASE_BUBBLE);
	g_signal_connect (key_ctrl, "key-pressed",
	                  G_CALLBACK (key_pressed_cb), edit);
	g_signal_connect (key_ctrl, "key-released",
	                  G_CALLBACK (key_released_cb), edit);
	gtk_im_context_set_use_preedit (priv->im_context, TRUE);
	/* Forward IMContext from this controller */
	gtk_event_controller_key_set_im_context (
	    GTK_EVENT_CONTROLLER_KEY (key_ctrl), priv->im_context);
	gtk_widget_add_controller (GTK_WIDGET (edit), key_ctrl);

	/* Click gesture */
	GtkGesture *click = gtk_gesture_click_new ();
	gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
	g_signal_connect (click, "pressed",
	                  G_CALLBACK (click_pressed_cb), edit);
	g_signal_connect (click, "released",
	                  G_CALLBACK (click_released_cb), edit);
	gtk_widget_add_controller (GTK_WIDGET (edit), GTK_EVENT_CONTROLLER (click));

	/* Right-click for context menu */
	GtkGesture *rclick = gtk_gesture_click_new ();
	gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rclick), GDK_BUTTON_SECONDARY);
	g_signal_connect (rclick, "pressed",
	                  G_CALLBACK (rclick_pressed_cb), edit);
	gtk_widget_add_controller (GTK_WIDGET (edit), GTK_EVENT_CONTROLLER (rclick));

	/* Middle-click pastes from the PRIMARY selection */
	GtkGesture *mclick = gtk_gesture_click_new ();
	gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (mclick), GDK_BUTTON_MIDDLE);
	g_signal_connect (mclick, "pressed",
	                  G_CALLBACK (middle_pressed_cb), edit);
	gtk_widget_add_controller (GTK_WIDGET (edit), GTK_EVENT_CONTROLLER (mclick));

	/* Drag gesture for selection */
	GtkGesture *drag = gtk_gesture_drag_new ();
	g_signal_connect (drag, "drag-update",
	                  G_CALLBACK (drag_update_cb), edit);
	gtk_widget_add_controller (GTK_WIDGET (edit), GTK_EVENT_CONTROLLER (drag));

	/* Scroll controller (mouse wheel) */
	GtkEventController *scroll_ctrl = gtk_event_controller_scroll_new (
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect (scroll_ctrl, "scroll",
	                  G_CALLBACK (scroll_cb), edit);
	gtk_widget_add_controller (GTK_WIDGET (edit), scroll_ctrl);

	/* Motion controller (for Ctrl+hover URL cursor) */
	GtkEventController *motion_ctrl = gtk_event_controller_motion_new ();
	g_signal_connect (motion_ctrl, "motion",
	                  G_CALLBACK (motion_cb), edit);
	g_signal_connect (motion_ctrl, "enter",
	                  G_CALLBACK (motion_enter_cb), edit);
	g_signal_connect (motion_ctrl, "leave",
	                  G_CALLBACK (motion_leave_cb), edit);
	gtk_widget_add_controller (GTK_WIDGET (edit), motion_ctrl);

	/* Focus controller */
	GtkEventController *focus_ctrl = gtk_event_controller_focus_new ();
	g_signal_connect (focus_ctrl, "enter",
	                  G_CALLBACK (focus_enter_cb), edit);
	g_signal_connect (focus_ctrl, "leave",
	                  G_CALLBACK (focus_leave_cb), edit);
	gtk_widget_add_controller (GTK_WIDGET (edit), focus_ctrl);

	/* Attach capture-phase key controller to toplevel when rooted,
	 * so we catch Ctrl press/release for URL cursor feedback. */
	g_signal_connect (edit, "notify::root",
	                  G_CALLBACK (on_root_notify), NULL);

	/* Set cursor to text cursor */
	gtk_widget_set_cursor_from_name (GTK_WIDGET (edit), "text");

	/* CSS */
	gtk_widget_add_css_class (GTK_WIDGET (edit), "hex-input-edit");
}

static void
hex_input_edit_class_init (HexInputEditClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = hex_input_edit_dispose;
	object_class->finalize = hex_input_edit_finalize;

	widget_class->snapshot = hex_input_edit_snapshot;
	widget_class->measure = hex_input_edit_measure;
	widget_class->size_allocate = hex_input_edit_size_allocate;
	widget_class->realize = hex_input_edit_realize;
	widget_class->unrealize = hex_input_edit_unrealize;
	widget_class->get_request_mode = hex_input_edit_get_request_mode;

	klass->word_check = default_word_check;

	gtk_widget_class_set_css_name (widget_class, "hexinput");

	/* Widget-class actions for context menu */
	gtk_widget_class_install_action (widget_class, "edit.undo", NULL, action_undo);
	gtk_widget_class_install_action (widget_class, "edit.redo", NULL, action_redo);
	gtk_widget_class_install_action (widget_class, "edit.cut", NULL, action_cut);
	gtk_widget_class_install_action (widget_class, "edit.copy", NULL, action_copy);
	gtk_widget_class_install_action (widget_class, "edit.paste", NULL, action_paste);
	gtk_widget_class_install_action (widget_class, "edit.select-all", NULL, action_select_all);
	gtk_widget_class_install_action (widget_class, "spell.replace", "s", action_spell_replace);
	gtk_widget_class_install_action (widget_class, "spell.add", NULL, action_spell_add);

	/* Suppress theme background — we draw our own from the xtext palette,
	 * but keep the theme's border, focus ring, and other entry styling. */
	GtkCssProvider *css = gtk_css_provider_new ();
	gtk_css_provider_load_from_string (css,
		"entry.hex-input-edit { background: none; }");
	gtk_style_context_add_provider_for_display (
		gdk_display_get_default (),
		GTK_STYLE_PROVIDER (css),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref (css);

	/* Signals */
	signals[SIGNAL_ACTIVATE] = g_signal_new (
		"activate",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
		G_STRUCT_OFFSET (HexInputEditClass, activate),
		NULL, NULL,
		g_cclosure_marshal_generic,
		G_TYPE_NONE, 0);

	signals[SIGNAL_WORD_CHECK] = g_signal_new (
		"word-check",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (HexInputEditClass, word_check),
		(GSignalAccumulator) spell_accumulator, NULL,
		g_cclosure_marshal_generic,
		G_TYPE_BOOLEAN, 1, G_TYPE_STRING);

	/* Emitted when the user pastes or drops an image; the argument is the
	 * image encoded as PNG. The embedder uploads it and inserts a link. */
	signals[SIGNAL_IMAGE_PASTE] = g_signal_new (
		"image-paste",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (HexInputEditClass, image_paste),
		NULL, NULL,
		g_cclosure_marshal_generic,
		G_TYPE_NONE, 1, G_TYPE_BYTES);
}

/* =============================== */
/* === Public API               === */
/* =============================== */

GtkWidget *
hex_input_edit_new (void)
{
	return g_object_new (HEX_TYPE_INPUT_EDIT, NULL);
}

const char *
hex_input_edit_get_text (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), "");
	HexInputEditPriv *priv = edit->priv;

	g_free (priv->cached_text);
	priv->cached_text = g_strdup (priv->text->str);
	return priv->cached_text;
}

void
hex_input_edit_set_text (HexInputEdit *edit, const char *text)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	HexInputEditPriv *priv = edit->priv;

	g_string_assign (priv->text, text ? text : "");
	priv->cursor_byte = (int) priv->text->len;
	priv->sel_anchor_byte = -1;
	mark_dirty (edit);  /* ensure_cursor_visible will scroll to cursor at end */
}

int
hex_input_edit_get_position (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), 0);
	return byte_to_char (edit->priv->text->str, edit->priv->cursor_byte);
}

void
hex_input_edit_set_position (HexInputEdit *edit, int pos)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	HexInputEditPriv *priv = edit->priv;

	priv->cursor_byte = char_to_byte (priv->text->str, priv->text->len, pos);
	priv->sel_anchor_byte = -1;
	reset_blink (edit);
}

void
hex_input_edit_insert_text (HexInputEdit *edit, const char *text,
                             int len, int *pos)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	HexInputEditPriv *priv = edit->priv;

	if (len < 0)
		len = (int) strlen (text);

	int byte_pos = char_to_byte (priv->text->str, priv->text->len, *pos);
	g_string_insert_len (priv->text, byte_pos, text, len);
	byte_pos += len;
	priv->cursor_byte = byte_pos;
	priv->sel_anchor_byte = -1;
	mark_dirty (edit);

	/* Return updated character position */
	*pos = byte_to_char (priv->text->str, byte_pos);
}

void
hex_input_edit_set_max_lines (HexInputEdit *edit, int max_lines)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->max_lines = max_lines;
	gtk_widget_queue_resize (GTK_WIDGET (edit));
}

void
hex_input_edit_set_multiline (HexInputEdit *edit, gboolean multiline)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->multiline = multiline;
	gtk_widget_queue_resize (GTK_WIDGET (edit));
}

void
hex_input_edit_set_editable (HexInputEdit *edit, gboolean editable)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->editable = editable;
}

void
hex_input_edit_set_max_chars (HexInputEdit *edit, int max_chars)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->max_chars = max_chars > 0 ? max_chars : 0;
}

void
hex_input_edit_set_width_chars (HexInputEdit *edit, int width_chars)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->width_chars = width_chars > 0 ? width_chars : 0;
	gtk_widget_queue_resize (GTK_WIDGET (edit));
}

void
hex_input_edit_set_max_width_chars (HexInputEdit *edit, int max_width_chars)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->max_width_chars = max_width_chars > 0 ? max_width_chars : 0;
	gtk_widget_queue_resize (GTK_WIDGET (edit));
}

void
hex_input_edit_set_emoji_cache (HexInputEdit *edit, xtext_emoji_cache *cache)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->emoji_cache = cache;
	/* Recalculate line_height to account for emoji sprite size */
	edit->priv->fontsize = 0;  /* force recalc */
	mark_dirty (edit);
}

void
hex_input_edit_set_palette (HexInputEdit *edit, const GdkRGBA *palette)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->palette = palette;
	gtk_widget_queue_draw (GTK_WIDGET (edit));
}

void
hex_input_edit_set_font (HexInputEdit *edit, PangoFontDescription *font_desc)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	HexInputEditPriv *priv = edit->priv;

	if (priv->font_desc)
		pango_font_description_free (priv->font_desc);
	priv->font_desc = font_desc ? pango_font_description_copy (font_desc) : NULL;

	/* Reset cached metrics so they're recomputed from the new font */
	priv->fontsize = 0;
	priv->parse_dirty = TRUE;
	g_clear_object (&priv->layout);
	gtk_widget_queue_resize (GTK_WIDGET (edit));
}

/* --- Layout line queries --- */

int
hex_input_edit_get_line_count (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), 1);
	HexInputEditPriv *priv = edit->priv;
	if (!priv->layout)
		return 1;
	update_layout (edit, gtk_widget_get_width (GTK_WIDGET (edit)));
	return pango_layout_get_line_count (priv->layout);
}

int
hex_input_edit_get_cursor_line (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), 0);
	HexInputEditPriv *priv = edit->priv;
	if (!priv->layout)
		return 0;

	update_layout (edit, gtk_widget_get_width (GTK_WIDGET (edit)));
	int stripped_cursor = cursor_stripped_offset (priv);

	/* Walk layout lines to find which one contains the cursor.
	 * At wrap boundaries, the cursor belongs to the *next* line
	 * (visually it appears at the start of that line), so use
	 * strict < for line_end, except on the very last line. */
	int total_lines = pango_layout_get_line_count (priv->layout);
	PangoLayoutIter *iter = pango_layout_get_iter (priv->layout);
	int line_idx = 0;
	do {
		PangoLayoutLine *pline = pango_layout_iter_get_line_readonly (iter);
		int line_start = pango_layout_iter_get_index (iter);
		int line_end = line_start + pline->length;
		gboolean is_last = (line_idx == total_lines - 1);
		if (stripped_cursor >= line_start &&
		    (is_last ? stripped_cursor <= line_end : stripped_cursor < line_end))
			break;
		line_idx++;
	} while (pango_layout_iter_next_line (iter));
	pango_layout_iter_free (iter);
	return line_idx;
}

gboolean
hex_input_edit_move_cursor_up (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), FALSE);
	HexInputEditPriv *priv = edit->priv;
	if (!priv->layout)
		return FALSE;

	update_layout (edit, gtk_widget_get_width (GTK_WIDGET (edit)));
	int stripped_cursor = cursor_stripped_offset (priv);

	/* Get cursor x position for vertical alignment */
	PangoRectangle strong;
	pango_layout_get_cursor_pos (priv->layout, stripped_cursor, &strong, NULL);
	int target_x = strong.x;
	int target_y = strong.y - 1;  /* one pixel above current line */

	if (target_y < 0)
		return FALSE;  /* already on first line */

	/* Find new index at same x, one line up */
	int new_index, trailing;
	pango_layout_xy_to_index (priv->layout, target_x, target_y, &new_index, &trailing);
	new_index += trailing;

	/* Convert stripped offset back to raw */
	int raw_off;
	if (priv->raw_to_stripped_map && priv->stripped_len > 0)
		raw_off = xtext_stripped_to_raw (priv->raw_to_stripped_map,
		                                 (int) priv->text->len, new_index);
	else
		raw_off = new_index;

	priv->cursor_byte = raw_off;
	priv->sel_anchor_byte = -1;
	reset_blink (edit);
	return TRUE;
}

gboolean
hex_input_edit_move_cursor_down (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), FALSE);
	HexInputEditPriv *priv = edit->priv;
	if (!priv->layout)
		return FALSE;

	update_layout (edit, gtk_widget_get_width (GTK_WIDGET (edit)));
	int stripped_cursor = cursor_stripped_offset (priv);

	/* Get cursor position */
	PangoRectangle strong;
	pango_layout_get_cursor_pos (priv->layout, stripped_cursor, &strong, NULL);
	int target_x = strong.x;
	int target_y = strong.y + strong.height + 1;  /* one pixel below current line */

	/* Check if below last line */
	int layout_h;
	pango_layout_get_size (priv->layout, NULL, &layout_h);
	if (target_y >= layout_h)
		return FALSE;  /* already on last line */

	/* Find new index at same x, one line down */
	int new_index, trailing;
	pango_layout_xy_to_index (priv->layout, target_x, target_y, &new_index, &trailing);
	new_index += trailing;

	/* Convert stripped offset back to raw */
	int raw_off;
	if (priv->raw_to_stripped_map && priv->stripped_len > 0)
		raw_off = xtext_stripped_to_raw (priv->raw_to_stripped_map,
		                                 (int) priv->text->len, new_index);
	else
		raw_off = new_index;

	priv->cursor_byte = raw_off;
	priv->sel_anchor_byte = -1;
	reset_blink (edit);
	return TRUE;
}

/* =============================== */
/* === Spell checking API       === */
/* =============================== */

static void
enumerate_dicts_cb (const char * const lang_tag,
                    const char * const provider_name,
                    const char * const provider_desc,
                    const char * const provider_file,
                    void * user_data)
{
	(void) provider_name; (void) provider_desc; (void) provider_file;
	GSList **langs = (GSList **) user_data;
	*langs = g_slist_append (*langs, g_strdup (lang_tag));
}

static gboolean
enchant_has_lang (const gchar *lang, GSList *langs)
{
	GSList *i;
	for (i = langs; i; i = g_slist_next (i))
	{
		if (strcmp (lang, i->data) == 0)
			return TRUE;
	}
	return FALSE;
}

static gboolean
activate_language_internal (HexInputEdit *edit, const gchar *lang)
{
	struct EnchantDict *dict;
	HexInputEditPriv *priv = edit->priv;

	if (!hie_have_enchant || !priv->broker)
		return FALSE;

	if (g_hash_table_lookup (priv->dict_hash, lang))
		return TRUE;

	dict = enchant_broker_request_dict (priv->broker, lang);
	if (!dict)
		return FALSE;

	priv->dict_list = g_slist_append (priv->dict_list, dict);
	g_hash_table_insert (priv->dict_hash, g_strdup (lang), dict);
	return TRUE;
}

void
hex_input_edit_set_checked (HexInputEdit *edit, gboolean checked)
{
	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	edit->priv->spell_checked = checked;
	mark_dirty (edit);
}

gboolean
hex_input_edit_is_checked (HexInputEdit *edit)
{
	g_return_val_if_fail (HEX_IS_INPUT_EDIT (edit), FALSE);
	return edit->priv->spell_checked;
}

void
hex_input_edit_activate_default_languages (HexInputEdit *edit)
{
	GSList *enchant_langs = NULL;
	char **langs, **i;
	HexInputEditPriv *priv;

	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	priv = edit->priv;

	if (!hie_have_enchant)
		return;

	if (!priv->broker)
		priv->broker = enchant_broker_init ();

	enchant_broker_list_dicts (priv->broker, enumerate_dicts_cb,
	                           &enchant_langs);

	langs = g_strsplit_set (prefs.hex_text_spell_langs, ", \t", 0);

	for (i = langs; *i; i++)
	{
		if (enchant_has_lang (*i, enchant_langs))
			activate_language_internal (edit, *i);
	}

	g_slist_free_full (enchant_langs, g_free);
	g_strfreev (langs);

	if (priv->dict_list == NULL)
		activate_language_internal (edit, "en");

	mark_dirty (edit);
}

void
hex_input_edit_deactivate_language (HexInputEdit *edit, const gchar *lang)
{
	HexInputEditPriv *priv;

	g_return_if_fail (HEX_IS_INPUT_EDIT (edit));
	priv = edit->priv;

	if (!hie_have_enchant || !priv->dict_list)
		return;

	if (lang)
	{
		struct EnchantDict *dict = g_hash_table_lookup (priv->dict_hash, lang);
		if (!dict)
			return;
		enchant_broker_free_dict (priv->broker, dict);
		priv->dict_list = g_slist_remove (priv->dict_list, dict);
		g_hash_table_remove (priv->dict_hash, lang);
	}
	else
	{
		/* Deactivate all */
		GSList *li;
		for (li = priv->dict_list; li; li = g_slist_next (li))
			enchant_broker_free_dict (priv->broker, (struct EnchantDict *) li->data);
		g_slist_free (priv->dict_list);
		g_hash_table_destroy (priv->dict_hash);
		priv->dict_hash = g_hash_table_new_full (g_str_hash, g_str_equal,
		                                          g_free, NULL);
		priv->dict_list = NULL;
	}

	mark_dirty (edit);
}
