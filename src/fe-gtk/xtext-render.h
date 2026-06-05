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

/* xtext-render.h — Shared text format parsing and Pango attribute building.
 *
 * These functions are used by both xtext (display widget) and hex-input-edit
 * (input widget) to parse mIRC format codes and build PangoAttrLists for
 * rendering formatted text with emoji sprites.
 */

#ifndef POXCHAT_XTEXT_RENDER_H
#define POXCHAT_XTEXT_RENDER_H

#include <glib.h>
#include <pango/pango.h>
#include <gdk/gdk.h>

/* --- mIRC format control codes --- */
#define ATTR_BOLD           '\002'
#define ATTR_COLOR          '\003'
#define ATTR_BLINK          '\006'
#define ATTR_BEEP           '\007'
#define ATTR_HIDDEN         '\010'
#define ATTR_ITALICS2       '\011'
#define ATTR_RESET          '\017'
#define ATTR_REVERSE        '\026'
#define ATTR_ITALICS        '\035'
#define ATTR_STRIKETHROUGH  '\036'
#define ATTR_UNDERLINE      '\037'

/* --- Palette indices (match palette.h) --- */
#define XTEXT_MIRC_COLS    32
#define XTEXT_COLS         37   /* 32 plus 5 for extra stuff below */
#define XTEXT_MARK_FG      32  /* for marking text */
#define XTEXT_MARK_BG      33
#define XTEXT_FG            34
#define XTEXT_BG            35
#define XTEXT_MARKER        36  /* for marker line */
#define XTEXT_MAX_COLOR     41

/* State colors (xtext-internal, not from external palette[]) */
#define XTEXT_PENDING_FG   37
#define XTEXT_REDACTED_FG  38
#define XTEXT_PALETTE_SIZE 39   /* total palette array size */

/* --- Emphasis flags (used in xtext_fmt_span.emph) --- */
#define EMPH_ITAL   1
#define EMPH_BOLD   2
#define EMPH_HIDDEN 4

/* --- Format span flags (used in xtext_fmt_span.flags) --- */
#define FMT_FLAG_UNDERLINE     (1 << 0)
#define FMT_FLAG_STRIKETHROUGH (1 << 1)
#define FMT_FLAG_HIDDEN        (1 << 2)
#define FMT_FLAG_REVERSE       (1 << 3)

/* --- U+FFFC placeholder bytes (emoji replacement in stripped text) --- */
#define UFFFC_BYTE0    0xEF
#define UFFFC_BYTE1    0xBF
#define UFFFC_BYTE2    0xBC
#define UFFFC_UTF8_LEN 3

/* Compact format change record — one per formatting transition in a message.
 * Built once at parse time, reused for every render of the same text. */
typedef struct {
	guint16 stripped_off;   /* byte offset in stripped text where this span starts */
	guint16 fg;             /* foreground palette index (XTEXT_FG = default) */
	guint16 bg;             /* background palette index (XTEXT_BG = default) */
	guint8  emph;           /* EMPH_BOLD | EMPH_ITAL */
	guint8  flags;          /* FMT_FLAG_* bits */
} xtext_fmt_span;

/* Emoji placeholder info — one per emoji sequence in the message.
 * Stored alongside format spans so the renderer knows where to draw sprites. */
typedef struct {
	guint16 stripped_off;   /* byte offset of U+FFFC in stripped text */
	char    filename[48];   /* twemoji sprite filename (e.g. "1f600.png") */
} xtext_emoji_info;

/* Sentinel emoji_info filename marking the IRCv3 reply arrow placeholder.
 * The renderer vector-draws the arrow instead of loading a Twemoji sprite.
 * A leading \x01 can never be a real sprite filename, so filename[0]
 * disambiguates. */
#define XTEXT_REPLY_ARROW_FILE "\x01reply"

/* In-text reply marker prepended by inbound_chanmsg/inbound_action (common/).
 * U+FDD0 is a Unicode *noncharacter*: permanently reserved, never assigned to a
 * glyph, and invalid for interchange — so it can never collide with real text a
 * user actually typed (unlike a visible arrow like U+2BA4 would).  The strip
 * pass converts it to a U+FFFC placeholder; the renderer vector-draws the arrow
 * and copy/paste strips it back out.  inbound.c can't include this fe-gtk
 * header, so it hardcodes the same bytes — keep them in sync. */
#define XTEXT_REPLY_SENTINEL "\xef\xb7\x90"

/* Aggregated format data — passed to xtext_build_attrlist() instead of
 * requiring a full textentry struct. Both xtext and hex-input-edit populate
 * this from their respective data models. */
typedef struct {
	const unsigned char *stripped_str;
	int stripped_len;
	const xtext_fmt_span *fmt_spans;
	int fmt_span_count;
	const xtext_emoji_info *emoji_list;
	int emoji_count;
} xtext_format_data;

/* Parse raw text containing mIRC format codes into rendering data.
 *
 * Produces:
 *   stripped_out         - text with format codes removed, emoji → U+FFFC
 *   stripped_len_out     - byte length of stripped text
 *   spans_out            - array of format transition records
 *   span_count_out       - number of spans
 *   emoji_out            - emoji placeholder info (NULL if detect_emoji=FALSE)
 *   emoji_count_out      - number of emoji (0 if detect_emoji=FALSE)
 *   raw_to_stripped_out  - offset map (raw_len+1 entries), NULL-ok to skip
 *   detect_emoji         - whether to detect and replace emoji sequences
 *
 * All output arrays are g_malloc'd; caller must g_free them.
 */
void
xtext_parse_formats (const unsigned char *raw, int raw_len,
                     unsigned char **stripped_out, int *stripped_len_out,
                     xtext_fmt_span **spans_out, int *span_count_out,
                     xtext_emoji_info **emoji_out, int *emoji_count_out,
                     guint16 **raw_to_stripped_out,
                     gboolean detect_emoji);

/* Build a PangoAttrList from format data for a subline slice.
 *
 * Parameters:
 *   fdata     - pre-parsed format data (from xtext_parse_formats output)
 *   sub_start - byte offset into stripped_str where the subline begins
 *   sub_len   - byte length of the subline in stripped text
 *   palette   - GdkRGBA color palette (XTEXT_PALETTE_SIZE entries)
 *   fontsize  - pixel size for emoji shape attributes
 *   ascent    - font ascent in pixels
 *
 * Returns: a new PangoAttrList (caller must pango_attr_list_unref).
 *          Attribute byte indices are relative to the subline (0-based).
 */
PangoAttrList *
xtext_build_attrlist (const xtext_format_data *fdata, int sub_start, int sub_len,
                      const GdkRGBA *palette, int fontsize, int ascent);

/* Convert a raw-text byte offset to a stripped-text byte offset. */
int xtext_raw_to_stripped (const guint16 *r2s_map, int raw_len, int raw_off);

/* Convert a stripped-text byte offset to a raw-text byte offset.
 * Returns the first raw byte that maps to >= stripped_off. */
int xtext_stripped_to_raw (const guint16 *r2s_map, int raw_len, int stripped_off);

#endif /* POXCHAT_XTEXT_RENDER_H */
