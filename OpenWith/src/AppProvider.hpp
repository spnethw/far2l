#pragma once

#include <string>
#include <vector>
#include <memory>

#include "common.hpp"

class AppProvider
{
public:
	virtual ~AppProvider() = default;
	virtual std::vector<CandidateInfo> GetAppCandidates(const std::wstring& pathname) = 0;
	virtual std::wstring ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname) = 0;
	static std::unique_ptr<AppProvider> CreateAppProvider();
};
