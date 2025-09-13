#pragma once

#include <string>
#include <vector>
#include <memory>

#include "common.hpp"

/*#include <optional>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <array>
#include <list>
#include <farplug-wide.h>
#include <KeyFileHelper.h>
#include <utils.h>
#include <stdio.h>
#include <cwctype>


#include "AppProvider.hpp"
#include "BSDAppProvider.hpp"
*/

class AppProvider
{
public:
	virtual ~AppProvider() = default;
	virtual std::vector<CandidateInfo> GetAppCandidates(const std::wstring& pathname) = 0;
	virtual std::wstring ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname) = 0;
	static std::unique_ptr<AppProvider> CreateAppProvider();
};
