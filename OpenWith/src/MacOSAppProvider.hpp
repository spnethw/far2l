#pragma once

#if defined(__APPLE__)

#include "AppProvider.hpp"
#include "common.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

class KeyFileReadHelper;
class KeyFileHelper;

namespace openwith
{
	class MacOSAppProvider : public AppProvider
	{
	public:
		MacOSAppProvider();
		GetCandidatesResult GetAppCandidates(const std::vector<std::wstring>& filepaths,  ProgressCallback progress = nullptr,
											 const std::atomic<bool>* cancel_flag = nullptr) override;
		std::vector<std::wstring> ConstructLaunchCommands(const CandidateInfo& candidate, const std::vector<std::wstring>& filepaths) override;
		std::vector<std::wstring> GetMimeTypes() override;
		std::vector<Field> GetCandidateDetails(const CandidateInfo& candidate) override;
		std::vector<ProviderSetting> GetPlatformSettings() override { return {}; }
		void SetPlatformSettings(const std::vector<ProviderSetting>& settings) override {}
		void LoadPlatformSettings(const KeyFileReadHelper &key_reader) override {}
		void SavePlatformSettings(KeyFileHelper& key_writer) override {}

	private:
		// A struct to cache the results of a file type query. It stores the file's UTI and accessibility state.
		struct MacFileProfile
		{
			std::string uti; // Contains the resolved UTI string, or an empty string if the file is inaccessible.
			bool accessible; // true if the file was accessible and its UTI was successfully resolved.

			bool operator==(const MacFileProfile& other) const
			{
				return accessible == other.accessible && uti == other.uti;
			}

			struct Hash
			{
				std::size_t operator()(const MacFileProfile& p) const noexcept
				{
					std::size_t h1 = std::hash<std::string>{}(p.uti);
					std::size_t h2 = std::hash<bool>{}(p.accessible);
					std::size_t seed = h1;
					seed ^= h2 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
					return seed;
				}
			};
		};


		struct AppBundleMetadata
		{
			std::wstring name;
			std::wstring id;             // The full path to the .app bundle, used as a unique identifier.
			std::wstring version_string; // Used for disambiguation if names conflict.
		};


		struct RankedCandidate
		{
			const AppBundleMetadata* metadata = nullptr;
			int score = 0;
			int match_count = 0;
			bool operator<(const RankedCandidate& other) const
			{
				if (score != other.score) return score > other.score;
				return metadata->name < other.metadata->name;
			}
		};


		struct AppListForUti
		{
			std::optional<AppBundleMetadata> default_app_metadata;
			std::vector<AppBundleMetadata> compatible_apps_metadata;
		};


		static std::wstring EscapeForShell(const std::wstring& arg);

		// Accumulates unique file profiles processed during the last GetAppCandidates() call, used later by GetMimeTypes().
		std::unordered_set<MacFileProfile, MacFileProfile::Hash> _last_uti_profiles;
	};
} // namespace openwith

#endif
