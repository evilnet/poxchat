/* PoxChat — window-manager hint abstraction.
 *
 * Currently implements "visible on all workspaces" (sticky) for X11
 * via _NET_WM_STATE_STICKY (EWMH). Other backends are no-ops; add a
 * matching block here when implementing for Wayland/Win32/macOS.
 */

#include <string.h>

#include "fe-gtk.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xatom.h>
#endif

#ifdef GDK_WINDOWING_MACOS
#include "wm-hints-macos.h"
#endif

#include "wm-hints.h"

#ifdef GDK_WINDOWING_X11

#define _NET_WM_STATE_REMOVE  0
#define _NET_WM_STATE_ADD     1

static gboolean
wm_hints_x11_get_handles (GtkWindow *win, Display **xdisplay_out, Window *xid_out)
{
	GdkDisplay *display;
	GdkSurface *surface;

	if (!win)
		return FALSE;

	display = gtk_widget_get_display (GTK_WIDGET (win));
	if (!display || !GDK_IS_X11_DISPLAY (display))
		return FALSE;

	surface = gtk_native_get_surface (GTK_NATIVE (win));
	if (!surface || !GDK_IS_X11_SURFACE (surface))
		return FALSE;

	*xdisplay_out = gdk_x11_display_get_xdisplay (display);
	*xid_out = gdk_x11_surface_get_xid (surface);
	return *xdisplay_out != NULL && *xid_out != None;
}

#endif

gboolean
wm_hints_supports_sticky (GtkWindow *win)
{
#ifdef GDK_WINDOWING_X11
	Display *xdisplay;
	Window xid;
	return wm_hints_x11_get_handles (win, &xdisplay, &xid);
#else
	(void) win;
	return FALSE;
#endif
}

gboolean
wm_hints_get_sticky (GtkWindow *win)
{
#ifdef GDK_WINDOWING_X11
	Display *xdisplay;
	Window xid;
	Atom net_wm_state, sticky, type;
	int format;
	unsigned long nitems, bytes_after;
	unsigned char *data = NULL;
	gboolean found = FALSE;

	if (!wm_hints_x11_get_handles (win, &xdisplay, &xid))
		return FALSE;

	net_wm_state = XInternAtom (xdisplay, "_NET_WM_STATE", False);
	sticky = XInternAtom (xdisplay, "_NET_WM_STATE_STICKY", False);

	if (XGetWindowProperty (xdisplay, xid, net_wm_state, 0, 1024, False,
	                       XA_ATOM, &type, &format, &nitems, &bytes_after,
	                       &data) == Success && data && type == XA_ATOM && format == 32)
	{
		Atom *atoms = (Atom *) data;
		unsigned long i;
		for (i = 0; i < nitems; i++)
		{
			if (atoms[i] == sticky)
			{
				found = TRUE;
				break;
			}
		}
	}
	if (data)
		XFree (data);
	return found;
#else
	(void) win;
	return FALSE;
#endif
}

void
wm_hints_set_sticky (GtkWindow *win, gboolean sticky)
{
#ifdef GDK_WINDOWING_X11
	Display *xdisplay;
	Window xid, root;
	XEvent ev;
	Atom net_wm_state, sticky_atom;

	if (!wm_hints_x11_get_handles (win, &xdisplay, &xid))
		return;

	net_wm_state = XInternAtom (xdisplay, "_NET_WM_STATE", False);
	sticky_atom = XInternAtom (xdisplay, "_NET_WM_STATE_STICKY", False);
	root = DefaultRootWindow (xdisplay);

	/* EWMH: for an unmapped window, set _NET_WM_STATE directly; for a
	 * mapped window the WM ignores property writes and we must send a
	 * client message instead. Doing both is harmless and avoids races
	 * where we don't know which side of map-notify we're on. */
	{
		Atom type;
		int format;
		unsigned long nitems, bytes_after;
		unsigned char *data = NULL;
		GArray *new_state = g_array_new (FALSE, FALSE, sizeof (Atom));

		if (XGetWindowProperty (xdisplay, xid, net_wm_state, 0, 1024, False,
		                       XA_ATOM, &type, &format, &nitems, &bytes_after,
		                       &data) == Success && data && type == XA_ATOM && format == 32)
		{
			Atom *atoms = (Atom *) data;
			unsigned long i;
			for (i = 0; i < nitems; i++)
				if (atoms[i] != sticky_atom)
					g_array_append_val (new_state, atoms[i]);
		}
		if (data)
			XFree (data);
		if (sticky)
			g_array_append_val (new_state, sticky_atom);
		XChangeProperty (xdisplay, xid, net_wm_state, XA_ATOM, 32,
		                PropModeReplace, (unsigned char *) new_state->data,
		                new_state->len);
		g_array_free (new_state, TRUE);
	}

	/* data.l[3] = source indication, 1 = normal application. */
	memset (&ev, 0, sizeof (ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.window = xid;
	ev.xclient.message_type = net_wm_state;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = sticky ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE;
	ev.xclient.data.l[1] = sticky_atom;
	ev.xclient.data.l[2] = 0;
	ev.xclient.data.l[3] = 1;
	ev.xclient.data.l[4] = 0;

	XSendEvent (xdisplay, root, False,
	            SubstructureNotifyMask | SubstructureRedirectMask, &ev);
	XFlush (xdisplay);
#else
	(void) win;
	(void) sticky;
#endif
}

gboolean
wm_hints_get_geometry (GtkWindow *win, int *x, int *y, int *w, int *h)
{
#ifdef GDK_WINDOWING_X11
	Display *xdisplay;
	Window xid, root, child;
	int rx = 0, ry = 0;
	unsigned int rw = 0, rh = 0, bw = 0, depth = 0;
	int tx = 0, ty = 0;

	if (!wm_hints_x11_get_handles (win, &xdisplay, &xid))
		return FALSE;

	root = DefaultRootWindow (xdisplay);

	/* XGetGeometry on the window itself returns size and border, but x/y
	 * are relative to its parent (the WM's frame). XTranslateCoordinates
	 * gives us absolute root-relative coords, then we subtract the frame
	 * extents so callers get "outer" geometry suitable for restoration. */
	if (!XGetGeometry (xdisplay, xid, &child, &rx, &ry, &rw, &rh, &bw, &depth))
		return FALSE;

	if (!XTranslateCoordinates (xdisplay, xid, root, 0, 0, &tx, &ty, &child))
		return FALSE;

	{
		Atom frame_extents = XInternAtom (xdisplay, "_NET_FRAME_EXTENTS", False);
		Atom type;
		int format;
		unsigned long nitems, bytes_after;
		unsigned char *data = NULL;

		if (XGetWindowProperty (xdisplay, xid, frame_extents, 0, 4, False,
		                       XA_CARDINAL, &type, &format, &nitems, &bytes_after,
		                       &data) == Success && data && type == XA_CARDINAL
		    && format == 32 && nitems >= 4)
		{
			long *ext = (long *) data;
			/* ext = {left, right, top, bottom} */
			tx -= (int) ext[0];
			ty -= (int) ext[2];
		}
		if (data)
			XFree (data);
	}

	if (x) *x = tx;
	if (y) *y = ty;
	if (w) *w = (int) rw;
	if (h) *h = (int) rh;
	return TRUE;
#elif defined(GDK_WINDOWING_MACOS)
	GdkSurface *surface;
	if (!win)
		return FALSE;
	surface = gtk_native_get_surface (GTK_NATIVE (win));
	return pox_macos_get_window_geometry (surface, x, y, w, h);
#else
	(void) win; (void) x; (void) y; (void) w; (void) h;
	return FALSE;
#endif
}

void
wm_hints_set_position (GtkWindow *win, int x, int y)
{
#ifdef GDK_WINDOWING_X11
	Display *xdisplay;
	Window xid;

	if (!wm_hints_x11_get_handles (win, &xdisplay, &xid))
		return;

	XMoveWindow (xdisplay, xid, x, y);
	XFlush (xdisplay);
#elif defined(GDK_WINDOWING_MACOS)
	GdkSurface *surface;
	if (!win)
		return;
	surface = gtk_native_get_surface (GTK_NATIVE (win));
	pox_macos_set_window_position (surface, x, y);
#else
	(void) win; (void) x; (void) y;
#endif
}

gboolean
wm_hints_position_on_screen (GtkWindow *win, int x, int y)
{
	GdkDisplay *display;
	GListModel *monitors;
	guint i, n;

	if (!win)
		return FALSE;

	display = gtk_widget_get_display (GTK_WIDGET (win));
	if (!display)
		return FALSE;

	monitors = gdk_display_get_monitors (display);
	if (!monitors)
		return FALSE;

	n = g_list_model_get_n_items (monitors);
	for (i = 0; i < n; i++)
	{
		GdkMonitor *mon = g_list_model_get_item (monitors, i);
		GdkRectangle geom;
		gboolean hit;

		if (!mon)
			continue;

		gdk_monitor_get_geometry (mon, &geom);
		hit = (x >= geom.x && x < geom.x + geom.width &&
		       y >= geom.y && y < geom.y + geom.height);
		g_object_unref (mon);
		if (hit)
			return TRUE;
	}
	return FALSE;
}
