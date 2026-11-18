#include "Overlay.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include "Abyssus/API.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Direct3D 11 and Dear ImGui only (no CommonLibSSE in this file). The overlay code
// (Present hook, ImGui setup, game UI render target) comes from SkyrimDiagnostics,
// and the note rule too: one note at a time, a new note replaces the one that shows.
// Plugin windows: the plugins send the content as data (AbyssusWindow), this file
// draws it. The plugins have no ImGui. All windows and notes go into one stack at
// the top left; F11 cycles which parts show.

namespace abyssus::overlay
{
	namespace
	{
		struct Note
		{
			std::string   source;
			std::string   text;
			int           level{ 0 };
			float         seconds{ 0.0f };
			LARGE_INTEGER start{};
		};

		// A copy of an AbyssusItem, with its own text and values.
		struct Item
		{
			int                type{ kAbyssusText };
			AbyssusColor       color{};
			std::string        text;
			std::vector<float> values;
			float              graphMin{ 0.0f };
			float              graphMax{ 0.0f };
			float              width{ 0.0f };
			float              height{ 0.0f };
		};

		struct Window
		{
			std::string       source;
			std::string       id;
			std::string       below;  // id of the window above this one (same source), or empty
			std::uint64_t     order{ 0 };  // when the window was first set: the order in its source
			float             backgroundAlpha{ 0.6f };
			float             wrapWidth{ 0.0f };
			std::vector<Item> items;
		};

		// The stack at the top left, from top to bottom:
		//   1. SkyrimDiagnostics (Diag)
		//   2. SkyGPS (GPS)
		//   3. all other plugins, in the order in which they first showed a window or status
		//   4. the notification queue (Notif), oldest note at the top
		// Each plugin: its windows, then its status line. Version 2.1 will read
		// this order from a file; now it is fixed here.
		constexpr const char* kDiagSource = "SkyrimDiagnostics";
		constexpr const char* kGpsSource = "SkyGPS";
		constexpr float       kStackX = 10.0f;
		constexpr float       kStackY = 10.0f;
		constexpr float       kGap = 6.0f;

		// F11 cycles these configurations. The windows of other plugins show
		// when Notif shows.
		struct Config
		{
			const char* name;
			bool        diag;
			bool        gps;
			bool        notes;
		};
		constexpr Config kConfigs[] = {
			{ "Diag / GPS / Notif", true, true, true },
			{ "GPS / Notif", false, true, true },
			{ "GPS", false, true, false },
			{ "Notif", false, false, true },
			{ "Off", false, false, false },
		};
		constexpr int   kConfigCount = static_cast<int>(sizeof(kConfigs) / sizeof(kConfigs[0]));
		constexpr int   kCycleKey = VK_F11;
		constexpr float kConfigLabelSeconds = 3.0f;

		SRWLOCK                                     g_lock = SRWLOCK_INIT;
		std::vector<Note>                           g_notes;       // the queue, oldest first; no limit
		std::uint64_t                               g_nextId = 1;  // window order
		std::map<std::string, std::string>          g_statuses;    // source -> permanent status line
		std::map<std::string, Window>               g_windows;     // "source/id" -> window
		std::vector<std::string>                    g_otherSources;  // other plugins, in the order of their first window or status
		Options                                     g_options;
		LARGE_INTEGER                               g_frequency{};

		// Render thread only.
		int           g_config = 0;
		bool          g_keyWasDown = false;
		HWND          g_gameWindow = nullptr;
		bool          g_configLabel = false;  // the name of the new configuration shows
		LARGE_INTEGER g_configLabelStart{};

		float Seconds(const LARGE_INTEGER& a_from, const LARGE_INTEGER& a_to)
		{
			return static_cast<float>(static_cast<double>(a_to.QuadPart - a_from.QuadPart) / static_cast<double>(g_frequency.QuadPart));
		}

		// Call with g_lock held for writing.
		void NoteSource(const std::string& a_source)
		{
			if (a_source == kDiagSource || a_source == kGpsSource)
				return;
			if (std::find(g_otherSources.begin(), g_otherSources.end(), a_source) == g_otherSources.end())
				g_otherSources.push_back(a_source);
		}

		bool SourceVisible(const std::string& a_source, const Config& a_config)
		{
			if (a_source == kDiagSource)
				return a_config.diag;
			if (a_source == kGpsSource)
				return a_config.gps;
			return a_config.notes;
		}

		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
		constexpr int kPresentSlot = 8;  // position of Present in the IDXGISwapChain function table

		std::atomic<PresentFn> g_originalPresent{ nullptr };
		ID3D11Device*          g_device = nullptr;
		ID3D11DeviceContext*   g_context = nullptr;
		bool                   g_ready = false;
		bool                   g_failed = false;
		LARGE_INTEGER          g_lastFrame{};

		bool Setup(IDXGISwapChain* a_swapChain)
		{
			if (FAILED(a_swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device) {
				return false;
			}
			g_device->GetImmediateContext(&g_context);
			DXGI_SWAP_CHAIN_DESC desc{};
			if (SUCCEEDED(a_swapChain->GetDesc(&desc)))
				g_gameWindow = desc.OutputWindow;
			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
			ImGui::StyleColorsDark();
			ImGui::GetStyle().WindowRounding = 4.0f;
			return ImGui_ImplDX11_Init(g_device, g_context);
		}

		// F11: the next configuration. Only when the game window is in front.
		void UpdateCycleKey(const LARGE_INTEGER& a_now)
		{
			const bool down = (GetAsyncKeyState(kCycleKey) & 0x8000) != 0;
			if (down && !g_keyWasDown && (!g_gameWindow || GetForegroundWindow() == g_gameWindow)) {
				g_config = (g_config + 1) % kConfigCount;
				g_configLabel = true;
				g_configLabelStart = a_now;
			}
			g_keyWasDown = down;
			if (g_configLabel && Seconds(g_configLabelStart, a_now) > kConfigLabelSeconds)
				g_configLabel = false;
		}

		// The render target of the game UI and its size. If the pointer from the
		// game is not a usable view, the swap chain image is used from now on.
		bool GameTarget(ID3D11RenderTargetView*& a_target, D3D11_TEXTURE2D_DESC& a_desc)
		{
			bool ok = false;
			__try {
				if (auto view = static_cast<ID3D11RenderTargetView*>(g_options.gameTarget())) {
					ID3D11Resource* resource = nullptr;
					view->GetResource(&resource);
					ID3D11Texture2D* texture = nullptr;
					if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture) {
						texture->GetDesc(&a_desc);
						texture->Release();
						if (a_desc.Width >= 64 && a_desc.Width <= 16384 && a_desc.Height >= 64 && a_desc.Height <= 16384) {
							view->AddRef();
							a_target = view;
							ok = true;
						}
					}
					if (resource) {
						resource->Release();
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				g_options.gameTarget = nullptr;
				ok = false;
			}
			return ok;
		}

		ImVec4 LevelColor(int a_level, float a_alpha)
		{
			switch (a_level) {
			case 2:
				return ImVec4(1.0f, 0.35f, 0.3f, a_alpha);
			case 1:
				return ImVec4(1.0f, 0.8f, 0.25f, a_alpha);
			default:
				return ImVec4(0.92f, 0.94f, 0.96f, a_alpha);
			}
		}

		constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
		                                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
		constexpr ImVec4 kSourceColor{ 0.55f, 0.85f, 1.0f, 0.9f };

		// Draws a plugin window at a_y. Gives the y of the next window in the stack.
		float BuildWindow(const std::string& a_name, const Window& a_window, float a_y)
		{
			float next = a_y;
			ImGui::SetNextWindowPos(ImVec2(kStackX, a_y), ImGuiCond_Always);
			ImGui::SetNextWindowBgAlpha(std::clamp(a_window.backgroundAlpha, 0.0f, 1.0f));
			if (ImGui::Begin(("##" + a_name).c_str(), nullptr, kWindowFlags)) {
				if (a_window.wrapWidth > 0.0f)
					ImGui::PushTextWrapPos(a_window.wrapWidth);
				bool lineOpen = false;  // a text is on the current line
				for (std::size_t i = 0; i < a_window.items.size(); ++i) {
					const Item& item = a_window.items[i];
					switch (item.type) {
					case kAbyssusText:
						{
							if (lineOpen)
								ImGui::SameLine(0.0f, 0.0f);
							const ImVec4 color = item.color.a > 0.0f ? ImVec4(item.color.r, item.color.g, item.color.b, item.color.a) :
							                                           ImGui::GetStyleColorVec4(ImGuiCol_Text);
							ImGui::TextColored(color, "%s", item.text.c_str());
							lineOpen = true;
						}
						break;
					case kAbyssusNewLine:
						if (!lineOpen)
							ImGui::NewLine();  // an empty line
						lineOpen = false;
						break;
					case kAbyssusSeparator:
						ImGui::Separator();
						lineOpen = false;
						break;
					case kAbyssusGraph:
						{
							const bool colored = item.color.a > 0.0f;
							if (colored)
								ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(item.color.r, item.color.g, item.color.b, item.color.a));
							ImGui::PlotLines(("##graph" + std::to_string(i)).c_str(), item.values.data(), static_cast<int>(item.values.size()), 0, nullptr,
								item.graphMin, item.graphMax, ImVec2(item.width, item.height));
							if (colored)
								ImGui::PopStyleColor();
							lineOpen = false;
						}
						break;
					default:
						break;
					}
				}
				if (a_window.wrapWidth > 0.0f)
					ImGui::PopTextWrapPos();
				next = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y + kGap;
			}
			ImGui::End();
			return next;
		}

		// A small window with "source: text".
		float BuildLine(const std::string& a_name, const std::string& a_source, const std::string& a_text, const ImVec4& a_color, float a_y)
		{
			float next = a_y;
			ImGui::SetNextWindowPos(ImVec2(kStackX, a_y), ImGuiCond_Always);
			ImGui::SetNextWindowBgAlpha(0.65f);
			if (ImGui::Begin(("##" + a_name).c_str(), nullptr, kWindowFlags)) {
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
				if (!a_source.empty()) {
					ImGui::TextColored(ImVec4(kSourceColor.x, kSourceColor.y, kSourceColor.z, a_color.w), "%s:", a_source.c_str());
					ImGui::SameLine();
				}
				ImGui::TextColored(a_color, "%s", a_text.c_str());
				ImGui::PopTextWrapPos();
				next = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y + kGap;
			}
			ImGui::End();
			return next;
		}

		// The windows and the status line of one plugin. Call with g_lock held.
		float BuildSource(const std::string& a_source, float a_y)
		{
			// Its windows: in the order of their first SetWindow, and a window
			// with "below" after that window.
			std::vector<const Window*> windows;
			for (const auto& [key, window] : g_windows) {
				if (window.source == a_source)
					windows.push_back(&window);
			}
			std::sort(windows.begin(), windows.end(), [](const Window* a, const Window* b) { return a->order < b->order; });
			std::vector<const Window*> placed;
			while (!windows.empty()) {
				auto next = windows.begin();
				for (auto it = windows.begin(); it != windows.end(); ++it) {
					const auto& below = (*it)->below;
					const bool  ready = below.empty() ||
					                   std::none_of(windows.begin(), windows.end(), [&](const Window* w) { return w->id == below && w != *it; });
					if (ready) {
						next = it;
						break;
					}
				}
				placed.push_back(*next);  // a loop of "below" values: the first one goes on
				windows.erase(next);
			}
			float y = a_y;
			for (const Window* window : placed)
				y = BuildWindow(window->source + "/" + window->id, *window, y);
			if (const auto status = g_statuses.find(a_source); status != g_statuses.end())
				y = BuildLine("status/" + a_source, a_source, status->second, ImVec4(0.8f, 0.85f, 0.9f, 0.9f), y);
			return y;
		}

		void BuildStack(const ImVec2& a_screen)
		{
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			const Config& config = kConfigs[g_config];

			AcquireSRWLockExclusive(&g_lock);
			std::erase_if(g_notes, [&](const Note& a_note) { return Seconds(a_note.start, now) > a_note.seconds; });
			ReleaseSRWLockExclusive(&g_lock);

			AcquireSRWLockShared(&g_lock);
			float y = kStackY;
			if (config.diag)
				y = BuildSource(kDiagSource, y);
			if (config.gps)
				y = BuildSource(kGpsSource, y);
			if (config.notes) {
				for (const auto& source : g_otherSources)
					y = BuildSource(source, y);
				// The window name is the place in the queue: ImGui keeps each window
				// name for all of the session, so a name for each note would add up.
				for (std::size_t i = 0; i < g_notes.size(); ++i) {
					const Note& note = g_notes[i];
					const float alpha = std::clamp(note.seconds - Seconds(note.start, now), 0.0f, 1.0f);  // fade out in the last second
					y = BuildLine("note" + std::to_string(i), note.source, note.text, LevelColor(note.level, alpha), y);
				}
			}
			ReleaseSRWLockShared(&g_lock);

			// The name of the new configuration, at the top center for a short time.
			if (g_configLabel) {
				ImGui::SetNextWindowPos(ImVec2(a_screen.x * 0.5f, kStackY), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
				ImGui::SetNextWindowBgAlpha(0.65f);
				if (ImGui::Begin("##AbyssusLibConfig", nullptr, kWindowFlags))
					ImGui::TextColored(kSourceColor, "AbyssusLib (F11): %s", config.name);
				ImGui::End();
			}
		}

		// True if a frame has something to draw.
		bool AnythingToDraw()
		{
			if (g_configLabel)
				return true;
			const Config& config = kConfigs[g_config];
			bool          any = false;
			AcquireSRWLockShared(&g_lock);
			if (config.notes && !g_notes.empty())
				any = true;
			for (auto it = g_windows.begin(); !any && it != g_windows.end(); ++it)
				any = SourceVisible(it->second.source, config);
			for (auto it = g_statuses.begin(); !any && it != g_statuses.end(); ++it)
				any = SourceVisible(it->first, config);
			ReleaseSRWLockShared(&g_lock);
			return any;
		}

		void Draw(IDXGISwapChain* a_swapChain, float a_deltaSeconds)
		{
			if (!AnythingToDraw()) {
				return;  // nothing to draw: no cost in normal frames
			}

			D3D11_TEXTURE2D_DESC    desc{};
			ID3D11RenderTargetView* target = nullptr;
			if (g_options.gameTarget && !GameTarget(target, desc)) {
				target = nullptr;
			}
			if (!target) {
				ID3D11Texture2D* backBuffer = nullptr;
				if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || !backBuffer) {
					return;
				}
				backBuffer->GetDesc(&desc);
				// Made and released in each frame: a kept view would stop resolution changes.
				const HRESULT hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &target);
				backBuffer->Release();
				if (FAILED(hr) || !target) {
					return;
				}
			}

			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
			io.DeltaTime = a_deltaSeconds > 0.0f ? a_deltaSeconds : 1.0f / 60.0f;
			ImGui_ImplDX11_NewFrame();
			ImGui::NewFrame();
			BuildStack(io.DisplaySize);
			ImGui::Render();

			ID3D11RenderTargetView* oldTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			ID3D11DepthStencilView* oldDepth = nullptr;
			g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldTargets, &oldDepth);
			g_context->OMSetRenderTargets(1, &target, nullptr);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldTargets, oldDepth);
			for (auto old : oldTargets) {
				if (old) {
					old->Release();
				}
			}
			if (oldDepth) {
				oldDepth->Release();
			}
			target->Release();
		}

		HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
		{
			const PresentFn original = g_originalPresent.load();
			if ((a_flags & DXGI_PRESENT_TEST) == 0 && !g_failed) {
				if (!g_ready) {
					g_ready = Setup(a_swapChain);
					g_failed = !g_ready;
					QueryPerformanceCounter(&g_lastFrame);
				}
				if (g_ready) {
					LARGE_INTEGER now;
					QueryPerformanceCounter(&now);
					const float dt = static_cast<float>(static_cast<double>(now.QuadPart - g_lastFrame.QuadPart) / static_cast<double>(g_frequency.QuadPart));
					g_lastFrame = now;
					UpdateCycleKey(now);
					Draw(a_swapChain, dt);
				}
			}
			return original(a_swapChain, a_syncInterval, a_flags);
		}

		bool HookPresent(void* a_swapChain)
		{
			if (!a_swapChain || g_originalPresent.load()) {
				return false;
			}
			// The first field of a COM object points to its function table.
			void** table = *reinterpret_cast<void***>(a_swapChain);
			void** slot = &table[kPresentSlot];
			DWORD  oldProtect = 0;
			if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				return false;
			}
			g_originalPresent.store(reinterpret_cast<PresentFn>(*slot));
			InterlockedExchangePointer(slot, reinterpret_cast<void*>(&HookedPresent));
			VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
			return true;
		}
	}

	bool Install(void* a_swapChain, const Options& a_options)
	{
		QueryPerformanceFrequency(&g_frequency);
		g_options = a_options;
		return HookPresent(a_swapChain);
	}

	void Notify(const char* a_source, const char* a_text, float a_seconds, int a_level)
	{
		if (!a_text || !*a_text)
			return;
		Note note{ a_source ? a_source : "", a_text, a_level, a_seconds, {} };
		QueryPerformanceCounter(&note.start);
		AcquireSRWLockExclusive(&g_lock);
		g_notes.push_back(std::move(note));  // the queue: each note shows until its time ends
		ReleaseSRWLockExclusive(&g_lock);
	}

	void SetStatus(const char* a_source, const char* a_text)
	{
		const std::string source = a_source ? a_source : "";
		AcquireSRWLockExclusive(&g_lock);
		if (!a_text || !*a_text) {
			g_statuses.erase(source);
		} else {
			g_statuses[source] = a_text;
			NoteSource(source);
		}
		ReleaseSRWLockExclusive(&g_lock);
	}

	void SetWindow(const char* a_source, const char* a_id, const AbyssusWindow* a_window)
	{
		const std::string source = a_source ? a_source : "";
		const std::string id = a_id ? a_id : "";
		const std::string key = source + "/" + id;
		if (!a_window || a_window->itemCount <= 0 || !a_window->items) {
			AcquireSRWLockExclusive(&g_lock);
			g_windows.erase(key);
			ReleaseSRWLockExclusive(&g_lock);
			return;
		}
		// The copy is made before the lock: the render thread waits less.
		Window window;
		window.source = source;
		window.id = id;
		window.below = a_window->below ? a_window->below : "";
		window.backgroundAlpha = a_window->backgroundAlpha;
		window.wrapWidth = a_window->wrapWidth;
		window.items.reserve(static_cast<std::size_t>(a_window->itemCount));
		for (int i = 0; i < a_window->itemCount; ++i) {
			const AbyssusItem& in = a_window->items[i];
			Item               item;
			item.type = in.type;
			item.color = in.color;
			if (in.text)
				item.text = in.text;
			if (in.values && in.valueCount > 0)
				item.values.assign(in.values, in.values + in.valueCount);
			item.graphMin = in.graphMin;
			item.graphMax = in.graphMax;
			item.width = in.width;
			item.height = in.height;
			window.items.push_back(std::move(item));
		}
		AcquireSRWLockExclusive(&g_lock);
		auto& stored = g_windows[key];
		window.order = stored.order != 0 ? stored.order : g_nextId++;  // a change keeps the place
		stored = std::move(window);
		NoteSource(source);
		ReleaseSRWLockExclusive(&g_lock);
	}
}
