# AbyssusLib

An SKSE plugin with shared services for other SKSE plugins (Skyrim AE 1.7.x).
License: GPL-3.0. Requires SKSE64 and Address Library for SKSE Plugins.

## The stack

All windows, status lines and notes of all plugins go into one stack at the top
left of the screen, drawn with Dear ImGui on Direct3D 11. From top to bottom:

1. **Diag:** SkyrimDiagnostics (its data window and its warnings)
2. **GPS:** SkyGPS (its status line, only while navigation is active)
3. **Other plugins:** in the order in which they first showed a window or a
   status line
4. **Notif:** the notification queue, oldest note at the top. Each note shows
   until its time ends. No limit. A note from `SetNote` has no time limit: it
   stays until its plugin changes or clears it.

Each plugin: its windows, then its status line. The order is fixed in this
version. Version 2.1 will read the order from a file.

## F11

F11 cycles the configurations. The windows of other plugins show when Notif shows.

| Configuration | Diag | GPS | Other plugins and Notif |
|---|---|---|---|
| Diag / GPS / Notif (at start) | yes | yes | yes |
| GPS / Notif | | yes | yes |
| GPS | | yes | |
| Notif | | | yes |
| Off | | | |

The name of the new configuration shows at the top center for 3 seconds. Notes
that come while Notif is hidden are not shown, but their time runs.

## Windows (API version 2)

A plugin can show its own windows. The plugin sends the content as data:
colored text, line ends, separators and line graphs. AbyssusLib draws it. The
plugin has no Dear ImGui and no Present hook for drawing.

- A window shows until the plugin changes or clears it. A change keeps its place.
- `below`: the id of another window of the same plugin. This window goes after it.

## Use from another plugin

Copy `include/Abyssus/API.h` into your plugin (it needs CommonLibSSE-NG). Nothing
to link: the header finds `AbyssusLib.dll` at runtime.

```cpp
#include "Abyssus/API.h"

if (abyssus::IsAvailable())  // at or after SKSE kPostLoad
    abyssus::Notify("MyMod", "Something happened", 5.0f);
abyssus::SetStatus("MyMod", "Working: step 2 of 5");
abyssus::SetStatus("MyMod", nullptr);  // clear
abyssus::SetNote("MyMod", "limit", "Over the limit", abyssus::Level::kError);  // no time limit
abyssus::ClearNote("MyMod", "limit");

// Windows: AbyssusLib 2 or later.
static abyssus::WindowBuilder b;
b.Clear();
b.Text(abyssus::WindowBuilder::kDefault, "FPS ");
b.Text({ 1.0f, 0.35f, 0.3f, 1.0f }, "%.0f", fps);  // same line, red
b.NewLine();
b.Graph(values, count, 0.0f, 50.0f, 380.0f, 50.0f);
const AbyssusWindow w = b.Make();  // AbyssusLib puts it into the stack
abyssus::SetWindow("MyMod", "main", &w);
abyssus::ClearWindow("MyMod", "main");
```

`IsAvailable()` is true for notes and status lines; `HasWindows()` is true if
the installed AbyssusLib has windows.

Code without CommonLibSSE can use the header too: include `windows.h` before it.

If AbyssusLib is not installed, the calls do nothing; use a fallback.

## Build

GitHub Actions builds every push; the artifact is ready for Mod Organizer 2.
Local build (Visual Studio developer prompt, `VCPKG_ROOT` set):

```
git submodule update --init --recursive
cmake --preset release
cmake --build build --target AbyssusLib
```

The overlay code (Present hook, ImGui setup, game UI render target) comes from
SkyrimDiagnostics.
