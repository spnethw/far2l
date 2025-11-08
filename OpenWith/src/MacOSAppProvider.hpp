#pragma once

#if defined(__APPLE__)

#include "AppProvider.hpp"
#include "common.hpp"
#include <string>
#include <vector>
#include <unordered_set>
#include <functional>

class MacOSAppProvider : public AppProvider
{
public:
	explicit MacOSAppProvider(TMsgGetter msg_getter);
	std::vector<CandidateInfo> GetAppCandidates(const std::vector<std::wstring>& pathnames) override;
	std::vector<std::wstring> ConstructCommandLine(const CandidateInfo& candidate, const std::vector<std::wstring>& pathnames) override;
	std::vector<std::wstring> GetMimeTypes() override;
	std::vector<Field> GetCandidateDetails(const CandidateInfo& candidate) override;
	std::vector<ProviderSetting> GetPlatformSettings() override { return {}; }
	void SetPlatformSettings(const std::vector<ProviderSetting>& settings) override {}
	void LoadPlatformSettings() override {}
	void SavePlatformSettings() override {}

private:
	// A struct to cache the results of a file type query.
	struct MacFileProfile {
		std::wstring uti;
		std::wstring mime_type;
		bool accessible; // True if file existed and UTI was gettable

		// Required for std::unordered_set
		bool operator==(const MacFileProfile& other) const {
			return accessible == other.accessible && uti == other.uti && mime_type == other.mime_type;
		}

		// Custom hash function for MacFileProfile.
		struct Hash {
			std::size_t operator()(const MacFileProfile& p) const noexcept {
				std::size_t h1 = std::hash<std::wstring>{}(p.uti);
				std::size_t h2 = std::hash<std::wstring>{}(p.mime_type);
				std::size_t h3 = std::hash<bool>{}(p.accessible);
				// Combine hashes
				std::size_t seed = h1;
				seed ^= h2 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
				seed ^= h3 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
				return seed;
			}
		};
	};

	// Caches all unique MacFileProfile objects collected during the last GetAppCandidates call.
	std::unordered_set<MacFileProfile, MacFileProfile::Hash> _last_mime_profiles;
};

#endif
