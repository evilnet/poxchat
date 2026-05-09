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
 * =========================================================================
 *
 * xtext, the text widget used by X-Chat.
 * By Peter Zelezny <zed@xchat.org>.
 *
 */

#define GDK_MULTIHEAD_SAFE
#define MARGIN 2						/* dont touch. */
/* When the widget is resized narrower than the current auto-indent can
 * comfortably accommodate, the indent is clamped so the right column
 * always retains at least XTEXT_MIN_TEXT_COL pixels for message text.
 * XTEXT_MIN_INDENT is the floor below which we refuse to shrink the
 * indent; together with XTEXT_MIN_TEXT_COL + MARGIN + scrollbar width,
 * this also drives the widget's reported horizontal minimum so the
 * surrounding pane cannot shrink us into the render-bail condition. */
#define XTEXT_MIN_TEXT_COL 48			/* reserve for message column */
#define XTEXT_MIN_INDENT    40			/* floor on auto-indent width */
#define REFRESH_TIMEOUT 20
#define VIRT_PAGE_SIZE 100				/* entries to keep beyond viewport as eviction buffer */
/* VIRT_MAT_WINDOW is defined in xtext.h */

/* Collapsible multiline messages: preview line count when collapsed */
#define COLLAPSE_PREVIEW_LINES 3

/* Cached display line count — set by ent_update_display_lines().
 * A simple field read, replacing the old g_slist_length call in hot paths. */
#define ENT_DISPLAY_LINES(ent) ((ent)->display_lines)


#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>

#include "config.h"
#include "../common/hexchat.h"
#include "../common/fe.h"
#include "../common/util.h"
#include "../common/hexchatc.h"
#include "../common/cfgfiles.h"
#include "../common/url.h"

#ifdef WIN32
#include "marshal.h"
#include <windows.h>
#else
#include "../common/marshal.h"
#endif

#include "fe-gtk.h"
#include "xtext.h"
#include "tree234.h"
#include "xtext-emoji.h"
#include "fkeys.h"
#include "gtk-helpers.h"
#include "../common/scrollback.h"	/* Virtual scrollback (Phase 3): scrollback_msg, scrollback_load_range */

#define charlen(str) g_utf8_skip[*(guchar *)(str)]

#ifdef WIN32
#include <windows.h>
#include <io.h>
#include <gdk/gdk.h>
#include <gdk/win32/gdkwin32.h>
#else /* !WIN32 */
#include <unistd.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif
#endif

/* is delimiter */
#define is_del(c) \
	(c == ' ' || c == '\n' || c == '>' || c == '<' || c == 0)

/* force scrolling off */
#define dontscroll(buf) (buf)->last_pixel_pos = 0x7fffffff

/* Local entry_id namespace base.  Entries without a DB rowid get IDs
 * from this range upward; DB rowids stay in [1 .. LOCAL_ENTRY_ID_BASE-1].
 * See gtk_xtext_init_entry for full rationale. */
#define LOCAL_ENTRY_ID_BASE  ((guint64) 1 << 62)

/* Debug instrumentation for scroll/anchor work.  Set to 1 to get traces in
 * VS Output window (Windows) or stderr (other).  g_warning/g_print do not
 * surface on Windows GUI builds, so we route through OutputDebugStringW.
 * Format strings are UTF-8 (source encoding); we widen to UTF-16 before
 * handing them to Windows so the Output window decodes correctly. */
#define XTEXT_DEBUG_SCROLL 0

#if XTEXT_DEBUG_SCROLL
static void
xt_dbg (const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start (ap, fmt);
	g_vsnprintf (buf, sizeof buf, fmt, ap);
	va_end (ap);
#ifdef WIN32
	{
		wchar_t *wbuf = g_utf8_to_utf16 (buf, -1, NULL, NULL, NULL);
		if (wbuf)
		{
			OutputDebugStringW (wbuf);
			g_free (wbuf);
		}
		else
		{
			OutputDebugStringA (buf);
		}
	}
#else
	fputs (buf, stderr);
#endif
}
#define XT_DBG(...) xt_dbg (__VA_ARGS__)
#else
#define XT_DBG(...) ((void)0)
#endif

static GtkWidgetClass *parent_class = NULL;

/* Phase 4: entry modification support */
#define TEXTENTRY_FLAG_SEPARATE_STR  0x01
#define TEXTENTRY_FLAG_DAY_BOUNDARY  0x02
#define TEXTENTRY_FLAG_EPHEMERAL     0x04  /* no DB row; pinned during eviction */

typedef struct xtext_redaction_info {
	char *original_content;		/* preserved text for audit/reveal */
	int original_len;			/* length of original_content */
	char *redacted_by;			/* nick who issued REDACT */
	char *redaction_reason;		/* optional reason */
	time_t redaction_time;
	gint64 prompt_shown_time;	/* monotonic µs when "click to reveal" was shown (0 = not shown) */
} xtext_redaction_info;

/* IRCv3 reactions: one entry per distinct reaction text on a message */
typedef struct xtext_reaction {
	char *text;				/* reaction content (emoji or arbitrary string) */
	GHashTable *nicks;		/* nick string → GINT_TO_POINTER(is_self) for lookup + unreact */
	int count;				/* cached g_hash_table_size(nicks) */
} xtext_reaction;

typedef struct xtext_reactions_info {
	GPtrArray *reactions;	/* array of xtext_reaction* — one per distinct text */
	int total_count;		/* sum of all counts (cache for rendering decisions) */
} xtext_reactions_info;

/* IRCv3 reply context: what message this entry is replying to */
typedef struct xtext_reply_info {
	char *target_msgid;			/* msgid of referenced message */
	guint64 target_entry_id;	/* resolved entry_id (0 = not found in buffer) */
	char *target_nick;			/* nick of original message (for display) */
	char *target_preview;		/* truncated original text (~80 chars, stripped) */
} xtext_reply_info;

/* Format span types and rendering functions shared with hex-input-edit */
#include "xtext-render.h"

struct textentry
{
	struct textentry *next;
	struct textentry *prev;
	unsigned char *str;
	time_t stamp;
	gint16 str_width;
	gint16 str_len;
	gint16 mark_start;
	gint16 mark_end;
	gint16 indent;
	gint16 left_len;
	GSList *slp;
	GSList *sublines;
	guchar tag;
	guchar state;				/* xtext_entry_state (was pad1) */
	guchar flags;				/* bit 0: TEXTENTRY_FLAG_SEPARATE_STR (was pad2) */
	GList *marks;	/* List of found strings */

	/* IRCv3 modernization: stable entry identification (Phase 1) */
	char *msgid;		/* Server-assigned message ID (may be NULL) */
	guint64 entry_id;	/* Local unique ID (always set, monotonic) */
	guint64 group_id;	/* Multiline group: entries with same non-zero group_id are one message */

	/* Phase 4: redaction accountability (lazy-allocated, NULL for most entries) */
	struct xtext_redaction_info *redaction;

	/* IRCv3 reactions and reply context (lazy-allocated, NULL for most entries) */
	struct xtext_reactions_info *reactions;
	struct xtext_reply_info *reply;
	guint8 extra_lines_above;		/* reply context lines (0 or 1) */
	guint8 extra_lines_below;		/* reaction badge lines (0 or 1) */

	unsigned int collapsed:1;		/* multiline entry is currently collapsed */
	unsigned int collapsible:1;		/* multiline entry can be collapsed/expanded */
	unsigned int has_db_row:1;		/* entry was saved to DB (has valid DB rowid) */

	int display_lines;				/* cached display line count (replaces g_slist_length in hot paths) */
	int sublines_width;				/* window_width when sublines were computed (0 = needs recompute) */

	/* Pre-parsed rendering data (built once at entry creation) */
	unsigned char *stripped_str;       /* text with format codes removed, emoji → U+FFFC */
	guint16 stripped_len;              /* byte length of stripped_str */
	xtext_fmt_span *fmt_spans;        /* array of format transition records */
	guint16 fmt_span_count;            /* number of spans */
	xtext_emoji_info *emoji_list;     /* emoji placeholder info, NULL if none */
	guint16 emoji_count;               /* number of emoji */
	guint16 *raw_to_stripped_map;     /* raw_len+1 entries mapping raw→stripped offsets */
};

enum
{
	WORD_CLICK,
	SET_SCROLL_ADJUSTMENTS,
	LAST_SIGNAL
};

/* Recompute display_lines from current sublines/collapse/extra state.
 * Must be called after changing sublines, collapsed, collapsible,
 * extra_lines_above, or extra_lines_below.
 * Note: callers that change display_lines directly (e.g. ent->display_lines++)
 * must call update_weight234 separately if the entry is in a tree. */
static inline void
ent_update_display_lines (textentry *ent)
{
	int text_lines = (int)g_slist_length (ent->sublines);

	if (ent->collapsed)
		ent->display_lines = MIN (COLLAPSE_PREVIEW_LINES, text_lines) + 1
			+ ent->extra_lines_above + ent->extra_lines_below;
	else
		ent->display_lines = text_lines + ent->extra_lines_above
			+ ent->extra_lines_below + (ent->collapsible ? 1 : 0);
}

/* Weight callback for the entry tree — returns display_lines for a textentry */
static int
entry_weight_cb (void *elem)
{
	textentry *ent = (textentry *)elem;
	return ent->display_lines;
}

/* Comparator for sorted insertion by timestamp */
static int
entry_stamp_cmp (void *a, void *b)
{
	textentry *ea = (textentry *)a;
	textentry *eb = (textentry *)b;
	if (ea->stamp < eb->stamp) return -1;
	if (ea->stamp > eb->stamp) return 1;
	/* Same timestamp — use entry_id for stable ordering */
	if (ea->entry_id < eb->entry_id) return -1;
	if (ea->entry_id > eb->entry_id) return 1;
	return 0;
}

/* values for selection info */
enum
{
	TARGET_UTF8_STRING,
	TARGET_STRING,
	TARGET_TEXT,
	TARGET_COMPOUND_TEXT
};

static guint xtext_signals[LAST_SIGNAL];

char *nocasestrstr (const char *text, const char *tofind);	/* util.c */
int xtext_get_stamp_str (time_t, char **);
static void gtk_xtext_render_page (GtkXText * xtext);
void gtk_xtext_calc_lines (xtext_buffer *buf, int);
static void gtk_xtext_calc_lines_virtual_ex (xtext_buffer *buf, int fire_signal,
                                              gboolean recompute_sublines);
static gboolean gtk_xtext_is_selecting (GtkXText *xtext);
static char *gtk_xtext_selection_get_text (GtkXText *xtext, int *len_ret);
static textentry *gtk_xtext_nth (GtkXText *xtext, int line, int *subline);
static void gtk_xtext_adjustment_changed (GtkAdjustment * adj,
														GtkXText * xtext);
static void gtk_xtext_scroll_adjustments (GtkXText *xtext, GtkAdjustment *hadj,
										GtkAdjustment *vadj);
static int gtk_xtext_render_ents (GtkXText * xtext, textentry *, textentry *);
static textentry *xtext_resolve_marker (xtext_buffer *buf);
static void gtk_xtext_recalc_widths (xtext_buffer *buf, int);
static void gtk_xtext_fix_indent (xtext_buffer *buf);
static int gtk_xtext_width_indent_cap (xtext_buffer *buf);
static int gtk_xtext_auto_indent_cap (xtext_buffer *buf);
static gboolean gtk_xtext_clamp_indent_to_width (xtext_buffer *buf);
static int gtk_xtext_find_subline (GtkXText *xtext, textentry *ent, int line);
static void gtk_xtext_draw_status_strip (GtkXText *xtext, int width, int height);
static void gtk_xtext_draw_toasts (GtkXText *xtext, int width, int height);
static gboolean gtk_xtext_toast_tick (gpointer data);
static int gtk_xtext_measure_reaction_badges (GtkXText *xtext, struct xtext_reactions_info *ri, int win_width);
static void gtk_xtext_virt_ensure_range (xtext_buffer *buf, int center_index, int radius);
static int gtk_xtext_lines_taken (xtext_buffer *buf, textentry *ent);
/* static char *gtk_xtext_conv_color (unsigned char *text, int len, int *newlen); */
/* For use by gtk_xtext_strip_color() and its callers -- */
struct offlen_s {
	guint16 off;
	guint16 len;
	guint16 emph;
	guint16 width;
};
typedef struct offlen_s offlen_t;

/* xtext_parse_formats, xtext_build_attrlist, xtext_raw_to_stripped,
 * xtext_stripped_to_raw are now in xtext-render.c/h */

/* Helper to build xtext_format_data from a textentry */
static inline xtext_format_data
xtext_fdata_from_entry (textentry *ent)
{
	xtext_format_data fd;
	fd.stripped_str = ent->stripped_str;
	fd.stripped_len = ent->stripped_len;
	fd.fmt_spans = ent->fmt_spans;
	fd.fmt_span_count = ent->fmt_span_count;
	fd.emoji_list = ent->emoji_list;
	fd.emoji_count = ent->emoji_count;
	return fd;
}

static unsigned char *
gtk_xtext_strip_color (unsigned char *text, int len, unsigned char *outbuf,
							  int *newlen, GSList **slp, int strip_hidden);
static gboolean gtk_xtext_check_ent_visibility (GtkXText * xtext, textentry *find_ent, int add);
static int gtk_xtext_render_page_timeout (GtkXText * xtext);
static GList * gtk_xtext_search_textentry (xtext_buffer *, textentry *);
static void gtk_xtext_search_textentry_add (xtext_buffer *, textentry *, GList *, gboolean);
static void gtk_xtext_search_textentry_del (xtext_buffer *, textentry *);
static void gtk_xtext_search_textentry_fini (gpointer, gpointer);
static void gtk_xtext_search_fini (xtext_buffer *);
static gboolean gtk_xtext_search_init (xtext_buffer *buf, const gchar *text, gtk_xtext_search_flags flags, GError **perr);
static char * gtk_xtext_get_word (GtkXText * xtext, int x, int y, textentry ** ret_ent, int *ret_off, int *ret_len, GSList **slp);

/* Click zone enum and forward declaration (used by motion handler) */
typedef enum {
	XTEXT_ZONE_REPLY   = 0,
	XTEXT_ZONE_TEXT    = 1,
	XTEXT_ZONE_REACT   = 2,
	XTEXT_ZONE_DAY_SEP = 3,
	XTEXT_ZONE_COLLAPSE = 4
} xtext_click_zone;
static xtext_click_zone gtk_xtext_get_click_zone (GtkXText *xtext, int y, textentry **ent_out);

/* Day boundary detection: are two timestamps on different calendar days? */
static gboolean
xtext_is_different_day (time_t a, time_t b)
{
	struct tm ta, tb;
#ifdef WIN32
	localtime_s (&ta, &a);
	localtime_s (&tb, &b);
#else
	localtime_r (&a, &ta);
	localtime_r (&b, &tb);
#endif
	return (ta.tm_year != tb.tm_year || ta.tm_yday != tb.tm_yday);
}

/* GTK4 event controller callbacks - forward declarations */
static void gtk_xtext_button_press (GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void gtk_xtext_button_release (GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void gtk_xtext_motion_notify (GtkEventControllerMotion *controller, double event_x, double event_y, gpointer user_data);
static void gtk_xtext_leave_notify (GtkEventControllerMotion *controller, gpointer user_data);
static gboolean gtk_xtext_scroll (GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data);

/* Cairo-based background drawing macro - uses the current cairo context stored in xtext */
#define xtext_draw_bg(xt,x,y,w,h) do { \
	if ((xt)->pixmap) { \
		cairo_set_source_surface((xt)->cr, (xt)->pixmap, (xt)->ts_x, (xt)->ts_y); \
		cairo_pattern_set_extend(cairo_get_source((xt)->cr), CAIRO_EXTEND_REPEAT); \
	} else { \
		gdk_cairo_set_source_rgba((xt)->cr, &(xt)->palette[XTEXT_BG]); \
	} \
	cairo_rectangle((xt)->cr, x, y, w, h); \
	cairo_fill((xt)->cr); \
} while(0)

/* ======================================= */
/* ======== DISPLAY LINE CACHE ========== */
/* ======================================= */

static void
line_display_free (xtext_line_display *disp)
{
	if (!disp)
		return;
	g_slist_free (disp->sublines);
	if (disp->layout)
		g_object_unref (disp->layout);
	g_free (disp);
}

static xtext_display_cache *
display_cache_new (int max_entries)
{
	xtext_display_cache *cache = g_new0 (xtext_display_cache, 1);
	cache->by_id = g_hash_table_new (g_int64_hash, g_int64_equal);
	g_queue_init (&cache->lru_order);
	cache->max_entries = max_entries;
	return cache;
}

static void
display_cache_free (xtext_display_cache *cache)
{
	GList *l;

	if (!cache)
		return;

	for (l = cache->lru_order.head; l; l = l->next)
		line_display_free (l->data);
	g_queue_clear (&cache->lru_order);
	g_hash_table_destroy (cache->by_id);
	g_free (cache);
}

/* Find entry by id in the buffer.
 * Uses the entries_by_id hash table for O(1) lookup. */
static textentry *
display_cache_find_entry (xtext_buffer *buf, guint64 entry_id)
{
	if (buf->entries_by_id)
		return g_hash_table_lookup (buf->entries_by_id, GSIZE_TO_POINTER (entry_id));
	return NULL;
}

/* Evict the least-recently-used display */
static void
display_cache_evict_lru (xtext_display_cache *cache, xtext_buffer *buf)
{
	xtext_line_display *evict;
	textentry *ent;

	if (g_queue_is_empty (&cache->lru_order))
		return;

	evict = g_queue_pop_tail (&cache->lru_order);
	g_hash_table_remove (cache->by_id, &evict->entry_id);

	/* Entry keeps its sublines — they're cheap (GSList of ints) and
	 * avoid full PangoLayout re-creation when scrolling back.  Only the
	 * PangoLayout (expensive) is freed via the cache.  Sublines go stale
	 * on width change (sublines_width != window_width), not on eviction. */
	line_display_free (evict);
}

/* Look up a cached display for an entry.  Returns NULL on miss or width mismatch. */
static xtext_line_display *
display_cache_get (xtext_display_cache *cache, guint64 entry_id, int window_width)
{
	GList *node;
	xtext_line_display *disp;

	if (!cache)
		return NULL;

	node = g_hash_table_lookup (cache->by_id, &entry_id);
	if (!node)
		return NULL;

	disp = node->data;
	if (disp->cached_width != window_width)
		return NULL;	/* stale — caller must recompute */

	/* Move to head (MRU) */
	g_queue_unlink (&cache->lru_order, node);
	g_queue_push_head_link (&cache->lru_order, node);
	return disp;
}

/* Store a display in the cache.  Evicts LRU if over capacity.
 * Takes ownership of disp — caller must not free. */
static void
display_cache_put (xtext_display_cache *cache, xtext_buffer *buf,
                   xtext_line_display *disp)
{
	GList *node;

	if (!cache || !disp)
		return;

	/* Remove old entry for this id if present */
	node = g_hash_table_lookup (cache->by_id, &disp->entry_id);
	if (node)
	{
		xtext_line_display *old = node->data;
		g_hash_table_remove (cache->by_id, &old->entry_id);
		g_queue_delete_link (&cache->lru_order, node);
		/* Don't free old->sublines — entry might still reference them.
		 * The entry's sublines pointer will be overwritten by the caller. */
		old->sublines = NULL;
		line_display_free (old);
	}

	/* Add to head (MRU) */
	g_queue_push_head (&cache->lru_order, disp);
	node = cache->lru_order.head;
	g_hash_table_insert (cache->by_id, &disp->entry_id, node);

	/* Evict LRU if over capacity */
	while (g_queue_get_length (&cache->lru_order) > (guint)cache->max_entries)
		display_cache_evict_lru (cache, buf);
}

/* Touch an entry — mark as recently used without storing new data */
static void
display_cache_touch (xtext_display_cache *cache, xtext_buffer *buf, guint64 entry_id)
{
	GList *node;

	if (!cache)
		return;

	node = g_hash_table_lookup (cache->by_id, &entry_id);
	if (node)
	{
		g_queue_unlink (&cache->lru_order, node);
		g_queue_push_head_link (&cache->lru_order, node);
	}
}

/* Remove an entry from the cache (entry being destroyed) */
static void
display_cache_remove (xtext_display_cache *cache, guint64 entry_id)
{
	GList *node;

	if (!cache)
		return;

	node = g_hash_table_lookup (cache->by_id, &entry_id);
	if (node)
	{
		xtext_line_display *disp = node->data;
		g_hash_table_remove (cache->by_id, &disp->entry_id);
		g_queue_delete_link (&cache->lru_order, node);
		line_display_free (disp);
	}
}

/* Flush entire cache (font/palette change) */
static void
display_cache_flush (xtext_display_cache *cache, xtext_buffer *buf)
{
	if (!cache)
		return;

	while (!g_queue_is_empty (&cache->lru_order))
		display_cache_evict_lru (cache, buf);
}

/* ======================================= */
/* ============ PANGO BACKEND ============ */
/* ======================================= */

/* EMPH_ITAL, EMPH_BOLD, EMPH_HIDDEN are now in xtext-render.h */
static PangoAttrList *attr_lists[4];
static int fontwidths[4][128];
static int fontwidths_pango[4][128];	/* widths in Pango units (1/1024 px) */

static PangoAttribute *
xtext_pango_attr (PangoAttribute *attr)
{
	attr->start_index = PANGO_ATTR_INDEX_FROM_TEXT_BEGINNING;
	attr->end_index = PANGO_ATTR_INDEX_TO_TEXT_END;
	return attr;
}

static void
xtext_pango_init (GtkXText *xtext)
{
	size_t i;
	int j;
	char buf[2] = "\000";

	if (attr_lists[0])
	{
		for (i = 0; i < (EMPH_ITAL | EMPH_BOLD); i++)
			pango_attr_list_unref (attr_lists[i]);
	}

	for (i = 0; i < sizeof attr_lists / sizeof attr_lists[0]; i++)
	{
		attr_lists[i] = pango_attr_list_new ();
		switch (i)
		{
		case 0:		/* Roman */
			break;
		case EMPH_ITAL:		/* Italic */
			pango_attr_list_insert (attr_lists[i],
				xtext_pango_attr (pango_attr_style_new (PANGO_STYLE_ITALIC)));
			break;
		case EMPH_BOLD:		/* Bold */
			pango_attr_list_insert (attr_lists[i],
				xtext_pango_attr (pango_attr_weight_new (PANGO_WEIGHT_BOLD)));
			break;
		case EMPH_ITAL | EMPH_BOLD:		/* Italic Bold */
			pango_attr_list_insert (attr_lists[i],
				xtext_pango_attr (pango_attr_style_new (PANGO_STYLE_ITALIC)));
			pango_attr_list_insert (attr_lists[i],
				xtext_pango_attr (pango_attr_weight_new (PANGO_WEIGHT_BOLD)));
			break;
		}

		/* Now initialize fontwidths[i] (pixel) and fontwidths_pango[i] (Pango units) */
		pango_layout_set_attributes (xtext->layout, attr_lists[i]);
		for (j = 0; j < 128; j++)
		{
			PangoRectangle logical;
			buf[0] = j;
			pango_layout_set_text (xtext->layout, buf, 1);
			pango_layout_get_extents (xtext->layout, NULL, &logical);
			fontwidths[i][j] = PANGO_PIXELS (logical.width);
			fontwidths_pango[i][j] = logical.width;
		}
	}
	xtext->space_width = fontwidths[0][' '];
}

static void
backend_font_close (GtkXText *xtext)
{
	pango_font_description_free (xtext->font->font);
}

static void
backend_init (GtkXText *xtext)
{
	if (xtext->layout == NULL)
	{
		xtext->layout = gtk_widget_create_pango_layout (GTK_WIDGET (xtext), 0); 
		if (xtext->font)
			pango_layout_set_font_description (xtext->layout, xtext->font->font);
	}
}

static PangoFontDescription *
backend_font_open_real (char *name)
{
	PangoFontDescription *font;

	font = pango_font_description_from_string (name);
	if (font && pango_font_description_get_size (font) == 0)
	{
		pango_font_description_free (font);
		font = pango_font_description_from_string ("sans 11");
	}
	if (!font)
		font = pango_font_description_from_string ("sans 11");

	/* hex_text_font is a combined main+alternative font string whose size
	 * may be stale on startup.  Override with hex_text_font_main's size. */
	{
		PangoFontDescription *main_desc = pango_font_description_from_string (prefs.hex_text_font_main);
		int main_size = pango_font_description_get_size (main_desc);
		if (main_size > 0)
			pango_font_description_set_size (font, main_size);
		pango_font_description_free (main_desc);
	}

	return font;
}

static void
backend_font_open (GtkXText *xtext, char *name)
{
	PangoLanguage *lang;
	PangoContext *context;
	PangoFontMetrics *metrics;

	xtext->font = &xtext->pango_font;
	xtext->font->font = backend_font_open_real (name);
	if (!xtext->font->font)
	{
		xtext->font = NULL;
		return;
	}

	backend_init (xtext);
	pango_layout_set_font_description (xtext->layout, xtext->font->font);
	xtext_pango_init (xtext);

	/* vte does it this way */
	context = gtk_widget_get_pango_context (GTK_WIDGET (xtext));
	lang = pango_context_get_language (context);
	metrics = pango_context_get_metrics (context, xtext->font->font, lang);
	xtext->font->ascent = pango_font_metrics_get_ascent (metrics) / PANGO_SCALE;
	xtext->font->descent = pango_font_metrics_get_descent (metrics) / PANGO_SCALE;

	/*
	 * In later versions of pango, a font's height should be calculated like
	 * this to account for line gap; a typical symptom of not doing so is
	 * cutting off the underscore on some fonts.
	 */
#if PANGO_VERSION_CHECK(1, 44, 0)
	xtext->fontsize = pango_font_metrics_get_height (metrics) / PANGO_SCALE + 1;

	if (xtext->fontsize == 0)
		xtext->fontsize = xtext->font->ascent + xtext->font->descent;
#else
	xtext->fontsize = xtext->font->ascent + xtext->font->descent;
#endif

	pango_font_metrics_unref (metrics);

	/* Update emoji sprite cache to match new font size */
	if (xtext->emoji_cache)
		xtext_emoji_cache_set_size (xtext->emoji_cache, xtext->fontsize);
}

/* Fast path for single ASCII character width using the font cache.
 * This avoids expensive Pango layout operations in tight loops like find_next_wrap.
 * For multi-character strings, use backend_get_text_width_emph instead. */
static inline int
backend_get_char_width (guchar c, int emphasis)
{
	if (c < 128)
		return fontwidths[emphasis][c];
	return -1;  /* Signal caller to use Pango for non-ASCII */
}

static int
backend_get_text_width_emph (GtkXText *xtext, guchar *str, int len, int emphasis)
{
	int width;

	if (*str == 0)
	{
		xtext->last_width_pango = 0;
		return 0;
	}

	if ((emphasis & EMPH_HIDDEN))
	{
		xtext->last_width_pango = 0;
		return 0;
	}
	emphasis &= (EMPH_ITAL | EMPH_BOLD);

	/* Fast path: single ASCII character - use cached width */
	if (len == 1 && *str < 128)
	{
		xtext->last_width_pango = fontwidths_pango[emphasis][*str];
		return fontwidths[emphasis][*str];
	}

	/* Use Pango's full-string width calculation to match actual rendering.
	 * Also store precise Pango-unit width for sub-pixel X tracking in
	 * render_str, eliminating per-chunk rounding accumulation. */
	pango_layout_set_attributes (xtext->layout, attr_lists[emphasis]);
	pango_layout_set_text (xtext->layout, (char *)str, len);
	{
		PangoRectangle logical;
		pango_layout_get_extents (xtext->layout, NULL, &logical);
		xtext->last_width_pango = logical.width;
		width = PANGO_PIXELS (logical.width);
	}

	return width;
}

static int
backend_get_text_width_slp (GtkXText *xtext, guchar *str, GSList *slp)
{
	int width = 0;

	while (slp)
	{
		offlen_t *meta;

		meta = slp->data;
		width += backend_get_text_width_emph (xtext, str, meta->len, meta->emph);
		str += meta->len;
		slp = g_slist_next (slp);
	}

	return width;
}

/* simplified version - draw layout line with Cairo */

static void
xtext_draw_layout_line (GtkXText         *xtext,
								gint              x,
								gint              y,
								PangoLayoutLine  *line)
{
	/* y is the baseline position. pango_cairo_show_layout_line draws with
	 * the current point as the left edge of the baseline. */
	cairo_move_to (xtext->cr, x, y);
	pango_cairo_show_layout_line (xtext->cr, line);
}

/* Set cairo source from palette, applying render_alpha for pending-state dimming */
static void
xtext_set_source_color (GtkXText *xtext, int color_index)
{
	if (xtext->render_alpha >= 1.0)
	{
		gdk_cairo_set_source_rgba (xtext->cr, &xtext->palette[color_index]);
	}
	else
	{
		GdkRGBA c = xtext->palette[color_index];
		c.alpha *= xtext->render_alpha;
		gdk_cairo_set_source_rgba (xtext->cr, &c);
	}
}

/* In GTK3/Cairo, we don't have separate GC objects - we just track the color indices
   and set them on the cairo context when needed */
static void
xtext_set_fg (GtkXText *xtext, int index)
{
	xtext->col_fore = index;
}

static void
xtext_set_bg (GtkXText *xtext, int index)
{
	xtext->col_back = index;
}

static void
gtk_xtext_init (GtkXText * xtext)
{
	xtext->pixmap = NULL;
	xtext->io_tag = 0;
	xtext->add_io_tag = 0;
	xtext->scroll_tag = 0;
	xtext->resize_tag = 0;
	xtext->max_lines = 0;
	xtext->col_back = XTEXT_BG;
	xtext->col_fore = XTEXT_FG;
	xtext->render_alpha = 1.0;
	xtext->pixel_offset = 0;
	xtext->font = NULL;
	xtext->layout = NULL;
	xtext->jump_out_offset = 0;
	xtext->jump_in_offset = 0;
	xtext->ts_x = 0;
	xtext->ts_y = 0;
	xtext->clip_x = 0;
	xtext->clip_x2 = 1000000;
	xtext->clip_y = 0;
	xtext->clip_y2 = 1000000;
	xtext->urlcheck_function = NULL;
	xtext->color_paste = FALSE;
	xtext->skip_border_fills = FALSE;
	xtext->scroll_top_debounce_tag = 0;
	xtext->scroll_top_backoff_ms = 500; /* Initial debounce: 500ms */
	xtext->scroll_to_top_cb = NULL;
	xtext->scroll_to_top_userdata = NULL;
	xtext->skip_stamp = FALSE;
	xtext->render_hilights_only = FALSE;
	xtext->un_hilight = FALSE;
	xtext->recycle = FALSE;
	xtext->dont_render = FALSE;
	xtext->dont_render2 = FALSE;
	xtext->emoji_cache = NULL;
	gtk_xtext_scroll_adjustments (xtext, NULL, NULL);

	xtext->scrollbar = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL, xtext->adj);
	gtk_widget_set_parent (xtext->scrollbar, GTK_WIDGET (xtext));

	/* GTK4: Set up event controllers for mouse/keyboard/scroll events */
	/* These replace the widget class vfuncs used in GTK3 */
	{
		GtkGesture *click_gesture;
		GtkEventController *motion_controller;
		GtkEventController *scroll_controller;

		/* Click gesture for button press/release */
		click_gesture = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click_gesture), 0); /* All buttons */
		g_signal_connect (click_gesture, "pressed",
		                  G_CALLBACK (gtk_xtext_button_press), xtext);
		g_signal_connect (click_gesture, "released",
		                  G_CALLBACK (gtk_xtext_button_release), xtext);
		gtk_widget_add_controller (GTK_WIDGET (xtext), GTK_EVENT_CONTROLLER (click_gesture));

		/* Motion controller for mouse movement and leave */
		motion_controller = gtk_event_controller_motion_new ();
		g_signal_connect (motion_controller, "motion",
		                  G_CALLBACK (gtk_xtext_motion_notify), xtext);
		g_signal_connect (motion_controller, "leave",
		                  G_CALLBACK (gtk_xtext_leave_notify), xtext);
		gtk_widget_add_controller (GTK_WIDGET (xtext), motion_controller);

		/* Scroll controller for mouse wheel */
		scroll_controller = gtk_event_controller_scroll_new (
			GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
		g_signal_connect (scroll_controller, "scroll",
		                  G_CALLBACK (gtk_xtext_scroll), xtext);
		gtk_widget_add_controller (GTK_WIDGET (xtext), scroll_controller);
	}
	/* GTK4: Clipboard text is set directly via gdk_clipboard_set_text() in
	 * gtk_xtext_set_clip_owner() - no need for selection targets/callbacks.
	 * Note: GTK4 doesn't notify when another app claims PRIMARY selection,
	 * so visual selection remains until user clicks elsewhere. */
}

static void
gtk_xtext_adjustment_set (xtext_buffer *buf, int fire_signal)
{
	GtkAdjustment *adj = buf->xtext->adj;
	gdouble upper, page_size, value;

	if (buf->xtext->buffer == buf)
	{
		int widget_height = gtk_widget_get_height (GTK_WIDGET (buf->xtext));

		upper = buf->num_lines;
		if (upper == 0)
			upper = 1;

		{
			int effective_height = widget_height;
			if (buf->xtext->status_strip_visible)
				effective_height -= (buf->xtext->fontsize * 2 / 3 + 4);
			/* Integer page_size: a fractional page_size makes upper - page_size
			 * fractional, which makes value fractional, which makes
			 * pixel_offset = (value - (int)value) * fontsize non-zero — the
			 * sub-line clip from snippet #1.  Floor to whole lines so that
			 * bottom-edge math stays exact.  The trade-off is a strip of
			 * background pixels below the last visible line equal to
			 * effective_height % fontsize, which matches the standard look
			 * in GtkTextView and other line-aligned scrollables. */
			page_size = (double)(effective_height / buf->xtext->fontsize);
		}
		value = gtk_adjustment_get_value (adj);

		if (value > upper - page_size)
		{
			buf->scroll_anchor.anchor_to_bottom = TRUE;
			value = upper - page_size;
		}

		if (value < 0)
			value = 0;

		gtk_adjustment_configure (adj, value, 0, upper, 1, page_size, page_size);
	}
}

static gint
gtk_xtext_adjustment_timeout (GtkXText * xtext)
{
	/* GTK3: Queue a redraw instead of rendering directly */
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	xtext->io_tag = 0;
	return 0;
}

/* Debounce timeout callback for scroll-to-top */
static gboolean
gtk_xtext_scroll_top_timeout (gpointer data)
{
	GtkXText *xtext = GTK_XTEXT (data);
	gdouble value;

	xtext->scroll_top_debounce_tag = 0;

	/* Verify we're still near the top of the current buffer.
	 * Buffer switches change the adjustment, so the original
	 * scroll-to-top condition may no longer hold.
	 * Check if the viewport includes the first materialized entry
	 * (within one page of it), which is stable across ensure_range
	 * anchor restores that shift the absolute adjustment value. */
	value = gtk_adjustment_get_value (xtext->adj);
	{
		double top_of_mat = LINES_BEFORE_MAT (xtext->buffer);
		double page = gtk_adjustment_get_page_size (xtext->adj);
		if (value > top_of_mat + page)
			return G_SOURCE_REMOVE;
	}

	/* Fire the callback if set */
	if (xtext->scroll_to_top_cb)
	{
		xtext->scroll_to_top_cb (xtext, xtext->scroll_to_top_userdata);

		/* Short backoff to coalesce rapid scroll events, but don't
		 * escalate aggressively — the user is actively scrolling. */
		xtext->scroll_top_backoff_ms = MIN (xtext->scroll_top_backoff_ms + 250, 2000);
	}

	return G_SOURCE_REMOVE;
}

static void
gtk_xtext_adjustment_changed (GtkAdjustment * adj, GtkXText * xtext)
{
	gdouble value, upper, page_size;

	if (!gtk_widget_get_realized (GTK_WIDGET (xtext)))
		return;

	value = gtk_adjustment_get_value (xtext->adj);
	upper = gtk_adjustment_get_upper (xtext->adj);
	page_size = gtk_adjustment_get_page_size (xtext->adj);

	if (xtext->buffer->old_value != value)
	{
		if (value >= upper - page_size - 1.0)
		{
			xtext->buffer->scroll_anchor.anchor_to_bottom = TRUE;
		}
		else
		{
			int anchor_subline;
			textentry *anchor_ent;
			int bot_line;

			xtext->buffer->scroll_anchor.anchor_to_bottom = FALSE;

			/* Convention: scroll_anchor.anchor_entry_id stores the entry+subline
			 * at the BOTTOM edge of the viewport, not the top.  This matches the
			 * bottom-anchored render path: render_page walks backward from this
			 * entry to fill the page, giving pixel-perfect bottom alignment.
			 * The top edge falls where it falls (potentially a partial line). */
			bot_line = (int) (value + page_size - 1.0);
			anchor_ent = gtk_xtext_nth (xtext, bot_line, &anchor_subline);
			if (anchor_ent)
			{
				xtext->buffer->scroll_anchor.anchor_entry_id = anchor_ent->entry_id;
				xtext->buffer->scroll_anchor.subline_offset = anchor_subline;
			}
		}

		/* Detect scroll-to-top for chathistory loading.
		 * For DB-backed buffers, the scrollbar minimum is lines_before_mat
		 * (not 0), so check relative to that offset. */
		{
			double top_threshold = LINES_BEFORE_MAT (xtext->buffer) + 1.0;
			if (value <= top_threshold && !xtext->buffer->scroll_anchor.anchor_to_bottom
			    && xtext->scroll_to_top_cb && upper > page_size)
			{
				/* Cancel existing debounce timer and start a new one */
				if (xtext->scroll_top_debounce_tag)
					g_source_remove (xtext->scroll_top_debounce_tag);

				xtext->scroll_top_debounce_tag = g_timeout_add (
					xtext->scroll_top_backoff_ms,
					gtk_xtext_scroll_top_timeout,
					xtext);
			}
			else if (value > top_threshold + page_size && xtext->scroll_top_debounce_tag)
			{
				/* User scrolled well away from top - cancel pending request.
				 * Use page_size as threshold to avoid false cancels from
				 * buffer switches or small position adjustments. */
				g_source_remove (xtext->scroll_top_debounce_tag);
				xtext->scroll_top_debounce_tag = 0;
			}
		}

		/* Virtual scrollback (Phase 3+5): ensure entries around viewport are loaded.
		 * Phase 5: only trigger when approaching the materialization boundary
		 * (within 1 page of the edge).  Scrolling within the safe interior of the
		 * materialized window does no DB work at all. */
		if (HAS_VIRT_DB (xtext->buffer) && xtext->buffer->avg_lines_per_entry > 0)
		{
			int scroll_line = (int) value;
			int mat_top = LINES_BEFORE_MAT (xtext->buffer);
			int mat_bot = mat_top + BUF_LINES_MAT (xtext->buffer);
			int margin = (int) page_size;  /* 1 page buffer before triggering */
			/* Detect "scrollbar click into the unmaterialized region":
			 * the click value is below mat_top, so the entry the user is
			 * pointing at is not currently in memory.  ensure_range will
			 * load entries around it, but ensure_range's internal
			 * save/restore_scroll_anchor_top would otherwise teleport
			 * value back to text_first (because pre-load nth(value)
			 * clamped there).  Remember the click target so we can
			 * re-place value after the load. */
			gboolean click_into_unmat = (scroll_line < mat_top);
			int click_target_value = scroll_line;

			/* Only load if viewport is within margin of mat boundary (or outside it).
			 * Skip the bottom-edge check when there are no entries beyond the
			 * materialized window — nothing to load, and the spurious trigger
			 * causes harmful head eviction that drifts lines_before_mat. */
			{
				int db_mat = BUF_MAT_COUNT (xtext->buffer) -
					xtext->buffer->ephemeral_count;
				int entries_after = xtext->buffer->total_entries -
					xtext->buffer->mat_first_index - db_mat;
				gboolean near_top = (scroll_line < mat_top + margin);
				gboolean near_bot = (entries_after > 0 &&
					scroll_line + (int) page_size > mat_bot - margin);

				if (near_top || near_bot || BUF_MAT_COUNT (xtext->buffer) == 0)
				{
					int idx;
					int radius;

					/* Compute the DB target index DIRECTIONALLY from the
					 * known boundaries of the materialized window, not from
					 * scroll_line / avg_lines_per_entry.  The avg-based
					 * estimate is unreliable: it can drift, and when
					 * scroll_line lies in the lines_after estimate region
					 * (which happens after the user scrolled up and new
					 * messages arrived as DB-only via virt_should_materialize),
					 * the extrapolated idx points far past the user's actual
					 * position, causing ensure_range to evict the visible
					 * window and producing a multi-day visual jump on a
					 * single wheel tick.
					 *
					 * The user is, by definition, viewing the materialized
					 * region — there is no need to compute their DB index.
					 * We only need to know which edge they are approaching
					 * and extend the window in that direction. */
					if (BUF_MAT_COUNT (xtext->buffer) == 0)
					{
						/* Initial load: nothing materialized yet, fall back
						 * to the avg-based estimate as the only available
						 * starting point. */
						if (xtext->buffer->avg_lines_per_entry > 0)
							idx = (int)(scroll_line /
								xtext->buffer->avg_lines_per_entry);
						else
							idx = 0;
					}
					else if (near_top)
					{
						/* Two sub-cases:
						 *
						 * (a) value is just inside mat_top — wheel scroll
						 *     approaching the head.  Extend the window
						 *     upward by one page; target mat_first - 1.
						 *
						 * (b) value is well below mat_top — scrollbar
						 *     click into the unmaterialized region.  The
						 *     user wants to land at an absolute line that
						 *     no anchor in the materialized window can
						 *     resolve to.  Compute a target DB index from
						 *     value / avg_lines_per_entry so ensure_range
						 *     loads around the click target instead of
						 *     adjacent to the current window. */
						if (scroll_line < mat_top)
							idx = (int)(scroll_line /
								xtext->buffer->avg_lines_per_entry);
						else
							idx = xtext->buffer->mat_first_index - 1;
					}
					else /* near_bot */
					{
						/* Extend the window downward.  Target the entry
						 * just after the current materialized end. */
						int mat_end_idx = xtext->buffer->mat_first_index +
							(BUF_MAT_COUNT (xtext->buffer) -
							 xtext->buffer->ephemeral_count) - 1;
						idx = mat_end_idx + 1;
					}

					if (idx < 0)
						idx = 0;
					if (idx >= xtext->buffer->total_entries)
						idx = xtext->buffer->total_entries - 1;

					radius = (int)(page_size * 2 /
						xtext->buffer->avg_lines_per_entry);
					if (radius < VIRT_PAGE_SIZE)
						radius = VIRT_PAGE_SIZE;

					gtk_xtext_virt_ensure_range (xtext->buffer, idx, radius);
				}
			}

			/* For a click into the unmaterialized region, ensure_range
			 * has now loaded the right window but its internal anchor
			 * restore moved value back to (the old) text_first, not the
			 * click target.  Re-place value to honor the click: nth on
			 * the post-load coordinate space resolves to a real entry
			 * close to where the user pointed, and we set value to that
			 * entry's actual line number.  buf->scroll_anchor is then
			 * refreshed to point at the BOTTOM-of-viewport entry per
			 * the convention used by render_page. */
			if (click_into_unmat)
			{
				int top_sub;
				textentry *top_target;
				int top_line;
				int new_lbm = LINES_BEFORE_MAT (xtext->buffer);

				/* Resolve the click target (will become the viewport top) in
				 * the new coordinate space. */
				top_target = gtk_xtext_nth (xtext, click_target_value,
					&top_sub);
				if (top_target)
				{
					top_line = gtk_xtext_entry_get_line (
						xtext->buffer, top_target);
					if (top_line >= 0)
					{
						gdouble new_val =
							(gdouble) (top_line + new_lbm + top_sub);
						gdouble new_upper = gtk_adjustment_get_upper (
							xtext->adj);
						gdouble new_page = gtk_adjustment_get_page_size (
							xtext->adj);
						int bot_sub;
						textentry *bot_target;

						if (new_val > new_upper - new_page)
							new_val = new_upper - new_page;
						if (new_val < 0)
							new_val = 0;
						g_signal_handler_block (xtext->adj,
							xtext->vc_signal_tag);
						gtk_adjustment_set_value (xtext->adj, new_val);
						g_signal_handler_unblock (xtext->adj,
							xtext->vc_signal_tag);

						/* The persistent anchor stores the BOTTOM entry of
						 * the viewport.  Resolve it from the new value. */
						bot_target = gtk_xtext_nth (xtext,
							(int) (new_val + new_page - 1.0), &bot_sub);
						if (bot_target)
						{
							xtext->buffer->scroll_anchor.anchor_entry_id =
								bot_target->entry_id;
							xtext->buffer->scroll_anchor.subline_offset =
								bot_sub;
						}
						xtext->buffer->scroll_anchor.anchor_to_bottom =
							FALSE;
						xtext->buffer->scroll_anchor.anchor_to_bottom = FALSE;
					}
				}
			}

			/* Re-read value after ensure_range + anchor restore, which may
			 * have shifted it.  Needed for the change-gate update below. */
			value = gtk_adjustment_get_value (xtext->adj);

			/* Re-check scroll-to-top after ensure_range.  When mat_first_index
			 * reaches 0, the DB is exhausted and only the server can provide
			 * more history.  Use a full page threshold since anchor restore
			 * may have shifted value away from 0. */
			{
				double top_of_mat = LINES_BEFORE_MAT (xtext->buffer);
				if (value <= top_of_mat + page_size && xtext->scroll_to_top_cb &&
				    gtk_adjustment_get_upper (xtext->adj) > page_size &&
				    xtext->buffer->mat_first_index == 0)
			{
				if (xtext->scroll_top_debounce_tag)
				{
					g_source_remove (xtext->scroll_top_debounce_tag);
					xtext->scroll_top_debounce_tag = 0;
				}
				xtext->scroll_to_top_cb (xtext, xtext->scroll_to_top_userdata);
				xtext->scroll_top_backoff_ms = MIN (xtext->scroll_top_backoff_ms + 250, 2000);
			}
			}
		}

		/* Queue a redraw.  The old code distinguished arrow clicks (±1 line,
		 * immediate redraw) from jumps (thumb drag, debounced 20ms).  With
		 * GTK4 queue_draw is already deferred to the next frame, so the
		 * distinction is unnecessary.  Cancel any pending debounce timer
		 * and redraw immediately. */
		if (xtext->io_tag)
		{
			g_source_remove (xtext->io_tag);
			xtext->io_tag = 0;
		}
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
	xtext->buffer->old_value = value;
}

GtkWidget *
gtk_xtext_new (GdkRGBA palette[], int separator)
{
	GtkXText *xtext;

	xtext = g_object_new (gtk_xtext_get_type (), NULL);
	xtext->separator = separator;
	xtext->wordwrap = TRUE;
	xtext->buffer = gtk_xtext_buffer_new (xtext);
	xtext->orig_buffer = xtext->buffer;
	xtext->cr = NULL;

	/* In GTK3, double buffering is handled differently */
	gtk_xtext_set_palette (xtext, palette);

	return GTK_WIDGET (xtext);
}

static void
gtk_xtext_dispose (GObject * object)
{
	GtkXText *xtext = GTK_XTEXT (object);

	if (xtext->add_io_tag)
	{
		g_source_remove (xtext->add_io_tag);
		xtext->add_io_tag = 0;
	}

	if (xtext->scroll_tag)
	{
		g_source_remove (xtext->scroll_tag);
		xtext->scroll_tag = 0;
	}

	if (xtext->resize_tag)
	{
		g_source_remove (xtext->resize_tag);
		xtext->resize_tag = 0;
	}

	if (xtext->scroll_top_debounce_tag)
	{
		g_source_remove (xtext->scroll_top_debounce_tag);
		xtext->scroll_top_debounce_tag = 0;
	}

	if (xtext->flash_tag)
	{
		g_source_remove (xtext->flash_tag);
		xtext->flash_tag = 0;
	}

	if (xtext->io_tag)
	{
		g_source_remove (xtext->io_tag);
		xtext->io_tag = 0;
	}

	if (xtext->pixmap)
	{
		cairo_surface_destroy (xtext->pixmap);
		xtext->pixmap = NULL;
	}

	if (xtext->emoji_cache)
	{
		xtext_emoji_cache_free (xtext->emoji_cache);
		xtext->emoji_cache = NULL;
	}

	gtk_xtext_status_clear (xtext);
	if (xtext->status_expire_timer)
	{
		g_source_remove (xtext->status_expire_timer);
		xtext->status_expire_timer = 0;
	}

	gtk_xtext_toast_clear (xtext);

	if (xtext->font)
	{
		backend_font_close (xtext);
		xtext->font = NULL;
	}

	g_clear_pointer (&xtext->scrollbar, gtk_widget_unparent);

	if (xtext->adj)
	{
		g_signal_handlers_disconnect_matched (G_OBJECT (xtext->adj),
					G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, xtext);
		g_object_unref (G_OBJECT (xtext->adj));
		xtext->adj = NULL;
	}

	/* GdkGC objects removed in GTK3 - no cleanup needed */

	g_free (xtext->reaction_click_msgid);
	xtext->reaction_click_msgid = NULL;
	g_free (xtext->reaction_click_text);
	xtext->reaction_click_text = NULL;

	if (xtext->hand_cursor)
	{
		g_object_unref (xtext->hand_cursor);
		xtext->hand_cursor = NULL;
	}

	if (xtext->resize_cursor)
	{
		g_object_unref (xtext->resize_cursor);
		xtext->resize_cursor = NULL;
	}

	if (xtext->orig_buffer)
	{
		gtk_xtext_buffer_free (xtext->orig_buffer);
		xtext->orig_buffer = NULL;
	}

	G_OBJECT_CLASS (parent_class)->dispose (object);
}


/* GTK4: Widgets no longer have GdkWindows - use simpler initialization */
static void
gtk_xtext_realize (GtkWidget * widget)
{
	GtkXText *xtext;

	/* GTK4: MUST chain up to parent class realize first */
	GTK_WIDGET_CLASS (parent_class)->realize (widget);

	xtext = GTK_XTEXT (widget);

	/* GTK4: Assume 32-bit depth (RGBA) */
	xtext->depth = 32;

	/* Initialize colors for separator/marker */
	/* for the separator bar (light) */
	xtext->light_color.red = 1.0;
	xtext->light_color.green = 1.0;
	xtext->light_color.blue = 1.0;
	xtext->light_color.alpha = 1.0;

	/* for the separator bar (dark) */
	xtext->dark_color.red = 0x1111 / 65535.0f;
	xtext->dark_color.green = 0x1111 / 65535.0f;
	xtext->dark_color.blue = 0x1111 / 65535.0f;
	xtext->dark_color.alpha = 1.0;

	/* for the separator bar (thinline) */
	xtext->thin_color.red = 0x8e38 / 65535.0f;
	xtext->thin_color.green = 0x8e38 / 65535.0f;
	xtext->thin_color.blue = 0x9f38 / 65535.0f;
	xtext->thin_color.alpha = 1.0;

	/* Set default foreground/background colors */
	xtext_set_fg (xtext, XTEXT_FG);
	xtext_set_bg (xtext, XTEXT_BG);

	xtext->draw_buf = NULL;
	xtext->ts_x = xtext->ts_y = 0;

	/* GTK4: Use cursor names instead of GDK_HAND1 etc */
	xtext->hand_cursor = gdk_cursor_new_from_name ("pointer", NULL);
	xtext->resize_cursor = gdk_cursor_new_from_name ("col-resize", NULL);

	backend_init (xtext);
}

/* GTK4: measure vfunc replaces get_preferred_width/height */
static void
gtk_xtext_measure (GtkWidget *widget,
                   GtkOrientation orientation,
                   int for_size,
                   int *minimum,
                   int *natural,
                   int *minimum_baseline,
                   int *natural_baseline)
{
	GtkXText *xtext = GTK_XTEXT (widget);
	int sb_min = 0, sb_nat = 0;

	if (xtext->scrollbar)
		gtk_widget_measure (xtext->scrollbar, orientation, -1, &sb_min, &sb_nat, NULL, NULL);

	if (orientation == GTK_ORIENTATION_HORIZONTAL)
	{
		/* Floor the widget width at the smallest size at which the indent
		 * clamp in size_allocate can still leave a renderable text column.
		 * Below this the render early-return at gtk_xtext_render_page()
		 * kicks in and the view goes blank. */
		*minimum = XTEXT_MIN_INDENT + XTEXT_MIN_TEXT_COL + MARGIN + sb_min;
		*natural = 200 + sb_nat;
	}
	else
	{
		*minimum = 90;
		*natural = 90;
	}

	if (minimum_baseline)
		*minimum_baseline = -1;
	if (natural_baseline)
		*natural_baseline = -1;
}

/* Historically there was a deferred resize_cb here (50ms timeout) that
 * ran an expensive Pango reflow pass after the immediate size_allocate.
 * With the lazy calc_lines path (recompute_sublines=FALSE) and
 * render_page's on-demand reflow, the deferred pass is redundant —
 * it does the same cheap O(n) sum the immediate path already did.
 * Removed entirely; resize_tag is kept in the struct for ABI but
 * never armed. */

/* GTK4: size_allocate has different signature - width, height, baseline */
static void
gtk_xtext_size_allocate (GtkWidget * widget, int width, int height, int baseline)
{
	GtkXText *xtext = GTK_XTEXT (widget);
	int sb_width = 0;
	int text_width;
	int old_width = xtext->buffer->window_width;
	int height_only = FALSE;

	/* Allocate internal scrollbar on the right edge */
	if (xtext->scrollbar)
	{
		int sb_min, sb_nat;
		GtkAllocation sb_alloc;

		gtk_widget_measure (xtext->scrollbar, GTK_ORIENTATION_HORIZONTAL, height,
		                    &sb_min, &sb_nat, NULL, NULL);
		sb_width = sb_nat;

		sb_alloc.x = width - sb_width;
		sb_alloc.y = 0;
		sb_alloc.width = sb_width;
		sb_alloc.height = height;
		gtk_widget_size_allocate (xtext->scrollbar, &sb_alloc, baseline);
	}

	text_width = width - sb_width;

	if (text_width == old_width)
		height_only = TRUE;

	xtext->buffer->window_width = text_width;
	xtext->buffer->window_height = height;

	/* If the widget narrowed, shrink the auto-indent so the right column
	 * keeps at least XTEXT_MIN_TEXT_COL pixels. This runs before the
	 * sublines recompute below so the new indent is reflected in the
	 * wrap layout on the same frame. */
	if (!height_only)
		gtk_xtext_clamp_indent_to_width (xtext->buffer);

	{
		gboolean was_down = xtext->buffer->scroll_anchor.anchor_to_bottom;
		dontscroll (xtext->buffer);	/* force scrolling off */
		/* Virtual scrollback: preserve anchor_to_bottom through layout reflows.
		 * Without this, the GTK layout phase (triggered by tab switch or
		 * adjustment changes) clears the flag, and the view drifts from the
		 * bottom because num_lines is an estimate.  The scroll anchor system
		 * still handles non-bottom positions correctly during width changes. */
		if (was_down)
		{
			xtext->buffer->scroll_anchor.anchor_to_bottom = TRUE;
		}
	}
	if (!height_only)
	{
		if (old_width == 0)
		{
			/* First real allocation — text was loaded before the widget had
			 * a width, so line counts are wrong.  Recalculate immediately
			 * and scroll to bottom if that's where we should be. */
			gtk_xtext_calc_lines (xtext->buffer, FALSE);
			if (xtext->buffer->scroll_anchor.anchor_to_bottom)
				gtk_adjustment_set_value (xtext->adj,
					gtk_adjustment_get_upper (xtext->adj) -
					gtk_adjustment_get_page_size (xtext->adj));
			gtk_widget_queue_draw (widget);
			return;
		}

		gtk_xtext_save_scroll_anchor (xtext->buffer, &xtext->resize_anchor);

		/* Lazy recompute: re-sum cached display_lines WITHOUT Pango
		 * reflow.  render_page reflows the ~30 visible entries on
		 * demand; everything else keeps its cached estimate.  With
		 * tail eviction bounding mat_count to ~500, this walk is
		 * sub-millisecond even in the worst case. */
		if (xtext->resize_tag)
		{
			g_source_remove (xtext->resize_tag);
			xtext->resize_tag = 0;
		}
		{
			gboolean was_down = xtext->buffer->scroll_anchor.anchor_to_bottom;
#if XTEXT_DEBUG_SCROLL
			gint64 t0 = g_get_monotonic_time ();
#endif
			if (xtext->vc_signal_tag)
				g_signal_handler_block (xtext->adj, xtext->vc_signal_tag);
			gtk_xtext_calc_lines_virtual_ex (xtext->buffer, TRUE, FALSE);
			if (xtext->vc_signal_tag)
				g_signal_handler_unblock (xtext->adj, xtext->vc_signal_tag);
			if (was_down)
			{
				xtext->buffer->scroll_anchor.anchor_to_bottom = TRUE;
			}
			gtk_xtext_restore_scroll_anchor (xtext->buffer, &xtext->resize_anchor);
			gtk_xtext_adjustment_set (xtext->buffer, FALSE);
#if XTEXT_DEBUG_SCROLL
			XT_DBG ("[resize] immediate: %lld us  mat=%d\n",
				(long long)(g_get_monotonic_time () - t0),
				BUF_MAT_COUNT (xtext->buffer));
#endif
		}
		gtk_widget_queue_draw (widget);
	}
	else
	{
		/* Height-only change: anchor to the bottom of the viewport.
		 * Adjust value so the same content stays at the bottom edge. */
		gboolean was_down = xtext->buffer->scroll_anchor.anchor_to_bottom;
		gdouble old_page = gtk_adjustment_get_page_size (xtext->adj);
		gdouble old_value = gtk_adjustment_get_value (xtext->adj);

		gtk_xtext_adjustment_set (xtext->buffer, FALSE);

		if (was_down)
		{
			xtext->buffer->scroll_anchor.anchor_to_bottom = TRUE;
		}
		else
		{
			gdouble new_page = gtk_adjustment_get_page_size (xtext->adj);
			gdouble new_value = old_value + old_page - new_page;
			gdouble upper = gtk_adjustment_get_upper (xtext->adj);
			if (new_value > upper - new_page)
				new_value = upper - new_page;
			if (new_value < 0)
				new_value = 0;
			gtk_adjustment_set_value (xtext->adj, new_value);
		}
		gtk_widget_queue_draw (widget);
	}
	if (xtext->buffer->scroll_anchor.anchor_to_bottom)
		gtk_adjustment_set_value (xtext->adj,
			gtk_adjustment_get_upper (xtext->adj) -
			gtk_adjustment_get_page_size (xtext->adj));
}

static int
gtk_xtext_selection_clear (xtext_buffer *buf)
{
	textentry *ent;
	int ret = 0;

	ent = gtk_xtext_find_by_id (buf, buf->last_ent_start_id);
	while (ent)
	{
		if (ent->mark_start != -1)
			ret = 1;
		ent->mark_start = -1;
		ent->mark_end = -1;
		if (ent->entry_id == buf->last_ent_end_id)
			break;
		ent = ent->next;
	}

	/* Clear selection pins — eviction can resume normally */
	buf->sel_pin_start_id = 0;
	buf->sel_pin_end_id = 0;

	return ret;
}

static int
find_x (GtkXText *xtext, textentry *ent, int x, int subline, int indent, gboolean use_trailing)
{
	/* Note: subline should already be adjusted for extra_lines_above by the caller
	 * (gtk_xtext_find_x handles this before calling find_x). */

	/* New path: use same PangoLayout as renderer for pixel-identical hit testing */
	if (ent->stripped_str && ent->raw_to_stripped_map)
	{
		int raw_offset, raw_len;
		int sub_start, sub_end, sub_len;
		int index, trailing;
		int stripped_hit;
		PangoAttrList *attrs;

		/* Get raw subline boundaries */
		if (subline > 0)
			raw_offset = GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, subline - 1));
		else
			raw_offset = 0;

		if (ent->sublines)
		{
			int cumulative = GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, subline));
			raw_len = cumulative - raw_offset;
		}
		else
			raw_len = ent->str_len - raw_offset;

		/* Convert to stripped offsets */
		sub_start = xtext_raw_to_stripped (ent->raw_to_stripped_map,
		                                   ent->str_len, raw_offset);
		sub_end = xtext_raw_to_stripped (ent->raw_to_stripped_map,
		                                 ent->str_len, raw_offset + raw_len);
		sub_len = sub_end - sub_start;

		if (sub_len <= 0)
			return raw_offset;

		/* Build same layout as renderer */
		{
			xtext_format_data fd = xtext_fdata_from_entry (ent);
			attrs = xtext_build_attrlist (&fd, sub_start, sub_len,
			                              xtext->palette,
			                              xtext->fontsize,
			                              xtext->font->ascent);
		}
		pango_layout_set_text (xtext->layout,
		                        (char *)(ent->stripped_str + sub_start), sub_len);
		pango_layout_set_attributes (xtext->layout, attrs);

		/* Hit test — x is relative to widget, subtract indent to get layout-relative.
		 * Use pango_layout_line_x_to_index for direct single-line hit testing.
		 * Account for layout logical x offset (e.g. left-side bearing). */
		{
			PangoLayoutLine *pline = pango_layout_get_lines_readonly (xtext->layout)->data;
			PangoRectangle line_logical;
			gboolean inside;
			pango_layout_line_get_extents (pline, NULL, &line_logical);
			int layout_x = (x - indent) * PANGO_SCALE - line_logical.x;
			inside = pango_layout_line_x_to_index (pline, layout_x,
			                                        &index, &trailing);

			/* x is past the end of text on this line — return str_len
			 * so callers treat it as out-of-bounds (matching old SLP behavior) */
			if (!inside && trailing > 0)
			{
				pango_attr_list_unref (attrs);
				pango_layout_set_attributes (xtext->layout, NULL);
				return ent->str_len;
			}
		}

		pango_attr_list_unref (attrs);
		pango_layout_set_attributes (xtext->layout, NULL);

		/* Advance past character when click is on its right half.
		 * Used for selection (include the character) but not for
		 * URL hit testing (need to be "on" the character). */
		if (use_trailing && trailing > 0 && index >= 0 && index < sub_len)
		{
			int mbl = charlen (ent->stripped_str + sub_start + index);
			index += mbl;
		}

		/* Clamp index to valid range */
		if (index < 0) index = 0;
		if (index > sub_len) index = sub_len;

		/* Convert stripped offset back to raw */
		stripped_hit = sub_start + index;
		return xtext_stripped_to_raw (ent->raw_to_stripped_map,
		                             ent->str_len, stripped_hit);
	}

	return 0;  /* no parsed data available */
}

static int
gtk_xtext_find_x (GtkXText * xtext, int x, textentry * ent, int subline,
						int line, int *out_of_bounds)
{
	int indent;
	unsigned char *str;

	/* Adjust for extra lines above (day separator, reply context) */
	subline -= ent->extra_lines_above;
	if (subline < 0)
	{
		*out_of_bounds = TRUE;
		return 0;  /* click was on day separator / reply context line */
	}

	if (subline < 1)
		indent = ent->indent;
	else
		indent = xtext->buffer->indent;

	if (line > gtk_adjustment_get_page_size (xtext->adj) || line < 0)
	{
		*out_of_bounds = TRUE;
		return 0;
	}

	str = ent->str + gtk_xtext_find_subline (xtext, ent, subline);
	if (str >= ent->str + ent->str_len)
		return 0;

	/* Let user select left a few pixels to grab hidden text e.g. '<' */
	if (x < indent - xtext->space_width)
	{
		*out_of_bounds = 1;
		return (str - ent->str);
	}

	*out_of_bounds = 0;

	return find_x (xtext, ent, x, subline, indent, TRUE);
}

static textentry *
gtk_xtext_find_char_ex (GtkXText * xtext, int x, int y, int *off,
                        int *out_of_bounds, int *out_subline)
{
	textentry *ent;
	int line;
	int subline;
	int outofbounds = FALSE;

	/* Adjust y value for negative rounding, double to int */
	if (y < 0)
		y -= xtext->fontsize;

	line = (y + xtext->pixel_offset) / xtext->fontsize;

	/* Walk from pagetop_ent when available — guarantees agreement with
	 * render_ents, which walks the same linked list.  The B-tree path
	 * (gtk_xtext_nth) can disagree when extra_lines_above/below create
	 * fractional weight mismatches. */
	if (xtext->buffer->pagetop_ent)
	{
		int remaining = line;
		int ent_lines;

		ent = xtext->buffer->pagetop_ent;
		subline = xtext->buffer->pagetop_subline;

		if (remaining >= 0)
		{
			/* Walk forward: advance past the remaining sublines of
			 * the starting entry, then step through following entries. */
			ent_lines = ENT_DISPLAY_LINES (ent) - subline;
			if (remaining < ent_lines)
			{
				subline += remaining;
				goto found;
			}
			remaining -= ent_lines;

			for (ent = ent->next; ent; ent = ent->next)
			{
				ent_lines = ENT_DISPLAY_LINES (ent);
				if (remaining < ent_lines)
				{
					subline = remaining;
					goto found;
				}
				remaining -= ent_lines;
			}
			return NULL;  /* past end of buffer */
		}
		else
		{
			/* Walk backward: y is above pagetop (e.g. selection anchor
			 * that was scrolled off the top of the viewport).  Count
			 * negative lines toward the previous entry's last subline. */
			int back = -remaining;
			if (back <= subline)
			{
				subline -= back;
				goto found;
			}
			back -= subline;
			for (ent = ent->prev; ent; ent = ent->prev)
			{
				ent_lines = ENT_DISPLAY_LINES (ent);
				if (back <= ent_lines)
				{
					subline = ent_lines - back;
					goto found;
				}
				back -= ent_lines;
			}
			return NULL;  /* above start of buffer */
		}

	found:
		/* find_x uses `line` only to flag out-of-bounds for clicks
		 * outside the viewport.  When we resolved via backward walk the
		 * line is negative; clamp to 0 so find_x computes the real x
		 * offset rather than returning 0.  Selection-draw decides what
		 * to do with an off-screen endpoint based on y, not this flag. */
		if (off)
			*off = gtk_xtext_find_x (xtext, x, ent, subline,
				line < 0 ? 0 : line, &outofbounds);
	}
	else
	{
		/* Fallback: B-tree lookup (pagetop not yet set, e.g., first render) */
		int abs_line = line + (int)gtk_adjustment_get_value (xtext->adj);
		int content_lines = LINES_BEFORE_MAT (xtext->buffer)
			+ totalweight234 (xtext->buffer->entry_tree);
		if (abs_line >= content_lines)
			return NULL;

		ent = gtk_xtext_nth (xtext, abs_line, &subline);
		if (!ent)
			return NULL;

		if (off)
			*off = gtk_xtext_find_x (xtext, x, ent, subline, line, &outofbounds);
	}
	if (out_of_bounds)
		*out_of_bounds = outofbounds;
	if (out_subline)
		*out_subline = subline;

	return ent;
}

static textentry *
gtk_xtext_find_char (GtkXText * xtext, int x, int y, int *off, int *out_of_bounds)
{
	return gtk_xtext_find_char_ex (xtext, x, y, off, out_of_bounds, NULL);
}

/* Recompute select_start_y from the pinned (entry_id, subline) relative
 * to the current pagetop.  Returns TRUE on successful resolution.  If
 * the anchor can't be found (entry evicted, no pagetop yet), leaves
 * select_start_y untouched and returns FALSE — callers can still proceed
 * with whatever stale value is there; the usual low/high fallbacks in
 * selection_draw will handle an out-of-viewport endpoint. */
static gboolean
gtk_xtext_resolve_select_start_y (GtkXText *xtext)
{
	textentry *ent;
	int line_count;

	if (xtext->select_start_entry_id == 0
	    || xtext->buffer->pagetop_ent == NULL)
		return FALSE;

	/* Walk forward from pagetop */
	ent = xtext->buffer->pagetop_ent;
	line_count = -xtext->buffer->pagetop_subline;
	while (ent)
	{
		if (ent->entry_id == xtext->select_start_entry_id)
		{
			/* Aim for the middle of the target subline so find_char's
			 * integer-division rounding for negative y doesn't push us
			 * one line off. */
			xtext->select_start_y =
				(line_count + xtext->select_start_subline)
					* xtext->fontsize
				- xtext->pixel_offset
				+ xtext->fontsize / 2;
			return TRUE;
		}
		line_count += ENT_DISPLAY_LINES (ent);
		ent = ent->next;
	}

	/* Not forward of pagetop — walk backward */
	line_count = -xtext->buffer->pagetop_subline;
	for (ent = xtext->buffer->pagetop_ent->prev; ent; ent = ent->prev)
	{
		line_count -= ENT_DISPLAY_LINES (ent);
		if (ent->entry_id == xtext->select_start_entry_id)
		{
			/* Aim for the middle of the target subline so find_char's
			 * integer-division rounding for negative y doesn't push us
			 * one line off. */
			xtext->select_start_y =
				(line_count + xtext->select_start_subline)
					* xtext->fontsize
				- xtext->pixel_offset
				+ xtext->fontsize / 2;
			return TRUE;
		}
	}

	return FALSE;
}

static void
gtk_xtext_draw_sep (GtkXText * xtext, int y)
{
	int x, height;
	int alloc_height;
	gboolean created_cr = FALSE;

	/* GTK4: We can only draw via the snapshot function - if no cr, just return */
	if (xtext->cr == NULL)
		return;
	alloc_height = gtk_widget_get_height (GTK_WIDGET (xtext));

	if (y == -1)
	{
		y = 0;
		height = alloc_height;
	} else
	{
		height = xtext->fontsize;
	}

	/* draw the separator line */
	if (xtext->separator && xtext->buffer->indent)
	{
		x = xtext->buffer->indent - ((xtext->space_width + 1) / 2);
		if (x < 1)
		{
			if (created_cr)
			{
				cairo_destroy (xtext->cr);
				xtext->cr = NULL;
			}
			return;
		}

		if (xtext->thinline)
		{
			if (xtext->moving_separator)
				gdk_cairo_set_source_rgba (xtext->cr, &xtext->light_color);
			else
				gdk_cairo_set_source_rgba (xtext->cr, &xtext->thin_color);
			cairo_move_to (xtext->cr, x + 0.5, y);
			cairo_line_to (xtext->cr, x + 0.5, y + height);
			cairo_stroke (xtext->cr);
		} else
		{
			if (xtext->moving_separator)
			{
				gdk_cairo_set_source_rgba (xtext->cr, &xtext->light_color);
				cairo_move_to (xtext->cr, x - 0.5, y);
				cairo_line_to (xtext->cr, x - 0.5, y + height);
				cairo_stroke (xtext->cr);
				gdk_cairo_set_source_rgba (xtext->cr, &xtext->dark_color);
				cairo_move_to (xtext->cr, x + 0.5, y);
				cairo_line_to (xtext->cr, x + 0.5, y + height);
				cairo_stroke (xtext->cr);
			} else
			{
				gdk_cairo_set_source_rgba (xtext->cr, &xtext->dark_color);
				cairo_move_to (xtext->cr, x - 0.5, y);
				cairo_line_to (xtext->cr, x - 0.5, y + height);
				cairo_stroke (xtext->cr);
				gdk_cairo_set_source_rgba (xtext->cr, &xtext->light_color);
				cairo_move_to (xtext->cr, x + 0.5, y);
				cairo_line_to (xtext->cr, x + 0.5, y + height);
				cairo_stroke (xtext->cr);
			}
		}
	}

	/* Clean up temporary cairo context if we created one */
	if (created_cr)
	{
		cairo_destroy (xtext->cr);
		xtext->cr = NULL;
	}
}

static void
gtk_xtext_draw_marker (GtkXText * xtext, textentry * ent, int y)
{
	int x, width, render_y;

	if (!xtext->marker) return;

	/* marker_pos points to the FIRST UNREAD message. The marker should be drawn
	 * ABOVE marker_pos (between last read and first unread). */
	if (xtext->buffer->marker_pos_id == ent->entry_id)
	{
		/* We're rendering marker_pos (first unread) - draw marker at top of entry */
		render_y = y + 4;
	}
	else if (ent->next != NULL && xtext->buffer->marker_pos_id == ent->next->entry_id)
	{
		/* We're rendering the last read entry - draw marker after all lines */
		render_y = y + xtext->fontsize * ENT_DISPLAY_LINES (ent) + 4;
	}
	else return;

	x = 0;
	width = xtext->buffer->window_width;

	gdk_cairo_set_source_rgba (xtext->cr, &xtext->palette[XTEXT_MARKER]);
	cairo_move_to (xtext->cr, x, render_y + 0.5);
	cairo_line_to (xtext->cr, x + width, render_y + 0.5);
	cairo_stroke (xtext->cr);

	if (gtk_window_is_active (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (xtext)))))
	{
		xtext->buffer->marker_seen = TRUE;
	}
}

static void
gtk_xtext_paint (GtkWidget *widget, GdkRectangle *area)
{
	GtkXText *xtext = GTK_XTEXT (widget);
	textentry *ent_start, *ent_end;
	int x, y;
	int widget_width = xtext->buffer->window_width;
	int widget_height = gtk_widget_get_height (widget);

	if (area->x == 0 && area->y == 0 &&
		 area->height == widget_height &&
		 area->width == widget_width)
	{
		dontscroll (xtext->buffer);	/* force scrolling off */
		gtk_xtext_render_page (xtext);
		return;
	}

	ent_start = gtk_xtext_find_char (xtext, area->x, area->y, NULL, NULL);
	if (!ent_start)
	{
		xtext_draw_bg (xtext, area->x, area->y, area->width, area->height);
		goto xit;
	}
	ent_end = gtk_xtext_find_char (xtext, area->x + area->width,
											 area->y + area->height, NULL, NULL);
	if (!ent_end)
		ent_end = xtext->buffer->text_last;

	xtext->clip_x = area->x;
	xtext->clip_x2 = area->x + area->width;
	xtext->clip_y = area->y;
	xtext->clip_y2 = area->y + area->height;

	/* y is the last pixel y location it rendered text at */
	y = gtk_xtext_render_ents (xtext, ent_start, ent_end);

	if (y && y < widget_height && !ent_end->next)
	{
		GdkRectangle rect;

		rect.x = 0;
		rect.y = y;
		rect.width = widget_width;
		rect.height = widget_height - y;

		/* fill any space below the last line that also intersects with
			the exposure rectangle */
		if (gdk_rectangle_intersect (area, &rect, &rect))
		{
			xtext_draw_bg (xtext, rect.x, rect.y, rect.width, rect.height);
		}
	}

	xtext->clip_x = 0;
	xtext->clip_x2 = 1000000;
	xtext->clip_y = 0;
	xtext->clip_y2 = 1000000;

xit:
	x = xtext->buffer->indent - ((xtext->space_width + 1) / 2);
	if (area->x <= x)
		gtk_xtext_draw_sep (xtext, -1);
}

/* GTK3 draw signal handler - replaces expose_event */
/*
 * Draw/Snapshot functions
 * GTK3: draw vfunc with cairo_t
 * GTK4: snapshot vfunc with GtkSnapshot
 */
static void
gtk_xtext_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
	GtkXText *xtext = GTK_XTEXT (widget);
	GdkRectangle area;
	int width, height;
	cairo_t *cr;
	graphene_rect_t bounds;

	/* Use buffer's window_width (excludes scrollbar) for text drawing */
	width = xtext->buffer->window_width;
	height = gtk_widget_get_height (widget);

	/* Create bounds for the snapshot */
	graphene_rect_init (&bounds, 0, 0, width, height);

	/* Get a cairo context from the snapshot */
	cr = gtk_snapshot_append_cairo (snapshot, &bounds);

	/* Store the cairo context for use by drawing functions */
	xtext->cr = cr;

	area.x = 0;
	area.y = 0;
	area.width = width;
	area.height = height;

	gtk_xtext_paint (widget, &area);

	xtext->cr = NULL;
	cairo_destroy (cr);

	/* Snapshot the internal scrollbar child */
	if (xtext->scrollbar)
		gtk_widget_snapshot_child (widget, xtext->scrollbar, snapshot);
}

/* render a selection that has extended or contracted upward */

static void
gtk_xtext_selection_up (GtkXText *xtext, textentry *start, textentry *end,
								int start_offset)
{
	if (!start || !end)
		return;

	/* render all the complete lines */
	if (start->next == end)
		gtk_xtext_render_ents (xtext, end, NULL);
	else
		gtk_xtext_render_ents (xtext, start->next, end);

	/* now the incomplete upper line */
	if (start->entry_id == xtext->buffer->last_ent_start_id)
		xtext->jump_in_offset = xtext->buffer->last_offset_start;
	else
		xtext->jump_in_offset = start_offset;
	gtk_xtext_render_ents (xtext, start, NULL);
	xtext->jump_in_offset = 0;
}

/* render a selection that has extended or contracted downward */

static void
gtk_xtext_selection_down (GtkXText *xtext, textentry *start, textentry *end,
								  int end_offset)
{
	if (!start || !end)
		return;

	/* render all the complete lines */
	if (end->prev == start)
		gtk_xtext_render_ents (xtext, start, NULL);
	else
		gtk_xtext_render_ents (xtext, start, end->prev);

	/* now the incomplete bottom line */
	if (end->entry_id == xtext->buffer->last_ent_end_id)
		xtext->jump_out_offset = xtext->buffer->last_offset_end;
	else
		xtext->jump_out_offset = end_offset;
	gtk_xtext_render_ents (xtext, end, NULL);
	xtext->jump_out_offset = 0;
}

static void
gtk_xtext_selection_render (GtkXText *xtext, textentry *start_ent, textentry *end_ent)
{
	textentry *ent;
	int start_offset = start_ent->mark_start;
	int end_offset = end_ent->mark_end;
	int start, end;

	xtext->skip_border_fills = TRUE;
	xtext->skip_stamp = TRUE;

	/* force an optimized render if there was no previous selection */
	if (xtext->buffer->last_ent_start_id == 0 && start_ent == end_ent)
	{
		xtext->buffer->last_offset_start = start_offset;
		xtext->buffer->last_offset_end = end_offset;
		goto lamejump;
	}

	/* mark changed within 1 ent only? */
	if (xtext->buffer->last_ent_start_id == start_ent->entry_id &&
		 xtext->buffer->last_ent_end_id == end_ent->entry_id)
	{
		/* when only 1 end of the selection is changed, we can really
			save on rendering */
		if (xtext->buffer->last_offset_start == start_offset ||
			 xtext->buffer->last_offset_end == end_offset)
		{
lamejump:
			ent = end_ent;
			/* figure out where to start and end the rendering */
			if (end_offset > xtext->buffer->last_offset_end)
			{
				end = end_offset;
				start = xtext->buffer->last_offset_end;
			} else if (end_offset < xtext->buffer->last_offset_end)
			{
				end = xtext->buffer->last_offset_end;
				start = end_offset;
			} else if (start_offset < xtext->buffer->last_offset_start)
			{
				end = xtext->buffer->last_offset_start;
				start = start_offset;
				ent = start_ent;
			} else if (start_offset > xtext->buffer->last_offset_start)
			{
				end = start_offset;
				start = xtext->buffer->last_offset_start;
				ent = start_ent;
			} else
			{	/* WORD selects end up here */
				end = end_offset;
				start = start_offset;
			}
		} else
		{
			/* LINE selects end up here */
			/* so which ent actually changed? */
			ent = start_ent;
			if (xtext->buffer->last_offset_start == start_offset)
				ent = end_ent;

			end = MAX (xtext->buffer->last_offset_end, end_offset);
			start = MIN (xtext->buffer->last_offset_start, start_offset);
		}

		xtext->jump_out_offset = end;
		xtext->jump_in_offset = start;
		gtk_xtext_render_ents (xtext, ent, NULL);
		xtext->jump_out_offset = 0;
		xtext->jump_in_offset = 0;
	}
	/* marking downward? */
	else if (xtext->buffer->last_ent_start_id == start_ent->entry_id &&
				xtext->buffer->last_offset_start == start_offset)
	{
		/* find the range that covers both old and new selection */
		textentry *last_end = gtk_xtext_find_by_id (xtext->buffer, xtext->buffer->last_ent_end_id);
		ent = start_ent;
		while (ent)
		{
			if (ent == last_end)
			{
				gtk_xtext_selection_down (xtext, ent, end_ent, end_offset);
				/*gtk_xtext_render_ents (xtext, ent, end_ent);*/
				break;
			}
			if (ent == end_ent)
			{
				gtk_xtext_selection_down (xtext, ent, last_end, end_offset);
				/*gtk_xtext_render_ents (xtext, ent, last_end);*/
				break;
			}
			ent = ent->next;
		}
	}
	/* marking upward? */
	else if (xtext->buffer->last_ent_start_id != 0 &&
				xtext->buffer->last_ent_end_id == end_ent->entry_id &&
				xtext->buffer->last_offset_end == end_offset)
	{
		textentry *last_start = gtk_xtext_find_by_id (xtext->buffer, xtext->buffer->last_ent_start_id);
		ent = end_ent;
		while (ent)
		{
			if (ent == start_ent && last_start)
			{
				gtk_xtext_selection_up (xtext, last_start, ent, start_offset);
				/*gtk_xtext_render_ents (xtext, last_start, ent);*/
				break;
			}
			if (ent == last_start)
			{
				gtk_xtext_selection_up (xtext, start_ent, ent, start_offset);
				/*gtk_xtext_render_ents (xtext, start_ent, ent);*/
				break;
			}
			ent = ent->prev;
		}
	}
	else	/* cross-over mark (stretched or shrunk at both ends) */
	{
		textentry *old_start = gtk_xtext_find_by_id (xtext->buffer, xtext->buffer->last_ent_start_id);
		textentry *old_end = gtk_xtext_find_by_id (xtext->buffer, xtext->buffer->last_ent_end_id);
		/* unrender the old mark */
		gtk_xtext_render_ents (xtext, old_start, old_end);
		/* now render the new mark, but skip overlaps */
		if (start_ent == old_start)
		{
			/* if the new mark is a sub-set of the old, do nothing */
			if (start_ent != end_ent)
				gtk_xtext_render_ents (xtext, start_ent->next, end_ent);
		} else if (end_ent == old_end)
		{
			/* if the new mark is a sub-set of the old, do nothing */
			if (start_ent != end_ent)
				gtk_xtext_render_ents (xtext, start_ent, end_ent->prev);
		} else
			gtk_xtext_render_ents (xtext, start_ent, end_ent);
	}

	xtext->buffer->last_ent_start_id = start_ent->entry_id;
	xtext->buffer->last_ent_end_id = end_ent->entry_id;

	/* Update selection pins for virtual scrollback eviction protection */
	xtext->buffer->sel_pin_start_id = start_ent->entry_id;
	xtext->buffer->sel_pin_end_id = end_ent->entry_id;
	xtext->buffer->last_offset_start = start_offset;
	xtext->buffer->last_offset_end = end_offset;

	xtext->skip_border_fills = FALSE;
	xtext->skip_stamp = FALSE;

	/* GTK3: gdk_cairo_create is deprecated and doesn't work reliably for
	 * drawing outside the draw callback. Queue a redraw to ensure the
	 * selection is properly rendered through the draw signal handler. */
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

static void
gtk_xtext_selection_draw (GtkXText * xtext, void * event, gboolean render)
{
	textentry *ent;
	textentry *ent_end;
	textentry *ent_start;
	int offset_start = 0;
	int offset_end = 0;
	textentry *low_ent, *high_ent;
	int low_x, low_y, low_offs;
	int high_x, high_y, high_offs, high_len;

	if (xtext->buffer->text_first == NULL)
		return;

	/* Re-derive select_start_y from the pinned (entry_id, subline) so the
	 * anchor survives scrolls and buffer mutations that happen between
	 * mousedown and this call.  select_end_y is fresh from the motion
	 * event and needs no resolve. */
	gtk_xtext_resolve_select_start_y (xtext);

	ent_start = gtk_xtext_find_char (xtext, xtext->select_start_x, xtext->select_start_y, &offset_start, NULL);
	ent_end = gtk_xtext_find_char (xtext, xtext->select_end_x, xtext->select_end_y, &offset_end, NULL);
	if (ent_start == NULL && ent_end == NULL)
		return;

	if	((ent_start != ent_end && xtext->select_start_y > xtext->select_end_y) || /* different entries */
		(ent_start == ent_end && offset_start > offset_end))	/* same entry, different character offsets */
	{
		/* marking up */
		low_ent = ent_end;
		low_x = xtext->select_end_x;
		low_y = xtext->select_end_y;
		low_offs = offset_end;
		high_ent = ent_start;
		high_x = xtext->select_start_x;
		high_y = xtext->select_start_y;
		high_offs = offset_start;
	}
	else
	{
		/* marking down */
		low_ent = ent_start;
		low_x = xtext->select_start_x;
		low_y = xtext->select_start_y;
		low_offs = offset_start;
		high_ent = ent_end;
		high_x = xtext->select_end_x;
		high_y = xtext->select_end_y;
		high_offs = offset_end;
	}
	if (low_ent == NULL)
	{
		low_ent = xtext->buffer->text_first;
		low_offs = 0;
	}
	if (high_ent == NULL)
	{
		high_ent = xtext->buffer->text_last;
		high_offs = high_ent->str_len;
	}

	/* word selection */
	if (xtext->word_select)
	{
		/* a word selection cannot be started if the cursor is out of bounds in gtk_xtext_button_press */
		gtk_xtext_get_word (xtext, low_x, low_y, NULL, &low_offs, NULL, NULL);

		/* in case the cursor is out of bounds we keep offset_end from gtk_xtext_find_char and fix the length */
		if (gtk_xtext_get_word (xtext, high_x, high_y, NULL, &high_offs, &high_len, NULL) == NULL)
			high_len = high_offs == high_ent->str_len? 0: -1; /* -1 for the space, 0 if at the end */
		high_offs += high_len;
		if (low_y < 0)
			low_offs = xtext->buffer->last_offset_start;
		if (high_y > xtext->buffer->window_height)
			high_offs = xtext->buffer->last_offset_end;
	}
	/* line/ent selection */
	else if (xtext->line_select)
	{
		low_offs = 0;
		high_offs = high_ent->str_len;
	}
	/* character selection */
	else
	{
		if (low_y < 0)
			low_offs = xtext->buffer->last_offset_start;
		if (high_y > xtext->buffer->window_height)
			high_offs = xtext->buffer->last_offset_end;
	}

	/* set all the old mark_ fields to -1 */
	gtk_xtext_selection_clear (xtext->buffer);

	low_ent->mark_start = low_offs;
	low_ent->mark_end = high_offs;

	if (low_ent != high_ent)
	{
		low_ent->mark_end = low_ent->str_len;
		if (high_offs != 0)
		{
			high_ent->mark_start = 0;
			high_ent->mark_end = high_offs;
		}

		/* set all the mark_ fields of the ents within the selection */
		ent = low_ent->next;
		while (ent && ent != high_ent)
		{
			ent->mark_start = 0;
			ent->mark_end = ent->str_len;
			ent = ent->next;
		}
	}

	if (render)
		gtk_xtext_selection_render (xtext, low_ent, high_ent);
}

static int
gtk_xtext_timeout_ms (GtkXText *xtext, int pixes)
{
	int apixes = abs(pixes);

	if (apixes < 6) return 100;
	if (apixes < 12) return 50;
	if (apixes < 20) return 20;
	return 10;
}
static gint
gtk_xtext_scrolldown_timeout (GtkXText * xtext)
{
	int p_y, win_height;
	xtext_buffer *buf = xtext->buffer;
	GtkAdjustment *adj = xtext->adj;
	gdouble adj_value, adj_upper, adj_page_size;

	/* GTK4: Use stored position from motion events */
	p_y = xtext->select_end_y;
	win_height = gtk_widget_get_height (GTK_WIDGET (xtext));

	adj_value = gtk_adjustment_get_value (adj);
	adj_upper = gtk_adjustment_get_upper (adj);
	adj_page_size = gtk_adjustment_get_page_size (adj);

	if (buf->last_ent_end_id == 0 ||	/* If context has changed OR */
		 buf->pagetop_ent == NULL ||	/* pagetop_ent is reset OR */
		 p_y <= win_height ||			/* pointer not below bottom margin OR */
		 adj_value >= adj_upper - adj_page_size) 	/* we're scrolled to bottom */
	{
		xtext->scroll_tag = 0;
		return 0;
	}

	gtk_adjustment_set_value (adj, adj_value + 1);
	gtk_xtext_selection_draw (xtext, NULL, TRUE);
	if (buf->pagetop_ent)
		gtk_xtext_render_ents (xtext, buf->pagetop_ent->next, gtk_xtext_find_by_id (buf, buf->last_ent_end_id));
	/* GTK3: Queue redraw after scroll selection update */
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	xtext->scroll_tag = g_timeout_add (gtk_xtext_timeout_ms (xtext, p_y - win_height),
													(GSourceFunc)
													gtk_xtext_scrolldown_timeout,
													xtext);

	return 0;
}

static gint
gtk_xtext_scrollup_timeout (GtkXText * xtext)
{
	int p_y;
	xtext_buffer *buf = xtext->buffer;
	GtkAdjustment *adj = xtext->adj;
	gdouble adj_value;

	/* GTK4: Use stored position from motion events */
	p_y = xtext->select_end_y;
	adj_value = gtk_adjustment_get_value (adj);

	if (buf->last_ent_start_id == 0 ||	/* If context has changed OR */
		 buf->pagetop_ent == NULL ||		/* pagetop_ent is reset OR */
		 p_y >= 0 ||							/* not above top margin OR */
		 adj_value == 0)						/* we're scrolled to the top */
	{
		xtext->scroll_tag = 0;
		return 0;
	}

	if (adj_value < 0)
	{
		gtk_adjustment_set_value (adj, 0);
	} else {
		gtk_adjustment_set_value (adj, adj_value - 1);
	}
	gtk_xtext_selection_draw (xtext, NULL, TRUE);
	if (buf->pagetop_ent)
		gtk_xtext_render_ents (xtext, buf->pagetop_ent->prev, gtk_xtext_find_by_id (buf, buf->last_ent_end_id));
	/* GTK3: Queue redraw after scroll selection update */
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	xtext->scroll_tag = g_timeout_add (gtk_xtext_timeout_ms (xtext, p_y),
													(GSourceFunc)
													gtk_xtext_scrollup_timeout,
													xtext);

	return 0;
}

static void
gtk_xtext_selection_update (GtkXText * xtext, void * event, int p_y, gboolean render)
{
	int win_height;
	gdouble adj_value, adj_upper, adj_page_size;

	if (xtext->scroll_tag)
	{
		return;
	}

	win_height = gtk_widget_get_height (GTK_WIDGET (xtext));
	adj_value = gtk_adjustment_get_value (xtext->adj);
	adj_upper = gtk_adjustment_get_upper (xtext->adj);
	adj_page_size = gtk_adjustment_get_page_size (xtext->adj);

	/* selecting past top of window, scroll up! */
	if (p_y < 0 && adj_value >= 0)
	{
		gtk_xtext_scrollup_timeout (xtext);
	}

	/* selecting past bottom of window, scroll down! */
	else if (p_y > win_height &&
		 adj_value < (adj_upper - adj_page_size))
	{
		gtk_xtext_scrolldown_timeout (xtext);
	}
	else
	{
		gtk_xtext_selection_draw (xtext, event, render);
	}
}

static char *
gtk_xtext_get_word (GtkXText * xtext, int x, int y, textentry ** ret_ent,
						  int *ret_off, int *ret_len, GSList **slp)
{
	textentry *ent;
	int offset;
	unsigned char *word;
	unsigned char *last, *end;
	int len;
	int out_of_bounds = 0;
	int len_to_offset = 0;

	ent = gtk_xtext_find_char (xtext, x, y, &offset, &out_of_bounds);
	if (ent == NULL || out_of_bounds || offset < 0 || offset >= ent->str_len)
		return NULL;

	word = ent->str + offset;
	while ((word = g_utf8_find_prev_char (ent->str, word)))
	{
		if (is_del (*word))
		{
			word++;
			break;
		}
		len_to_offset += charlen (word);
	}
	if (!word)
		word = ent->str;

	/* remove color characters from the length */
	gtk_xtext_strip_color (word, len_to_offset, xtext->scratch_buffer, &len_to_offset, NULL, FALSE);

	last = word;
	end = ent->str + ent->str_len;
	len = 0;
	do
	{
		if (is_del (*last))
			break;
		len += charlen (last);
		last = g_utf8_find_next_char (last, end);
	}
	while (last);

	if (len > 0 && word[len-1]=='.')
		len--;

	if (ret_ent)
		*ret_ent = ent;
	if (ret_off)
		*ret_off = word - ent->str;
	if (ret_len)
		*ret_len = len;		/* Length before stripping */

	word = gtk_xtext_strip_color (word, len, xtext->scratch_buffer, NULL, slp, FALSE);

	/* avoid turning the cursor into a hand for non-url part of the word */
	if (xtext->urlcheck_function && xtext->urlcheck_function (GTK_WIDGET (xtext), word))
	{
		int start, end;
		url_last (&start, &end);

		/* make sure we're not before the start of the match */
		if (len_to_offset < start)
			return NULL;

		/* and not after it */
		if (len_to_offset - start >= end - start)
			return NULL;
	}

	return word;
}

static void
gtk_xtext_unrender_hilight (GtkXText *xtext)
{
	xtext->render_hilights_only = TRUE;
	xtext->skip_border_fills = TRUE;
	xtext->skip_stamp = TRUE;
	xtext->un_hilight = TRUE;

	gtk_xtext_render_ents (xtext, xtext->hilight_ent, NULL);

	xtext->render_hilights_only = FALSE;
	xtext->skip_border_fills = FALSE;
	xtext->skip_stamp = FALSE;
	xtext->un_hilight = FALSE;

	/* GTK3: Queue redraw to ensure highlight changes are visible */
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

/*
 * Common leave handler logic - shared by both GTK3 and GTK4
 */
static void
gtk_xtext_leave_common (GtkXText *xtext)
{
	GtkWidget *widget = GTK_WIDGET (xtext);

	if (xtext->cursor_hand)
	{
		gtk_xtext_unrender_hilight (xtext);
		xtext->hilight_start = -1;
		xtext->hilight_end = -1;
		xtext->cursor_hand = FALSE;
		gtk_widget_set_cursor (widget, NULL);
		xtext->hilight_ent = NULL;
	}

	if (xtext->cursor_resize)
	{
		gtk_xtext_unrender_hilight (xtext);
		xtext->hilight_start = -1;
		xtext->hilight_end = -1;
		xtext->cursor_resize = FALSE;
		gtk_widget_set_cursor (widget, NULL);
		xtext->hilight_ent = NULL;
	}

}

/* Format a relative time string for hover stamp display */
static void
xtext_format_relative_time (time_t stamp, char *buf, int bufsize)
{
	time_t now = time (NULL);
	time_t diff = now - stamp;

	if (diff < 60)
		g_snprintf (buf, bufsize, _("Just now"));
	else if (diff < 3600)
		g_snprintf (buf, bufsize, _("%dm ago"), (int)(diff / 60));
	else if (diff < 86400)
	{
		int hours = (int)(diff / 3600);
		int mins = (int)((diff % 3600) / 60);
		if (mins > 0)
			g_snprintf (buf, bufsize, "%dh%dm", hours, mins);
		else
			g_snprintf (buf, bufsize, "%dh", hours);
	}
	else
	{
		int days = (int)(diff / 86400);
		if (days < 30)
		{
			int hours = (int)((diff % 86400) / 3600);
			if (hours > 0 && days < 7)
				g_snprintf (buf, bufsize, "%dd%dh", days, hours);
			else
				g_snprintf (buf, bufsize, "%dd", days);
		}
		else if (days < 365)
		{
			int months = days / 30;
			int rem = days % 30;
			if (rem > 0)
				g_snprintf (buf, bufsize, "%dmo%dd", months, rem);
			else
				g_snprintf (buf, bufsize, "%dmo", months);
		}
		else
		{
			int years = days / 365;
			int months = (days % 365) / 30;
			if (months > 0)
				g_snprintf (buf, bufsize, "%dy%dmo", years, months);
			else
				g_snprintf (buf, bufsize, "%dy", years);
		}
	}
}

/* Timer callback: show hover stamp after delay */
static gboolean
xtext_hover_stamp_timeout (gpointer data)
{
	GtkXText *xtext = data;
	xtext->hover_stamp_tag = 0;
	xtext->hover_stamp_visible = TRUE;
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	return G_SOURCE_REMOVE;
}

/* Timer callback: hide hover stamp after linger period */
static gboolean
xtext_hover_stamp_linger_timeout (gpointer data)
{
	GtkXText *xtext = data;
	xtext->hover_stamp_tag = 0;
	xtext->hover_stamp_visible = FALSE;
	xtext->hover_stamp_alt = FALSE;
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	return G_SOURCE_REMOVE;
}

/* Clear hover state — only called on actual widget leave, not on URL check fallthrough */
static void
gtk_xtext_leave_hover (GtkXText *xtext)
{
	if (xtext->hover_ent || xtext->hover_reply_target)
	{
		xtext->hover_ent = NULL;
		xtext->hover_reply_target = NULL;
		xtext->hover_btn_size = 0;
		xtext->redact_confirm_ent = NULL;
		if (xtext->hover_stamp_tag)
		{
			g_source_remove (xtext->hover_stamp_tag);
			xtext->hover_stamp_tag = 0;
		}
		xtext->hover_stamp_visible = FALSE;
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
}

/*
 * Leave notify event handler
 * GTK3: widget_class vfunc with GdkEventCrossing
 * GTK4: GtkEventControllerMotion "leave" signal
 */
static void
gtk_xtext_leave_notify (GtkEventControllerMotion *controller, gpointer user_data)
{
	GtkXText *xtext = GTK_XTEXT (user_data);
	gtk_xtext_leave_common (xtext);
	gtk_xtext_leave_hover (xtext);
}

/* check if we should mark time stamps, and if a redraw is needed */

static gboolean
gtk_xtext_check_mark_stamp (GtkXText *xtext, GdkModifierType mask)
{
	gboolean redraw = FALSE;

	if ((mask & STATE_SHIFT || prefs.hex_text_autocopy_stamp)
	    && prefs.hex_stamp_text)
	{
		if (!xtext->mark_stamp)
		{
			redraw = TRUE;	/* must redraw all */
			xtext->mark_stamp = TRUE;
		}
	} else
	{
		if (xtext->mark_stamp)
		{
			redraw = TRUE;	/* must redraw all */
			xtext->mark_stamp = FALSE;
		}
	}
	return redraw;
}

static int
gtk_xtext_get_word_adjust (GtkXText *xtext, int x, int y, textentry **word_ent, int *offset, int *len)
{
	GSList *slp = NULL;
	unsigned char *word;
	int word_type = 0;

	word = gtk_xtext_get_word (xtext, x, y, word_ent, offset, len, &slp);
	if (word)
	{
		int laststart, lastend;

		word_type = xtext->urlcheck_function (GTK_WIDGET (xtext), word);
		if (word_type > 0)
		{
			if (url_last (&laststart, &lastend))
			{
				int cumlen, startadj = 0, endadj = 0;
				offlen_t *meta;
				GSList *sl;

				for (sl = slp, cumlen = 0; sl; sl = g_slist_next (sl))
				{
					meta = sl->data;
					startadj = meta->off - cumlen;
					cumlen += meta->len;
					if (laststart < cumlen)
						break;
				}
				for (sl = slp, cumlen = 0; sl; sl = g_slist_next (sl))
				{
					meta = sl->data;
					endadj = meta->off - cumlen;
					cumlen += meta->len;
					if (lastend < cumlen)
						break;
				}
				laststart += startadj;
				*offset += laststart;
				*len = lastend + endadj - laststart;
			}
		}
	}
	g_slist_free_full (slp, g_free);

	return word_type;
}

/*
 * Motion notify event handler
 * GTK3: widget_class vfunc with GdkEventMotion
 * GTK4: GtkEventControllerMotion "motion" signal
 */
static void
gtk_xtext_motion_notify (GtkEventControllerMotion *controller, double event_x, double event_y, gpointer user_data)
{
	GtkXText *xtext = GTK_XTEXT (user_data);
	GtkWidget *widget = GTK_WIDGET (xtext);
	GdkModifierType mask;
	int redraw, tmp, x, y, offset, len, line_x;
	textentry *word_ent;
	int word_type;

	x = (int)event_x;
	y = (int)event_y;
	mask = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (controller));

	if (xtext->moving_separator)
	{
		int widget_width = xtext->buffer->window_width;
		if (x < (3 * widget_width) / 5 && x > 15)
		{
			tmp = xtext->buffer->indent;
			xtext->buffer->indent = x;
			gtk_xtext_fix_indent (xtext->buffer);
			if (tmp != xtext->buffer->indent)
			{
				gtk_xtext_recalc_widths (xtext->buffer, FALSE);
				if (xtext->buffer->scroll_anchor.anchor_to_bottom)
					gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper (xtext->adj) -
													  gtk_adjustment_get_page_size (xtext->adj));
				if (!xtext->io_tag)
					xtext->io_tag = g_timeout_add (REFRESH_TIMEOUT,
															(GSourceFunc)
															gtk_xtext_adjustment_timeout,
															xtext);
			}
		}
		return;
	}

	if (xtext->button_down)
	{
		redraw = gtk_xtext_check_mark_stamp (xtext, mask);
		xtext->select_end_x = x;
		xtext->select_end_y = y;
		gtk_xtext_selection_update (xtext, NULL, y, !redraw);

		/* user has pressed or released SHIFT, must redraw entire selection */
		if (redraw)
		{
			xtext->force_stamp = TRUE;
			gtk_xtext_render_ents (xtext, gtk_xtext_find_by_id (xtext->buffer, xtext->buffer->last_ent_start_id),
										  gtk_xtext_find_by_id (xtext->buffer, xtext->buffer->last_ent_end_id));
			xtext->force_stamp = FALSE;
			gtk_widget_queue_draw (widget);
		}
		return;
	}

	if (xtext->separator && xtext->buffer->indent)
	{
		line_x = xtext->buffer->indent - ((xtext->space_width + 1) / 2);
		if (line_x == x || line_x == x + 1 || line_x == x - 1)
		{
			if (!xtext->cursor_resize)
			{
				gtk_widget_set_cursor (widget, xtext->resize_cursor);
				xtext->cursor_hand = FALSE;
				xtext->cursor_resize = TRUE;
			}
			return;
		}
	}

	/* Track hovered entry for reply button and reply context target */
	{
		textentry *new_hover = NULL;
		textentry *new_reply_target = NULL;
		int hover_off;
		new_hover = gtk_xtext_find_char (xtext, x, y, &hover_off, NULL);

		/* If hovering over a reply context zone, resolve the target entry */
		if (new_hover && new_hover->reply)
		{
			textentry *zone_ent;
			xtext_click_zone zone = gtk_xtext_get_click_zone (xtext, y, &zone_ent);
			if (zone == XTEXT_ZONE_REPLY && zone_ent == new_hover)
			{
				if (new_hover->reply->target_entry_id)
					new_reply_target = gtk_xtext_find_by_id (xtext->buffer,
					                                          new_hover->reply->target_entry_id);
				else if (new_hover->reply->target_msgid)
					new_reply_target = gtk_xtext_find_by_msgid (xtext->buffer,
					                                             new_hover->reply->target_msgid);
			}
		}

		if (new_hover != xtext->hover_ent ||
		    new_reply_target != xtext->hover_reply_target)
		{
			xtext->hover_ent = new_hover;
			xtext->hover_reply_target = new_reply_target;
			gtk_widget_queue_draw (widget);
		}

		/* Hover stamp: check on every motion for area/modifier changes */
		{
			gboolean want_stamp = FALSE;
			gboolean via_alt = FALSE;

			if (new_hover && new_hover->stamp)
			{
				if (xtext->buffer->time_stamp && x < xtext->stamp_width)
					want_stamp = TRUE;
				else if (!xtext->buffer->time_stamp && (mask & GDK_ALT_MASK))
				{
					want_stamp = TRUE;
					via_alt = TRUE;
				}
			}

			if (want_stamp && !xtext->hover_stamp_visible && !xtext->hover_stamp_tag)
			{
				xtext->hover_stamp_alt = via_alt;
				xtext->hover_stamp_tag = g_timeout_add (750,
					xtext_hover_stamp_timeout, xtext);
			}
			else if (!want_stamp && (xtext->hover_stamp_visible || xtext->hover_stamp_tag))
			{
				if (xtext->hover_stamp_tag)
				{
					g_source_remove (xtext->hover_stamp_tag);
					xtext->hover_stamp_tag = 0;
				}
				if (xtext->hover_stamp_visible)
				{
					if (xtext->hover_stamp_alt)
					{
						/* Alt path: linger for 2 seconds before hiding */
						xtext->hover_stamp_tag = g_timeout_add (2000,
							xtext_hover_stamp_linger_timeout, xtext);
					}
					else
					{
						xtext->hover_stamp_visible = FALSE;
						gtk_widget_queue_draw (widget);
					}
				}
				xtext->hover_stamp_alt = FALSE;
			}
		}
	}

	if (xtext->urlcheck_function == NULL)
		return;

	word_type = gtk_xtext_get_word_adjust (xtext, x, y, &word_ent, &offset, &len);
	if (word_type > 0)
	{
		if (!xtext->cursor_hand ||
			 xtext->hilight_ent != word_ent ||
			 xtext->hilight_start != offset ||
			 xtext->hilight_end != offset + len)
		{
			if (!xtext->cursor_hand)
			{
				gtk_widget_set_cursor (widget, xtext->hand_cursor);
				xtext->cursor_hand = TRUE;
				xtext->cursor_resize = FALSE;
			}

			/* un-render the old hilight */
			if (xtext->hilight_ent)
				gtk_xtext_unrender_hilight (xtext);

			xtext->hilight_ent = word_ent;
			xtext->hilight_start = offset;
			xtext->hilight_end = offset + len;

			xtext->skip_border_fills = TRUE;
			xtext->render_hilights_only = TRUE;
			xtext->skip_stamp = TRUE;

			gtk_xtext_render_ents (xtext, word_ent, NULL);

			xtext->skip_border_fills = FALSE;
			xtext->render_hilights_only = FALSE;
			xtext->skip_stamp = FALSE;

			gtk_widget_queue_draw (widget);
		}
		return;
	}

	gtk_xtext_leave_common (xtext);
}

static void
gtk_xtext_set_clip_owner (GtkWidget * xtext, void * event)
{
	char *str;
	int len;

	if (GTK_XTEXT (xtext)->selection_buffer &&
		GTK_XTEXT (xtext)->selection_buffer != GTK_XTEXT (xtext)->buffer)
		gtk_xtext_selection_clear (GTK_XTEXT (xtext)->selection_buffer);

	GTK_XTEXT (xtext)->selection_buffer = GTK_XTEXT (xtext)->buffer;

	str = gtk_xtext_selection_get_text (GTK_XTEXT (xtext), &len);
	if (str)
	{
		if (str[0])
		{
			/* GTK4: Use GdkClipboard API */
			GdkClipboard *clipboard = gtk_widget_get_clipboard (xtext);
			gdk_clipboard_set_text (clipboard, str);
			/* GTK4: PRIMARY selection is handled differently - use primary clipboard */
			GdkClipboard *primary = gtk_widget_get_primary_clipboard (xtext);
			gdk_clipboard_set_text (primary, str);
		}

		g_free (str);
	}
}

void
gtk_xtext_copy_selection (GtkXText *xtext)
{
	gtk_xtext_set_clip_owner (GTK_WIDGET (xtext), NULL);
}

static void
gtk_xtext_unselect (GtkXText *xtext)
{
	xtext_buffer *buf = xtext->buffer;

	xtext->skip_border_fills = TRUE;
	xtext->skip_stamp = TRUE;

	{
		textentry *sel_start = gtk_xtext_find_by_id (buf, buf->last_ent_start_id);
		textentry *sel_end = gtk_xtext_find_by_id (buf, buf->last_ent_end_id);

		if (sel_start)
			xtext->jump_in_offset = sel_start->mark_start;
		/* just a single ent was marked? */
		if (buf->last_ent_start_id == buf->last_ent_end_id)
		{
			if (sel_start)
				xtext->jump_out_offset = sel_start->mark_end;
			buf->last_ent_end_id = 0;
			sel_end = NULL;
		}

		gtk_xtext_selection_clear (xtext->buffer);

		/* FIXME: use jump_out on multi-line selects too! */
		xtext->jump_in_offset = 0;
		xtext->jump_out_offset = 0;
		gtk_xtext_render_ents (xtext, sel_start, sel_end);
	}

	xtext->skip_border_fills = FALSE;
	xtext->skip_stamp = FALSE;

	xtext->buffer->last_ent_start_id = 0;
	xtext->buffer->last_ent_end_id = 0;

	/* Clear selection pins — eviction can resume normally */
	xtext->buffer->sel_pin_start_id = 0;
	xtext->buffer->sel_pin_end_id = 0;

	/* GTK3: Queue redraw to ensure selection is cleared */
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

/*
 * Button release event handler
 * GTK3: widget_class vfunc with GdkEventButton
 * GTK4: GtkGestureClick "released" signal
 */
static void
gtk_xtext_button_release (GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
	GtkXText *xtext = GTK_XTEXT (user_data);
	GtkWidget *widget = GTK_WIDGET (xtext);
	unsigned char *word;
	int old;
	guint button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
	GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));

	if (xtext->moving_separator)
	{
		int widget_width = xtext->buffer->window_width;
		xtext->moving_separator = FALSE;
		old = xtext->buffer->indent;
		if (x < (4 * widget_width) / 5 && x > 15)
			xtext->buffer->indent = (int)x;
		gtk_xtext_fix_indent (xtext->buffer);
		if (xtext->buffer->indent != old)
		{
			gtk_xtext_recalc_widths (xtext->buffer, FALSE);
			gtk_xtext_adjustment_set (xtext->buffer, TRUE);
			gtk_widget_queue_draw (widget);
		} else
			gtk_widget_queue_draw (widget);
		return;
	}

	if (button == 1)
	{
		xtext->button_down = FALSE;
		if (xtext->scroll_tag)
		{
			g_source_remove (xtext->scroll_tag);
			xtext->scroll_tag = 0;
		}

		/* If button_press already handled this click (reply button, reaction
		 * badge, reply context line), skip all release-side processing. */
		if (xtext->press_handled)
		{
			xtext->press_handled = FALSE;
			return;
		}

		/* got a new selection? */
		if (xtext->buffer->last_ent_start_id)
		{
			xtext->color_paste = FALSE;
			if (state & STATE_CTRL || prefs.hex_text_autocopy_color)
				xtext->color_paste = TRUE;
			if (prefs.hex_text_autocopy_text)
			{
				gtk_xtext_set_clip_owner (widget, NULL);
			}
		}

		if (xtext->word_select || xtext->line_select)
		{
			xtext->word_select = FALSE;
			xtext->line_select = FALSE;
			return;
		}

		if (xtext->select_start_x == (int)x &&
			 xtext->select_start_y == (int)y &&
			 xtext->buffer->last_ent_start_id)
		{
			gtk_xtext_unselect (xtext);
			xtext->mark_stamp = FALSE;
			return;
		}

		if (!gtk_xtext_is_selecting (xtext))
		{
			word = gtk_xtext_get_word (xtext, (int)x, (int)y, 0, 0, 0, 0);
			/* GTK4: Store click info before emitting signal since event will be NULL */
			xtext->last_click_button = button;
			xtext->last_click_state = state;
			xtext->last_click_n_press = n_press;
			xtext->last_click_x = (int)x;
			xtext->last_click_y = (int)y;
			g_signal_emit (G_OBJECT (xtext), xtext_signals[WORD_CLICK], 0, word ? word : NULL, NULL);
		}
	}
}

/* Determine which zone of an entry a y-coordinate hits.
 * Returns: 0 = reply context line, 1 = normal text, 2 = reaction badges,
 * 3 = day separator, 4 = collapse/expand indicator.
 * Also sets *ent_out to the entry found. */
static xtext_click_zone
gtk_xtext_get_click_zone (GtkXText *xtext, int y, textentry **ent_out)
{
	textentry *ent;
	int subline, text_sublines, visible_text;

	if (ent_out)
		*ent_out = NULL;

	/* Resolve (ent, subline) via the same pagetop walk find_char_ex
	 * uses — gtk_xtext_nth via adj->value can disagree with what's
	 * actually rendered when extra_lines_above/below or a collapse
	 * indicator are in play, and we'd misroute clicks because of it. */
	ent = gtk_xtext_find_char_ex (xtext, 0, y, NULL, NULL, &subline);
	if (ent_out)
		*ent_out = ent;

	if (!ent)
		return XTEXT_ZONE_TEXT;

	/* Subline layout for a rendered entry, in order:
	 *   [extra_lines_above]              day_sep + reply context
	 *   [visible_text]                    text wraps (clamped to preview when collapsed)
	 *   [collapse_indicator: 0 or 1]      shown when collapsible
	 *   [extra_lines_below: 0 or 1]       reaction badge row
	 *
	 * sum == ent->display_lines per ent_update_display_lines. */
	if (subline < ent->extra_lines_above)
	{
		gboolean has_day_sep = (ent->flags & TEXTENTRY_FLAG_DAY_BOUNDARY)
		                       && prefs.hex_gui_day_separator;
		if (has_day_sep && subline == 0)
			return XTEXT_ZONE_DAY_SEP;
		return XTEXT_ZONE_REPLY;
	}

	text_sublines = g_slist_length (ent->sublines);
	visible_text = ent->collapsed
		? MIN (COLLAPSE_PREVIEW_LINES, text_sublines)
		: text_sublines;

	if (ent->collapsible)
	{
		int indicator_subline = ent->extra_lines_above + visible_text;
		if (subline == indicator_subline)
			return XTEXT_ZONE_COLLAPSE;
	}

	/* Reaction badge sits after the visible text and (if present) the
	 * collapse indicator.  Use the rendered layout's bounds, not the
	 * full text wrap count — for collapsed entries those differ. */
	{
		int badge_start = ent->extra_lines_above + visible_text
			+ (ent->collapsible ? 1 : 0);
		if (subline >= badge_start)
			return XTEXT_ZONE_REACT;
	}

	return XTEXT_ZONE_TEXT;
}

/* Flash highlight timeout — clear after brief display */
static gboolean
gtk_xtext_flash_timeout (gpointer data)
{
	GtkXText *xtext = GTK_XTEXT (data);
	xtext->flash_ent = NULL;
	xtext->flash_tag = 0;
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	return G_SOURCE_REMOVE;
}

/* Handle click on a redacted message entry.
 * Cycles: REDACTED → REDACTED_PROMPT → REDACTED_REVEALED → REDACTED */
static void
gtk_xtext_redaction_click (GtkXText *xtext, textentry *ent)
{
	xtext_redaction_info *ri = ent->redaction;
	xtext_buffer *buf = xtext->buffer;

	if (!ri)
		return;

	switch ((xtext_entry_state)ent->state)
	{
	case XTEXT_STATE_REDACTED:
		{
			/* Show "click again to reveal" prompt */
			int left_len = ent->left_len;
			char *prompt = g_strdup ("\017[Click again to reveal original message]");
			int plen = strlen (prompt);

			if (left_len >= 0)
			{
				int new_len = left_len + 1 + plen;
				unsigned char *new_str = g_malloc (new_len + 1);
				memcpy (new_str, ent->str, left_len + 1);
				memcpy (new_str + left_len + 1, prompt, plen);
				new_str[new_len] = '\0';
				gtk_xtext_entry_set_text (buf, ent, new_str, new_len);
				g_free (new_str);
			}
			else
			{
				gtk_xtext_entry_set_text (buf, ent,
				                          (const unsigned char *)prompt, plen);
			}

			g_free (prompt);
			ent->state = XTEXT_STATE_REDACTED_PROMPT;
			ri->prompt_shown_time = g_get_monotonic_time ();
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		}
		break;

	case XTEXT_STATE_REDACTED_PROMPT:
		{
			/* Must wait at least 1 second before reveal */
			gint64 elapsed = g_get_monotonic_time () - ri->prompt_shown_time;
			if (elapsed < G_USEC_PER_SEC)
				break;

			/* Reveal the original content */
			gtk_xtext_entry_set_text (buf, ent,
			                          (const unsigned char *)ri->original_content,
			                          ri->original_len);
			ent->state = XTEXT_STATE_REDACTED_REVEALED;
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		}
		break;

	case XTEXT_STATE_REDACTED_REVEALED:
		{
			/* Re-redact: show deletion placeholder again */
			int left_len = ent->left_len;
			char *placeholder;

			if (ri->redaction_reason && *ri->redaction_reason)
				placeholder = g_strdup_printf ("\017[Message deleted by %s: %s]",
				                               ri->redacted_by, ri->redaction_reason);
			else
				placeholder = g_strdup_printf ("\017[Message deleted by %s]",
				                               ri->redacted_by);

			if (left_len >= 0)
			{
				const unsigned char *orig = (const unsigned char *)ri->original_content;
				int plen = strlen (placeholder);
				int new_len = left_len + 1 + plen;
				unsigned char *new_str = g_malloc (new_len + 1);
				memcpy (new_str, orig, left_len + 1);
				memcpy (new_str + left_len + 1, placeholder, plen);
				new_str[new_len] = '\0';
				gtk_xtext_entry_set_text (buf, ent, new_str, new_len);
				g_free (new_str);
			}
			else
			{
				gtk_xtext_entry_set_text (buf, ent,
				                          (const unsigned char *)placeholder,
				                          strlen (placeholder));
			}

			g_free (placeholder);
			ent->state = XTEXT_STATE_REDACTED;
			ri->prompt_shown_time = 0;
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		}
		break;

	default:
		break;
	}
}

static void
gtk_xtext_click_reply_context (GtkXText *xtext, textentry *ent)
{
	textentry *target;

	if (!ent || !ent->reply)
		return;

	/* Try to find the target by entry_id first, then by msgid */
	if (ent->reply->target_entry_id)
		target = gtk_xtext_find_by_id (xtext->buffer, ent->reply->target_entry_id);
	else if (ent->reply->target_msgid)
		target = gtk_xtext_find_by_msgid (xtext->buffer, ent->reply->target_msgid);
	else
		return;

	/* Virtual scrollback: if target is evicted, try to materialize it from DB */
	if (!target && HAS_VIRT_DB (xtext->buffer) &&
	    ent->reply->target_msgid && xtext->buffer->virt_db)
	{
		scrollback_db *db = (scrollback_db *) xtext->buffer->virt_db;
		gint64 rowid = scrollback_get_rowid_by_msgid (db,
			xtext->buffer->virt_channel, ent->reply->target_msgid);
		if (rowid > 0)
		{
			int idx = scrollback_get_index_of_rowid (db,
				xtext->buffer->virt_channel, rowid);
			gtk_xtext_virt_ensure_range (xtext->buffer, idx, VIRT_PAGE_SIZE);
			/* Retry lookup after materialization */
			target = gtk_xtext_find_by_msgid (xtext->buffer,
				ent->reply->target_msgid);
		}
	}

	if (!target)
		return;

	/* Scroll to target entry and flash-highlight it */
	gtk_xtext_scroll_to_entry (xtext->buffer, target);

	if (xtext->flash_tag)
		g_source_remove (xtext->flash_tag);
	xtext->flash_ent = target;
	xtext->flash_tag = g_timeout_add (1500, gtk_xtext_flash_timeout, xtext);
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

/* Handle a click on a reaction badge: determine which badge was hit by x-coord.
 * If self-reaction: send unreact. If not: send react with same text. */
static void
gtk_xtext_click_reaction_badge (GtkXText *xtext, textentry *ent, int click_x)
{
	struct xtext_reactions_info *ri;
	int badge_x, pad_x;
	guint i;

	if (!ent || !ent->reactions)
		return;

	ri = ent->reactions;
	pad_x = 6;

	/* Right-align: compute starting x from total badge width */
	{
		int win_width = xtext->buffer->window_width - MARGIN;
		int total_width = gtk_xtext_measure_reaction_badges (xtext, ri, win_width);
		badge_x = win_width - total_width;
	}

	for (i = 0; i < ri->reactions->len; i++)
	{
		struct xtext_reaction *react = g_ptr_array_index (ri->reactions, i);
		PangoRectangle logical;
		int badge_width;
		char *label;

		if (react->count <= 0)
			continue;

		if (react->count > 1)
			label = g_strdup_printf ("%s %d", react->text, react->count);
		else
			label = g_strdup (react->text);

		pango_layout_set_text (xtext->layout, label, -1);
		pango_layout_set_attributes (xtext->layout, NULL);
		pango_layout_get_extents (xtext->layout, NULL, &logical);
		badge_width = PANGO_PIXELS (logical.width) + pad_x * 2;
		g_free (label);

		if (click_x >= badge_x && click_x < badge_x + badge_width)
		{
			/* Found the clicked badge — store info and invoke callback */
			const char *msgid = ent->msgid;
			if (msgid)
			{
				g_free (xtext->reaction_click_msgid);
				xtext->reaction_click_msgid = g_strdup (msgid);
				g_free (xtext->reaction_click_text);
				xtext->reaction_click_text = g_strdup (react->text);
				xtext->reaction_click_is_self = gtk_xtext_entry_has_self_reaction (ent, react->text);

				if (xtext->reaction_click_cb)
					xtext->reaction_click_cb (xtext, msgid, react->text,
					                          xtext->reaction_click_is_self,
					                          xtext->reaction_click_userdata);
			}
			return;
		}

		badge_x += badge_width + 4;
	}
}

/*
 * Button press event handler
 * GTK3: widget_class vfunc with GdkEventButton
 * GTK4: GtkGestureClick "pressed" signal
 */
static void
gtk_xtext_button_press (GtkGestureClick *gesture, int n_press, double event_x, double event_y, gpointer user_data)
{
	GtkXText *xtext = GTK_XTEXT (user_data);
	GdkModifierType mask;
	textentry *ent;
	unsigned char *word;
	int line_x, x, y, offset, len;
	guint button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));

	x = (int)event_x;
	y = (int)event_y;
	mask = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
	xtext->press_handled = FALSE;

	if (button == 3 || button == 2) /* right/middle click */
	{
		word = gtk_xtext_get_word (xtext, x, y, 0, 0, 0, 0);
		/* GTK4: Store click info before emitting signal since event will be NULL */
		xtext->last_click_button = button;
		xtext->last_click_state = mask;
		xtext->last_click_n_press = n_press;
		xtext->last_click_x = x;
		xtext->last_click_y = y;
		if (word)
		{
			g_signal_emit (G_OBJECT (xtext), xtext_signals[WORD_CLICK], 0,
								word, NULL);
		} else
			g_signal_emit (G_OBJECT (xtext), xtext_signals[WORD_CLICK], 0,
								"", NULL);
		return;
	}

	if (button != 1)		  /* we only want left button */
		return;

	/* Check if click lands on status strip dismiss button */
	if (n_press == 1 && xtext->status_strip_visible)
	{
		int height = gtk_widget_get_height (GTK_WIDGET (xtext));
		int strip_h = xtext->fontsize * 2 / 3 + 4;
		int strip_y = height - strip_h;

		if (y >= strip_y)
		{
			int si;
			for (si = 0; si < xtext->status_item_count; si++)
			{
				xtext_status_item *item = &xtext->status_items[si];
				if (item->dismiss_cb && item->dismiss_w > 0 &&
				    x >= item->dismiss_x && x < item->dismiss_x + item->dismiss_w)
				{
					item->dismiss_cb (xtext, item->key, item->dismiss_userdata);
					xtext->press_handled = TRUE;
					return;
				}
			}
			/* Click was in strip but not on dismiss — ignore */
			xtext->press_handled = TRUE;
			return;
		}
	}

	/* Picker mode: clicking any message grabs its msgid */
	if (n_press == 1 && xtext->picker_click_cb)
	{
		ent = gtk_xtext_find_char (xtext, x, y, NULL, NULL);
		if (ent)
		{
			const char *msgid = ent->msgid;
			xtext->picker_click_cb (xtext, msgid, xtext->picker_click_userdata);
			xtext->press_handled = TRUE;
			return;
		}
	}

	/* Check if click lands on any hover button (reply, react-text, react-emoji) */
	if (n_press == 1 && xtext->hover_ent && xtext->hover_ent->msgid &&
	    xtext->hover_btn_size > 0 &&
	    y >= xtext->hover_btn_y - 4 && y < xtext->hover_btn_y + xtext->hover_btn_size + 4)
	{
		typedef void (*hover_btn_cb) (GtkXText *, const char *, const char *, gpointer);
		hover_btn_cb cb = NULL;
		gpointer ud = NULL;
		int bs = xtext->hover_btn_size;

		if (x >= xtext->reply_btn_x - 4 && x < xtext->reply_btn_x + bs + 4)
		{
			cb = xtext->reply_button_cb;
			ud = xtext->reply_button_userdata;
		}
		else if (x >= xtext->react_text_btn_x - 4 && x < xtext->react_text_btn_x + bs + 4)
		{
			cb = xtext->react_text_button_cb;
			ud = xtext->react_text_button_userdata;
		}
		else if (x >= xtext->react_emoji_btn_x - 4 && x < xtext->react_emoji_btn_x + bs + 4)
		{
			cb = xtext->react_emoji_button_cb;
			ud = xtext->react_emoji_button_userdata;
		}
		else if (xtext->redact_btn_x >= 0 &&
		         x >= xtext->redact_btn_x - 4 && x < xtext->redact_btn_x + bs + 4)
		{
			cb = xtext->redact_button_cb;
			ud = xtext->redact_button_userdata;
		}

		if (cb)
		{
			textentry *ent = xtext->hover_ent;
			const unsigned char *str = gtk_xtext_entry_get_str (ent);
			int left_len = gtk_xtext_entry_get_left_len (ent);
			char *nick_raw = g_strndup ((const char *)str, left_len);
			char *nick = strip_color (nick_raw, -1, STRIP_ALL);
			g_free (nick_raw);
			/* Trim surrounding whitespace/brackets from nick (e.g. "<nick>") */
			g_strstrip (nick);
			{
				char *p = nick;
				int len = strlen (p);
				if (len > 2 && p[0] == '<' && p[len - 1] == '>')
				{
					memmove (p, p + 1, len - 2);
					p[len - 2] = '\0';
				}
			}
			cb (xtext, ent->msgid, nick, ud);
			g_free (nick);
			xtext->press_handled = TRUE;
			return;
		}
	}

	/* Check if click lands on reply context, reaction badges, or collapse indicator */
	if (n_press == 1)
	{
		textentry *zone_ent;
		xtext_click_zone zone = gtk_xtext_get_click_zone (xtext, y, &zone_ent);

		if (zone == XTEXT_ZONE_COLLAPSE && zone_ent)
		{
			xtext_scroll_anchor anchor;
			gboolean was_down = xtext->buffer->scroll_anchor.anchor_to_bottom;

			gtk_xtext_save_scroll_anchor (xtext->buffer, &anchor);
			{
				int old_dl = zone_ent->display_lines;
				zone_ent->collapsed = !zone_ent->collapsed;
				ent_update_display_lines (zone_ent);
				if (xtext->buffer->entry_tree && zone_ent->display_lines != old_dl)
					update_weight234 (xtext->buffer->entry_tree, zone_ent,
					                  zone_ent->display_lines - old_dl);
			}
			gtk_xtext_calc_lines (xtext->buffer, FALSE);
			gtk_xtext_restore_scroll_anchor (xtext->buffer, &anchor);
			if (was_down)
			{
				xtext->buffer->scroll_anchor.anchor_to_bottom = TRUE;
			}
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
			xtext->press_handled = TRUE;
			return;
		}
		if (zone == XTEXT_ZONE_REPLY && zone_ent)
		{
			gtk_xtext_click_reply_context (xtext, zone_ent);
			xtext->press_handled = TRUE;
			return;
		}
		if (zone == XTEXT_ZONE_REACT && zone_ent)
		{
			gtk_xtext_click_reaction_badge (xtext, zone_ent, x);
			xtext->press_handled = TRUE;
			return;
		}
	}

	/* Click on redacted message: cycle through redacted → prompt → reveal → redacted */
	if (n_press == 1)
	{
		textentry *click_ent = gtk_xtext_find_char (xtext, x, y, NULL, NULL);
		if (click_ent && click_ent->redaction)
		{
			gtk_xtext_redaction_click (xtext, click_ent);
			xtext->press_handled = TRUE;
			return;
		}
	}

	if (n_press == 2)	/* WORD select (double click) */
	{
		gtk_xtext_check_mark_stamp (xtext, mask);
		if (gtk_xtext_get_word (xtext, x, y, &ent, &offset, &len, 0))
		{
			if (len == 0)
				return;
			gtk_xtext_selection_clear (xtext->buffer);
			ent->mark_start = offset;
			ent->mark_end = offset + len;
			gtk_xtext_selection_render (xtext, ent, ent);
			xtext->word_select = TRUE;
		}

		return;
	}

	if (n_press == 3)	/* LINE select (triple click) */
	{
		gtk_xtext_check_mark_stamp (xtext, mask);
		if (gtk_xtext_get_word (xtext, x, y, &ent, 0, 0, 0))
		{
			gtk_xtext_selection_clear (xtext->buffer);
			ent->mark_start = 0;
			ent->mark_end = ent->str_len;
			gtk_xtext_selection_render (xtext, ent, ent);
			xtext->line_select = TRUE;
		}

		return;
	}

	/* check if it was a separator-bar click */
	if (xtext->separator && xtext->buffer->indent)
	{
		line_x = xtext->buffer->indent - ((xtext->space_width + 1) / 2);
		if (line_x == x || line_x == x + 1 || line_x == x - 1)
		{
			xtext->moving_separator = TRUE;
			/* draw the separator line */
			gtk_xtext_draw_sep (xtext, -1);
			return;
		}
	}

	xtext->button_down = TRUE;
	xtext->select_start_x = x;
	xtext->select_start_y = y;
	/* Initialize select_end to same position to avoid stale values */
	xtext->select_end_x = x;
	xtext->select_end_y = y;

	/* Pin the clicked entry so virtual scrollback doesn't evict it, and
	 * capture (entry_id, subline) so select_start_y can be re-derived
	 * after scrolls and buffer mutations. */
	{
		int sub = 0;
		textentry *ent =
			gtk_xtext_find_char_ex (xtext, x, y, NULL, NULL, &sub);
		if (ent)
		{
			xtext->buffer->sel_pin_start_id = ent->entry_id;
			xtext->select_start_entry_id = ent->entry_id;
			xtext->select_start_subline = sub;
		}
		else
		{
			xtext->select_start_entry_id = 0;
			xtext->select_start_subline = 0;
		}
	}
}

static gboolean
gtk_xtext_is_selecting (GtkXText *xtext)
{
	textentry *ent;
	xtext_buffer *buf;

	buf = xtext->selection_buffer;
	if (!buf)
		return FALSE;

	for (ent = gtk_xtext_find_by_id (buf, buf->last_ent_start_id); ent; ent = ent->next)
	{
		if (ent->mark_start != -1 && ent->mark_end - ent->mark_start > 0)
			return TRUE;

		if (ent->entry_id == buf->last_ent_end_id)
			break;
	}

	return FALSE;
}

static char *
gtk_xtext_selection_get_text (GtkXText *xtext, int *len_ret)
{
	textentry *ent;
	char *txt;
	char *pos;
	char *stripped;
	int len;
	int first = TRUE;
	xtext_buffer *buf;

	buf = xtext->selection_buffer;
	if (!buf)
		return NULL;

	/* first find out how much we need to malloc ... */
	len = 0;
	ent = gtk_xtext_find_by_id (buf, buf->last_ent_start_id);
	while (ent)
	{
		if (ent->mark_start != -1)
		{
			/* include timestamp? */
			if (ent->mark_start == 0 && xtext->mark_stamp)
			{
				char *time_str;
				int stamp_size = xtext_get_stamp_str (ent->stamp, &time_str);
				g_free (time_str);
				len += stamp_size;
			}

			if (ent->mark_end - ent->mark_start > 0)
				len += (ent->mark_end - ent->mark_start) + 1;
			else
				len++;
		}
		if (ent->entry_id == buf->last_ent_end_id)
			break;
		ent = ent->next;
	}

	if (len < 1)
		return NULL;

	/* now allocate mem and copy buffer */
	pos = txt = g_malloc (len);
	ent = gtk_xtext_find_by_id (buf, buf->last_ent_start_id);
	while (ent)
	{
		if (ent->mark_start != -1)
		{
			if (!first)
			{
				*pos = '\n';
				pos++;
			}
			first = FALSE;
			if (ent->mark_end - ent->mark_start > 0)
			{
				/* include timestamp? */
				if (ent->mark_start == 0 && xtext->mark_stamp)
				{
					char *time_str;
					int stamp_size = xtext_get_stamp_str (ent->stamp, &time_str);
					memcpy (pos, time_str, stamp_size);
					g_free (time_str);
					pos += stamp_size;
				}

				memcpy (pos, ent->str + ent->mark_start,
						  ent->mark_end - ent->mark_start);
				pos += ent->mark_end - ent->mark_start;
			}
		}
		if (ent->entry_id == buf->last_ent_end_id)
			break;
		ent = ent->next;
	}
	*pos = 0;

	if (xtext->color_paste)
	{
		/*stripped = gtk_xtext_conv_color (txt, strlen (txt), &len);*/
		stripped = txt;
		len = strlen (txt);
	}
	else
	{
		stripped = gtk_xtext_strip_color (txt, strlen (txt), NULL, &len, NULL, FALSE);
		g_free (txt);
	}

	*len_ret = len;
	return stripped;
}


/*
 * Scroll event handler
 * GTK3: widget_class vfunc with GdkEventScroll
 * GTK4: GtkEventControllerScroll "scroll" signal
 */
static gboolean
gtk_xtext_scroll (GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data)
{
	GtkXText *xtext = GTK_XTEXT (user_data);
	gfloat new_value;
	gdouble adj_value = gtk_adjustment_get_value (xtext->adj);
	gdouble adj_upper = gtk_adjustment_get_upper (xtext->adj);
	gdouble adj_lower = gtk_adjustment_get_lower (xtext->adj);
	gdouble adj_page_size = gtk_adjustment_get_page_size (xtext->adj);

	/* GTK4 DISCRETE mode: dy is 1.0 per wheel notch.  Lines per notch is
	 * user-configurable via /SET gui_scroll_lines (default 3). */
	if (dy != 0)
	{
		int scroll_lines = prefs.hex_gui_scroll_lines;
		if (scroll_lines < 1)
			scroll_lines = 1;
		new_value = adj_value + dy * scroll_lines;
		if (new_value < adj_lower)
			new_value = adj_lower;
		if (new_value > (adj_upper - adj_page_size))
			new_value = adj_upper - adj_page_size;
		/* Skip the set_value call entirely when the clamped value
		 * doesn't differ from the current one — free-spinning the
		 * wheel into the bottom edge can produce hundreds of events
		 * per second, all clamped to the same value, all otherwise
		 * walking the value-changed signal path for nothing. */
		if (new_value != adj_value)
			gtk_adjustment_set_value (xtext->adj, new_value);
	}

	return TRUE; /* Stop propagation */
}

static void
gtk_xtext_scroll_adjustments (GtkXText *xtext, GtkAdjustment *hadj, GtkAdjustment *vadj)
{
	/* hadj is ignored entirely */

	if (vadj)
		g_return_if_fail (GTK_IS_ADJUSTMENT (vadj));
	else
		vadj = GTK_ADJUSTMENT(gtk_adjustment_new (0, 0, 1, 1, 1, 1));

	if (xtext->adj && (xtext->adj != vadj))
	{
		g_signal_handlers_disconnect_by_func (xtext->adj,
								gtk_xtext_adjustment_changed,
								xtext);
		g_object_unref (xtext->adj);
	}

	if (xtext->adj != vadj)
	{
		xtext->adj = vadj;
		g_object_ref_sink (xtext->adj);

		xtext->vc_signal_tag = g_signal_connect (xtext->adj, "value-changed",
							G_CALLBACK (gtk_xtext_adjustment_changed),
							xtext);

		gtk_xtext_adjustment_changed (xtext->adj, xtext);
	}
}

static void gtk_xtext_snapshot (GtkWidget *widget, GtkSnapshot *snapshot);

static void
gtk_xtext_class_init (GtkXTextClass * class)
{
	GObjectClass *gobject_class;
	GtkWidgetClass *widget_class;
	GtkXTextClass *xtext_class;

	gobject_class = (GObjectClass *) class;
	widget_class = (GtkWidgetClass *) class;
	xtext_class = (GtkXTextClass *) class;

	parent_class = g_type_class_peek (gtk_widget_get_type ());

	xtext_signals[WORD_CLICK] =
		g_signal_new ("word_click",
							G_TYPE_FROM_CLASS (gobject_class),
							G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION,
							G_STRUCT_OFFSET (GtkXTextClass, word_click),
							NULL, NULL,
							_hexchat_marshal_VOID__POINTER_POINTER,
							G_TYPE_NONE,
							2, G_TYPE_POINTER, G_TYPE_POINTER);
	xtext_signals[SET_SCROLL_ADJUSTMENTS] =
		g_signal_new ("set_scroll_adjustments",
							G_OBJECT_CLASS_TYPE (gobject_class),
							G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
							G_STRUCT_OFFSET (GtkXTextClass, set_scroll_adjustments),
							NULL, NULL,
							_hexchat_marshal_VOID__OBJECT_OBJECT,
							G_TYPE_NONE,
							2, GTK_TYPE_ADJUSTMENT, GTK_TYPE_ADJUSTMENT);

	gobject_class->dispose = gtk_xtext_dispose;

	widget_class->realize = gtk_xtext_realize;
	/* GTK4: Use measure vfunc instead of get_preferred_width/height */
	/* GTK4: Event handling is done via controllers in init, not vfuncs */
	widget_class->measure = gtk_xtext_measure;
	widget_class->snapshot = gtk_xtext_snapshot;
	widget_class->size_allocate = gtk_xtext_size_allocate;

	xtext_class->word_click = NULL;
	xtext_class->set_scroll_adjustments = gtk_xtext_scroll_adjustments;
}

GType
gtk_xtext_get_type (void)
{
	static GType xtext_type = 0;

	if (!xtext_type)
	{
		static const GTypeInfo xtext_info =
		{
			sizeof (GtkXTextClass),
			NULL,		/* base_init */
			NULL,		/* base_finalize */
			(GClassInitFunc) gtk_xtext_class_init,
			NULL,		/* class_finalize */
			NULL,		/* class_data */
			sizeof (GtkXText),
			0,		/* n_preallocs */
			(GInstanceInitFunc) gtk_xtext_init,
		};

		xtext_type = g_type_register_static (GTK_TYPE_WIDGET, "GtkXText",
														 &xtext_info, 0);
	}

	return xtext_type;
}

/* strip MIRC colors and other attribs. */

/* CL: needs to strip hidden when called by gtk_xtext_text_width, but not when copying text */

typedef struct chunk_s {
	GSList *slp;
	int off1, len1, emph;
	offlen_t meta;
} chunk_t;

static void
xtext_do_chunk(chunk_t *c)
{
	offlen_t *meta;

	if (c->len1 == 0)
		return;

	meta = g_new (offlen_t, 1);
	meta->off = c->off1;
	meta->len = c->len1;
	meta->emph = c->emph;
	meta->width = 0;
	c->slp = g_slist_append (c->slp, meta);

	c->len1 = 0;
}

static unsigned char *
gtk_xtext_strip_color (unsigned char *text, int len, unsigned char *outbuf,
							  int *newlen, GSList **slpp, int strip_hidden)
{
	chunk_t c;
	int i = 0;
	int rcol = 0, bgcol = 0;
	int hidden = FALSE;
	unsigned char *new_str;
	unsigned char *text0 = text;
	int mbl;	/* multi-byte length */

	if (outbuf == NULL)
		new_str = g_malloc (len + 2);
	else
		new_str = outbuf;

	c.slp = NULL;
	c.off1 = 0;
	c.len1 = 0;
	c.emph = 0;
	while (len > 0)
	{
		mbl = charlen (text);
		if (mbl > len)
			goto bad_utf8;

		if (rcol > 0 && (isdigit (*text) || (*text == ',' && isdigit (text[1]) && !bgcol)))
		{
			if (text[1] != ',') rcol--;
			if (*text == ',')
			{
				rcol = 2;
				bgcol = 1;
			}
		} else
		{
			rcol = bgcol = 0;
			switch (*text)
			{
			case ATTR_COLOR:
				xtext_do_chunk (&c);
				rcol = 2;
				break;
			case ATTR_BEEP:
			case ATTR_RESET:
			case ATTR_REVERSE:
			case ATTR_BOLD:
			case ATTR_UNDERLINE:
			case ATTR_STRIKETHROUGH:
			case ATTR_ITALICS:
				xtext_do_chunk (&c);
				if (*text == ATTR_RESET)
					c.emph = 0;
				if (*text == ATTR_ITALICS)
					c.emph ^= EMPH_ITAL;
				if (*text == ATTR_BOLD)
					c.emph ^= EMPH_BOLD;
				break;
			case ATTR_HIDDEN:
				xtext_do_chunk (&c);
				c.emph ^= EMPH_HIDDEN;
				hidden = !hidden;
				break;
			default:
				if (strip_hidden == 2 || (!(hidden && strip_hidden)))
				{
					if (c.len1 == 0)
						c.off1 = text - text0;
					memcpy (new_str + i, text, mbl);
					i += mbl;
					c.len1 += mbl;
				}
			}
		}
		text += mbl;
		len -= mbl;
	}

bad_utf8:		/* Normal ending sequence, and give up if bad utf8 */
	xtext_do_chunk (&c);

	new_str[i] = 0;

	if (newlen != NULL)
		*newlen = i;

	if (slpp)
		*slpp = c.slp;
	else
		g_slist_free_full (c.slp, g_free);

	return new_str;
}

/* xtext_parse_formats, xtext_stripped_to_raw, xtext_raw_to_stripped,
 * and xtext_build_attrlist have been moved to xtext-render.c */

/* (Implementations moved to xtext-render.c) */

/* gives width of a string, excluding the mIRC codes */

static int
gtk_xtext_text_width_ent (GtkXText *xtext, textentry *ent)
{
	unsigned char *new_buf;
	GSList *slp0, *slp;
	int width;

	if (ent->slp)
	{
		g_slist_free_full (ent->slp, g_free);
		ent->slp = NULL;
	}

	new_buf = gtk_xtext_strip_color (ent->str, ent->str_len, xtext->scratch_buffer,
												NULL, &slp0, 2);

	width =  backend_get_text_width_slp (xtext, new_buf, slp0);
	ent->slp = slp0;

	for (slp = slp0; slp; slp = g_slist_next (slp))
	{
		offlen_t *meta;

		meta = slp->data;
		meta->width = backend_get_text_width_emph (xtext, ent->str + meta->off, meta->len, meta->emph);
	}
	return width;
}

static int
gtk_xtext_text_width (GtkXText *xtext, unsigned char *text, int len)
{
	unsigned char *new_buf;
	int new_len;
	GSList *slp;
	int width;

	new_buf = gtk_xtext_strip_color (text, len, xtext->scratch_buffer,
												&new_len, &slp, !xtext->ignore_hidden);

	width =  backend_get_text_width_slp (xtext, new_buf, slp);
	g_slist_free_full (slp, g_free);

	return width;
}

static void
gtk_xtext_reset (GtkXText * xtext, int mark)
{
	if (!mark)
	{
		if (xtext->col_fore != XTEXT_FG)
			xtext_set_fg (xtext, XTEXT_FG);
		if (xtext->col_back != XTEXT_BG)
			xtext_set_bg (xtext, XTEXT_BG);
		xtext->col_fore = XTEXT_FG;
		xtext->col_back = XTEXT_BG;
	}
}

/* render a single line, which WONT wrap, and parse mIRC colors */

/* --- New subline renderer ---
 *
 * Renders a single subline using one PangoLayout with a pre-computed
 * PangoAttrList from the entry's format spans. Replaces
 * the old character-by-character renderer (removed).
 *
 * Returns: 1 normally, 0 if xtext->cr is NULL (GTK4 outside snapshot)
 */
static int
gtk_xtext_render_subline (GtkXText *xtext, int y, textentry *ent,
                          int raw_offset, int raw_len,
                          int win_width, int indent, int line,
                          int left_only, int *x_size_ret,
                          PangoLayoutLine *cached_pline,
                          int cached_text_offset)
{
	int ret = 1;
	int x;
	PangoLayoutLine *pango_line;
	PangoRectangle logical;

	/* Entry not yet parsed — fall back to old renderer */
	if (!ent->stripped_str || !ent->raw_to_stripped_map)
		return 1;

	/* Convert raw subline boundaries to stripped offsets */
	int sub_start = xtext_raw_to_stripped (ent->raw_to_stripped_map,
	                                       ent->str_len, raw_offset);
	int sub_end = xtext_raw_to_stripped (ent->raw_to_stripped_map,
	                                     ent->str_len, raw_offset + raw_len);
	int sub_len = sub_end - sub_start;

	if (sub_len <= 0)
		return 1;

	if (xtext->cr == NULL)
		return 0;

	if (xtext->dont_render)
		return 1;

	/* --- Background fills (left margin) --- */
	if (!xtext->skip_border_fills)
	{
		gboolean has_stamp = xtext->buffer->time_stamp ||
			(xtext->hover_stamp_visible && xtext->hover_ent != NULL);

		if (raw_offset == 0 && indent > MARGIN && has_stamp)
		{
			/* Don't overwrite the timestamp / hover stamp */
			int stamp_end = xtext->stamp_width;
			if (indent > stamp_end)
				xtext_draw_bg (xtext, stamp_end, y - xtext->font->ascent,
				               indent - stamp_end, xtext->fontsize);
		}
		else
		{
			if (indent >= xtext->clip_x)
				xtext_draw_bg (xtext, 0, y - xtext->font->ascent,
				               MIN (indent, xtext->clip_x2), xtext->fontsize);
		}
	}

	/* --- Get PangoLayoutLine and text width --- */
	PangoAttrList *attrs = NULL;
	int text_width;
	gboolean using_cached = (cached_pline != NULL);
	/* For index_to_x on cached layout lines, indices are relative to the
	 * full layout text start (layout_text_offset), not sub_start.
	 * idx_base is the stripped offset that maps to index 0 in the layout. */
	int idx_base;

	if (cached_pline)
	{
		/* Cache hit: use pre-shaped line from per-entry PangoLayout.
		 * No set_text/set_attributes/reshape needed. */
		pango_line = cached_pline;
		pango_layout_line_get_extents (pango_line, NULL, &logical);
		text_width = PANGO_PIXELS (logical.width);
		idx_base = cached_text_offset;
	}
	else
	{
		/* Cache miss: build layout on the shared xtext->layout */
		xtext_format_data fd = xtext_fdata_from_entry (ent);
		attrs = xtext_build_attrlist (&fd, sub_start, sub_len,
		                               xtext->palette,
		                               xtext->fontsize,
		                               xtext->font->ascent);

		pango_layout_set_text (xtext->layout,
		                        (char *)(ent->stripped_str + sub_start), sub_len);
		pango_layout_set_attributes (xtext->layout, attrs);

		idx_base = sub_start;

		pango_layout_get_extents (xtext->layout, NULL, &logical);
		text_width = PANGO_PIXELS (logical.width);
		pango_line = pango_layout_get_lines_readonly (xtext->layout)->data;
	}

	/* --- Draw background --- */
	x = indent;
	if (!xtext->dont_render2)
	{
		/* Default background */
		xtext_set_source_color (xtext, XTEXT_BG);
		cairo_rectangle (xtext->cr, x, y - xtext->font->ascent,
		                 text_width, xtext->fontsize);
		cairo_fill (xtext->cr);

		/* Draw background image if set */
		if (xtext->pixmap)
			xtext_draw_bg (xtext, x, y - xtext->font->ascent,
			               text_width, xtext->fontsize);

		/* --- Draw text --- */
		/* Default foreground (Pango attrs override per-span) */
		xtext_set_source_color (xtext, xtext->col_fore);

		xtext_draw_layout_line (xtext, x, y, pango_line);

		/* --- Draw emoji sprites over U+FFFC placeholders --- */
		for (int ei = 0; ei < ent->emoji_count; ei++)
		{
			xtext_emoji_info *em = &ent->emoji_list[ei];
			int em_off = em->stripped_off;

			if (em_off < sub_start || em_off >= sub_end)
				continue;

			/* Get x position from layout */
			int sprite_x;
			if (using_cached)
			{
				int x_pos;
				pango_layout_line_index_to_x (pango_line,
				                               em_off - idx_base, FALSE, &x_pos);
				sprite_x = x + PANGO_PIXELS (x_pos);
			}
			else
			{
				PangoRectangle pos;
				pango_layout_index_to_pos (xtext->layout, em_off - idx_base, &pos);
				sprite_x = x + PANGO_PIXELS (pos.x);
			}
			int sprite_y = y - xtext->font->ascent;

			cairo_surface_t *sprite = xtext_emoji_cache_get (
				xtext->emoji_cache, em->filename);
			if (sprite)
			{
				/* Clear the U+FFFC glyph area first (it may have rendered a box) */
				xtext_set_source_color (xtext, XTEXT_BG);
				cairo_rectangle (xtext->cr, sprite_x, sprite_y,
				                 xtext->fontsize, xtext->fontsize);
				cairo_fill (xtext->cr);

				cairo_set_source_surface (xtext->cr, sprite, sprite_x, sprite_y);
				cairo_paint (xtext->cr);
			}
		}

		/* --- Selection highlight (solid BG + clipped FG re-render) --- */
		if (ent->mark_start != -1)
		{
			int mark_s = ent->mark_start;  /* raw offset */
			int mark_e = ent->mark_end;    /* raw offset */
			int raw_end = raw_offset + raw_len;

			/* Clip mark to this subline's raw range */
			if (mark_s < raw_offset) mark_s = raw_offset;
			if (mark_e > raw_end) mark_e = raw_end;

			if (mark_s < mark_e)
			{
				/* Convert clipped raw mark offsets to stripped offsets */
				int ms = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                                ent->str_len, mark_s);
				int me = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                                ent->str_len, mark_e);
				int ms_local = ms - idx_base;
				int me_local = me - idx_base;
				int sub_extent = sub_end - idx_base;  /* length in layout coords */

				/* Get pixel positions */
				int sel_x1, sel_x2;
				if (using_cached)
				{
					int x1_pos, x2_pos;
					pango_layout_line_index_to_x (pango_line, ms_local, FALSE, &x1_pos);
					sel_x1 = x + PANGO_PIXELS (x1_pos);
					if (me_local >= sub_extent)
						sel_x2 = x + text_width;
					else
					{
						pango_layout_line_index_to_x (pango_line, me_local, FALSE, &x2_pos);
						sel_x2 = x + PANGO_PIXELS (x2_pos);
					}
				}
				else
				{
					PangoRectangle r1, r2;
					pango_layout_index_to_pos (xtext->layout, ms_local, &r1);
					pango_layout_index_to_pos (xtext->layout, me_local, &r2);
					sel_x1 = x + PANGO_PIXELS (r1.x);
					sel_x2 = (me_local >= sub_extent)
					         ? x + text_width
					         : x + PANGO_PIXELS (r2.x);
				}

				/* Draw solid selection background */
				GdkRGBA mark_bg = xtext->palette[XTEXT_MARK_BG];
				gdk_cairo_set_source_rgba (xtext->cr, &mark_bg);
				cairo_rectangle (xtext->cr, sel_x1, y - xtext->font->ascent,
				                 sel_x2 - sel_x1, xtext->fontsize);
				cairo_fill (xtext->cr);

				/* Re-render text within selection region using MARK_FG.
				 * With cached layout: redraw same line with solid color + clip.
				 * Without: build modified attrlist on shared layout. */
				{
					GdkRGBA mark_fg = xtext->palette[XTEXT_MARK_FG];

					cairo_save (xtext->cr);
					cairo_rectangle (xtext->cr, sel_x1, y - xtext->font->ascent,
					                 sel_x2 - sel_x1, xtext->fontsize);
					cairo_clip (xtext->cr);

					if (using_cached)
					{
						/* Cairo clip approach: redraw cached line with solid fg.
						 * No Pango reshaping needed. */
						gdk_cairo_set_source_rgba (xtext->cr, &mark_fg);
						xtext_draw_layout_line (xtext, x, y, pango_line);
					}
					else
					{
						/* Shared layout: override attributes for selection colors */
						PangoAttrList *sel_attrs = pango_attr_list_copy (attrs);
						PangoAttribute *fg_override = pango_attr_foreground_new (
							(guint16)(mark_fg.red * 65535),
							(guint16)(mark_fg.green * 65535),
							(guint16)(mark_fg.blue * 65535));
						fg_override->start_index = 0;
						fg_override->end_index = (guint) sub_len;
						pango_attr_list_change (sel_attrs, fg_override);

						pango_layout_set_attributes (xtext->layout, sel_attrs);
						gdk_cairo_set_source_rgba (xtext->cr, &mark_fg);
						xtext_draw_layout_line (xtext, x, y,
							pango_layout_get_lines_readonly (xtext->layout)->data);
						pango_attr_list_unref (sel_attrs);
						pango_layout_set_attributes (xtext->layout, attrs);
					}

					/* Redraw emoji sprites within selection (dimmed) */
					for (int ei = 0; ei < ent->emoji_count; ei++)
					{
						xtext_emoji_info *em = &ent->emoji_list[ei];
						int em_off = em->stripped_off;

						if (em_off < sub_start || em_off >= sub_end)
							continue;

						int sprite_x;
						if (using_cached)
						{
							int x_pos;
							pango_layout_line_index_to_x (pango_line,
							                               em_off - idx_base, FALSE, &x_pos);
							sprite_x = x + PANGO_PIXELS (x_pos);
						}
						else
						{
							PangoRectangle pos;
							pango_layout_index_to_pos (xtext->layout,
							                           em_off - idx_base, &pos);
							sprite_x = x + PANGO_PIXELS (pos.x);
						}
						{
							int sprite_y = y - xtext->font->ascent;
							cairo_surface_t *sprite = xtext_emoji_cache_get (
								xtext->emoji_cache, em->filename);
							if (sprite)
							{
								cairo_set_source_surface (xtext->cr, sprite,
								                          sprite_x, sprite_y);
								cairo_paint_with_alpha (xtext->cr, 0.6);
							}
						}
					}

					cairo_restore (xtext->cr);
				}
			}
		}

		/* --- Search highlight overlay --- */
		if (ent->marks)
		{
			gboolean highlight_all = (xtext->buffer->search_flags & highlight) != 0;
			GList *gl;
			int raw_end = raw_offset + raw_len;

			for (gl = g_list_first (ent->marks); gl; gl = g_list_next (gl))
			{
				gboolean is_current = (gl == xtext->buffer->curmark);

				/* Skip non-current matches unless highlight-all is on */
				if (!is_current && !highlight_all)
					continue;

				offsets_t o;
				o.u = GPOINTER_TO_UINT (gl->data);
				int m_start = o.o.start;  /* raw offset */
				int m_end = o.o.end;      /* raw offset */

				/* Clip to this subline's raw range */
				if (m_start < raw_offset) m_start = raw_offset;
				if (m_end > raw_end) m_end = raw_end;
				if (m_start >= m_end)
					continue;

				/* Convert to stripped offsets local to this subline */
				int ms = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                                ent->str_len, m_start);
				int me = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                                ent->str_len, m_end);
				int ms_local = ms - idx_base;
				int me_local = me - idx_base;
				int hl_extent = sub_end - idx_base;

				/* Get pixel positions */
				int hl_x1, hl_x2;
				if (using_cached)
				{
					int x1_pos, x2_pos;
					pango_layout_line_index_to_x (pango_line, ms_local, FALSE, &x1_pos);
					hl_x1 = x + PANGO_PIXELS (x1_pos);
					if (me_local >= hl_extent)
						hl_x2 = x + text_width;
					else
					{
						pango_layout_line_index_to_x (pango_line, me_local, FALSE, &x2_pos);
						hl_x2 = x + PANGO_PIXELS (x2_pos);
					}
				}
				else
				{
					PangoRectangle r1, r2;
					pango_layout_index_to_pos (xtext->layout, ms_local, &r1);
					pango_layout_index_to_pos (xtext->layout, me_local, &r2);
					hl_x1 = x + PANGO_PIXELS (r1.x);
					hl_x2 = (me_local >= hl_extent)
					         ? x + text_width
					         : x + PANGO_PIXELS (r2.x);
				}

				cairo_save (xtext->cr);
				cairo_rectangle (xtext->cr, hl_x1, y - xtext->font->ascent,
				                 hl_x2 - hl_x1, xtext->fontsize);
				cairo_clip (xtext->cr);

				if (is_current)
				{
					/* Current match: solid MARK_BG + text in MARK_FG */
					GdkRGBA mark_bg = xtext->palette[XTEXT_MARK_BG];
					GdkRGBA mark_fg = xtext->palette[XTEXT_MARK_FG];

					gdk_cairo_set_source_rgba (xtext->cr, &mark_bg);
					cairo_paint (xtext->cr);

					if (using_cached)
					{
						gdk_cairo_set_source_rgba (xtext->cr, &mark_fg);
						xtext_draw_layout_line (xtext, x, y, pango_line);
					}
					else
					{
						PangoAttrList *hl_attrs = pango_attr_list_copy (attrs);
						PangoAttribute *fg_override = pango_attr_foreground_new (
							(guint16)(mark_fg.red * 65535),
							(guint16)(mark_fg.green * 65535),
							(guint16)(mark_fg.blue * 65535));
						fg_override->start_index = 0;
						fg_override->end_index = (guint) sub_len;
						pango_attr_list_change (hl_attrs, fg_override);
						pango_layout_set_attributes (xtext->layout, hl_attrs);

						gdk_cairo_set_source_rgba (xtext->cr, &mark_fg);
						xtext_draw_layout_line (xtext, x, y,
							pango_layout_get_lines_readonly (xtext->layout)->data);
						pango_attr_list_unref (hl_attrs);
						pango_layout_set_attributes (xtext->layout, attrs);
					}
				}
				else
				{
					/* Other matches: solid BG + translucent tint + MARK_FG text */
					GdkRGBA mark_fg = xtext->palette[XTEXT_MARK_FG];
					GdkRGBA tint = xtext->palette[XTEXT_MARK_BG];
					tint.alpha = 0.35f;

					xtext_draw_bg (xtext, hl_x1, y - xtext->font->ascent,
					               hl_x2 - hl_x1, xtext->fontsize);
					gdk_cairo_set_source_rgba (xtext->cr, &tint);
					cairo_paint (xtext->cr);

					if (using_cached)
					{
						gdk_cairo_set_source_rgba (xtext->cr, &mark_fg);
						xtext_draw_layout_line (xtext, x, y, pango_line);
					}
					else
					{
						PangoAttrList *hl_attrs = pango_attr_list_copy (attrs);
						PangoAttribute *fg_override = pango_attr_foreground_new (
							(guint16)(mark_fg.red * 65535),
							(guint16)(mark_fg.green * 65535),
							(guint16)(mark_fg.blue * 65535));
						fg_override->start_index = 0;
						fg_override->end_index = (guint) sub_len;
						pango_attr_list_change (hl_attrs, fg_override);
						pango_layout_set_attributes (xtext->layout, hl_attrs);

						gdk_cairo_set_source_rgba (xtext->cr, &mark_fg);
						xtext_draw_layout_line (xtext, x, y,
							pango_layout_get_lines_readonly (xtext->layout)->data);
						pango_attr_list_unref (hl_attrs);
						pango_layout_set_attributes (xtext->layout, attrs);
					}
				}

				cairo_restore (xtext->cr);
			}
		}

		/* --- URL underline overlay --- */
		if (xtext->hilight_ent == ent)
		{
			int hl_start = xtext->hilight_start;  /* raw offset */
			int hl_end = xtext->hilight_end;       /* raw offset */
			int raw_end = raw_offset + raw_len;

			/* Clip to subline */
			if (hl_start < raw_offset) hl_start = raw_offset;
			if (hl_end > raw_end) hl_end = raw_end;

			if (hl_start < hl_end)
			{
				int hs = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                                ent->str_len, hl_start);
				int he = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                                ent->str_len, hl_end);
				int ul_x1, ul_x2;
				{
					int hs_idx = hs - idx_base;
					int he_idx = he - idx_base;
					int ul_ext = sub_end - idx_base;
					if (using_cached)
					{
						int x1_pos, x2_pos;
						pango_layout_line_index_to_x (pango_line, hs_idx, FALSE, &x1_pos);
						ul_x1 = x + PANGO_PIXELS (x1_pos);
						if (he_idx >= ul_ext)
							ul_x2 = x + text_width;
						else
						{
							pango_layout_line_index_to_x (pango_line, he_idx, FALSE, &x2_pos);
							ul_x2 = x + PANGO_PIXELS (x2_pos);
						}
					}
					else
					{
						PangoRectangle r1, r2;
						pango_layout_index_to_pos (xtext->layout, hs_idx, &r1);
						pango_layout_index_to_pos (xtext->layout, he_idx, &r2);
						ul_x1 = x + PANGO_PIXELS (r1.x);
						ul_x2 = (he_idx >= ul_ext)
						         ? x + text_width
						         : x + PANGO_PIXELS (r2.x);
					}
				}
				int under_y = y + 1;

				xtext_set_source_color (xtext, xtext->col_fore);
				cairo_move_to (xtext->cr, ul_x1, under_y + 0.5);
				cairo_line_to (xtext->cr, ul_x2 - 1, under_y + 0.5);
				cairo_stroke (xtext->cr);
			}
		}
	}

	x = indent + text_width;

	/* --- Right-side background fill and separator --- */
	if (!left_only && !xtext->dont_render)
	{
		gtk_xtext_draw_sep (xtext, y - xtext->font->ascent);
		if (!xtext->skip_border_fills && xtext->clip_x2 >= x)
		{
			int xx = MAX (x, xtext->clip_x);
			xtext_draw_bg (xtext, xx, y - xtext->font->ascent,
			               MIN (xtext->clip_x2 - xx, (win_width + MARGIN) - xx),
			               xtext->fontsize);
		}
	}

	if (attrs)
	{
		pango_attr_list_unref (attrs);
		/* Reset shared layout attributes so we don't leak into other callers */
		pango_layout_set_attributes (xtext->layout, NULL);
	}

	if (x_size_ret)
		*x_size_ret = x - indent;

	return ret;
}

/* Walk stripped text to find the next line wrap point.
 * Uses pre-computed format spans for emphasis (affects character width)
 * and U+FFFC placeholders for emoji (fontsize width).
 * Returns raw byte count for compatibility with sublines storage. */

static int
find_next_wrap (GtkXText * xtext, textentry * ent, unsigned char *str,
					 int win_width, int indent)
{
	int raw_offset = str - ent->str;
	int ret;

	/* If entry has parsed format data, use the fast stripped-text path */
	if (ent->stripped_str && ent->raw_to_stripped_map)
	{
		int stripped_start = xtext_raw_to_stripped (ent->raw_to_stripped_map,
		                                           ent->str_len, raw_offset);
		const unsigned char *sstr = ent->stripped_str;
		int slen = ent->stripped_len;
		int si;

		/* Single-liner fast path */
		if (win_width >= ent->str_width + ent->indent &&
		    !memchr (sstr + stripped_start, '\n', slen - stripped_start))
			return ent->str_len - raw_offset;

		if (win_width < 1)
		{
			ret = ent->str_len - raw_offset;
			goto done;
		}

		/* Check for hard newline first */
		{
			const unsigned char *nl = memchr (sstr + stripped_start, '\n',
			                                  slen - stripped_start);
			int sub_end = nl ? (int)(nl - sstr) + 1 : slen;
			int sub_len = sub_end - stripped_start;
			int avail = win_width - indent;
			PangoLayoutLine *pline;
			int consumed;
			int pango_off = stripped_start;  /* start of text fed to Pango */
			int pango_len = sub_len;         /* length of text fed to Pango */

			if (avail < 1)
				avail = 1;

			/* For the first subline in indent mode, exclude the nick (left)
			 * text from the Pango layout.  The nick is rendered separately
			 * in the left margin; including it causes PANGO_WRAP_WORD_CHAR
			 * to word-break between nick and message.  In non-indent mode,
			 * the nick is part of the text flow and must be included. */
			if (raw_offset == 0 && ent->left_len > 0 && xtext->auto_indent)
			{
				int right_start = xtext_raw_to_stripped (ent->raw_to_stripped_map,
				                      ent->str_len, ent->left_len + 1);
				if (right_start > stripped_start && right_start < sub_end)
				{
					pango_off = right_start;
					pango_len = sub_end - right_start;
					avail = win_width - xtext->buffer->indent;
					if (avail < 1)
						avail = 1;
				}
			}

			/* Build attributes and set up layout for Pango-based wrapping */
			xtext_format_data fd_wrap = xtext_fdata_from_entry (ent);
			PangoAttrList *attrs = xtext_build_attrlist (&fd_wrap, pango_off,
			                           pango_len, xtext->palette, xtext->fontsize,
			                           xtext->font->ascent);

			pango_layout_set_text (xtext->layout,
			                       (char *)(sstr + pango_off), pango_len);
			pango_layout_set_attributes (xtext->layout, attrs);
			pango_layout_set_width (xtext->layout, avail * PANGO_SCALE);
			pango_layout_set_wrap (xtext->layout, PANGO_WRAP_WORD_CHAR);

			/* Read back where Pango broke the first line */
			pline = pango_layout_get_line_readonly (xtext->layout, 0);
			consumed = pline->length;

			/* Reset layout state */
			pango_layout_set_width (xtext->layout, -1);
			pango_layout_set_attributes (xtext->layout, NULL);
			pango_attr_list_unref (attrs);

			if (consumed >= pango_len)
			{
				/* Everything fits (including newline if present) */
				si = sub_end;
			}
			else
			{
				si = pango_off + consumed;
			}

			/* Skip past \n so the next subline starts at content,
			 * not at a newline that would render as a blank line */
			while (si < slen && sstr[si] == '\n')
				si++;

			/* If all stripped (visible) text is consumed, take the rest of
			 * the raw string — trailing formatting codes belong to the last
			 * subline, not to a phantom empty one. */
			if (si >= slen)
				ret = ent->str_len - raw_offset;
			else
				ret = xtext_stripped_to_raw (ent->raw_to_stripped_map,
				                            ent->str_len, si) - raw_offset;
		}
	}
	/* no parsed data — shouldn't happen, all entries are parsed */
	else
		ret = ent->str_len;

done:

	/* must make progress */
	if (ret < 1)
		ret = 1;

	return ret;
}

/* find the offset, in bytes, that wrap number 'line' starts at */

static int
gtk_xtext_find_subline (GtkXText *xtext, textentry *ent, int line)
{
	int rlen = 0;

	if (line > 0)
	{
		rlen = GPOINTER_TO_UINT (g_slist_nth_data (ent->sublines, line - 1));
		if (rlen == 0)
			rlen = ent->str_len;
	}
	return rlen;
}

/* horrible hack for drawing time stamps
 * returns the pixel width used (text_width + MARGIN), for hover stamp indent adjustment */

static int
gtk_xtext_render_stamp (GtkXText * xtext, textentry * ent,
								char *text, int len, int line, int win_width)
{
	unsigned char *stripped;
	xtext_fmt_span *spans;
	guint16 *r2s;
	int stripped_len, span_count;
	int y, x, text_width;
	PangoAttrList *attrs;
	PangoRectangle logical;
	PangoLayoutLine *pango_line;
	gboolean is_marked;

	if (xtext->cr == NULL)
		return xtext->stamp_width;

	/* Parse format codes in timestamp text (supports mIRC colors etc.) */
	xtext_parse_formats ((unsigned char *) text, len,
	                     &stripped, &stripped_len,
	                     &spans, &span_count,
	                     NULL, NULL,  /* no emoji in timestamps */
	                     &r2s, FALSE);

	/* Build PangoAttrList from format spans */
	{
		xtext_format_data fd_stamp = {
			.stripped_str = stripped,
			.stripped_len = stripped_len,
			.fmt_spans = spans,
			.fmt_span_count = span_count,
			.emoji_list = NULL,
			.emoji_count = 0
		};
		attrs = xtext_build_attrlist (&fd_stamp, 0, stripped_len,
		                              xtext->palette,
		                              xtext->fontsize,
		                              xtext->font->ascent);
	}

	/* Set up layout */
	pango_layout_set_text (xtext->layout, (char *) stripped, stripped_len);
	pango_layout_set_attributes (xtext->layout, attrs);
	pango_layout_get_extents (xtext->layout, NULL, &logical);
	text_width = PANGO_PIXELS (logical.width);

	y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
	x = MARGIN;

	/* Draw background (include left margin so no theme bleed at x=0..MARGIN) */
	xtext_set_source_color (xtext, XTEXT_BG);
	cairo_rectangle (xtext->cr, 0, y - xtext->font->ascent,
	                 text_width + x, xtext->fontsize);
	cairo_fill (xtext->cr);

	if (xtext->pixmap)
		xtext_draw_bg (xtext, 0, y - xtext->font->ascent,
		               text_width + x, xtext->fontsize);

	/* Draw text */
	xtext_set_source_color (xtext, xtext->col_fore);
	pango_line = pango_layout_get_lines_readonly (xtext->layout)->data;
	xtext_draw_layout_line (xtext, x, y, pango_line);

	/* Selection highlight on timestamp */
	is_marked = xtext->mark_stamp && ent->mark_start == 0;
	if (is_marked)
	{
		GdkRGBA mark_bg = xtext->palette[XTEXT_MARK_BG];
		GdkRGBA mark_fg = xtext->palette[XTEXT_MARK_FG];
		PangoAttrList *sel_attrs;
		PangoAttribute *fg_override;

		/* Solid selection background */
		gdk_cairo_set_source_rgba (xtext->cr, &mark_bg);
		cairo_rectangle (xtext->cr, 0, y - xtext->font->ascent,
		                 text_width + x, xtext->fontsize);
		cairo_fill (xtext->cr);

		/* Re-render text with selection foreground */
		sel_attrs = pango_attr_list_copy (attrs);
		fg_override = pango_attr_foreground_new (
			(guint16)(mark_fg.red * 65535),
			(guint16)(mark_fg.green * 65535),
			(guint16)(mark_fg.blue * 65535));
		fg_override->start_index = 0;
		fg_override->end_index = (guint) stripped_len;
		pango_attr_list_change (sel_attrs, fg_override);

		pango_layout_set_attributes (xtext->layout, sel_attrs);
		gdk_cairo_set_source_rgba (xtext->cr, &mark_fg);
		pango_line = pango_layout_get_lines_readonly (xtext->layout)->data;
		xtext_draw_layout_line (xtext, x, y, pango_line);
		pango_attr_list_unref (sel_attrs);
	}

	pango_attr_list_unref (attrs);
	pango_layout_set_attributes (xtext->layout, NULL);

	/* Fill remaining stamp area with background */
	text_width += MARGIN;
	if (text_width < xtext->stamp_width)
	{
		xtext_draw_bg (xtext, text_width, y - xtext->font->ascent,
		               xtext->stamp_width - text_width, xtext->fontsize);
	}

	g_free (stripped);
	g_free (spans);
	g_free (r2s);

	return text_width > xtext->stamp_width ? text_width : xtext->stamp_width;
}

/* Render a reply context line above a message: "\xe2\x86\xa9 nick: preview text..."
 * Draws at reduced alpha to visually distinguish from message text. */
static void
gtk_xtext_render_reply_context (GtkXText *xtext, textentry *ent, int line, int win_width)
{
	struct xtext_reply_info *reply = ent->reply;
	char *text;
	int y, x;
	PangoLayoutLine *pango_line;
	double saved_alpha;

	if (!reply || !xtext->cr)
		return;

	/* Build display text */
	if (reply->target_nick && reply->target_nick[0] && reply->target_preview && reply->target_preview[0])
		text = g_strdup_printf ("\xe2\x86\xa9 %s: %s", reply->target_nick, reply->target_preview);
	else if (reply->target_nick && reply->target_nick[0])
		text = g_strdup_printf ("\xe2\x86\xa9 %s", reply->target_nick);
	else
		text = g_strdup ("\xe2\x86\xa9 (unknown message)");

	y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
	x = xtext->buffer->indent;

	/* Tinted background: shift BG toward mid-gray for subtle visual distinction.
	 * Uses neutral gray (0.5) so it darkens light themes and lightens dark themes,
	 * distinct from the hover highlight which uses the FG color. */
	xtext_draw_bg (xtext, 0, y - xtext->font->ascent, win_width + MARGIN, xtext->fontsize);
	{
		GdkRGBA tint = { 0.5f, 0.5f, 0.5f, 0.08f };
		gdk_cairo_set_source_rgba (xtext->cr, &tint);
		cairo_rectangle (xtext->cr, 0, y - xtext->font->ascent, win_width + MARGIN, xtext->fontsize);
		cairo_fill (xtext->cr);
	}

	/* Set up layout with plain text (no format codes) */
	pango_layout_set_text (xtext->layout, text, -1);
	pango_layout_set_attributes (xtext->layout, NULL);
	pango_layout_set_width (xtext->layout, (win_width - x) * PANGO_SCALE);
	pango_layout_set_ellipsize (xtext->layout, PANGO_ELLIPSIZE_END);

	/* Draw at reduced alpha for muted appearance */
	saved_alpha = xtext->render_alpha;
	xtext->render_alpha = 0.5;
	xtext_set_source_color (xtext, XTEXT_FG);
	xtext->render_alpha = saved_alpha;

	pango_line = pango_layout_get_lines_readonly (xtext->layout)->data;
	xtext_draw_layout_line (xtext, x, y, pango_line);

	/* Reset layout state */
	pango_layout_set_width (xtext->layout, -1);
	pango_layout_set_ellipsize (xtext->layout, PANGO_ELLIPSIZE_NONE);

	g_free (text);
}

/* Build a display string for reaction text, replacing emoji with U+FFFC placeholders.
 * Returns the display string and populates emoji_positions array with sprite info.
 * Caller must g_free the returned string. */
typedef struct {
	int byte_offset;		/* offset of U+FFFC in display string */
	char filename[64];		/* sprite filename */
} reaction_emoji_pos;

static char *
reaction_build_display (const char *text, xtext_emoji_cache *cache,
                        reaction_emoji_pos **out_emojis, int *out_emoji_count)
{
	GString *display;
	GArray *positions;
	const unsigned char *p;
	int remaining;

	display = g_string_sized_new (strlen (text) + 16);
	positions = g_array_new (FALSE, FALSE, sizeof (reaction_emoji_pos));

	p = (const unsigned char *) text;
	remaining = strlen (text);

	while (remaining > 0)
	{
		int emoji_bytes = 0;
		char filename[64];
		gboolean is_emoji = FALSE;

		if (cache)
			is_emoji = xtext_emoji_detect (p, remaining, &emoji_bytes, filename, sizeof (filename));

		if (is_emoji && xtext_emoji_cache_get (cache, filename))
		{
			reaction_emoji_pos pos;
			pos.byte_offset = display->len;
			g_strlcpy (pos.filename, filename, sizeof (pos.filename));
			g_array_append_val (positions, pos);

			/* U+FFFC = EF BF BC in UTF-8 */
			g_string_append (display, "\xef\xbf\xbc");
			p += emoji_bytes;
			remaining -= emoji_bytes;
		}
		else
		{
			/* Copy one UTF-8 character */
			int char_len = g_utf8_next_char ((const char *)p) - (const char *)p;
			if (char_len > remaining) char_len = remaining;
			g_string_append_len (display, (const char *)p, char_len);
			p += char_len;
			remaining -= char_len;
		}
	}

	*out_emoji_count = positions->len;
	*out_emojis = (reaction_emoji_pos *) g_array_free (positions, FALSE);
	return g_string_free (display, FALSE);
}

/* Measure total width of all reaction badges (for right-alignment and click detection).
 * Uses Pango to measure each badge label. Returns total width including inter-badge gaps. */
static int
gtk_xtext_measure_reaction_badges (GtkXText *xtext, struct xtext_reactions_info *ri, int win_width)
{
	int total_width = 0;
	int pad_x = 6;
	guint i;
	int badge_count = 0;

	for (i = 0; i < ri->reactions->len; i++)
	{
		struct xtext_reaction *react = g_ptr_array_index (ri->reactions, i);
		PangoRectangle logical;
		char *label;
		int content_width, badge_width;
		reaction_emoji_pos *emojis;
		int emoji_count, j;
		char *base_display;

		if (react->count <= 0)
			continue;

		base_display = reaction_build_display (react->text, xtext->emoji_cache,
		                                       &emojis, &emoji_count);
		if (react->count > 1)
		{
			label = g_strdup_printf ("%s %d", base_display, react->count);
			g_free (base_display);
		}
		else
		{
			label = base_display;
		}

		pango_layout_set_text (xtext->layout, label, -1);
		pango_layout_set_attributes (xtext->layout, NULL);
		pango_layout_get_extents (xtext->layout, NULL, &logical);

		content_width = PANGO_PIXELS (logical.width);
		for (j = 0; j < emoji_count; j++)
		{
			PangoRectangle fffc_pos;
			pango_layout_index_to_pos (xtext->layout, emojis[j].byte_offset, &fffc_pos);
			content_width += xtext->fontsize - PANGO_PIXELS (fffc_pos.width);
		}

		badge_width = content_width + pad_x * 2;

		if (total_width + badge_width > win_width)
		{
			g_free (label);
			g_free (emojis);
			break;
		}

		total_width += badge_width;
		badge_count++;

		g_free (label);
		g_free (emojis);
	}

	/* Add inter-badge gaps */
	if (badge_count > 1)
		total_width += (badge_count - 1) * 4;

	return total_width;
}

/* Render reaction badges below a message: [emoji 2] [text] [emoji emoji 3] ...
 * Each badge is a rounded rect. Emoji are rendered as sprites when available,
 * with Pango fallback for text and unsupported emoji. Right-aligned. */
static void
gtk_xtext_render_reaction_badges (GtkXText *xtext, textentry *ent, int line, int win_width)
{
	struct xtext_reactions_info *ri = ent->reactions;
	int y, badge_x;
	double saved_alpha;
	guint i;
	int total_width;

	if (!ri || ri->total_count == 0 || !xtext->cr)
		return;

	y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;

	/* Right-align: measure total width, then start from right edge */
	total_width = gtk_xtext_measure_reaction_badges (xtext, ri, win_width);
	badge_x = win_width - total_width;

	/* Clear background for full line */
	xtext_draw_bg (xtext, 0, y - xtext->font->ascent, win_width + MARGIN, xtext->fontsize);

	for (i = 0; i < ri->reactions->len; i++)
	{
		struct xtext_reaction *react = g_ptr_array_index (ri->reactions, i);
		int badge_width, badge_height, pad_x, pad_y;
		int content_width;
		gboolean is_self;
		char *display_text;
		reaction_emoji_pos *emojis;
		int emoji_count;
		PangoRectangle logical;
		PangoLayoutLine *pango_line;
		int j;

		if (react->count <= 0)
			continue;

		/* Build display string with U+FFFC for sprite emoji */
		{
			char *base_display;
			base_display = reaction_build_display (react->text, xtext->emoji_cache,
			                                       &emojis, &emoji_count);
			if (react->count > 1)
			{
				display_text = g_strdup_printf ("%s %d", base_display, react->count);
				g_free (base_display);
			}
			else
			{
				display_text = base_display;
			}
		}

		/* Measure via Pango (U+FFFC gets a placeholder width from the font) */
		pango_layout_set_text (xtext->layout, display_text, -1);
		pango_layout_set_attributes (xtext->layout, NULL);
		pango_layout_get_extents (xtext->layout, NULL, &logical);

		/* If we have sprite emoji, account for sprite width vs U+FFFC glyph width.
		 * Each sprite is fontsize pixels wide. Compute width adjustment. */
		content_width = PANGO_PIXELS (logical.width);
		for (j = 0; j < emoji_count; j++)
		{
			PangoRectangle fffc_pos;
			pango_layout_index_to_pos (xtext->layout, emojis[j].byte_offset, &fffc_pos);
			content_width += xtext->fontsize - PANGO_PIXELS (fffc_pos.width);
		}

		pad_x = 6;
		pad_y = 1;
		badge_width = content_width + pad_x * 2;
		badge_height = xtext->fontsize - pad_y * 2;

		/* Clip if badges exceed width */
		if (badge_x + badge_width > win_width)
		{
			g_free (display_text);
			g_free (emojis);
			break;
		}

		is_self = gtk_xtext_entry_has_self_reaction (ent, react->text);

		/* Draw rounded rect background */
		{
			double rx = badge_x;
			double ry = y - xtext->font->ascent + pad_y;
			double rw = badge_width;
			double rh = badge_height;
			double radius = 4.0;
			GdkRGBA bg;

			bg = xtext->palette[XTEXT_FG];
			bg.alpha = is_self ? 0.2 : 0.1;

			cairo_new_sub_path (xtext->cr);
			cairo_arc (xtext->cr, rx + rw - radius, ry + radius, radius, -G_PI/2, 0);
			cairo_arc (xtext->cr, rx + rw - radius, ry + rh - radius, radius, 0, G_PI/2);
			cairo_arc (xtext->cr, rx + radius, ry + rh - radius, radius, G_PI/2, G_PI);
			cairo_arc (xtext->cr, rx + radius, ry + radius, radius, G_PI, 3*G_PI/2);
			cairo_close_path (xtext->cr);
			gdk_cairo_set_source_rgba (xtext->cr, &bg);
			cairo_fill (xtext->cr);
		}

		/* Draw text content via Pango */
		saved_alpha = xtext->render_alpha;
		xtext->render_alpha = is_self ? 1.0 : 0.7;
		xtext_set_source_color (xtext, XTEXT_FG);
		xtext->render_alpha = saved_alpha;

		pango_line = pango_layout_get_lines_readonly (xtext->layout)->data;
		xtext_draw_layout_line (xtext, badge_x + pad_x, y, pango_line);

		/* Overlay sprite emoji on top of U+FFFC placeholders */
		for (j = 0; j < emoji_count; j++)
		{
			cairo_surface_t *sprite = xtext_emoji_cache_get (xtext->emoji_cache,
			                                                  emojis[j].filename);
			if (sprite)
			{
				PangoRectangle pos;
				int sprite_x, sprite_y;
				pango_layout_index_to_pos (xtext->layout, emojis[j].byte_offset, &pos);
				sprite_x = badge_x + pad_x + PANGO_PIXELS (pos.x);
				sprite_y = y - xtext->font->ascent;

				/* Clear U+FFFC glyph area */
				xtext_draw_bg (xtext, sprite_x, sprite_y, xtext->fontsize, xtext->fontsize);
				cairo_set_source_surface (xtext->cr, sprite, sprite_x, sprite_y);
				cairo_paint_with_alpha (xtext->cr, 0.92); /* subtle alpha to let bg tint through */
			}
		}

		badge_x += badge_width + 4; /* 4px gap between badges */

		g_free (display_text);
		g_free (emojis);
	}
}

/* render a day separator line: ──── March 29, 2026 ──── */

static void
gtk_xtext_render_day_separator (GtkXText *xtext, textentry *ent, int line, int win_width)
{
	int y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
	int mid_y = y - xtext->font->ascent + xtext->fontsize / 2;
	char date_buf[128];
	struct tm entry_tm, now_tm;
	time_t now = time (NULL);
	PangoRectangle logical;
	int text_w, text_x, pad;
	GdkRGBA color;

#ifdef WIN32
	localtime_s (&entry_tm, &ent->stamp);
	localtime_s (&now_tm, &now);
#else
	localtime_r (&ent->stamp, &entry_tm);
	localtime_r (&now, &now_tm);
#endif

	if (entry_tm.tm_year == now_tm.tm_year && entry_tm.tm_yday == now_tm.tm_yday)
		g_strlcpy (date_buf, _("Today"), sizeof (date_buf));
	else if ((entry_tm.tm_year == now_tm.tm_year && entry_tm.tm_yday == now_tm.tm_yday - 1) ||
	         (now_tm.tm_yday == 0 && entry_tm.tm_year == now_tm.tm_year - 1 &&
	          entry_tm.tm_mon == 11 && entry_tm.tm_mday == 31))
		g_strlcpy (date_buf, _("Yesterday"), sizeof (date_buf));
	else
		strftime (date_buf, sizeof (date_buf), "%B %d, %Y", &entry_tm);

	/* Clear background */
	xtext_draw_bg (xtext, 0, y - xtext->font->ascent, win_width + MARGIN, xtext->fontsize);

	/* Measure text */
	pango_layout_set_text (xtext->layout, date_buf, -1);
	pango_layout_set_attributes (xtext->layout, NULL);
	pango_layout_get_pixel_extents (xtext->layout, NULL, &logical);
	text_w = logical.width;
	pad = 12;
	text_x = (win_width - text_w) / 2;

	/* Draw horizontal lines */
	color = xtext->palette[XTEXT_FG];
	color.alpha = 0.15;
	gdk_cairo_set_source_rgba (xtext->cr, &color);

	cairo_set_line_width (xtext->cr, 1.0);
	cairo_move_to (xtext->cr, MARGIN + 8, mid_y + 0.5);
	cairo_line_to (xtext->cr, text_x - pad, mid_y + 0.5);
	cairo_stroke (xtext->cr);

	cairo_move_to (xtext->cr, text_x + text_w + pad, mid_y + 0.5);
	cairo_line_to (xtext->cr, win_width - 8, mid_y + 0.5);
	cairo_stroke (xtext->cr);

	/* Draw centered date text */
	color = xtext->palette[XTEXT_FG];
	color.alpha = 0.5;
	gdk_cairo_set_source_rgba (xtext->cr, &color);
	{
		PangoLayoutLine *pl = pango_layout_get_lines_readonly (xtext->layout)->data;
		xtext_draw_layout_line (xtext, text_x, y, pl);
	}

	pango_layout_set_attributes (xtext->layout, NULL);
}

/* Render the collapse/expand indicator line for collapsible multiline entries */
static void
gtk_xtext_render_collapse_indicator (GtkXText *xtext, textentry *ent,
                                     int line, int win_width, gboolean collapsed)
{
	int y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
	char buf[128];
	int text_x;
	GdkRGBA color;

	int total_sublines = g_slist_length (ent->sublines);
	int hidden = total_sublines - COLLAPSE_PREVIEW_LINES;

	if (collapsed)
		g_snprintf (buf, sizeof (buf), "\xe2\x96\xbc Show more (%d lines)", hidden);
	else
		g_snprintf (buf, sizeof (buf), "\xe2\x96\xb2 Show less");

	/* Clear background */
	xtext_draw_bg (xtext, 0, y - xtext->font->ascent, win_width + MARGIN, xtext->fontsize);

	/* Set text for layout */
	pango_layout_set_text (xtext->layout, buf, -1);
	pango_layout_set_attributes (xtext->layout, NULL);
	text_x = xtext->buffer->indent + 4;  /* align with message text start */

	/* Draw text in half-alpha foreground */
	color = xtext->palette[XTEXT_FG];
	color.alpha = 0.5;
	gdk_cairo_set_source_rgba (xtext->cr, &color);
	{
		PangoLayoutLine *pl = pango_layout_get_lines_readonly (xtext->layout)->data;
		xtext_draw_layout_line (xtext, text_x, y, pl);
	}
	pango_layout_set_attributes (xtext->layout, NULL);
}

/* render a single line, which may wrap to more lines */

static int
gtk_xtext_render_line (GtkXText * xtext, textentry * ent, int line,
							  int lines_max, int subline, int win_width)
{
	unsigned char *str;
	int indent, taken, entline, len, y, start_subline;
	int raw_offset;
	int text_subline; /* subline offset into text sublines (after extra_lines_above) */
	int first_subline_y = 0; /* y position of the first rendered text subline (for reply button) */

	entline = taken = 0;
	str = ent->str;
	raw_offset = 0;
	indent = ent->indent;
	start_subline = subline;

	/* --- Day separator line above the message --- */
	if ((ent->flags & TEXTENTRY_FLAG_DAY_BOUNDARY) && prefs.hex_gui_day_separator)
	{
		if (subline == 0)
		{
			gtk_xtext_render_day_separator (xtext, ent, line, win_width);
			line++;
			taken++;
			if (line >= lines_max)
				return taken;
		}
		else
		{
			subline--;
		}
	}

	/* --- Reply context line above the message --- */
	if (ent->reply && ent->extra_lines_above > 0)
	{
		if (subline == 0)
		{
			/* Render the reply context line */
			gtk_xtext_render_reply_context (xtext, ent, line, win_width);
			line++;
			taken++;
			if (line >= lines_max)
				return taken;
		}
		else
		{
			/* Skipping the reply context line */
			subline--;
		}
	}

	/* Convert remaining subline to text subline offset */
	text_subline = subline;

	/* draw the timestamp (only on the first text subline) */
	if (text_subline == 0)
	{
		gboolean show_stamp = xtext->buffer->time_stamp &&
			(!xtext->skip_stamp || xtext->mark_stamp || xtext->force_stamp);
		gboolean swap_relative = xtext->hover_stamp_visible && xtext->hover_ent != NULL;
		gboolean show_hover_stamp = swap_relative && !xtext->buffer->time_stamp
			&& ent->left_len > 0;

		if ((show_stamp || show_hover_stamp) && ent->left_len > 0)
		{
			int stamp_used;

			if (swap_relative && ent->stamp)
			{
				char rel[128];
				xtext_format_relative_time (ent->stamp, rel, sizeof (rel));
				stamp_used = gtk_xtext_render_stamp (xtext, ent, rel, strlen (rel), line, win_width);
			}
			else
			{
				char *time_str;
				int ts_len;

				ts_len = xtext_get_stamp_str (ent->stamp, &time_str);
				stamp_used = gtk_xtext_render_stamp (xtext, ent, time_str, ts_len, line, win_width);
				g_free (time_str);
			}

			/* Shift text right to make room for wider/new stamp */
			if (swap_relative)
			{
				if (show_hover_stamp && !xtext->auto_indent)
					indent = stamp_used;  /* flat layout, text starts after stamp */
				else if (stamp_used > xtext->stamp_width)
					indent = ent->indent + (stamp_used - xtext->stamp_width);
			}
		}
		else if (xtext->buffer->time_stamp && ent->left_len <= 0)
		{
			int bg_y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
			xtext_draw_bg (xtext, 0, bg_y - xtext->font->ascent,
								xtext->stamp_width, xtext->fontsize);
		}
	}

	/* draw each text subline one by one */
	{
		int max_text_entline = ent->collapsed
			? MIN(COLLAPSE_PREVIEW_LINES, (int)g_slist_length (ent->sublines))
			: INT_MAX;

		/* Look up cached PangoLayout for this entry */
		xtext_line_display *cached_disp = display_cache_get (
			xtext->buffer->display_cache, ent->entry_id,
			xtext->buffer->window_width);

		do
		{
			if (entline > 0)
				len = GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, entline)) - GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, entline - 1));
			else
				len = GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, entline));

			entline++;

			y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
			if (!text_subline)
			{
				/* Get cached PangoLayoutLine for this text subline if available.
				 * Skip for the first text subline (entline==1 after increment)
				 * when auto_indent is on and entry has a nick — the cached layout
				 * excludes the nick text, but the first subline's raw data includes
				 * it.  Use the shared layout for the first subline so the nick
				 * renders correctly. */
				PangoLayoutLine *cached_pline = NULL;
				if (cached_disp && cached_disp->layout)
				{
					int text_subline_idx = entline - 1;	/* entline was just incremented */
					gboolean has_nick = (cached_disp->layout_text_offset > 0);

					/* The cached layout excludes the nick text.  The first text
					 * subline includes the nick, so skip it.  Continuation sublines
					 * map to cached layout lines with a -1 offset.
					 *
					 * For no-nick entries where ent->indent != buf->indent, the
					 * layout was wrapped at ent->indent but continuation sublines
					 * render at buf->indent — width mismatch.  Only use the cached
					 * layout when indents match (single-line or same indent). */
					if (has_nick)
					{
						if (text_subline_idx > 0)
						{
							/* Layout lines map directly to text sublines — the nick
							 * is excluded from the layout text but doesn't add a
							 * layout line.  Subline 1 = layout line 1, etc. */
							if (text_subline_idx < cached_disp->layout_n_lines)
								cached_pline = pango_layout_get_line_readonly (
									cached_disp->layout, text_subline_idx);
						}
						/* text_subline_idx == 0: first subline with nick, use shared layout */
					}
					else if (ent->indent == xtext->buffer->indent ||
					         cached_disp->layout_n_lines <= 1)
					{
						/* No nick, same indent (or single line): direct mapping */
						if (text_subline_idx < cached_disp->layout_n_lines)
							cached_pline = pango_layout_get_line_readonly (
								cached_disp->layout, text_subline_idx);
					}
					/* else: no-nick entry with indent mismatch and wrapping, skip cache */
				}

				if (!gtk_xtext_render_subline (xtext, y, ent, raw_offset, len,
				                               win_width, indent, line, FALSE, NULL, cached_pline,
				                               cached_disp ? cached_disp->layout_text_offset : 0))
				{
					/* small optimization */
					gtk_xtext_draw_marker (xtext, ent, y - xtext->fontsize * (taken + start_subline + 1));
					return ENT_DISPLAY_LINES (ent) - start_subline;
				}

				/* Save y of first text subline for reply button (drawn after loop) */
				if (entline == 1)
					first_subline_y = y;
			} else
			{
				/* Skipping sublines before the visible region */
				text_subline--;
				line--;
				taken--;
			}

			indent = xtext->buffer->indent;
			line++;
			taken++;
			str += len;
			raw_offset += len;

			if (line >= lines_max)
				break;

			if (entline >= max_text_entline)
				break;

		}
		while (str < ent->str + ent->str_len);
	}

	/* Render collapse/expand indicator for collapsible entries */
	if (ent->collapsed && line < lines_max)
	{
		gtk_xtext_render_collapse_indicator (xtext, ent, line, win_width, TRUE);
		line++;
		taken++;
	}
	else if (ent->collapsible && !ent->collapsed && line < lines_max)
	{
		gtk_xtext_render_collapse_indicator (xtext, ent, line, win_width, FALSE);
		line++;
		taken++;
	}

	gtk_xtext_draw_marker (xtext, ent, y - xtext->fontsize * (taken + start_subline));

	/* --- Hover buttons: reply, react-text, react-emoji, [redact] --- */
	if (ent == xtext->hover_ent && ent->msgid && first_subline_y && xtext->cr)
	{
		int btn_size = xtext->fontsize + 2;
		int gap = 2;
		int has_redact = xtext->redact_button_visible;
		int btn_count = has_redact ? 4 : 3;
		int total_w = btn_size * btn_count + gap * (btn_count - 1);
		int base_x = win_width - total_w - 4;
		int btn_y = first_subline_y - xtext->font->ascent;
		GdkRGBA btn_bg, fg;
		PangoLayoutLine *pango_line;
		PangoRectangle glyph_rect;
		static const char *labels[] = { "\xe2\x86\xa9", "Aa", "\xf0\x9f\x98\x80", "\xc3\x97" };
		int btn_xs[4];
		int i;

		for (i = 0; i < btn_count; i++)
			btn_xs[i] = base_x + (btn_size + gap) * i;

		btn_bg = xtext->palette[XTEXT_FG];
		btn_bg.alpha = 0.15f;
		fg = xtext->palette[XTEXT_FG];
		fg.alpha = 0.7f;

		for (i = 0; i < btn_count; i++)
		{
			int bx = btn_xs[i];
			GdkRGBA this_bg = btn_bg;

			if (has_redact && i == 3 &&
			    xtext->redact_confirm_ent == ent &&
			    (g_get_monotonic_time () - xtext->redact_confirm_time) < 3 * G_USEC_PER_SEC)
			{
				this_bg.alpha = 0.35f;
			}

			/* Rounded rect background */
			cairo_new_sub_path (xtext->cr);
			cairo_arc (xtext->cr, bx + btn_size - 4, btn_y + 4, 4, -G_PI/2, 0);
			cairo_arc (xtext->cr, bx + btn_size - 4, btn_y + btn_size - 4, 4, 0, G_PI/2);
			cairo_arc (xtext->cr, bx + 4, btn_y + btn_size - 4, 4, G_PI/2, G_PI);
			cairo_arc (xtext->cr, bx + 4, btn_y + 4, 4, G_PI, 3*G_PI/2);
			cairo_close_path (xtext->cr);
			gdk_cairo_set_source_rgba (xtext->cr, &this_bg);
			cairo_fill (xtext->cr);

			/* Emoji button: use sprite from cache for consistent Twemoji rendering */
			if (i == 2 && xtext->emoji_cache)
			{
				int emoji_bytes = 0;
				char emoji_fn[64];
				const unsigned char *emoji_str = (const unsigned char *) labels[2];
				int emoji_len = strlen (labels[2]);

				if (xtext_emoji_detect (emoji_str, emoji_len, &emoji_bytes, emoji_fn, sizeof (emoji_fn)))
				{
					cairo_surface_t *sprite = xtext_emoji_cache_get (xtext->emoji_cache, emoji_fn);
					if (sprite)
					{
						int sprite_sz = btn_size - 4; /* slight padding inside button */
						int sx = bx + (btn_size - sprite_sz) / 2;
						int sy = btn_y + (btn_size - sprite_sz) / 2;
						double scale = (double) sprite_sz / cairo_image_surface_get_width (sprite);

						cairo_save (xtext->cr);
						cairo_translate (xtext->cr, sx, sy);
						cairo_scale (xtext->cr, scale, scale);
						cairo_set_source_surface (xtext->cr, sprite, 0, 0);
						cairo_paint_with_alpha (xtext->cr, 0.85);
						cairo_restore (xtext->cr);
						continue;
					}
				}
			}

			/* Text label (reply ↩, react-text Aa, or emoji fallback) */
			pango_layout_set_text (xtext->layout, labels[i], -1);
			pango_layout_set_attributes (xtext->layout, NULL);
			gdk_cairo_set_source_rgba (xtext->cr, &fg);
			pango_layout_get_extents (xtext->layout, NULL, &glyph_rect);
			pango_line = pango_layout_get_lines_readonly (xtext->layout)->data;
			xtext_draw_layout_line (xtext,
				bx + (btn_size - PANGO_PIXELS (glyph_rect.width)) / 2,
				first_subline_y, pango_line);
		}

		/* Store button positions for click detection */
		xtext->reply_btn_x = btn_xs[0];
		xtext->react_text_btn_x = btn_xs[1];
		xtext->react_emoji_btn_x = btn_xs[2];
		xtext->redact_btn_x = has_redact ? btn_xs[3] : -1;
		xtext->hover_btn_y = btn_y;
		xtext->hover_btn_size = btn_size;
	}

	/* --- Reaction badges below the message --- */
	if (ent->extra_lines_below > 0 && line < lines_max)
	{
		gtk_xtext_render_reaction_badges (xtext, ent, line, win_width);
		line++;
		taken++;
	}

	/* --- Hover / flash highlight overlay (drawn after all content) --- */
	{
		gboolean is_hover = (ent == xtext->hover_ent ||
		                     ent == xtext->hover_reply_target);
		gboolean is_flash = (ent == xtext->flash_ent);

		/* Check group_id match for multiline messages */
		if (!is_hover && !is_flash && ent->group_id != 0)
		{
			if (xtext->hover_ent && xtext->hover_ent->group_id == ent->group_id)
				is_hover = TRUE;
			if (xtext->flash_ent && xtext->flash_ent->group_id == ent->group_id)
				is_flash = TRUE;
		}

		if (xtext->cr && taken > 0 && (is_hover || is_flash))
		{
			int hl_y_top = xtext->fontsize * (line - taken) - xtext->pixel_offset;
			int hl_height = xtext->fontsize * taken;
			GdkRGBA hl = xtext->palette[XTEXT_FG];
			hl.alpha = is_flash ? 0.12 : 0.06;
			gdk_cairo_set_source_rgba (xtext->cr, &hl);
			cairo_rectangle (xtext->cr, 0, hl_y_top, win_width + MARGIN, hl_height);
			cairo_fill (xtext->cr);
		}
	}

	return taken;
}

void
gtk_xtext_set_palette (GtkXText * xtext, GdkRGBA palette[])
{
	int i;

	for (i = (XTEXT_COLS-1); i >= 0; i--)
	{
		xtext->palette[i] = palette[i];
	}

	/* Phase 4: state colors (xtext-internal, not from external palette) */
	xtext->palette[XTEXT_PENDING_FG] = (GdkRGBA){0.6f, 0.6f, 0.6f, 1.0f};
	xtext->palette[XTEXT_REDACTED_FG] = (GdkRGBA){0.5f, 0.5f, 0.5f, 1.0f};

	if (gtk_widget_get_realized (GTK_WIDGET(xtext)))
	{
		xtext_set_fg (xtext, XTEXT_FG);
		xtext_set_bg (xtext, XTEXT_BG);
		/* Marker color is stored in palette, no separate GC needed in GTK3 */
	}
	xtext->col_fore = XTEXT_FG;
	xtext->col_back = XTEXT_BG;

	/* Palette change invalidates all cached PangoLayouts (attribute colors) */
	if (xtext->buffer)
		display_cache_flush (xtext->buffer->display_cache, xtext->buffer);
}

static void
gtk_xtext_fix_indent (xtext_buffer *buf)
{
	int j;

	/* make indent a multiple of the space width */
	if (buf->indent && buf->xtext->space_width)
	{
		j = 0;
		while (j < buf->indent)
		{
			j += buf->xtext->space_width;
		}
		buf->indent = j;
	}

	dontscroll (buf);	/* force scrolling off */
}

/* Width-based indent cap: the largest buf->indent the current window
 * can afford while still leaving XTEXT_MIN_TEXT_COL pixels for message
 * text. Never returns below XTEXT_MIN_INDENT — at that floor the measure
 * vfunc keeps the widget from shrinking further. Used by the resize
 * clamp so a manually dragged separator is preserved up to the width
 * limit, independent of max_auto_indent. */
static int
gtk_xtext_width_indent_cap (xtext_buffer *buf)
{
	int width_cap;

	if (buf->window_width <= 0)
		return G_MAXINT;

	width_cap = buf->window_width - MARGIN - XTEXT_MIN_TEXT_COL;
	if (width_cap < XTEXT_MIN_INDENT)
		width_cap = XTEXT_MIN_INDENT;
	return width_cap;
}

/* Cap on automatic indent growth: the lesser of the user-configured
 * max_auto_indent and the current width cap. Used by the grow paths
 * and by the indent-toggle/timestamp rebuilds so auto-grown indent
 * respects both the user pref ceiling and the width floor. Manual
 * separator drags are NOT bounded by this — see the width cap. */
static int
gtk_xtext_auto_indent_cap (xtext_buffer *buf)
{
	int cap = buf->xtext->max_auto_indent;
	int width_cap = gtk_xtext_width_indent_cap (buf);
	if (width_cap < cap)
		cap = width_cap;
	return cap;
}

/* If the current indent exceeds the width-based cap, clamp it and
 * recompute per-entry indents so the grown nick column is realigned
 * against the shifted separator. Returns TRUE if anything changed. */
static gboolean
gtk_xtext_clamp_indent_to_width (xtext_buffer *buf)
{
	int cap;

	if (!buf->xtext->auto_indent)
		return FALSE;

	cap = gtk_xtext_width_indent_cap (buf);
	if (buf->indent <= cap)
		return FALSE;

	buf->indent = cap;
	gtk_xtext_fix_indent (buf);
	gtk_xtext_recalc_widths (buf, FALSE);
	buf->xtext->force_render = TRUE;
	return TRUE;
}

static void
gtk_xtext_recalc_widths (xtext_buffer *buf, int do_str_width)
{
	textentry *ent;

	/* In virtual mode, only materialized entries are in the linked list,
	 * so this naturally only processes the visible window. */

	/* since we have a new font, we have to recalc the text widths */
	ent = buf->text_first;
	while (ent)
	{
		if (do_str_width)
		{
			ent->str_width = gtk_xtext_text_width_ent (buf->xtext, ent);
		}
		if (ent->left_len != -1)
		{
			if (buf->xtext->auto_indent)
			{
				ent->indent =
					(buf->indent -
					 gtk_xtext_text_width (buf->xtext, ent->str,
											ent->left_len)) - buf->xtext->space_width;
				if (ent->indent < MARGIN)
					ent->indent = MARGIN;
			}
			else
			{
				ent->indent = buf->time_stamp ? buf->xtext->stamp_width : MARGIN;
			}
		}
		ent = ent->next;
	}

	gtk_xtext_calc_lines (buf, FALSE);
}

int
gtk_xtext_set_font (GtkXText *xtext, char *name)
{

	if (xtext->font)
		backend_font_close (xtext);

	/* realize now, so that font_open has a XDisplay */
	gtk_widget_realize (GTK_WIDGET (xtext));

	backend_font_open (xtext, name);
	if (xtext->font == NULL)
		return FALSE;

	/* Initialize emoji sprite cache if enabled and not already created */
	if (prefs.hex_gui_emoji_sprites && !xtext->emoji_cache)
	{
		char *emoji_dir = g_build_filename (get_xdir (), "emoji", NULL);
		if (g_file_test (emoji_dir, G_FILE_TEST_IS_DIR))
			xtext->emoji_cache = xtext_emoji_cache_new (emoji_dir, xtext->fontsize);
		g_free (emoji_dir);
	}

	{
		char *time_str;
		int stamp_size = xtext_get_stamp_str (time(0), &time_str);
		xtext->stamp_width =
			gtk_xtext_text_width (xtext, time_str, stamp_size) + MARGIN;
		g_free (time_str);
	}

	gtk_xtext_fix_indent (xtext->buffer);

	/* Font change invalidates all cached PangoLayouts */
	display_cache_flush (xtext->buffer->display_cache, xtext->buffer);

	if (gtk_widget_get_realized (GTK_WIDGET(xtext)))
	{
		gtk_xtext_recalc_widths (xtext->buffer, TRUE);

		/* Snap to bottom after reflow — font change alters line count
		 * and page size, which can unanchor the scroll position. */
		if (xtext->buffer->scroll_anchor.anchor_to_bottom)
			gtk_adjustment_set_value (xtext->adj,
				gtk_adjustment_get_upper (xtext->adj) -
				gtk_adjustment_get_page_size (xtext->adj));
	}

	return TRUE;
}

void
gtk_xtext_set_background (GtkXText * xtext, cairo_surface_t * pixmap)
{
	if (xtext->pixmap)
	{
		cairo_surface_destroy (xtext->pixmap);
		xtext->pixmap = NULL;
	}

	dontscroll (xtext->buffer);
	xtext->pixmap = pixmap;

	if (pixmap != NULL)
	{
		cairo_surface_reference (pixmap);
		xtext->ts_x = xtext->ts_y = 0;
	}
	/* In GTK3/Cairo, background tiling is handled in the drawing code */
}

void
gtk_xtext_save (GtkXText * xtext, int fh)
{
	textentry *ent;
	int newlen;
	char *buf;
	char *stamp_str;
	int stamp_len;
	/* Honor the user's "Show timestamps" preference, mirroring what they
	 * see on screen.  Per-buffer time_stamp flag is the source of truth
	 * (it tracks prefs.hex_stamp_text via gtk_xtext_set_time_stamp). */
	gboolean want_stamp = xtext->buffer->time_stamp;

	/* Virtual mode: stream all messages from DB in batches */
	if (HAS_VIRT_DB (xtext->buffer) &&
	    xtext->buffer->virt_channel)
	{
		int total = scrollback_count (xtext->buffer->virt_db,
		                              xtext->buffer->virt_channel);
		int offset = 0;
		int batch = 500;

		while (offset < total)
		{
			GSList *msgs = scrollback_load_range (xtext->buffer->virt_db,
			                                      xtext->buffer->virt_channel,
			                                      offset, batch);
			GSList *iter;

			for (iter = msgs; iter; iter = iter->next)
			{
				scrollback_msg *msg = iter->data;
				if (msg->text)
				{
					if (want_stamp && msg->timestamp > 0)
					{
						stamp_len = xtext_get_stamp_str (msg->timestamp, &stamp_str);
						HC_IGNORE_RESULT (write (fh, stamp_str, stamp_len));
						g_free (stamp_str);
					}
					buf = gtk_xtext_strip_color ((unsigned char *)msg->text,
					                             strlen (msg->text), NULL,
					                             &newlen, NULL, FALSE);
					HC_IGNORE_RESULT (write (fh, buf, newlen));
					HC_IGNORE_RESULT (write (fh, "\n", 1));
					g_free (buf);
				}
			}
			offset += batch;
			scrollback_msg_list_free (msgs);
		}
		return;
	}

	ent = xtext->buffer->text_first;
	while (ent)
	{
		if (want_stamp && ent->stamp > 0)
		{
			stamp_len = xtext_get_stamp_str (ent->stamp, &stamp_str);
			HC_IGNORE_RESULT (write (fh, stamp_str, stamp_len));
			g_free (stamp_str);
		}
		buf = gtk_xtext_strip_color (ent->str, ent->str_len, NULL,
											  &newlen, NULL, FALSE);
		HC_IGNORE_RESULT (write (fh, buf, newlen));
		HC_IGNORE_RESULT (write (fh, "\n", 1));
		g_free (buf);
		ent = ent->next;
	}
}

/* count how many lines 'ent' will take (with wraps) */

static int
gtk_xtext_lines_taken (xtext_buffer *buf, textentry * ent)
{
	unsigned char *str;
	int indent, len;
	int win_width;

	win_width = buf->window_width - MARGIN;

	/* Fast path: if entry was already a single-liner and still fits,
	 * sublines are unchanged — skip the free/realloc/Pango cycle.
	 * Inline display_lines update (we know text_lines=1). */
	if (ent->sublines && !ent->sublines->next &&
	    win_width >= ent->indent + ent->str_width &&
	    !(ent->stripped_str && memchr (ent->stripped_str, '\n', ent->stripped_len)))
	{
		ent->sublines_width = buf->window_width;
		ent->display_lines = 1 + ent->extra_lines_above
			+ ent->extra_lines_below + (ent->collapsible ? 1 : 0);
		return 1;
	}

	g_slist_free (ent->sublines);
	ent->sublines = NULL;

	if (win_width >= ent->indent + ent->str_width &&
	    !(ent->stripped_str && memchr (ent->stripped_str, '\n', ent->stripped_len)))
	{
		ent->sublines = g_slist_append (ent->sublines, GINT_TO_POINTER (ent->str_len));
		ent->sublines_width = buf->window_width;
		ent->display_lines = 1 + ent->extra_lines_above
			+ ent->extra_lines_below + (ent->collapsible ? 1 : 0);
		return 1;
	}

	/* Multi-liner: create a PangoLayout with the message text, let Pango
	 * wrap it, and read back line boundaries.  This replaces the find_next_wrap
	 * loop — one Pango shaping pass instead of N. */
	{
		xtext_line_display *disp;
		PangoLayout *layout;
		xtext_format_data fd;
		PangoAttrList *attrs;
		int text_start = 0;
		int text_len;
		int avail;
		int n_lines, i;
		GSList *sublines_rev = NULL;
		int cum_stripped;

		/* Determine layout text range and available width.
		 * For nick entries: layout covers message text only (after nick),
		 * wrapped at buf->indent width (continuation indent).
		 * For no-nick entries: layout covers full text, wrapped at
		 * ent->indent width (same indent for all lines). */
		if (buf->xtext->auto_indent && ent->left_len > 0 && ent->raw_to_stripped_map)
		{
			text_start = xtext_raw_to_stripped (ent->raw_to_stripped_map,
			                                    ent->str_len, ent->left_len + 1);
			avail = win_width - buf->indent;
		}
		else
		{
			text_start = 0;
			avail = win_width - ent->indent;
		}
		if (avail < 1)
			avail = 1;

		text_len = ent->stripped_len - text_start;
		if (text_len <= 0)
		{
			ent->sublines = g_slist_append (NULL, GINT_TO_POINTER (ent->str_len));
			ent->sublines_width = buf->window_width;
			ent->display_lines = 1 + ent->extra_lines_above
				+ ent->extra_lines_below + (ent->collapsible ? 1 : 0);
			return 1;
		}

		/* Create layout with full message text and wrapping */
		layout = pango_layout_new (gtk_widget_get_pango_context (GTK_WIDGET (buf->xtext)));
		pango_layout_set_font_description (layout, buf->xtext->font->font);
		pango_layout_set_text (layout, (char *)(ent->stripped_str + text_start), text_len);

		fd = xtext_fdata_from_entry (ent);
		attrs = xtext_build_attrlist (&fd, text_start, text_len,
		                               buf->xtext->palette, buf->xtext->fontsize,
		                               buf->xtext->font->ascent);
		pango_layout_set_attributes (layout, attrs);
		pango_attr_list_unref (attrs);

		pango_layout_set_width (layout, avail * PANGO_SCALE);
		pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);

		/* Read back line boundaries and convert to raw byte offsets.
		 * Skip trailing empty layout lines — Pango creates one for text
		 * ending with \n or at exact width boundaries.  The render loop's
		 * str < str+str_len check won't render it, causing a display_lines
		 * overcount that produces blank space at the bottom. */
		n_lines = pango_layout_get_line_count (layout);
		if (n_lines > 1)
		{
			PangoLayoutLine *last = pango_layout_get_line_readonly (layout, n_lines - 1);
			if (last->length == 0)
				n_lines--;
		}
		cum_stripped = text_start;

		for (i = 0; i < n_lines; i++)
		{
			PangoLayoutLine *pline = pango_layout_get_line_readonly (layout, i);
			int raw_off;

			cum_stripped += pline->length;

			/* Skip past hard newlines so next subline starts at content */
			while (cum_stripped < ent->stripped_len &&
			       ent->stripped_str[cum_stripped] == '\n')
				cum_stripped++;

			/* Convert stripped offset to raw offset for sublines list */
			if (cum_stripped >= ent->stripped_len)
				raw_off = ent->str_len;
			else
				raw_off = xtext_stripped_to_raw (ent->raw_to_stripped_map,
				                                 ent->str_len, cum_stripped);

			sublines_rev = g_slist_prepend (sublines_rev, GINT_TO_POINTER (raw_off));
		}

		ent->sublines = g_slist_reverse (sublines_rev);
		ent->sublines_width = buf->window_width;
		ent_update_display_lines (ent);

		/* Store layout in display cache for render reuse */
		disp = g_new0 (xtext_line_display, 1);
		disp->entry_id = ent->entry_id;
		disp->cached_width = buf->window_width;
		disp->display_lines = ent->display_lines;
		disp->sublines = g_slist_copy (ent->sublines);
		disp->layout = layout;	/* takes ownership */
		disp->layout_text_offset = text_start;
		disp->layout_n_lines = n_lines;

		display_cache_put (buf->display_cache, buf, disp);

		return ent->display_lines;
	}
}

/* Recompute day boundary flags for all entries in a buffer.
 * Called when the hex_gui_day_separator preference changes. */

void
gtk_xtext_recalc_day_boundaries (xtext_buffer *buf)
{
	textentry *ent;

	for (ent = buf->text_first; ent; ent = ent->next)
	{
		gboolean was = (ent->flags & TEXTENTRY_FLAG_DAY_BOUNDARY) != 0;
		gboolean should = prefs.hex_gui_day_separator && ent->prev &&
		                   ent->stamp > 0 && ent->prev->stamp > 0 &&
		                   xtext_is_different_day (ent->prev->stamp, ent->stamp);

		if (should && !was)
		{
			ent->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
			ent->extra_lines_above++;
		}
		else if (!should && was)
		{
			ent->flags &= ~TEXTENTRY_FLAG_DAY_BOUNDARY;
			ent->extra_lines_above--;
		}
	}

	gtk_xtext_calc_lines (buf, TRUE);
}

/* Calculate number of actual lines (with wraps), to set adj->lower. *
 * This should only be called when the window resizes.               */

static void
gtk_xtext_calc_lines_virtual_ex (xtext_buffer *buf, int fire_signal,
                                  gboolean recompute_sublines)
{
	textentry *ent;
	int lines = 0;
	int count = 0;

	/* Walk materialized entries.
	 * recompute_sublines = TRUE on resize: invalidate stale entries but
	 * keep their cached display_lines as an estimate (GTK4-style lazy
	 * validation — actual Pango recompute deferred to render/access time).
	 * recompute_sublines = FALSE during scroll: just sum cached values. */
	for (ent = buf->text_first; ent; ent = ent->next)
	{
		if (recompute_sublines && ent->sublines_width != buf->window_width)
		{
			int old_dl = ent->display_lines;

			/* Never computed (sublines_width=0): must do full Pango */
			if (ent->sublines_width == 0 || !ent->sublines)
			{
				gtk_xtext_lines_taken (buf, ent);
			}
			else
			{
				int win_width = buf->window_width - MARGIN;

				/* Single-liners that still fit: update stamp, no Pango */
				if (!ent->sublines->next &&
				    win_width >= ent->indent + ent->str_width &&
				    !(ent->stripped_str && memchr (ent->stripped_str, '\n', ent->stripped_len)))
				{
					ent->sublines_width = buf->window_width;
					ent->display_lines = 1 + ent->extra_lines_above
						+ ent->extra_lines_below + (ent->collapsible ? 1 : 0);
				}
				/* Multi-liners: reflow now via lines_taken so display_lines
				 * and tree weight match the current width before we set
				 * adj->upper at the end of this pass.  The previous defer-
				 * to-render approach left num_lines summed from stale
				 * estimates, which made adj->upper too small (sometimes
				 * <= page_size — scrollbar hid the thumb entirely) until
				 * the user scrolled each entry into view and the render
				 * path's lazy reflow ran.  Note: resize takes
				 * recompute_sublines=FALSE and skips this branch entirely,
				 * so the resize-fast-path is unaffected. */
				else
				{
					display_cache_remove (buf->display_cache, ent->entry_id);
					gtk_xtext_lines_taken (buf, ent);
				}
			}

			/* Keep tree weight in sync whenever display_lines changes */
			if (ent->display_lines != old_dl && buf->entry_tree)
				update_weight234 (buf->entry_tree, ent, ent->display_lines - old_dl);
		}
		lines += ent->display_lines;
		count++;
	}

	/* lines_mat and mat_count are now derived from the tree via
	 * BUF_LINES_MAT / BUF_MAT_COUNT macros. The loop's 'lines' and 'count'
	 * are used below for avg computation and should match tree values. */

	/* Update running average */
	if (count > 0)
	{
		double new_avg = (double)lines / count;
		if (buf->avg_lines_per_entry <= 0)
			buf->avg_lines_per_entry = new_avg;
		else
			buf->avg_lines_per_entry = 0.9 * buf->avg_lines_per_entry + 0.1 * new_avg;
	}

	/* DB-authoritative: total_entries comes from the database, not
	 * incremental counting.  This eliminates drift from INSERT OR IGNORE
	 * rejecting duplicates while insert functions no longer increment. */
	if (HAS_VIRT_DB (buf) && buf->virt_channel)
	{
		int db_total = scrollback_count (buf->virt_db, buf->virt_channel);
		buf->total_entries = db_total;
	}

	/* Clamp lines_before_mat */
	if (buf->mat_first_index == 0)
		buf->lines_before_mat = 0;
	if (buf->lines_before_mat < 0)
		buf->lines_before_mat = 0;
	{
		int db_mat = BUF_MAT_COUNT (buf) - buf->ephemeral_count;
		int entries_after = buf->total_entries - buf->mat_first_index - db_mat;
		int lines_after;
		if (entries_after < 0)
			entries_after = 0;
		lines_after = (entries_after == 0) ? 0
			: (int)(entries_after * buf->avg_lines_per_entry);
		buf->num_lines = LINES_BEFORE_MAT (buf) + BUF_LINES_MAT (buf) + lines_after;
	}

	if (buf->num_lines < 1)
		buf->num_lines = 1;

	gtk_xtext_adjustment_set (buf, fire_signal);
}

static void
gtk_xtext_calc_lines_virtual (xtext_buffer *buf, int fire_signal)
{
	gtk_xtext_calc_lines_virtual_ex (buf, fire_signal, TRUE);
}

void
gtk_xtext_calc_lines (xtext_buffer *buf, int fire_signal)
{
	/* All buffers use the lazy-reflow path (GTK4-style).  Block
	 * value-changed during recompute to prevent adjustment_changed
	 * from clearing anchor_to_bottom (the old value < new upper after
	 * Pango remeasurement increases num_lines). */
	gboolean was_down = buf->scroll_anchor.anchor_to_bottom;
	if (buf->xtext->vc_signal_tag)
		g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
	gtk_xtext_calc_lines_virtual (buf, fire_signal);
	if (buf->xtext->vc_signal_tag)
		g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
	if (was_down)
	{
		buf->scroll_anchor.anchor_to_bottom = TRUE;
	}
}

/* find the n-th line in the linked list, this includes wrap calculations */

static textentry *
gtk_xtext_nth (GtkXText *xtext, int line, int *subline)
{
	textentry *ent;
	int offset;

	/* Adjust line to be relative to the materialized window.
	 * For non-DB buffers lines_before_mat is 0, making this a no-op. */
	{
		int lm = BUF_LINES_MAT (xtext->buffer);
		line -= LINES_BEFORE_MAT (xtext->buffer);
		if (line < 0)
			line = 0;
		if (lm > 0 && line >= lm)
			line = lm - 1;
	}

	/* O(log n) lookup via the counted B-tree */
	if (xtext->buffer->entry_tree)
	{
		ent = (textentry *)index234_by_weight (xtext->buffer->entry_tree,
		                                       line, &offset);
		if (ent)
		{
			*subline = offset;
			return ent;
		}
	}

	/* Fallback: linear walk (shouldn't happen if tree is maintained) */
	{
		int lines = 0;
		ent = xtext->buffer->text_first;
		while (ent)
		{
			lines += ent->display_lines;
			if (lines > line)
			{
				*subline = ent->display_lines - (lines - line);
				return ent;
			}
			ent = ent->next;
		}
	}
	return NULL;
}

/* render enta (or an inclusive range enta->entb) */

static int
gtk_xtext_render_ents (GtkXText * xtext, textentry * enta, textentry * entb)
{
	textentry *ent, *orig_ent, *tmp_ent;
	int line;
	int lines_max;
	int width;
	int height;
	int subline;
	int drawing = FALSE;
	gboolean created_cr = FALSE;
	int result;

	if (xtext->buffer->indent < MARGIN)
		xtext->buffer->indent = MARGIN;	  /* 2 pixels is our left margin */

	/* GTK4: Can't render outside snapshot - if no cr, just return and rely on queue_draw */
	if (xtext->cr == NULL)
		return 0;
	height = gtk_widget_get_height (GTK_WIDGET (xtext));
	width = xtext->buffer->window_width;
	width -= MARGIN;

	if (width < 32 || height < xtext->fontsize || width < xtext->buffer->indent + 30)
	{
		if (created_cr)
		{
			cairo_destroy (xtext->cr);
			xtext->cr = NULL;
		}
		return 0;
	}

	lines_max = ((height + xtext->pixel_offset) / xtext->fontsize) + 1;
	line = 0;
	orig_ent = xtext->buffer->pagetop_ent;
	subline = xtext->buffer->pagetop_subline;

	/* used before a complete page is in buffer */
	if (orig_ent == NULL)
		orig_ent = xtext->buffer->text_first;

	/* check if enta is before the start of this page */
	if (entb)
	{
		tmp_ent = orig_ent;
		while (tmp_ent)
		{
			if (tmp_ent == enta)
				break;
			if (tmp_ent == entb)
			{
				drawing = TRUE;
				break;
			}
			tmp_ent = tmp_ent->next;
		}
	}

	ent = orig_ent;
	while (ent)
	{
		if (entb && ent == enta)
			drawing = TRUE;

		if (drawing || ent == entb || ent == enta)
		{
			gtk_xtext_reset (xtext, FALSE);
			/* Phase 4: state-based rendering */
			if (ent->state == XTEXT_STATE_REDACTED)
				xtext->col_fore = XTEXT_REDACTED_FG;
			if (ent->state == XTEXT_STATE_PENDING)
				xtext->render_alpha = 0.5;
			line += gtk_xtext_render_line (xtext, ent, line, lines_max,
													 subline, width);
			xtext->render_alpha = 1.0;
			subline = 0;
			xtext->jump_in_offset = 0;	/* jump_in_offset only for the 1st */
		} else
		{
			if (ent == orig_ent)
			{
				line -= subline;
				subline = 0;
			}
			line += ENT_DISPLAY_LINES (ent);
		}

		if (ent == entb)
			break;

		if (line >= lines_max)
			break;

		ent = ent->next;
	}

	/* space below last line */
	result = (xtext->fontsize * line) - xtext->pixel_offset;

	/* Clean up temporary cairo context if we created one */
	if (created_cr)
	{
		cairo_destroy (xtext->cr);
		xtext->cr = NULL;
	}

	return result;
}

/* render a whole page/window, starting from 'startline' */

static void
gtk_xtext_render_page (GtkXText * xtext)
{
	textentry *ent;
	int line;
	int lines_max;
	int width;
	int height;
	int subline;
	int startline = gtk_adjustment_get_value (xtext->adj);
	int pos, overlap;
#if XTEXT_DEBUG_SCROLL
	gint64 render_t0 = g_get_monotonic_time ();
#endif

	if(!gtk_widget_get_realized(GTK_WIDGET(xtext)))
	  return;

	/* GTK4: Can't render outside snapshot - if no cr, just return and rely on queue_draw */
	if (xtext->cr == NULL)
		return;
	width = xtext->buffer->window_width;
	height = gtk_widget_get_height (GTK_WIDGET (xtext));

	if (xtext->buffer->indent < MARGIN)
		xtext->buffer->indent = MARGIN;	  /* 2 pixels is our left margin */

	if (width < 34 || height < xtext->fontsize || width < xtext->buffer->indent + 32)
	{
		return;
	}

	line = 0;
	{
		/* Anchoring strategy:
		 *
		 *   Top-anchored:   value <= LINES_BEFORE_MAT (literal top of buffer)
		 *                   → render top-down from text_first, pixel_offset 0.
		 *                   Top edge is line-aligned; bottom may be partial.
		 *
		 *   Otherwise:      anchor the bottom edge of the viewport.  Pick the
		 *                   entry+subline at the intended viewport bottom and
		 *                   walk backward.  Bottom edge is pixel-perfect; top
		 *                   may be partial.  This includes the anchor_to_bottom
		 *                   case (where the bottom entry is text_last) and
		 *                   the free-scroll case (where it's whatever entry
		 *                   the bottom of the user's viewport currently
		 *                   intersects).
		 *
		 * The integer page_size in adjustment_set means line-aligned edges
		 * are exact; the partial-edge artifact only appears at the unanchored
		 * side. */
		int effective_height = height;
		gdouble adj_page = gtk_adjustment_get_page_size (xtext->adj);
		gboolean at_top;
		gboolean use_bottom_anchor;

		if (xtext->status_strip_visible)
			effective_height -= (xtext->fontsize * 2 / 3 + 4);

		/* "At top" is encoded by anchor_entry_id == 0 with no bottom pin.
		 * Fall back to checking adj->value for the very first render
		 * (before adjustment_changed has captured an anchor) and for
		 * paths that adjust adj->value without going through the
		 * value-changed signal handler. */
		at_top = (xtext->buffer->scroll_anchor.anchor_entry_id == 0
			&& !xtext->buffer->scroll_anchor.anchor_to_bottom)
			|| (gtk_adjustment_get_value (xtext->adj)
				<= (gdouble) LINES_BEFORE_MAT (xtext->buffer));
		use_bottom_anchor = !at_top
			&& xtext->buffer->text_last
			&& xtext->buffer->num_lines > adj_page;

		if (use_bottom_anchor)
		{
			int lines_needed, accumulated;
			int start_sub;
			textentry *start_ent;
			textentry *walk;
			gboolean from_text_last;

			lines_needed = (effective_height + xtext->fontsize - 1) / xtext->fontsize;
			from_text_last = xtext->buffer->scroll_anchor.anchor_to_bottom;

			if (from_text_last)
			{
				start_ent = xtext->buffer->text_last;
				start_sub = -1;  /* "all sublines of start_ent" */
			}
			else
			{
				/* Free-scroll: read the bottom-of-viewport entry directly
				 * from the persistent scroll_anchor.  This bypasses adj->value
				 * and gtk_xtext_nth, both of which can drift after a buffer
				 * mutation (the anchor entry_id is stable across mutations
				 * because it survives in the entries_by_id hash). */
				start_ent = NULL;
				start_sub = -1;
				if (xtext->buffer->scroll_anchor.anchor_entry_id != 0)
				{
					start_ent = gtk_xtext_find_by_id (xtext->buffer,
						xtext->buffer->scroll_anchor.anchor_entry_id);
					if (start_ent)
						start_sub =
							xtext->buffer->scroll_anchor.subline_offset;
				}
				if (!start_ent)
				{
					/* Anchor lost or never set — fall back to nth(value)
					 * resolution.  This also covers the very first render
					 * after a buffer switch when the anchor hasn't been
					 * captured yet. */
					int bot_line = (int) (gtk_adjustment_get_value (xtext->adj)
						+ adj_page - 1.0);
					start_ent = gtk_xtext_nth (xtext, bot_line, &start_sub);
					if (!start_ent)
					{
						start_ent = xtext->buffer->text_last;
						start_sub = -1;
					}
				}
			}

			accumulated = 0;
			ent = NULL;
			subline = 0;
			{
				int total_above_delta = 0;

				for (walk = start_ent; walk; walk = walk->prev)
				{
					int contrib;

					/* Reflow stale entries before using display_lines for positioning.
					 * Only entries in the backward walk (~1 page) are touched, not all
					 * materialized entries.  Without this, stale estimates cause the
					 * walk to overcount lines and start rendering too close to the
					 * anchor, leaving blank space at the bottom. */
					if (walk->sublines_width != xtext->buffer->window_width)
					{
						int old_dl = walk->display_lines;
						gtk_xtext_lines_taken (xtext->buffer, walk);
						if (walk->display_lines != old_dl && xtext->buffer->entry_tree)
						{
							int delta = walk->display_lines - old_dl;
							xtext->buffer->num_lines += delta;
							update_weight234 (xtext->buffer->entry_tree, walk, delta);
							/* Track only deltas from entries strictly ABOVE the
							 * anchor (start_ent).  Those grow lines_before_start_ent
							 * by delta and therefore push the anchor's absolute
							 * line position down; we'll compensate adj->value
							 * after the walk so the same content stays at the
							 * bottom of the viewport.  start_ent's own delta
							 * doesn't shift its anchor row, so it's excluded. */
							if (walk != start_ent)
								total_above_delta += delta;
						}
					}

					/* For the start entry in free-scroll mode, only sublines
					 * 0..start_sub are inside the viewport at this end; for all
					 * other entries, the full display_lines count contributes. */
					if (walk == start_ent && start_sub >= 0)
						contrib = start_sub + 1;
					else
						contrib = ENT_DISPLAY_LINES (walk);

					accumulated += contrib;
					if (accumulated >= lines_needed)
					{
						ent = walk;
						/* This entry's first visible subline is at the
						 * (accumulated - lines_needed)-th position within its
						 * contributing range, which (since contributing ranges
						 * always start at subline 0) is also the subline index. */
						subline = accumulated - lines_needed;
						break;
					}
				}

				/* If reflow grew (or shrank) entries above start_ent, the absolute
				 * line number of start_ent's bot row shifted by total_above_delta.
				 * Push adj->value/upper to match so the user's perceived viewport
				 * doesn't jump on the next scroll/render — without this, scrolling
				 * up into a previously-unrendered region (e.g. cert info) reflows
				 * long entries placeholder→actual, and the walk's pagetop lands
				 * closer to start_ent than the user expected. */
				if (total_above_delta != 0)
				{
					gdouble cur_value = gtk_adjustment_get_value (xtext->adj);
					gdouble new_value = cur_value + total_above_delta;
					gdouble page = gtk_adjustment_get_page_size (xtext->adj);
					gdouble new_upper = xtext->buffer->num_lines;

					if (new_value > new_upper - page)
						new_value = new_upper - page;
					if (new_value < 0)
						new_value = 0;

					g_signal_handler_block (xtext->adj, xtext->vc_signal_tag);
					gtk_adjustment_configure (xtext->adj, new_value, 0, new_upper, 1, page, page);
					g_signal_handler_unblock (xtext->adj, xtext->vc_signal_tag);
				}
			}

			if (ent)
				xtext->pixel_offset = lines_needed * xtext->fontsize - effective_height;
			else
			{
				/* Materialized entries don't fill the page even though num_lines
				 * says they should (lbm over-estimate).  Fall through to top-down. */
				goto top_down;
			}
		}
		else
		{
top_down:
			/* Top-anchored or short buffer: render from text_first.
			 * pixel_offset = 0 → top edge is line-aligned.  The persistent
			 * scroll_anchor is maintained by adjustment_changed; render_page
			 * is purely a reader. */
			xtext->pixel_offset = 0;
			subline = 0;
			ent = xtext->buffer->text_first;
			if (startline > 0)
				ent = gtk_xtext_nth (xtext, startline, &subline);
		}
	}

	xtext->buffer->pagetop_ent = ent;
	xtext->buffer->pagetop_subline = subline;

	if (xtext->buffer->num_lines <= gtk_adjustment_get_page_size (xtext->adj))
		dontscroll (xtext->buffer);

	pos = gtk_adjustment_get_value (xtext->adj) * xtext->fontsize;
	overlap = xtext->buffer->last_pixel_pos - pos;
	xtext->buffer->last_pixel_pos = pos;

	/* In GTK3/Cairo, we don't use the scroll optimization with gdk_draw_drawable.
	 * We simply redraw the entire visible area. This is less efficient but
	 * more compatible with Cairo's drawing model. Modern systems handle this well. */
	(void)overlap;

	width -= MARGIN;

	/* Reserve space for status strip at bottom when visible */
	{
		int text_height = height;
		if (xtext->status_strip_visible)
			text_height -= (xtext->fontsize * 2 / 3 + 4);
		lines_max = ((text_height + xtext->pixel_offset) / xtext->fontsize) + 1;
	}

	while (ent)
	{
		/* Lazy reflow: recompute stale sublines before rendering.
		 * calc_lines uses the lazy path for all buffers, so stale entries
		 * (multi-liners with old sublines_width) get reflowed here on demand. */
			if (ent->sublines_width != xtext->buffer->window_width)
			{
				int old_dl = ent->display_lines;
				gtk_xtext_lines_taken (xtext->buffer, ent);
					if (ent->display_lines != old_dl)
				{
					int delta = ent->display_lines - old_dl;
					xtext->buffer->num_lines += delta;
					if (xtext->buffer->entry_tree)
						update_weight234 (xtext->buffer->entry_tree, ent, delta);
				}
			}

			/* Mark this entry as recently rendered — LRU cache will evict
			 * cold entries' sublines to bound memory. */
			display_cache_touch (xtext->buffer->display_cache, xtext->buffer, ent->entry_id);

			gtk_xtext_reset (xtext, FALSE);
			/* Phase 4: state-based rendering */
			if (ent->state == XTEXT_STATE_REDACTED || ent->state == XTEXT_STATE_REDACTED_PROMPT)
				xtext->col_fore = XTEXT_REDACTED_FG;
			if (ent->state == XTEXT_STATE_PENDING)
				xtext->render_alpha = 0.5;
			line += gtk_xtext_render_line (xtext, ent, line, lines_max,
													 subline, width);
			xtext->render_alpha = 1.0;
			subline = 0;

			if (line >= lines_max)
				break;

			ent = ent->next;
	}

	/* Auto-collapse expanded entries that scrolled off screen */
	if (prefs.hex_gui_collapse_multiline)
	{
		textentry *check;
		gboolean changed = FALSE;
		int adj_val = (int)gtk_adjustment_get_value (xtext->adj);
		int adj_page = (int)gtk_adjustment_get_page_size (xtext->adj);
		int line_pos = 0;

		for (check = xtext->buffer->text_first; check; check = check->next)
		{
			int ent_lines = ENT_DISPLAY_LINES (check);
			if (check->collapsible && !check->collapsed)
			{
				/* Entry is expanded — check if entirely outside viewport */
				if (line_pos + ent_lines <= adj_val || line_pos >= adj_val + adj_page)
				{
					int old_dl = check->display_lines;
					check->collapsed = TRUE;
					ent_update_display_lines (check);
					if (xtext->buffer->entry_tree && check->display_lines != old_dl)
						update_weight234 (xtext->buffer->entry_tree, check,
						                  check->display_lines - old_dl);
					changed = TRUE;
				}
			}
			line_pos += ent_lines;
		}

		if (changed)
		{
			gtk_xtext_calc_lines (xtext->buffer, TRUE);
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		}
	}


	line = (xtext->fontsize * line) - xtext->pixel_offset;


	/* fill any space below the last line with our background GC */
	xtext_draw_bg (xtext, 0, line, width + MARGIN, height - line);

	/* draw the separator line */
	gtk_xtext_draw_sep (xtext, -1);

	/* draw the status strip at the bottom */
	if (xtext->status_strip_visible)
		gtk_xtext_draw_status_strip (xtext, width + MARGIN, height);

	/* draw toast overlays at the top */
	if (xtext->toast_count > 0)
		gtk_xtext_draw_toasts (xtext, width + MARGIN, height);

#if XTEXT_DEBUG_SCROLL
	XT_DBG ("[render_page] %lld us  mat=%d bottom=%d at_top=%d\n",
		(long long)(g_get_monotonic_time () - render_t0),
		BUF_MAT_COUNT (xtext->buffer),
		xtext->buffer->scroll_anchor.anchor_to_bottom,
		(int)(gtk_adjustment_get_value (xtext->adj)
			<= (gdouble) LINES_BEFORE_MAT (xtext->buffer)));
#endif
}

/* ---- Bottom status strip (two-zone layout) ---- */

static int
status_item_cmp_priority (const void *a, const void *b)
{
	const xtext_status_item *ia = a, *ib = b;
	return ia->priority - ib->priority;
}

static void
gtk_xtext_draw_status_strip (GtkXText *xtext, int width, int height)
{
	int strip_h = xtext->fontsize * 2 / 3 + 4;
	int y = height - strip_h;
	cairo_t *cr = xtext->cr;
	PangoLayout *layout;
	PangoRectangle logical;
	xtext_status_item *left_items[XTEXT_STATUS_MAX_ITEMS];
	xtext_status_item *right_items[XTEXT_STATUS_MAX_ITEMS];
	int left_count = 0, right_count = 0;
	int i, text_y, right_x, left_x, right_zone_left;
	const char *sep = " \xc2\xb7 ";  /* UTF-8 middle dot */

	if (!cr || xtext->status_item_count == 0)
		return;

	/* Partition items into left/right zones */
	for (i = 0; i < xtext->status_item_count; i++)
	{
		xtext_status_item *item = &xtext->status_items[i];
		if (item->priority >= XTEXT_STATUS_PRIORITY_RIGHT)
			right_items[right_count++] = item;
		else
			left_items[left_count++] = item;
	}

	/* Sort each zone by priority */
	if (left_count > 1)
		qsort (left_items, left_count, sizeof (left_items[0]), status_item_cmp_priority);
	if (right_count > 1)
		qsort (right_items, right_count, sizeof (right_items[0]), status_item_cmp_priority);

	cairo_save (cr);

	/* Background */
	gdk_cairo_set_source_rgba (cr, &xtext->palette[XTEXT_BG]);
	cairo_rectangle (cr, 0, y, width, strip_h);
	cairo_fill (cr);

	layout = pango_layout_new (gtk_widget_get_pango_context (GTK_WIDGET (xtext)));
	pango_layout_set_font_description (layout, xtext->font->font);

	cairo_set_source_rgba (cr,
	                       xtext->palette[XTEXT_FG].red,
	                       xtext->palette[XTEXT_FG].green,
	                       xtext->palette[XTEXT_FG].blue,
	                       0.6);

	/* Render right zone (right-aligned) */
	right_zone_left = width;
	if (right_count > 0)
	{
		GString *rstr = g_string_new (NULL);
		for (i = right_count - 1; i >= 0; i--)
		{
			if (rstr->len > 0)
				g_string_prepend (rstr, sep);
			g_string_prepend (rstr, right_items[i]->display_text);
		}
		pango_layout_set_text (layout, rstr->str, -1);
		pango_layout_get_pixel_extents (layout, NULL, &logical);
		right_x = width - logical.width - 6;
		if (right_x < 6)
			right_x = 6;
		right_zone_left = right_x;
		text_y = y + (strip_h - logical.height) / 2;
		cairo_move_to (cr, right_x, text_y);
		pango_cairo_show_layout (cr, layout);
		g_string_free (rstr, TRUE);
	}

	/* Render left zone (left-aligned, ellipsize if overlapping right) */
	if (left_count > 0)
	{
		GString *lstr = g_string_new (NULL);
		gboolean has_dismiss = FALSE;
		int dismiss_pad = 0;

		/* Check if any left item has a dismiss callback */
		for (i = 0; i < left_count; i++)
		{
			if (left_items[i]->dismiss_cb)
			{
				has_dismiss = TRUE;
				break;
			}
		}

		/* Reserve space for × button if needed */
		if (has_dismiss)
			dismiss_pad = strip_h + 4; /* square button + gap */

		for (i = 0; i < left_count; i++)
		{
			if (lstr->len > 0)
				g_string_append (lstr, sep);
			g_string_append (lstr, left_items[i]->display_text);
		}
		left_x = 6;
		pango_layout_set_text (layout, lstr->str, -1);
		/* Ellipsize if it would overlap the right zone (minus dismiss button space) */
		pango_layout_set_width (layout, (right_zone_left - left_x - 12 - dismiss_pad) * PANGO_SCALE);
		pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
		pango_layout_get_pixel_extents (layout, NULL, &logical);
		text_y = y + (strip_h - logical.height) / 2;
		cairo_move_to (cr, left_x, text_y);
		pango_cairo_show_layout (cr, layout);

		/* Draw × dismiss button for dismissable items */
		if (has_dismiss)
		{
			int btn_x = left_x + logical.width + 6;
			int btn_sz = strip_h - 2;
			PangoRectangle x_rect;

			/* × glyph */
			pango_layout_set_width (layout, -1);
			pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_NONE);
			pango_layout_set_text (layout, "\xc3\x97", -1);  /* × (U+00D7) */
			pango_layout_get_pixel_extents (layout, NULL, &x_rect);
			cairo_set_source_rgba (cr,
			                       xtext->palette[XTEXT_FG].red,
			                       xtext->palette[XTEXT_FG].green,
			                       xtext->palette[XTEXT_FG].blue,
			                       0.5);
			cairo_move_to (cr, btn_x + (btn_sz - x_rect.width) / 2,
			               y + (strip_h - x_rect.height) / 2);
			pango_cairo_show_layout (cr, layout);

			/* Store dismiss hit area for all dismissable left items */
			for (i = 0; i < left_count; i++)
			{
				if (left_items[i]->dismiss_cb)
				{
					left_items[i]->dismiss_x = btn_x;
					left_items[i]->dismiss_w = btn_sz;
				}
			}
		}

		pango_layout_set_width (layout, -1);
		pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_NONE);
		g_string_free (lstr, TRUE);
	}

	g_object_unref (layout);
	cairo_restore (cr);
}

static int
xtext_status_find (GtkXText *xtext, const char *key)
{
	int i;
	for (i = 0; i < xtext->status_item_count; i++)
	{
		if (strcmp (xtext->status_items[i].key, key) == 0)
			return i;
	}
	return -1;
}

static gboolean
xtext_status_expire_tick (gpointer data)
{
	GtkXText *xtext = data;
	gint64 now = g_get_monotonic_time ();
	gint64 next_expire = 0;
	int i;

	for (i = xtext->status_item_count - 1; i >= 0; i--)
	{
		xtext_status_item *item = &xtext->status_items[i];
		if (item->expire_at > 0 && item->expire_at <= now)
		{
			g_free (item->key);
			g_free (item->display_text);
			/* Shift remaining items down */
			if (i < xtext->status_item_count - 1)
				memmove (&xtext->status_items[i], &xtext->status_items[i + 1],
				         sizeof (xtext_status_item) * (xtext->status_item_count - 1 - i));
			xtext->status_item_count--;
		}
		else if (item->expire_at > 0)
		{
			if (next_expire == 0 || item->expire_at < next_expire)
				next_expire = item->expire_at;
		}
	}

	xtext->status_strip_visible = (xtext->status_item_count > 0);
	xtext->status_expire_timer = 0;

	if (next_expire > 0)
	{
		int ms = (int)((next_expire - now) / 1000);
		if (ms < 1) ms = 1;
		xtext->status_expire_timer = g_timeout_add (ms, xtext_status_expire_tick, xtext);
	}

	gtk_xtext_adjustment_set (xtext->buffer, TRUE);
	if (xtext->buffer->scroll_anchor.anchor_to_bottom)
		gtk_adjustment_set_value (xtext->adj,
			gtk_adjustment_get_upper (xtext->adj) -
			gtk_adjustment_get_page_size (xtext->adj));
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
	return G_SOURCE_REMOVE;
}

void
gtk_xtext_status_set (GtkXText *xtext, const char *key, const char *text,
                      int priority, int timeout_ms)
{
	int idx;
	xtext_status_item *item;

	if (!text)
	{
		gtk_xtext_status_remove (xtext, key);
		return;
	}

	idx = xtext_status_find (xtext, key);
	if (idx >= 0)
	{
		item = &xtext->status_items[idx];
		g_free (item->display_text);
		item->display_text = g_strdup (text);
		item->priority = priority;
	}
	else
	{
		if (xtext->status_item_count >= XTEXT_STATUS_MAX_ITEMS)
			return;  /* full */
		item = &xtext->status_items[xtext->status_item_count++];
		item->key = g_strdup (key);
		item->display_text = g_strdup (text);
		item->priority = priority;
	}

	if (timeout_ms > 0)
		item->expire_at = g_get_monotonic_time () + (gint64)timeout_ms * 1000;
	else
		item->expire_at = 0;

	{
		gboolean was_visible = xtext->status_strip_visible;
		xtext->status_strip_visible = (xtext->status_item_count > 0);

		/* Schedule expiry timer if needed */
		if (item->expire_at > 0 && xtext->status_expire_timer == 0)
		{
			xtext->status_expire_timer = g_timeout_add (timeout_ms,
			                                            xtext_status_expire_tick, xtext);
		}

		if (was_visible != xtext->status_strip_visible)
		{
			gtk_xtext_adjustment_set (xtext->buffer, TRUE);
			if (xtext->buffer->scroll_anchor.anchor_to_bottom)
				gtk_adjustment_set_value (xtext->adj,
					gtk_adjustment_get_upper (xtext->adj) -
					gtk_adjustment_get_page_size (xtext->adj));
		}
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
}

void
gtk_xtext_status_set_dismiss (GtkXText *xtext, const char *key,
                              void (*cb) (GtkXText *, const char *, gpointer),
                              gpointer userdata)
{
	int idx = xtext_status_find (xtext, key);
	if (idx >= 0)
	{
		xtext->status_items[idx].dismiss_cb = cb;
		xtext->status_items[idx].dismiss_userdata = userdata;
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
}

void
gtk_xtext_status_remove (GtkXText *xtext, const char *key)
{
	int idx = xtext_status_find (xtext, key);

	if (idx < 0)
		return;

	g_free (xtext->status_items[idx].key);
	g_free (xtext->status_items[idx].display_text);

	if (idx < xtext->status_item_count - 1)
		memmove (&xtext->status_items[idx], &xtext->status_items[idx + 1],
		         sizeof (xtext_status_item) * (xtext->status_item_count - 1 - idx));
	xtext->status_item_count--;
	{
		gboolean was_visible = xtext->status_strip_visible;
		xtext->status_strip_visible = (xtext->status_item_count > 0);
		if (was_visible != xtext->status_strip_visible)
		{
			gtk_xtext_adjustment_set (xtext->buffer, TRUE);
			if (xtext->buffer->scroll_anchor.anchor_to_bottom)
				gtk_adjustment_set_value (xtext->adj,
					gtk_adjustment_get_upper (xtext->adj) -
					gtk_adjustment_get_page_size (xtext->adj));
		}
	}
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

void
gtk_xtext_status_clear (GtkXText *xtext)
{
	int i;
	for (i = 0; i < xtext->status_item_count; i++)
	{
		g_free (xtext->status_items[i].key);
		g_free (xtext->status_items[i].display_text);
	}
	xtext->status_item_count = 0;
	xtext->status_strip_visible = FALSE;
}

/* ---- Top toast overlay notifications ---- */

#define TOAST_ENTER_US   (200 * 1000)    /* 200ms in microseconds */
#define TOAST_LINGER_US  (4000 * 1000)   /* 4000ms default */
#define TOAST_EXIT_US    (300 * 1000)    /* 300ms */
#define TOAST_ANIM_MS    16              /* ~60fps */
#define TOAST_PADDING    6
#define TOAST_GAP        2
#define TOAST_CORNER_R   4.0

static void
xtext_toast_free (xtext_toast *toast)
{
	g_free (toast->text);
	g_free (toast);
}

static void
xtext_draw_rounded_rect (cairo_t *cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path (cr);
	cairo_arc (cr, x + w - r, y + r, r, -G_PI_2, 0);
	cairo_arc (cr, x + w - r, y + h - r, r, 0, G_PI_2);
	cairo_arc (cr, x + r, y + h - r, r, G_PI_2, G_PI);
	cairo_arc (cr, x + r, y + r, r, G_PI, 3 * G_PI_2);
	cairo_close_path (cr);
}

static gboolean
gtk_xtext_toast_tick (gpointer data)
{
	GtkXText *xtext = data;
	gint64 now = g_get_monotonic_time ();
	int i;

	for (i = xtext->toast_count - 1; i >= 0; i--)
	{
		xtext_toast *t = xtext->toasts[i];
		gint64 elapsed = now - t->phase_start;
		double progress;

		switch (t->phase)
		{
		case TOAST_ENTERING:
			progress = (double)elapsed / TOAST_ENTER_US;
			if (progress >= 1.0)
			{
				progress = 1.0;
				t->phase = TOAST_VISIBLE;
				t->phase_start = now;
			}
			t->alpha = progress;
			t->y_offset = -(t->rendered_height + TOAST_PADDING * 2) * (1.0 - progress);
			break;

		case TOAST_VISIBLE:
			t->alpha = 1.0;
			t->y_offset = 0;
			if (!(t->flags & TOAST_FLAG_STICKY) &&
			    elapsed >= (gint64)t->linger_ms * 1000)
			{
				t->phase = TOAST_EXITING;
				t->phase_start = now;
			}
			break;

		case TOAST_EXITING:
			progress = (double)elapsed / TOAST_EXIT_US;
			if (progress >= 1.0)
			{
				xtext_toast_free (t);
				if (i < xtext->toast_count - 1)
					memmove (&xtext->toasts[i], &xtext->toasts[i + 1],
					         sizeof (xtext_toast *) * (xtext->toast_count - 1 - i));
				xtext->toast_count--;
				continue;
			}
			t->alpha = 1.0 - progress;
			t->y_offset = 0;
			break;
		}
	}

	if (xtext->toast_count > 0)
	{
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
		return G_SOURCE_CONTINUE;
	}

	xtext->toast_anim_timer = 0;
	return G_SOURCE_REMOVE;
}

/* Accent colors per toast type (R, G, B) */
static const double toast_accent_colors[][3] = {
	{ 0.40, 0.60, 0.80 },  /* INFO  - steel blue */
	{ 0.55, 0.75, 0.50 },  /* NICK  - muted green */
	{ 0.70, 0.55, 0.85 },  /* TOPIC - soft purple */
	{ 0.80, 0.65, 0.30 },  /* MODE  - amber */
	{ 0.50, 0.70, 0.70 },  /* JOIN  - teal */
	{ 0.85, 0.40, 0.40 },  /* ERROR   - muted red */
	{ 0.40, 0.75, 0.40 },  /* SUCCESS - green */
};

static void
gtk_xtext_draw_toasts (GtkXText *xtext, int width, int height)
{
	cairo_t *cr = xtext->cr;
	PangoLayout *layout;
	PangoRectangle logical;
	double draw_y = 6.0;
	int i;
	double bg_lum;
	int dark_theme;
	const double *accent;
	double bg_r, bg_g, bg_b;

	if (!cr || xtext->toast_count == 0)
		return;

	cairo_save (cr);

	/* Clip to widget bounds so entering toasts don't draw above */
	cairo_rectangle (cr, 0, 0, width, height);
	cairo_clip (cr);

	/* Detect dark vs light theme from background luminance */
	bg_lum = xtext->palette[XTEXT_BG].red * 0.299
	       + xtext->palette[XTEXT_BG].green * 0.587
	       + xtext->palette[XTEXT_BG].blue * 0.114;
	dark_theme = (bg_lum < 0.5);

	layout = pango_layout_new (gtk_widget_get_pango_context (GTK_WIDGET (xtext)));
	pango_layout_set_font_description (layout, xtext->font->font);

	for (i = 0; i < xtext->toast_count; i++)
	{
		xtext_toast *t = xtext->toasts[i];
		int type_idx = (t->type >= 0 && t->type <= TOAST_TYPE_SUCCESS) ? t->type : 0;
		int accent_w = 4;
		int box_x = MARGIN + 8;
		int box_w = width - 2 * (MARGIN + 8);
		int box_h = t->rendered_height + TOAST_PADDING * 2 + 2;
		double y = draw_y + t->y_offset;

		accent = toast_accent_colors[type_idx];

		/* Background: blend accent color into BG for a tinted, high-contrast fill */
		if (dark_theme)
		{
			/* Dark theme: lighten BG slightly, tint with accent */
			bg_r = xtext->palette[XTEXT_BG].red * 0.6 + accent[0] * 0.15 + 0.08;
			bg_g = xtext->palette[XTEXT_BG].green * 0.6 + accent[1] * 0.15 + 0.08;
			bg_b = xtext->palette[XTEXT_BG].blue * 0.6 + accent[2] * 0.15 + 0.08;
		}
		else
		{
			/* Light theme: darken BG slightly, tint with accent */
			bg_r = xtext->palette[XTEXT_BG].red * 0.85 + accent[0] * 0.08 - 0.05;
			bg_g = xtext->palette[XTEXT_BG].green * 0.85 + accent[1] * 0.08 - 0.05;
			bg_b = xtext->palette[XTEXT_BG].blue * 0.85 + accent[2] * 0.08 - 0.05;
		}
		bg_r = CLAMP(bg_r, 0.0, 1.0);
		bg_g = CLAMP(bg_g, 0.0, 1.0);
		bg_b = CLAMP(bg_b, 0.0, 1.0);

		/* Background rounded rect - fully opaque for contrast */
		xtext_draw_rounded_rect (cr, box_x, y, box_w, box_h, TOAST_CORNER_R);
		cairo_set_source_rgba (cr, bg_r, bg_g, bg_b, 0.95 * t->alpha);
		cairo_fill (cr);

		/* Left accent bar */
		xtext_draw_rounded_rect (cr, box_x, y, accent_w, box_h, 2.0);
		cairo_set_source_rgba (cr, accent[0], accent[1], accent[2], 0.9 * t->alpha);
		cairo_fill (cr);

		/* Border */
		xtext_draw_rounded_rect (cr, box_x, y, box_w, box_h, TOAST_CORNER_R);
		cairo_set_source_rgba (cr, accent[0], accent[1], accent[2], 0.3 * t->alpha);
		cairo_set_line_width (cr, 1.0);
		cairo_stroke (cr);

		/* Text - centered */
		pango_layout_set_text (layout, t->text, -1);
		pango_layout_set_width (layout, (box_w - accent_w - TOAST_PADDING * 2) * PANGO_SCALE);
		pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
		pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
		pango_layout_get_pixel_extents (layout, NULL, &logical);

		/* Position text vertically centered in the box.
		 * Pango's logical rect includes leading above the text, so nudge
		 * down to visually center the glyphs within the box. */
		cairo_move_to (cr, box_x + accent_w + TOAST_PADDING,
		               y + TOAST_PADDING + 3);
		cairo_set_source_rgba (cr,
		                       xtext->palette[XTEXT_FG].red,
		                       xtext->palette[XTEXT_FG].green,
		                       xtext->palette[XTEXT_FG].blue,
		                       t->alpha);
		pango_cairo_show_layout (cr, layout);

		draw_y += box_h + TOAST_GAP;
	}

	pango_layout_set_width (layout, -1);
	pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_NONE);
	pango_layout_set_alignment (layout, PANGO_ALIGN_LEFT);
	g_object_unref (layout);
	cairo_restore (cr);
}

void
gtk_xtext_toast_show (GtkXText *xtext, const char *text, int linger_ms,
                      xtext_toast_type type, unsigned int flags)
{
	xtext_toast *toast;
	PangoLayout *layout;
	PangoRectangle logical;

	/* If at max, force-remove oldest non-sticky toast */
	if (xtext->toast_count >= XTEXT_TOAST_MAX)
	{
		int victim = -1, j;
		for (j = 0; j < xtext->toast_count; j++)
		{
			if (!(xtext->toasts[j]->flags & TOAST_FLAG_STICKY))
			{
				victim = j;
				break;
			}
		}
		if (victim >= 0)
		{
			xtext_toast_free (xtext->toasts[victim]);
			if (victim < xtext->toast_count - 1)
				memmove (&xtext->toasts[victim], &xtext->toasts[victim + 1],
				         sizeof (xtext_toast *) * (xtext->toast_count - 1 - victim));
			xtext->toast_count--;
		}
		else
			return;  /* all slots are sticky, can't add more */
	}

	toast = g_new0 (xtext_toast, 1);
	toast->text = g_strdup (text);
	toast->phase = TOAST_ENTERING;
	toast->type = type;
	toast->flags = flags;
	toast->phase_start = g_get_monotonic_time ();
	toast->linger_ms = (linger_ms > 0) ? linger_ms : 4000;
	toast->alpha = 0.0;

	/* Measure text height */
	layout = pango_layout_new (gtk_widget_get_pango_context (GTK_WIDGET (xtext)));
	pango_layout_set_font_description (layout, xtext->font->font);
	pango_layout_set_text (layout, text, -1);
	pango_layout_get_pixel_extents (layout, NULL, &logical);
	toast->rendered_height = logical.height;
	toast->y_offset = -(logical.height + TOAST_PADDING * 2);
	g_object_unref (layout);

	xtext->toasts[xtext->toast_count++] = toast;

	/* Start animation timer if not running */
	if (xtext->toast_anim_timer == 0)
		xtext->toast_anim_timer = g_timeout_add (TOAST_ANIM_MS,
		                                         gtk_xtext_toast_tick, xtext);
}

void
gtk_xtext_toast_clear (GtkXText *xtext)
{
	int i;
	for (i = 0; i < xtext->toast_count; i++)
		xtext_toast_free (xtext->toasts[i]);
	xtext->toast_count = 0;

	if (xtext->toast_anim_timer)
	{
		g_source_remove (xtext->toast_anim_timer);
		xtext->toast_anim_timer = 0;
	}
}

void
gtk_xtext_refresh (GtkXText * xtext)
{
	if (gtk_widget_get_realized (GTK_WIDGET (xtext)))
	{
		/* In GTK3, queue a redraw instead of rendering directly.
		 * The draw signal handler will call gtk_xtext_render_page. */
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
}

static int
gtk_xtext_kill_ent (xtext_buffer *buffer, textentry *ent)
{
	int visible;

	/* Set visible to TRUE if this is the current buffer */
	/* and this ent shows up on the screen now */
	visible = buffer->xtext->buffer == buffer &&
				 gtk_xtext_check_ent_visibility (buffer->xtext, ent, 0);

	/* If the scroll anchor points to this entry, move to neighbor */
	if (buffer->scroll_anchor.anchor_entry_id == ent->entry_id)
	{
		if (ent->next)
		{
			buffer->scroll_anchor.anchor_entry_id = ent->next->entry_id;
			buffer->scroll_anchor.subline_offset = 0;
		}
		else if (ent->prev)
		{
			buffer->scroll_anchor.anchor_entry_id = ent->prev->entry_id;
			buffer->scroll_anchor.subline_offset = 0;
		}
		else
		{
			buffer->scroll_anchor.anchor_entry_id = 0;
			buffer->scroll_anchor.anchor_to_bottom = TRUE;
		}
	}
	if (ent == buffer->xtext->hover_ent)
		buffer->xtext->hover_ent = NULL;
	if (ent == buffer->xtext->hover_reply_target)
		buffer->xtext->hover_reply_target = NULL;
	if (ent == buffer->xtext->flash_ent)
		buffer->xtext->flash_ent = NULL;

	/* last_ent_start_id / last_ent_end_id: stale IDs self-heal (resolve to NULL) */

	if (buffer->marker_pos_id == ent->entry_id)
	{
		/* Allow for "Marker line reset because exceeded scrollback limit. to appear. */
		buffer->marker_pos_id = ent->next ? ent->next->entry_id : 0;
		buffer->marker_state = MARKER_RESET_BY_KILL;
	}

	if (ent->marks)
	{
		gtk_xtext_search_textentry_del (buffer, ent);
	}

	/* IRCv3 modernization: remove from hash tables (Phase 1) */
	if (ent->msgid && buffer->entries_by_msgid)
		g_hash_table_remove (buffer->entries_by_msgid, ent->msgid);
	if (buffer->entries_by_id)
		g_hash_table_remove (buffer->entries_by_id, GSIZE_TO_POINTER (ent->entry_id));

	/* Remove from display cache */
	display_cache_remove (buffer->display_cache, ent->entry_id);

	/* Remove from B-tree */
	if (buffer->entry_tree)
		del234 (buffer->entry_tree, ent);

	if (ent->flags & TEXTENTRY_FLAG_EPHEMERAL)
		buffer->ephemeral_count--;

	g_free (ent->msgid);

	/* Phase 4: free separate str and redaction info */
	if (ent->flags & TEXTENTRY_FLAG_SEPARATE_STR)
		g_free (ent->str);
	if (ent->redaction)
	{
		g_free (ent->redaction->original_content);
		g_free (ent->redaction->redacted_by);
		g_free (ent->redaction->redaction_reason);
		g_free (ent->redaction);
	}

	/* IRCv3 reactions and reply context */
	if (ent->reactions)
	{
		guint i;
		for (i = 0; i < ent->reactions->reactions->len; i++)
		{
			xtext_reaction *r = g_ptr_array_index (ent->reactions->reactions, i);
			g_free (r->text);
			g_hash_table_destroy (r->nicks);
			g_free (r);
		}
		g_ptr_array_free (ent->reactions->reactions, TRUE);
		g_free (ent->reactions);
	}
	if (ent->reply)
	{
		g_free (ent->reply->target_msgid);
		g_free (ent->reply->target_nick);
		g_free (ent->reply->target_preview);
		g_free (ent->reply);
	}

	g_slist_free_full (ent->slp, g_free);
	g_slist_free (ent->sublines);

	g_free (ent->stripped_str);
	g_free (ent->fmt_spans);
	g_free (ent->emoji_list);
	g_free (ent->raw_to_stripped_map);

	g_free (ent);
	return visible;
}

/* Remove the topline from the linked list.
 * In virtual mode (Phase 4), this only evicts from the materialized window —
 * the entry survives in the SQLite DB and can be rematerialized later.
 * max_lines thus limits the in-memory window, not total history. */

static void
gtk_xtext_remove_top (xtext_buffer *buffer)
{
	textentry *ent;

	ent = buffer->text_first;
	if (!ent)
		return;

	/* Ephemeral entries have no DB row and can't be rematerialized.
	 * Skip eviction — the entry stays pinned.  Callers use if-not-while
	 * so this won't loop. */
	if (ent->flags & TEXTENTRY_FLAG_EPHEMERAL)
		return;

	/* DB-backed: advance mat_first_index (entry survives in DB) */
	if (HAS_VIRT_DB (buffer))
	{
		buffer->mat_first_index++;
	}

	{
		int ent_lines = ENT_DISPLAY_LINES (ent);
		buffer->num_lines -= ent_lines;
		buffer->last_pixel_pos -= (ent_lines * buffer->xtext->fontsize);
		buffer->text_first = ent->next;
		if (buffer->text_first)
		{
			buffer->text_first->prev = NULL;
			/* Remove stale day boundary - first entry has no predecessor */
			if (buffer->text_first->flags & TEXTENTRY_FLAG_DAY_BOUNDARY)
			{
				buffer->text_first->flags &= ~TEXTENTRY_FLAG_DAY_BOUNDARY;
				buffer->text_first->extra_lines_above--;
				buffer->text_first->display_lines--;
				if (buffer->entry_tree)
					update_weight234 (buffer->entry_tree, buffer->text_first, -1);
				/* Keep num_lines in sync with tree weight. Without this
				 * decrement, num_lines is off by 1 (stale) until the
				 * next gtk_xtext_calc_lines, and adj->upper with it. */
				buffer->num_lines--;
				ent_lines++;  /* account for the removed boundary line */
			}
		}
		else
			buffer->text_last = NULL;

		if (buffer->xtext->buffer == buffer)	/* is it the current buffer? */
		{
			g_signal_handler_block (buffer->xtext->adj, buffer->xtext->vc_signal_tag);
			gtk_adjustment_set_value (buffer->xtext->adj,
				gtk_adjustment_get_value (buffer->xtext->adj) - ent_lines);
			g_signal_handler_unblock (buffer->xtext->adj, buffer->xtext->vc_signal_tag);
		}
	}

	if (gtk_xtext_kill_ent (buffer, ent))
	{
		if (!buffer->xtext->add_io_tag)
		{
			/* remove scrolling events */
			if (buffer->xtext->io_tag)
			{
				g_source_remove (buffer->xtext->io_tag);
				buffer->xtext->io_tag = 0;
			}
			buffer->xtext->force_render = TRUE;
			buffer->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
														(GSourceFunc)
														gtk_xtext_render_page_timeout,
														buffer->xtext);
		}
	}
}

static void
gtk_xtext_remove_bottom (xtext_buffer *buffer)
{
	textentry *ent;

	ent = buffer->text_last;
	if (!ent)
		return;

	/* Ephemeral entries are pinned — can't be rematerialized from DB */
	if (ent->flags & TEXTENTRY_FLAG_EPHEMERAL)
		return;
	{
		int ent_lines = ENT_DISPLAY_LINES (ent);
		buffer->num_lines -= ent_lines;
	}
	buffer->text_last = ent->prev;
	if (buffer->text_last)
		buffer->text_last->next = NULL;
	else
		buffer->text_first = NULL;

	if (gtk_xtext_kill_ent (buffer, ent))
	{
		if (!buffer->xtext->add_io_tag)
		{
			/* remove scrolling events */
			if (buffer->xtext->io_tag)
			{
				g_source_remove (buffer->xtext->io_tag);
				buffer->xtext->io_tag = 0;
			}
			buffer->xtext->force_render = TRUE;
			buffer->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
														(GSourceFunc)
														gtk_xtext_render_page_timeout,
														buffer->xtext);
		}
	}
}

/* If lines=0 => clear all */

void
gtk_xtext_clear (xtext_buffer *buf, int lines)
{
	textentry *next;
	int marker_reset = FALSE;

	if (lines != 0)
	{
		if (lines < 0)
		{
			/* delete lines from bottom */
			lines *= -1;
			while (lines)
			{
				if (buf->text_last && buf->text_last->entry_id == buf->marker_pos_id)
					marker_reset = TRUE;
				gtk_xtext_remove_bottom (buf);
				lines--;
			}
		}
		else
		{
			/* delete lines from top */
			while (lines)
			{
				if (buf->text_first && buf->text_first->entry_id == buf->marker_pos_id)
					marker_reset = TRUE;
				gtk_xtext_remove_top (buf);
				lines--;
			}
		}
	}
	else
	{
		/* delete all */
		if (buf->search_found)
			gtk_xtext_search_fini (buf);
		if (buf->xtext->auto_indent)
			buf->indent = MARGIN;
		buf->scroll_anchor.anchor_to_bottom = TRUE;
		buf->scroll_anchor.anchor_entry_id = 0;
		buf->last_ent_start_id = 0;
		buf->last_ent_end_id = 0;
		buf->marker_pos_id = 0;
		if (buf->text_first)
			marker_reset = TRUE;
		dontscroll (buf);

		while (buf->text_first)
		{
			next = buf->text_first->next;
			g_free (buf->text_first);
			buf->text_first = next;
		}
		buf->text_last = NULL;

		/* Recreate the B-tree (old one has stale pointers) */
		if (buf->entry_tree)
		{
			freetree234 (buf->entry_tree);
			buf->entry_tree = newtree234_weighted (entry_stamp_cmp, entry_weight_cb);
		}

		/* DB-backed: clear the database and reset virtual counters */
		if (buf->virt_db && buf->virt_channel)
		{
			scrollback_clear (buf->virt_db, buf->virt_channel);
			buf->total_entries = 0;
			buf->mat_first_index = 0;
			buf->lines_before_mat = 0;
			buf->ephemeral_count = 0;
		}
	}

	if (buf->xtext->buffer == buf)
	{
		gtk_xtext_calc_lines (buf, TRUE);
		gtk_xtext_refresh (buf->xtext);
	} else
	{
		gtk_xtext_calc_lines (buf, FALSE);
	}

	if (marker_reset)
		buf->marker_state = MARKER_RESET_BY_CLEAR;
}

static gboolean
gtk_xtext_check_ent_visibility (GtkXText * xtext, textentry *find_ent, int add)
{
	textentry *ent;
	int lines;
	xtext_buffer *buf = xtext->buffer;
	int height;

	if (find_ent == NULL)
	{
		return FALSE;
	}

	height = gtk_widget_get_height (GTK_WIDGET (xtext));

	ent = buf->pagetop_ent;
	/* If top line not completely displayed return FALSE */
	if (ent == find_ent && buf->pagetop_subline > 0)
	{
		return FALSE;
	}
	/* Loop through line positions looking for find_ent */
	lines = ((height + xtext->pixel_offset) / xtext->fontsize) + buf->pagetop_subline + add;
	while (ent)
	{
		lines -= ENT_DISPLAY_LINES (ent);
		if (lines <= 0)
		{
			return FALSE;
		}
		if (ent == find_ent)
		{
			return TRUE;
		}
		ent = ent->next;
	}

	return FALSE;
}

void
gtk_xtext_check_marker_visibility (GtkXText * xtext)
{
	if (gtk_xtext_check_ent_visibility (xtext, xtext_resolve_marker (xtext->buffer), 1))
		xtext->buffer->marker_seen = TRUE;
}

static void
gtk_xtext_unstrip_color (gint start, gint end, GSList *slp, GList **gl, gint maxo)
{
	gint off1, off2, curlen;
	GSList *cursl;
	offsets_t marks;
	offlen_t *meta;

	off1 = 0;
	curlen = 0;
	cursl = slp;
	while (cursl)
	{
		meta = cursl->data;
		if (start < meta->len)
		{
			off1 = meta->off + start;
			break;
		}
		curlen += meta->len;
		start -= meta->len;
		end -= meta->len;
		cursl = g_slist_next (cursl);
	}

	off2 = off1;
	while (cursl)
	{
		meta = cursl->data;
		if (end < meta->len)
		{
			off2 = meta->off + end;
			break;
		}
		curlen += meta->len;
		end -= meta->len;
		cursl = g_slist_next (cursl);
	}
	if (!cursl)
	{
		off2 = maxo;
	}

	marks.o.start = off1;
	marks.o.end = off2;
	*gl = g_list_append (*gl, GUINT_TO_POINTER (marks.u));
}

/* Search a single textentry for occurrence(s) of search arg string */
static GList *
gtk_xtext_search_textentry (xtext_buffer *buf, textentry *ent)
{
	gchar *str;								/* text string to be searched */
	GList *gl = NULL;
	GSList *slp;
	gint lstr;

	if (buf->search_text == NULL)
	{
		return gl;
	}

	str = gtk_xtext_strip_color (ent->str, ent->str_len, buf->xtext->scratch_buffer,
										  &lstr, &slp, !buf->xtext->ignore_hidden);

	/* Regular-expression matching --- */
	if (buf->search_flags & regexp)
	{
		GMatchInfo *gmi;
		gint start, end;

		if (buf->search_re == NULL)
		{
			return gl;
		}
		g_regex_match (buf->search_re, str, 0, &gmi);
		while (g_match_info_matches (gmi))
		{
			g_match_info_fetch_pos (gmi, 0,  &start, &end);
			gtk_xtext_unstrip_color (start, end, slp, &gl, ent->str_len);
			g_match_info_next (gmi, NULL);
		}
		g_match_info_free (gmi);

	/* Non-regular-expression matching --- */
	} else {
		gchar *hay, *pos;
		gint lhay, off, len;
		gint match = buf->search_flags & case_match;

		hay = match? g_strdup (str): g_utf8_casefold (str, lstr);
		lhay = strlen (hay);

		for (pos = hay, len = lhay; len;
			  off += buf->search_lnee, pos = hay + off, len = lhay - off)
		{
			str = g_strstr_len (pos, len, buf->search_nee);
			if (str == NULL)
			{
				break;
			}
			off = str - hay;
			gtk_xtext_unstrip_color (off, off + buf->search_lnee,
											 slp, &gl, ent->str_len);
		}

		g_free (hay);
	}

	/* Common processing --- */
	g_slist_free_full (slp, g_free);
	return gl;
}

/* Add a list of found search results to an entry, maybe NULL */
static void
gtk_xtext_search_textentry_add (xtext_buffer *buf, textentry *ent, GList *gl, gboolean pre)
{
	ent->marks = gl;
	if (gl)
	{
		buf->search_found = (pre? g_list_prepend: g_list_append) (buf->search_found, ent);
		if (pre == FALSE && buf->hintsearch_id == 0)
		{
			buf->hintsearch_id = ent->entry_id;
		}
	}
}

/* Free all search information for a textentry */
static void
gtk_xtext_search_textentry_del (xtext_buffer *buf, textentry *ent)
{
	g_list_free (ent->marks);
	ent->marks = NULL;
	if (buf->cursearch && buf->cursearch->data == ent)
	{
		buf->cursearch = NULL;
		buf->curmark = NULL;
		buf->curdata.u = 0;
	}
	if (buf->hintsearch_id == ent->entry_id)
	{
		buf->hintsearch_id = 0;
	}
	buf->search_found = g_list_remove (buf->search_found, ent);
}

/* Used only by glist_foreach */
static void
gtk_xtext_search_textentry_fini (gpointer entp, gpointer dummy)
{
	textentry *ent = entp;

	g_list_free (ent->marks);
	ent->marks = NULL;
}

static GList *gtk_xtext_search_raw_text (xtext_buffer *out, const char *text, int len);

/* Free all search information for all textentrys and the xtext_buffer */
static void
gtk_xtext_search_fini (xtext_buffer *buf)
{
	g_list_foreach (buf->search_found, gtk_xtext_search_textentry_fini, 0);
	g_list_free (buf->search_found);
	buf->search_found = NULL;
	g_free (buf->search_text);
	buf->search_text = NULL;
	g_free (buf->search_nee);
	buf->search_nee = NULL;
	buf->search_flags = 0;
	buf->cursearch = NULL;
	buf->curmark = NULL;
	/* but leave buf->curdata.u alone! */
	if (buf->search_re)
	{
		g_regex_unref (buf->search_re);
		buf->search_re = NULL;
	}
	if (buf->search_virt_ids)
	{
		g_array_free (buf->search_virt_ids, TRUE);
		buf->search_virt_ids = NULL;
	}
	buf->search_virt_pos = -1;
}

/* Returns TRUE if the base search information exists and is still okay to use */
static gboolean
gtk_xtext_search_init (xtext_buffer *buf, const gchar *text, gtk_xtext_search_flags flags, GError **perr)
{
	/* Of the five flags, backward and highlight_all do not need a new search */
	if (buf->search_found &&
		 strcmp (buf->search_text, text) == 0 &&
		 (buf->search_flags & case_match) == (flags & case_match) &&
		 (buf->search_flags & follow) == (flags & follow) &&
		 (buf->search_flags & regexp) == (flags & regexp))
	{
		return TRUE;
	}
	buf->hintsearch_id = buf->cursearch? ((textentry *)buf->cursearch->data)->entry_id: 0;
	gtk_xtext_search_fini (buf);
	buf->search_text = g_strdup (text);
	if (flags & regexp)
	{
		buf->search_re = g_regex_new (text, (flags & case_match)? 0: G_REGEX_CASELESS, 0, perr);
		if (perr && *perr)
		{
			return FALSE;
		}
	}
	else
	{
		if (flags & case_match)
		{
			buf->search_nee = g_strdup (text);
		}
		else
		{
			buf->search_nee = g_utf8_casefold (text, strlen (text));
		}
		buf->search_lnee = strlen (buf->search_nee);
	}
	buf->search_flags = flags;
	buf->cursearch = NULL;
	buf->curmark = NULL;
	/* but leave buf->curdata.u alone! */
	return FALSE;
}

/* Virtual mode: build ordered array of DB rowids matching the search.
 * Also marks any currently-materialized matches for highlight_all. */
static void
gtk_xtext_search_virt_scan (xtext_buffer *buf)
{
	int total, offset, batch;
	textentry *ent;

	if (buf->search_virt_ids)
		g_array_free (buf->search_virt_ids, TRUE);
	buf->search_virt_ids = g_array_new (FALSE, FALSE, sizeof (gint64));
	buf->search_virt_pos = -1;

	total = scrollback_count (buf->virt_db, buf->virt_channel);
	buf->search_virt_db_total = buf->total_entries;
	offset = 0;
	batch = 500;

	while (offset < total)
	{
		GSList *msgs = scrollback_load_range (buf->virt_db, buf->virt_channel,
		                                      offset, batch);
		GSList *iter;

		for (iter = msgs; iter; iter = iter->next)
		{
			scrollback_msg *msg = iter->data;
			GList *gl;

			if (!msg->text)
				continue;

			gl = gtk_xtext_search_raw_text (buf, msg->text, strlen (msg->text));
			if (gl)
			{
				g_array_append_val (buf->search_virt_ids, msg->id);
				g_list_free (gl);  /* marks stored per-rowid, not per-textentry */
			}
		}
		offset += batch;
		scrollback_msg_list_free (msgs);
	}

	/* Also mark materialized entries for highlight_all rendering */
	for (ent = buf->text_first; ent; ent = ent->next)
	{
		GList *gl = gtk_xtext_search_textentry (buf, ent);
		gtk_xtext_search_textentry_add (buf, ent, gl, TRUE);
	}
	buf->search_found = g_list_reverse (buf->search_found);
}

/* Virtual mode: navigate to the entry at search_virt_pos.
 * Materializes the region around the target and scrolls to it. */
static textentry *
gtk_xtext_search_virt_navigate (GtkXText *xtext, gtk_xtext_search_flags flags)
{
	xtext_buffer *buf = xtext->buffer;
	gint64 rowid;
	int idx, radius;
	textentry *ent;
	GList *gl;

	if (!buf->search_virt_ids || buf->search_virt_ids->len == 0)
		return NULL;

	if (buf->search_virt_pos < 0 ||
	    buf->search_virt_pos >= (int)buf->search_virt_ids->len)
		return NULL;

	rowid = g_array_index (buf->search_virt_ids, gint64, buf->search_virt_pos);

	/* Ensure the target entry is materialized */
	idx = scrollback_get_index_of_rowid (buf->virt_db, buf->virt_channel, rowid);
	radius = VIRT_PAGE_SIZE;
	if (radius < 100)
		radius = 100;
	gtk_xtext_virt_ensure_range (buf, idx, radius);

	/* Find the now-materialized entry */
	ent = gtk_xtext_find_by_id (buf, (guint64)rowid);
	if (!ent)
		return NULL;

	/* Apply search marks to this entry */
	if (!ent->marks)
	{
		gl = gtk_xtext_search_textentry (buf, ent);
		if (gl)
			gtk_xtext_search_textentry_add (buf, ent, gl, FALSE);
	}

	/* Set curmark so the renderer can identify the focused result.
	 * This drives both the non-highlight-all single-match display
	 * and the visual distinction (underline + full opacity) in
	 * highlight-all mode. */
	buf->curmark = ent->marks ? g_list_first (ent->marks) : NULL;
	buf->curdata.u = buf->curmark ? GPOINTER_TO_UINT (buf->curmark->data) : 0;

	/* Scroll to the entry: compute its absolute line position */
	{
		GtkAdjustment *adj = xtext->adj;
		float value;
		textentry *walk;

		value = LINES_BEFORE_MAT (buf);
		for (walk = buf->text_first; walk && walk != ent; walk = walk->next)
			value += ENT_DISPLAY_LINES (walk);

		if (value > gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj))
			value = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
		else if ((flags & backward) && walk)
		{
			value -= gtk_adjustment_get_page_size (adj) - ENT_DISPLAY_LINES (walk);
			if (value < 0)
				value = 0;
		}
		gtk_adjustment_set_value (adj, value);
	}

	buf->hintsearch_id = ent->entry_id;
	return ent;
}

#define BACKWARD (flags & backward)
#define FIRSTLAST(lp)  (BACKWARD? g_list_last(lp): g_list_first(lp))
#define NEXTPREVIOUS(lp) (BACKWARD? g_list_previous(lp): g_list_next(lp))
textentry *
gtk_xtext_search (GtkXText * xtext, const gchar *text, gtk_xtext_search_flags flags, GError **perr)
{
	textentry *ent = NULL;
	xtext_buffer *buf = xtext->buffer;
	GList *gl;

	if (buf->text_first == NULL && !HAS_VIRT_DB (buf))
	{
		return NULL;
	}

	/* If the text arg is NULL, one of these has been toggled: highlight follow */
	if (text == NULL)		/* Here on highlight or follow toggle */
	{
		gint oldfollow = buf->search_flags & follow;
		gint newfollow = flags & follow;

		/* If "Follow" has just been checked, search possible new textentries --- */
		if (newfollow && (newfollow != oldfollow))
		{
			gl = g_list_last (buf->search_found);
			ent = gl? gl->data: buf->text_first;
			for (; ent; ent = ent->next)
			{
				GList *gl;

				gl = gtk_xtext_search_textentry (buf, ent);
				gtk_xtext_search_textentry_add (buf, ent, gl, FALSE);
			}
		}
		buf->search_flags = flags;
		ent = buf->pagetop_ent;
	}

	/* if the text arg is "", the reset button has been clicked or Control-Shift-F has been hit */
	else if (text[0] == 0)		/* Let a null string do a reset. */
	{
		gtk_xtext_search_fini (buf);
	}

	/* If the text arg is neither NULL nor "", it's the search string */
	else
	{
		if (gtk_xtext_search_init (buf, text, flags, perr) == FALSE)	/* If a new search: */
		{
			if (perr && *perr)
			{
				return NULL;
			}

			/* Virtual mode: search entire DB */
			if (HAS_VIRT_DB (buf) && buf->virt_channel)
			{
				gtk_xtext_search_virt_scan (buf);
			}
			else
			{
				for (ent = buf->text_first; ent; ent = ent->next)
				{
					GList *gl;

					gl = gtk_xtext_search_textentry (buf, ent);
					gtk_xtext_search_textentry_add (buf, ent, gl, TRUE);
				}
				buf->search_found = g_list_reverse (buf->search_found);
			}
		}

		/* Virtual mode: navigate via rowid array */
		if (buf->search_virt_ids && buf->search_virt_ids->len > 0)
		{
			/* Re-scan if DB has grown since last scan (new history arrived).
			 * Restore current position in the rebuilt array so forward/back
			 * from the current result works correctly. */
			if (buf->total_entries != buf->search_virt_db_total)
			{
				guint64 current_id = buf->hintsearch_id;
				gtk_xtext_search_virt_scan (buf);

				buf->search_virt_pos = -1;
				if (current_id && buf->search_virt_ids)
				{
					int i;
					for (i = 0; i < (int)buf->search_virt_ids->len; i++)
					{
						if (g_array_index (buf->search_virt_ids, gint64, i) == (gint64)current_id)
						{
							buf->search_virt_pos = i;
							break;
						}
					}
				}
			}

			if (buf->search_virt_pos < 0)
			{
				/* Fresh search: start from first or last */
				buf->search_virt_pos = BACKWARD
					? (int)buf->search_virt_ids->len - 1 : 0;
			}
			else
			{
				/* Advance to next/prev result */
				buf->search_virt_pos += BACKWARD ? -1 : 1;
			}

			if (buf->search_virt_pos < 0 ||
			    buf->search_virt_pos >= (int)buf->search_virt_ids->len)
			{
				buf->search_virt_pos = -1;
				ent = NULL;
			}
			else
			{
				ent = gtk_xtext_search_virt_navigate (xtext, flags);
			}

			gtk_widget_queue_draw (GTK_WIDGET (xtext));
			return ent;
		}

		/* Non-virtual navigation (existing code) */
		if (buf->search_found)
		{
			/* If we're in the midst of moving among found items */
			if (buf->cursearch)
			{
				ent = buf->cursearch->data;
				buf->curmark = NEXTPREVIOUS (buf->curmark);
				if (buf->curmark == NULL)
				{
					/* We've returned all the matches for this textentry. */
					buf->cursearch = NEXTPREVIOUS (buf->cursearch);
					if (buf->cursearch)
					{
						ent = buf->cursearch->data;
						buf->curmark = FIRSTLAST (ent->marks);
					}
					else	/* We've returned all the matches for all textentries */
					{
						ent = NULL;
					}
				}
			}

			/* If user changed the search, let's look starting where he was */
			else if (buf->hintsearch_id)
			{
				GList *mark;
				offsets_t last, this;
				/*
				 * If we already have a 'current' item from the last search, and if
				 * the first character of an occurrence on this line for this new search
				 * is within that former item, use the occurrence as current.
				 */
				ent = gtk_xtext_find_by_id (buf, buf->hintsearch_id);
				last.u = buf->curdata.u;
				for (mark = ent->marks; mark; mark = mark->next)
				{
					this.u = GPOINTER_TO_UINT (mark->data);
					if (this.o.start >= last.o.start && this.o.start < last.o.end)
					break;
				}
				if (mark == NULL)
				{
					for (ent = gtk_xtext_find_by_id (buf, buf->hintsearch_id); ent; ent = BACKWARD? ent->prev: ent->next)
						if (ent->marks)
							break;
					mark = ent? FIRSTLAST (ent->marks): NULL;
				}
				buf->cursearch = g_list_find (buf->search_found, ent);
				buf->curmark = mark;
			}

			/* This is a fresh search */
			else
			{
				buf->cursearch = FIRSTLAST (buf->search_found);
				ent = buf->cursearch->data;
				buf->curmark = FIRSTLAST (ent->marks);
			}
			buf->curdata.u = (buf->curmark)? GPOINTER_TO_UINT (buf->curmark->data): 0;
		}
	}
	buf->hintsearch_id = ent ? ent->entry_id : 0;

	if (!gtk_xtext_check_ent_visibility (xtext, ent, 1))
	{
		GtkAdjustment *adj = xtext->adj;
		float value;
		textentry *hint = ent;

		value = LINES_BEFORE_MAT (buf);
		for (ent = buf->text_first;
			  ent && ent != hint; ent = ent->next)
		{
			value += ENT_DISPLAY_LINES (ent);
		}
		if (value > gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj))
		{
			value = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
		}
		else if ((flags & backward)  && ent)
		{
			value -= gtk_adjustment_get_page_size (adj) - ENT_DISPLAY_LINES (ent);
			if (value < 0)
			{
				value = 0;
			}
		}
		gtk_adjustment_set_value (adj, value);
	}

	gtk_widget_queue_draw (GTK_WIDGET (xtext));

	return gtk_xtext_find_by_id (buf, buf->hintsearch_id);
}
#undef BACKWARD
#undef FIRSTLAST
#undef NEXTPREVIOUS

static int
gtk_xtext_render_page_timeout (GtkXText * xtext)
{
	GtkAdjustment *adj = xtext->adj;

	xtext->add_io_tag = 0;

	/* less than a complete page? */
	if (xtext->buffer->num_lines <= gtk_adjustment_get_page_size (adj))
	{
		gtk_adjustment_set_value (adj, 0);
		/* GTK3: Queue a redraw instead of rendering directly */
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	} else if (xtext->buffer->scroll_anchor.anchor_to_bottom)
	{
		g_signal_handler_block (xtext->adj, xtext->vc_signal_tag);
		gtk_xtext_adjustment_set (xtext->buffer, FALSE);
		gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));
		g_signal_handler_unblock (xtext->adj, xtext->vc_signal_tag);
		xtext->buffer->scroll_anchor.anchor_to_bottom = TRUE;
		/* GTK3: Queue a redraw instead of rendering directly */
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	} else
	{
		gtk_xtext_adjustment_set (xtext->buffer, TRUE);
		if (xtext->force_render)
		{
			xtext->force_render = FALSE;
			/* GTK3: Queue a redraw instead of rendering directly */
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		}
	}

	return 0;
}

/* Shared entry initialization — tab→space, stamp, widths, format parsing,
 * entry ID, state/flags.  Caller must have already set ent->str, ent->str_len,
 * ent->left_len, and ent->indent.  Does NOT touch ent->next/prev (linkage). */

static void
gtk_xtext_init_entry (xtext_buffer *buf, textentry *ent, time_t stamp)
{
	int i;

	/* we don't like tabs */
	for (i = 0; i < ent->str_len; i++)
	{
		if (ent->str[i] == '\t')
			ent->str[i] = ' ';
	}

	ent->stamp = stamp;
	if (stamp == 0)
		ent->stamp = time (0);
	ent->slp = NULL;
	ent->str_width = gtk_xtext_text_width_ent (buf->xtext, ent);
	ent->mark_start = -1;
	ent->mark_end = -1;
	ent->marks = NULL;

	/* Parse format spans, stripped text, emoji placeholders */
	{
		unsigned char *stripped;
		int stripped_len, span_count, emoji_count;
		xtext_fmt_span *spans;
		xtext_emoji_info *emojis;
		guint16 *r2s;

		xtext_parse_formats (ent->str, ent->str_len,
		                     &stripped, &stripped_len,
		                     &spans, &span_count,
		                     &emojis, &emoji_count,
		                     &r2s,
		                     buf->xtext->emoji_cache != NULL);
		ent->stripped_str = stripped;
		ent->stripped_len = (guint16) stripped_len;
		ent->fmt_spans = spans;
		ent->fmt_span_count = (guint16) span_count;
		ent->emoji_list = emojis;
		ent->emoji_count = (guint16) emoji_count;
		ent->raw_to_stripped_map = r2s;
	}

	/* Entry identification: use DB rowid when available so
	 * eviction + rematerialization preserves the same ID.
	 *
	 * Two disjoint ID spaces:
	 *   [1 .. LOCAL_ENTRY_ID_BASE-1]   — DB rowids (assigned by SQLite)
	 *   [LOCAL_ENTRY_ID_BASE .. ∞]     — local-only IDs (no DB row)
	 *
	 * The split eliminates collisions between locally-allocated IDs
	 * (e.g. wrapped lines of a multiline replay message, or system
	 * messages printed before scrollback_save runs) and future DB
	 * rowids.  Previously, next_entry_id started at 1 and incremented
	 * through the same space SQLite was about to use; multiline
	 * replay batches consumed IDs that the next replay iteration's
	 * rowid would also claim, producing duplicate (stamp, entry_id)
	 * keys that add234 silently dropped, leaving entries in the
	 * linked list but absent from the tree.
	 *
	 * 2^62 leaves the full int64 positive range usable by SQLite
	 * while reserving the top quarter for local IDs (~4.6e18 entries
	 * before exhaustion — effectively unbounded). */
	ent->msgid = NULL;
	{
		gboolean has_db_row = (buf->pending_db_rowid > 0);
		if (has_db_row)
		{
			ent->entry_id = (guint64) buf->pending_db_rowid;
			buf->pending_db_rowid = 0;
		}
		else
		{
			if (buf->next_entry_id < LOCAL_ENTRY_ID_BASE)
				buf->next_entry_id = LOCAL_ENTRY_ID_BASE;
			ent->entry_id = buf->next_entry_id++;
		}
		ent->has_db_row = has_db_row ? 1 : 0;
	}
	ent->group_id = buf->current_group_id;
	g_hash_table_insert (buf->entries_by_id, GSIZE_TO_POINTER (ent->entry_id), ent);

	/* Entry modification support */
	ent->state = XTEXT_STATE_NORMAL;
	if (!ent->has_db_row && HAS_VIRT_DB (buf))
	{
		ent->flags = TEXTENTRY_FLAG_EPHEMERAL;
		buf->ephemeral_count++;
	}
	else
	{
		ent->flags = 0;
	}
	ent->redaction = NULL;

	if (ent->indent < MARGIN)
		ent->indent = MARGIN;	  /* 2 pixels is the left margin */
}

/* Link an initialized entry into the buffer's doubly-linked list, compute
 * day boundaries, sublines, auto-collapse, and add to B-tree.
 * Returns the number of display lines added (for scroll adjustment). */

static int
gtk_xtext_link_entry (xtext_buffer *buf, textentry *ent, xtext_link_position pos)
{
	int new_lines;

	/* Linked list insertion */
	switch (pos)
	{
	case LINK_TAIL:
		if (buf->text_last)
			buf->text_last->next = ent;
		else
			buf->text_first = ent;
		ent->prev = buf->text_last;
		buf->text_last = ent;
		break;
	case LINK_HEAD:
		if (buf->text_first)
			buf->text_first->prev = ent;
		else
			buf->text_last = ent;
		ent->next = buf->text_first;
		buf->text_first = ent;
		break;
	case LINK_BEFORE:
		/* Caller has set ent->prev and ent->next; splice into list */
		if (ent->prev)
			ent->prev->next = ent;
		else
			buf->text_first = ent;
		if (ent->next)
			ent->next->prev = ent;
		else
			buf->text_last = ent;
		break;
	}

	/* Day boundary: check ent vs its predecessor */
	if (prefs.hex_gui_day_separator && ent->prev && ent->stamp > 0 &&
	    ent->prev->stamp > 0 && xtext_is_different_day (ent->prev->stamp, ent->stamp))
	{
		ent->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
		ent->extra_lines_above++;
	}

	/* Day boundary: update next entry's boundary based on new predecessor */
	if (prefs.hex_gui_day_separator && ent->next && ent->next->stamp > 0 && ent->stamp > 0)
	{
		gboolean was_boundary = (ent->next->flags & TEXTENTRY_FLAG_DAY_BOUNDARY) != 0;
		gboolean is_boundary = xtext_is_different_day (ent->stamp, ent->next->stamp);
		if (is_boundary && !was_boundary)
		{
			ent->next->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
			ent->next->extra_lines_above++;
			ent->next->display_lines++;
			if (buf->entry_tree)
				update_weight234 (buf->entry_tree, ent->next, 1);
			buf->num_lines++;
		}
		else if (!is_boundary && was_boundary)
		{
			ent->next->flags &= ~TEXTENTRY_FLAG_DAY_BOUNDARY;
			ent->next->extra_lines_above--;
			ent->next->display_lines--;
			if (buf->entry_tree)
				update_weight234 (buf->entry_tree, ent->next, -1);
			buf->num_lines--;
		}
	}

	/* Subline computation */
	ent->sublines = NULL;
	if (buf->window_width > 0)
	{
		gtk_xtext_lines_taken (buf, ent);

		/* Auto-collapse multiline messages exceeding threshold */
		if (prefs.hex_gui_collapse_multiline && ent->group_id != 0 && ent->sublines)
		{
			int sublines = g_slist_length (ent->sublines);
			int threshold = prefs.hex_gui_collapse_threshold;
			gboolean should_collapse = (sublines > threshold);

			/* Page-adaptive threshold */
			if (!should_collapse && prefs.hex_gui_collapse_page_divisor > 0 && buf->xtext)
			{
				int page_size = gtk_widget_get_height (GTK_WIDGET (buf->xtext)) / buf->xtext->fontsize;
				int adaptive = page_size / prefs.hex_gui_collapse_page_divisor;
				if (adaptive > 0 && sublines > adaptive)
					should_collapse = TRUE;
			}

			if (should_collapse)
			{
				ent->collapsible = TRUE;
				ent->collapsed = TRUE;
				ent_update_display_lines (ent);
			}
		}

		new_lines = ENT_DISPLAY_LINES (ent);
	}
	else
	{
		/* Widget not yet sized — placeholder count, real wrap deferred */
		ent->display_lines = 1 + ent->extra_lines_above;
		new_lines = ent->display_lines;
	}
	buf->num_lines += new_lines;

	/* Add to B-tree for O(log n) positional access */
	if (buf->entry_tree)
		add234 (buf->entry_tree, ent);

	return new_lines;
}

/* append a textentry to our linked list */

static void
gtk_xtext_append_entry (xtext_buffer *buf, textentry * ent, time_t stamp)
{
	gtk_xtext_init_entry (buf, ent, stamp);

	/* Out-of-order arrival: if this entry's (stamp, id) sorts before
	 * text_last, link it at the chronologically-correct position rather
	 * than the tail.  The B-tree (sorted by stamp+id) places entries by
	 * the comparator, but a tail-link puts a chronologically-older entry
	 * after newer-stamped peers in the linked list.  render_page walks
	 * back via prev pointers (list order) while gtk_xtext_nth resolves
	 * via tree order — when the two diverge, pagetop is computed against
	 * a different set of entries than nth picked, and the user sees a
	 * scroll-up jump.  Common trigger: IRCv3 server-time tags that drift
	 * from local time during connect (cert-info events vs server numerics).
	 *
	 * Fast-path stays O(1) for monotonic stamps (the typical case).
	 * Out-of-order walks back from text_last; usually only a few entries
	 * since drift is small. */
	if (buf->text_last && entry_stamp_cmp (ent, buf->text_last) < 0)
	{
		textentry *pos = buf->text_last;
		while (pos && entry_stamp_cmp (ent, pos) < 0)
			pos = pos->prev;
		if (pos == NULL)
		{
			ent->prev = NULL;
			ent->next = buf->text_first;
		}
		else
		{
			ent->prev = pos;
			ent->next = pos->next;
		}
		gtk_xtext_link_entry (buf, ent, LINK_BEFORE);
	}
	else
	{
		gtk_xtext_link_entry (buf, ent, LINK_TAIL);
	}

	/* Track DB-row count.  Only entries actually written to the DB
	 * contribute — ephemerals (system messages, dedup-rejected duplicates)
	 * have no DB row, so they must not bump total_entries. */
	if (HAS_VIRT_DB (buf) && ent->has_db_row)
		buf->total_entries++;

	/* Local auto-advance: move marker to newest unread message when window is
	 * unfocused.  Suppressed when server controls the marker (draft/read-marker). */
	if (!buf->server_read_marker &&
	    (buf->marker_pos_id == 0 || buf->marker_seen) &&
	    (buf->xtext->buffer != buf ||
	     !gtk_window_is_active (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (buf->xtext))))))
	{
		buf->marker_pos_id = ent->entry_id;
		buf->marker_state = MARKER_IS_SET;
		dontscroll (buf); /* force scrolling off */
		buf->marker_seen = FALSE;
	}

	/* In virtual mode, use the materialization window size for pruning.
	 * max_lines is the hard ceiling (for large selections); VIRT_MAT_WINDOW
	 * is the normal working set.  ensure_range handles scrolling eviction. */
	if (buf->xtext->max_lines > 2)
	{
		int limit = HAS_VIRT_DB (buf) ? VIRT_MAT_WINDOW : buf->xtext->max_lines;
		if (BUF_MAT_COUNT (buf) > limit || (!HAS_VIRT_DB (buf) && buf->num_lines > limit))
			gtk_xtext_remove_top (buf);
	}

	if (buf->xtext->buffer == buf)
	{
		/* this could be improved */
		if ((buf->num_lines - 1) <= gtk_adjustment_get_page_size (buf->xtext->adj))
			dontscroll (buf);

		if (!buf->xtext->add_io_tag)
		{
			/* remove scrolling events */
			if (buf->xtext->io_tag)
			{
				g_source_remove (buf->xtext->io_tag);
				buf->xtext->io_tag = 0;
			}
			buf->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
															(GSourceFunc)
															gtk_xtext_render_page_timeout,
															buf->xtext);
		}
	}
	if (buf->search_flags & follow)
	{
		GList *gl;

		gl = gtk_xtext_search_textentry (buf, ent);
		gtk_xtext_search_textentry_add (buf, ent, gl, FALSE);
	}
}

/* IRCv3 modernization: prepend entry at head (Phase 3)
 * Used for chathistory BEFORE requests - adds older messages at the start.
 * Key differences from append:
 * - Inserts at head of linked list
 * - Adjusts scroll position to preserve user's view
 * - Does NOT update marker_pos (historical entries)
 * - Prunes from bottom if exceeding max_lines
 */
static void
gtk_xtext_prepend_entry (xtext_buffer *buf, textentry *ent, time_t stamp)
{
	int new_lines;

	gtk_xtext_init_entry (buf, ent, stamp);
	new_lines = gtk_xtext_link_entry (buf, ent, LINK_HEAD);

	/* DB-backed bookkeeping — only entries with a real DB row count */
	if (HAS_VIRT_DB (buf))
	{
		if (ent->has_db_row)
			buf->total_entries++;
		if (buf->mat_first_index > 0)
			buf->mat_first_index--;
	}

	/* Adjust scroll position - we added lines at the top, so everything shifts down */
	if (buf->xtext && buf->xtext->buffer == buf)
	{
		/* Update adjustment value to keep view stable */
		g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
		gtk_adjustment_set_value (buf->xtext->adj,
			gtk_adjustment_get_value (buf->xtext->adj) + new_lines);
		g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
	}

	/* Don't update marker_pos for historical entries - they're old */
	/* marker_pos should stay where it was */

	/* Prune if exceeding limit */
	if (buf->xtext->max_lines > 2)
	{
		int limit = HAS_VIRT_DB (buf) ? VIRT_MAT_WINDOW : buf->xtext->max_lines;
		if (BUF_MAT_COUNT (buf) > limit || (!HAS_VIRT_DB (buf) && buf->num_lines > limit))
			gtk_xtext_remove_bottom (buf);
	}

	/* Schedule render if this buffer is active */
	if (buf->xtext->buffer == buf)
	{
		if (!buf->xtext->add_io_tag)
		{
			if (buf->xtext->io_tag)
			{
				g_source_remove (buf->xtext->io_tag);
				buf->xtext->io_tag = 0;
			}
			buf->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
													(GSourceFunc)
													gtk_xtext_render_page_timeout,
													buf->xtext);
		}
	}

	/* Handle search follow mode */
	if (buf->search_flags & follow)
	{
		GList *gl;

		gl = gtk_xtext_search_textentry (buf, ent);
		gtk_xtext_search_textentry_add (buf, ent, gl, FALSE);
	}
}

/* IRCv3 modernization: insert entry sorted by timestamp (Phase 3)
 * Used for chathistory gap filling - inserts at correct chronological position.
 * Walks the list to find insertion point (O(n) - acceptable for gap filling).
 * For bulk operations, use prepend/append which are O(1).
 */
static void
gtk_xtext_insert_sorted_entry (xtext_buffer *buf, textentry *ent, time_t stamp)
{
	int new_lines;
	int lines_before_insert = 0;
	textentry *pos;
	gboolean inserted_before_pagetop = FALSE;

	gtk_xtext_init_entry (buf, ent, stamp);

	/* Find insertion point - walk forward to find first entry with stamp > ent->stamp.
	 * When insert_hint is set (sorted batch), start from the hint to avoid O(N²). */
	if (buf->insert_hint && buf->insert_hint->stamp <= ent->stamp)
	{
		lines_before_insert = buf->insert_hint_lines;
		pos = buf->insert_hint->next;
	}
	else
	{
		pos = buf->text_first;
	}
	while (pos && pos->stamp <= ent->stamp)
	{
		if (pos->sublines)
			lines_before_insert += g_slist_length (pos->sublines);
		else
			lines_before_insert += 1;
		pos = pos->next;
	}

	/* Set prev/next for insertion point, then link into list */
	if (pos == NULL)
	{
		/* Insert at end */
		ent->prev = buf->text_last;
		ent->next = NULL;
	}
	else if (pos == buf->text_first)
	{
		/* Insert at head */
		ent->prev = NULL;
		ent->next = buf->text_first;
		inserted_before_pagetop = TRUE;
	}
	else
	{
		/* Insert in middle */
		ent->prev = pos->prev;
		ent->next = pos;
	}

	new_lines = gtk_xtext_link_entry (buf, ent, LINK_BEFORE);

	/* Check if we inserted before the current view (middle case) */
	if (!inserted_before_pagetop && pos != NULL && buf->pagetop_ent)
	{
		textentry *check = buf->text_first;
		while (check && check != buf->pagetop_ent)
		{
			if (check == ent)
			{
				inserted_before_pagetop = TRUE;
				break;
			}
			check = check->next;
		}
	}

	/* Update insert hint for next sorted insert in this batch */
	if (buf->batch_mode)
	{
		buf->insert_hint = ent;
		buf->insert_hint_lines = lines_before_insert + new_lines;
	}

	/* DB-backed bookkeeping — only entries with a real DB row count */
	if (HAS_VIRT_DB (buf))
	{
		if (ent->has_db_row)
			buf->total_entries++;

		/* If inserted at head, the materialized window shifted down */
		if (ent == buf->text_first)
			buf->mat_first_index = (buf->mat_first_index > 0) ? buf->mat_first_index - 1 : 0;

		/* Enforce materialization window for non-batch inserts.
		 * During batch_mode, pruning is deferred to fe_set_batch_mode. */
		if (!buf->batch_mode && buf->xtext->max_lines > 2 &&
		    BUF_MAT_COUNT (buf) > VIRT_MAT_WINDOW)
		{
			gtk_xtext_remove_top (buf);
		}
	}

	/* Adjust scroll position if we inserted before current view */
	if (inserted_before_pagetop)
	{
		if (buf->xtext && buf->xtext->buffer == buf && !buf->batch_mode)
		{
			g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
			gtk_adjustment_set_value (buf->xtext->adj,
				gtk_adjustment_get_value (buf->xtext->adj) + new_lines);
			g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
		}
	}

	/* Don't update marker_pos for historical entries */

	/* Keep scroll anchored to bottom if appropriate */
	if (!buf->scroll_anchor.anchor_to_bottom
	    && !inserted_before_pagetop && buf->xtext && buf->xtext->buffer == buf
	         && !buf->batch_mode)
	{
		/* Entry inserted after viewport (e.g., chathistory catch-up while
		 * scrolled up).  Extend the scrollbar range without moving the
		 * viewport to prevent flicker from stale adj upper. */
		g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
		gtk_adjustment_set_upper (buf->xtext->adj, buf->num_lines);
		g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
	}

	/* Schedule render if this buffer is active.
	 * Skip during batch_mode — caller will trigger one render when done. */
	if (buf->xtext->buffer == buf && !buf->batch_mode)
	{
		if (!buf->xtext->add_io_tag)
		{
			if (buf->xtext->io_tag)
			{
				g_source_remove (buf->xtext->io_tag);
				buf->xtext->io_tag = 0;
			}
			buf->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
													(GSourceFunc)
													gtk_xtext_render_page_timeout,
													buf->xtext);
		}
	}

	/* Handle search follow mode */
	if (buf->search_flags & follow)
	{
		GList *gl;

		gl = gtk_xtext_search_textentry (buf, ent);
		gtk_xtext_search_textentry_add (buf, ent, gl, FALSE);
	}

}

/* the main two public functions */

void
gtk_xtext_append_indent (xtext_buffer *buf,
								 unsigned char *left_text, int left_len,
								 unsigned char *right_text, int right_len,
								 time_t stamp)
{
	textentry *ent;
	unsigned char *str;
	int space;
	int tempindent;
	int left_width;

	if (left_len == -1)
		left_len = strlen (left_text);

	if (right_len == -1)
		right_len = strlen (right_text);

	if (left_len + right_len + 2 >= sizeof (buf->xtext->scratch_buffer))
		right_len = sizeof (buf->xtext->scratch_buffer) - left_len - 2;

	if (right_text[right_len-1] == '\n')
		right_len--;

	ent = g_malloc0 (left_len + right_len + 2 + sizeof (textentry));
	str = (unsigned char *) ent + sizeof (textentry);

	if (left_len)
		memcpy (str, left_text, left_len);
	str[left_len] = ' ';
	if (right_len)
		memcpy (str + left_len + 1, right_text, right_len);
	str[left_len + 1 + right_len] = 0;

	left_width = gtk_xtext_text_width (buf->xtext, left_text, left_len);

	ent->left_len = left_len;
	ent->str = str;
	ent->str_len = left_len + 1 + right_len;
	/* This is copied into the scratch buffer later, double check math */
	g_assert (ent->str_len < sizeof (buf->xtext->scratch_buffer));

	if (buf->xtext->auto_indent)
	{
		int cap = gtk_xtext_auto_indent_cap (buf);

		ent->indent = (buf->indent - left_width) - buf->xtext->space_width;

		if (buf->time_stamp)
			space = buf->xtext->stamp_width;
		else
			space = 0;

		/* do we need to auto adjust the separator position? */
		if (buf->indent < cap && ent->indent < MARGIN + space)
		{
			tempindent = MARGIN + space + buf->xtext->space_width + left_width;

			if (tempindent > buf->indent)
				buf->indent = tempindent;

			if (buf->indent > cap)
				buf->indent = cap;

			gtk_xtext_fix_indent (buf);
			gtk_xtext_recalc_widths (buf, FALSE);

			ent->indent = (buf->indent - left_width) - buf->xtext->space_width;
			buf->xtext->force_render = TRUE;
		}
	}
	else
	{
		ent->indent = buf->time_stamp ? buf->xtext->stamp_width : MARGIN;
	}

	gtk_xtext_append_entry (buf, ent, stamp);
}

void
gtk_xtext_append (xtext_buffer *buf, unsigned char *text, int len, time_t stamp)
{
	textentry *ent;
	gboolean truncate = FALSE;

	if (len == -1)
		len = strlen (text);

	if (text[len-1] == '\n')
		len--;

	if (len >= sizeof (buf->xtext->scratch_buffer))
	{
		len = sizeof (buf->xtext->scratch_buffer) - 1;
		truncate = TRUE;
	}

	ent = g_malloc0 (len + 1 + sizeof (textentry));
	ent->str = (unsigned char *) ent + sizeof (textentry);
	ent->str_len = len;
	if (len)
	{
		if (!truncate)
		{
			memcpy (ent->str, text, len);
			ent->str[len] = '\0';
		}
		else
		{
			safe_strcpy (ent->str, text, sizeof (buf->xtext->scratch_buffer));
			ent->str_len = strlen (ent->str);
		}
	}
	ent->indent = 0;
	ent->left_len = -1;

	gtk_xtext_append_entry (buf, ent, stamp);
}

/* IRCv3 modernization: prepend functions for chathistory (Phase 3)
 * These insert at the head of the buffer for older messages.
 */

void
gtk_xtext_prepend_indent (xtext_buffer *buf,
								 unsigned char *left_text, int left_len,
								 unsigned char *right_text, int right_len,
								 time_t stamp)
{
	textentry *ent;
	unsigned char *str;
	int space;
	int tempindent;
	int left_width;

	if (left_len == -1)
		left_len = strlen (left_text);

	if (right_len == -1)
		right_len = strlen (right_text);

	if (left_len + right_len + 2 >= sizeof (buf->xtext->scratch_buffer))
		right_len = sizeof (buf->xtext->scratch_buffer) - left_len - 2;

	if (right_text[right_len-1] == '\n')
		right_len--;

	ent = g_malloc0 (left_len + right_len + 2 + sizeof (textentry));
	str = (unsigned char *) ent + sizeof (textentry);

	if (left_len)
		memcpy (str, left_text, left_len);
	str[left_len] = ' ';
	if (right_len)
		memcpy (str + left_len + 1, right_text, right_len);
	str[left_len + 1 + right_len] = 0;

	left_width = gtk_xtext_text_width (buf->xtext, left_text, left_len);

	ent->left_len = left_len;
	ent->str = str;
	ent->str_len = left_len + 1 + right_len;
	/* This is copied into the scratch buffer later, double check math */
	g_assert (ent->str_len < sizeof (buf->xtext->scratch_buffer));

	if (buf->xtext->auto_indent)
	{
		int cap = gtk_xtext_auto_indent_cap (buf);

		ent->indent = (buf->indent - left_width) - buf->xtext->space_width;

		if (buf->time_stamp)
			space = buf->xtext->stamp_width;
		else
			space = 0;

		if (buf->indent < cap && ent->indent < MARGIN + space)
		{
			tempindent = MARGIN + space + buf->xtext->space_width + left_width;

			if (tempindent > buf->indent)
				buf->indent = tempindent;

			if (buf->indent > cap)
				buf->indent = cap;

			gtk_xtext_fix_indent (buf);
			gtk_xtext_recalc_widths (buf, FALSE);

			ent->indent = (buf->indent - left_width) - buf->xtext->space_width;
			buf->xtext->force_render = TRUE;
		}
	}
	else
	{
		ent->indent = buf->time_stamp ? buf->xtext->stamp_width : MARGIN;
	}

	gtk_xtext_prepend_entry (buf, ent, stamp);
}

void
gtk_xtext_prepend (xtext_buffer *buf, unsigned char *text, int len, time_t stamp)
{
	textentry *ent;
	gboolean truncate = FALSE;

	if (len == -1)
		len = strlen (text);

	if (text[len-1] == '\n')
		len--;

	if (len >= sizeof (buf->xtext->scratch_buffer))
	{
		len = sizeof (buf->xtext->scratch_buffer) - 1;
		truncate = TRUE;
	}

	ent = g_malloc0 (len + 1 + sizeof (textentry));
	ent->str = (unsigned char *) ent + sizeof (textentry);
	ent->str_len = len;
	if (len)
	{
		if (!truncate)
		{
			memcpy (ent->str, text, len);
			ent->str[len] = '\0';
		}
		else
		{
			safe_strcpy (ent->str, text, sizeof (buf->xtext->scratch_buffer));
			ent->str_len = strlen (ent->str);
		}
	}
	ent->indent = 0;
	ent->left_len = -1;

	gtk_xtext_prepend_entry (buf, ent, stamp);
}

/* IRCv3 modernization: sorted insert for chathistory gap filling (Phase 3)
 * These insert at the correct chronological position by timestamp.
 */

textentry *
gtk_xtext_insert_sorted_indent (xtext_buffer *buf,
								unsigned char *left_text, int left_len,
								unsigned char *right_text, int right_len,
								time_t stamp)
{
	textentry *ent;
	unsigned char *str;
	int space;
	int tempindent;
	int left_width;

	if (left_len == -1)
		left_len = strlen (left_text);

	if (right_len == -1)
		right_len = strlen (right_text);

	if (left_len + right_len + 2 >= sizeof (buf->xtext->scratch_buffer))
		right_len = sizeof (buf->xtext->scratch_buffer) - left_len - 2;

	if (right_text[right_len-1] == '\n')
		right_len--;

	ent = g_malloc0 (left_len + right_len + 2 + sizeof (textentry));
	str = (unsigned char *) ent + sizeof (textentry);

	if (left_len)
		memcpy (str, left_text, left_len);
	str[left_len] = ' ';
	if (right_len)
		memcpy (str + left_len + 1, right_text, right_len);
	str[left_len + 1 + right_len] = 0;

	left_width = gtk_xtext_text_width (buf->xtext, left_text, left_len);

	ent->left_len = left_len;
	ent->str = str;
	ent->str_len = left_len + 1 + right_len;
	g_assert (ent->str_len < sizeof (buf->xtext->scratch_buffer));

	if (buf->xtext->auto_indent)
	{
		int cap = gtk_xtext_auto_indent_cap (buf);

		ent->indent = (buf->indent - left_width) - buf->xtext->space_width;

		if (buf->time_stamp)
			space = buf->xtext->stamp_width;
		else
			space = 0;

		if (buf->indent < cap && ent->indent < MARGIN + space)
		{
			tempindent = MARGIN + space + buf->xtext->space_width + left_width;

			if (tempindent > buf->indent)
				buf->indent = tempindent;

			if (buf->indent > cap)
				buf->indent = cap;

			gtk_xtext_fix_indent (buf);
			gtk_xtext_recalc_widths (buf, FALSE);

			ent->indent = (buf->indent - left_width) - buf->xtext->space_width;
			buf->xtext->force_render = TRUE;
		}
	}
	else
	{
		ent->indent = buf->time_stamp ? buf->xtext->stamp_width : MARGIN;
	}

	gtk_xtext_insert_sorted_entry (buf, ent, stamp);

	return ent;
}

void
gtk_xtext_insert_sorted (xtext_buffer *buf, unsigned char *text, int len, time_t stamp)
{
	textentry *ent;
	gboolean truncate = FALSE;

	if (len == -1)
		len = strlen (text);

	if (text[len-1] == '\n')
		len--;

	if (len >= sizeof (buf->xtext->scratch_buffer))
	{
		len = sizeof (buf->xtext->scratch_buffer) - 1;
		truncate = TRUE;
	}

	ent = g_malloc0 (len + 1 + sizeof (textentry));
	ent->str = (unsigned char *) ent + sizeof (textentry);
	ent->str_len = len;
	if (len)
	{
		if (!truncate)
		{
			memcpy (ent->str, text, len);
			ent->str[len] = '\0';
		}
		else
		{
			safe_strcpy (ent->str, text, sizeof (buf->xtext->scratch_buffer));
			ent->str_len = strlen (ent->str);
		}
	}
	ent->indent = 0;
	ent->left_len = -1;

	gtk_xtext_insert_sorted_entry (buf, ent, stamp);
}

gboolean
gtk_xtext_is_empty (xtext_buffer *buf)
{
	return buf->text_first == NULL;
}


/* Helper: search a raw text string using the output buffer's search state.
 * Creates a minimal temporary textentry for gtk_xtext_search_textentry. */
static GList *
gtk_xtext_search_raw_text (xtext_buffer *out, const char *text, int len)
{
	textentry tmp;
	memset (&tmp, 0, sizeof (tmp));
	tmp.str = (unsigned char *)text;
	tmp.str_len = len;
	return gtk_xtext_search_textentry (out, &tmp);
}

int
gtk_xtext_lastlog (xtext_buffer *out, xtext_buffer *search_area)
{
	textentry *ent;
	int matches;
	GList *gl;

	/* Virtual mode: search entire DB history */
	if (HAS_VIRT_DB (search_area) &&
	    search_area->virt_channel)
	{
		int total = scrollback_count (search_area->virt_db,
		                              search_area->virt_channel);
		int offset = 0;
		int batch = 500;

		matches = 0;
		while (offset < total)
		{
			GSList *msgs = scrollback_load_range (search_area->virt_db,
			                                      search_area->virt_channel,
			                                      offset, batch);
			GSList *iter;

			for (iter = msgs; iter; iter = iter->next)
			{
				scrollback_msg *msg = iter->data;
				if (!msg->text)
					continue;

				gl = gtk_xtext_search_raw_text (out, msg->text,
				                                strlen (msg->text));
				if (gl)
				{
					int len = strlen (msg->text);
					char *tab;

					matches++;
					tab = strchr (msg->text, '\t');
					if (tab && search_area->xtext->auto_indent)
					{
						int left_len = tab - msg->text;
						gtk_xtext_append_indent (out,
						                         (unsigned char *)msg->text, left_len,
						                         (unsigned char *)tab + 1,
						                         len - left_len - 1, 0);
					}
					else
					{
						gtk_xtext_append (out, (unsigned char *)msg->text, len, 0);
					}

					if (out->text_last)
					{
						out->text_last->stamp = msg->timestamp;
						gtk_xtext_search_textentry_add (out, out->text_last,
						                                gl, TRUE);
					}
				}
			}
			offset += batch;
			scrollback_msg_list_free (msgs);
		}
		out->search_found = g_list_reverse (out->search_found);
		return matches;
	}

	ent = search_area->text_first;
	matches = 0;

	while (ent)
	{
		gl = gtk_xtext_search_textentry (out, ent);
		if (gl)
		{
			matches++;
			/* copy the text over */
			if (search_area->xtext->auto_indent)
			{
				gtk_xtext_append_indent (out, ent->str, ent->left_len,
												 ent->str + ent->left_len + 1,
												 ent->str_len - ent->left_len - 1, 0);
			}
			else
			{
				gtk_xtext_append (out, ent->str, ent->str_len, 0);
			}

			if (out->text_last)
			{
				out->text_last->stamp = ent->stamp;
				gtk_xtext_search_textentry_add (out, out->text_last, gl, TRUE);
			}
		}
		ent = ent->next;
	}
	out->search_found = g_list_reverse (out->search_found);

	return matches;
}

void
gtk_xtext_foreach (xtext_buffer *buf, GtkXTextForeach func, void *data)
{
	/* Virtual mode: stream all messages from DB */
	if (HAS_VIRT_DB (buf) && buf->virt_channel)
	{
		int total = scrollback_count (buf->virt_db, buf->virt_channel);
		int offset = 0;
		int batch = 500;

		while (offset < total)
		{
			GSList *msgs = scrollback_load_range (buf->virt_db, buf->virt_channel,
			                                      offset, batch);
			GSList *iter;

			for (iter = msgs; iter; iter = iter->next)
			{
				scrollback_msg *msg = iter->data;
				if (msg->text)
					(*func) (buf->xtext, (unsigned char *)msg->text, data);
			}
			offset += batch;
			scrollback_msg_list_free (msgs);
		}
		return;
	}

	{
		textentry *ent = buf->text_first;

		while (ent)
		{
			(*func) (buf->xtext, ent->str, data);
			ent = ent->next;
		}
	}
}

void
gtk_xtext_set_indent (GtkXText *xtext, gboolean indent)
{
	if (xtext->auto_indent == indent)
		return;

	xtext->auto_indent = indent;

	if (xtext->stamp_width == 0)
	{
		char *time_str;
		int stamp_size = xtext_get_stamp_str (time (0), &time_str);
		xtext->stamp_width =
			gtk_xtext_text_width (xtext, (unsigned char *)time_str, stamp_size) + MARGIN;
		g_free (time_str);
	}

	/* When turning indent OFF, reset buf->indent so continuation lines
	 * don't render at the old separator position. */
	if (!indent && xtext->buffer)
	{
		xtext->buffer->indent = MARGIN;
	}

	/* When turning indent ON, recalculate buf->indent from existing entries
	 * since the auto-adjust in append_indent was skipped while indent was off. */
	if (indent && xtext->buffer)
	{
		xtext_buffer *buf = xtext->buffer;
		textentry *ent;
		int space = buf->time_stamp ? xtext->stamp_width : 0;
		int left_width, tempindent;

		buf->indent = MARGIN;

		for (ent = buf->text_first; ent; ent = ent->next)
		{
			if (ent->left_len <= 0)
				continue;
			left_width = gtk_xtext_text_width (xtext, ent->str, ent->left_len);
			tempindent = MARGIN + space + xtext->space_width + left_width;
			if (tempindent > buf->indent)
				buf->indent = tempindent;
		}

		{
			int cap = gtk_xtext_auto_indent_cap (buf);
			if (buf->indent > cap)
				buf->indent = cap;
		}

		gtk_xtext_fix_indent (buf);
	}
}

void
gtk_xtext_set_max_indent (GtkXText *xtext, int max_auto_indent)
{
	xtext->max_auto_indent = max_auto_indent;
}

void
gtk_xtext_set_max_lines (GtkXText *xtext, int max_lines)
{
	xtext->max_lines = max_lines;
}

void
gtk_xtext_enforce_mat_window (xtext_buffer *buf)
{
	int viewport_center;

	if (!buf || !buf->virt_db)
		return;
	if (BUF_MAT_COUNT (buf) <= VIRT_MAT_WINDOW)
		return;

	/* Find viewport center relative to materialized window.
	 * Evict from whichever end is furthest from the viewport. */
	if (buf->xtext && buf->xtext->adj)
	{
		gdouble val = gtk_adjustment_get_value (buf->xtext->adj);
		gdouble page = gtk_adjustment_get_page_size (buf->xtext->adj);
		viewport_center = (int)(val + page / 2.0) - LINES_BEFORE_MAT (buf);
	}
	else
	{
		viewport_center = BUF_LINES_MAT (buf);  /* assume bottom */
	}

	while (BUF_MAT_COUNT (buf) > VIRT_MAT_WINDOW)
	{
		int old_count = BUF_MAT_COUNT (buf);

		/* If viewport is in the bottom half, evict from head.
		 * If viewport is in the top half, evict from tail. */
		if (viewport_center > BUF_LINES_MAT (buf) / 2)
			gtk_xtext_remove_top (buf);
		else
			gtk_xtext_remove_bottom (buf);

		/* Break if eviction was blocked (e.g., ephemeral entries pinned) */
		if (BUF_MAT_COUNT (buf) >= old_count)
			break;
	}
}

void
gtk_xtext_set_show_marker (GtkXText *xtext, gboolean show_marker)
{
	xtext->marker = show_marker;
}

void
gtk_xtext_set_show_separator (GtkXText *xtext, gboolean show_separator)
{
	xtext->separator = show_separator;
}

void
gtk_xtext_set_thin_separator (GtkXText *xtext, gboolean thin_separator)
{
	xtext->thinline = thin_separator;
}

void
gtk_xtext_set_time_stamp (xtext_buffer *buf, gboolean time_stamp)
{
	if (buf->time_stamp == time_stamp)
		return;

	buf->time_stamp = time_stamp;

	if (!buf->xtext || !buf->xtext->auto_indent)
		return;

	/* Recalculate buf->indent from existing entries since stamp space changed */
	{
		textentry *ent;
		int space = time_stamp ? buf->xtext->stamp_width : 0;
		int left_width, tempindent;

		buf->indent = MARGIN;

		for (ent = buf->text_first; ent; ent = ent->next)
		{
			if (ent->left_len <= 0)
				continue;
			left_width = gtk_xtext_text_width (buf->xtext, ent->str, ent->left_len);
			tempindent = MARGIN + space + buf->xtext->space_width + left_width;
			if (tempindent > buf->indent)
				buf->indent = tempindent;
		}

		{
			int cap = gtk_xtext_auto_indent_cap (buf);
			if (buf->indent > cap)
				buf->indent = cap;
		}

		gtk_xtext_fix_indent (buf);
	}
}

void
gtk_xtext_set_urlcheck_function (GtkXText *xtext, int (*urlcheck_function) (GtkWidget *, char *))
{
	xtext->urlcheck_function = urlcheck_function;
}

void
gtk_xtext_set_wordwrap (GtkXText *xtext, gboolean wordwrap)
{
	xtext->wordwrap = wordwrap;
}

void
gtk_xtext_set_scroll_to_top_callback (GtkXText *xtext,
                                      void (*callback) (GtkXText *, gpointer),
                                      gpointer userdata)
{
	xtext->scroll_to_top_cb = callback;
	xtext->scroll_to_top_userdata = userdata;
}

void
gtk_xtext_reset_scroll_top_backoff (GtkXText *xtext)
{
	/* Reset backoff to initial value (called when new content arrives) */
	xtext->scroll_top_backoff_ms = 500;
}

void
gtk_xtext_set_reply_button_callback (GtkXText *xtext,
                                     void (*callback) (GtkXText *, const char *, const char *, gpointer),
                                     gpointer userdata)
{
	xtext->reply_button_cb = callback;
	xtext->reply_button_userdata = userdata;
}

void
gtk_xtext_set_react_text_button_callback (GtkXText *xtext,
                                           void (*callback) (GtkXText *, const char *, const char *, gpointer),
                                           gpointer userdata)
{
	xtext->react_text_button_cb = callback;
	xtext->react_text_button_userdata = userdata;
}

void
gtk_xtext_set_react_emoji_button_callback (GtkXText *xtext,
                                            void (*callback) (GtkXText *, const char *, const char *, gpointer),
                                            gpointer userdata)
{
	xtext->react_emoji_button_cb = callback;
	xtext->react_emoji_button_userdata = userdata;
}

void
gtk_xtext_set_redact_button_callback (GtkXText *xtext,
                                       void (*callback) (GtkXText *, const char *, const char *, gpointer),
                                       gpointer userdata)
{
	xtext->redact_button_cb = callback;
	xtext->redact_button_userdata = userdata;
}

void
gtk_xtext_set_reaction_click_callback (GtkXText *xtext,
                                       void (*callback) (GtkXText *, const char *, const char *, gboolean, gpointer),
                                       gpointer userdata)
{
	xtext->reaction_click_cb = callback;
	xtext->reaction_click_userdata = userdata;
}

void
gtk_xtext_set_picker_click_callback (GtkXText *xtext,
                                     void (*callback) (GtkXText *, const char *, gpointer),
                                     gpointer userdata)
{
	xtext->picker_click_cb = callback;
	xtext->picker_click_userdata = userdata;
}

/* Resolve marker_pos_id to a textentry pointer. Returns NULL if not set or stale. */
static textentry *
xtext_resolve_marker (xtext_buffer *buf)
{
	return gtk_xtext_find_by_id (buf, buf->marker_pos_id);
}

void
gtk_xtext_set_marker_last (session *sess)
{
	xtext_buffer *buf = sess->res->buffer;

	buf->marker_pos_id = buf->text_last ? buf->text_last->entry_id : 0;
	buf->marker_state = MARKER_IS_SET;
}

void
gtk_xtext_reset_marker_pos (GtkXText *xtext)
{
	if (xtext->buffer->marker_pos_id)
	{
		xtext->buffer->marker_pos_id = 0;
		dontscroll (xtext->buffer); /* force scrolling off */
		/* GTK3: Queue a redraw instead of rendering directly */
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
		xtext->buffer->marker_state = MARKER_RESET_MANUALLY;
	}
}

int
gtk_xtext_moveto_marker_pos (GtkXText *xtext)
{
	gdouble value = 0;
	xtext_buffer *buf = xtext->buffer;
	textentry *ent = buf->text_first;
	GtkAdjustment *adj = xtext->adj;

	if (buf->marker_pos_id == 0)
		return buf->marker_state;

	{
		textentry *marker = xtext_resolve_marker (buf);
		if (!marker)
			return buf->marker_state;

		if (gtk_xtext_check_ent_visibility (xtext, marker, 1) == FALSE)
		{
			while (ent)
			{
				if (ent == marker)
					break;
				value += ENT_DISPLAY_LINES (ent);
				ent = ent->next;
			}
			if (value >= gtk_adjustment_get_value (adj) && value < gtk_adjustment_get_value (adj) + gtk_adjustment_get_page_size (adj))
				return MARKER_IS_SET;
			value -= gtk_adjustment_get_page_size (adj) / 2;
			if (value < 0)
				value = 0;
			if (value > gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj))
				value = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
			gtk_adjustment_set_value (adj, value);
			/* GTK3: Queue a redraw instead of rendering directly */
			gtk_widget_queue_draw (GTK_WIDGET (xtext));
		}

		/* If we previously lost marker position to scrollback limit -- */
		if (marker == buf->text_first &&
			 buf->marker_state == MARKER_RESET_BY_KILL)
			return MARKER_RESET_BY_KILL;
		else
			return MARKER_IS_SET;
	}
}

/* Scroll to make a specific entry visible, centered in the viewport. */
void
gtk_xtext_scroll_to_entry (xtext_buffer *buf, textentry *target)
{
	GtkXText *xtext = buf->xtext;
	GtkAdjustment *adj = xtext->adj;
	textentry *ent;
	gdouble value = 0;
	gdouble upper, page_size;

	if (!target || !adj)
		return;

	/* Already visible? */
	if (gtk_xtext_check_ent_visibility (xtext, target, 1))
		return;

	/* Calculate line offset of target entry */
	for (ent = buf->text_first; ent; ent = ent->next)
	{
		if (ent == target)
			break;
		value += ENT_DISPLAY_LINES (ent);
	}
	if (!ent)
		return; /* target not in buffer */

	/* Center the entry in the viewport */
	upper = gtk_adjustment_get_upper (adj);
	page_size = gtk_adjustment_get_page_size (adj);
	value -= page_size / 2;
	if (value < 0)
		value = 0;
	if (value > upper - page_size)
		value = upper - page_size;

	gtk_adjustment_set_value (adj, value);
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

void
gtk_xtext_set_marker_from_timestamp (xtext_buffer *buf, time_t timestamp)
{
	textentry *ent;

	buf->server_read_marker = TRUE;

	/* Find the first entry AFTER the read timestamp = first unread */
	for (ent = buf->text_first; ent; ent = ent->next)
	{
		if (ent->stamp > timestamp)
		{
			buf->marker_pos_id = ent->entry_id;
			buf->marker_state = MARKER_IS_SET;
			buf->marker_seen = FALSE;
			if (buf->xtext)
				gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
			return;
		}
	}

	/* All messages are at or before the timestamp — everything is read */
	buf->marker_pos_id = 0;
	buf->marker_state = MARKER_WAS_NEVER_SET;
	if (buf->xtext)
		gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
}

void
gtk_xtext_buffer_show (GtkXText *xtext, xtext_buffer *buf, int render)
{
	int w, h;

	buf->xtext = xtext;

	if (xtext->buffer == buf)
		return;

/*printf("text_buffer_show: xtext=%p buffer=%p\n", xtext, buf);*/

	/* Scroll position is preserved in buf->scroll_anchor, updated by
	 * adjustment_changed on every value change.  No explicit save needed. */

	if (xtext->add_io_tag)
	{
		g_source_remove (xtext->add_io_tag);
		xtext->add_io_tag = 0;
	}

	if (xtext->io_tag)
	{
		g_source_remove (xtext->io_tag);
		xtext->io_tag = 0;
	}

	/* Dismiss toasts on buffer switch (widget-scoped) */
	gtk_xtext_toast_clear (xtext);

	if (!gtk_widget_get_realized (GTK_WIDGET (xtext)))
		gtk_widget_realize (GTK_WIDGET (xtext));

	h = gtk_widget_get_height (GTK_WIDGET (xtext));
	w = gtk_widget_get_width (GTK_WIDGET (xtext));
	if (xtext->scrollbar)
	{
		int sb_min, sb_nat;
		gtk_widget_measure (xtext->scrollbar, GTK_ORIENTATION_HORIZONTAL, -1,
		                    &sb_min, &sb_nat, NULL, NULL);
		w -= sb_nat;
	}

	/* after a font change */
	if (buf->needs_recalc)
	{
		buf->needs_recalc = FALSE;
		gtk_xtext_recalc_widths (buf, TRUE);
	}

	/* now change to the new buffer */
	{
		gboolean was_down = buf->scroll_anchor.anchor_to_bottom;
		xtext->buffer = buf;
		dontscroll (buf);	/* force scrolling off */

		/* Set upper before value to avoid clamping issues */
		gtk_adjustment_set_upper (xtext->adj, buf->num_lines);

		/* Restore scroll position - force to bottom if buffer was tracking bottom.
		 * Must use saved was_down since dontscroll cleared anchor_to_bottom above. */
		if (was_down)
		{
			buf->scroll_anchor.anchor_to_bottom = TRUE;
			gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper (xtext->adj) - gtk_adjustment_get_page_size (xtext->adj));
		}
		else
		{
			/* Restore scroll position from the persistent anchor.
			 * The anchor was captured in adjustment_changed when the
			 * user last interacted with this buffer. */
			gtk_xtext_restore_scroll_anchor (buf, &buf->scroll_anchor);
		}
	}

	if (gtk_adjustment_get_upper (xtext->adj) == 0)
		gtk_adjustment_set_upper (xtext->adj, 1);
	/* sanity check */
	else if (gtk_adjustment_get_value (xtext->adj) > gtk_adjustment_get_upper (xtext->adj) - gtk_adjustment_get_page_size (xtext->adj))
	{
		gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper (xtext->adj) - gtk_adjustment_get_page_size (xtext->adj));
		if (gtk_adjustment_get_value (xtext->adj) < 0)
			gtk_adjustment_set_value (xtext->adj, 0);
	}

	if (render)
	{
		/* did the window change size or indent since this buffer was last shown? */
		if (buf->window_width != w || buf->last_indent != buf->indent)
		{
			gboolean width_changed = (buf->window_width != w);
			buf->last_indent = buf->indent;
			buf->window_width = w;
			buf->window_height = h;
			gtk_xtext_recalc_widths (buf, width_changed);
			if (buf->scroll_anchor.anchor_to_bottom)
				gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper (xtext->adj) -
												  gtk_adjustment_get_page_size (xtext->adj));
		} else if (buf->window_height != h)
		{
			buf->window_height = h;
			if (buf->scroll_anchor.anchor_to_bottom)
				gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper (xtext->adj));
			gtk_xtext_adjustment_set (buf, FALSE);
		}

		/* GTK3: Queue a redraw instead of rendering directly */
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
}

xtext_buffer *
gtk_xtext_buffer_new (GtkXText *xtext)
{
	xtext_buffer *buf;

	buf = g_new0 (xtext_buffer, 1);
	buf->old_value = -1;
	buf->xtext = xtext;
	buf->scroll_anchor.anchor_to_bottom = TRUE;
	buf->indent = xtext->space_width * 2;
	dontscroll (buf);

	/* IRCv3 modernization: entry identification (Phase 1) */
	buf->entries_by_msgid = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	buf->entries_by_id = g_hash_table_new (g_direct_hash, g_direct_equal);
	/* Start at 1 (so 0 can mean "not set"); init_entry lazily bumps
	 * to LOCAL_ENTRY_ID_BASE on first local-ID allocation. */
	buf->next_entry_id = 1;

	/* Counted B-tree for O(log n) positional access by display line.
	 * Uses timestamp comparator for sorted insertion. */
	buf->entry_tree = newtree234_weighted (entry_stamp_cmp, entry_weight_cb);

	/* LRU display cache — manages sublines lifecycle for bounded memory */
	buf->display_cache = display_cache_new (DISPLAY_CACHE_SIZE);

	return buf;
}

void
gtk_xtext_buffer_free (xtext_buffer *buf)
{
	textentry *ent, *next;

	if (buf->xtext->buffer == buf)
		buf->xtext->buffer = buf->xtext->orig_buffer;

	if (buf->xtext->selection_buffer == buf)
		buf->xtext->selection_buffer = NULL;

	/* Free search state first — but skip the per-entry fini walk
	 * (search_textentry_fini) since we're about to free all entries
	 * anyway.  Just free the list and clear the search metadata. */
	if (buf->search_found)
	{
		g_list_free (buf->search_found);
		buf->search_found = NULL;
	}
	g_free (buf->search_text);
	buf->search_text = NULL;
	g_free (buf->search_nee);
	buf->search_nee = NULL;
	if (buf->search_re)
		g_regex_unref (buf->search_re);
	if (buf->search_virt_ids)
		g_array_free (buf->search_virt_ids, TRUE);

	ent = buf->text_first;
	while (ent)
	{
		next = ent->next;
		g_list_free (ent->marks);	/* search marks */
		g_free (ent->msgid);	/* Free msgid if set (Phase 1) */
		/* Phase 4: free separate str and redaction info */
		if (ent->flags & TEXTENTRY_FLAG_SEPARATE_STR)
			g_free (ent->str);
		if (ent->redaction)
		{
			g_free (ent->redaction->original_content);
			g_free (ent->redaction->redacted_by);
			g_free (ent->redaction->redaction_reason);
			g_free (ent->redaction);
		}
		g_free (ent->stripped_str);
		g_free (ent->fmt_spans);
		g_free (ent->emoji_list);
		g_free (ent->raw_to_stripped_map);
		g_free (ent);
		ent = next;
	}

	/* IRCv3 modernization: clean up hash tables (Phase 1) */
	if (buf->entries_by_msgid)
		g_hash_table_destroy (buf->entries_by_msgid);
	if (buf->entries_by_id)
		g_hash_table_destroy (buf->entries_by_id);

	/* Free the B-tree (entries already freed above, tree just has pointers) */
	if (buf->entry_tree)
		freetree234 (buf->entry_tree);

	/* Free the display line cache */
	display_cache_free (buf->display_cache);

	/* Virtual scrollback (Phase 2) */
	g_free (buf->virt_channel);
	buf->virt_channel = NULL;
	buf->virt_db = NULL;	/* borrowed pointer, don't free */

	g_free (buf);
}

/* Virtual scrollback (Phase 2): configure buffer for virtual mode */

void
gtk_xtext_buffer_set_virtual (xtext_buffer *buf, void *db, const char *channel,
                               int total_entries, gint64 max_rowid)
{
	buf->virt_db = db;
	buf->virt_channel = g_strdup (channel);
	buf->total_entries = total_entries;

	/* Compute avg_lines_per_entry from loaded data.
	 * This avoids the hardcoded 1.5 estimate which causes flicker on channels
	 * where the actual average differs significantly.
	 * Entry count and line totals come from the tree (already populated). */
	{
		int count = BUF_MAT_COUNT (buf);
		int total_lines = BUF_LINES_MAT (buf);
		buf->avg_lines_per_entry = (count > 0) ? (double)total_lines / count : 1.5;
	}

	/* Loaded entries are the newest — they start at this index.
	 * Exclude ephemerals from the count (they have no DB position). */
	buf->mat_first_index = total_entries - (BUF_MAT_COUNT (buf) - buf->ephemeral_count);
	if (buf->mat_first_index < 0)
		buf->mat_first_index = 0;

	/* Local IDs live in [LOCAL_ENTRY_ID_BASE..) which is disjoint from
	 * the DB rowid space, so the historical bump-past-max_rowid here is
	 * no longer needed.  max_rowid is unused — keep the parameter for
	 * API stability. */
	(void) max_rowid;

	/* Set initial lines_before_mat from the formula — the only place this
	 * is computed from the formula.  After this, it's absorptive. */
	buf->lines_before_mat = (int)(buf->mat_first_index * buf->avg_lines_per_entry);

	/* Compute initial line estimates so the scrollbar reflects total history.
	 * Block the value-changed handler during calc_lines_virtual to prevent
	 * adjustment_changed from clearing anchor_to_bottom (the old scroll value
	 * is less than the new virtual upper - page_size).
	 * Then explicitly scroll to the new bottom if we were already there. */
	{
		gboolean was_down = buf->scroll_anchor.anchor_to_bottom;

		g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
		gtk_xtext_calc_lines_virtual (buf, TRUE);

		if (was_down && buf->xtext->buffer == buf)
		{
			gdouble upper = gtk_adjustment_get_upper (buf->xtext->adj);
			gdouble page = gtk_adjustment_get_page_size (buf->xtext->adj);
			gdouble bottom = upper - page;
			if (bottom < 0)
				bottom = 0;
			buf->scroll_anchor.anchor_to_bottom = TRUE;
			gtk_adjustment_set_value (buf->xtext->adj, bottom);
		}
		else if (was_down)
		{
			buf->scroll_anchor.anchor_to_bottom = TRUE;
		}

		g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
	}
}

/* Virtual scrollback (Phase 3): create a textentry from a DB record.
 * Returns an unlinked entry — caller must link it into the list.
 * The entry uses the DB rowid as its entry_id for stability. */

static textentry *
gtk_xtext_virt_materialize_msg (xtext_buffer *buf, scrollback_msg *msg)
{
	textentry *ent;
	unsigned char *str;
	int text_len, left_len;
	const char *tab;
	int left_width;

	if (!msg->text || !msg->text[0] || !buf->xtext)
		return NULL;

	/* Skip if this DB entry is already materialized — prevents duplicates
	 * when ensure_range is called with overlapping ranges during rapid
	 * thumb scrolling. */
	if (msg->id > 0 && buf->entries_by_id &&
	    g_hash_table_lookup (buf->entries_by_id, GSIZE_TO_POINTER ((guint64)msg->id)))
		return NULL;

	text_len = (int)strlen (msg->text);

	/* Find the \t separator between nick and message text */
	tab = strchr (msg->text, '\t');
	if (tab)
		left_len = (int)(tab - msg->text);
	else
		left_len = -1;

	/* Allocate entry + string in one block (same layout as append_indent) */
	if (left_len >= 0)
	{
		/* indent mode: left_text + ' ' + right_text */
		int right_len = text_len - left_len - 1;	/* skip the \t */
		if (right_len < 0) right_len = 0;

		ent = g_malloc0 (left_len + right_len + 2 + sizeof (textentry));
		str = (unsigned char *) ent + sizeof (textentry);

		if (left_len > 0)
			memcpy (str, msg->text, left_len);
		str[left_len] = ' ';
		if (right_len > 0)
			memcpy (str + left_len + 1, msg->text + left_len + 1, right_len);
		str[left_len + 1 + right_len] = 0;

		ent->str = str;
		ent->str_len = left_len + 1 + right_len;
		ent->left_len = left_len;
	}
	else
	{
		/* no indent (no \t found) */
		ent = g_malloc0 (text_len + 1 + sizeof (textentry));
		str = (unsigned char *) ent + sizeof (textentry);
		memcpy (str, msg->text, text_len);
		str[text_len] = 0;

		ent->str = str;
		ent->str_len = text_len;
		ent->left_len = -1;
	}

	/* Compute indent before init (mirror append_indent, skip auto-adjust) */
	if (left_len >= 0 && buf->xtext->auto_indent)
	{
		left_width = gtk_xtext_text_width (buf->xtext, (unsigned char *)msg->text, left_len);
		ent->indent = (buf->indent - left_width) - buf->xtext->space_width;
	}
	else if (left_len >= 0)
	{
		ent->indent = buf->time_stamp ? buf->xtext->stamp_width : MARGIN;
	}
	else
	{
		ent->indent = 0;
	}

	/* Use DB rowid as entry_id via pending_db_rowid mechanism */
	buf->pending_db_rowid = msg->id;
	gtk_xtext_init_entry (buf, ent, msg->timestamp);

	/* Register msgid if present */
	if (msg->msgid && msg->msgid[0])
		gtk_xtext_set_msgid (buf, ent, msg->msgid);

	/* Compute sublines */
	ent->sublines = NULL;
	if (buf->window_width > 0)
		gtk_xtext_lines_taken (buf, ent);
	else
		ent->display_lines = 1;

	/* Add to B-tree for O(log n) positional access */
	if (buf->entry_tree)
		add234 (buf->entry_tree, ent);

	return ent;
}

/* Virtual scrollback (Phase 3): evict an entry from head or tail.
 * Unlinks it from the list and frees it. Does NOT update mat_first_index. */

static void
gtk_xtext_virt_evict_head (xtext_buffer *buf)
{
	textentry *ent = buf->text_first;
	if (!ent)
		return;

	/* Skip ephemeral entries — they can't be rematerialized from DB */
	if (ent->flags & TEXTENTRY_FLAG_EPHEMERAL)
		return;

	buf->text_first = ent->next;
	if (buf->text_first)
	{
		buf->text_first->prev = NULL;
		/* Remove stale day boundary on new head */
		if (buf->text_first->flags & TEXTENTRY_FLAG_DAY_BOUNDARY)
		{
			buf->text_first->flags &= ~TEXTENTRY_FLAG_DAY_BOUNDARY;
			buf->text_first->extra_lines_above--;
			buf->text_first->display_lines--;
			if (buf->entry_tree)
				update_weight234 (buf->entry_tree, buf->text_first, -1);
		}
	}
	else
		buf->text_last = NULL;

	gtk_xtext_kill_ent (buf, ent);
}

static void
gtk_xtext_virt_evict_tail (xtext_buffer *buf)
{
	textentry *ent = buf->text_last;
	if (!ent)
		return;

	/* Skip ephemeral entries — they can't be rematerialized from DB */
	if (ent->flags & TEXTENTRY_FLAG_EPHEMERAL)
		return;

	buf->text_last = ent->prev;
	if (buf->text_last)
		buf->text_last->next = NULL;
	else
		buf->text_first = NULL;

	gtk_xtext_kill_ent (buf, ent);
}

/* Virtual scrollback (Phase 3+5): load/evict entries to maintain a window
 * of materialized entries around the given center_index.
 *
 * Phase 5: after load/evict, recomputes line counts via calc_lines_virtual_ex
 * with recompute_sublines=FALSE (skips Pango — just sums cached values).
 * Only triggered when the viewport approaches the materialization boundary
 * (smart trigger in adjustment_changed). */

static void
gtk_xtext_virt_ensure_range (xtext_buffer *buf, int center_index, int radius)
{
	int want_start, want_end;
	int mat_start, mat_end;
	int old_mat_count, old_mat_first;
	scrollback_db *db;

	if (!buf->virt_db || !buf->virt_channel)
		return;

	old_mat_count = BUF_MAT_COUNT (buf);
	old_mat_first = buf->mat_first_index;

	db = (scrollback_db *) buf->virt_db;

	/* Clamp desired range */
	want_start = center_index - radius;
	if (want_start < 0)
		want_start = 0;
	want_end = center_index + radius;
	if (want_end >= buf->total_entries)
		want_end = buf->total_entries - 1;

	if (want_end < want_start)
		return;

	mat_start = buf->mat_first_index;
	mat_end = buf->mat_first_index + (BUF_MAT_COUNT (buf) - buf->ephemeral_count) - 1;

	/* Prepend: load entries before current materialized window.
	 * Build a local chain (oldest→newest) then splice at head. */
	if (want_start < mat_start)
	{
		int count = mat_start - want_start;
		GSList *msgs = scrollback_load_range (db, buf->virt_channel, want_start, count);
		GSList *iter;
		textentry *chain_first = NULL, *chain_last = NULL;
		int loaded = 0;

		/* Build chain in chronological order (oldest first) */
		for (iter = msgs; iter; iter = iter->next)
		{
			scrollback_msg *msg = iter->data;
			textentry *ent = gtk_xtext_virt_materialize_msg (buf, msg);
			if (!ent)
				continue;

			ent->prev = chain_last;
			ent->next = NULL;
			if (chain_last)
				chain_last->next = ent;
			else
				chain_first = ent;
			chain_last = ent;
			loaded++;
		}

		/* Splice chain at head of buffer's linked list */
		if (chain_first)
		{
			if (buf->text_first)
			{
				chain_last->next = buf->text_first;
				buf->text_first->prev = chain_last;
			}
			else
			{
				buf->text_last = chain_last;
			}
			buf->text_first = chain_first;
		}

		buf->mat_first_index = want_start;
		scrollback_msg_list_free (msgs);

		/* Fix day boundaries for newly prepended entries and the join point */
		{
			textentry *e;
			int checked = 0;
			for (e = buf->text_first; e && checked < loaded + 1; e = e->next, checked++)
			{
				gboolean was = (e->flags & TEXTENTRY_FLAG_DAY_BOUNDARY) != 0;
				gboolean should = prefs.hex_gui_day_separator && e->prev &&
				                   e->stamp > 0 && e->prev->stamp > 0 &&
				                   xtext_is_different_day (e->prev->stamp, e->stamp);
				if (should && !was)
				{
					e->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
					e->extra_lines_above++;
					e->display_lines++;
					if (buf->entry_tree)
						update_weight234 (buf->entry_tree, e, 1);
				}
				else if (!should && was)
				{
					e->flags &= ~TEXTENTRY_FLAG_DAY_BOUNDARY;
					e->extra_lines_above--;
					e->display_lines--;
					if (buf->entry_tree)
						update_weight234 (buf->entry_tree, e, -1);
				}
			}
		}

		/* Absorptive: prepended entries move from estimated to actual */
		{
			textentry *e;
			int checked = 0;
			for (e = buf->text_first; e && checked < loaded; e = e->next, checked++)
				buf->lines_before_mat -= ENT_DISPLAY_LINES (e);
			if (buf->lines_before_mat < 0)
				buf->lines_before_mat = 0;
		}
	}

	/* Append: load entries after current materialized window */
	if (want_end > mat_end && BUF_MAT_COUNT (buf) > 0)
	{
		int count = want_end - mat_end;
		GSList *msgs = scrollback_load_range (db, buf->virt_channel, mat_end + 1, count);
		GSList *iter;
		int loaded = 0;

		for (iter = msgs; iter; iter = iter->next)
		{
			scrollback_msg *msg = iter->data;
			textentry *ent = gtk_xtext_virt_materialize_msg (buf, msg);
			if (!ent)
				continue;

			/* Link at tail */
			ent->prev = buf->text_last;
			ent->next = NULL;
			if (buf->text_last)
				buf->text_last->next = ent;
			else
				buf->text_first = ent;
			buf->text_last = ent;

			/* Day boundary */
			if (prefs.hex_gui_day_separator && ent->prev &&
			    ent->stamp > 0 && ent->prev->stamp > 0 &&
			    xtext_is_different_day (ent->prev->stamp, ent->stamp))
			{
				ent->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
				ent->extra_lines_above++;
				ent->display_lines++;
				if (buf->entry_tree)
					update_weight234 (buf->entry_tree, ent, 1);
			}
			loaded++;
		}

		scrollback_msg_list_free (msgs);
	}
	else if (want_end > mat_end && BUF_MAT_COUNT (buf) == 0)
	{
		/* Nothing materialized yet — initial load */
		int count = want_end - want_start + 1;
		GSList *msgs = scrollback_load_range (db, buf->virt_channel, want_start, count);
		GSList *iter;

		buf->mat_first_index = want_start;

		for (iter = msgs; iter; iter = iter->next)
		{
			scrollback_msg *msg = iter->data;
			textentry *ent = gtk_xtext_virt_materialize_msg (buf, msg);
			if (!ent)
				continue;

			/* Link at tail */
			ent->prev = buf->text_last;
			ent->next = NULL;
			if (buf->text_last)
				buf->text_last->next = ent;
			else
				buf->text_first = ent;
			buf->text_last = ent;

			/* Day boundary */
			if (prefs.hex_gui_day_separator && ent->prev &&
			    ent->stamp > 0 && ent->prev->stamp > 0 &&
			    xtext_is_different_day (ent->prev->stamp, ent->stamp))
			{
				ent->flags |= TEXTENTRY_FLAG_DAY_BOUNDARY;
				ent->extra_lines_above++;
				ent->display_lines++;
				if (buf->entry_tree)
					update_weight234 (buf->entry_tree, ent, 1);
			}

		}

		scrollback_msg_list_free (msgs);
	}

	/* Evict from head if too far behind desired window.
	 * Only evict when BUF_MAT_COUNT exceeds the materialization window size.
	 * During active selection, pins prevent eviction of selected entries,
	 * allowing count to grow up to max_lines. */
	{
		int max = VIRT_MAT_WINDOW;
		while (buf->mat_first_index < want_start - VIRT_PAGE_SIZE &&
		       BUF_MAT_COUNT (buf) > max)
	{
		/* Don't evict pinned entries (selection or ephemeral) */
		if (buf->text_first && buf->sel_pin_start_id != 0 &&
		    buf->text_first->entry_id == buf->sel_pin_start_id)
			break;
		if (buf->text_first && (buf->text_first->flags & TEXTENTRY_FLAG_EPHEMERAL))
			break;

		{
			int evicted_lines = ENT_DISPLAY_LINES (buf->text_first);
			gboolean next_loses_boundary = (buf->text_first->next &&
			    (buf->text_first->next->flags & TEXTENTRY_FLAG_DAY_BOUNDARY));
			gtk_xtext_virt_evict_head (buf);
			buf->lines_before_mat += evicted_lines;
			if (next_loses_boundary)
				buf->lines_before_mat++;
		}
		buf->mat_first_index++;
	}
	}

	/* Tail eviction: symmetric with head eviction.  When the user scrolls
	 * up, the materialized window grows at the head; evict from the tail
	 * to keep mat_count near VIRT_MAT_WINDOW.  Previously omitted because
	 * "converting actual to lossy estimates creates blank space at the
	 * bottom" — but with bottom-up rendering anchored to text_last (or
	 * scroll_anchor), the renderer doesn't derive the viewport from
	 * num_lines, so estimate drift only affects the scrollbar thumb
	 * position, not visible content.  Keeping mat_count bounded prevents
	 * O(n) walks in calc_lines from hitting thousands of entries after
	 * the user scrolls to the top and back. */
	{
		int mat_db_count = BUF_MAT_COUNT (buf) - buf->ephemeral_count;
		int mat_end_idx = buf->mat_first_index + mat_db_count - 1;
		int max = VIRT_MAT_WINDOW;

		while (mat_end_idx > want_end + VIRT_PAGE_SIZE &&
		       BUF_MAT_COUNT (buf) > max)
		{
			/* Don't evict pinned entries (selection or ephemeral) */
			if (buf->text_last && buf->sel_pin_end_id != 0 &&
			    buf->text_last->entry_id == buf->sel_pin_end_id)
				break;
			if (buf->text_last && (buf->text_last->flags & TEXTENTRY_FLAG_EPHEMERAL))
				break;

			gtk_xtext_virt_evict_tail (buf);
			mat_db_count = BUF_MAT_COUNT (buf) - buf->ephemeral_count;
			mat_end_idx = buf->mat_first_index + mat_db_count - 1;
		}
	}

	/* Phase 5: only recompute if something was actually loaded or evicted.
	 * If ensure_range was called but the materialization window didn't change,
	 * skip the recompute entirely.  This prevents a convergence loop where
	 * each recompute shifts avg_lines_per_entry slightly, changing num_lines,
	 * which triggers another adjustment_changed → ensure_range cycle. */
	if (BUF_MAT_COUNT (buf) != old_mat_count || buf->mat_first_index != old_mat_first)
	{
		xtext_scroll_anchor er_anchor;

		gtk_xtext_save_scroll_anchor_top (buf, &er_anchor);

		if (buf->xtext && buf->xtext->vc_signal_tag)
		{
			g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
			gtk_xtext_calc_lines_virtual_ex (buf, TRUE, FALSE);
			g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
		}
		else
		{
			gtk_xtext_calc_lines_virtual_ex (buf, FALSE, FALSE);
		}

		if (buf->xtext && buf->xtext->adj)
		{
			if (buf->xtext->vc_signal_tag)
				g_signal_handler_block (buf->xtext->adj, buf->xtext->vc_signal_tag);
			gtk_xtext_restore_scroll_anchor_top (buf, &er_anchor);
			if (buf->xtext->vc_signal_tag)
				g_signal_handler_unblock (buf->xtext->adj, buf->xtext->vc_signal_tag);
		}
	}
}

/* Virtual scrollback: decide whether an entry should be materialized into
 * the in-memory linked list, or left in the DB for ensure_range to load
 * on demand.  Returns TRUE to materialize, FALSE to skip.
 *
 * When returning FALSE, performs the necessary bookkeeping (total_entries,
 * mat_first_index, lines_before_mat, num_lines, adjustment) so the
 * scrollbar reflects the entry's existence without it being in the list.
 *
 * dir: LINK_TAIL  — live message appended at end
 *      LINK_HEAD  — chathistory BEFORE prepended at start
 *      LINK_BEFORE — chathistory AFTER / gap-fill inserted sorted */
gboolean
gtk_xtext_virt_should_materialize (xtext_buffer *buf, time_t stamp,
                                   xtext_link_position dir)
{
	int est;

	if (!HAS_VIRT_DB (buf))
		return TRUE;

	/* Mat window not full — always materialize so content is visible
	 * as it arrives (e.g., fresh channel during catch-up).
	 * Use DB-backed count only; ephemerals don't consume DB capacity. */
	if (BUF_MAT_COUNT (buf) - buf->ephemeral_count < VIRT_MAT_WINDOW)
		return TRUE;

	if (dir == LINK_TAIL)
	{
		/* Live message at tail: materialize if user is following
		 * (scrollbar at bottom), skip if scrolled up. */
		if (!buf->text_last || buf->scroll_anchor.anchor_to_bottom)
			return TRUE;
	}
	else
	{
		/* HEAD or SORTED: materialize if the entry falls within or
		 * adjacent to the materialized time range.  Skip only if
		 * strictly older than the entire mat window. */
		if (!buf->text_first || stamp <= 0 || stamp >= buf->text_first->stamp)
			return TRUE;
	}

	/* --- Skip materialization — update bookkeeping --- */

	est = (int)(buf->avg_lines_per_entry + 0.5);
	if (est < 1) est = 1;

	buf->total_entries++;
	buf->num_lines += est;

	if (dir != LINK_TAIL)
	{
		/* Older entry shifts all DB indices up by 1 */
		buf->mat_first_index++;
		buf->lines_before_mat += est;
	}

	/* Update scrollbar atomically via configure.
	 * For TAIL (new msg while scrolled up): grow upper, keep value.
	 * For HEAD/SORTED (older entry): grow upper AND value in lockstep. */
	if (buf->xtext && buf->xtext->buffer == buf && !buf->batch_mode)
	{
		GtkAdjustment *adj = buf->xtext->adj;
		gdouble cur_val = gtk_adjustment_get_value (adj);
		gdouble new_val = (dir == LINK_TAIL) ? cur_val : cur_val + est;
		gdouble new_upper = buf->num_lines;
		gdouble page = gtk_adjustment_get_page_size (adj);

		g_signal_handler_block (adj, buf->xtext->vc_signal_tag);
		gtk_adjustment_configure (adj, new_val, 0, new_upper, 1, page, page);
		g_signal_handler_unblock (adj, buf->xtext->vc_signal_tag);
	}

	return FALSE;
}

/* IRCv3 modernization: entry lookup functions (Phase 1) */

/**
 * Find an entry by its server-assigned message ID.
 * Returns NULL if not found or if msgid is NULL/empty.
 */
textentry *
gtk_xtext_find_by_msgid (xtext_buffer *buf, const char *msgid)
{
	if (!buf || !buf->entries_by_msgid || !msgid || !msgid[0])
		return NULL;

	return g_hash_table_lookup (buf->entries_by_msgid, msgid);
}

/**
 * Find an entry by its local unique ID.
 * Returns NULL if not found or if entry_id is 0.
 */
textentry *
gtk_xtext_find_by_id (xtext_buffer *buf, guint64 entry_id)
{
	if (!buf || !buf->entries_by_id || entry_id == 0)
		return NULL;

	return g_hash_table_lookup (buf->entries_by_id, GSIZE_TO_POINTER (entry_id));
}

/**
 * Set the msgid for an entry and add it to the msgid hash table.
 * This is called when the server provides a message ID (e.g., IRCv3 msgid tag).
 * If the entry already has a msgid, it will be replaced.
 * Returns the entry for chaining, or NULL on error.
 */
textentry *
gtk_xtext_set_msgid (xtext_buffer *buf, textentry *ent, const char *msgid)
{
	if (!buf || !ent || !msgid || !msgid[0])
		return NULL;

	/* Remove old msgid from hash table if set */
	if (ent->msgid)
	{
		if (buf->entries_by_msgid)
			g_hash_table_remove (buf->entries_by_msgid, ent->msgid);
		g_free (ent->msgid);
	}

	/* Set new msgid */
	ent->msgid = g_strdup (msgid);

	/* Add to hash table (key is duplicated by the hash table) */
	if (buf->entries_by_msgid)
		g_hash_table_insert (buf->entries_by_msgid, g_strdup (msgid), ent);

	return ent;
}

void
gtk_xtext_begin_group (xtext_buffer *buf)
{
	/* Consume a slot from the local-ID counter for a unique group_id.
	 * Group IDs and local entry_ids share the [LOCAL_ENTRY_ID_BASE..)
	 * space, so this is safe even if compared against an entry_id
	 * elsewhere — neither will ever equal a DB rowid. */
	if (buf->next_entry_id < LOCAL_ENTRY_ID_BASE)
		buf->next_entry_id = LOCAL_ENTRY_ID_BASE;
	buf->current_group_id = buf->next_entry_id++;
}

void
gtk_xtext_end_group (xtext_buffer *buf)
{
	buf->current_group_id = 0;
}

/**
 * Get the entry_id of an entry.
 * Returns 0 if ent is NULL.
 */
guint64
gtk_xtext_get_entry_id (textentry *ent)
{
	return ent ? ent->entry_id : 0;
}

/**
 * Get the msgid of an entry.
 * Returns NULL if ent is NULL or has no msgid set.
 */
const char *
gtk_xtext_get_msgid (textentry *ent)
{
	return ent ? ent->msgid : NULL;
}

/**
 * Get the last entry in a buffer.
 * Returns NULL if buf is NULL or empty.
 */
textentry *
gtk_xtext_buffer_get_last (xtext_buffer *buf)
{
	return buf ? buf->text_last : NULL;
}

/**
 * Get the first entry in a buffer.
 * Returns NULL if buf is NULL or empty.
 */
textentry *
gtk_xtext_buffer_get_first (xtext_buffer *buf)
{
	return buf ? buf->text_first : NULL;
}

/**
 * Get the next entry in the linked list.
 * Returns NULL if ent is NULL or is the last entry.
 */
textentry *
gtk_xtext_entry_get_next (textentry *ent)
{
	return ent ? ent->next : NULL;
}

textentry *
gtk_xtext_entry_get_prev (textentry *ent)
{
	return ent ? ent->prev : NULL;
}

/* IRCv3 modernization: entry modification (Phase 4) */

gboolean
gtk_xtext_entry_set_text (xtext_buffer *buf, textentry *ent,
                          const unsigned char *new_text, int new_len)
{
	int old_sublines, new_sublines;

	if (!buf || !ent || !new_text)
		return FALSE;

	if (new_len == -1)
		new_len = strlen ((const char *)new_text);

	/* Free old separate-allocated str (not inline) */
	if (ent->flags & TEXTENTRY_FLAG_SEPARATE_STR)
		g_free (ent->str);

	/* Free old parsed format data */
	g_free (ent->stripped_str);
	g_free (ent->fmt_spans);
	g_free (ent->emoji_list);
	g_free (ent->raw_to_stripped_map);

	/* Allocate new separate buffer */
	ent->str = g_malloc (new_len + 1);
	memcpy (ent->str, new_text, new_len);
	ent->str[new_len] = '\0';
	ent->str_len = new_len;
	ent->flags |= TEXTENTRY_FLAG_SEPARATE_STR;

	/* Rebuild parsed format data */
	{
		unsigned char *stripped;
		int stripped_len, span_count, emoji_count;
		xtext_fmt_span *spans;
		xtext_emoji_info *emojis;
		guint16 *r2s;

		xtext_parse_formats (ent->str, ent->str_len,
		                     &stripped, &stripped_len,
		                     &spans, &span_count,
		                     &emojis, &emoji_count,
		                     &r2s,
		                     buf->xtext->emoji_cache != NULL);
		ent->stripped_str = stripped;
		ent->stripped_len = (guint16) stripped_len;
		ent->fmt_spans = spans;
		ent->fmt_span_count = (guint16) span_count;
		ent->emoji_list = emojis;
		ent->emoji_count = (guint16) emoji_count;
		ent->raw_to_stripped_map = r2s;
	}

	/* Recalculate derived data */
	ent->str_width = gtk_xtext_text_width_ent (buf->xtext, ent);
	old_sublines = g_slist_length (ent->sublines);
	new_sublines = gtk_xtext_lines_taken (buf, ent);
	buf->num_lines += (new_sublines - old_sublines);

	/* Invalidate search marks */
	if (ent->marks)
		gtk_xtext_search_textentry_del (buf, ent);

	/* Update scrollbar and redraw if visible */
	if (buf->xtext->buffer == buf)
	{
		gtk_xtext_adjustment_set (buf, TRUE);
		if (gtk_xtext_check_ent_visibility (buf->xtext, ent, 0))
			gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
	}

	return TRUE;
}

void
gtk_xtext_entry_set_state (xtext_buffer *buf, textentry *ent,
                           xtext_entry_state new_state)
{
	if (!buf || !ent || ent->state == (guchar)new_state)
		return;

	ent->state = (guchar)new_state;

	if (buf->xtext->buffer == buf &&
	    gtk_xtext_check_ent_visibility (buf->xtext, ent, 0))
		gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
}

xtext_entry_state
gtk_xtext_entry_get_state (textentry *ent)
{
	return ent ? (xtext_entry_state)ent->state : XTEXT_STATE_NORMAL;
}

const unsigned char *
gtk_xtext_entry_get_str (textentry *ent)
{
	return ent ? ent->str : NULL;
}

int
gtk_xtext_entry_get_str_len (textentry *ent)
{
	return ent ? ent->str_len : 0;
}

int
gtk_xtext_entry_get_left_len (textentry *ent)
{
	return ent ? ent->left_len : -1;
}

time_t
gtk_xtext_entry_get_stamp (textentry *ent)
{
	return ent ? ent->stamp : 0;
}

void
gtk_xtext_entry_set_redaction_info (xtext_buffer *buf, textentry *ent,
                                    const char *original_str, int original_len,
                                    const char *redacted_by, const char *reason,
                                    time_t redact_time)
{
	if (!ent || ent->redaction)
		return;  /* already has redaction info — don't overwrite */

	ent->redaction = g_new0 (xtext_redaction_info, 1);
	ent->redaction->original_content = g_strndup (original_str, original_len);
	ent->redaction->original_len = original_len;
	ent->redaction->redacted_by = g_strdup (redacted_by);
	ent->redaction->redaction_reason = (reason && *reason) ? g_strdup (reason) : NULL;
	ent->redaction->redaction_time = redact_time;
}

/* Lookup the textentry at a given y pixel coordinate.
 * Used by context menus to identify which message was clicked.
 */
textentry *
gtk_xtext_get_entry_at_y (GtkXText *xtext, int y)
{
	return gtk_xtext_find_char (xtext, 0, y, NULL, NULL);
}

/* IRCv3 reactions: add a reaction from nick to this entry.
 * If the same nick already reacted with the same text, this is a no-op.
 * Triggers redraw if the reaction state changed.
 */
void
gtk_xtext_entry_add_reaction (xtext_buffer *buf, textentry *ent,
                              const char *reaction_text, const char *nick,
                              gboolean is_self)
{
	xtext_reactions_info *ri;
	xtext_reaction *reaction = NULL;
	guint i;

	/* Ephemeral entries (system output) are not reactable */
	if (ent->flags & TEXTENTRY_FLAG_EPHEMERAL)
		return;

	if (!ent || !reaction_text || !nick)
		return;

	/* Lazy-allocate reactions info */
	if (!ent->reactions)
	{
		ri = g_new0 (xtext_reactions_info, 1);
		ri->reactions = g_ptr_array_new ();
		ri->total_count = 0;
		ent->reactions = ri;
	}
	ri = ent->reactions;

	/* Find existing reaction with same text */
	for (i = 0; i < ri->reactions->len; i++)
	{
		xtext_reaction *r = g_ptr_array_index (ri->reactions, i);
		if (!strcmp (r->text, reaction_text))
		{
			reaction = r;
			break;
		}
	}

	/* Create new reaction entry if not found */
	if (!reaction)
	{
		reaction = g_new0 (xtext_reaction, 1);
		reaction->text = g_strdup (reaction_text);
		reaction->nicks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
		reaction->count = 0;
		g_ptr_array_add (ri->reactions, reaction);
	}

	/* Add nick if not already present */
	if (!g_hash_table_contains (reaction->nicks, nick))
	{
		g_hash_table_insert (reaction->nicks, g_strdup (nick),
		                     GINT_TO_POINTER (is_self ? 1 : 0));
		reaction->count++;
		ri->total_count++;

		/* Grow the entry by one badge row. calc_lines_virtual_ex skips
		 * entries whose sublines_width still matches window_width, so a
		 * gtk_xtext_calc_lines call here would not pick up the change —
		 * num_lines comes from totalweight234 of entry_tree. Update
		 * display_lines, tree weight, and num_lines by hand. */
		if (ent->extra_lines_below == 0)
		{
			gboolean was_at_bottom = FALSE;
			if (buf && buf->xtext && buf->xtext->buffer == buf)
			{
				gdouble v = gtk_adjustment_get_value (buf->xtext->adj);
				gdouble u = gtk_adjustment_get_upper (buf->xtext->adj);
				gdouble ps = gtk_adjustment_get_page_size (buf->xtext->adj);
				was_at_bottom = buf->scroll_anchor.anchor_to_bottom
					|| v >= u - ps - 1.0;
			}
			ent->extra_lines_below = 1;
			ent->display_lines += 1;
			if (buf && buf->entry_tree)
				update_weight234 (buf->entry_tree, ent, 1);
			if (buf)
			{
				buf->num_lines += 1;
				if (buf->xtext)
				{
					gtk_xtext_adjustment_set (buf, TRUE);
					if (was_at_bottom)
					{
						buf->scroll_anchor.anchor_to_bottom = TRUE;
						gtk_adjustment_set_value (buf->xtext->adj,
							gtk_adjustment_get_upper (buf->xtext->adj) -
							gtk_adjustment_get_page_size (buf->xtext->adj));
					}
				}
			}
		}
		if (buf && buf->xtext)
			gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
	}
}

/* IRCv3 reactions: remove a reaction from nick on this entry.
 * If the reaction text or nick is not found, this is a no-op.
 */
void
gtk_xtext_entry_remove_reaction (xtext_buffer *buf, textentry *ent,
                                 const char *reaction_text, const char *nick)
{
	xtext_reactions_info *ri;
	guint i;

	if (!ent || !ent->reactions || !reaction_text || !nick)
		return;

	ri = ent->reactions;

	for (i = 0; i < ri->reactions->len; i++)
	{
		xtext_reaction *r = g_ptr_array_index (ri->reactions, i);
		if (!strcmp (r->text, reaction_text))
		{
			if (g_hash_table_remove (r->nicks, nick))
			{
				r->count--;
				ri->total_count--;

				/* Remove empty reaction entry */
				if (r->count <= 0)
				{
					g_free (r->text);
					g_hash_table_destroy (r->nicks);
					g_free (r);
					g_ptr_array_remove_index (ri->reactions, i);
				}

				/* Symmetric with add: shrink display_lines, tree weight,
				 * and num_lines so adj->upper tracks the new height. */
				if (ri->total_count <= 0)
				{
					ent->extra_lines_below = 0;
					ent->display_lines -= 1;
					if (buf && buf->entry_tree)
						update_weight234 (buf->entry_tree, ent, -1);
					if (buf)
					{
						buf->num_lines -= 1;
						if (buf->xtext)
						{
							gtk_xtext_adjustment_set (buf, TRUE);
							if (buf->scroll_anchor.anchor_to_bottom)
								gtk_adjustment_set_value (buf->xtext->adj,
									gtk_adjustment_get_upper (buf->xtext->adj) -
									gtk_adjustment_get_page_size (buf->xtext->adj));
						}
					}
				}

				if (buf && buf->xtext)
					gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
			}
			return;
		}
	}
}

/* IRCv3 reply context: set reply metadata on an entry.
 * Called when an incoming PRIVMSG/NOTICE has a +draft/reply tag.
 */
void
gtk_xtext_entry_set_reply (xtext_buffer *buf, textentry *ent,
                           const char *target_msgid, const char *target_nick,
                           const char *target_preview, guint64 target_entry_id)
{
	if (!ent || !target_msgid)
		return;

	/* Ephemeral entries (system output) are not replyable */
	if (ent->flags & TEXTENTRY_FLAG_EPHEMERAL)
		return;

	/* Don't overwrite existing reply info */
	if (ent->reply)
		return;

	ent->reply = g_new0 (xtext_reply_info, 1);
	ent->reply->target_msgid = g_strdup (target_msgid);
	ent->reply->target_entry_id = target_entry_id;
	ent->reply->target_nick = target_nick ? g_strdup (target_nick) : NULL;
	ent->reply->target_preview = target_preview ? g_strdup (target_preview) : NULL;

	{
		int new_above = (ent->flags & TEXTENTRY_FLAG_DAY_BOUNDARY) ? 2 : 1;
		int delta = new_above - ent->extra_lines_above;
		ent->extra_lines_above = new_above;

		if (delta != 0)
		{
			gboolean was_at_bottom = FALSE;
			if (buf && buf->xtext && buf->xtext->buffer == buf)
			{
				gdouble v = gtk_adjustment_get_value (buf->xtext->adj);
				gdouble u = gtk_adjustment_get_upper (buf->xtext->adj);
				gdouble ps = gtk_adjustment_get_page_size (buf->xtext->adj);
				was_at_bottom = buf->scroll_anchor.anchor_to_bottom
					|| v >= u - ps - 1.0;
			}
			ent->display_lines += delta;
			if (buf && buf->entry_tree)
				update_weight234 (buf->entry_tree, ent, delta);
			if (buf)
			{
				buf->num_lines += delta;
				if (buf->xtext)
				{
					gtk_xtext_adjustment_set (buf, TRUE);
					if (was_at_bottom)
					{
						buf->scroll_anchor.anchor_to_bottom = TRUE;
						gtk_adjustment_set_value (buf->xtext->adj,
							gtk_adjustment_get_upper (buf->xtext->adj) -
							gtk_adjustment_get_page_size (buf->xtext->adj));
					}
				}
			}
		}
	}
	if (buf && buf->xtext)
		gtk_widget_queue_draw (GTK_WIDGET (buf->xtext));
}

/* IRCv3 reply context: read-only accessors */
const struct xtext_reply_info *
gtk_xtext_entry_get_reply (textentry *ent)
{
	return ent ? ent->reply : NULL;
}

const struct xtext_reactions_info *
gtk_xtext_entry_get_reactions (textentry *ent)
{
	return ent ? ent->reactions : NULL;
}

/* Check whether any reaction on this entry belongs to the given nick */
gboolean
gtk_xtext_entry_has_self_reaction (textentry *ent, const char *reaction_text)
{
	xtext_reaction *r;
	guint i;

	if (!ent || !ent->reactions || !reaction_text)
		return FALSE;

	for (i = 0; i < ent->reactions->reactions->len; i++)
	{
		r = g_ptr_array_index (ent->reactions->reactions, i);
		if (!strcmp (r->text, reaction_text))
		{
			GHashTableIter iter;
			gpointer key, value;
			g_hash_table_iter_init (&iter, r->nicks);
			while (g_hash_table_iter_next (&iter, &key, &value))
			{
				if (GPOINTER_TO_INT (value))
					return TRUE;
			}
			return FALSE;
		}
	}
	return FALSE;
}

/* IRCv3 modernization: scroll anchor system (Phase 2)
 * These functions allow saving and restoring scroll position across buffer
 * modifications (prepend, insert, delete). Uses entry_id for stability.
 */

/**
 * Calculate the line number for an entry (sum of sublines of all previous entries).
 * Used for scroll anchor restoration.
 *
 * @param buf The xtext buffer
 * @param target_ent The entry to find the line number for
 * @return Line number (0-based), or -1 if entry not found
 */
int
gtk_xtext_entry_get_line (xtext_buffer *buf, textentry *target_ent)
{
	if (!buf || !target_ent)
		return -1;

	/* O(log n) lookup via counted B-tree */
	if (buf->entry_tree)
	{
		int pos = weight_pos234 (buf->entry_tree, target_ent);
		if (pos >= 0)
			return pos;
	}

	/* Fallback: linear walk */
	{
		textentry *ent;
		int lines = 0;

		ent = buf->text_first;
		while (ent)
		{
			if (ent == target_ent)
				return lines;

			lines += ent->display_lines;

			ent = ent->next;
		}
	}

	return -1;  /* Entry not found in buffer */
}

/**
 * Save the current scroll position as an anchor.
 * The anchor uses entry_id for stability - it will survive buffer modifications.
 *
 * @param buf The xtext buffer to save scroll position from
 * @param anchor Output struct to store the anchor state
 */
void
gtk_xtext_save_scroll_anchor (xtext_buffer *buf, xtext_scroll_anchor *anchor)
{
	if (!buf || !anchor)
		return;

	/* Initialize anchor */
	anchor->anchor_entry_id = 0;
	anchor->subline_offset = 0;
	anchor->pixel_offset = 0;
	anchor->anchor_to_bottom = FALSE;

	/* Special case: if scrolled to bottom, just anchor to bottom */
	if (buf->scroll_anchor.anchor_to_bottom)
	{
		anchor->anchor_to_bottom = TRUE;
		return;
	}

	/* Anchor to the center of the viewport — stays visually stable during
	 * width-change resize even when line counts shift from re-wrapping. */
	if (buf->xtext && buf->xtext->adj)
	{
		int subline;
		gdouble adj_val = gtk_adjustment_get_value (buf->xtext->adj);
		gdouble page = gtk_adjustment_get_page_size (buf->xtext->adj);
		int center_line = (int)(adj_val + page / 2.0);
		textentry *ent;

		if (center_line < 0)
			center_line = 0;
		ent = gtk_xtext_nth (buf->xtext, center_line, &subline);
		if (ent)
		{
			anchor->anchor_entry_id = ent->entry_id;
			anchor->subline_offset = subline;
			anchor->pixel_offset = 0;
		}
	}
}

/**
 * Restore scroll position from a saved anchor.
 * After buffer modifications (prepend, insert, delete), call this to restore
 * the user's view to approximately the same position.
 *
 * @param buf The xtext buffer to restore scroll position to
 * @param anchor The anchor state previously saved with gtk_xtext_save_scroll_anchor
 */
void
gtk_xtext_restore_scroll_anchor (xtext_buffer *buf, const xtext_scroll_anchor *anchor)
{
	GtkAdjustment *adj;
	textentry *ent;
	int target_line;
	gdouble new_value, upper, page_size;

	if (!buf || !anchor || !buf->xtext)
		return;

	adj = buf->xtext->adj;
	if (!adj)
		return;

	/* Special case: anchor to bottom */
	if (anchor->anchor_to_bottom)
	{
		buf->scroll_anchor.anchor_to_bottom = TRUE;
		upper = gtk_adjustment_get_upper (adj);
		page_size = gtk_adjustment_get_page_size (adj);
		gtk_adjustment_set_value (adj, upper - page_size);
		return;
	}

	/* Find the anchor entry by ID */
	if (anchor->anchor_entry_id == 0)
		return;  /* Invalid anchor */

	ent = gtk_xtext_find_by_id (buf, anchor->anchor_entry_id);

	if (!ent)
	{
		/* Anchor entry was deleted or evicted between save and restore.
		 * Without a fallback the scrollbar value stays in the pre-mutation
		 * coordinate space and resolves to a wildly different entry on next
		 * render — this is the multi-day-jump symptom.  Future enhancement:
		 * find entry with nearest timestamp.  For now, warn loudly so the
		 * symptom is visible in the log instead of a silent visual glitch. */
		XT_DBG ("xtext: scroll anchor (center) entry_id %" G_GUINT64_FORMAT
			" not found after mutation; scroll position lost\n",
			anchor->anchor_entry_id);
		return;
	}

	/* Calculate the absolute line number for this entry.
	 * entry_get_line returns relative to text_first; in virtual mode,
	 * add LINES_BEFORE_MAT to get the absolute adjustment position. */
	target_line = gtk_xtext_entry_get_line (buf, ent);
	if (target_line < 0)
		return;  /* Entry not found (shouldn't happen) */
	target_line += LINES_BEFORE_MAT (buf);

	/* Add subline offset, clamped to the entry's current subline count
	 * (wrap points may have changed after reflow at a new width) */
	{
		int text_sublines = (int)g_slist_length (ent->sublines) + 1;
		int clamped = anchor->subline_offset;
		if (clamped >= text_sublines)
			clamped = text_sublines - 1;
		if (clamped < 0)
			clamped = 0;
		target_line += clamped;
	}

	/* Position the anchor entry at the CENTER of the viewport.
	 * Keeps the user's focus point visually stable during resize. */
	upper = gtk_adjustment_get_upper (adj);
	page_size = gtk_adjustment_get_page_size (adj);
	new_value = (gdouble)target_line - page_size / 2.0;

	if (new_value > upper - page_size)
		new_value = upper - page_size;
	if (new_value < 0)
		new_value = 0;

	/* Update anchor_to_bottom flag based on new position */
	if (new_value >= upper - page_size - 1.0)
	{
		buf->scroll_anchor.anchor_to_bottom = TRUE;
	}
	else
	{
		buf->scroll_anchor.anchor_to_bottom = FALSE;
	}

	/* Set the new scroll position */
	gtk_adjustment_set_value (adj, new_value);

}

/* Top-based anchor save: anchor to the top-visible entry (not center).
 * Used by mutation operations (prepend, insert, eviction) where the user
 * expects the top of the viewport to stay put. */
void
gtk_xtext_save_scroll_anchor_top (xtext_buffer *buf, xtext_scroll_anchor *anchor)
{
	if (!buf || !anchor)
		return;

	anchor->anchor_entry_id = 0;
	anchor->subline_offset = 0;
	anchor->pixel_offset = 0;
	anchor->anchor_to_bottom = FALSE;

	if (buf->scroll_anchor.anchor_to_bottom)
	{
		anchor->anchor_to_bottom = TRUE;
		return;
	}

	if (buf->xtext && buf->xtext->adj)
	{
		int subline;
		int top_line = (int) gtk_adjustment_get_value (buf->xtext->adj);
		textentry *ent;

		if (top_line < 0)
			top_line = 0;
		ent = gtk_xtext_nth (buf->xtext, top_line, &subline);
		if (ent)
		{
			anchor->anchor_entry_id = ent->entry_id;
			anchor->subline_offset = subline;
		}
	}
}

/* Top-based anchor restore: position the anchor entry at the TOP of the
 * viewport.  Keeps the user's top-visible content stable after mutations. */
void
gtk_xtext_restore_scroll_anchor_top (xtext_buffer *buf, const xtext_scroll_anchor *anchor)
{
	GtkAdjustment *adj;
	textentry *ent;
	int target_line;
	gdouble new_value, upper, page_size;

	if (!buf || !anchor || !buf->xtext)
		return;

	adj = buf->xtext->adj;
	if (!adj)
		return;

	if (anchor->anchor_to_bottom)
	{
		buf->scroll_anchor.anchor_to_bottom = TRUE;
		upper = gtk_adjustment_get_upper (adj);
		page_size = gtk_adjustment_get_page_size (adj);
		gtk_adjustment_set_value (adj, upper - page_size);
		return;
	}

	if (anchor->anchor_entry_id == 0)
		return;

	ent = gtk_xtext_find_by_id (buf, anchor->anchor_entry_id);
	if (!ent)
	{
		/* See restore_scroll_anchor (center variant) for rationale —
		 * silent failure here was a primary cause of the multi-day jump. */
		XT_DBG ("xtext: scroll anchor (top) entry_id %" G_GUINT64_FORMAT
			" not found after mutation; scroll position lost\n",
			anchor->anchor_entry_id);
		return;
	}

	target_line = gtk_xtext_entry_get_line (buf, ent);
	if (target_line < 0)
		return;
	target_line += LINES_BEFORE_MAT (buf);

	/* Clamp subline offset */
	{
		int text_sublines = ent->sublines ? (int)g_slist_length (ent->sublines) : 1;
		int clamped = anchor->subline_offset;
		if (clamped >= text_sublines)
			clamped = text_sublines - 1;
		if (clamped < 0)
			clamped = 0;
		target_line += clamped;
	}

	/* Position at TOP of viewport (not center) */
	upper = gtk_adjustment_get_upper (adj);
	page_size = gtk_adjustment_get_page_size (adj);
	new_value = (gdouble) target_line;

	if (new_value > upper - page_size)
		new_value = upper - page_size;
	if (new_value < 0)
		new_value = 0;

	if (new_value >= upper - page_size - 1.0)
	{
		buf->scroll_anchor.anchor_to_bottom = TRUE;
	}
	else
	{
		buf->scroll_anchor.anchor_to_bottom = FALSE;
	}

	gtk_adjustment_set_value (adj, new_value);
}

GtkWidget *
gtk_xtext_get_scrollbar (GtkXText *xtext)
{
	return xtext->scrollbar;
}
