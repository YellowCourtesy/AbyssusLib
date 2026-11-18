#pragma once

// The note box and the plugin windows: Present hook and Dear ImGui on Direct3D 11.
// No CommonLibSSE.

struct AbyssusWindow;  // include/Abyssus/API.h

namespace abyssus::overlay
{
	struct Options
	{
		// Gives the game UI render target view (ID3D11RenderTargetView*), or nullptr.
		void* (*gameTarget)(){ nullptr };
		float x{ 0.5f };   // horizontal center of the box, part of the screen width
		float y{ 0.08f };  // top of the box, part of the screen height
	};

	// Hooks Present of the swap chain (IDXGISwapChain*). Call one time.
	bool Install(void* a_swapChain, const Options& a_options);

	// Safe from all threads. Usable before Install(): the text shows when the box is ready.
	void Notify(const char* a_source, const char* a_text, float a_seconds, int a_level);
	void SetStatus(const char* a_source, const char* a_text);
	void SetWindow(const char* a_source, const char* a_id, const AbyssusWindow* a_window);
}
