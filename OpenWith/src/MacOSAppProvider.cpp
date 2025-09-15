#include "MacOSAppProvider.hpp"

#if defined(__APPLE__)

std::vector<CandidateInfo> MacOSAppProvider::GetAppCandidates(const std::wstring& pathname)
{
	return {};
}

std::wstring MacOSAppProvider::ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname)
{
	return {};
}

std::wstring MacOSAppProvider::GetMimeType(const std::wstring& pathname)
{
	return {};
}

#endif
