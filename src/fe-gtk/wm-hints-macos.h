/* PoxChat — macOS NSWindow bridge for wm-hints. Only present on macOS
 * builds; callers in wm-hints.c gate inclusion on GDK_WINDOWING_MACOS. */

#ifndef POXCHAT_WM_HINTS_MACOS_H
#define POXCHAT_WM_HINTS_MACOS_H

#include <glib.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

gboolean pox_macos_get_window_geometry (GdkSurface *surface,
                                        int *x, int *y, int *w, int *h);
void     pox_macos_set_window_position (GdkSurface *surface, int x, int y);

G_END_DECLS

#endif /* POXCHAT_WM_HINTS_MACOS_H */
