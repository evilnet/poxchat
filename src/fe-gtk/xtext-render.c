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

/* xtext-render.c — Shared text format parsing and Pango attribute building.
 *
 * Extracted from xtext.c so both the display widget (xtext) and the input
 * widget (hex-input-edit) can share the same rendering primitives.
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "xtext-render.h"  /* ATTR_*, XTEXT_*, format span types */
#include "xtext-emoji.h"  /* xtext_emoji_detect() */

#define charlen(str) g_utf8_skip[*(guchar *)(str)]

void
xtext_parse_formats (const unsigned char *raw, int raw_len,
                     unsigned char **stripped_out, int *stripped_len_out,
                     xtext_fmt_span **spans_out, int *span_count_out,
                     xtext_emoji_info **emoji_out, int *emoji_count_out,
                     guint16 **raw_to_stripped_out,
                     gboolean detect_emoji)
{
	unsigned char *stripped;
	xtext_fmt_span *spans;
	xtext_emoji_info *emojis;
	guint16 *r2s;
	int stripped_pos = 0;
	int span_count = 0, span_alloc = 8;
	int emoji_count = 0, emoji_alloc = 4;
	int ri = 0;  /* raw index */
	int mbl;

	/* Current formatting state */
	guint16 cur_fg = XTEXT_FG;
	guint16 cur_bg = XTEXT_BG;
	guint8  cur_emph = 0;
	guint8  cur_flags = 0;

	/* Color parsing state */
	gboolean parsing_color = FALSE;
	gboolean parsing_backcolor = FALSE;
	char num_buf[8];
	int nc = 0;

	stripped = g_malloc (raw_len + 2);  /* worst case: same size as raw */
	spans = g_new (xtext_fmt_span, span_alloc);
	emojis = detect_emoji ? g_new (xtext_emoji_info, emoji_alloc) : NULL;
	r2s = g_new (guint16, raw_len + 1);

	/* Emit first span (initial state) */
	spans[0].stripped_off = 0;
	spans[0].fg = cur_fg;
	spans[0].bg = cur_bg;
	spans[0].emph = cur_emph;
	spans[0].flags = cur_flags;
	span_count = 1;

	/* Helper macro: emit a new span if state changed */
#define EMIT_SPAN_IF_CHANGED() do { \
	xtext_fmt_span *_prev = &spans[span_count - 1]; \
	if (_prev->fg != cur_fg || _prev->bg != cur_bg || \
	    _prev->emph != cur_emph || _prev->flags != cur_flags) \
	{ \
		if (span_count >= span_alloc) { \
			span_alloc *= 2; \
			spans = g_renew (xtext_fmt_span, spans, span_alloc); \
		} \
		spans[span_count].stripped_off = (guint16) stripped_pos; \
		spans[span_count].fg = cur_fg; \
		spans[span_count].bg = cur_bg; \
		spans[span_count].emph = cur_emph; \
		spans[span_count].flags = cur_flags; \
		span_count++; \
	} \
} while (0)

	while (ri < raw_len)
	{
		mbl = charlen (raw + ri);
		if (mbl + ri > raw_len)
			break;  /* bad utf8 */

		/* --- Color digit/comma parsing (after ATTR_COLOR) --- */
		if (parsing_color &&
		    ((isdigit (raw[ri]) && nc < 2) ||
		     (raw[ri] == ',' && ri + 1 < raw_len &&
		      isdigit (raw[ri + 1]) && nc < 3 && !parsing_backcolor)))
		{
			r2s[ri] = (guint16) stripped_pos;
			if (raw[ri] == ',')
			{
				parsing_backcolor = TRUE;
				if (nc)
				{
					int col_num;
					num_buf[nc] = 0;
					nc = 0;
					col_num = atoi (num_buf);
					if (col_num == 99)
						col_num = XTEXT_FG;
					else if (col_num > XTEXT_MAX_COLOR)
						col_num = col_num % XTEXT_MIRC_COLS;
					cur_fg = (guint16) col_num;
				}
			}
			else
			{
				num_buf[nc] = raw[ri];
				if (nc < 7)
					nc++;
			}
			ri++;
			continue;
		}

		/* --- End of color digit sequence --- */
		if (parsing_color)
		{
			parsing_color = FALSE;
			if (nc)
			{
				int col_num;
				num_buf[nc] = 0;
				nc = 0;
				col_num = atoi (num_buf);
				if (parsing_backcolor)
				{
					if (col_num == 99)
						col_num = XTEXT_BG;
					else if (col_num > XTEXT_MAX_COLOR)
						col_num = col_num % XTEXT_MIRC_COLS;
					cur_bg = (guint16) col_num;
				}
				else
				{
					if (col_num == 99)
						col_num = XTEXT_FG;
					else if (col_num > XTEXT_MAX_COLOR)
						col_num = col_num % XTEXT_MIRC_COLS;
					cur_fg = (guint16) col_num;
				}
				parsing_backcolor = FALSE;
			}
			else
			{
				/* \003 followed by non-digit: reset colors */
				cur_fg = XTEXT_FG;
				cur_bg = XTEXT_BG;
			}
			EMIT_SPAN_IF_CHANGED ();
			/* Don't consume this byte — fall through to handle it */
		}

		/* --- Reply sentinel (U+FDD0, a Unicode noncharacter) --- prepended by
		 * inbound_chanmsg / inbound_action.  Emit a U+FFFC placeholder and a
		 * sentinel emoji_info so the renderer vector-draws the arrow.  Handled
		 * independently of detect_emoji so reply arrows render even without
		 * Twemoji sprites; lazily allocates the emoji array.  (raw is unsigned;
		 * cast the macro's bytes, which are signed char, before comparing.) */
		if (raw[ri] == (unsigned char) XTEXT_REPLY_SENTINEL[0] && ri + 2 < raw_len &&
		    raw[ri + 1] == (unsigned char) XTEXT_REPLY_SENTINEL[1] &&
		    raw[ri + 2] == (unsigned char) XTEXT_REPLY_SENTINEL[2])
		{
			EMIT_SPAN_IF_CHANGED ();
			if (!emojis)
				emojis = g_new (xtext_emoji_info, emoji_alloc);
			else if (emoji_count >= emoji_alloc)
			{
				emoji_alloc *= 2;
				emojis = g_renew (xtext_emoji_info, emojis, emoji_alloc);
			}
			r2s[ri] = r2s[ri + 1] = r2s[ri + 2] = (guint16) stripped_pos;
			emojis[emoji_count].stripped_off = (guint16) stripped_pos;
			g_strlcpy (emojis[emoji_count].filename, XTEXT_REPLY_ARROW_FILE,
			           sizeof (emojis[emoji_count].filename));
			emoji_count++;
			stripped[stripped_pos++] = UFFFC_BYTE0;
			stripped[stripped_pos++] = UFFFC_BYTE1;
			stripped[stripped_pos++] = UFFFC_BYTE2;
			ri += 3;
			continue;
		}

		/* --- Emoji detection --- */
		if (detect_emoji)
		{
			int emoji_bytes;
			char emoji_file[64];
			if (xtext_emoji_detect (raw + ri, raw_len - ri, &emoji_bytes, emoji_file, sizeof (emoji_file)))
			{
				EMIT_SPAN_IF_CHANGED ();
				/* Map all raw emoji bytes to this stripped position */
				for (int eb = 0; eb < emoji_bytes && ri + eb < raw_len; eb++)
					r2s[ri + eb] = (guint16) stripped_pos;

				/* Store emoji info */
				if (emoji_count >= emoji_alloc)
				{
					emoji_alloc *= 2;
					emojis = g_renew (xtext_emoji_info, emojis, emoji_alloc);
				}
				emojis[emoji_count].stripped_off = (guint16) stripped_pos;
				g_strlcpy (emojis[emoji_count].filename, emoji_file,
				           sizeof (emojis[emoji_count].filename));
				emoji_count++;

				/* Write U+FFFC placeholder */
				stripped[stripped_pos++] = UFFFC_BYTE0;
				stripped[stripped_pos++] = UFFFC_BYTE1;
				stripped[stripped_pos++] = UFFFC_BYTE2;

				ri += emoji_bytes;
				continue;
			}
		}

		/* --- Format control codes --- */
		switch (raw[ri])
		{
		case ATTR_COLOR:
			r2s[ri] = (guint16) stripped_pos;
			parsing_color = TRUE;
			parsing_backcolor = FALSE;
			nc = 0;
			ri++;
			continue;

		case ATTR_BOLD:
			r2s[ri] = (guint16) stripped_pos;
			cur_emph ^= EMPH_BOLD;
			EMIT_SPAN_IF_CHANGED ();
			ri++;
			continue;

		case ATTR_ITALICS:
			r2s[ri] = (guint16) stripped_pos;
			cur_emph ^= EMPH_ITAL;
			EMIT_SPAN_IF_CHANGED ();
			ri++;
			continue;

		case ATTR_UNDERLINE:
			r2s[ri] = (guint16) stripped_pos;
			cur_flags ^= FMT_FLAG_UNDERLINE;
			EMIT_SPAN_IF_CHANGED ();
			ri++;
			continue;

		case ATTR_STRIKETHROUGH:
			r2s[ri] = (guint16) stripped_pos;
			cur_flags ^= FMT_FLAG_STRIKETHROUGH;
			EMIT_SPAN_IF_CHANGED ();
			ri++;
			continue;

		case ATTR_HIDDEN:
			r2s[ri] = (guint16) stripped_pos;
			cur_flags ^= FMT_FLAG_HIDDEN;
			EMIT_SPAN_IF_CHANGED ();
			ri++;
			continue;

		case ATTR_REVERSE:
		{
			guint16 tmp;
			r2s[ri] = (guint16) stripped_pos;
			tmp = cur_fg;
			cur_fg = cur_bg;
			cur_bg = tmp;
			cur_flags ^= FMT_FLAG_REVERSE;
			EMIT_SPAN_IF_CHANGED ();
			ri++;
			continue;
		}

		case ATTR_RESET:
			r2s[ri] = (guint16) stripped_pos;
			cur_fg = XTEXT_FG;
			cur_bg = XTEXT_BG;
			cur_emph = 0;
			cur_flags = 0;
			EMIT_SPAN_IF_CHANGED ();
			ri++;
			continue;

		case ATTR_BEEP:
		case ATTR_ITALICS2:
			/* Consume but don't output */
			r2s[ri] = (guint16) stripped_pos;
			ri++;
			continue;

		default:
			break;
		}

		/* --- Regular character (or newline) --- */
		if (!(cur_flags & FMT_FLAG_HIDDEN))
		{
			for (int b = 0; b < mbl; b++)
			{
				r2s[ri + b] = (guint16) stripped_pos;
				stripped[stripped_pos++] = raw[ri + b];
			}
		}
		else
		{
			for (int b = 0; b < mbl; b++)
				r2s[ri + b] = (guint16) stripped_pos;
		}
		ri += mbl;
	}

	/* Map any remaining bytes (shouldn't happen, but be safe) */
	while (ri <= raw_len)
	{
		r2s[ri] = (guint16) stripped_pos;
		ri++;
	}

#undef EMIT_SPAN_IF_CHANGED

	stripped[stripped_pos] = 0;

	*stripped_out = stripped;
	*stripped_len_out = stripped_pos;
	*spans_out = spans;
	*span_count_out = span_count;
	if (emoji_out)
		*emoji_out = emojis;
	else
		g_free (emojis);
	if (emoji_count_out)
		*emoji_count_out = emoji_count;
	if (raw_to_stripped_out)
		*raw_to_stripped_out = r2s;
	else
		g_free (r2s);
}

int
xtext_stripped_to_raw (const guint16 *r2s_map, int raw_len, int stripped_off)
{
	for (int ri = 0; ri < raw_len; ri++)
	{
		if (r2s_map[ri] >= (guint16) stripped_off)
			return ri;
	}
	return raw_len;
}

int
xtext_raw_to_stripped (const guint16 *r2s_map, int raw_len, int raw_off)
{
	if (raw_off >= raw_len)
		return r2s_map[raw_len];  /* one past end */
	return r2s_map[raw_off];
}

PangoAttrList *
xtext_build_attrlist (const xtext_format_data *fdata, int sub_start, int sub_len,
                      const GdkRGBA *palette, int fontsize, int ascent)
{
	PangoAttrList *list = pango_attr_list_new ();
	int sub_end = sub_start + sub_len;

	/* Walk format spans and create Pango attributes for the subline range */
	for (int si = 0; si < fdata->fmt_span_count; si++)
	{
		const xtext_fmt_span *span = &fdata->fmt_spans[si];
		int span_end = (si + 1 < fdata->fmt_span_count)
		               ? fdata->fmt_spans[si + 1].stripped_off
		               : fdata->stripped_len;
		PangoAttribute *attr;

		/* Clip span to subline range */
		int start = MAX (span->stripped_off, sub_start) - sub_start;
		int end   = MIN (span_end, sub_end) - sub_start;

		if (start >= end)
			continue;  /* span doesn't overlap this subline */

		if (span->flags & FMT_FLAG_HIDDEN)
			continue;  /* hidden text not in stripped_str */


		/* Foreground color */
		if (span->fg != XTEXT_FG)
		{
			const GdkRGBA *c = &palette[span->fg];
			attr = pango_attr_foreground_new (
				(guint16)(c->red * 65535),
				(guint16)(c->green * 65535),
				(guint16)(c->blue * 65535));
			attr->start_index = start;
			attr->end_index = end;
			pango_attr_list_insert (list, attr);
		}

		/* Background color */
		if (span->bg != XTEXT_BG)
		{
			const GdkRGBA *c = &palette[span->bg];
			attr = pango_attr_background_new (
				(guint16)(c->red * 65535),
				(guint16)(c->green * 65535),
				(guint16)(c->blue * 65535));
			attr->start_index = start;
			attr->end_index = end;
			pango_attr_list_insert (list, attr);
		}

		/* Bold */
		if (span->emph & EMPH_BOLD)
		{
			attr = pango_attr_weight_new (PANGO_WEIGHT_BOLD);
			attr->start_index = start;
			attr->end_index = end;
			pango_attr_list_insert (list, attr);
		}

		/* Italic */
		if (span->emph & EMPH_ITAL)
		{
			attr = pango_attr_style_new (PANGO_STYLE_ITALIC);
			attr->start_index = start;
			attr->end_index = end;
			pango_attr_list_insert (list, attr);
		}

		/* Underline */
		if (span->flags & FMT_FLAG_UNDERLINE)
		{
			attr = pango_attr_underline_new (PANGO_UNDERLINE_SINGLE);
			attr->start_index = start;
			attr->end_index = end;
			pango_attr_list_insert (list, attr);
		}

		/* Strikethrough */
		if (span->flags & FMT_FLAG_STRIKETHROUGH)
		{
			attr = pango_attr_strikethrough_new (TRUE);
			attr->start_index = start;
			attr->end_index = end;
			pango_attr_list_insert (list, attr);
		}
	}

	/* Add shape attributes for emoji placeholders (U+FFFC) so Pango
	 * reserves the correct width without trying to render the glyph */
	for (int ei = 0; ei < fdata->emoji_count; ei++)
	{
		const xtext_emoji_info *em = &fdata->emoji_list[ei];
		int em_start = em->stripped_off - sub_start;
		int em_end = em_start + UFFFC_UTF8_LEN;

		if (em_start < 0 || em_start >= sub_len)
			continue;  /* emoji not in this subline */

		PangoRectangle ink = { 0, -ascent * PANGO_SCALE, fontsize * PANGO_SCALE, fontsize * PANGO_SCALE };
		PangoRectangle logical = ink;
		PangoAttribute *attr = pango_attr_shape_new (&ink, &logical);
		attr->start_index = em_start;
		attr->end_index = em_end;
		pango_attr_list_insert (list, attr);
	}

	return list;
}
