#include "Settings.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace abyssus::settings
{
	namespace
	{
		constexpr char kVersionMark[] = "; Config version:";

		// One setting: its section, key, default value and comment lines.
		struct Entry
		{
			const char*              section;
			const char*              key;
			const char*              value;
			std::vector<const char*> comments;
		};

		const std::vector<Entry>& Entries()
		{
			static const std::vector<Entry> entries{
				{ "Keys", "iCycleKey", "0x7A", { "; Key that cycles the configurations. Windows virtual key code: 0x7A = F11." } },
				{ "Stack", "iStartConfig", "1", { "; Configuration at the game start: its number in the [Configurations] list (1 = the first)." } },
				{ "Stack", "fLabelSeconds", "3", { "; Seconds that the name of a new configuration shows at the top center (0 = never)." } },
			};
			return entries;
		}

		constexpr const char* kConfigsComment[] = {
			"; One line for each configuration, in the order of the cycle:",
			";   name = the parts that show, separated by commas",
			"; Parts: Diag (SkyrimDiagnostics), GPS (SkyGPS), Others (all other plugins),",
			"; Notif (the notification queue). Nothing after \"=\": nothing shows.",
		};

		std::string Trim(const std::string& a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
				return {};
			return a_text.substr(first, a_text.find_last_not_of(" \t\r\n") - first + 1);
		}

		std::string Lower(std::string a_text)
		{
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			return a_text;
		}

		std::string SectionOf(const std::string& a_line)
		{
			const std::string t = Trim(a_line);
			if (t.size() < 2 || t.front() != '[')
				return {};
			const auto close = t.find(']');
			return Lower(Trim(t.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
		}

		std::string ConfigLine(const Config& a_config)
		{
			std::string parts;
			const std::pair<unsigned, const char*> names[] = { { kDiag, "Diag" }, { kGps, "GPS" }, { kOthers, "Others" }, { kNotif, "Notif" } };
			for (const auto& [group, name] : names) {
				if (a_config.groups & group)
					parts += (parts.empty() ? "" : ", ") + std::string(name);
			}
			return a_config.name + (parts.empty() ? " =" : " = " + parts);
		}

		std::vector<std::string> DefaultLines(const std::string& a_version)
		{
			std::vector<std::string> lines{
				"; AbyssusLib settings. This file is made on the first launch.",
				std::string(kVersionMark) + " " + a_version,
				"; Do not change the line above. A new version of AbyssusLib adds its new",
				"; settings to this file with their default values, and changes that line.",
				"; Your values, comments and order stay. Delete this file to restore all",
				"; default values.",
			};
			std::string section;
			for (const auto& e : Entries()) {
				if (section != e.section) {
					section = e.section;
					lines.push_back("");
					lines.push_back("[" + section + "]");
				}
				for (const char* comment : e.comments)
					lines.push_back(comment);
				lines.push_back(std::string(e.key) + " = " + e.value);
			}
			lines.push_back("");
			lines.push_back("[Configurations]");
			for (const char* comment : kConfigsComment)
				lines.push_back(comment);
			for (const auto& config : DefaultConfigs())
				lines.push_back(ConfigLine(config));
			return lines;
		}

		unsigned ParseGroups(const std::string& a_text)
		{
			unsigned          groups = 0;
			std::stringstream stream(a_text);
			std::string       part;
			while (std::getline(stream, part, ',')) {
				const std::string name = Lower(Trim(part));
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
	}

	std::vector<Config> DefaultConfigs()
	{
		return {
			{ "Diag / GPS / Notif", kDiag | kGps | kOthers | kNotif },
			{ "GPS / Notif", kGps | kOthers | kNotif },
			{ "GPS", kGps },
			{ "Notif", kOthers | kNotif },
			{ "Off", 0 },
		};
	}

	Settings Parse(const std::string& a_text, const std::string& a_version, std::string& a_out, bool& a_changed)
	{
		Settings result;
		a_changed = false;

		std::vector<std::string> lines;
		{
			std::istringstream stream(a_text);
			std::string        line;
			while (std::getline(stream, line)) {
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				lines.push_back(line);
			}
		}

		// Read the values.
		std::vector<std::pair<std::string, std::string>> values;  // lower-case key -> value (settings sections)
		bool                                             hasConfigs = false;
		std::string                                      section;
		for (const auto& raw : lines) {
			const std::string line = Trim(raw);
			if (line.rfind(kVersionMark, 0) == 0) {
				result.oldVersion = Trim(line.substr(sizeof(kVersionMark) - 1));
				continue;
			}
			if (const auto name = SectionOf(line); !name.empty()) {
				section = name;
				hasConfigs = hasConfigs || section == "configurations";
				continue;
			}
			if (line.empty() || line[0] == ';' || line[0] == '#')
				continue;
			const auto equals = line.find('=');
			if (equals == std::string::npos)
				continue;
			const std::string key = Trim(line.substr(0, equals));
			const std::string value = Trim(line.substr(equals + 1));
			if (section == "configurations") {
				if (!key.empty())
					result.configs.push_back({ key, ParseGroups(value) });
			} else {
				values.emplace_back(Lower(key), value);
			}
		}

		const auto find = [&](const char* a_key) -> const std::string* {
			for (const auto& [key, value] : values) {
				if (key == Lower(a_key))
					return &value;
			}
			return nullptr;
		};
		for (const auto& e : Entries()) {
			const std::string* value = find(e.key);
			const std::string  text = value ? *value : e.value;
			try {
				if (std::string_view(e.key) == "iCycleKey")
					result.cycleKey = std::clamp(std::stoi(text, nullptr, 0), 1, 254);
				else if (std::string_view(e.key) == "iStartConfig")
					result.startConfig = std::stoi(text, nullptr, 0) - 1;
				else if (std::string_view(e.key) == "fLabelSeconds")
					result.labelSeconds = std::clamp(std::stof(text), 0.0f, 60.0f);
			} catch (...) {
				result.error = "bad value for " + std::string(e.key) + ": '" + text + "', the default is used";
			}
		}
		if (result.configs.empty())
			result.configs = DefaultConfigs();
		result.startConfig = std::clamp(result.startConfig, 0, static_cast<int>(result.configs.size()) - 1);

		// Update in place if the version is different.
		a_out = a_text;
		if (result.oldVersion == a_version)
			return result;

		for (const auto& e : Entries()) {
			if (find(e.key))
				continue;
			int  end = -1;  // after the last line with text in the section
			bool in = false;
			for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
				if (const auto name = SectionOf(lines[i]); !name.empty())
					in = name == Lower(e.section);
				if (in && !Trim(lines[i]).empty())
					end = i + 1;
			}
			std::vector<std::string> add;
			if (end < 0) {
				// A new section: before [Configurations], or at the end.
				add = { "", "[" + std::string(e.section) + "]" };
				end = static_cast<int>(lines.size());
				for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
					if (SectionOf(lines[i]) == "configurations") {
						end = i;
						add = { "[" + std::string(e.section) + "]" };
						break;
					}
				}
			}
			for (const char* comment : e.comments)
				add.push_back(comment);
			add.push_back(std::string(e.key) + " = " + e.value);
			if (end < static_cast<int>(lines.size()) && !SectionOf(lines[end]).empty())
				add.push_back("");
			lines.insert(lines.begin() + end, add.begin(), add.end());
			result.added.push_back("[" + std::string(e.section) + "] " + e.key);
		}
		if (!hasConfigs) {
			lines.push_back("");
			lines.push_back("[Configurations]");
			for (const char* comment : kConfigsComment)
				lines.push_back(comment);
			for (const auto& config : DefaultConfigs())
				lines.push_back(ConfigLine(config));
			result.added.push_back("[Configurations]");
		}

		const std::string versionLine = std::string(kVersionMark) + " " + a_version;
		const auto        found = std::find_if(lines.begin(), lines.end(), [](const std::string& a_line) { return Trim(a_line).rfind(kVersionMark, 0) == 0; });
		if (found != lines.end())
			*found = versionLine;
		else
			lines.insert(lines.begin() + (!lines.empty() && Trim(lines[0]).rfind(";", 0) == 0 ? 1 : 0), versionLine);

		a_out.clear();
		for (const auto& line : lines)
			a_out += line + "\n";
		a_changed = true;
		return result;
	}

	Settings Load(const std::filesystem::path& a_path, const std::string& a_version)
	{
		std::error_code ec;
		std::string     text;
		const bool      exists = std::filesystem::exists(a_path, ec);
		if (exists) {
			std::ifstream     file(a_path, std::ios::binary);
			std::stringstream stream;
			stream << file.rdbuf();
			text = stream.str();
		} else {
			for (const auto& line : DefaultLines(a_version))
				text += line + "\n";
		}

		std::string updated;
		bool        changed = false;
		Settings    result = Parse(text, a_version, updated, changed);
		if (!exists) {
			updated = text;
			changed = true;
			result.added.clear();
			result.oldVersion.clear();
		}
		if (!changed)
			return result;

		// A new file first, then in the place of the old one.
		std::filesystem::create_directories(a_path.parent_path(), ec);
		auto temporary = a_path;
		temporary += ".tmp";
		{
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			out << updated;
			out.flush();
			if (!out) {
				result.error = "could not write " + temporary.string();
				return result;
			}
		}
		std::filesystem::rename(temporary, a_path, ec);
		if (ec) {
			std::error_code copyEc;
			std::filesystem::copy_file(temporary, a_path, std::filesystem::copy_options::overwrite_existing, copyEc);
			std::filesystem::remove(temporary, ec);
			if (copyEc)
				result.error = "could not replace " + a_path.string() + " (" + copyEc.message() + ")";
		}
		return result;
	}
}
