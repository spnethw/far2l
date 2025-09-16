#if defined (__linux__)

#include "LinuxAppProvider.hpp"
#include "WideMB.h"
#include "common.hpp"
#include "utils.h"
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <wctype.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>


std::string LinuxAppProvider::EscapePathForShell(const std::string& path)
{
	std::string escaped_path;
	escaped_path.reserve(path.size() + 2);
	escaped_path += '\'';
	for (char c : path) {
		if (c == '\'') {
			escaped_path += "'\\''";
		} else {
			escaped_path += c;
		}
	}
	escaped_path += '\'';
	return escaped_path;
}


std::string LinuxAppProvider::Trim(std::string str)
{
	str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) { return !std::isspace(ch); }));
	str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), str.end());
	return str;
}


std::string LinuxAppProvider::RunCommandAndCaptureOutput(const std::string& cmd)
{
	std::string result;
	return POpen(result, cmd.c_str()) ? Trim(result) : "";
}

// Query system default application for given MIME type using xdg-mime
std::string LinuxAppProvider::GetDefaultApp(const std::string& mime_type)
{
	std::string escaped_mime = EscapePathForShell(mime_type);
	std::string cmd = "xdg-mime query default " + escaped_mime + " 2>/dev/null";
	return RunCommandAndCaptureOutput(cmd);
}

// Check if directory exists and is actually a directory
bool LinuxAppProvider::IsValidApplicationsDir(const std::string& path)
{
	struct stat buffer;
	if (stat(path.c_str(), &buffer) != 0) return false;
	return S_ISDIR(buffer.st_mode);
}

// Get user-specific application directories according to XDG Base Directory spec
// Checks XDG_DATA_HOME or falls back to ~/.local/share/applications
std::vector<std::string> LinuxAppProvider::GetUserDirs()
{
	std::vector<std::string> dirs;
	const char* xdg_data_home = getenv("XDG_DATA_HOME");
	if (xdg_data_home && *xdg_data_home) {
		std::string path = std::string(xdg_data_home) + "/applications";
		if (IsValidApplicationsDir(path)) dirs.push_back(path);
	} else {
		auto home = GetMyHome();
		if (!home.empty()) {
			std::string path = home + "/.local/share/applications";
			if (IsValidApplicationsDir(path)) dirs.push_back(path);
		}
	}
	return dirs;
}

// Get system-wide application directories according to XDG Base Directory spec
// Parses XDG_DATA_DIRS or uses standard fallback locations
std::vector<std::string> LinuxAppProvider::GetSystemDirs()
{
	std::vector<std::string> dirs;
	const char* xdg_data_dirs = getenv("XDG_DATA_DIRS");
	if (xdg_data_dirs && *xdg_data_dirs) {
		std::stringstream ss(xdg_data_dirs);
		std::string dir;
		size_t count = 0;
		while (std::getline(ss, dir, ':') && count < 50) {
			if (!dir.empty()) {
				std::string path = dir + "/applications";
				if (IsValidApplicationsDir(path)) dirs.push_back(path);
			}
			++count;
		}
	} else {
		std::string paths[] = {"/usr/local/share/applications", "/usr/share/applications"};
		for (const auto& path : paths) {
			if (IsValidApplicationsDir(path)) dirs.push_back(path);
		}
	}
	return dirs;
}


// Combine directories: user ones have higher priority than system ones
std::vector<std::string> LinuxAppProvider::GetXDGDataDirs()
{
	auto dirs = GetUserDirs();
	auto system_dirs = GetSystemDirs();
	dirs.insert(dirs.end(), system_dirs.begin(), system_dirs.end());
	return dirs;
}


std::vector<std::string> LinuxAppProvider::CollectAndPrioritizeMimeTypes(const std::wstring& pathname)
{
	std::vector<std::string> mime_types;
	std::unordered_set<std::string> seen;

	// Helper lambda to add unique MIME types
	auto add_unique = [&](std::string mime) {
		if (!mime.empty() && mime.find('/') != std::string::npos && seen.insert(mime).second) {
			mime_types.push_back(std::move(mime));
		}
	};

	std::string narrow_path = StrWide2MB(pathname);
	std::string escaped_path = EscapePathForShell(narrow_path);

	// Priority #1: xdg-mime (most accurate, respects user preferences)
	add_unique(RunCommandAndCaptureOutput("xdg-mime query filetype " + escaped_path + " 2>/dev/null"));

	// Priority #2: file command (libmagic-based detection)
	add_unique(RunCommandAndCaptureOutput("file -b --mime-type " + escaped_path + " 2>/dev/null"));

	// Priority #3: Generalize MIME types by removing "+suffix" extensions
	std::vector<std::string> base_types = mime_types;
	for (const auto& mime : base_types) {
		size_t plus_pos = mime.find('+');
		if (plus_pos != std::string::npos) {
			add_unique(mime.substr(0, plus_pos));
		}
	}

	// Priority #4: Extension-based MIME type mapping for common file types
	size_t dot_pos = pathname.rfind(L'.');
	if (dot_pos != std::wstring::npos) {
		std::wstring ext = pathname.substr(dot_pos);
		std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
		if (ext == L".sh" || ext == L".bash" || ext == L".csh") add_unique("text/x-shellscript");
		if (ext == L".py") add_unique("text/x-python");
		if (ext == L".pl") add_unique("text/x-perl");
		if (ext == L".rb") add_unique("text/x-ruby");
		if (ext == L".js") add_unique("text/javascript");
		if (ext == L".html" || ext == L".htm") add_unique("text/html");
		if (ext == L".xml") add_unique("application/xml");
		if (ext == L".pdf") add_unique("application/pdf");
		if (ext == L".exe") add_unique("application/x-ms-dos-executable");
		if (ext == L".bin" || ext == L".elf") add_unique("application/x-executable");
		if (ext == L".txt" || ext == L".conf" || ext == L".cfg") add_unique("text/plain");
		if (ext == L".md") add_unique("text/markdown");
		if (ext == L".jpg" || ext == L".jpeg") add_unique("image/jpeg");
		if (ext == L".png") add_unique("image/png");
		if (ext == L".gif") add_unique("image/gif");
		if (ext == L".doc") add_unique("application/msword");
		if (ext == L".odt") add_unique("application/vnd.oasis.opendocument.text");
		if (ext == L".zip") add_unique("application/zip");
		if (ext == L".tar") add_unique("application/x-tar");
		if (ext == L".gz") add_unique("application/gzip");
	}

	// Priority #5: Add wildcard fallbacks (e.g., "text/*" for any "text/something")
	for (const auto& mime : base_types) {
		size_t slash_pos = mime.find('/');
		if (slash_pos != std::string::npos) {
			add_unique(mime.substr(0, slash_pos) + "/*");
		}
		// For text files, always add text/plain as fallback
		if (mime.rfind("text/", 0) == 0) {
			add_unique("text/plain");
		}
	}

	// Final fallback for binary files
	add_unique("application/octet-stream");

	return mime_types;
}


// Extract basename from full desktop file path
std::string LinuxAppProvider::GetDesktopBasename(const std::wstring& full_path)
{
	size_t last_slash = full_path.find_last_of(L'/');
	if (last_slash == std::wstring::npos) {
		return StrWide2MB(full_path);
	}
	return StrWide2MB(full_path.substr(last_slash + 1));
}


// Main function to find all application candidates for a given file
// Implements complex ranking algorithm considering MIME type specificity and source priority
std::vector<CandidateInfo> LinuxAppProvider::GetAppCandidates(const std::wstring& pathname)
{
	// Get prioritized list of MIME types for the file
	std::vector<std::string> prioritized_mimes = CollectAndPrioritizeMimeTypes(pathname);
	if (prioritized_mimes.empty()) {
		return {};
	}

	std::string primary_mime = prioritized_mimes[0];

	// Parse and merge all mimeapps.list files
	auto mimeapps_paths = GetMimeAppsPaths();
	MimeAppsData merged = MergeMimeApps(mimeapps_paths);

	// Get system default application for primary MIME type
	std::string default_desktop_str = GetDefaultApp(primary_mime);

	// Set up directory priorities for ranking
	auto xdg_dirs = GetXDGDataDirs();
	std::unordered_map<std::string, int> dir_to_prio;
	for (size_t i = 0; i < xdg_dirs.size(); ++i) {
		dir_to_prio[xdg_dirs[i]] = static_cast<int>(i);
	}

	std::vector<RankedCandidate> candidates;
	std::unordered_set<std::string> seen_desktops;

	// Step 1: Add system default application with highest priority
	if (!default_desktop_str.empty()) {
		auto path_opt = FindDesktopFileLocation(default_desktop_str);
		if (path_opt) {
			auto info_opt = ParseDesktopFile(*path_opt);
			if (info_opt) {
				std::string desk_basename = GetDesktopBasename(info_opt->desktop_file);
				if (seen_desktops.insert(desk_basename).second) {
					int rank = 0;  // Highest priority rank
					candidates.push_back({*info_opt, rank, true});
				}
			}
		}
	}

	// Step 2: Process applications from mimeapps.list files
	// Iterate through MIME types in priority order
	for (size_t mime_idx = 0; mime_idx < prioritized_mimes.size(); ++mime_idx) {
		std::string mime = prioritized_mimes[mime_idx];

		std::vector<std::pair<std::string, int>> app_list;

		// Add default applications for this MIME type
		auto def_it = merged.defaults.find(mime);
		if (def_it != merged.defaults.end()) {
			std::string desk = def_it->second.first;
			auto rem_it = merged.removed.find(mime);
			const std::unordered_set<std::string>& rem_set = (rem_it != merged.removed.end()) ? rem_it->second : std::unordered_set<std::string>{};
			// Only add if not in removed list
			if (rem_set.find(desk) == rem_set.end()) {
				app_list.emplace_back(desk, def_it->second.second);
			}
		}

		// Add applications from "Added Associations" section
		auto add_it = merged.added.find(mime);
		if (add_it != merged.added.end()) {
			for (const auto& assoc : add_it->second) {
				std::string desk = assoc.desktop;
				auto rem_it = merged.removed.find(mime);
				const std::unordered_set<std::string>& rem_set = (rem_it != merged.removed.end()) ? rem_it->second : std::unordered_set<std::string>{};
				// Skip if in removed list
				if (rem_set.find(desk) != rem_set.end()) continue;

				// Update priority if already exists, otherwise add new
				bool found = false;
				for (auto& pr : app_list) {
					if (pr.first == desk) {
						pr.second = std::min(pr.second, assoc.prio);  // Take best priority
						found = true;
						break;
					}
				}
				if (!found) {
					app_list.emplace_back(desk, assoc.prio);
				}
			}
		}

		// Convert mimeapps.list entries to candidates with appropriate ranking
		for (const auto& pr : app_list) {
			std::string desk = pr.first;
			auto path_opt = FindDesktopFileLocation(desk);
			if (!path_opt) continue;

			auto info_opt = ParseDesktopFile(*path_opt);
			if (!info_opt) continue;

			std::string desk_basename = GetDesktopBasename(info_opt->desktop_file);
			if (seen_desktops.insert(desk_basename).second) {
				// Ranking formula: MIME type index * 1000 + priority * 10 + 100 (for non-default)
				int base_rank = static_cast<int>(mime_idx) * 1000 + pr.second * 10 + 100;
				bool is_def = (mime == primary_mime && desk == default_desktop_str);
				candidates.push_back({*info_opt, base_rank, is_def});
			}
		}
	}

	// Step 3: Scan directories for .desktop files that declare MIME type support
	// This catches applications not explicitly listed in mimeapps.list
	for (const std::string& dir : xdg_dirs) {
		auto dir_prio_it = dir_to_prio.find(dir);
		if (dir_prio_it == dir_to_prio.end()) continue;
		int dir_prio = dir_prio_it->second;

		DIR* d = opendir(dir.c_str());
		if (!d) continue;

		struct dirent* ent;
		while ((ent = readdir(d)) != nullptr) {
			if (ent->d_type != DT_REG) continue;  // Only regular files
			std::string fname = ent->d_name;
			// Check for .desktop extension
			if (fname.size() < 9 || fname.compare(fname.size() - 8, 8, ".desktop") != 0) continue;

			std::string full_path = dir + "/" + fname;
			auto info_opt = ParseDesktopFile(full_path);
			if (!info_opt) continue;

			CandidateInfo& info = *info_opt;
			if (info.mimetype.empty()) continue;

			std::string desk_basename = GetDesktopBasename(info.desktop_file);
			if (seen_desktops.count(desk_basename)) continue;  // Already processed

			// Check if this desktop file handles any of our MIME types
			auto mime_list = Split(info.mimetype, L';');
			int best_mime_idx = static_cast<int>(prioritized_mimes.size());
			bool matches = false;
			for (size_t j = 0; j < prioritized_mimes.size(); ++j) {
				for (const auto& dmime : mime_list) {
					if (StrWide2MB(dmime) == prioritized_mimes[j]) {
						best_mime_idx = std::min(best_mime_idx, static_cast<int>(j));
						matches = true;
						break;
					}
				}
				if (matches) break;
			}

			if (matches) {
				seen_desktops.insert(desk_basename);
				// Fallback ranking: MIME index * 1000 + 500 + directory priority * 10
				int fallback_rank = best_mime_idx * 1000 + 500 + dir_prio * 10;
				candidates.push_back({info, fallback_rank, false});
			}
		}
		closedir(d);
	}

	// Sort candidates by rank (lower = better)
	std::sort(candidates.begin(), candidates.end());

	// Remove duplicates based on application info
	auto last = std::unique(candidates.begin(), candidates.end(),
							[](const RankedCandidate& a, const RankedCandidate& b) {
								return a.info == b.info;
							});
	candidates.erase(last, candidates.end());

	// Convert to final result format
	std::vector<CandidateInfo> result;
	result.reserve(candidates.size());
	for (const auto& rc : candidates) {
		result.push_back(rc.info);
	}

	return result;
}


// Check if character is whitespace according to desktop entry specification
bool LinuxAppProvider::IsDesktopWhitespace(wchar_t c)
{
	return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f' || c == L'\v';
}


// Tokenize desktop entry Exec field respecting shell quoting rules
std::vector<Token> LinuxAppProvider::TokenizeDesktopExec(const std::wstring& str)
{
	std::vector<Token> tokens;
	std::wstring cur;
	bool in_double_quotes = false;
	bool in_single_quotes = false;
	bool cur_quoted = false;
	bool cur_single_quoted = false;
	bool prev_backslash = false;

	for (size_t i = 0; i < str.size(); ++i) {
		wchar_t c = str[i];

		// Handle escaped characters
		if (prev_backslash) {
			cur.push_back(L'\\');
			cur.push_back(c);
			prev_backslash = false;
			continue;
		}

		if (c == L'\\') {
			prev_backslash = true;
			continue;
		}

		// Handle quote state transitions
		if (c == L'"' && !in_single_quotes) {
			in_double_quotes = !in_double_quotes;
			cur_quoted = true;
			continue;
		}

		if (c == L'\'' && !in_double_quotes) {
			in_single_quotes = !in_single_quotes;
			cur_single_quoted = true;
			continue;
		}

		// Handle whitespace token separation
		if (!in_double_quotes && !in_single_quotes && IsDesktopWhitespace(c)) {
			if (!cur.empty() || cur_quoted || cur_single_quoted) {
				tokens.push_back({cur, cur_quoted, cur_single_quoted});
				cur.clear();
				cur_quoted = false;
				cur_single_quoted = false;
			}
			continue;
		}

		cur.push_back(c);
	}

	// Handle final backslash
	if (prev_backslash) {
		cur.push_back(L'\\');
	}

	// Add final token if any
	if (!cur.empty() || cur_quoted || cur_single_quoted) {
		// Check for unclosed quotes (error condition)
		if ((cur_quoted && in_double_quotes) || (cur_single_quoted && in_single_quotes)) {
			return {};  // Invalid tokenization
		}
		tokens.push_back({cur, cur_quoted, cur_single_quoted});
	}

	return tokens;
}


// Process escape sequences in tokenized desktop entry text
std::wstring LinuxAppProvider::UndoEscapes(const Token& token)
{
	std::wstring result;
	result.reserve(token.text.size());

	for (size_t i = 0; i < token.text.size(); ++i) {
		if (token.text[i] == L'\\' && i + 1 < token.text.size()) {
			wchar_t next = token.text[i + 1];
			if (next == L'"' || next == L'\'' || next == L'`' || next == L'$' || next == L'\\') {
				result.push_back(next);
			} else {
				result.push_back(L'\\');
				result.push_back(next);
			}
			++i;
		} else {
			result.push_back(token.text[i]);
		}
	}

	return result;
}


// Expand desktop entry field codes according to freedesktop.org specification
bool LinuxAppProvider::ExpandFieldCodes(const CandidateInfo& candidate,
										const std::wstring& pathname,
										const std::wstring& unescaped,
										std::vector<std::wstring>& out_args)
{
	std::wstring cur;
	for (size_t i = 0; i < unescaped.size(); ++i) {
		wchar_t c = unescaped[i];
		if (c == L'%') {
			if (i + 1 >= unescaped.size()) return false;
			wchar_t code = unescaped[i + 1];
			++i;
			switch (code) {
			case L'f': case L'F': case L'u': case L'U':
				cur += pathname;
				break;
			case L'c':
				cur += candidate.name;
				break;
			case L'%':
				cur.push_back(L'%');
				break;
			case L'n': case L'd': case L'D': case L't': case L'T': case L'v': case L'm':
			case L'k': case L'i':
				// These field codes are not supported but should not cause errors
				break;
			default:
				// Unknown field code - this is an error
				return false;
			}
		} else {
			cur.push_back(c);
		}
	}
	if (!cur.empty()) out_args.push_back(cur);
	return true;
}

// Escape argument for shell execution by wrapping in double quotes
// Escapes special characters that have meaning inside double quotes
std::wstring LinuxAppProvider::EscapeArg(const std::wstring& arg)
{
	std::wstring out;
	out.push_back(L'"');
	for (wchar_t c : arg) {
		// Escape characters that are special inside double quotes
		if (c == L'\\' || c == L'"' || c == L'$' || c == L'`') {
			out.push_back(L'\\');
			out.push_back(c);
		} else {
			out.push_back(c);
		}
	}
	out.push_back(L'"');
	return out;
}


// Get localized value from desktop entry using system locale settings
// Follows XDG specification for localized keys like Name[locale]
std::string LinuxAppProvider::GetLocalizedValue(const std::unordered_map<std::string, std::string>& values,
												const std::string& key)
{
	// Check environment variables in priority order
	const char* env_vars[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
	for (const auto* var : env_vars) {
		const char* value = getenv(var);
		if (value && *value && std::strlen(value) >= 2) {
			std::string locale(value);
			// Remove encoding part (everything after '.')
			size_t dot_pos = locale.find('.');
			if (dot_pos != std::string::npos) {
				locale = locale.substr(0, dot_pos);
			}
			if (!locale.empty()) {
				// Try full locale first (e.g., "en_US")
				auto it = values.find(key + "[" + locale + "]");
				if (it != values.end()) return it->second;
				// Try language part only (e.g., "en")
				size_t underscore_pos = locale.find('_');
				if (underscore_pos != std::string::npos) {
					std::string lang_only = locale.substr(0, underscore_pos);
					it = values.find(key + "[" + lang_only + "]");
					if (it != values.end()) return it->second;
				}
			}
		}
	}
	// Fall back to unlocalized key
	auto it = values.find(key);
	return (it != values.end()) ? it->second : "";
}


// Parse .desktop file according to freedesktop.org Desktop Entry Specification
// Extracts essential information needed for application launching
std::optional<CandidateInfo> LinuxAppProvider::ParseDesktopFile(const std::string& path)
{
	std::ifstream file(path);
	if (!file.is_open()) {
		return std::nullopt;
	}

	std::string line;
	bool in_main_section = false;
	CandidateInfo info;
	info.terminal = false;
	info.desktop_file = StrMB2Wide(path);

	// Store all key-value pairs for localization processing
	std::unordered_map<std::string, std::string> entries;
	std::string exec;
	bool hidden = false;
	bool is_application = false;

	// Parse desktop file line by line
	while (std::getline(file, line)) {
		if (!file.good()) {
			return std::nullopt;
		}
		line = Trim(line);
		if (line.empty() || line[0] == '#') continue;
		
		if (line == "[Desktop Entry]") {
			in_main_section = true;
			continue;
		}
		if (line[0] == '[') {
			in_main_section = false;
			continue;
		}
		if (in_main_section) {
			auto eq_pos = line.find('=');
			if (eq_pos == std::string::npos) continue;
			std::string key = Trim(line.substr(0, eq_pos));
			std::string value = Trim(line.substr(eq_pos + 1));
			entries[key] = value;

			if (key == "Exec") exec = value;
			else if (key == "Terminal" && value == "true") info.terminal = true;
			else if (key == "MimeType") info.mimetype = StrMB2Wide(value);
			else if (key == "Hidden" && value == "true") hidden = true;
			else if (key == "Type" && value == "Application") is_application = true;
		}
	}

	if (hidden) {
		return std::nullopt;
	}

	if (exec.empty() || !is_application) {
		return std::nullopt;
	}

	exec = Trim(exec);
	if (exec.empty()) {
		return std::nullopt;
	}

	// Tokenize and validate Exec field
	std::wstring wide_exec = StrMB2Wide(exec);
	std::vector<Token> tokens = TokenizeDesktopExec(wide_exec);
	if (tokens.empty()) {
		return std::nullopt;  // invalid Exec field
	}

	// Get localized name with fallbacks
	std::string name = GetLocalizedValue(entries, "Name");
	if (name.empty()) {
		name = GetLocalizedValue(entries, "GenericName");
	}

	info.name = StrMB2Wide(name);
	if (info.name.empty()) {
		// Use filename as last resort
		std::size_t slash_pos = path.find_last_of('/');
		info.name = StrMB2Wide(path.substr(slash_pos != std::string::npos ? slash_pos + 1 : 0));
	}
	info.exec = wide_exec;
	return info;
}


// Find .desktop file in XDG data directories
// Searches through user and system directories in priority order
std::optional<std::string> LinuxAppProvider::FindDesktopFileLocation(const std::string& desktopFile)
{
	if (desktopFile.empty()) return std::nullopt;

	for (const auto& dir : GetXDGDataDirs()) {
		std::string full_path = dir + "/" + desktopFile;
		struct stat buffer;
		if (stat(full_path.c_str(), &buffer) == 0 && S_ISREG(buffer.st_mode)) {
			return full_path;
		}
	}
	return std::nullopt;
}


// Construct command line for launching application with given file
// Handles field code expansion and shell escaping
std::wstring LinuxAppProvider::ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname)
{
	if (candidate.exec.empty()) return std::wstring();

	// Tokenize the Exec field
	std::vector<Token> tokens = TokenizeDesktopExec(candidate.exec);
	if (tokens.empty()) {
		return std::wstring();
	}

	std::vector<std::wstring> args;
	args.reserve(tokens.size());
	bool has_field_code = false;

	// Check if any tokens contain field codes
	for (const Token& t : tokens) {
		std::wstring unescaped = UndoEscapes(t);
		if (unescaped.find(L'%') != std::wstring::npos) {
			has_field_code = true;
			break;
		}
	}

	// Expand all tokens and field codes
	for (const Token& t : tokens) {
		std::wstring unescaped = UndoEscapes(t);
		std::vector<std::wstring> expanded;
		if (!ExpandFieldCodes(candidate, pathname, unescaped, expanded)) {
			return std::wstring();  // field code expansion failed
		}
		for (auto &a : expanded) args.push_back(std::move(a));
	}

	// If no field codes were found, append the file path as argument
	if (!has_field_code && !args.empty()) {
		args.push_back(pathname);
	}

	if (args.empty()) {
		return std::wstring();
	}

	// Build final command line with proper escaping
	std::wstring cmd;
	for (size_t i = 0; i < args.size(); ++i) {
		if (i) cmd.push_back(L' ');
		cmd += EscapeArg(args[i]);
	}
	return cmd;
}


// Get MIME type of file using both system tools: xdg-mime and file
std::wstring LinuxAppProvider::GetMimeType(const std::wstring& pathname)
{
	std::string narrow_path = StrWide2MB(pathname);
	std::string escaped_path = EscapePathForShell(narrow_path);

	std::string xdg_mime_result = RunCommandAndCaptureOutput("xdg-mime query filetype " + escaped_path + " 2>/dev/null");
	std::string file_result = RunCommandAndCaptureOutput("file -b --mime-type " + escaped_path + " 2>/dev/null");

	std::string result;

	if (xdg_mime_result.empty()) {
		result = file_result;
	} else if (file_result.empty()) {
		result = xdg_mime_result;
	} else if (xdg_mime_result == file_result) {
		result = xdg_mime_result;
	} else {
		result = xdg_mime_result + ";" + file_result;
	}

	return StrMB2Wide(result);
}


// Get paths to mimeapps.list files in priority order
// Follows XDG MIME Apps specification for file locations and precedence
std::vector<std::pair<std::string, int>> LinuxAppProvider::GetMimeAppsPaths()
{
	std::vector<std::pair<std::string, int>> paths;
	std::string home_str = GetMyHome();

	// 1. System-wide files (lowest priority)
	paths.emplace_back("/usr/share/applications/mimeapps.list", 5);
	paths.emplace_back("/usr/local/share/applications/mimeapps.list", 4);

	// 2. User-specific files (higher priority)
	if (!home_str.empty()) {
		std::string user_local = home_str + "/.local/share/applications/mimeapps.list";
		paths.emplace_back(user_local, 3);
	}

	// 3. System config directory
	paths.emplace_back("/etc/xdg/mimeapps.list", 2);

	// 4. Desktop environment specific (high priority)
	const char* desktop_env = getenv("XDG_CURRENT_DESKTOP");
	if (desktop_env && *desktop_env) {
		std::string de_name(desktop_env);
		// Convert to lowercase for consistency
		std::transform(de_name.begin(), de_name.end(), de_name.begin(),
					   [](unsigned char c) { return std::tolower(static_cast<unsigned char>(c)); });
		std::string de_file = "/etc/xdg/" + de_name + "-mimeapps.list";
		paths.emplace_back(de_file, 1);
	}

	// 5. User config file (highest priority)
	std::string user_config;
	const char* xdg_config = getenv("XDG_CONFIG_HOME");
	if (xdg_config && *xdg_config) {
		user_config = std::string(xdg_config) + "/mimeapps.list";
	} else if (!home_str.empty()) {
		user_config = home_str + "/.config/mimeapps.list";
	}
	paths.emplace_back(user_config, 0);

	return paths;
}


// Parse single mimeapps.list file according to XDG specification
std::optional<MimeAppsData> LinuxAppProvider::ParseMimeApps(const std::string& path, int prio)
{
	std::ifstream file(path);
	if (!file.is_open()) {
		return std::nullopt;
	}

	MimeAppsData data;
	std::string section;
	std::string line;

	while (std::getline(file, line)) {
		line = Trim(line);
		if (line.empty() || (line[0] == '#')) continue;

		// Detect section headers
		if (line.size() > 2 && line[0] == '[' && line.back() == ']') {
			section = Trim(line.substr(1, line.size() - 2));
			continue;
		}

		// Parse key=value pairs
		size_t eq_pos = line.find('=');
		if (eq_pos == std::string::npos) continue;

		std::string mime = Trim(line.substr(0, eq_pos));
		std::string value = Trim(line.substr(eq_pos + 1));

		// Process based on current section
		if (section == "Default Applications") {
			if (!value.empty()) {
				data.defaults[mime] = {value, prio};
			}
		} else if (section == "Added Associations") {
			auto desks = Split(value, ';');
			for (const auto& d : desks) {
				if (!d.empty()) {
					data.added[mime].push_back({d, prio});
				}
			}
		} else if (section == "Removed Associations") {
			auto desks = Split(value, ';');
			for (const auto& d : desks) {
				if (!d.empty()) {
					data.removed[mime].insert(d);
				}
			}
		}
	}

	return data;
}


// Merge multiple mimeapps.list files respecting priority order
// Higher priority files override settings from lower priority files
MimeAppsData LinuxAppProvider::MergeMimeApps(const std::vector<std::pair<std::string, int>>& paths)
{
	MimeAppsData merged;

	// Process files in the order provided (should be priority order)
	for (const auto& p : paths) {
		auto data_opt = ParseMimeApps(p.first, p.second);
		if (!data_opt) continue;
		const auto& data = *data_opt;

		// Merge default applications (higher priority overrides)
		for (const auto& d : data.defaults) {
			merged.defaults[d.first] = {d.second.first, p.second};
		}

		// Merge added associations (combine with priority tracking)
		for (const auto& a : data.added) {
			const std::string& mime = a.first;
			auto& mvec = merged.added[mime];
			for (const auto& assoc : a.second) {
				const std::string& desk = assoc.desktop;
				int new_prio = p.second;
				// Update existing entry or add new one
				auto it = std::find_if(mvec.begin(), mvec.end(),
									   [desk](const Association& as) { return as.desktop == desk; });
				if (it != mvec.end()) {
					it->prio = std::min(it->prio, new_prio);  // Keep best priority
				} else {
					mvec.push_back({desk, new_prio});
				}
			}
		}

		// Merge removed associations (union of all removed sets)
		for (const auto& r : data.removed) {
			const std::string& mime = r.first;
			auto& mset = merged.removed[mime];
			mset.insert(r.second.begin(), r.second.end());
		}
	}

	return merged;
}


// Split string by delimiter and return non-empty trimmed parts
std::vector<std::string> LinuxAppProvider::Split(const std::string& s, char delim)
{
	std::vector<std::string> res;
	if (s.empty()) return res;

	std::stringstream ss(s);
	std::string token;
	while (std::getline(ss, token, delim)) {
		token = Trim(token);
		if (!token.empty()) {
			res.push_back(std::move(token));
		}
	}
	return res;
}


// Split wide string by delimiter and return non-empty trimmed parts
std::vector<std::wstring> LinuxAppProvider::Split(const std::wstring& s, wchar_t delim)
{
	std::vector<std::wstring> res;
	if (s.empty()) return res;

	std::wstringstream ss(s);
	std::wstring token;
	while (std::getline(ss, token, delim)) {
		// Trim wide string manually
		token.erase(0, token.find_first_not_of(L" \t"));
		token.erase(token.find_last_not_of(L" \t") + 1);
		if (!token.empty()) {
			res.push_back(std::move(token));
		}
	}
	return res;
}

#endif
