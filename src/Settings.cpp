#include "Settings.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "IniUpdate.h"

// Made by CMake from AbyssusLib.ini at the top level of the repository.
#include "DefaultIni.inc"

namespace abyssus::settings
{
	namespace
	{
		const std::string& DefaultText()
		{
			static const std::string text = reinterpret_cast<const char*>(kDefaultIni);
			return text;
		}

		std::string Lower(std::string a_text)
		{
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			return a_text;
		}

		unsigned ParseGroups(const std::string& a_text)
		{
			unsigned          groups = 0;
			std::stringstream stream(a_text);
			std::string       part;
			while (std::getline(stream, part, ',')) {
				part.erase(0, part.find_first_not_of(" \t"));
				part.erase(part.find_last_not_of(" \t") + 1);
				const std::string name = Lower(part);
				if (name == "diag")
					groups |= kDiag;
				else if (name == "gps")
					groups |= kGps;
				else if (name == "others")
					groups |= kOthers;
				else if (name == "notif")
					groups |= kNotif;
			}
			return groups;
		}

		std::vector<Config> ConfigsOf(const std::string& a_text)
		{
			std::vector<Config> configs;
			for (const auto& [name, parts] : iniupdate::Lines(a_text, "Configurations")) {
				if (!name.empty())
					configs.push_back({ name, ParseGroups(parts) });
			}
			return configs;
		}

		// The value of a key: from the file, else from the default text.
		std::string Value(const std::string& a_text, const char* a_section, const char* a_key)
		{
			for (const auto* text : { &a_text, &DefaultText() }) {
				const auto values = iniupdate::Values(*text);
				if (const auto s = values.find(Lower(a_section)); s != values.end()) {
					if (const auto k = s->second.find(Lower(a_key)); k != s->second.end())
						return k->second;
				}
			}
			return {};
		}
	}

	std::vector<Config> DefaultConfigs()
	{
		return ConfigsOf(DefaultText());
	}

	Settings Parse(const std::string& a_text, const std::string& a_version, std::string& a_out, bool& a_changed)
	{
		Settings   result;
		const auto update = iniupdate::Update(a_text, DefaultText(), a_version, { "configurations" });
		a_out = update.text;
		a_changed = update.changed;
		result.added = update.added;
		result.oldVersion = update.oldVersion;

		const auto number = [&](const char* a_section, const char* a_key, auto a_read) {
			const std::string text = Value(a_out, a_section, a_key);
			try {
				a_read(text);
			} catch (...) {
				result.error = "bad value for " + std::string(a_key) + ": '" + text + "', the default is used";
			}
		};
		number("Keys", "iCycleKey", [&](const std::string& a_v) { result.cycleKey = std::clamp(std::stoi(a_v, nullptr, 0), 1, 254); });
		number("Stack", "iStartConfig", [&](const std::string& a_v) { result.startConfig = std::stoi(a_v, nullptr, 0) - 1; });
		number("Stack", "fLabelSeconds", [&](const std::string& a_v) { result.labelSeconds = std::clamp(std::stof(a_v), 0.0f, 60.0f); });

		result.configs = ConfigsOf(a_out);
		if (result.configs.empty())
			result.configs = DefaultConfigs();
		result.startConfig = std::clamp(result.startConfig, 0, static_cast<int>(result.configs.size()) - 1);
		return result;
	}

	Settings Load(const std::filesystem::path& a_path, const std::string& a_version)
	{
		std::string text;
		const bool  exists = iniupdate::ReadFile(a_path, text);
		if (!exists)
			text = iniupdate::WithVersion(DefaultText(), a_version);

		std::string updated;
		bool        changed = false;
		Settings    result = Parse(text, a_version, updated, changed);
		if (exists && !changed)
			return result;
		if (!exists) {
			result.added.clear();  // a new file: nothing was "added"
			result.oldVersion.clear();
		}
		if (const auto error = iniupdate::WriteFile(a_path, updated); !error.empty())
			result.error = error;
		return result;
	}
}
