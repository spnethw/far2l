#pragma once

#include "AppProvider.hpp"

class DummyAppProvider : public AppProvider
{
public:
	std::vector<CandidateInfo> GetAppCandidates(const std::wstring& pathname) override;
	std::wstring ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname) override;
};
