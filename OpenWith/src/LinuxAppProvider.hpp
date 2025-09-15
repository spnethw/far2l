#pragma once

#include "AppProvider.hpp"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <array>
#include <list>
#include <farplug-wide.h>
#include <KeyFileHelper.h>
#include <utils.h>
#include <stdio.h>
#include <cwctype>
#include "common.hpp"

#ifdef __linux__
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/wait.h>

// For sorting found applications
struct RankedCandidate {
	CandidateInfo info;
	int rank; // match rank (the lower the better)
	bool is_default = false;

	// Operator for std::sort
	bool operator<(const RankedCandidate& other) const {
		if (is_default != other.is_default) return is_default; // true (default app) comes first
		if (rank != other.rank) return rank < other.rank; // sort by rank
		return info.name < other.info.name; // secondary sort by name
	}
};

// For deduplication by executable file
inline bool operator==(const CandidateInfo& a, const CandidateInfo& b)
{
	return a.exec == b.exec;
}


struct Token
{
	std::wstring text;
	bool quoted;
	bool single_quoted; // Added for single quotes support
};


class LinuxAppProvider : public AppProvider
{
public:
	std::wstring GetMimeType(const std::wstring& pathname) override;
	std::vector<CandidateInfo> GetAppCandidates(const std::wstring& pathname) override;
	std::wstring ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname) override;

private:
	std::string Trim(std::string str) const;
	std::string RunCommandAndCaptureOutput(const std::string& cmd) const;
	std::string GetDefaultApp(const std::string& mime_type) const;
	std::vector<std::string> GetXDGDataDirs() const;
	std::vector<std::string> GetUserDirs() const;
	std::vector<std::string> GetSystemDirs() const;
	static bool IsValidApplicationsDir(const std::string& path);

	std::optional<CandidateInfo> ParseDesktopFile(const std::string& path) const;
	std::optional<std::string> FindDesktopFileLocation(const std::string& desktopFile) const;
	std::string GetLocalizedValue(const std::unordered_map<std::string, std::string>& values, const std::string& key) const;

	std::string EscapePathForShell(const std::string& path) const;

	// Новые вспомогательные функции для улучшенного алгоритма
	std::vector<std::string> CollectAndPrioritizeMimeTypes(const std::wstring& pathname) const;
	std::optional<int> GetBestMimeMatchRank(const std::string& desktop_pathname, const std::vector<std::string>& prioritized_mimes) const;

	// Статические вспомогательные функции для парсинга Exec
	static bool IsDesktopWhitespace(wchar_t c);
	static std::vector<Token> TokenizeDesktopExec(const std::wstring& str);
	static std::wstring UndoEscapes(const Token& token);
	static bool ExpandFieldCodes(const CandidateInfo& candidate, const std::wstring& pathname, const std::wstring& unescaped, std::vector<std::wstring>& out_args);
	static std::wstring EscapeArg(const std::wstring& arg);
};

#endif
