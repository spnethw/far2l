#include "DummyAppProvider.hpp"
#include "common.hpp"
#include <string>
#include <vector>

std::vector<CandidateInfo> DummyAppProvider::GetAppCandidates(const std::wstring& pathname)
{
	return {};
}

std::wstring DummyAppProvider::ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname)
{
	return {};
}

std::wstring DummyAppProvider::GetMimeType(const std::wstring& pathname)
{
	return {};
}

