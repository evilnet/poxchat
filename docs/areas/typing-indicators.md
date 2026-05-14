# Typing indicators (IRCv3 +typing)

End-to-end map of how `+typing` TAGMSGs are sent, received, displayed, and
filtered. Useful when touching anything in the strip's lifecycle, the
outbound state machine, the self-echo filter, or the `fe_typing_update`
callsites.

## Wire protocol

IRCv3 client tag `+typing` carried on `TAGMSG`. Values:

- `active`  — keystroke happening / continuing
- `paused`  — input box still non-empty but no keystrokes for ~3 s
- `done`    — message sent or composition abandoned

We re-send `active` every 3 s while the user is typing; the receiver
expires entries 6 s after the last seen `active`/`paused`.

## Outbound (`src/common/outbound.c`)

State machine on the session, driven by keystrokes from the GTK input
box (`typing_indicator_keystroke`, called from fkeys.c):

| Send site                            | When                                   | Marks echo window |
|--------------------------------------|----------------------------------------|-------------------|
| `typing_send_active`                 | first keystroke + every 3 s thereafter | yes               |
| `typing_send_timer_cb` → paused      | timer fires with no recent keystrokes  | yes               |
| `typing_indicator_cancel` → done     | message sent or input cleared          | yes (only if we'd previously sent) |

Every send goes through `typing_mark_self_echo_window` which sets
`sess->typing_self_echo_until = now + 5 s`. That window is the only way
the inbound side knows "this TAGMSG is just our own round-trip" — see
below.

Outbound gating prefs:

- `hex_irc_typing_send` (default 1) — master switch on
  `typing_indicator_keystroke`. If off, we never send anything.

## Inbound (`src/common/inbound.c`)

`inbound_tagmsg` looks up the `+typing` tag, then:

1. Compute `is_self = !serv->p_cmp (nick, serv->nick)`.
2. If `is_self`:
   - Inside `typing_self_echo_until` → **always drop** (our own echo via
     echo-message / bouncer).
   - Outside the window → controlled by `hex_irc_typing_self` (default 1).
     Outside-window self-typing means *another client of ours* (same
     nick) is composing, which is genuinely useful info.
3. Dispatch:
   - `active` / `paused` → `typing_indicator_update (sess, nick)`
   - `done`              → `typing_indicator_remove (sess, nick)`

`typing_indicator_update` keeps a per-session `GSList<typing_entry*>`
(`sess->typing_nicks`) with a 2 s sweep timer that drops anyone whose
`last_seen` is older than 6 s.

Inbound display gating:

- `hex_irc_typing_show` (default 1) — master switch on
  `typing_indicator_update`. If off, the inbound side parses and tracks
  nothing.

## Display: the bottom status strip

The typing indicator renders into a generalized "bottom status strip"
owned by the xtext widget. It is not typing-specific; reply-state and
future features share it. Items are keyed by string id (`"typing"`,
`"reply"`, …) with a priority that partitions them into a left zone
(`< 100`) and a right zone (`>= 100`); typing uses priority 100.

API (`src/fe-gtk/xtext.h`):

| Function                                | Purpose                                            |
|-----------------------------------------|----------------------------------------------------|
| `gtk_xtext_status_set`                  | Insert or update an item by key.                   |
| `gtk_xtext_status_remove`               | Remove immediately (force-collapse the strip).     |
| `gtk_xtext_status_set_placeholder`      | Convert existing item to a placeholder (see below).|
| `gtk_xtext_status_set_dismiss`          | Attach an `x` button + callback.                   |
| `gtk_xtext_status_clear`                | Wipe everything.                                   |

### Placeholder mechanic

A naïve "remove on last typer stops" causes a visible scroll jump — the
strip vanishes and the scrollback shifts down by one line in one
animation frame. Fix: when typing stops, `fe_typing_update` calls
`gtk_xtext_status_set_placeholder(xtext, "typing")` instead of
`status_remove`. The slot stays, the text is blanked, the strip's
reserved height stays put.

Sweep happens in `gtk_xtext_append_entry` (xtext.c) *only when the
appended-to buffer is the visible one* — `buf->xtext->buffer == buf`.
This means background-tab activity does **not** collapse a placeholder
on the front tab. `xtext_status_sweep_placeholders` runs there, frees
any item with `placeholder == 1`, and toggles `status_strip_visible`
to false if the count drops to zero.

A subsequent `gtk_xtext_status_set` for the same key resets
`item->placeholder = 0`, so a fresh typing event un-placeholders cleanly
without going through remove.

### Strip height

Exactly one chat-line stride (`xtext->fontsize`, which already includes
the inter-line gap via `pango_font_metrics_get_height + 1`). Computed in
**one** place — the static inline `gtk_xtext_status_strip_height` near
the top of `xtext.c`. Five sites use it:

- `gtk_xtext_adjustment_set` — page-size math.
- `gtk_xtext_button_press` — click hit-test for the strip (used by
  dismiss-button handling).
- `gtk_xtext_render_page` x2 — reserved area in the two render paths.
- `gtk_xtext_draw_status_strip` — the draw itself.

If you add a sixth site, route it through the helper. They have to
agree exactly or click coordinates drift off the visible strip.

### Empty strip behavior

`gtk_xtext_draw_status_strip` skips placeholder items when building the
displayed text, so a strip containing only placeholders renders as a
blank bar of reserved height (background-fill only). That's the
"reserved space" state.

## fe_typing_update callsites

`fe_typing_update(sess)` is the single entry point that pushes the
current `typing_nicks` set into the status strip. Called from:

- `inbound.c` — on every state change (add, remove, sweep, server quit).
- `maingui.c:1086` — on tab switch (so the strip reflects the newly
  visible session).
- The session-clear path in `inbound.c:73` after a part/disconnect.

When called with an empty `typing_nicks`, it placeholders the slot
rather than removing it. This is correct for tab-switch too: in tabbed
mode the xtext is shared, so a placeholder left over from the previous
tab is harmless — the next appended line on the now-visible buffer
sweeps it.

## Session state (`src/common/poxchat.h`)

```c
GSList *typing_nicks;            /* who's typing here (typing_entry*) */
int     typing_sweep_timer;      /* 2s tick, expires stale typers      */
gint64  typing_last_sent;        /* last outbound active timestamp     */
int     typing_send_timer;       /* 3s re-send tick while typing       */
gint64  typing_last_keystroke;   /* last keystroke in input box        */
gint64  typing_self_echo_until;  /* echo-window deadline (see above)   */
```

## Gotchas

- Don't replace `gtk_xtext_status_set_placeholder` with `…_remove` in
  `fe_typing_update`. That re-introduces the strip-vanish jump and is
  the whole reason placeholders exist.
- Don't sweep placeholders from a non-visible buffer's append path.
  The check is `buf->xtext->buffer == buf` in
  `gtk_xtext_append_entry`.
- Self-echo detection is time-based, not label-based. It assumes
  echoes arrive within 5 s of our send. If we ever wire labeled-response
  for typing TAGMSGs, prefer label correlation and shrink/remove the
  window.
- `hex_irc_typing_self` semantics changed: it no longer toggles "show
  *any* self-attributed typing". It now toggles only the *other-client*
  signal — this-client echoes are always suppressed.
