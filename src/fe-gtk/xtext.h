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

#ifndef POXCHAT_XTEXT_H
#define POXCHAT_XTEXT_H

#include <gtk/gtk.h>
#include "gtk-helpers.h"
#include "xtext-render.h"   /* ATTR_*, XTEXT_*, format span types, rendering functions */
#include "tree234.h"

#define VIRT_MAT_WINDOW 500				/* virtual scrollback materialization window (entries) */

/* Tree-derived buffer metrics (O(1) — read from root node aggregate data).
 * These replace the manually-tracked lines_mat and mat_count fields. */
#define BUF_LINES_MAT(buf) (totalweight234((buf)->entry_tree))
#define BUF_MAT_COUNT(buf) (count234((buf)->entry_tree))

/* Estimated lines above the materialized window.  Absorptive: initialized
 * from mat_first_index * avg_lines_per_entry, then refined as entries
 * are loaded (subtract actual) or evicted (add actual). */
#define LINES_BEFORE_MAT(buf) ((buf)->lines_before_mat)

/* TRUE when buffer has SQLite-backed scrollback (DB paging, ensure_range).
 * Rendering optimizations (lazy reflow, bottom-up) apply to ALL buffers. */
#define HAS_VIRT_DB(buf) ((buf)->virt_db != NULL)

#define GTK_TYPE_XTEXT              (gtk_xtext_get_type ())
#define GTK_XTEXT(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), GTK_TYPE_XTEXT, GtkXText))
#define GTK_XTEXT_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GTK_TYPE_XTEXT, GtkXTextClass))
#define GTK_IS_XTEXT(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), GTK_TYPE_XTEXT))
#define GTK_IS_XTEXT_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GTK_TYPE_XTEXT))
#define GTK_XTEXT_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_XTEXT, GtkXTextClass))

typedef enum {
	XTEXT_STATE_NORMAL   = 0,
	XTEXT_STATE_PENDING  = 1,	/* Echo-message: awaiting server confirmation */
	XTEXT_STATE_REDACTED = 2,	/* Message-redaction: content replaced */
	XTEXT_STATE_REDACTED_PROMPT  = 3,	/* "Click again to reveal" prompt shown */
	XTEXT_STATE_REDACTED_REVEALED = 4	/* Original content revealed */
} xtext_entry_state;

typedef struct _GtkXText GtkXText;
typedef struct _GtkXTextClass GtkXTextClass;
typedef struct textentry textentry;

/* Bottom status strip: keyed items with priority-based two-zone layout */
#define XTEXT_STATUS_MAX_ITEMS 4
#define XTEXT_STATUS_PRIORITY_RIGHT 100  /* >= this anchors right zone */

typedef struct xtext_status_item {
	char *key;           /* unique id: "typing", "lag", etc. */
	char *display_text;  /* rendered text */
	int priority;        /* >= 100 = right zone, < 100 = left zone */
	gint64 expire_at;    /* g_get_monotonic_time() deadline, 0 = manual */
	void (*dismiss_cb) (GtkXText *xtext, const char *key, gpointer userdata);
	gpointer dismiss_userdata;
	int dismiss_x;       /* left edge of × button (set during render) */
	int dismiss_w;       /* width of × button area */
	unsigned int placeholder:1;  /* reserves strip space, draws no text;
	                                swept on next append to visible buffer */
} xtext_status_item;

/* Top toast overlay notifications */
#define XTEXT_TOAST_MAX 5

typedef enum {
	TOAST_ENTERING,   /* sliding down */
	TOAST_VISIBLE,    /* lingering */
	TOAST_EXITING     /* fading out */
} xtext_toast_phase;

typedef enum {
	TOAST_TYPE_INFO,      /* general/default - neutral */
	TOAST_TYPE_NICK,      /* nick changes */
	TOAST_TYPE_TOPIC,     /* topic changes */
	TOAST_TYPE_MODE,      /* mode changes */
	TOAST_TYPE_JOIN,      /* join/part (when hidden) */
	TOAST_TYPE_ERROR,     /* errors/warnings */
	TOAST_TYPE_SUCCESS    /* confirmations - green */
} xtext_toast_type;

#define TOAST_FLAG_STICKY  0x01  /* no auto-dismiss, exempt from eviction */

typedef struct xtext_toast {
	char *text;
	xtext_toast_phase phase;
	xtext_toast_type type;
	unsigned int flags;
	gint64 phase_start;      /* monotonic time when phase began */
	double alpha;            /* computed each frame */
	double y_offset;         /* computed each frame (slide-in) */
	int rendered_height;     /* cached Pango measurement */
	int linger_ms;           /* per-toast linger duration */
} xtext_toast;

/*
 * offsets_t is used for retaining search information.
 * It is stored in the 'data' member of a GList,
 * as chained from ent->marks.  It saves starting and
 * ending+1 offset of a found occurrence.
 */
typedef union offsets_u {
	struct offsets_s {
		guint16	start;
		guint16	end;
	} o;
	guint32 u;
} offsets_t;

typedef enum marker_reset_reason_e {
	MARKER_WAS_NEVER_SET,
	MARKER_IS_SET,
	MARKER_RESET_MANUALLY,
	MARKER_RESET_BY_KILL,
	MARKER_RESET_BY_CLEAR
} marker_reset_reason;

/* IRCv3 modernization: scroll anchor for stable scroll position (Phase 2)
 * Used to preserve scroll position across buffer modifications (prepend, insert, delete).
 * Uses entry_id for stability - pointers can become invalid during modifications.
 */
typedef struct xtext_scroll_anchor {
	guint64 anchor_entry_id;   /* Entry ID to anchor to (0 = not set/invalid) */
	int subline_offset;        /* Subline within entry (0 = first subline) */
	int pixel_offset;          /* Pixel offset for smooth scroll preservation */
	gboolean anchor_to_bottom; /* Special: always show newest (ignores entry_id) */
} xtext_scroll_anchor;

/* Cached display data for a single entry — owns sublines and PangoLayout. */
typedef struct {
	guint64 entry_id;			/* key for lookup */
	int cached_width;			/* window_width when layout was created */
	int display_lines;			/* line count at this width */
	GSList *sublines;			/* wrap points (GINT_TO_POINTER byte offsets) */
	PangoLayout *layout;		/* shaped message text (NULL if not cached) */
	int layout_text_offset;		/* stripped_str offset where layout text starts */
	int layout_n_lines;			/* pango_layout_get_line_count(layout) */
} xtext_line_display;

/* LRU cache for display data.  Manages PangoLayout and sublines lifecycle.
 * Entries outside the LRU window have their layouts freed, triggering
 * lazy Pango recompute on next render. */
typedef struct {
	GHashTable *by_id;		/* guint64 entry_id → GList* node in lru_order */
	GQueue lru_order;		/* xtext_line_display*, MRU at head, LRU at tail */
	int max_entries;		/* capacity */
} xtext_display_cache;

#define DISPLAY_CACHE_SIZE 200	/* ~4x typical viewport */

typedef struct {
	GtkXText *xtext;					/* attached to this widget */

	gfloat old_value;					/* last processed adj->value (change gate only) */
	textentry *text_first;
	textentry *text_last;
	tree234 *entry_tree;			/* counted B-tree for O(log n) positional access */
	xtext_display_cache *display_cache;	/* LRU tracker for sublines lifecycle */

	guint64 last_ent_start_id;	  /* this basically describes the last rendered */
	guint64 last_ent_end_id;	  /* selection (entry_ids, 0 = not set). */
	int last_offset_start;
	int last_offset_end;

	int last_pixel_pos;

	int pagetop_subline;
	textentry *pagetop_ent;			/* what's at xtext->adj->value */

	xtext_scroll_anchor scroll_anchor;	/* persistent scroll position (source of truth) */

	int num_lines;
	int indent;						  /* position of separator (pixels) from left */
	int last_indent;				  /* indent value when sublines were last computed */

	guint64 marker_pos_id;				/* entry_id of first unread message (0 = not set) */
	marker_reset_reason marker_state;

	int window_width;				/* window size when last rendered. */
	int window_height;

	unsigned int time_stamp:1;
	unsigned int needs_recalc:1;
	unsigned int marker_seen:1;
	unsigned int server_read_marker:1;	/* server has draft/read-marker — suppress local auto-advance */

	GList *search_found;		/* list of textentries where search found strings */
	gchar *search_text;		/* desired text to search for */
	gchar *search_nee;		/* prepared needle to look in haystack for */
	gint search_lnee;		/* its length */
	gtk_xtext_search_flags search_flags;	/* match, bwd, highlight */
	GList *cursearch;			/* GList whose 'data' pts to current textentry */
	GList *curmark;			/* current item in ent->marks */
	offsets_t curdata;		/* current offset info, from *curmark */
	GRegex *search_re;		/* Compiled regular expression */
	guint64 hintsearch_id;	/* entry_id found for last search (0 = not set) */

	/* Virtual mode: DB-backed search results (Phase 7a) */
	GArray *search_virt_ids;	/* ordered array of matching DB rowids (gint64) */
	int search_virt_pos;		/* current index in search_virt_ids (-1 = none) */
	int search_virt_db_total;	/* total_entries at last scan (detect DB growth) */

	/* IRCv3 modernization: entry identification (Phase 1) */
	GHashTable *entries_by_msgid;	/* msgid string → textentry* for O(1) lookup */
	GHashTable *entries_by_id;		/* entry_id → textentry* for O(1) lookup */
	guint64 next_entry_id;			/* monotonic counter for generating entry IDs */
	guint64 current_group_id;		/* non-zero during multiline output; entries inherit this */

	/* DB-backed scrollback */
	unsigned int batch_mode:1;	/* TRUE during bulk insert — suppress per-entry renders */
	xtext_scroll_anchor batch_anchor;	/* scroll anchor saved at batch start */
	textentry *insert_hint;			/* cursor for sorted batch inserts (cleared at batch end) */
	int insert_hint_lines;			/* cumulative display lines up to insert_hint */
	void *virt_db;					/* scrollback_db* (void* to avoid header dependency) */
	char *virt_channel;				/* channel name for DB queries */

	int total_entries;				/* total messages in DB for this channel */
	int mat_first_index;			/* 0-based index of text_first in total order */
	int ephemeral_count;			/* materialized entries without DB rows (pinned) */

	int lines_before_mat;			/* absorptive: estimated lines above text_first */
	double avg_lines_per_entry;		/* running average (uses ENT_DISPLAY_LINES) */

	guint64 sel_pin_start_id;		/* entry_id pinned by selection (0=none) */
	guint64 sel_pin_end_id;

	gint64 pending_db_rowid;		/* DB rowid to use as entry_id for next entry (Phase 4) */
} xtext_buffer;

typedef struct _xtext_emoji_cache xtext_emoji_cache;

struct _GtkXText
{
	GtkWidget widget;

	xtext_buffer *buffer;
	xtext_buffer *orig_buffer;
	xtext_buffer *selection_buffer;

	GtkAdjustment *adj;
	GtkWidget *scrollbar;			/* internal vertical scrollbar */
	cairo_surface_t *pixmap;		/* background image surface, NULL = use palette[XTEXT_BG] */
	cairo_surface_t *draw_buf;		/* backing surface for drawing */
	GdkCursor *hand_cursor;
	GdkCursor *resize_cursor;

	cairo_t *cr;						/* current Cairo context for drawing operations */

	/* Colors for separator and marker lines */
	GdkRGBA light_color;
	GdkRGBA dark_color;
	GdkRGBA thin_color;

	int pixel_offset;					/* amount of pixels the top line is chopped by */
	int last_width_pango;				/* last width from backend_get_text_width_emph in Pango units */

	int last_win_x;
	int last_win_y;
	int last_win_h;
	int last_win_w;

	/* GdkGC removed in GTK3 - we use Cairo for all drawing now */
	GdkRGBA palette[XTEXT_PALETTE_SIZE];

	gint io_tag;					  /* for delayed refresh events */
	gint add_io_tag;				  /* "" when adding new text */
	gint scroll_tag;				  /* marking-scroll timeout */
	gint resize_tag;				  /* deferred line recalculation on resize */
	xtext_scroll_anchor resize_anchor;	/* scroll anchor saved before reflow */
	gulong vc_signal_tag;        /* signal handler for "value_changed" adj */

	/* Selection start anchor.  select_start_y is derived on demand from
	 * (select_start_entry_id, select_start_subline) relative to the current
	 * pagetop — see gtk_xtext_resolve_select_start_y.  Pinning y directly
	 * to pixel coordinates doesn't survive buffer mutations or anchor-
	 * driven scrolls in virtual scrollback. */
	guint64 select_start_entry_id;  /* 0 = not pinned (click below last ent) */
	int select_start_subline;
	int select_start_x;
	int select_start_y;
	int select_end_x;
	int select_end_y;

	int max_lines;

	int col_fore;
	int col_back;
	double render_alpha;			  /* 1.0 = normal, <1.0 = dimmed (pending state) */

	int depth;						  /* gdk window depth */

	textentry *hilight_ent;
	int hilight_start;
	int hilight_end;

	guint16 fontwidth[128];	  /* each char's width, only the ASCII ones */

	struct pangofont
	{
		PangoFontDescription *font;
		int ascent;
		int descent;
	} *font, pango_font;
	PangoLayout *layout;

	int fontsize;
	xtext_emoji_cache *emoji_cache;	/* NULL if emoji sprites disabled */
	int space_width;				  /* width (pixels) of the space " " character */
	int stamp_width;				  /* width of "[88:88:88]" */
	int max_auto_indent;

	unsigned char scratch_buffer[4096];

	int (*urlcheck_function) (GtkWidget * xtext, char *word);

	int jump_out_offset;	/* point at which to stop rendering */
	int jump_in_offset;	/* "" start rendering */

	int ts_x;			/* ts origin for ->bgc GC */
	int ts_y;

	int clip_x;			/* clipping (x directions) */
	int clip_x2;		/* from x to x2 */

	int clip_y;			/* clipping (y directions) */
	int clip_y2;		/* from y to y2 */

	/* various state information */
	unsigned int moving_separator:1;
	unsigned int word_select:1;
	unsigned int line_select:1;
	unsigned int button_down:1;
	unsigned int press_handled:1;	/* button_press consumed the click (e.g. reply button) */
	unsigned int dont_render:1;
	unsigned int dont_render2:1;
	unsigned int cursor_hand:1;
	unsigned int cursor_resize:1;
	unsigned int skip_border_fills:1;
	unsigned int skip_stamp:1;
	unsigned int mark_stamp:1;	/* Cut&Paste with stamps? */
	unsigned int force_stamp:1;	/* force redrawing it */
	unsigned int render_hilights_only:1;
	unsigned int in_hilight:1;
	unsigned int un_hilight:1;
	unsigned int recycle:1;
	unsigned int force_render:1;
	unsigned int color_paste:1; /* CTRL was pressed when selection finished */

	/* settings/prefs */
	unsigned int auto_indent:1;
	unsigned int thinline:1;
	unsigned int marker:1;
	unsigned int separator:1;
	unsigned int wordwrap:1;
	unsigned int ignore_hidden:1;	/* rawlog uses this */

	/* Last click info - used by GTK4 where event is not available in signal handlers */
	guint last_click_button;		/* button that was clicked (1=left, 2=middle, 3=right) */
	guint last_click_state;			/* modifier state (shift, ctrl, etc.) */
	int last_click_n_press;			/* number of clicks (1=single, 2=double, 3=triple) */
	int last_click_x;				/* click x position for GTK4 popover menus */
	int last_click_y;				/* click y position for GTK4 popover menus */

	/* Reaction badge click callback */
	char *reaction_click_msgid;		/* msgid of the message whose badge was clicked */
	char *reaction_click_text;		/* reaction text that was clicked */
	gboolean reaction_click_is_self;	/* whether user already has this reaction */
	void (*reaction_click_cb) (GtkXText *xtext, const char *msgid, const char *reaction_text,
	                           gboolean is_self, gpointer userdata);
	gpointer reaction_click_userdata;

	/* Hover buttons: reply, react-text, react-emoji, redact */
	textentry *hover_ent;			/* entry the mouse is currently over (NULL = none) */
	textentry *hover_reply_target;	/* entry referenced by hovered reply context (NULL = none) */
	int hover_btn_y;				/* top edge of hover button row */
	int hover_btn_size;				/* individual button width/height */
	guint hover_stamp_tag;			/* timer for delayed hover stamp */
	unsigned int hover_stamp_visible:1;	/* TRUE after delay fires */
	unsigned int hover_stamp_alt:1;		/* TRUE if triggered via Alt key */
	int reply_btn_x;				/* left edge of reply button */
	int react_text_btn_x;			/* left edge of react-text button */
	int react_emoji_btn_x;			/* left edge of react-emoji button */
	int redact_btn_x;				/* left edge of redact button (-1 = hidden) */
	void (*reply_button_cb) (GtkXText *xtext, const char *msgid, const char *nick, gpointer userdata);
	gpointer reply_button_userdata;
	void (*react_text_button_cb) (GtkXText *xtext, const char *msgid, const char *nick, gpointer userdata);
	gpointer react_text_button_userdata;
	void (*react_emoji_button_cb) (GtkXText *xtext, const char *msgid, const char *nick, gpointer userdata);
	gpointer react_emoji_button_userdata;
	void (*redact_button_cb) (GtkXText *xtext, const char *msgid, const char *nick, gpointer userdata);
	gpointer redact_button_userdata;
	unsigned int redact_button_visible:1;	/* set by host when server supports redaction */

	/* Redact hover-button confirmation (two-click) */
	textentry *redact_confirm_ent;	/* entry awaiting second click (NULL = none) */
	gint64 redact_confirm_time;		/* monotonic time of first click (µs) */

	/* Picker mode: click a message to grab its msgid */
	void (*picker_click_cb) (GtkXText *xtext, const char *msgid, gpointer userdata);
	gpointer picker_click_userdata;

	/* Flash highlight after scroll-to-entry */
	textentry *flash_ent;			/* entry to highlight temporarily (NULL = none) */
	guint flash_tag;				/* timeout source id for clearing flash */

	/* Scroll-to-load (chathistory) support */
	guint scroll_top_debounce_tag;	/* debounce timeout for scroll-to-top */
	int scroll_top_backoff_ms;		/* current backoff delay (exponential) */
	void (*scroll_to_top_cb) (GtkXText *xtext, gpointer userdata);
	gpointer scroll_to_top_userdata;

	/* Bottom status strip (generalized from typing indicator) */
	xtext_status_item status_items[XTEXT_STATUS_MAX_ITEMS];
	int status_item_count;
	unsigned int status_strip_visible:1;
	guint status_expire_timer;

	/* Top toast overlay notifications */
	xtext_toast *toasts[XTEXT_TOAST_MAX];
	int toast_count;
	guint toast_anim_timer;
};

struct _GtkXTextClass
{
	GtkWidgetClass parent_class;
	void (*word_click) (GtkXText * xtext, char *word, guint button, GdkModifierType state, double x, double y);
	void (*set_scroll_adjustments) (GtkXText *xtext, GtkAdjustment *hadj, GtkAdjustment *vadj);
};

GtkWidget *gtk_xtext_new (GdkRGBA palette[], int separator);
GtkWidget *gtk_xtext_get_scrollbar (GtkXText *xtext);
void gtk_xtext_append (xtext_buffer *buf, unsigned char *text, int len, time_t stamp);
void gtk_xtext_append_indent (xtext_buffer *buf,
										unsigned char *left_text, int left_len,
										unsigned char *right_text, int right_len,
										time_t stamp);
/* IRCv3 modernization: prepend for chathistory BEFORE requests (Phase 3) */
void gtk_xtext_prepend (xtext_buffer *buf, unsigned char *text, int len, time_t stamp);
void gtk_xtext_prepend_indent (xtext_buffer *buf,
										unsigned char *left_text, int left_len,
										unsigned char *right_text, int right_len,
										time_t stamp);
/* IRCv3 modernization: sorted insert for chathistory gap filling (Phase 3) */
void gtk_xtext_insert_sorted (xtext_buffer *buf, unsigned char *text, int len, time_t stamp);
textentry *gtk_xtext_insert_sorted_indent (xtext_buffer *buf,
										unsigned char *left_text, int left_len,
										unsigned char *right_text, int right_len,
										time_t stamp);
int gtk_xtext_set_font (GtkXText *xtext, char *name);
void gtk_xtext_set_background (GtkXText * xtext, cairo_surface_t * pixmap);
void gtk_xtext_set_palette (GtkXText * xtext, GdkRGBA palette[]);
void gtk_xtext_clear (xtext_buffer *buf, int lines);
void gtk_xtext_save (GtkXText * xtext, int fh);
void gtk_xtext_refresh (GtkXText * xtext);
int gtk_xtext_lastlog (xtext_buffer *out, xtext_buffer *search_area);
textentry *gtk_xtext_search (GtkXText * xtext, const gchar *text, gtk_xtext_search_flags flags, GError **err);
void gtk_xtext_reset_marker_pos (GtkXText *xtext);
int gtk_xtext_moveto_marker_pos (GtkXText *xtext);
void gtk_xtext_scroll_to_entry (xtext_buffer *buf, textentry *target);
void gtk_xtext_calc_lines (xtext_buffer *buf, int fire_signal);
void gtk_xtext_recalc_day_boundaries (xtext_buffer *buf);
void gtk_xtext_set_marker_from_timestamp (xtext_buffer *buf, time_t timestamp);
void gtk_xtext_check_marker_visibility(GtkXText *xtext);
void gtk_xtext_set_marker_last (session *sess);

gboolean gtk_xtext_is_empty (xtext_buffer *buf);
typedef void (*GtkXTextForeach) (GtkXText *xtext, unsigned char *text, void *data);
void gtk_xtext_foreach (xtext_buffer *buf, GtkXTextForeach func, void *data);

void gtk_xtext_set_error_function (GtkXText *xtext, void (*error_function) (int));
void gtk_xtext_set_indent (GtkXText *xtext, gboolean indent);
void gtk_xtext_set_max_indent (GtkXText *xtext, int max_auto_indent);
void gtk_xtext_set_max_lines (GtkXText *xtext, int max_lines);
void gtk_xtext_enforce_mat_window (xtext_buffer *buf);
void gtk_xtext_set_show_marker (GtkXText *xtext, gboolean show_marker);
void gtk_xtext_set_show_separator (GtkXText *xtext, gboolean show_separator);
void gtk_xtext_set_thin_separator (GtkXText *xtext, gboolean thin_separator);
void gtk_xtext_set_time_stamp (xtext_buffer *buf, gboolean timestamp);
void gtk_xtext_set_urlcheck_function (GtkXText *xtext, int (*urlcheck_function) (GtkWidget *, char *));
void gtk_xtext_set_wordwrap (GtkXText *xtext, gboolean word_wrap);
void gtk_xtext_set_scroll_to_top_callback (GtkXText *xtext, void (*callback) (GtkXText *, gpointer), gpointer userdata);
void gtk_xtext_reset_scroll_top_backoff (GtkXText *xtext);
void gtk_xtext_set_reply_button_callback (GtkXText *xtext, void (*callback) (GtkXText *, const char *, const char *, gpointer), gpointer userdata);
void gtk_xtext_set_react_text_button_callback (GtkXText *xtext, void (*callback) (GtkXText *, const char *, const char *, gpointer), gpointer userdata);
void gtk_xtext_set_react_emoji_button_callback (GtkXText *xtext, void (*callback) (GtkXText *, const char *, const char *, gpointer), gpointer userdata);
void gtk_xtext_set_redact_button_callback (GtkXText *xtext, void (*callback) (GtkXText *, const char *, const char *, gpointer), gpointer userdata);
void gtk_xtext_set_reaction_click_callback (GtkXText *xtext, void (*callback) (GtkXText *, const char *, const char *, gboolean, gpointer), gpointer userdata);
void gtk_xtext_set_picker_click_callback (GtkXText *xtext, void (*callback) (GtkXText *, const char *, gpointer), gpointer userdata);

xtext_buffer *gtk_xtext_buffer_new (GtkXText *xtext);
void gtk_xtext_buffer_free (xtext_buffer *buf);
void gtk_xtext_buffer_show (GtkXText *xtext, xtext_buffer *buf, int render);
void gtk_xtext_copy_selection (GtkXText *xtext);
GType gtk_xtext_get_type (void);

/* Virtual scrollback (Phase 2) */
void gtk_xtext_buffer_set_virtual (xtext_buffer *buf, void *db, const char *channel,
                                    int total_entries, gint64 max_rowid);

/* IRCv3 modernization: entry identification (Phase 1) */
textentry *gtk_xtext_find_by_msgid (xtext_buffer *buf, const char *msgid);
textentry *gtk_xtext_find_by_id (xtext_buffer *buf, guint64 entry_id);
textentry *gtk_xtext_set_msgid (xtext_buffer *buf, textentry *ent, const char *msgid);

/* Multiline grouping: entries created while group_id is set share the same value */
void gtk_xtext_begin_group (xtext_buffer *buf);
void gtk_xtext_end_group (xtext_buffer *buf);

/* Bottom status strip */
void gtk_xtext_status_set (GtkXText *xtext, const char *key, const char *text,
                           int priority, int timeout_ms);
void gtk_xtext_status_remove (GtkXText *xtext, const char *key);
/* Convert an existing item into a placeholder: keeps the strip's reserved
 * space but draws no text. Swept automatically when a new line is appended
 * to the visible buffer. No-op if the key has no current item. */
void gtk_xtext_status_set_placeholder (GtkXText *xtext, const char *key);
void gtk_xtext_status_set_dismiss (GtkXText *xtext, const char *key,
                                   void (*cb) (GtkXText *, const char *, gpointer),
                                   gpointer userdata);
void gtk_xtext_status_clear (GtkXText *xtext);

/* Top toast overlay notifications */
void gtk_xtext_toast_show (GtkXText *xtext, const char *text, int linger_ms,
                           xtext_toast_type type, unsigned int flags);
void gtk_xtext_toast_clear (GtkXText *xtext);
guint64 gtk_xtext_get_entry_id (textentry *ent);
time_t gtk_xtext_entry_get_stamp (textentry *ent);
const char *gtk_xtext_get_msgid (textentry *ent);

/* Link position for entry insertion and materialization gating */
typedef enum {
	LINK_TAIL,    /* append at end */
	LINK_HEAD,    /* prepend at start */
	LINK_BEFORE,  /* insert using pre-set ent->prev / ent->next */
} xtext_link_position;

/* Virtual scrollback: decide whether to materialize an entry or leave it in DB.
 * Returns TRUE to materialize, FALSE to skip (bookkeeping updated internally). */
gboolean gtk_xtext_virt_should_materialize (xtext_buffer *buf, time_t stamp,
                                            xtext_link_position dir);

/* Entry accessors (textentry is opaque outside xtext.c) */
textentry *gtk_xtext_buffer_get_last (xtext_buffer *buf);
textentry *gtk_xtext_buffer_get_first (xtext_buffer *buf);
textentry *gtk_xtext_entry_get_next (textentry *ent);
textentry *gtk_xtext_entry_get_prev (textentry *ent);

/* IRCv3 modernization: entry modification (Phase 4) */
gboolean gtk_xtext_entry_set_text (xtext_buffer *buf, textentry *ent,
                                   const unsigned char *new_text, int new_len);
void gtk_xtext_entry_set_state (xtext_buffer *buf, textentry *ent,
                                xtext_entry_state new_state);
xtext_entry_state gtk_xtext_entry_get_state (textentry *ent);

/* Redaction accountability */
void gtk_xtext_entry_set_redaction_info (xtext_buffer *buf, textentry *ent,
                                         const char *original_str, int original_len,
                                         const char *redacted_by, const char *reason,
                                         time_t redact_time);

/* IRCv3 reactions and reply context */
struct xtext_reactions_info;
struct xtext_reply_info;
struct xtext_reaction;

void gtk_xtext_entry_add_reaction (xtext_buffer *buf, textentry *ent,
                                   const char *reaction_text, const char *nick,
                                   gboolean is_self);
void gtk_xtext_entry_remove_reaction (xtext_buffer *buf, textentry *ent,
                                      const char *reaction_text, const char *nick);
void gtk_xtext_entry_set_reply (xtext_buffer *buf, textentry *ent,
                                const char *target_msgid, const char *target_nick,
                                const char *target_preview, guint64 target_entry_id);
const struct xtext_reply_info *gtk_xtext_entry_get_reply (textentry *ent);
const struct xtext_reactions_info *gtk_xtext_entry_get_reactions (textentry *ent);
gboolean gtk_xtext_entry_has_self_reaction (textentry *ent, const char *reaction_text);

/* Lookup entry by click position (for context menus) */
textentry *gtk_xtext_get_entry_at_y (GtkXText *xtext, int y);

/* Read accessors (textentry is opaque outside xtext.c) */
const unsigned char *gtk_xtext_entry_get_str (textentry *ent);
int gtk_xtext_entry_get_str_len (textentry *ent);
int gtk_xtext_entry_get_left_len (textentry *ent);

/* IRCv3 modernization: scroll anchor system (Phase 2)
 * Save/restore scroll position across buffer modifications.
 * Uses entry_id for stability - survives prepend, insert, delete operations.
 */
void gtk_xtext_save_scroll_anchor (xtext_buffer *buf, xtext_scroll_anchor *anchor);
void gtk_xtext_restore_scroll_anchor (xtext_buffer *buf, const xtext_scroll_anchor *anchor);
void gtk_xtext_save_scroll_anchor_top (xtext_buffer *buf, xtext_scroll_anchor *anchor);
void gtk_xtext_restore_scroll_anchor_top (xtext_buffer *buf, const xtext_scroll_anchor *anchor);

/* Calculate line number for an entry (needed for scroll anchor restoration) */
int gtk_xtext_entry_get_line (xtext_buffer *buf, textentry *ent);

#endif
