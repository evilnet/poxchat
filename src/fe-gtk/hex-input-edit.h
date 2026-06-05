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

/* hex-input-edit.h — Custom editable input widget using xtext's rendering
 *                    pipeline (Pango + Cairo + emoji sprites).
 *
 * Replaces the GtkTextView-based HexInputView with a from-scratch widget
 * that has full control over text layout, emoji rendering, and selection.
 */

#ifndef POXCHAT_HEX_INPUT_EDIT_H
#define POXCHAT_HEX_INPUT_EDIT_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define HEX_TYPE_INPUT_EDIT            (hex_input_edit_get_type ())
#define HEX_INPUT_EDIT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), HEX_TYPE_INPUT_EDIT, HexInputEdit))
#define HEX_INPUT_EDIT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), HEX_TYPE_INPUT_EDIT, HexInputEditClass))
#define HEX_IS_INPUT_EDIT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), HEX_TYPE_INPUT_EDIT))
#define HEX_IS_INPUT_EDIT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), HEX_TYPE_INPUT_EDIT))

typedef struct _HexInputEdit      HexInputEdit;
typedef struct _HexInputEditClass HexInputEditClass;
typedef struct _HexInputEditPriv  HexInputEditPriv;

struct _HexInputEdit
{
	GtkWidget parent_instance;
	HexInputEditPriv *priv;
};

struct _HexInputEditClass
{
	GtkWidgetClass parent_class;

	/* Signals */
	void     (*activate)    (HexInputEdit *edit);
	gboolean (*word_check)  (HexInputEdit *edit, const char *word);
	void     (*image_paste) (HexInputEdit *edit, GBytes *png_bytes);
};

GType      hex_input_edit_get_type     (void) G_GNUC_CONST;
GtkWidget *hex_input_edit_new          (void);

/* Text API — positions are character offsets, not byte offsets.
 * pos == -1 means "end of text". */
const char *hex_input_edit_get_text     (HexInputEdit *edit);
void        hex_input_edit_set_text     (HexInputEdit *edit, const char *text);
int         hex_input_edit_get_position (HexInputEdit *edit);
void        hex_input_edit_set_position (HexInputEdit *edit, int pos);
void        hex_input_edit_insert_text  (HexInputEdit *edit, const char *text,
                                         int len, int *pos);

/* Configuration */
void        hex_input_edit_set_max_lines (HexInputEdit *edit, int max_lines);
void        hex_input_edit_set_multiline (HexInputEdit *edit, gboolean multiline);
void        hex_input_edit_set_editable  (HexInputEdit *edit, gboolean editable);
void        hex_input_edit_set_max_chars (HexInputEdit *edit, int max_chars);
void        hex_input_edit_set_width_chars (HexInputEdit *edit, int width_chars);
void        hex_input_edit_set_max_width_chars (HexInputEdit *edit, int max_width_chars);

/* Emoji sprite cache (shared with xtext) */
typedef struct _xtext_emoji_cache xtext_emoji_cache;
void        hex_input_edit_set_emoji_cache (HexInputEdit *edit,
                                            xtext_emoji_cache *cache);

/* Palette (shared with xtext) */
void        hex_input_edit_set_palette (HexInputEdit *edit,
                                        const GdkRGBA *palette);

/* Font */
void        hex_input_edit_set_font    (HexInputEdit *edit,
                                        PangoFontDescription *font_desc);

/* Spell checking */
void        hex_input_edit_set_checked        (HexInputEdit *edit, gboolean checked);
gboolean    hex_input_edit_is_checked         (HexInputEdit *edit);
void        hex_input_edit_activate_default_languages (HexInputEdit *edit);
void        hex_input_edit_deactivate_language (HexInputEdit *edit, const gchar *lang);

/* Layout line queries (for subline navigation / history integration) */
int         hex_input_edit_get_cursor_line (HexInputEdit *edit);
int         hex_input_edit_get_line_count  (HexInputEdit *edit);

/* Move cursor up/down one layout line. Returns TRUE if moved. */
gboolean    hex_input_edit_move_cursor_up   (HexInputEdit *edit);
gboolean    hex_input_edit_move_cursor_down (HexInputEdit *edit);

G_END_DECLS

#endif /* POXCHAT_HEX_INPUT_EDIT_H */
