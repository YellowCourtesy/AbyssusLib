#pragma once

// Data/SKSE/Plugins/AbyssusLib.ini: the key and the configurations of the
// stack. Standard C++ only.
//
// The file has "; Config version: x" as its second line. When a different
// version starts, the missing settings are added in place with their default
// values (the player's lines stay), and the version line changes.

#include <filesystem>
#include <string>
#include <vector>

namespace abyssus::settings
{
	// The parts of the stack that a configuration can show.
	enum Group : unsigned
	{
		kDiag = 1u << 0,    // SkyrimDiagnostics
		kGps = 1u << 1,     // SkyGPS
		kOthers = 1u << 2,  // all other plugins
		kNotif = 1u << 3,   // the notification queue
	};

	struct Config
	{
		std::string name;
		unsigned    groups{ 0 };
	};

	struct Settings
	{
		int                 cycleKey{ 0x7A };  // Windows virtual key code, 0x7A = F11
		int                 startConfig{ 0 };  // index in configs
		float               labelSeconds{ 3.0f };
		std::vector<Config> configs;

		std::vector<std::string> added;  // what this version added to the file
		std::string              oldVersion;
		std::string              error;  // empty = the file was read (and written) correctly
	};

	// The default configurations: Diag / GPS / Notif, GPS / Notif, GPS, Notif, Off.
	std::vector<Config> DefaultConfigs();

	// Reads the file. Makes it with the default values if it is not there,
	// and adds new settings of a_version in place.
	Settings Load(const std::filesystem::path& a_path, const std::string& a_version);

	// The same, on text (for tests): a_text is the file, the updated text goes to a_out.
	Settings Parse(const std::string& a_text, const std::string& a_version, std::string& a_out, bool& a_changed);
}
