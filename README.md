# AbyssusLib

An SKSE plugin with shared services for other SKSE plugins (Skyrim AE 1.7.x).
License: GPL-3.0. Requires SKSE64 and Address Library for SKSE Plugins.

## Notes and status lines

A small box at the top of the screen, drawn with Dear ImGui on Direct3D 11, in
place of the game's notification queue.

- **Notes:** all plugins share one note. A new note replaces the note that shows
  (the same rule as SkyrimDiagnostics), so a note is never minutes old.
- **Status lines:** each plugin can show one permanent line, until it changes or
  clears it.

## Windows (API version 2)

A plugin can show its own windows. The plugin sends the content as data:
colored text, line ends, separators and line graphs. AbyssusLib draws it. The
plugin has no Dear ImGui and no Present hook for drawing.

- A window shows until the plugin changes or clears it.
- `below`: the id of another window of the same plugin. If that window shows,
  this window goes below it. If not, at its own position.

## Use from another plugin

Copy `include/Abyssus/API.h` into your plugin (it needs CommonLibSSE-NG). Nothing
to link: the header finds `AbyssusLib.dll` at runtime.

```cpp
#include "Abyssus/API.h"

if (abyssus::IsAvailable())  // at or after SKSE kPostLoad
    abyssus::Notify("MyMod", "Something happened", 5.0f);
abyssus::SetStatus("MyMod", "Working: step 2 of 5");
abyssus::SetStatus("MyMod", nullptr);  // clear

// Windows: AbyssusLib 2 or later.
static abyssus::WindowBuilder b;
b.Clear();
b.Text(abyssus::WindowBuilder::kDefault, "FPS ");
b.Text({ 1.0f, 0.35f, 0.3f, 1.0f }, "%.0f", fps);  // same line, red
b.NewLine();
b.Graph(values, count, 0.0f, 50.0f, 380.0f, 50.0f);
const AbyssusWindow w = b.Make(10.0f, 10.0f);  // top left corner, pixels
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
