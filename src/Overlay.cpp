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
// draws it. The plugins have no ImGui.

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
			std::string       below;  // key of the window above this one, or empty
			float             x{ 0.0f };
			float             y{ 0.0f };
			float             backgroundAlpha{ 0.6f };
			float             wrapWidth{ 0.0f };
			std::vector<Item> items;
		};

		SRWLOCK                            g_lock = SRWLOCK_INIT;
		Note                               g_note;      // the note that shows (empty text: none)
		std::map<std::string, std::string> g_statuses;  // source -> permanent status line
		std::map<std::string, Window>      g_windows;   // "source/id" -> window
		Options                            g_options;
		LARGE_INTEGER                      g_frequency{};

		float Seconds(const LARGE_INTEGER& a_from, const LARGE_INTEGER& a_to)
		{
			return static_cast<float>(static_cast<double>(a_to.QuadPart - a_from.QuadPart) / static_cast<double>(g_frequency.QuadPart));
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

		void BuildNotes(const ImVec2& a_screen)
		{
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);

			Note                                             note;
			std::vector<std::pair<std::string, std::string>> statuses;
			AcquireSRWLockExclusive(&g_lock);
			if (!g_note.text.empty() && Seconds(g_note.start, now) > g_note.seconds)
				g_note.text.clear();
			note = g_note;
			statuses.assign(g_statuses.begin(), g_statuses.end());
			ReleaseSRWLockExclusive(&g_lock);
			if (note.text.empty() && statuses.empty())
				return;

			ImGui::SetNextWindowPos(ImVec2(a_screen.x * g_options.x, a_screen.y * g_options.y), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
			ImGui::SetNextWindowBgAlpha(0.65f);
			const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
			                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
			if (ImGui::Begin("AbyssusLibNotes", nullptr, flags)) {
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
				for (const auto& [source, text] : statuses) {
					ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 0.9f), "%s:", source.c_str());
					ImGui::SameLine();
					ImGui::TextColored(ImVec4(0.8f, 0.85f, 0.9f, 0.9f), "%s", text.c_str());
				}
				if (!note.text.empty()) {
					const float alpha = std::clamp(note.seconds - Seconds(note.start, now), 0.0f, 1.0f);  // fade out in the last second
					if (!statuses.empty())
						ImGui::Separator();
					if (!note.source.empty()) {
						ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, alpha), "%s:", note.source.c_str());
						ImGui::SameLine();
					}
					ImGui::TextColored(LevelColor(note.level, alpha), "%s", note.text.c_str());
				}
				ImGui::PopTextWrapPos();
			}
			ImGui::End();
		}

		std::string WindowKey(const char* a_source, const char* a_id)
		{
			return std::string(a_source ? a_source : "") + "/" + (a_id ? a_id : "");
		}

		// Gives the bottom edge of the window, in pixels.
		float BuildWindow(const std::string& a_key, const Window& a_window, float a_y)
		{
			float bottom = a_y;
			ImGui::SetNextWindowPos(ImVec2(a_window.x, a_y), ImGuiCond_Always);
			ImGui::SetNextWindowBgAlpha(std::clamp(a_window.backgroundAlpha, 0.0f, 1.0f));
			const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
			                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
			if (ImGui::Begin(("##" + a_key).c_str(), nullptr, flags)) {
				if (a_window.wrapWidth > 0.0f)
					ImGui::PushTextWrapPos(a_window.wrapWidth);
				bool lineOpen = false;  // a text is on the current line
				for (const auto& item : a_window.items) {
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
							ImGui::PlotLines(("##graph" + std::to_string(&item - a_window.items.data())).c_str(), item.values.data(),
								static_cast<int>(item.values.size()), 0, nullptr, item.graphMin, item.graphMax, ImVec2(item.width, item.height));
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
				bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
			}
			ImGui::End();
			return bottom;
		}

		// Plugin windows. A window with "below" goes under that window when it shows.
		void BuildWindows()
		{
			AcquireSRWLockShared(&g_lock);
			std::map<std::string, float> bottoms;  // key -> bottom of the drawn window
			std::vector<const std::pair<const std::string, Window>*> waiting;
			for (const auto& entry : g_windows)
				waiting.push_back(&entry);
			// Each pass draws the windows whose upper window is drawn or does not show.
			for (int pass = 0; pass < 8 && !waiting.empty(); ++pass) {
				std::vector<const std::pair<const std::string, Window>*> next;
				for (const auto* entry : waiting) {
					const Window& window = entry->second;
					float         y = window.y;
					if (!window.below.empty() && g_windows.contains(window.below)) {
						const auto above = bottoms.find(window.below);
						if (above == bottoms.end() && pass < 7) {
							next.push_back(entry);  // the upper window is not drawn yet
							continue;
						}
						if (above != bottoms.end())
							y = above->second + 6.0f;
					}
					bottoms[entry->first] = BuildWindow(entry->first, window, y);
				}
				waiting = std::move(next);
			}
			ReleaseSRWLockShared(&g_lock);
		}

		void Draw(IDXGISwapChain* a_swapChain, float a_deltaSeconds)
		{
			AcquireSRWLockShared(&g_lock);
			const bool empty = g_note.text.empty() && g_statuses.empty() && g_windows.empty();
			ReleaseSRWLockShared(&g_lock);
			if (empty) {
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
			BuildWindows();
			BuildNotes(io.DisplaySize);
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
		Note note{ a_source ? a_source : "", a_text ? a_text : "", a_level, a_seconds, {} };
		QueryPerformanceCounter(&note.start);
		AcquireSRWLockExclusive(&g_lock);
		g_note = std::move(note);  // replaces the note that shows: no queue, no backlog
		ReleaseSRWLockExclusive(&g_lock);
	}

	void SetStatus(const char* a_source, const char* a_text)
	{
		const std::string source = a_source ? a_source : "";
		AcquireSRWLockExclusive(&g_lock);
		if (!a_text || !*a_text)
			g_statuses.erase(source);
		else
			g_statuses[source] = a_text;
		ReleaseSRWLockExclusive(&g_lock);
	}

	void SetWindow(const char* a_source, const char* a_id, const AbyssusWindow* a_window)
	{
		const std::string key = WindowKey(a_source, a_id);
		if (!a_window || a_window->itemCount <= 0 || !a_window->items) {
			AcquireSRWLockExclusive(&g_lock);
			g_windows.erase(key);
			ReleaseSRWLockExclusive(&g_lock);
			return;
		}
		// The copy is made before the lock: the render thread waits less.
		Window window;
		window.source = a_source ? a_source : "";
		window.below = a_window->below && *a_window->below ? WindowKey(a_source, a_window->below) : std::string{};
		window.x = a_window->x;
		window.y = a_window->y;
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
		g_windows[key] = std::move(window);
		ReleaseSRWLockExclusive(&g_lock);
	}
}
