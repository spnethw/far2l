#pragma once

#if defined (__linux__)

#include "AppProvider.hpp"
#include "common.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <unordered_set>


// Used to sort applications by preference based on MIME type specificity and source priority
struct RankedCandidate {
	CandidateInfo info;  // info from .desktop file
	int rank;            // match rank (lower values - better matches)
	bool is_default = false;

	// Comparator for std::sort - prioritizes default apps, then by rank, then alphabetically
	bool operator<(const RankedCandidate& other) const {
		if (is_default != other.is_default) return is_default;
		if (rank != other.rank) return rank < other.rank;
		return info.name < other.info.name;
	}
};


// Compares by executable path to avoid duplicate entries for the same application
inline bool operator==(const CandidateInfo& a, const CandidateInfo& b)
{
	return a.exec == b.exec;
}

// Represents MIME type association from mimeapps.list
struct Association
{
	std::string desktop;
	int prio;            // priority level (lower values - higher priority)
};

// Aggregated data from all mimeapps.list files across the system
struct MimeAppsData
{
	// [Default Applications] section: maps MIME type to default handler
	std::unordered_map<std::string, std::pair<std::string, int>> defaults;
	
	// [Added Associations] section: additional applications that can handle the MIME type
	std::unordered_map<std::string, std::vector<Association>> added;
	
	// [Removed Associations] section: applications explicitly removed from handling this MIME type
	std::unordered_map<std::string, std::unordered_set<std::string>> removed;
};


// Represents tokenized component of desktop entry Exec field
struct Token
{
	std::wstring text;
	bool quoted;
	bool single_quoted;
};


class LinuxAppProvider : public AppProvider
{
public:
	std::wstring GetMimeType(const std::wstring& pathname) override;
	std::vector<CandidateInfo> GetAppCandidates(const std::wstring& pathname) override;
	std::wstring ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname) override;

private:
	static std::string GetDefaultApp(const std::string& mime_type);
	static std::vector<std::string> GetXDGDataDirs();
	static std::vector<std::string> GetUserDirs();
	static std::vector<std::string> GetSystemDirs();
	static bool IsValidApplicationsDir(const std::string& path);
	static std::optional<CandidateInfo> ParseDesktopFile(const std::string& path);
	static std::optional<std::string> FindDesktopFileLocation(const std::string& desktopFile);
	static std::string GetLocalizedValue(const std::unordered_map<std::string, std::string>& values, const std::string& key);
	static std::vector<std::string> CollectAndPrioritizeMimeTypes(const std::wstring& pathname);
	static std::string RunCommandAndCaptureOutput(const std::string& cmd);
	static std::string Trim(std::string str);
	static bool IsDesktopWhitespace(wchar_t c);
	static std::vector<Token> TokenizeDesktopExec(const std::wstring& str);
	static std::wstring UndoEscapes(const Token& token);
	static std::string EscapePathForShell(const std::string& path);
	static bool ExpandFieldCodes(const CandidateInfo& candidate, const std::wstring& pathname, const std::wstring& unescaped, std::vector<std::wstring>& out_args);
	static std::wstring EscapeArg(const std::wstring& arg);
	static std::vector<std::pair<std::string, int>> GetMimeAppsPaths();
	static std::optional<MimeAppsData> ParseMimeApps(const std::string& path, int prio);
	static MimeAppsData MergeMimeApps(const std::vector<std::pair<std::string, int>>& paths);
	static std::vector<std::string> Split(const std::string& s, char delim);
	static std::vector<std::wstring> Split(const std::wstring& s, wchar_t delim);
	static std::string GetDesktopBasename(const std::wstring& full_path);
};

#endif
