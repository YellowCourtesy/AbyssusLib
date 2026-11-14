# AbyssusLib

Shared code for SKSE plugins (Skyrim SE/AE). License: GPL-3.0.

## Notifications (`include/Abyssus/Notify.h`)

An on-screen note box, drawn with Dear ImGui on Direct3D 11, in place of the
game's notification queue. Only the newest notes show (4 by default); older
notes are dropped, never queued, so a note is never minutes old.

All plugins in one game process share one box: the first plugin that calls
`abyssus::Init()` hooks Present and draws; the others send their notes to it.
Plugins can link different copies of AbyssusLib, if the API version is the same.

```cpp
#include "Abyssus/Notify.h"

// At SKSE kDataLoaded:
auto* window = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
abyssus::Init(window->swapChain);

// From any thread:
abyssus::Notify("MyMod", "Something happened", 5.0f);
```

The overlay code (Present hook, ImGui setup, game UI render target) comes from
SkyrimDiagnostics.

## Use in a CMake project

1. Add this repo as a git submodule, for example in `lib/abyssuslib`.
2. In the vcpkg manifest, add `{ "name": "imgui", "features": ["dx11-binding"] }`.
3. In `CMakeLists.txt`:
   ```cmake
   add_subdirectory(lib/abyssuslib)
   target_link_libraries(MyPlugin PRIVATE AbyssusLib::AbyssusLib)
   ```
