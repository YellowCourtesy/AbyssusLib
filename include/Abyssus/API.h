#pragma once

// AbyssusLib API for other SKSE plugins. Copy this one header into your plugin.
// It links nothing: it finds AbyssusLib.dll at runtime. If AbyssusLib is not
// installed, IsAvailable() is false and the calls do nothing.
//
// Needs CommonLibSSE-NG (it uses REX::W32 to find the DLL).
//
// All functions are safe from all threads.

#include <cstdint>

#include "REX/W32/KERNEL32.h"

namespace abyssus
{
	inline constexpr int kApiVersion = 1;

	enum class Level : int
	{
		kInfo = 0,
		kWarning = 1,
		kError = 2,
	};

	namespace detail
	{
		using VersionFn = int (*)();
		using NotifyFn = void (*)(const char* a_source, const char* a_text, float a_seconds, int a_level);
		using StatusFn = void (*)(const char* a_source, const char* a_text);

		struct Api
		{
			VersionFn version{ nullptr };
			NotifyFn  notify{ nullptr };
			StatusFn  status{ nullptr };
		};

		inline const Api& Get()
		{
			static const Api api = [] {
				Api a{};
				if (auto module = REX::W32::GetModuleHandleW(L"AbyssusLib.dll")) {
					a.version = reinterpret_cast<VersionFn>(REX::W32::GetProcAddress(module, "AbyssusAPI_Version"));
					if (a.version && a.version() >= kApiVersion) {
						a.notify = reinterpret_cast<NotifyFn>(REX::W32::GetProcAddress(module, "AbyssusAPI_Notify"));
						a.status = reinterpret_cast<StatusFn>(REX::W32::GetProcAddress(module, "AbyssusAPI_SetStatus"));
					}
				}
				return a;
			}();
			return api;
		}
	}

	// True if AbyssusLib.dll is loaded and has a compatible API. Call it at or
	// after SKSE kPostLoad (all plugins are loaded then).
	inline bool IsAvailable() { return detail::Get().notify != nullptr; }

	// Shows a note for a_seconds. All plugins share one note: a new note
	// replaces the note that shows (the same rule as SkyrimDiagnostics), so a
	// note is never old.
	inline void Notify(const char* a_source, const char* a_text, float a_seconds = 5.0f, Level a_level = Level::kInfo)
	{
		if (const auto fn = detail::Get().notify)
			fn(a_source, a_text, a_seconds, static_cast<int>(a_level));
	}

	// A permanent status line for a_source, shown until it is changed or
	// cleared. An empty or null a_text clears it.
	inline void SetStatus(const char* a_source, const char* a_text)
	{
		if (const auto fn = detail::Get().status)
			fn(a_source, a_text);
	}
}
