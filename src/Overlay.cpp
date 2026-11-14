#include "Overlay.h"

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
#include <map>
#include <string>
#include <vector>

// Direct3D 11 and Dear ImGui only (no CommonLibSSE in this file). The overlay code
// (Present hook, ImGui setup, game UI render target) comes from SkyrimDiagnostics,
// and the note rule too: one note at a time, a new note replaces the one that shows.

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

		SRWLOCK                            g_lock = SRWLOCK_INIT;
		Note                               g_note;      // the note that shows (empty text: none)
		std::map<std::string, std::string> g_statuses;  // source -> permanent status line
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

		void Draw(IDXGISwapChain* a_swapChain, float a_deltaSeconds)
		{
			AcquireSRWLockShared(&g_lock);
			const bool empty = g_note.text.empty() && g_statuses.empty();
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
}
