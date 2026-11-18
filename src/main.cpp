#include "Overlay.h"

#include "Abyssus/API.h"

#include "REX/W32/KERNEL32.h"

#include <cstring>

// AbyssusLib: shared services for the author's SKSE plugins.

namespace abyssus
{
	// The features of this DLL (AbyssusAPI_Has). Add a name for each new
	// feature; never remove or rename one.
	constexpr const char* kFeatures[] = {
		"notes",             // Notify: the notification queue
		"status",            // SetStatus
		"windows",           // SetWindow
		"persistent-notes",  // SetNote: notes with no time limit
		"stack",             // one stack at the top left, cycle key
		"settings-file",     // AbyssusLib.ini with the cycle key and the configurations
		"source-info",       // SetSourceInfo, [Groups] and [Order]: no plugin names in the code
		"sized-data",        // window records start with their size
	};
}
// Other plugins use include/Abyssus/API.h, which finds these exports at runtime.

extern "C" __declspec(dllexport) int AbyssusAPI_Version()
{
	return abyssus::kApiVersion;
}

// Features by name, so that a plugin can ask for what it needs, also from a
// newer or older AbyssusLib. A name that is not in the list: false.
extern "C" __declspec(dllexport) bool AbyssusAPI_Has(const char* a_feature)
{
	if (!a_feature)
		return false;
	for (const char* name : abyssus::kFeatures) {
		if (std::strcmp(name, a_feature) == 0)
			return true;
	}
	return false;
}

extern "C" __declspec(dllexport) void AbyssusAPI_Notify(const char* a_source, const char* a_text, float a_seconds, int a_level)
{
	abyssus::overlay::Notify(a_source, a_text, a_seconds, a_level);
}

extern "C" __declspec(dllexport) void AbyssusAPI_SetStatus(const char* a_source, const char* a_text)
{
	abyssus::overlay::SetStatus(a_source, a_text);
}

extern "C" __declspec(dllexport) void AbyssusAPI_SetNote(const char* a_source, const char* a_id, const char* a_text, int a_level)
{
	abyssus::overlay::SetNote(a_source, a_id, a_text, a_level);
}

extern "C" __declspec(dllexport) void AbyssusAPI_SetSourceInfo(const char* a_source, const char* a_group, int a_order)
{
	abyssus::overlay::SetSourceInfo(a_source, a_group, a_order);
}

extern "C" __declspec(dllexport) void AbyssusAPI_SetWindow(const char* a_source, const char* a_id, const AbyssusWindow* a_window)
{
	abyssus::overlay::SetWindow(a_source, a_id, a_window);
}

namespace
{
	// The render target that the game uses for its menus and HUD now (all runtimes).
	void* GameUiTarget()
	{
		const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		return renderer ? static_cast<void*>(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER].RTV) : nullptr;
	}

	// Data/SKSE/Plugins/AbyssusLib.ini in the game folder (not from the
	// current folder of the process, which a launcher can change).
	std::filesystem::path IniPath()
	{
		wchar_t    buffer[1024] = L"";
		const auto length = REX::W32::GetModuleFileNameW(nullptr, buffer, static_cast<std::uint32_t>(std::size(buffer)));
		const std::filesystem::path game = length > 0 && length < std::size(buffer) ? std::filesystem::path(buffer).parent_path() : std::filesystem::path{};
		return game / "Data" / "SKSE" / "Plugins" / "AbyssusLib.ini";
	}

	abyssus::settings::Settings g_settings;

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type != SKSE::MessagingInterface::kDataLoaded)
			return;
		if (!g_settings.added.empty()) {
			std::string text = "AbyssusLib\n\nThis version added new settings to AbyssusLib.ini, with their default values:\n";
			for (const auto& added : g_settings.added)
				text += "  " + added + "\n";
			RE::DebugMessageBox(text.c_str());
		}
		auto* window = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
		if (!window || !window->swapChain) {
			logger::error("Notes: no swap chain found");
			return;
		}
		abyssus::overlay::Options options;
		options.gameTarget = &GameUiTarget;
		options.cycleKey = g_settings.cycleKey;
		options.startConfig = g_settings.startConfig;
		options.labelSeconds = g_settings.labelSeconds;
		options.configs = g_settings.configs;
		options.groups = g_settings.groups;
		options.order = g_settings.order;
		const bool ok = abyssus::overlay::Install(static_cast<void*>(window->swapChain), options);
		logger::info("Notes: overlay {}", ok ? "ready" : "failed (Present hook)");
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	logger::info("AbyssusLib {} on Skyrim {}", ABYSSUS_VERSION, REL::Module::get().version().string());
	g_settings = abyssus::settings::Load(IniPath(), ABYSSUS_VERSION);
	if (!g_settings.error.empty())
		logger::error("Settings: {}", g_settings.error);
	std::string added;
	for (const auto& item : g_settings.added)
		added += (added.empty() ? "" : ", ") + item;
	logger::info("Settings: {} (version before: {}), key 0x{:X}, {} configurations, start: {}. Added: {}", IniPath().string(),
		g_settings.oldVersion.empty() ? "none" : g_settings.oldVersion, g_settings.cycleKey, g_settings.configs.size(),
		g_settings.configs.empty() ? "-" : g_settings.configs[static_cast<std::size_t>(g_settings.startConfig)].name, added.empty() ? "nothing" : added);
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
