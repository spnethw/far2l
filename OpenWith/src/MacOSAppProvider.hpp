#pragma once

#include "AppProvider.hpp"

#if defined(__APPLE__)

class MacOSAppProvider : public AppProvider
{
public:
	std::string IdentifyFileMimeType(const std::wstring& pathname) const override;
	std::vector<CandidateInfo> GetAppCandidates(const std::wstring& pathname) override;
	std::wstring ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname) override;
};

#endif
