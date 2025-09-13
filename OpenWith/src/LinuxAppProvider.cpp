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
    result.reserve(1024);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        fprintf(stderr, "OpenWith: Failed to execute command\n");
        return "";
    }
    struct PipeGuard {
        FILE* p;
        ~PipeGuard() { if (p) pclose(p); }
    } guard{pipe};
    std::array<char, 256> buffer;
    size_t total_size = 0;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
        total_size += strlen(buffer.data());
        if (total_size > 1024 * 1024) { // Limit output to 1MB
            fprintf(stderr, "OpenWith: Command output too large\n");
            return "";
        }
    }
    int status = pclose(pipe);
    guard.p = nullptr; // Prevent double close
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "OpenWith: Command execution failed\n");
        return "";
    }
    return Trim(result);
}


std::string LinuxAppProvider::GetMimeType(const std::wstring& pathname) const
{
    std::string narrow_path = StrWide2MB(pathname);
    std::string escaped_path = EscapePathForShell(narrow_path);

    std::string cmd = "xdg-mime query filetype " + escaped_path + " 2>/dev/null";
	std::string mime = RunCommandAndCaptureOutput(cmd);
    if (mime.empty() || mime.find('/') == std::string::npos) {
        fprintf(stderr, "OpenWith: Failed to determine MIME type (xdg-mime may be missing)\n");
        return "";
    }
    return mime;
}


std::string LinuxAppProvider::GetDefaultApp(const std::string& mime_type) const
{
	std::string escaped_mime = EscapePathForShell(mime_type);
    std::string cmd = "xdg-mime query default " + escaped_mime + " 2>/dev/null";
	return RunCommandAndCaptureOutput(cmd);
}

// GetXDGDataDirs with directory validation
std::vector<std::string> LinuxAppProvider::GetXDGDataDirs() const
{
    std::vector<std::string> dirs;
    const char* xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home && *xdg_data_home) {
        std::string path = std::string(xdg_data_home) + "/applications";
        struct stat buffer;
        if (stat(path.c_str(), &buffer) == 0 && S_ISDIR(buffer.st_mode)) {
            dirs.push_back(path);
        }
    } else {
        auto home = GetMyHome();
        if (!home.empty()) {
            std::string path = home + "/.local/share/applications";
            struct stat buffer;
            if (stat(path.c_str(), &buffer) == 0 && S_ISDIR(buffer.st_mode)) {
                dirs.push_back(path);
            }
        }
    }

    const char* xdg_data_dirs = getenv("XDG_DATA_DIRS");
    if (xdg_data_dirs && *xdg_data_dirs) {
        std::stringstream ss(xdg_data_dirs);
        std::string dir;
        size_t count = 0;
        while (std::getline(ss, dir, ':') && count < 50) { // Limit to 50 directories
            if (!dir.empty()) {
                std::string path = dir + "/applications";
                struct stat buffer;
                if (stat(path.c_str(), &buffer) == 0 && S_ISDIR(buffer.st_mode)) {
                    dirs.push_back(path);
                }
            }
            ++count;
        }
    } else {
        std::string paths[] = {"/usr/local/share/applications", "/usr/share/applications"};
        for (const auto& path : paths) {
            struct stat buffer;
            if (stat(path.c_str(), &buffer) == 0 && S_ISDIR(buffer.st_mode)) {
                dirs.push_back(path);
            }
        }
    }
    return dirs;
}

bool LinuxAppProvider::IsDesktopWhitespace(wchar_t c)
{
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f' || c == L'\v';
}


// TokenizeDesktopExec with full whitespace handling, now supporting single quotes
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
            // Unclosed quotes detected
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
            // POSIX-like escaping: escape " ` $ \ , but also handle others if needed
            if (next == L'"' || next == L'\'' || next == L'`' || next == L'$' || next == L'\\') {
                result.push_back(next);
            } else {
                result.push_back(L'\\'); // Preserve unknown escapes
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
                break;
            case L'k': case L'i':
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


// GetLocalizedValue with locale validation
std::string LinuxAppProvider::GetLocalizedValue(const std::unordered_map<std::string, std::string>& values,
                                                const std::string& key) const
{
    const char* env_vars[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    for (const auto* var : env_vars) {
        const char* value = getenv(var);
        if (value && *value && std::strlen(value) >= 2) { // At least language code (e.g., "en")
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


// ParseDesktopFile with error handling and exec validation
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
            else if (key == "Hidden" && value == "true") hidden = true;
            else if (key == "Type" && value == "Application") is_application = true;
        }
    }

    if (hidden) {
        // Skip Hidden=true entries as per spec
        return std::nullopt;
    }

    if (exec.empty() || !is_application) {
        fprintf(stderr, "OpenWith: Invalid .desktop file: missing Exec or Type=Application\n");
        return std::nullopt;
    }

    // Validate Exec: trim and check non-empty, basic tokenize
    exec = Trim(exec);
    if (exec.empty()) {
        fprintf(stderr, "OpenWith: Empty Exec after trim\n");
        return std::nullopt;
    }

    std::wstring wide_exec = StrMB2Wide(exec);
	std::vector<Token> tokens = TokenizeDesktopExec(wide_exec);
    if (tokens.empty()) {
        fprintf(stderr, "OpenWith: Invalid or empty tokens in Exec\n");
        return std::nullopt;
    }

    std::string name = GetLocalizedValue(entries, "Name");
    if (name.empty()) {
        std::string generic_name = GetLocalizedValue(entries, "GenericName");
        name = generic_name;
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


std::vector<std::string> LinuxAppProvider::GetMimeAppsAssociations(const std::string& mime_type) const
{
    std::vector<std::string> apps;
    std::unordered_set<std::string> seen_apps;

    std::vector<std::string> mimeapps_paths;
    const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
    if (xdg_config_home && *xdg_config_home) {
        mimeapps_paths.push_back(std::string(xdg_config_home) + "/mimeapps.list");
    } else {
        auto home = GetMyHome();
        if (!home.empty()) {
            mimeapps_paths.push_back(home + "/.config/mimeapps.list");
        }
    }

    mimeapps_paths.push_back("/etc/xdg/mimeapps.list");
    for (const auto& dir : GetXDGDataDirs()) {
        mimeapps_paths.push_back(dir + "/../mimeapps.list");
        mimeapps_paths.push_back(dir + "/../gnome-mimeapps.list");
    }

    for (const auto& path : mimeapps_paths) {
        std::ifstream file(path);
        if (!file) continue;

        std::string line;
        bool in_default_section = false;
        bool in_added_section = false;

        while (std::getline(file, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == '#') continue;

            if (line == "[Default Applications]") {
                in_default_section = true;
                in_added_section = false;
                continue;
            }
            if (line == "[Added Associations]") {
                in_added_section = true;
                in_default_section = false;
                continue;
            }
            if (line[0] == '[') {
                in_default_section = false;
                in_added_section = false;
                continue;
            }

            if (in_default_section || in_added_section) {
                auto eq_pos = line.find('=');
                if (eq_pos == std::string::npos) continue;

                std::string key = Trim(line.substr(0, eq_pos));
				if (key != mime_type) continue;

                std::string value = Trim(line.substr(eq_pos + 1));
                std::stringstream ss(value);
                std::string desktop_file;
                while (std::getline(ss, desktop_file, ';')) {
                    desktop_file = Trim(desktop_file);
                    if (!desktop_file.empty() && seen_apps.insert(desktop_file).second) {
                        apps.push_back(desktop_file);
                    }
                }
            }
        }
    }

    return apps;
}

bool LinuxAppProvider::CheckDesktopFileMimeMatch(const std::string& desktop_pathname, const std::string& mime_type) const
{
    std::ifstream file(desktop_pathname);
    if (!file) return false;

    std::string line;
    bool in_main_section = false;
    while (std::getline(file, line)) {
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
        if (in_main_section && line.rfind("MimeType=", 0) == 0) {
            size_t eq_pos = line.find('=');
            if (eq_pos != std::string::npos) {
                std::string value = Trim(line.substr(eq_pos + 1));
                std::stringstream ss(value);
                std::string token;
                while (std::getline(ss, token, ';')) {
                    token = Trim(token);
                    if (token.empty()) continue;

                    bool is_wildcard = false;
                    std::string base_type = token;
                    size_t slash_pos = token.find('/');
                    if (slash_pos != std::string::npos && token.substr(slash_pos + 1) == "*") {
                        is_wildcard = true;
                        base_type = token.substr(0, slash_pos);
                    }

					if (!is_wildcard && token == mime_type) {
                        return true;
                    }
                    if (is_wildcard) {
						size_t mime_slash = mime_type.find('/');
						if (mime_slash != std::string::npos && mime_type.substr(0, mime_slash) == base_type) {
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

void LinuxAppProvider::ScanAppsForMime(const std::string& mime_type, std::vector<CandidateInfo>& candidates) const
{
    std::unordered_set<std::string> processed_files;

    // First, add apps from mimeapps.list
	auto mime_apps = GetMimeAppsAssociations(mime_type);
    for (const auto& desktop_file : mime_apps) {
        if (auto location = FindDesktopFileLocation(desktop_file)) {
            if (processed_files.insert(*location).second) {
                if (auto candidate = ParseDesktopFile(*location)) {
                    candidates.push_back(*candidate);
                }
            }
        }
    }

    // Check .desktop files for MIME type
    for (const auto& dir_path : GetXDGDataDirs()) {
        DIR* dir = opendir(dir_path.c_str());
        if (!dir) continue;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename = entry->d_name;
            if (filename.length() > 8 && filename.substr(filename.length() - 8) == ".desktop") {
                std::string full_path = dir_path + "/" + filename;
                if (processed_files.count(full_path)) continue;
				if (CheckDesktopFileMimeMatch(full_path, mime_type)) {
                    processed_files.insert(full_path);
                    if (auto candidate = ParseDesktopFile(full_path)) {
                        candidates.push_back(*candidate);
                    }
                }
            }
        }
        closedir(dir);
    }
}

std::string LinuxAppProvider::IdentifyFileMimeType(const std::wstring& pathname) const
{
	std::string mime_type = GetMimeType(pathname);

	if (!mime_type.empty()) {
		return mime_type;
    }

    // Fallback 1: use `file` command
    std::string narrow_path = StrWide2MB(pathname);
    std::string escaped_path = EscapePathForShell(narrow_path);

    std::string fallback_cmd = "file -b --mime-type " + escaped_path + " 2>/dev/null";
	std::string fallback_mime = RunCommandAndCaptureOutput(fallback_cmd);
    if (!fallback_mime.empty() && fallback_mime.find('/') != std::string::npos) {
        return fallback_mime;
    }

    // Fallback 2: by extension
    size_t dot_pos = pathname.rfind(L'.');
    if (dot_pos != std::wstring::npos) {
        std::wstring ext = pathname.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".sh" || ext == L".bash" || ext == L".csh") return "text/x-shellscript";
        if (ext == L".py") return "text/x-python";
        if (ext == L".pl") return "text/x-perl";
        if (ext == L".rb") return "text/x-ruby";
        if (ext == L".js") return "text/javascript";
        if (ext == L".exe") return "application/x-ms-dos-executable";
        if (ext == L".bin" || ext == L".elf") return "application/x-executable";
        if (ext == L".txt" || ext == L".conf" || ext == L".cfg") return "text/plain";
        if (ext == L".md") return "text/markdown";
        if (ext == L".jpg" || ext == L".jpeg") return "image/jpeg";
        if (ext == L".png") return "image/png";
        if (ext == L".gif") return "image/gif";
        if (ext == L".pdf") return "application/pdf";
        if (ext == L".doc") return "application/msword";
        if (ext == L".odt") return "application/vnd.oasis.opendocument.text";
        if (ext == L".zip") return "application/zip";
        if (ext == L".tar") return "application/x-tar";
        if (ext == L".gz") return "application/gzip";
    }

    return "application/octet-stream";
}

std::vector<CandidateInfo> LinuxAppProvider::DeduplicateAndSortCandidates(std::vector<CandidateInfo>& candidates, const std::string& mime_type) const
{
    std::unordered_set<CandidateInfo, CandidateInfoHasher> unique_candidates(candidates.begin(), candidates.end());
    std::vector<CandidateInfo> result(unique_candidates.begin(), unique_candidates.end());

    std::optional<CandidateInfo> default_candidate;
	std::string default_desktop_file = GetDefaultApp(mime_type);
    if (!default_desktop_file.empty()) {
        if (auto location = FindDesktopFileLocation(default_desktop_file)) {
            default_candidate = ParseDesktopFile(*location);
        }
    }

    std::sort(result.begin(), result.end(), [&](const auto& a, const auto& b) {
        if (default_candidate && a.exec == default_candidate->exec) return true;
        if (default_candidate && b.exec == default_candidate->exec) return false;
        return a.name < b.name;
    });

    return result;
}

std::vector<CandidateInfo> LinuxAppProvider::GetAppCandidates(const std::wstring& pathname)
{
	std::string mime_type = IdentifyFileMimeType(pathname);
    std::vector<CandidateInfo> all_candidates;

	ScanAppsForMime(mime_type, all_candidates);

    // Fallback 1: for text files we will look for editors
	if (all_candidates.empty() && mime_type.rfind("text/", 0) == 0 && mime_type != "text/plain") {
        ScanAppsForMime("text/plain", all_candidates);
    }

    // Fallback 2: ищем приложения для базового типа (image/* для image/jpeg)
    if (all_candidates.empty()) {
		size_t slash_pos = mime_type.find('/');
        if (slash_pos != std::string::npos) {
			std::string base_type = mime_type.substr(0, slash_pos) + "/*";
            ScanAppsForMime(base_type, all_candidates);
        }
    }

    // Fallback 3: ищем приложения для исполняемых файлов
    if (all_candidates.empty()) {
        ScanAppsForMime("application/x-executable", all_candidates);
    }

    // Fallback 4: ищем приложения для бинарных данных (самый общий случай)
    if (all_candidates.empty()) {
        ScanAppsForMime("application/octet-stream", all_candidates);
    }

	return DeduplicateAndSortCandidates(all_candidates, mime_type);
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

#endif
