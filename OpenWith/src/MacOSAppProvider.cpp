#if defined(__APPLE__)

#include "MacOSAppProvider.hpp"
#include "common.hpp"
#include <string>
#include <vector>

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
