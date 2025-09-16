#pragma once

#include "AppProvider.hpp"
#include "common.hpp"
#include <string>
#include <vector>

class DummyAppProvider : public AppProvider
{
public:
	std::wstring GetMimeType(const std::wstring& pathname) override;
	std::vector<CandidateInfo> GetAppCandidates(const std::wstring& pathname) override;
	std::wstring ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname) override;
};
