#include "IniUpdate.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace iniupdate
{
	namespace
	{
		constexpr char kVersionMark[] = "; Config version:";
		constexpr char kVersionToken[] = "@VERSION@";

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

		bool IsComment(const std::string& a_text)
		{
			const std::string t = Trim(a_text);
			return !t.empty() && (t.front() == ';' || t.front() == '#');
		}

		struct Line
		{
			std::string text;
			std::string section;  // lower-case
			std::string key;      // lower-case, empty = not a "key = value" line
			bool        header{ false };
		};

		std::vector<Line> ToLines(const std::string& a_text)
		{
			std::vector<Line>  lines;
			std::istringstream stream(a_text);
			std::string        text;
			std::string        section;
			while (std::getline(stream, text)) {
				if (!text.empty() && text.back() == '\r')
					text.pop_back();
				Line              line{ text, section, {}, false };
				const std::string t = Trim(text);
				if (t.size() >= 2 && t.front() == '[') {
					const auto close = t.find(']');
					section = Lower(Trim(t.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
					line.section = section;
					line.header = true;
				} else if (!t.empty() && !IsComment(t)) {
					if (const auto equals = t.find('='); equals != std::string::npos)
						line.key = Lower(Trim(t.substr(0, equals)));
				}
				lines.push_back(std::move(line));
			}
			return lines;
		}

		std::string Join(const std::vector<Line>& a_lines)
		{
			std::string text;
			for (const auto& line : a_lines)
				text += line.text + "\n";
			return text;
		}

		// The comment lines just above line a_index (no blank line between).
		std::vector<std::string> CommentsAbove(const std::vector<Line>& a_lines, int a_index)
		{
			int first = a_index;
			while (first > 0 && IsComment(a_lines[first - 1].text) && Trim(a_lines[first - 1].text).rfind(kVersionMark, 0) != 0)
				--first;
			std::vector<std::string> texts;
			for (int i = first; i < a_index; ++i)
				texts.push_back(a_lines[i].text);
			return texts;
		}

		bool HasSection(const std::vector<Line>& a_lines, const std::string& a_section)
		{
			return std::any_of(a_lines.begin(), a_lines.end(), [&](const Line& a_l) { return a_l.header && a_l.section == a_section; });
		}
	}

	std::string VersionOf(const std::string& a_text)
	{
		for (const auto& line : ToLines(a_text)) {
			const std::string t = Trim(line.text);
			if (t.rfind(kVersionMark, 0) == 0)
				return Trim(t.substr(sizeof(kVersionMark) - 1));
		}
		return {};
	}

	std::string WithVersion(const std::string& a_default, const std::string& a_version)
	{
		std::string text = a_default;
		if (const auto at = text.find(kVersionToken); at != std::string::npos)
			text.replace(at, sizeof(kVersionToken) - 1, a_version);
		return text;
	}

	Result Update(const std::string& a_text, const std::string& a_default, const std::string& a_version, const std::vector<std::string>& a_listSections)
	{
		Result result;
		result.text = a_text;
		result.oldVersion = VersionOf(a_text);
		if (result.oldVersion == a_version)
			return result;

		std::vector<Line>       lines = ToLines(a_text);
		const std::vector<Line> defaults = ToLines(WithVersion(a_default, a_version));
		const auto              isList = [&](const std::string& a_section) {
            return std::find(a_listSections.begin(), a_listSections.end(), a_section) != a_listSections.end();
		};

		for (int i = 0; i < static_cast<int>(defaults.size()); ++i) {
			const Line& d = defaults[i];
			if (d.header && !HasSection(lines, d.section)) {
				// A whole section: its comments, its header, its lines.
				std::vector<std::string> add{ "" };
				for (auto& c : CommentsAbove(defaults, i))
					add.push_back(c);
				add.push_back(d.text);
				int j = i + 1;
				for (; j < static_cast<int>(defaults.size()) && !defaults[j].header; ++j)
					add.push_back(defaults[j].text);
				// Without the comment lines of the next section, and blank lines at the end.
				if (j < static_cast<int>(defaults.size())) {
					const auto next = CommentsAbove(defaults, j).size();
					add.resize(add.size() - std::min(next, add.size() - 1));
				}
				while (add.size() > 1 && Trim(add.back()).empty())
					add.pop_back();
				// The place: before the next section of the default text that the
				// file has (and before its comment lines), else at the end.
				int at = static_cast<int>(lines.size());
				for (int n = j; n < static_cast<int>(defaults.size()) && at == static_cast<int>(lines.size()); ++n) {
					if (!defaults[n].header)
						continue;
					for (int k = 0; k < static_cast<int>(lines.size()); ++k) {
						if (lines[k].header && lines[k].section == defaults[n].section) {
							at = k - static_cast<int>(CommentsAbove(lines, k).size());
							break;
						}
					}
				}
				if (at < static_cast<int>(lines.size())) {
					add.erase(add.begin());  // the blank line goes after the section, not before
					add.push_back("");
				}
				std::vector<Line> insert;
				for (const auto& text : add)
					insert.push_back(Line{ text, d.section, {}, false });
				lines.insert(lines.begin() + at, insert.begin(), insert.end());
				lines = ToLines(Join(lines));
				result.added.push_back("[" + Trim(d.text).substr(1, Trim(d.text).find(']') - 1) + "]");
				continue;
			}
			if (d.key.empty() || isList(d.section))
				continue;
			const bool has = std::any_of(lines.begin(), lines.end(), [&](const Line& a_l) { return a_l.section == d.section && a_l.key == d.key; });
			if (has)
				continue;
			// A key: at the end of its section, with its comments.
			int end = -1;
			for (int k = 0; k < static_cast<int>(lines.size()); ++k) {
				if (lines[k].section == d.section && !Trim(lines[k].text).empty())
					end = k + 1;
			}
			if (end < 0)
				continue;  // not possible: the section was added above
			std::vector<std::string> add = CommentsAbove(defaults, i);
			add.push_back(d.text);
			if (end < static_cast<int>(lines.size()) && lines[end].header)
				add.push_back("");
			std::vector<Line> insert;
			for (const auto& text : add)
				insert.push_back(Line{ text, d.section, {}, false });
			lines.insert(lines.begin() + end, insert.begin(), insert.end());
			lines = ToLines(Join(lines));
			const std::string sectionName = [&] {
				for (const auto& l : defaults) {
					if (l.header && l.section == d.section)
						return Trim(l.text).substr(1, Trim(l.text).find(']') - 1);
				}
				return d.section;
			}();
			result.added.push_back("[" + sectionName + "] " + Trim(d.text.substr(0, d.text.find('='))));
		}

		const std::string versionLine = std::string(kVersionMark) + " " + a_version;
		auto              found = std::find_if(lines.begin(), lines.end(), [](const Line& a_l) { return Trim(a_l.text).rfind(kVersionMark, 0) == 0; });
		if (found != lines.end())
			found->text = versionLine;
		else
			lines.insert(lines.begin() + (!lines.empty() && IsComment(lines[0].text) ? 1 : 0), Line{ versionLine, {}, {}, false });

		result.text = Join(lines);
		result.changed = true;
		return result;
	}

	std::map<std::string, std::map<std::string, std::string>> Values(const std::string& a_text)
	{
		std::map<std::string, std::map<std::string, std::string>> values;
		for (const auto& line : ToLines(a_text)) {
			if (line.key.empty())
				continue;
			const std::string t = Trim(line.text);
			std::string       value = Trim(t.substr(t.find('=') + 1));
			if (const auto comment = value.find(" ;"); comment != std::string::npos)
				value = Trim(value.substr(0, comment));
			values[line.section][line.key] = value;
		}
		return values;
	}

	std::vector<std::pair<std::string, std::string>> Lines(const std::string& a_text, const std::string& a_section)
	{
		std::vector<std::pair<std::string, std::string>> result;
		for (const auto& line : ToLines(a_text)) {
			if (line.key.empty() || line.section != Lower(a_section))
				continue;
			const std::string t = Trim(line.text);
			const auto        equals = t.find('=');
			result.emplace_back(Trim(t.substr(0, equals)), Trim(t.substr(equals + 1)));
		}
		return result;
	}

	bool ReadFile(const std::filesystem::path& a_path, std::string& a_text)
	{
		std::ifstream file(a_path, std::ios::binary);
		if (!file)
			return false;
		std::stringstream stream;
		stream << file.rdbuf();
		a_text = stream.str();
		return true;
	}

	std::string WriteFile(const std::filesystem::path& a_path, const std::string& a_text)
	{
		const auto&     path = a_path;
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
		auto temporary = path;
		temporary += ".tmp";
		{
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			out << a_text;
			out.flush();
			if (!out)
				return "could not write " + temporary.string();
		}
		std::filesystem::rename(temporary, path, ec);
		if (ec) {
			std::error_code copyEc;
			std::filesystem::copy_file(temporary, path, std::filesystem::copy_options::overwrite_existing, copyEc);
			std::filesystem::remove(temporary, ec);
			if (copyEc)
				return "could not replace " + path.string() + " (" + copyEc.message() + ")";
		}
		return {};
	}
}
