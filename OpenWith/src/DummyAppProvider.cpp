#include "DummyAppProvider.hpp"

std::vector<CandidateInfo> DummyAppProvider :: GetAppCandidates(const std::wstring& pathname)
{
	return {};
}

std::wstring DummyAppProvider :: ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname)
{
	return {};
}

