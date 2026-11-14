#include "Overlay.h"

// AbyssusLib: shared services for the author's SKSE plugins.
// Other plugins use include/Abyssus/API.h, which finds these exports at runtime.

extern "C" __declspec(dllexport) int AbyssusAPI_Version()
{
	return 1;
}

extern "C" __declspec(dllexport) void AbyssusAPI_Notify(const char* a_source, const char* a_text, float a_seconds, int a_level)
{
	abyssus::overlay::Notify(a_source, a_text, a_seconds, a_level);
}

extern "C" __declspec(dllexport) void AbyssusAPI_SetStatus(const char* a_source, const char* a_text)
{
	abyssus::overlay::SetStatus(a_source, a_text);
}

namespace
{
	void* GameUiTarget()
	{
		auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
		return data ? static_cast<void*>(data->renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER].RTV) : nullptr;
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type != SKSE::MessagingInterface::kDataLoaded)
			return;
		auto* window = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
		if (!window || !window->swapChain) {
			logger::error("Notes: no swap chain found");
			return;
		}
		abyssus::overlay::Options options;
		options.gameTarget = &GameUiTarget;
		const bool ok = abyssus::overlay::Install(static_cast<void*>(window->swapChain), options);
		logger::info("Notes: overlay {}", ok ? "ready" : "failed (Present hook)");
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	logger::info("AbyssusLib {} on Skyrim {}", "1.0.0", REL::Module::get().version().string());
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
