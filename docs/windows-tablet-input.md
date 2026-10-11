# Windows Wacom tablet input

The fork can capture an installed Windows WinTab tablet driver's pen data and
send it through Moonlight's existing native pen protocol. It sends absolute
position, tip contact, normalized pressure, pen inclination and direction,
eraser identity, and up to three barrel buttons. It does not redirect the USB
device or install a tablet driver on either computer.

## Enable it

1. Verify the tablet works locally using its existing Wacom driver.
2. In Moonlight **Settings → Input Settings**, enable **Stream Wacom tablet
   pressure and tilt (WinTab)**.
3. Start a new stream to a Sunshine host with native pen/touch input enabled.
4. In the host's drawing application, select its **Windows Ink** or native
   Windows pen input API. For Blender, this is **Preferences → Input → Tablet
   API → Windows Ink**; restart Blender if the setting requests it.

The option is off by default. A usable tablet context enables absolute mouse
control for that stream. The keyboard shortcut to switch to relative mouse
mode also suspends WinTab capture until absolute mode is restored. Missing
drivers or hosts without native pen support retain the ordinary input path
and log why tablet capture could not start.

Pen coordinates follow the tablet driver's system display mapping, converted
to the Moonlight window's video rectangle. Strokes that start in the video can
continue to its boundary. Hover or contact starting in the letterbox is not
forwarded. Choose the display containing Moonlight in the Wacom mapping if
using multiple displays.

The private context is active only while the streaming window has focus and
input capture is enabled. Losing focus, leaving proximity, disconnecting a
device or ending the stream clears the remote pen state. Tablet-generated
mouse/pointer compatibility events are filtered without suppressing input
from a separate physical mouse.

## Compatibility

The Windows Sunshine backend injects a Windows pen. Applications that only
read WinTab on the **host** are not supported by this path; select Windows Ink
in that application or add a separate compatibility solution. Capturing
WinTab on the **client** does not make a remote Wacom USB device appear.
ExpressKeys, touch strips, tablet identity and vendor configuration tools are
outside the pen protocol. Existing driver assignments that emit ordinary
keyboard shortcuts may still work through Moonlight's keyboard input.

The protocol carries pen inclination and its direction. It does not carry
independent twist around a pen's barrel. Hover distance is not available from
the selected WinTab packet fields.

Windows Ink exposes one barrel-button flag. Sunshine combines the streamed
barrel buttons into that flag, keeping it pressed while any button remains
held. Independent assignments for both physical barrel buttons need a future
button-mapping feature; this path does not invent mouse-button assignments.

## Verification

The connected test tablet is USB `056A:00B1`, an Intuos3 6×8. Its installed
driver reports 200 Hz packets, pressure `0..1023`, and orientation axes. A
local WinTab capture test received 5,551 packets, changing pressure across
the full `0..1023` range, azimuth `0..3580`, altitude `250..830`, and button
masks `0,1,2,3`. That establishes driver-level pressure and tilt acquisition.
The actual Moonlight capture backend was also exercised locally for 30
seconds. It produced 3,888 pen events, including 3,047 contact events, 23
releases, two cancellations and 736 eraser events. Normalized pressure reached
`0..1`, inclination reached `0..61` degrees and both barrel-button bits were
observed. SDL reported zero mouse-button-down events during that capture.
Remote drawing-app compatibility still requires a real stream test.

Pure conversion tests live in `tests/input/wintabmapping.pro`. They cover
pressure range offsets and clamping, tilt/azimuth conversion, inverted pen
inclination, unknown axes, letterbox boundaries and invalid viewport sizes.
Build the qmake project in an isolated test build directory, then run
`test_wintabmapping`.

For acceptance, draw light and hard strokes in Blender or the target painting
app; verify varying size/strength, tilt directions, barrel buttons and eraser.
Also test lifting the pen, leaving the window, Alt+Tab during a stroke,
stopping/restarting a stream, and using a physical mouse while the pen hovers.
Repeat with the option off to verify ordinary mouse behavior.

## Sources and reuse

- [Wacom WinTab overview](https://developer-docs.wacom.com/docs/icbt/windows/wintab/wintab-overview/)
  and [API reference](https://developer-docs.wacom.com/docs/icbt/windows/wintab/wintab-reference/).
- [Wacom SDK ABI header](https://github.com/Wacom-Developer/wacom-device-kit-windows/blob/master/WintabSDK/WINTAB.H)
  permits unrestricted use/copy/distribution of its text and information. The
  local ABI subset retains its attribution. The installed driver DLL is loaded
  from the Windows system directory and is not redistributed by Moonlight.
- [Blender input preferences](https://docs.blender.org/api/current/bpy.types.PreferencesInput.html)
  expose Automatic, WinTab and Windows Ink tablet APIs.
- [Linux Wacom device definitions](https://github.com/torvalds/linux/blob/master/drivers/hid/wacom_wac.c)
  identify product `0xB1` as Intuos3 6×8.
