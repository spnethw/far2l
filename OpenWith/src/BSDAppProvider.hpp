#pragma once

#include "AppProvider.hpp"

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

class BSDAppProvider : public AppProvider
{
public:
	std::string IdentifyFileMimeType(const std::wstring& pathname) const override;
	std::vector<CandidateInfo> GetAppCandidates(const std::wstring& pathname, bool use_cache) override;
	std::wstring ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname) override;
};

#endif
