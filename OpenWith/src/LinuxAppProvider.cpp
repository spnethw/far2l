#if defined (__linux__)

#include "AppProvider.hpp"
#include "LinuxAppProvider.hpp"

std::string LinuxAppProvider::EscapePathForShell(const std::string& path) const
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


std::string LinuxAppProvider::Trim(std::string str) const
{
	str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) { return !std::isspace(ch); }));
	str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), str.end());
	return str;
}


std::string LinuxAppProvider::RunCommandAndCaptureOutput(const std::string& cmd) const
{
	std::string result;
	return POpen(result, cmd.c_str()) ? Trim(result) : "";
}


std::string LinuxAppProvider::GetDefaultApp(const std::string& mime_type) const
{
	std::string escaped_mime = EscapePathForShell(mime_type);
	std::string cmd = "xdg-mime query default " + escaped_mime + " 2>/dev/null";
	return RunCommandAndCaptureOutput(cmd);
}


bool LinuxAppProvider::IsValidApplicationsDir(const std::string& path)
{
	struct stat buffer;
	if (stat(path.c_str(), &buffer) != 0) return false;
	return S_ISDIR(buffer.st_mode);
}


std::vector<std::string> LinuxAppProvider::GetUserDirs() const
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


std::vector<std::string> LinuxAppProvider::GetSystemDirs() const
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


std::vector<std::string> LinuxAppProvider::GetXDGDataDirs() const
{
	auto dirs = GetUserDirs();
	auto system_dirs = GetSystemDirs();
	dirs.insert(dirs.end(), system_dirs.begin(), system_dirs.end());
	return dirs;
}


std::vector<std::string> LinuxAppProvider::CollectAndPrioritizeMimeTypes(const std::wstring& pathname) const
{
	std::vector<std::string> mime_types;
	std::unordered_set<std::string> seen;

	auto add_unique = [&](std::string mime) {
		if (!mime.empty() && mime.find('/') != std::string::npos && seen.insert(mime).second) {
			mime_types.push_back(std::move(mime));
		}
	};

	std::string narrow_path = StrWide2MB(pathname);
	std::string escaped_path = EscapePathForShell(narrow_path);

	// 1. Priority №1: xdg-mime
	add_unique(RunCommandAndCaptureOutput("xdg-mime query filetype " + escaped_path + " 2>/dev/null"));

	// 2. Priority №2: file
	add_unique(RunCommandAndCaptureOutput("file -b --mime-type " + escaped_path + " 2>/dev/null"));

	// 3. Generalize MIME types by stripping "+suffix"
	std::vector<std::string> base_types = mime_types;
	for (const auto& mime : base_types) {
		size_t plus_pos = mime.find('+');
		if (plus_pos != std::string::npos) {
			add_unique(mime.substr(0, plus_pos));
		}
	}

	// 4. Fallback: map filename extension to a probable MIME type and add it if missing
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

	// 5. Fallbacks: add top-level wildcards and ensure "text/plain" for any "text/".
	for (const auto& mime : base_types) {
		size_t slash_pos = mime.find('/');
		if (slash_pos != std::string::npos) {
			add_unique(mime.substr(0, slash_pos) + "/*");
		}
		if (mime.rfind("text/", 0) == 0) {
			add_unique("text/plain");
		}
	}

	add_unique("application/x-executable");
	add_unique("application/octet-stream");

	return mime_types;
}


std::optional<int> LinuxAppProvider::GetBestMimeMatchRank(const std::string& desktop_pathname, const std::vector<std::string>& prioritized_mimes) const
{
	std::ifstream file(desktop_pathname);
	if (!file) return std::nullopt;

	std::string line;
	bool in_main_section = false;
	while (std::getline(file, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == '#') continue;

		// Enter main Desktop Entry section; only keys inside this section are relevant
		if (line == "[Desktop Entry]") {
			in_main_section = true;
			continue;
		}

		// Any other section ends the main section scope
		if (line[0] == '[') {
			in_main_section = false;
			continue;
		}

		// Only process MimeType key inside the main section
		if (in_main_section && line.rfind("MimeType=", 0) == 0) {
			size_t eq_pos = line.find('=');
			if (eq_pos == std::string::npos) continue;

			// Extract and split semicolon-separated MIME list
			std::string value = Trim(line.substr(eq_pos + 1));
			std::stringstream ss(value);
			std::string app_mime;

			int best_rank = -1;

			while (std::getline(ss, app_mime, ';')) {
				app_mime = Trim(app_mime);
				if (app_mime.empty()) continue;

				for (size_t i = 0; i < prioritized_mimes.size(); ++i) {
					const auto& target_mime = prioritized_mimes[i];

					bool match = false;

					// Support wildcard entries like "image/*": match top-level type prefix
					size_t star_pos = app_mime.find("/*");
					if (star_pos != std::string::npos && star_pos == app_mime.length() - 2) {
						if (target_mime.rfind(app_mime.substr(0, star_pos + 1), 0) == 0) {
							match = true;
						}
					} else { // Exact match
						if (app_mime == target_mime) {
							match = true;
						}
					}

					// Keep the best (lowest index) match among prioritized_mimes
					if (match) {
						if (best_rank == -1 || static_cast<int>(i) < best_rank) {
							best_rank = static_cast<int>(i);
						}
					}
				}
			}

			if (best_rank != -1) {
				// Return the best (smallest) rank found for this desktop file
				return best_rank;
			}
		}
	}
	return std::nullopt;
}


std::vector<CandidateInfo> LinuxAppProvider::GetAppCandidates(const std::wstring& pathname)
{
	std::vector<std::string> prioritized_mimes = CollectAndPrioritizeMimeTypes(pathname);
	if (prioritized_mimes.empty()) return {};

	std::vector<RankedCandidate> ranked_candidates;
	std::unordered_set<std::wstring> seen_execs; // for deduplication by Exec field

	// Single pass through all .desktop files
	for (const auto& dir_path : GetXDGDataDirs()) {
		DIR* dir = opendir(dir_path.c_str());
		if (!dir) continue;

		struct dirent* entry;
		while ((entry = readdir(dir)) != nullptr) {
			std::string filename = entry->d_name;
			if (filename.length() > 8 && filename.substr(filename.length() - 8) == ".desktop") {
				std::string full_path = dir_path + "/" + filename;

				if (auto rank = GetBestMimeMatchRank(full_path, prioritized_mimes)) {
					if (auto candidate = ParseDesktopFile(full_path)) {
						if (seen_execs.insert(candidate->exec).second) {
							ranked_candidates.push_back({*candidate, *rank, false});
						}
					}
				}
			}
		}
		closedir(dir);
	}

	// Determine the default application
	if (!prioritized_mimes.empty()) {
		std::string default_app_desktop = GetDefaultApp(prioritized_mimes.front());
		if (!default_app_desktop.empty()) {
			for (auto& cand : ranked_candidates) {
				// The desktop_file name may be a full pathname, so use find
				if (StrWide2MB(cand.info.desktop_file).find(default_app_desktop) != std::string::npos) {
					cand.is_default = true;
					break;
				}
			}
		}
	}

	// Final sorting by all criteria
	std::sort(ranked_candidates.begin(), ranked_candidates.end());

	// Convert the result to the final format
	std::vector<CandidateInfo> result;
	result.reserve(ranked_candidates.size());
	for (const auto& ranked : ranked_candidates) {
		result.push_back(ranked.info);
	}

	return result;
}


bool LinuxAppProvider::IsDesktopWhitespace(wchar_t c)
{
	return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f' || c == L'\v';
}


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

	if (prev_backslash) {
		cur.push_back(L'\\');
	}

	if (!cur.empty() || cur_quoted || cur_single_quoted) {
		if ((cur_quoted && in_double_quotes) || (cur_single_quoted && in_single_quotes)) {
			return {};
		}
		tokens.push_back({cur, cur_quoted, cur_single_quoted});
	}

	return tokens;
}


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
				// These codes are not supported, but should not cause an error
				break;
			default:
				return false;
			}
		} else {
			cur.push_back(c);
		}
	}
	if (!cur.empty()) out_args.push_back(cur);
	return true;
}


std::wstring LinuxAppProvider::EscapeArg(const std::wstring& arg)
{
	std::wstring out;
	out.push_back(L'"');
	for (wchar_t c : arg) {
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


std::string LinuxAppProvider::GetLocalizedValue(const std::unordered_map<std::string, std::string>& values,
												const std::string& key) const
{
	const char* env_vars[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
	for (const auto* var : env_vars) {
		const char* value = getenv(var);
		if (value && *value && std::strlen(value) >= 2) {
			std::string locale(value);
			size_t dot_pos = locale.find('.');
			if (dot_pos != std::string::npos) {
				locale = locale.substr(0, dot_pos);
			}
			if (!locale.empty()) {
				auto it = values.find(key + "[" + locale + "]");
				if (it != values.end()) return it->second;
				size_t underscore_pos = locale.find('_');
				if (underscore_pos != std::string::npos) {
					std::string lang_only = locale.substr(0, underscore_pos);
					it = values.find(key + "[" + lang_only + "]");
					if (it != values.end()) return it->second;
				}
			}
		}
	}
	auto it = values.find(key);
	return (it != values.end()) ? it->second : "";
}


std::optional<CandidateInfo> LinuxAppProvider::ParseDesktopFile(const std::string& path) const
{
	std::ifstream file(path);
	if (!file.is_open()) {
		fprintf(stderr, "OpenWith: Cannot open .desktop file\n");
		return std::nullopt;
	}

	std::string line;
	bool in_main_section = false;
	CandidateInfo info;
	info.terminal = false;
	info.desktop_file = StrMB2Wide(path);

	std::unordered_map<std::string, std::string> entries;
	std::string exec;
	bool hidden = false;
	bool is_application = false;

	while (std::getline(file, line)) {
		if (!file.good()) {
			fprintf(stderr, "OpenWith: Error reading .desktop file\n");
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

	std::wstring wide_exec = StrMB2Wide(exec);
	std::vector<Token> tokens = TokenizeDesktopExec(wide_exec);
	if (tokens.empty()) {
		return std::nullopt;
	}

	std::string name = GetLocalizedValue(entries, "Name");
	if (name.empty()) {
		name = GetLocalizedValue(entries, "GenericName");
	}

	info.name = StrMB2Wide(name);
	if (info.name.empty()) {
		std::size_t slash_pos = path.find_last_of('/');
		info.name = StrMB2Wide(path.substr(slash_pos != std::string::npos ? slash_pos + 1 : 0));
	}
	info.exec = wide_exec;
	return info;
}


std::optional<std::string> LinuxAppProvider::FindDesktopFileLocation(const std::string& desktopFile) const
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


std::wstring LinuxAppProvider::ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname)
{
	if (candidate.exec.empty()) return std::wstring();

	std::vector<Token> tokens = TokenizeDesktopExec(candidate.exec);
	if (tokens.empty()) {
		fprintf(stderr, "OpenWith: Invalid Exec line in .desktop file\n");
		return std::wstring();
	}

	std::vector<std::wstring> args;
	args.reserve(tokens.size());
	bool has_field_code = false;

	for (const Token& t : tokens) {
		std::wstring unescaped = UndoEscapes(t);
		if (unescaped.find(L'%') != std::wstring::npos) {
			has_field_code = true;
			break;
		}
	}

	for (const Token& t : tokens) {
		std::wstring unescaped = UndoEscapes(t);
		std::vector<std::wstring> expanded;
		if (!ExpandFieldCodes(candidate, pathname, unescaped, expanded)) {
			fprintf(stderr, "OpenWith: Invalid field codes in Exec line\n");
			return std::wstring();
		}
		for (auto &a : expanded) args.push_back(std::move(a));
	}

	if (!has_field_code && !args.empty()) {
		args.push_back(pathname);
	}

	if (args.empty()) {
		fprintf(stderr, "OpenWith: No valid arguments in Exec line\n");
		return std::wstring();
	}

	std::wstring cmd;
	for (size_t i = 0; i < args.size(); ++i) {
		if (i) cmd.push_back(L' ');
		cmd += EscapeArg(args[i]);
	}
	return cmd;
}


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

#endif
