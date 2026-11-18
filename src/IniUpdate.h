#pragma once

// Version line and in-place update of an ini file with ";" comments.
// Standard C++ only. The same file is in AbyssusLib and SkyGPS (a copy, not
// a shared library: each mod is built alone).
//
// The default text (the ini at the top level of the repository, put into the
// DLL by the build) has the line "; Config version: @VERSION@". When a file
// has a different version, Update() adds to it, in place, what the default
// text has and the file does not have:
//   - a key: with its comment lines, at the end of its [Section]
//   - a [Section] that the file does not have: all of it, at the end
//   - a list section (each line is an item, for example [Configurations]):
//     only if the file does not have the section; its lines are never added
//     one by one, so that items that the player removed stay removed
// Values, comments and order of the file stay. Then the version line changes.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace iniupdate
{
	// The version in the line "; Config version: x", or empty.
	std::string VersionOf(const std::string& a_text);

	// The default text with a_version in its version line.
	std::string WithVersion(const std::string& a_default, const std::string& a_version);

	struct Result
	{
		std::string              text;        // the updated text
		std::string              oldVersion;  // empty = the file had no version line
		std::vector<std::string> added;       // "[Section] key", "[Section]"
		bool                     changed{ false };
	};

	// a_listSections: lower-case names of list sections.
	Result Update(const std::string& a_text, const std::string& a_default, const std::string& a_version,
		const std::vector<std::string>& a_listSections = {});

	// The values of a text: lower-case "section" -> lower-case "key" -> value.
	// The lines of list sections: "section" -> "" -> not used; use Lines().
	std::map<std::string, std::map<std::string, std::string>> Values(const std::string& a_text);

	// The "key = value" lines of one section, in their order (key as written).
	std::vector<std::pair<std::string, std::string>> Lines(const std::string& a_text, const std::string& a_section);

	// Reads a file. False if it is not there or cannot be read.
	bool ReadFile(const std::filesystem::path& a_path, std::string& a_text);

	// Writes a new file and then puts it in the place of the old one. Gives
	// an empty text if correct, else the cause.
	std::string WriteFile(const std::filesystem::path& a_path, const std::string& a_text);
}
