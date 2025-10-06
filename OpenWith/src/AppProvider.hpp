#pragma once

#include "common.hpp"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <utility>


struct ProviderSetting
{
	std::wstring internal_key;
	std::wstring display_name;
	bool value;
};


class AppProvider
{
public:
	using TMsgGetter = std::function<const wchar_t*(int)>;

	explicit AppProvider(TMsgGetter msg_getter) : m_GetMsg(std::move(msg_getter)) {}
	virtual ~AppProvider() = default;

	// MODIFIED: Now accepts a vector of pathnames to find common applications.
	virtual std::vector<CandidateInfo> GetAppCandidates(const std::vector<std::wstring>& pathnames) = 0;

	// MODIFIED: Accepts multiple pathnames and returns a vector of unique MIME types for them.
	virtual std::vector<std::wstring> GetMimeTypes(const std::vector<std::wstring>& pathnames) = 0;

	// MODIFIED: Now accepts multiple pathnames and returns one or more command lines.
	virtual std::vector<std::wstring> ConstructCommandLine(const CandidateInfo& candidate, const std::vector<std::wstring>& pathnames) = 0;

	virtual std::vector<Field> GetCandidateDetails(const CandidateInfo& candidate) = 0;

	static std::unique_ptr<AppProvider> CreateAppProvider(TMsgGetter msg_getter);

	virtual std::vector<ProviderSetting> GetPlatformSettings() { return {}; }

	virtual void SetPlatformSettings(const std::vector<ProviderSetting>& settings) { }
	virtual void LoadPlatformSettings() { }
	virtual void SavePlatformSettings() { }

protected:
	TMsgGetter m_GetMsg;
};
