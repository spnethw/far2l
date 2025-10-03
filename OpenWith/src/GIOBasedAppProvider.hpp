#pragma once

#ifdef ENABLE_GIO_SUPPORT

#include "AppProvider.hpp"
#include "common.hpp"
#include <string>
#include <vector>
#include <gio/gio.h>

class GIOBasedAppProvider : public AppProvider
{
public:
	explicit GIOBasedAppProvider(TMsgGetter msg_getter);
	~GIOBasedAppProvider() override = default;

	std::vector<CandidateInfo> GetAppCandidates(const std::wstring& pathname) override;
	std::wstring GetMimeType(const std::wstring& pathname) override;
	std::wstring ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname) override;
	std::vector<Field> GetCandidateDetails(const CandidateInfo& candidate) override;

private:
	static CandidateInfo ConvertGAppInfoToCandidate(GAppInfo* app_info);
};

#endif // ENABLE_GIO_SUPPORT
