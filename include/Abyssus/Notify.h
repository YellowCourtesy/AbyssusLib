#pragma once

// AbyssusLib notifications: a small on-screen note box drawn with Dear ImGui on
// Direct3D 11, in place of the game's notification queue.
//
// Each SKSE plugin links AbyssusLib. In one game process, the first plugin that
// calls Init() becomes the host: it hooks Present and draws the box. Plugins
// that call Init() later find the host and send their notes to it, so all
// plugins share one box. No CommonLibSSE: the plugin gives the swap chain.

namespace abyssus
{
	enum class Level : int
	{
		kInfo = 0,
		kWarning = 1,
		kError = 2,
	};

	struct Options
	{
		// Gives the render target view (ID3D11RenderTargetView*) that the game
		// uses for its menus and HUD now, or nullptr. If set, the box is drawn on
		// it, so upscalers and frame generation handle it like the game UI. If
		// not set or not usable, the box is drawn on the swap chain image.
		void* (*gameTarget)(){ nullptr };
		// Position of the box, as a part of the screen size (0..1).
		float x{ 0.5f };  // horizontal center of the box
		float y{ 0.08f };  // top of the box
		int   maxNotes{ 4 };  // only the newest notes show; older ones are dropped, never queued
	};

	// Call one time per plugin, when the swap chain exists (for example at
	// SKSE kDataLoaded). a_swapChain is an IDXGISwapChain pointer. Options are
	// used only if this plugin becomes the host. Returns true if notes can show.
	bool Init(void* a_swapChain, const Options& a_options = {});

	// Shows a note for a_seconds. Safe from all threads. A note with the same
	// source and text as one that shows replaces it (its timer starts again).
	// Notes sent before Init() are kept (newest 8) and shown after it.
	void Notify(const char* a_source, const char* a_text, float a_seconds = 5.0f, Level a_level = Level::kInfo);

	// True if this plugin draws the box (it is the host).
	bool IsHost();
}
