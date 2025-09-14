#include "BSDAppProvider.hpp"

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

std::vector<CandidateInfo> BSDAppProvider::GetAppCandidates(const std::wstring& pathname)
{
	return {};
}

std::wstring BSDAppProvider :: ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname)
{
	return {};
}

std::string BSDAppProvider::IdentifyFileMimeType(const std::wstring& pathname) const
{
	return {};
}


#endif
