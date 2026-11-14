#include "Abyssus/Notify.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The overlay code comes from SkyrimDiagnostics (metakill, GPL-3.0): Present
// hook, ImGui setup and the choice of the game UI render target.

namespace abyssus
{
	namespace
	{
		// ---- process-wide registry ------------------------------------------------
		// A named file mapping that holds the host's note function. The name has the
		// process ID, so it is the same for all plugins in this game process only.

		constexpr std::uint32_t kMagic = 0x41425953;  // "ABYS"
		constexpr std::uint32_t kVersion = 1;

		using PostFn = void (*)(const char* a_source, const char* a_text, float a_seconds, int a_level);

		struct Registry
		{
			std::uint32_t magic;
			std::uint32_t version;
			PostFn        post;
		};

		Registry* g_registry = nullptr;
		bool      g_host = false;
		bool      g_initDone = false;

		Registry* OpenRegistry(bool& a_created)
		{
			wchar_t name[96];
			swprintf_s(name, L"Local\\AbyssusLib.Notify.%lu", GetCurrentProcessId());
			HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Registry), name);
			if (!mapping) {
				return nullptr;
			}
			a_created = GetLastError() != ERROR_ALREADY_EXISTS;
			// The handle stays open for the life of the process, so the mapping stays.
			return static_cast<Registry*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Registry)));
		}

		// ---- notes (host side) ----------------------------------------------------

		struct Note
		{
			std::string   source;
			std::string   text;
			int           level;
			float         seconds;
			LARGE_INTEGER start;
		};

		SRWLOCK           g_lock = SRWLOCK_INIT;
		std::vector<Note> g_notes;    // newest first
		std::vector<Note> g_pending;  // sent before Init()
		Options           g_options;
		LARGE_INTEGER     g_frequency{};

		void AddNote(std::vector<Note>& a_list, std::size_t a_max, const char* a_source, const char* a_text, float a_seconds, int a_level)
		{
			Note note{ a_source ? a_source : "", a_text ? a_text : "", a_level, a_seconds, {} };
			QueryPerformanceCounter(&note.start);
			AcquireSRWLockExclusive(&g_lock);
			std::erase_if(a_list, [&](const Note& n) { return n.source == note.source && n.text == note.text; });
			a_list.insert(a_list.begin(), std::move(note));
			if (a_list.size() > a_max) {
				a_list.resize(a_max);  // drop the oldest: never a backlog
			}
			ReleaseSRWLockExclusive(&g_lock);
		}

		void HostPost(const char* a_source, const char* a_text, float a_seconds, int a_level)
		{
			AddNote(g_notes, static_cast<std::size_t>(std::max(1, g_options.maxNotes)), a_source, a_text, a_seconds, a_level);
		}

		// ---- overlay (host side) --------------------------------------------------

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

			std::vector<Note> shown;
			AcquireSRWLockExclusive(&g_lock);
			std::erase_if(g_notes, [&](const Note& n) {
				return static_cast<double>(now.QuadPart - n.start.QuadPart) / static_cast<double>(g_frequency.QuadPart) > n.seconds;
			});
			shown = g_notes;
			ReleaseSRWLockExclusive(&g_lock);
			if (shown.empty()) {
				return;
			}

			ImGui::SetNextWindowPos(ImVec2(a_screen.x * g_options.x, a_screen.y * g_options.y), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
			ImGui::SetNextWindowBgAlpha(0.65f);
			const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
			                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
			if (ImGui::Begin("AbyssusLibNotes", nullptr, flags)) {
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
				for (const auto& n : shown) {
					const float age = static_cast<float>(static_cast<double>(now.QuadPart - n.start.QuadPart) / static_cast<double>(g_frequency.QuadPart));
					const float alpha = std::clamp(n.seconds - age, 0.0f, 1.0f);  // fade out in the last second
					if (!n.source.empty()) {
						ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, alpha), "%s:", n.source.c_str());
						ImGui::SameLine();
					}
					ImGui::TextColored(LevelColor(n.level, alpha), "%s", n.text.c_str());
				}
				ImGui::PopTextWrapPos();
			}
			ImGui::End();
		}

		void Draw(IDXGISwapChain* a_swapChain, float a_deltaSeconds)
		{
			AcquireSRWLockShared(&g_lock);
			const bool empty = g_notes.empty();
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

	bool Init(void* a_swapChain, const Options& a_options)
	{
		if (g_initDone) {
			return g_registry && g_registry->post;
		}
		g_initDone = true;
		QueryPerformanceFrequency(&g_frequency);

		bool created = false;
		g_registry = OpenRegistry(created);
		if (!g_registry) {
			return false;
		}
		if (created) {
			// First plugin in this process: become the host.
			g_options = a_options;
			if (!HookPresent(a_swapChain)) {
				return false;
			}
			g_registry->magic = kMagic;
			g_registry->version = kVersion;
			g_registry->post = &HostPost;
			g_host = true;
		} else if (g_registry->magic != kMagic || g_registry->version != kVersion || !g_registry->post) {
			return false;  // another AbyssusLib version, or the host failed
		}

		// Send the notes that came before Init().
		AcquireSRWLockExclusive(&g_lock);
		auto pending = std::move(g_pending);
		g_pending.clear();
		ReleaseSRWLockExclusive(&g_lock);
		for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
			g_registry->post(it->source.c_str(), it->text.c_str(), it->seconds, it->level);
		}
		return true;
	}

	void Notify(const char* a_source, const char* a_text, float a_seconds, Level a_level)
	{
		if (g_registry && g_registry->post) {
			g_registry->post(a_source, a_text, a_seconds, static_cast<int>(a_level));
		} else if (!g_initDone) {
			AddNote(g_pending, 8, a_source, a_text, a_seconds, static_cast<int>(a_level));
		}
	}

	bool IsHost()
	{
		return g_host;
	}
}
