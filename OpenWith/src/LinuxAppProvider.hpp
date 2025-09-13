
#pragma once

#include "AppProvider.hpp"
#include "LinuxAppProvider.hpp"
#include "LinuxAppProvider.hpp"
#include "AppProvider.hpp"
#include "MacOSAppProvider.hpp"
#include "LinuxAppProvider.hpp"
#include "DummyAppProvider.hpp"
#include "BSDAppProvider.hpp"

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <algorithm>
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

class LinuxAppProvider : public AppProvider
{
public:
    std::vector<CandidateInfo> GetAppCandidates(const std::wstring& pathname) override;
    std::wstring ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname) override;

private:
    std::string Trim(std::string str) const;
	std::string RunCommandAndCaptureOutput(const std::string& cmd) const;
    std::string GetMimeType(const std::wstring& pathname) const;
	std::string GetDefaultApp(const std::string& mime_type) const;
    std::vector<std::string> GetXDGDataDirs() const;
    std::optional<CandidateInfo> ParseDesktopFile(const std::string& path) const;
    std::optional<std::string> FindDesktopFileLocation(const std::string& desktopFile) const;
	std::vector<std::string> GetMimeAppsAssociations(const std::string& mime_type) const;
	void ScanAppsForMime(const std::string& mime_type, std::vector<CandidateInfo>& candidates) const;
    std::string GetLocalizedValue(const std::unordered_map<std::string, std::string>& values, const std::string& key) const;

    std::string EscapePathForShell(const std::string& path) const;

	std::string IdentifyFileMimeType(const std::wstring& pathname) const;
	bool CheckDesktopFileMimeMatch(const std::string& desktop_pathname, const std::string& mime_type) const;
	std::vector<CandidateInfo> DeduplicateAndSortCandidates(std::vector<CandidateInfo>& candidates, const std::string& mime_type) const;

    // Статические вспомогательные функции
    static bool IsDesktopWhitespace(wchar_t c);
	static std::vector<Token> TokenizeDesktopExec(const std::wstring& str);
    static std::wstring UndoEscapes(const Token& token);
    static bool ExpandFieldCodes(const CandidateInfo& candidate, const std::wstring& pathname, const std::wstring& unescaped, std::vector<std::wstring>& out_args);
    static std::wstring EscapeArg(const std::wstring& arg);
};

#endif
