/* PoxChat — macOS NSWindow bridge for wm-hints.
 *
 * GTK4 deliberately drops public window-positioning APIs because Wayland
 * forbids them, but on macOS the platform does support placement and
 * users expect saved geometry to be honoured. We reach the underlying
 * NSWindow via gdk_macos_surface_get_native_window() and read/write its
 * frame directly. Coordinates here are converted to GTK's top-left-origin
 * convention (same as the X11 path in wm-hints.c) so the values that land
 * in poxchat.conf are cross-platform comparable.
 */

#import <AppKit/AppKit.h>
#import <math.h>

#include <gdk/gdk.h>
#include <gdk/macos/gdkmacos.h>

#include "wm-hints-macos.h"

/* Height of the screen whose bottom-left is NS coordinate (0,0). NSScreen
 * coordinates have origin at the bottom-left of the primary screen; GTK
 * uses top-left of the primary screen. We need the primary screen's height
 * for the Y flip. */
static CGFloat
pox_macos_primary_screen_height (void)
{
	NSArray<NSScreen *> *screens = [NSScreen screens];
	if ([screens count] == 0)
		return 0.0;

	/* The first entry in +[NSScreen screens] is the screen with origin
	 * (0,0) in NS coordinate space — i.e. the primary screen. */
	return [[screens objectAtIndex:0] frame].size.height;
}

static NSWindow *
pox_macos_nswindow_from_surface (GdkSurface *surface)
{
	if (!surface || !GDK_IS_MACOS_SURFACE (surface))
		return nil;
	return (NSWindow *) gdk_macos_surface_get_native_window (GDK_MACOS_SURFACE (surface));
}

gboolean
pox_macos_get_window_geometry (GdkSurface *surface, int *x, int *y, int *w, int *h)
{
	NSWindow *nswin = pox_macos_nswindow_from_surface (surface);
	if (!nswin)
		return FALSE;

	NSRect frame = [nswin frame];
	CGFloat primary_h = pox_macos_primary_screen_height ();

	if (x) *x = (int) lround (frame.origin.x);
	if (y) *y = (int) lround (primary_h - (frame.origin.y + frame.size.height));
	if (w) *w = (int) lround (frame.size.width);
	if (h) *h = (int) lround (frame.size.height);
	return TRUE;
}

void
pox_macos_set_window_position (GdkSurface *surface, int x, int y)
{
	NSWindow *nswin = pox_macos_nswindow_from_surface (surface);
	if (!nswin)
		return;

	CGFloat primary_h = pox_macos_primary_screen_height ();

	/* setFrameTopLeftPoint takes the *top-left* of the frame in NS screen
	 * coordinates (Y up from primary-screen bottom). Converting from GTK's
	 * top-down Y is just (primary_h - gtk_y); the NSWindow handles its
	 * own height internally so we don't need to look up the current frame. */
	[nswin setFrameTopLeftPoint:NSMakePoint ((CGFloat) x, primary_h - (CGFloat) y)];
}
