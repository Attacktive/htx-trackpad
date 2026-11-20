# htx-touchpad

A small macOS background helper that makes the "HTX HID Device" USB touchpad usable on a Mac.

The pad is [this one on Amazon](https://www.amazon.com/dp/B0BDLJXBS9).
It identifies itself as USB `048D:8911` with the serial `MIIX320_HTK_V7T_HTP`; its firmware appears to come from a Lenovo Miix 320 keyboard dock.

## Why this exists

The pad is a Windows Precision Touchpad, and macOS has no driver for that protocol.

- In its power-on mouse mode, the firmware sends nothing at all.
- After the host explicitly sets mouse mode, it sends mouse reports, but their values look garbled and macOS ignores them anyway.
- In Precision Touchpad mode (feature report 3 set to 3), it sends clean multi-finger reports, which macOS cannot interpret.

This helper switches the pad to Precision Touchpad mode, reads the finger reports, and posts cursor, click, and scroll events itself.

## Gestures

| Gesture | Action |
| --- | --- |
| One finger | Move the pointer |
| Tap | Left click, a moment after the lift while tap-drag is on (tap again quickly for a double click) |
| Tap, then touch again and slide | Drag, unless dragging is off in Accessibility settings; after a lift the item stays held for a second, so touch again to keep dragging or tap to drop it |
| Two-finger tap | Right click |
| Three-finger tap | Middle click |
| Two-finger slide | Scroll, following the system's natural-scrolling setting; a flick keeps coasting after the lift until a touch stops it |
| Two-finger pinch or spread | Zoom out or in, unless pinch to zoom is off in Trackpad settings |
| Three-finger swipe up | Mission Control |
| Three-finger swipe down | App Exposé |
| Three-finger swipe left or right | Move to the Space on the right or left |

The pad has no physical button.
Gestures outside this table, such as rotating, smart zoom, and anything with four or more fingers, are not supported.

Three-finger swipes send the same Dock gesture events as Apple's trackpads, so Spaces and Mission Control follow the fingers and hold wherever they stop.
Lifting finishes the swipe, unless the fingers were heading back toward where they started, which snaps it back.
These events come from private macOS functions.
If an update removes them, swipes fall back to the Mission Control keyboard shortcuts (Control with an arrow key), fired once the fingers travel 10 mm, with no animation following the fingers.
That fallback needs the shortcuts enabled in System Settings → Keyboard → Keyboard Shortcuts → Mission Control.

## Requirements

- macOS; developed and tested on macOS 27.2.1 on a 14-inch MacBook Pro with Apple silicon (Mac16,8).
- The Xcode Command Line Tools, for `make` and `clang`: `xcode-select --install`.
- For touches, a built-in trackpad or a Magic Trackpad; see [Touches](#touches).

Three-finger swipes and touches use private macOS functions, so a macOS update could break them; [Troubleshooting](#troubleshooting) shows how to tell.

## Install

```sh
make install            # or: make install SPEED=1.2 VERBOSE=1
```

`VERBOSE=1` logs every touch: taps, drags, scroll and momentum endings, and per-movement pointer statistics.

This installs `~/.local/bin/htx-touchpad` and the login agent `~/Library/LaunchAgents/xyz.attacktive.htx-touchpad.plist`, then starts it.

Then grant two permissions in System Settings → Privacy & Security, adding `~/.local/bin/htx-touchpad` with **+** and **⌘⇧G** (not the copy in `build/`):

- **Input Monitoring**, to read the pad.
	The helper does not notice this grant while running, so run `make restart` afterwards.
- **Accessibility**, to move the pointer.
	The helper restarts itself within about 10 seconds of this grant.

## Rebuilding

The build signs the binary with the first valid code-signing identity in the keychain (override with `SIGN_IDENTITY=<hash>`), so permissions survive rebuilds.

Without an identity it falls back to ad-hoc signing, which pins both permissions to one exact build.
In that case, after installing changed code, remove both entries in Privacy & Security and add them again.

Changing only `SPEED` reinstalls the same binary and keeps the permissions either way.

## Pointer acceleration

The acceleration curve is copied from Apple's: cursor points per mm of finger travel, by finger speed, measured on the built-in trackpad of a 14-inch MacBook Pro (Mac16,8) at Tracking speed 1.0.
It runs from 3 pt/mm when nearly still to about 61 pt/mm for a flick at 850 mm/s and beyond.

The pointer also follows the Trackpad "Tracking speed" slider in System Settings, read at the start of every touch.
The slider's 0–3 range maps linearly onto a multiplier from 0.25 to 2.5, which is 1 at the 1.0 notch, applied to the whole curve along with `SPEED`.
At 1.0 the curve matches Apple's; at other notches it is an approximation, since Apple's curve also changes shape.
The add-on pad and the built-in trackpad share that one slider.

## Tapping and dragging

Taps copy the timing of Apple's trackpad with its dragging option on, measured on the same MacBook.
A one-finger tap clicks 0.23 s after the lift, unless the finger lands again first.
Landing again presses the button at once and holds it, so the drag that follows is a single press, which lets Mission Control drag a desktop instead of switching to it.
Lifting quickly from that second touch completes a double click instead.

This follows the trackpad's dragging option in Accessibility settings, read at the start of every touch.
With it off, taps click right at the lift, and touching again only moves the pointer.

## Scrolling

Scrolling copies Apple's trackpad too, measured on the same MacBook at the default scrolling speed.
The curve gives about 6 points per mm of finger travel up to 65 mm/s, rising to about 11 at 300 mm/s and beyond.
As on Apple's trackpad, every report in which the fingers move along the scroll sends at least one point, so a slow crawl scrolls faster than the curve alone would.
Neither `SPEED` nor the Tracking speed slider affects scrolling.

Scroll events carry the same gesture phases as Apple's, and after a lift the helper sends the momentum events itself, since apps such as Kitty and JetBrains IDEs coast only on those.
Momentum starts at 2.4 times the scroll speed over the last 50 ms and decays exponentially with a time constant of 0.224 s, both measured on the built-in trackpad.
It follows the trackpad's scrolling-inertia option in Accessibility settings, read at the start of every touch, and any touch stops it.

## Pinch to zoom

A two-finger touch waits for a little movement before it commits, as on Apple's trackpad.
It scrolls once the fingers' midpoint has traveled 1 mm, and farther than the distance between the fingers has changed.
It pinches once that distance has changed by 2 mm, and at least as much as the midpoint has traveled, even with one finger held still.

A pinch sends the same magnify events as Apple's trackpad, at 0.0315 per mm of change in the distance between the fingers, the steady rate measured on the built-in trackpad at every speed.
They are built with public CoreGraphics calls and undocumented field numbers, so they need no private functions.

## Touches

Apple's trackpads also send every finger as a touch, and some apps act on those rather than on gestures alone.
Chrome, for one, swipes between pages after 8% of the trackpad's width when it has touches, but without them it counted so little of a two-finger swipe that going back took a long, fast swipe.
The helper sends the pad's fingers as touches too, built with private IOKit and SkyLight functions.
AppKit accepts trackpad touches only from a multitouch device it knows, so they are filed under the Mac's built-in trackpad, or a Magic Trackpad when there is no built-in one; with neither, the helper sends no touches.

## Other targets

- `make restart` restarts the helper.
- `make log` shows the end of `~/Library/Logs/htx-touchpad.log`.
- `make uninstall` stops the helper and removes the installed files; remove the two Privacy & Security entries by hand.

## Troubleshooting

Every start of the helper logs a line like this one, which `make log` shows:

```text
started: speed 1.00, verbose: no, accessibility: yes, input monitoring: yes, dock swipes: yes, touches: yes, HID manager open: 0x00000000
```

- `accessibility: no` or `input monitoring: no` means that permission is missing; see [Install](#install).
- `dock swipes: no` means the private swipe functions are gone, so three-finger swipes use the keyboard-shortcut fallback.
- `touches: no` means the private touch functions are gone or there is no built-in trackpad or Magic Trackpad, so the helper sends no touches.

Connecting the pad logs `touchpad connected; switching to touchpad mode: 0x00000000`, where any code other than zero means the switch failed.

## Keyboard Setup Assistant

The pad also exposes a keyboard interface, which makes macOS open Keyboard Setup Assistant on first connection.
To keep it away, register that interface as an ANSI keyboard (type 40) once:

```sh
sudo defaults write /Library/Preferences/com.apple.keyboardtype.plist keyboardtype -dict-add 35089-1165-0 -int 40
```

The key is the product ID and the vendor ID in decimal (`0x8911` is 35089 and `0x048D` is 1165), followed by 0.

## Touch report layout

Input report 4 is 30 bytes long:

- Byte 0: report ID.
- Bytes 1–25: five 5-byte contact slots.
	- Byte 0 of each slot is flags: bit 0 is confidence, bit 1 is tip switch, and bits 2–4 are the contact ID.
	- Bytes 1–2 are X (little-endian, 0–1599), and bytes 3–4 are Y (0–1199), over a 117 mm × 88 mm surface.
- Bytes 26–27: scan time.
- Byte 28: contact count.
- Byte 29: buttons.

## License

MIT; see [LICENSE](LICENSE).
