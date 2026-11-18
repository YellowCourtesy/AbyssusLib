#pragma once

// Data/SKSE/Plugins/AbyssusLib.ini: the key and the configurations of the
// stack. Standard C++ only.
//
// The file has "; Config version: x" as its second line. When a different
// version starts, the missing settings are added in place with their default
// values (the player's lines stay), and the version line changes.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace abyssus::settings
{
	// A configuration of the cycle: its name and the groups that show
	// (lower case). "others" and "notif" are special: see AbyssusLib.ini.
	struct Config
	{
		std::string              name;
		std::vector<std::string> groups;

		bool Shows(const std::string& a_group) const
		{
			for (const auto& group : groups) {
				if (group == a_group)
					return true;
			}
			return false;
		}
	};

	struct Settings
	{
		int                 cycleKey{ 0x7A };  // Windows virtual key code, 0x7A = F11
		int                 startConfig{ 0 };  // index in configs
		float               labelSeconds{ 3.0f };
		std::vector<Config> configs;
		std::map<std::string, std::string> groups;  // lower-case plugin name -> lower-case group ([Groups])
		std::map<std::string, int>         order;   // lower-case plugin name -> place ([Order])

		std::vector<std::string> added;  // what this version added to the file
		std::string              oldVersion;
		std::string              error;  // empty = the file was read (and written) correctly
	};

	// The configurations of the default text.
	std::vector<Config> DefaultConfigs();

	// Reads the file. Makes it with the default values if it is not there,
	// and adds new settings of a_version in place.
	Settings Load(const std::filesystem::path& a_path, const std::string& a_version);

	// The same, on text (for tests): a_text is the file, the updated text goes to a_out.
	Settings Parse(const std::string& a_text, const std::string& a_version, std::string& a_out, bool& a_changed);
}
