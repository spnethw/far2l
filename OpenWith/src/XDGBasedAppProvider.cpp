#if defined (__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)

#include "AppProvider.hpp"
#include "XDGBasedAppProvider.hpp"
#include "lng.hpp"
#include "KeyFileHelper.h"
#include "WideMB.h"
#include "common.hpp"
#include "utils.h"
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cctype>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <unordered_set>
#include <utility>
#include <vector>
#include <map>
#include <set>

#define INI_LOCATION_XDG InMyConfig("plugins/openwith/config.ini")
#define INI_SECTION_XDG  "Settings.XDG"



// ****************************** Public API ******************************


XDGBasedAppProvider::XDGBasedAppProvider(TMsgGetter msg_getter) : AppProvider(std::move(msg_getter))
{
	_platform_settings_definitions = {
		{ "UseXdgMimeTool", MUseXdgMimeTool, &XDGBasedAppProvider::_use_xdg_mime_tool, true },
		{ "UseFileTool", MUseFileTool, &XDGBasedAppProvider::_use_file_tool, true },
		{ "UseMagikaTool", MUseMagikaTool, &XDGBasedAppProvider::_use_magika_tool, false },
		{ "UseExtensionBasedFallback", MUseExtensionBasedFallback, &XDGBasedAppProvider::_use_extension_based_fallback, false },
		{ "LoadMimeTypeAliases", MLoadMimeTypeAliases, &XDGBasedAppProvider::_load_mimetype_aliases, true },
		{ "LoadMimeTypeSubclasses", MLoadMimeTypeSubclasses, &XDGBasedAppProvider::_load_mimetype_subclasses, true },
		{ "ResolveStructuredSuffixes", MResolveStructuredSuffixes, &XDGBasedAppProvider::_resolve_structured_suffixes, true },
		{ "UseGenericMimeFallbacks", MUseGenericMimeFallbacks, &XDGBasedAppProvider::_use_generic_mime_fallbacks, true },
		{ "ShowUniversalHandlers", MShowUniversalHandlers, &XDGBasedAppProvider::_show_universal_handlers, true },
		{ "UseMimeinfoCache", MUseMimeinfoCache, &XDGBasedAppProvider::_use_mimeinfo_cache, true },
		{ "FilterByShowIn", MFilterByShowIn, &XDGBasedAppProvider::_filter_by_show_in, false },
		{ "ValidateTryExec", MValidateTryExec, &XDGBasedAppProvider::_validate_try_exec, false },
		{ "SortAlphabetically", MSortAlphabetically, &XDGBasedAppProvider::_sort_alphabetically, false },
		{ "TreatUrlsAsPaths", MTreatUrlsAsPaths, &XDGBasedAppProvider::_treat_urls_as_paths, false }
	};

	for (const auto& def : _platform_settings_definitions) {
		_key_wide_to_member_map[StrMB2Wide(def.key)] = def.member_variable;
	}
}


void XDGBasedAppProvider::LoadPlatformSettings()
{
	KeyFileReadSection key_file_reader(INI_LOCATION_XDG, INI_SECTION_XDG);
	for (const auto& def : _platform_settings_definitions) {
		this->*(def.member_variable) = key_file_reader.GetInt(def.key.c_str(), def.default_value) != 0;
	}
}


void XDGBasedAppProvider::SavePlatformSettings()
{
	KeyFileHelper key_file_helper(INI_LOCATION_XDG);
	for (const auto& def : _platform_settings_definitions) {
		key_file_helper.SetInt(INI_SECTION_XDG, def.key.c_str(), this->*(def.member_variable));
	}
	key_file_helper.Save();
}


std::vector<ProviderSetting> XDGBasedAppProvider::GetPlatformSettings()
{
	std::vector<ProviderSetting> settings;
	settings.reserve(_platform_settings_definitions.size());
	for (const auto& def : _platform_settings_definitions) {

		// Check if this setting is linked to a command-line tool via its internal string key.
		bool is_disabled = false;
		if (auto it = s_tool_key_map.find(def.key); it != s_tool_key_map.end()) {
			// If a corresponding tool name is found, check for the executable's existence.
			// The option is disabled if the tool is not found on the system.
			const std::string& tool_name = it->second;
			is_disabled = !IsExecutableAvailable(tool_name);
		}

		settings.push_back({StrMB2Wide(def.key), m_GetMsg(def.display_name_id), this->*(def.member_variable), is_disabled});
	}
	return settings;
}


void XDGBasedAppProvider::SetPlatformSettings(const std::vector<ProviderSetting>& settings)
{
	for (const auto& s : settings) {
		auto it = _key_wide_to_member_map.find(s.internal_key);
		if (it != _key_wide_to_member_map.end()) {
			this->*(it->second) = s.value;
		}
	}
}


// Finds applications that can open all specified files.
std::vector<CandidateInfo> XDGBasedAppProvider::GetAppCandidates(const std::vector<std::wstring>& filepaths_wide)
{
	if (filepaths_wide.empty()) {
		return {};
	}

	// Clear operation-scoped caches.
	_desktop_entry_cache.clear();
	_last_candidates_source_info.clear();
	_last_unique_mime_profiles.clear();

	// RAII helper: loads mimeapps.list, checks for external tools, and initializes lookup maps.
	OperationContext op_context(*this);

	CandidateMap final_candidates;

	if (filepaths_wide.size() == 1) {

		// --- Single file logic ---

		auto profile = GetRawMimeProfile(StrWide2MB(filepaths_wide[0]));
		_last_unique_mime_profiles.insert(profile);
		auto expanded_mimes = ExpandAndPrioritizeMimeTypes(profile);
		final_candidates = DiscoverCandidatesForExpandedMimes(expanded_mimes);

	} else {

		// --- Multiple files intersection logic ---

		// Step 1: Profile Deduplication. Group N files into K unique MIME profiles.
		for (const auto& filepath_wide : filepaths_wide) {
			_last_unique_mime_profiles.insert(GetRawMimeProfile(StrWide2MB(filepath_wide)));
		}

		// Step 2: Candidate gathering.
		// Resolve applications for each unique profile (K times, not N).
		std::unordered_map<RawMimeProfile, CandidateMap, RawMimeProfile::Hash> candidate_cache;
		if (_last_unique_mime_profiles.empty()) {
			return {};
		}
		for (const auto& profile : _last_unique_mime_profiles) {
			auto expanded_mimes_for_profile = ExpandAndPrioritizeMimeTypes(profile);
			auto candidates_for_profile = DiscoverCandidatesForExpandedMimes(expanded_mimes_for_profile);
			// Fail-fast: If one profile has no handlers, the intersection for all files is empty.
			if (candidates_for_profile.empty()) {
				return {};
			}
			candidate_cache.try_emplace(profile, std::move(candidates_for_profile));
		}

		// Step 3: Intersection.
		// Find common applications that exist in ALL profiles.

		// Step 3a: Optimization (smallest set first).
		// Start with the most restrictive set to minimize lookups and removals.
		auto smallest_set_it = _last_unique_mime_profiles.begin();
		size_t min_size = candidate_cache.at(*smallest_set_it).size();
		for (auto it = std::next(smallest_set_it); it != _last_unique_mime_profiles.end(); ++it) {
			size_t current_size = candidate_cache.at(*it).size();
			if (current_size < min_size) {
				min_size = current_size;
				smallest_set_it = it;
			}
		}

		// Step 3b: Establish the baseline survivors set.
		const RawMimeProfile base_profile = *smallest_set_it;
		// WARNING: candidate_cache.at(base_profile) becomes empty/invalid after this move.
		// Safe because we explicitly skip 'base_profile' in the filtering loop below.
		final_candidates = std::move(candidate_cache.at(base_profile));

		// Step 3c: Filter survivors against other profiles.
		for (const auto& filter_profile : _last_unique_mime_profiles) {
			if (filter_profile == base_profile) {
				continue;
			}
			const CandidateMap& filter_candidates = candidate_cache.at(filter_profile);
			for (auto survivor_it = final_candidates.begin(); survivor_it != final_candidates.end(); ) {
				const auto& survivor_key = survivor_it->first;
				auto& survivor_candidate = survivor_it->second;
				auto filter_match_it = filter_candidates.find(survivor_key);
				if (filter_match_it == filter_candidates.end()) {
					// The app is missing in the filter profile, so it cannot handle all files.
					// It fails the intersection test -> remove it.
					survivor_it = final_candidates.erase(survivor_it);
				} else {
					// The app exists in both lists. It survives this round.
					// Check if the match in the filter profile offers a better (higher) rank.
					const RankedCandidate& filter_candidate = filter_match_it->second;
					if (filter_candidate.rank > survivor_candidate.rank) {
						survivor_candidate.rank = filter_candidate.rank;
					}
					++survivor_it;
				}
			}
			if (final_candidates.empty()) {
				return {};
			}
		}
	}

	// --- Common post-processing (for both 1 and >1 file cases) ---

	auto final_candidates_sorted = BuildSortedRankedCandidatesList(final_candidates);
	// Only store source info (for F3) when a single file is selected,
	// as sources vary per file in multi-selection mode.
	return FormatCandidatesForUI(final_candidates_sorted, /* store_source_info = */ (filepaths_wide.size() == 1));
}


// Generates one or more executable command lines for the selected candidate and files.
std::vector<std::wstring> XDGBasedAppProvider::GenerateLaunchCommands(const CandidateInfo& candidate, const std::vector<std::wstring>& filepaths_wide)
{
	if (filepaths_wide.empty()) {
		return {};
	}

	const std::string desktop_id = StrWide2MB(candidate.id);

	if (auto it = _desktop_entry_cache.find(desktop_id);
		it == _desktop_entry_cache.end() || !it->second.has_value()) {
		return {};
	} else {
		const DesktopEntry& desktop_entry = it->second.value();

		if (desktop_entry.exec.empty()) {
			return {};
		}

		AnalyzeExecLine(desktop_entry);

		if (desktop_entry.arg_templates.empty()) {
			return {};
		}

		std::vector<std::string> filepaths;
		filepaths.reserve(filepaths_wide.size());
		for (const auto& filepath_wide : filepaths_wide) {
			filepaths.push_back(StrWide2MB(filepath_wide));
		}

		std::vector<std::wstring> launch_commands_wide;

		auto add_command_from_batch = [&](const std::vector<std::string>& batch) {
			auto command = AssembleLaunchCommand(desktop_entry, batch);
			if (!command.empty()) {
				launch_commands_wide.push_back(StrMB2Wide(command));
			}
		};

		if (desktop_entry.execution_model == ExecutionModel::PerFile) {
			// The application uses %f or %u field codes and accepts only one file per invocation.
			// Generate a separate command line for each selected file.
			launch_commands_wide.reserve(filepaths.size());
			for (const auto& filepath : filepaths) {
				add_command_from_batch({ filepath });
			}
		} else {
			// The application uses %F, %U, or has no field codes (Legacy).
			// It accepts the entire list of files in a single invocation.
			add_command_from_batch(filepaths);
		}
		return launch_commands_wide;
	}
}


// Gets the detailed information for a candidate to display in the F3 dialog.
std::vector<Field> XDGBasedAppProvider::GetCandidateDetails(const CandidateInfo& candidate)
{
	std::vector<Field> details;
	std::string desktop_filename = StrWide2MB(candidate.id);

	// Retrieve the cached DesktopEntry. This was populated during GetAppCandidates.
	auto it = _desktop_entry_cache.find(desktop_filename);
	if (it == _desktop_entry_cache.end() || !it->second.has_value()) {
		return details;
	}
	const DesktopEntry& desktop_entry = it->second.value();

	details.push_back({m_GetMsg(MDesktopFile), StrMB2Wide(desktop_entry.desktop_filepath)});

	// Retrieve the source info (e.g., "mimeapps.list") stored during GetAppCandidates.
	// This is only available for single-file selections.
	auto it_source = _last_candidates_source_info.find(candidate.id);
	if (it_source != _last_candidates_source_info.end()) {
		details.push_back({m_GetMsg(MSource), StrMB2Wide(it_source->second)});
	}

	for (const auto& [label_wide, member_ptr] : {
			 std::pair{L"Name =",        &DesktopEntry::name},
			 std::pair{L"GenericName =", &DesktopEntry::generic_name},
			 std::pair{L"Comment =",     &DesktopEntry::comment},
			 std::pair{L"Categories =",  &DesktopEntry::categories},
			 std::pair{L"Exec =",        &DesktopEntry::exec},
			 std::pair{L"TryExec =",     &DesktopEntry::try_exec},
			 std::pair{L"Terminal =",    &DesktopEntry::terminal},
			 std::pair{L"MimeType =",    &DesktopEntry::mimetype},
			 std::pair{L"NotShowIn =",   &DesktopEntry::not_show_in},
			 std::pair{L"OnlyShowIn =",  &DesktopEntry::only_show_in}
		 })
	{
		const std::string& val = desktop_entry.*member_ptr;
		if (!val.empty()) {
			details.push_back({ label_wide, StrMB2Wide(val) });
		}
	}

	return details;
}


// Returns a list of formatted strings representing the unique MIME profiles
// that were collected during the last GetAppCandidates() call.
std::vector<std::wstring> XDGBasedAppProvider::GetMimeTypes()
{
	std::set<std::wstring> unique_representations_wide;
	bool has_none = false;

	for (const auto& profile : _last_unique_mime_profiles)
	{
		std::set<std::string> profile_mimes;

		auto add_if_present = [&](const std::string& s) {
			if (!s.empty()) profile_mimes.insert(s);
		};

		add_if_present(profile.xdg_mime);
		add_if_present(profile.file_mime);
		add_if_present(profile.magika_mime);
		add_if_present(profile.stat_mime);
		add_if_present(profile.ext_mime);

		if (profile_mimes.empty()) {
			has_none = true;
		} else {
			std::stringstream ss;
			ss << "(";
			bool first = true;
			for (const auto& mime : profile_mimes) {
				if (!first) { ss << ";"; }
				ss << mime;
				first = false;
			}
			ss << ")";
			unique_representations_wide.insert(StrMB2Wide(ss.str()));
		}
	}

	std::vector<std::wstring> result_representations_wide;
	if (has_none) {
		result_representations_wide.push_back(L"(none)");
	}
	result_representations_wide.insert(result_representations_wide.end(), unique_representations_wide.begin(), unique_representations_wide.end());
	return result_representations_wide;
}



// ****************************** Searching and ranking candidates logic ******************************

// Resolves MIME types to a map of unique candidates.
XDGBasedAppProvider::CandidateMap XDGBasedAppProvider::DiscoverCandidatesForExpandedMimes(const std::vector<std::string>& expanded_mimes)
{
	// This map will store the final, unique candidates for this MIME list.
	CandidateMap unique_candidates;

	if (_op_xdg_mime_exists) {
		// Check for a global default app using 'xdg-mime query default'.
		const auto mime_for_default = expanded_mimes.empty() ? "" : expanded_mimes[0];
		std::string desktop_filename = GetDefaultApp(mime_for_default);
		if (!desktop_filename.empty()) {
			if (!IsAssociationRemoved(mime_for_default, desktop_filename)) {
				int rank = (expanded_mimes.size() - 0) * Ranking::SPECIFICITY_MULTIPLIER + Ranking::SOURCE_RANK_GLOBAL_DEFAULT;
				RegisterCandidateById(unique_candidates, desktop_filename, rank,  "xdg-mime query default " + mime_for_default);
			}
		}
	}

	AppendCandidatesFromMimeAppsLists(expanded_mimes, unique_candidates);

	// Find candidates using the cache prepared by OperationContext.
	if (_op_mime_to_handlers_map.has_value()) {
		// Use the results from mimeinfo.cache (it was successfully loaded and was not empty).
		AppendCandidatesFromMimeinfoCache(expanded_mimes, unique_candidates);
	} else {
		// Use the full scan index (either because caching was disabled, or as a fallback).
		AppendCandidatesByFullScan(expanded_mimes, unique_candidates);
	}

	return unique_candidates;
}


// Find the system's global default handler for a MIME type.
std::string XDGBasedAppProvider::GetDefaultApp(const std::string& mime)
{
	if (mime.empty()) return "";

	auto it = _op_default_app_cache.find(mime);
	if (it != _op_default_app_cache.end()) {
		return it->second;
	}

	std::string cmd = "xdg-mime query default " + EscapeArgForShell(mime) + " 2>/dev/null";

	auto desktop_filename = RunCommandAndCaptureOutput(cmd);
	_op_default_app_cache.try_emplace(mime, desktop_filename);
	return desktop_filename;
}


// Finds candidates from the parsed mimeapps.list files. This is a high-priority source.
void XDGBasedAppProvider::AppendCandidatesFromMimeAppsLists(const std::vector<std::string>& expanded_mimes, CandidateMap& unique_candidates)
{
	const int total_mimes = expanded_mimes.size();
	const auto& mimeapps_lists_data = _op_mimeapps_lists_data.value();

	for (int i = 0; i < total_mimes; ++i) {
		const auto& mime = expanded_mimes[i];
		// Rank based on MIME type specificity. The more specific, the higher the rank.
		int mime_specificity_rank = (total_mimes - i);

		// 1. Default application from mimeapps.list: high source rank.
		auto it_defaults = mimeapps_lists_data.defaults.find(mime);
		if (it_defaults != mimeapps_lists_data.defaults.end()) {
			const auto& default_app = it_defaults->second;
			if (!IsAssociationRemoved(mime, default_app.desktop_filename)) {
				int rank = mime_specificity_rank * Ranking::SPECIFICITY_MULTIPLIER + Ranking::SOURCE_RANK_MIMEAPPS_DEFAULT;
				auto source_info = default_app.source_filepath + StrWide2MB(m_GetMsg(MIn)) + " [Default Applications] " + StrWide2MB(m_GetMsg(MFor)) + mime;
				RegisterCandidateById(unique_candidates, default_app.desktop_filename, rank, source_info);
			}
		}
		// 2. Added associations from mimeapps.list: medium source rank.
		auto it_added = mimeapps_lists_data.added.find(mime);
		if (it_added != mimeapps_lists_data.added.end()) {
			for (const auto& app_assoc : it_added->second) {
				if (!IsAssociationRemoved(mime, app_assoc.desktop_filename)) {
					int rank = mime_specificity_rank * Ranking::SPECIFICITY_MULTIPLIER + Ranking::SOURCE_RANK_MIMEAPPS_ADDED;
					auto source_info = app_assoc.source_filepath + StrWide2MB(m_GetMsg(MIn)) + "[Added Associations]" + StrWide2MB(m_GetMsg(MFor)) + mime;
					RegisterCandidateById(unique_candidates, app_assoc.desktop_filename, rank, source_info);
				}
			}
		}
	}
}


// Finds candidates from the pre-generated mimeinfo.cache file for performance.
void XDGBasedAppProvider::AppendCandidatesFromMimeinfoCache(const std::vector<std::string>& expanded_mimes, CandidateMap& unique_candidates)
{
	const int total_mimes = expanded_mimes.size();

	// Tracks the highest score for each application to avoid rank demotion by a less-specific MIME type.
	std::unordered_map<std::string, AssociationScore> desktop_filename_to_score_map;

	const auto& mime_to_handlers_map = _op_mime_to_handlers_map.value();

	// First, find the best possible rank for each application across all matching MIME types.
	// This prevents an app from getting a low rank for a generic MIME type (e.g., text/plain)
	// if it has already been matched with a high-rank specific MIME type.
	for (int i = 0; i < total_mimes; ++i) {
		const auto& mime = expanded_mimes[i];
		auto it_cache = mime_to_handlers_map.find(mime);
		if (it_cache != mime_to_handlers_map.end()) {
			// Calculate rank using the tiered formula.
			int rank = (total_mimes - i) * Ranking::SPECIFICITY_MULTIPLIER + Ranking::SOURCE_RANK_CACHE_OR_SCAN;
			for (const auto& handler_provenance : it_cache->second) {
				const auto& desktop_filename = handler_provenance.desktop_filename;
				if (desktop_filename.empty()) continue;
				if (IsAssociationRemoved(mime, desktop_filename)) continue;
				auto source_info = handler_provenance.source_filepath + StrWide2MB(m_GetMsg(MFor)) + mime;
				auto [it, inserted] = desktop_filename_to_score_map.try_emplace(desktop_filename, rank, source_info);
				// Update only if the new rank is higher than the existing one.
				if (!inserted && rank > it->second.rank) {
					it->second.rank = rank;
					it->second.source_info = source_info;
				}
			}
		}
	}

	// Register each application using its highest calculated rank.
	for (const auto& [desktop_filename, score] : desktop_filename_to_score_map) {
		RegisterCandidateById(unique_candidates, desktop_filename, score.rank, score.source_info);
	}
}


// Finds candidates by looking up the pre-built mime-to-app index. Used when _use_mimeinfo_cache is false.
void XDGBasedAppProvider::AppendCandidatesByFullScan(const std::vector<std::string>& expanded_mimes, CandidateMap& unique_candidates)
{
	const int total_mimes = expanded_mimes.size();

	// Tracks the highest score for each application to avoid rank demotion by a less-specific MIME type.
	std::unordered_map<const DesktopEntry*, AssociationScore> desktop_entry_to_score_map;

	const auto& mime_to_desktop_entry_map = _op_mime_to_desktop_entry_map.value();

	// Iterate through all expanded MIME types for the current file.
	for (int i = 0; i < total_mimes; ++i) {
		const auto& mime = expanded_mimes[i];

		// Find this MIME type in the pre-built mime-to-app index.
		auto it_index = mime_to_desktop_entry_map.find(mime);
		if (it_index == mime_to_desktop_entry_map.end()) {
			continue; // no applications are associated with this MIME type in the index.
		}

		// Calculate the rank for this level of MIME specificity.
		int rank = (total_mimes - i) * Ranking::SPECIFICITY_MULTIPLIER + Ranking::SOURCE_RANK_CACHE_OR_SCAN;

		// Iterate through all applications found for this MIME type.
		for (const DesktopEntry* desktop_entry_ptr : it_index->second) {
			if (!desktop_entry_ptr) continue;

			// Check if this specific association is explicitly removed in mimeapps.list.
			if (IsAssociationRemoved(mime, GetBaseName(desktop_entry_ptr->desktop_filepath))) {
				continue;
			}

			auto source_info = StrWide2MB(m_GetMsg(MFullScanFor)) + mime;

			auto [it, inserted] = desktop_entry_to_score_map.try_emplace(desktop_entry_ptr, rank, source_info);
			// Update only if the new rank is higher than the existing one.
			if (!inserted && rank > it->second.rank) {
				it->second.rank = rank;
				it->second.source_info = source_info;
			}
		}
	}

	// Register each application using its highest calculated rank.
	for (const auto& [desktop_entry_ptr, score] : desktop_entry_to_score_map) {
		// Call the registration helper directly, passing the dereferenced entry to avoid a redundant cache lookup.
		RegisterCandidateFromObject(unique_candidates, *desktop_entry_ptr, score.rank, score.source_info);
	}
}


// Processes a single application candidate: validates it, filters it, and adds it to the map.
// This version retrieves the DesktopEntry from cache via its name.
void XDGBasedAppProvider::RegisterCandidateById(CandidateMap& unique_candidates, const std::string& desktop_filename,
													   int rank, const std::string& source_info)
{
	if (desktop_filename.empty()) return;

	// Retrieve the full DesktopEntry, either from cache or by parsing it.
	const auto& desktop_entry_opt = GetCachedDesktopEntry(desktop_filename);
	if (!desktop_entry_opt) {
		return;
	}

	// Pass the actual DesktopEntry object to the core validation and registration logic.
	RegisterCandidateFromObject(unique_candidates, *desktop_entry_opt, rank, source_info);
}


// This is the core validation and registration helper.
// It filters a candidate (TryExec, ShowIn/NotShowIn) and adds it to the map.
void XDGBasedAppProvider::RegisterCandidateFromObject(CandidateMap& unique_candidates, const DesktopEntry& desktop_entry,
											int rank, const std::string& source_info)
{
	// Optionally validate the TryExec key to ensure the executable exists.
	if (_validate_try_exec && !desktop_entry.try_exec.empty() && !IsExecutableAvailable(UnescapeGKeyFileString(desktop_entry.try_exec))) {
		return;
	}

	const auto& current_desktop_env = _op_current_desktop_env.value();

	// Optionally filter applications based on the current desktop environment
	// using the OnlyShowIn and NotShowIn keys.
	if (_filter_by_show_in && !current_desktop_env.empty()) {
		if (!desktop_entry.only_show_in.empty()) {
			bool found = false;
			for (const auto& desktop : SplitString(desktop_entry.only_show_in, ';')) {
				if (desktop == current_desktop_env) { found = true; break; }
			}
			if (!found) return;
		}
		if (!desktop_entry.not_show_in.empty()) {
			bool found = false;
			for (const auto& desktop : SplitString(desktop_entry.not_show_in, ';')) {
				if (desktop == current_desktop_env) { found = true; break; }
			}
			if (found) return;
		}
	}

	AddOrUpdateCandidate(unique_candidates, desktop_entry, rank, source_info);
}


// Adds a new candidate to the results map or updates its rank if a better one is found.
void XDGBasedAppProvider::AddOrUpdateCandidate(CandidateMap& unique_candidates, const DesktopEntry& desktop_entry,
											   int rank, const std::string& source_info)
{
	// Composite key (Name + Exec) used to deduplicate candidates defined across multiple .desktop files or overlays.
	AppUniqueKey unique_key{desktop_entry.name, desktop_entry.exec};

	auto [it, inserted] = unique_candidates.try_emplace(unique_key, RankedCandidate{&desktop_entry, rank, source_info});

	// If the candidate already exists, update it only if the new rank is higher.
	if (!inserted && rank > it->second.rank) {
		it->second.rank = rank;
		it->second.desktop_entry = &desktop_entry;
		it->second.source_info = source_info;
	}
}


// Checks if an application association for a given MIME type is explicitly removed
// in the [Removed Associations] section of mimeapps.list.
bool XDGBasedAppProvider::IsAssociationRemoved(const std::string& mime, const std::string& desktop_filename)
{
	// Get the parsed config from the operation-scoped cache.
	const auto& mimeapps_lists_data = _op_mimeapps_lists_data.value();

	// 1. Check for an exact match (e.g., "image/jpeg")
	auto it_exact = mimeapps_lists_data.removed.find(mime);
	if (it_exact != mimeapps_lists_data.removed.end() && it_exact->second.count(desktop_filename)) {
		return true;
	}

	// 2. Check for a wildcard match (e.g., "image/*")
	const size_t slash_pos = mime.find('/');
	if (slash_pos != std::string::npos) {
		const std::string wildcard_mime = mime.substr(0, slash_pos) + "/*";
		auto it_wildcard = mimeapps_lists_data.removed.find(wildcard_mime);
		if (it_wildcard != mimeapps_lists_data.removed.end() && it_wildcard->second.count(desktop_filename)) {
			return true;
		}
	}

	return false;
}


// Converts the final map of candidates into a vector and sorts it.
std::vector<XDGBasedAppProvider::RankedCandidate> XDGBasedAppProvider::BuildSortedRankedCandidatesList(const CandidateMap& candidate_map)
{
	// Convert the map of unique candidates to a vector for sorting.
	std::vector<RankedCandidate> sorted_candidates;
	sorted_candidates.reserve(candidate_map.size());
	for (const auto& [key, ranked_candidate] : candidate_map) {
		sorted_candidates.push_back(ranked_candidate);
	}

	// Sort the final vector based on rank or alphabetical settings.
	if (_sort_alphabetically) {
		// Sort candidates based on their name only
		std::sort(sorted_candidates.begin(), sorted_candidates.end(), [](const RankedCandidate& a, const RankedCandidate& b) {
			if (!a.desktop_entry || !b.desktop_entry) return b.desktop_entry != nullptr;
			return a.desktop_entry->name < b.desktop_entry->name;
		});
	} else {
		// Use the standard ranking defined in the < operator for RankedCandidate (sorts by rank descending, then name ascending).
		std::sort(sorted_candidates.begin(), sorted_candidates.end());
	}

	return sorted_candidates;
}


// Converts the sorted list of RankedCandidates into the final CandidateInfo list for the UI.
std::vector<CandidateInfo> XDGBasedAppProvider::FormatCandidatesForUI(
	const std::vector<RankedCandidate>& ranked_candidates,
	bool store_source_info)
{
	std::vector<CandidateInfo> result;
	result.reserve(ranked_candidates.size());

	for (const auto& ranked_candidate : ranked_candidates) {
		// Convert the internal DesktopEntry representation to the UI-facing CandidateInfo.
		CandidateInfo ci = ConvertDesktopEntryToCandidateInfo(*ranked_candidate.desktop_entry);

		// If requested (only for single-file lookups), store the association's source
		// for the F3 details dialog.
		if (store_source_info) {
			_last_candidates_source_info.try_emplace(ci.id, ranked_candidate.source_info);
		}
		result.push_back(ci);
	}
	return result;
}


// Converts the internal DesktopEntry representation into the UI-facing CandidateInfo structure.
CandidateInfo XDGBasedAppProvider::ConvertDesktopEntryToCandidateInfo(const DesktopEntry& desktop_entry)
{
	CandidateInfo candidate;
	candidate.terminal = (desktop_entry.terminal == "true");
	candidate.name = StrMB2Wide(UnescapeGKeyFileString(desktop_entry.name));
	candidate.id = StrMB2Wide(GetBaseName(desktop_entry.desktop_filepath));

	// Ensure the execution model is determined before checking capabilities.
	AnalyzeExecLine(desktop_entry);

	// The 'multi_file_aware' flag indicates if the plugin can pass a list of files to this app.
	// If the model is PerFile (%f/%u), the app strictly accepts one argument, so it is *not* multi-file aware
	// from the perspective of a single invocation. FileList (%F/%U) and LegacyImplicit modes support lists.
	candidate.multi_file_aware = (desktop_entry.execution_model != ExecutionModel::PerFile);

	return candidate;
}



// ****************************** File MIME Type Detection & Expansion ******************************

// Gathers file attributes and "raw" MIME types from all enabled detection methods for a single file.
XDGBasedAppProvider::RawMimeProfile XDGBasedAppProvider::GetRawMimeProfile(const std::string& filepath)
{
	RawMimeProfile profile = {};
	struct stat st;

	if (stat(filepath.c_str(), &st) != 0) {
		// stat() failed (e.g., ENOENT).
		// We can't determine any file type. Return the empty profile.
		return profile;
	}

	// Path exists. Determine file type and get MIME types based on it.
	if (S_ISREG(st.st_mode)) {
		profile.is_regular_file = true;

		// Only call extension-based lookup for regular files
		if (_use_extension_based_fallback) {
			profile.ext_mime = GuessMimeTypeByExtension(filepath);
		}

		auto should_run_cli_tools = ((_use_xdg_mime_tool && _op_xdg_mime_exists) || _op_file_tool_enabled_and_exists || _op_magika_tool_enabled_and_exists);

		// Run expensive external tools ONLY for accessible regular files.
		if (should_run_cli_tools && access(filepath.c_str(), R_OK) == 0) {

			auto filepath_escaped = EscapeArgForShell(filepath);

			if (_use_xdg_mime_tool && _op_xdg_mime_exists) {
				profile.xdg_mime = DetectMimeTypeWithXdgMimeTool(filepath_escaped);
			}
			if (_op_file_tool_enabled_and_exists) {
				profile.file_mime = DetectMimeTypeWithFileTool(filepath_escaped);
			}
			if (_op_magika_tool_enabled_and_exists) {
				profile.magika_mime = DetectMimeTypeWithMagikaTool(filepath_escaped);
			}
		}

	} else if (S_ISDIR(st.st_mode)) {
		profile.stat_mime = "inode/directory";
	} else if (S_ISFIFO(st.st_mode)) {
		profile.stat_mime = "inode/fifo";
	} else if (S_ISSOCK(st.st_mode)) {
		profile.stat_mime = "inode/socket";
	} else if (S_ISCHR(st.st_mode)) {
		profile.stat_mime = "inode/chardevice";
	} else if (S_ISBLK(st.st_mode)) {
		profile.stat_mime = "inode/blockdevice";
	}

	// Return the populated profile.
	return profile;
}


// Expands and prioritizes MIME types for a file using multiple methods.
std::vector<std::string> XDGBasedAppProvider::ExpandAndPrioritizeMimeTypes(const RawMimeProfile& profile)
{
	std::vector<std::string> mimes;
	std::unordered_set<std::string> seen_mimes;

	static const std::string s_octet_stream = "application/octet-stream";
	bool octet_stream_detected = false;

	// Helper to add a MIME type only if it's valid and not already present.
	auto add_unique = [&](std::string mime) {
		mime = Trim(mime);
		if (mime.empty()) return;
		if (mime == s_octet_stream) {
			octet_stream_detected = true;
			return;
		}
		if (mime.find('/') != std::string::npos && seen_mimes.insert(mime).second) {
			mimes.push_back(std::move(mime));
		}
	};

	// --- Step 1: Initial, most specific MIME type detection ---
	// Use the pre-detected types from the RawMimeProfile in order of priority.
	add_unique(profile.xdg_mime);
	add_unique(profile.file_mime);
	add_unique(profile.magika_mime);
	add_unique(profile.stat_mime);
	add_unique(profile.ext_mime);

	// --- Step 2: Iteratively expand the MIME type list with parents and aliases ---
	if (_op_subclass_to_parent_map || _op_alias_to_canonical_map) {

		for (size_t i = 0; i < mimes.size(); ++i) {
			const std::string current_mime = mimes[i];

			// Expansion A: process aliases
			if (_op_alias_to_canonical_map) {

				// --- Standard forward lookup (alias -> canonical) ---
				const auto& alias_to_canonical_map = *_op_alias_to_canonical_map;
				auto it_canonical = alias_to_canonical_map.find(current_mime);
				if (it_canonical != alias_to_canonical_map.end()) {
					add_unique(it_canonical->second);
				}

				// --- Reverse lookup (canonical -> aliases) using major-type filtered map ---
				auto it_aliases = _op_canonical_to_aliases_map->find(current_mime);
				if (it_aliases != _op_canonical_to_aliases_map->end()) {
					for (const auto& alias : it_aliases->second) {
						add_unique(alias);
					}
				}
			}

			// Expansion B: Add parent from subclass hierarchy.
			if (_op_subclass_to_parent_map) {
				auto it = _op_subclass_to_parent_map->find(current_mime);
				if (it != _op_subclass_to_parent_map->end()) {
					add_unique(it->second);
				}
			}
		}
	}

	// --- Step 3: Add base types for structured syntaxes (e.g., +xml, +zip) ---
	// This is a reliable fallback that works even if the subclass hierarchy is incomplete in the system's MIME database.
	if (_resolve_structured_suffixes) {

		static const std::map<std::string, std::string> s_suffix_to_base_mime = {
			{"xml", "application/xml"},
			{"zip", "application/zip"},
			{"json", "application/json"},
			{"gzip", "application/gzip"}
		};

		const auto size_before_suffixes = mimes.size();
		for (size_t i = 0; i < size_before_suffixes; ++i) {
			const auto mime = mimes[i];
			size_t plus_pos = mime.rfind('+');
			if (plus_pos != std::string::npos && plus_pos < mime.length() - 1) {
				std::string suffix = mime.substr(plus_pos + 1);
				auto it = s_suffix_to_base_mime.find(suffix);
				if (it != s_suffix_to_base_mime.end()) {
					add_unique(it->second);
				}
			}
		}
	}

	// --- Step 4: Add generic fallback MIME types ---
	if (_use_generic_mime_fallbacks) {

		const auto size_before_generic_types = mimes.size();
		for (size_t i = 0; i < size_before_generic_types; ++i) {
			const auto mime = mimes[i];
			// text/plain is a safe fallback for any text/* type.
			if (mime.rfind("text/", 0) == 0) {
				add_unique("text/plain");
			}
			// Add wildcard for the major type (e.g., image/jpeg -> image/*)
			size_t slash_pos = mime.find('/');
			if (slash_pos != std::string::npos) {
				add_unique(mime.substr(0, slash_pos) + "/*");
			}
		}
	}

	// --- Step 5: Add the ultimate fallback for any binary data ---
	if (profile.is_regular_file) {
		if (_show_universal_handlers || octet_stream_detected) {
			mimes.push_back(s_octet_stream);
		}
	}

	return mimes;
}


std::string XDGBasedAppProvider::DetectMimeTypeWithXdgMimeTool(const std::string& filepath_escaped)
{
	return RunCommandAndCaptureOutput("xdg-mime query filetype " + filepath_escaped + " 2>/dev/null");
}


std::string XDGBasedAppProvider::DetectMimeTypeWithFileTool(const std::string& filepath_escaped)
{
	return RunCommandAndCaptureOutput("file --brief --dereference --mime-type " + filepath_escaped + " 2>/dev/null");
}


std::string XDGBasedAppProvider::DetectMimeTypeWithMagikaTool(const std::string& filepath_escaped)
{
	return RunCommandAndCaptureOutput("magika --no-colors --format '%m' " + filepath_escaped + " 2>/dev/null");
}


std::string XDGBasedAppProvider::GuessMimeTypeByExtension(const std::string& filepath)
{
	// A static map for common file extensions as a last-resort fallback.
	// This is not comprehensive but covers many common cases if other tools fail.
	static const std::unordered_map<std::string, std::string> s_ext_to_mime_map = {

		// Shell / scripts / source code

		{".sh",    "application/x-shellscript"},
		{".bash",  "application/x-shellscript"},
		{".csh",   "application/x-csh"},
		{".zsh",   "application/x-shellscript"},
		{".ps1",   "application/x-powershell"},
		{".py",    "text/x-python"},
		{".pyw",   "text/x-python"},
		{".pl",    "text/x-perl"},
		{".pm",    "text/x-perl"},
		{".rb",    "text/x-ruby"},
		{".php",   "application/x-php"},
		{".phps",  "application/x-php"},
		{".js",    "application/javascript"},
		{".mjs",   "application/javascript"},
		{".java",  "text/x-java-source"},
		{".c",     "text/x-csrc"},
		{".h",     "text/x-chdr"},
		{".cpp",   "text/x-c++src"},
		{".cc",    "text/x-c++src"},
		{".cxx",   "text/x-c++src"},
		{".hpp",   "text/x-c++hdr"},
		{".go",    "text/x-go"},
		{".rs",    "text/rust"},
		{".swift", "text/x-swift"},

		// Plain text / markup / data

		{".txt",   "text/plain"},
		{".md",    "text/markdown"},
		{".markdown","text/markdown"},
		{".rtf",   "application/rtf"},
		{".tex",   "application/x-tex"},
		{".csv",   "text/csv"},
		{".tsv",   "text/tab-separated-values"},
		{".log",   "text/plain"},
		{".json",  "application/json"},
		{".yaml",  "text/yaml"},
		{".yml",   "text/yaml"},
		{".xml",   "application/xml"},
		{".html",  "text/html"},
		{".htm",   "text/html"},
		{".xhtml", "application/xhtml+xml"},
		{".ics",   "text/calendar"},

		// Office / documents

		{".pdf",   "application/pdf"},
		{".doc",   "application/msword"},
		{".docx",  "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
		{".xls",   "application/vnd.ms-excel"},
		{".xlsx",  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
		{".ppt",   "application/vnd.ms-powerpoint"},
		{".pptx",  "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
		{".odt",   "application/vnd.oasis.opendocument.text"},
		{".ods",   "application/vnd.oasis.opendocument.spreadsheet"},
		{".odp",   "application/vnd.oasis.opendocument.presentation"},
		{".epub",  "application/epub+zip"},
		{".rtf",   "application/rtf"},

		// Images

		{".jpg",   "image/jpeg"},
		{".jpeg",  "image/jpeg"},
		{".jpe",   "image/jpeg"},
		{".png",   "image/png"},
		{".gif",   "image/gif"},
		{".webp",  "image/webp"},
		{".svg",   "image/svg+xml"},
		{".ico",   "image/vnd.microsoft.icon"},
		{".bmp",   "image/bmp"},
		{".tif",   "image/tiff"},
		{".tiff",  "image/tiff"},
		{".heic",  "image/heic"},
		{".avif",  "image/avif"},
		{".apng",  "image/apng"},

		// Audio

		{".mp3",   "audio/mpeg"},
		{".m4a",   "audio/mp4"},
		{".aac",   "audio/aac"},
		{".ogg",   "audio/ogg"},
		{".oga",   "audio/ogg"},
		{".opus",  "audio/opus"},
		{".wav",   "audio/x-wav"},
		{".flac",  "audio/flac"},
		{".mid",   "audio/midi"},
		{".midi",  "audio/midi"},
		{".weba",  "audio/webm"},

		// Video

		{".mp4",   "video/mp4"},
		{".m4v",   "video/mp4"},
		{".mov",   "video/quicktime"},
		{".mkv",   "video/x-matroska"},
		{".webm",  "video/webm"},
		{".ogv",   "video/ogg"},
		{".avi",   "video/x-msvideo"},
		{".flv",   "video/x-flv"},
		{".wmv",   "video/x-ms-wmv"},
		{".3gp",   "video/3gpp"},
		{".3g2",   "video/3gpp2"},
		{".ts",    "video/mp2t"},

		// Archives / compressed

		{".zip",   "application/zip"},
		{".tar",   "application/x-tar"},
		{".gz",    "application/gzip"},
		{".tgz",   "application/gzip"},
		{".bz",    "application/x-bzip"},
		{".bz2",   "application/x-bzip2"},
		{".xz",    "application/x-xz"},
		{".7z",    "application/x-7z-compressed"},
		{".rar",   "application/vnd.rar"},
		{".jar",   "application/java-archive"},

		// Executables / binaries

		{".exe",   "application/x-ms-dos-executable"},
		{".dll",   "application/x-msdownload"},
		{".so",    "application/x-sharedlib"},
		{".elf",   "application/x-executable"},
		{".bin",   "application/octet-stream"},
		{".class", "application/java-vm"},

		// Fonts

		{".ttf",   "font/ttf"},
		{".otf",   "font/otf"},
		{".woff",  "font/woff"},
		{".woff2", "font/woff2"},
		{".eot",   "application/vnd.ms-fontobject"},

		// PostScript / vector

		{".ps",    "application/postscript"},
		{".eps",   "application/postscript"},
		{".ai",    "application/postscript"},

		// Disk images / containers

		{".iso",   "application/x-iso9660-image"},
		{".img",   "application/octet-stream"},
		{".dmg",   "application/x-apple-diskimage"},

		// Web / misc

		{".css",   "text/css"},
		{".map",   "application/json"},
		{".wasm",  "application/wasm"},
		{".jsonld","application/ld+json"},
		{".webmanifest","application/manifest+json"},

		// CAD / specialized

		{".dxf",   "image/vnd.dxf"},
		{".dwg",   "application/acad"},

		// Mail / office miscellany

		{".msg",   "application/vnd.ms-outlook"}
	};

	auto filename = GetBaseName(filepath);
	auto dot_pos = filename.rfind('.');
	if (dot_pos != std::string::npos) {
		std::string ext = filename.substr(dot_pos);
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
		auto it = s_ext_to_mime_map.find(ext);
		if (it != s_ext_to_mime_map.end()) {
			return it->second;
		}
	}

	return "";
}



// ****************************** XDG Database Parsing & Caching ******************************

// Retrieves a DesktopEntry from the cache or parses it from disk if not present.
const std::optional<XDGBasedAppProvider::DesktopEntry>& XDGBasedAppProvider::GetCachedDesktopEntry(const std::string& desktop_filename)
{
	// Use the class-level cache
	if (auto it = _desktop_entry_cache.find(desktop_filename); it != _desktop_entry_cache.end()) {
		return it->second;
	}

	for (const auto& dirpath : _op_desktop_file_dirpaths.value()) {
		std::string pathname = dirpath + "/" + desktop_filename;
		if (auto desktop_entry = ParseDesktopFile(pathname)) {
			// A valid entry was found and parsed, cache and return it.
			auto [it, inserted] = _desktop_entry_cache.try_emplace(desktop_filename, std::move(desktop_entry));
			return it->second;
		}
	}
	// Cache a nullopt if the file is not found anywhere to avoid repeated searches.
	auto [it, inserted] = _desktop_entry_cache.try_emplace(desktop_filename, std::nullopt);
	return it->second;
}


// Builds in-memory index that maps a MIME type to a list of DesktopEntry pointers
// that can handle it. This avoids repeated filesystem scanning.
XDGBasedAppProvider::MimeToDesktopEntryIndex XDGBasedAppProvider::FullScanDesktopFiles(const std::vector<std::string>& search_dirpaths)
{
	MimeToDesktopEntryIndex index;
	for (const auto& dirpath : search_dirpaths) {
		DIR* dir_stream = opendir(dirpath.c_str());
		if (!dir_stream) continue;

		struct dirent* dir_entry;
		while ((dir_entry = readdir(dir_stream))) {
			std::string filename = dir_entry->d_name;
			if (filename.size() <= 8 || filename.compare(filename.size() - 8, 8, ".desktop") != 0) {
				continue;
			}

			// This call populates _desktop_entry_cache as a side effect.
			const auto& desktop_entry_opt = GetCachedDesktopEntry(filename);
			if (!desktop_entry_opt.has_value()) {
				continue;
			}

			// A non-owning pointer is safe because _desktop_entry_cache is a std::map.
			const DesktopEntry* desktop_entry_ptr = &desktop_entry_opt.value();

			// Split the MimeType= key and populate the index for each MIME type.
			std::vector<std::string> entry_mimes = SplitString(desktop_entry_ptr->mimetype, ';');
			for (const auto& mime : entry_mimes) {
				if (!mime.empty()) {
					// Add the pointer to the list for this MIME type.
					index[mime].push_back(desktop_entry_ptr);
				}
			}
		}
		closedir(dir_stream);
	}
	return index;
}


// Parses all mimeinfo.cache files found in the XDG search paths into a single map, avoiding repeated file I/O.
XDGBasedAppProvider::MimeinfoCacheData XDGBasedAppProvider::ParseAllMimeinfoCacheFiles(const std::vector<std::string>& search_dirpaths)
{
	MimeinfoCacheData mimeinfo_cache_data;
	for (const auto& dirpath : search_dirpaths) {
		std::string filepath = dirpath + "/mimeinfo.cache";
		if (IsReadableFile(filepath)) {
			ParseMimeinfoCache(filepath, mimeinfo_cache_data);
		}
	}
	return mimeinfo_cache_data;
}


// Parses mimeinfo.cache file format: [MIME Cache] section with mime/type=app1.desktop;app2.desktop;
void XDGBasedAppProvider::ParseMimeinfoCache(const std::string& filepath, MimeinfoCacheData& mimeinfo_cache_data)
{
	std::ifstream file(filepath);
	if (!file.is_open()) return;

	std::string line;
	bool in_cache_section = false;
	while (std::getline(file, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == '#') continue;

		if (line == "[MIME Cache]") {
			in_cache_section = true;
			continue;
		}

		if (line[0] == '[') {
			in_cache_section = false;
			continue;
		}

		if (in_cache_section) {
			auto eq_pos = line.find('=');
			if (eq_pos == std::string::npos) continue;

			std::string mime = Trim(line.substr(0, eq_pos));
			std::string desktop_filenames_str = Trim(line.substr(eq_pos + 1));

			auto desktop_filenames = SplitString(desktop_filenames_str, ';');
			if (!mime.empty() && !desktop_filenames.empty()) {
				auto& existing = mimeinfo_cache_data[mime];
				for (const auto& desktop_filename : desktop_filenames) {
					if (!desktop_filename.empty()) {
						// We append apps from all cache files; duplicates are okay.
						existing.push_back(HandlerProvenance(desktop_filename, filepath));
					}
				}
			}
		}
	}
}


// Combines multiple mimeapps.list files into a single association structure.
XDGBasedAppProvider::MimeappsListsData XDGBasedAppProvider::ParseMimeappsLists(const std::vector<std::string>& filepaths)
{
	MimeappsListsData mimeapps_lists_data;
	for (const auto& filepath : filepaths) {
		ParseMimeappsList(filepath, mimeapps_lists_data);
	}
	return mimeapps_lists_data;
}


// Parses a single mimeapps.list file, extracting Default/Added/Removed associations.
void XDGBasedAppProvider::ParseMimeappsList(const std::string& filepath, MimeappsListsData& mimeapps_lists_data)
{
	std::ifstream file(filepath);
	if (!file.is_open()) return;

	std::string line, current_section;
	while (std::getline(file, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == '#') continue;

		if (line[0] == '[' && line.back() == ']') {
			current_section = line;
			continue;
		}

		auto eq_pos = line.find('=');
		if (eq_pos == std::string::npos) continue;

		std::string mime = Trim(line.substr(0, eq_pos));
		std::string desktop_filenames_str = Trim(line.substr(eq_pos + 1));
		auto desktop_filenames = SplitString(desktop_filenames_str, ';');

		if (desktop_filenames.empty()) continue;

		if (current_section == "[Default Applications]") {
			// Only use the first default if not already set, as higher priority files are parsed first.
			mimeapps_lists_data.defaults.try_emplace(mime, desktop_filenames[0], filepath);
		} else if (current_section == "[Added Associations]") {
			for (const auto& desktop_filename : desktop_filenames) {
				mimeapps_lists_data.added[mime].push_back(HandlerProvenance(desktop_filename, filepath));
			}
		} else if (current_section == "[Removed Associations]") {
			for(const auto& desktop_filename : desktop_filenames) {
				mimeapps_lists_data.removed[mime].insert(desktop_filename);
			}
		}
	}
}


// Parses a .desktop file according to the Desktop Entry Specification.
std::optional<XDGBasedAppProvider::DesktopEntry> XDGBasedAppProvider::ParseDesktopFile(const std::string& filepath)
{
	std::ifstream file(filepath);
	if (!file.is_open()) {
		return std::nullopt;
	}

	std::string line;
	bool in_main_section = false;
	DesktopEntry desktop_entry;
	desktop_entry.desktop_filepath = filepath;

	std::unordered_map<std::string, std::string> kv_entries;

	// Parse key-value pairs from the [Desktop Entry] section only.
	while (std::getline(file, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == '#') continue;
		if (line == "[Desktop Entry]") {
			in_main_section = true;
			continue;
		}
		if (line[0] == '[') {
			in_main_section = false;
			continue;
		}
		if (in_main_section) {
			auto eq_pos = line.find('=');
			if (eq_pos == std::string::npos) continue;
			std::string key = Trim(line.substr(0, eq_pos));
			std::string value = Trim(line.substr(eq_pos + 1));
			kv_entries[key] = value;
		}
	}

	// Validate required fields and application type according to the spec.
	bool is_application = false;
	if (auto it = kv_entries.find("Type"); it != kv_entries.end() && it->second == "Application") {
		is_application = true;
	}

	bool hidden = false;
	if (auto it = kv_entries.find("Hidden"); it != kv_entries.end() && it->second == "true") {
		hidden = true;
	}

	// Ignore hidden entries and non-applications
	if (hidden || !is_application) {
		return std::nullopt;
	}

	//  An application must have a non-empty Exec field.
	if (auto it = kv_entries.find("Exec"); it == kv_entries.end() || it->second.empty()) {
		return std::nullopt;
	} else {
		desktop_entry.exec = it->second;
	}

	// The Name field is required for a valid desktop entry.
	desktop_entry.name = GetLocalizedValue(kv_entries, "Name");
	if (desktop_entry.name.empty()) {
		return std::nullopt;
	}

	// Extract optional fields with localization support.
	desktop_entry.generic_name = GetLocalizedValue(kv_entries, "GenericName");
	desktop_entry.comment = GetLocalizedValue(kv_entries, "Comment");

	for (const auto& [key, member_ptr] : {
			 std::pair{"Categories", &DesktopEntry::categories},
			 std::pair{"TryExec",    &DesktopEntry::try_exec},
			 std::pair{"Terminal",   &DesktopEntry::terminal},
			 std::pair{"MimeType",   &DesktopEntry::mimetype},
			 std::pair{"OnlyShowIn", &DesktopEntry::only_show_in},
			 std::pair{"NotShowIn",  &DesktopEntry::not_show_in}
		 })
	{
		if (auto it = kv_entries.find(key); it != kv_entries.end()) {
			desktop_entry.*member_ptr = it->second;
		}
	}

	return desktop_entry;
}


// Retrieves a localized string value (e.g., Name[en_US]) from a map of key-value pairs,
// following the locale resolution logic based on environment variables.
// Priority: LC_ALL -> LC_MESSAGES -> LANG.
// Fallback: full locale (en_US) -> language only (en) -> unlocalized key.
std::string XDGBasedAppProvider::GetLocalizedValue(const std::unordered_map<std::string, std::string>& kv_entries,
												   const std::string& base_key)
{
	const char* env_vars[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
	for (const auto* var : env_vars) {
		const char* value = getenv(var);
		if (value && *value && std::strlen(value) >= 2) {
			std::string locale(value);
			// Remove character set suffix (e.g., en_US.UTF-8 -> en_US).
			size_t dot_pos = locale.find('.');
			if (dot_pos != std::string::npos) {
				locale = locale.substr(0, dot_pos);
			}
			if (!locale.empty()) {
				// Try full locale (e.g., Name[en_US]).
				auto it = kv_entries.find(base_key + "[" + locale + "]");
				if (it != kv_entries.end()) return it->second;
				// Try language part only (e.g., Name[en]).
				size_t underscore_pos = locale.find('_');
				if (underscore_pos != std::string::npos) {
					std::string lang_only = locale.substr(0, underscore_pos);
					it = kv_entries.find(base_key + "[" + lang_only + "]");
					if (it != kv_entries.end()) return it->second;
				}
			}
		}
	}
	// Fallback to the unlocalized key (e.g., Name=).
	auto it = kv_entries.find(base_key);
	return (it != kv_entries.end()) ? it->second : "";
}


// Loads the MIME alias map (alias -> canonical) from all 'aliases' files.
std::unordered_map<std::string, std::string> XDGBasedAppProvider::LoadMimeAliases()
{
	std::unordered_map<std::string, std::string> alias_to_canonical_map;
	auto mime_db_dirpaths = GetMimeDatabaseSearchDirpaths();

	// Iterate paths from high priority (user) to low priority (system).
	for (const auto& dirpath : mime_db_dirpaths) {
		std::string alias_filepath = dirpath + "/aliases";
		std::ifstream file(alias_filepath);
		if (!file.is_open()) continue;

		std::string line;
		while (std::getline(file, line)) {
			line = XDGBasedAppProvider::Trim(line);
			if (line.empty() || line[0] == '#') continue;

			std::stringstream ss(line);
			std::string alias_mime, canonical_mime;
			if (ss >> alias_mime >> canonical_mime) {
				// Use try_emplace to ensure that only the first-encountered alias
				// (from the highest-priority file) is inserted.
				alias_to_canonical_map.try_emplace(alias_mime, canonical_mime);
			}
		}
	}
	return alias_to_canonical_map;
}


// Returns a std::string_view of the major part of a MIME type (e.g., "image" from "image/png").
// Returns an empty view if the MIME type is malformed.
std::string_view XDGBasedAppProvider::GetMajorMimeType(const std::string& mime)
{
	size_t slash_pos = mime.find('/');
	// Check for invalid formats like "/png", "image", or an empty string.
	if (slash_pos == std::string::npos || slash_pos == 0) {
		return {};
	}
	// Create a view of the major type part.
	return std::string_view(mime.data(), slash_pos);
}


// Loads the MIME type inheritance map (child -> parent) from all 'subclasses' files.
// Files with higher priority (user-specific) override lower-priority (system) ones.
std::unordered_map<std::string, std::string> XDGBasedAppProvider::LoadMimeSubclasses()
{
	std::unordered_map<std::string, std::string> subclass_to_parent_map;
	auto mime_db_dirpaths = GetMimeDatabaseSearchDirpaths();

	// Iterate paths from low priority (system) to high (user).
	// This ensures that user-defined rules in higher-priority files
	// will overwrite the system-wide ones when inserted into the map.
	for (auto it = mime_db_dirpaths.rbegin(); it != mime_db_dirpaths.rend(); ++it) {
		std::string subclasses_filepath = *it + "/subclasses";
		std::ifstream file(subclasses_filepath);
		if (!file.is_open()) continue;

		std::string line;
		while (std::getline(file, line)) {
			line = Trim(line);
			if (line.empty() || line[0] == '#') continue;

			std::stringstream ss(line);
			std::string child_mime, parent_mime;
			if (ss >> child_mime >> parent_mime) {
				subclass_to_parent_map[child_mime] = parent_mime;
			}
		}
	}
	return subclass_to_parent_map;
}


// Returns XDG-compliant search paths for .desktop files, ordered by priority.
std::vector<std::string> XDGBasedAppProvider::GetDesktopFileSearchDirpaths()
{
	std::vector<std::string> dirpaths;
	std::unordered_set<std::string> seen_dirpaths;

	auto add_path = [&](const std::string& p) {
		if (!p.empty() && IsTraversableDirectory(p) && seen_dirpaths.insert(p).second) {
			dirpaths.push_back(p);
		}
	};

	// User-specific data directory ($XDG_DATA_HOME or ~/.local/share) has the highest priority.
	std::string xdg_data_home = GetEnv("XDG_DATA_HOME", "");
	if (!xdg_data_home.empty()) {
		add_path(xdg_data_home + "/applications");
	} else {
		add_path(GetEnv("HOME", "") + "/.local/share/applications");
	}

	// System-wide data directories ($XDG_DATA_DIRS) have lower priority.
	std::string xdg_data_dirs = GetEnv("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
	for (const auto& dir : SplitString(xdg_data_dirs, ':')) {
		if (dir.empty() || dir[0] != '/') continue;
		add_path(dir + "/applications");
	}

	// Additional common paths for Flatpak and Snap applications.
	add_path(GetEnv("HOME", "") + "/.local/share/flatpak/exports/share/applications");
	add_path("/var/lib/flatpak/exports/share/applications");
	add_path("/var/lib/snapd/desktop/applications");

	return dirpaths;
}


// Returns XDG-compliant search paths for mimeapps.list files, ordered by priority.
std::vector<std::string> XDGBasedAppProvider::GetMimeappsListSearchFilepaths()
{
	std::vector<std::string> filepaths;
	std::unordered_set<std::string> seen_filepaths;

	// This helper only adds paths that point to existing, readable files.
	auto add_path = [&](const std::string& p) {
		if (p.empty() || !seen_filepaths.insert(p).second) return;
		if (IsReadableFile(p)) {
			filepaths.push_back(p);
		}
	};


	// User config directory ($XDG_CONFIG_HOME or ~/.config) has the highest priority.
	std::string home = GetEnv("HOME", "");
	std::string xdg_config_home = GetEnv("XDG_CONFIG_HOME", "");
	if (!xdg_config_home.empty() && xdg_config_home[0] == '/') {
		add_path(xdg_config_home + "/mimeapps.list");
	} else if (!home.empty()) {
		add_path(home + "/.config/mimeapps.list");
	}

	// System-wide config directories ($XDG_CONFIG_DIRS) have lower priority.
	std::string xdg_config_dirs = GetEnv("XDG_CONFIG_DIRS", "/etc/xdg");
	for (const auto& dir : SplitString(xdg_config_dirs, ':')) {
		if (dir.empty() || dir[0] != '/') continue;
		add_path(dir + "/mimeapps.list");
	}

	// Data directory mimeapps.list files (user and system) are legacy locations.
	std::string xdg_data_home = GetEnv("XDG_DATA_HOME", "");
	if (!xdg_data_home.empty() && xdg_data_home[0] == '/') {
		add_path(xdg_data_home + "/applications/mimeapps.list");
	} else if (!home.empty()) {
		add_path(home + "/.local/share/applications/mimeapps.list");
	}

	std::string xdg_data_dirs = GetEnv("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
	for (const auto& dir : SplitString(xdg_data_dirs, ':')) {
		if (dir.empty() || dir[0] != '/') continue;
		add_path(dir + "/applications/mimeapps.list");
	}

	return filepaths;
}


// Returns XDG-compliant search paths for MIME database files (aliases, subclasses, etc.)
std::vector<std::string> XDGBasedAppProvider::GetMimeDatabaseSearchDirpaths()
{
	std::vector<std::string> dirpaths;
	std::unordered_set<std::string> seen_dirpaths;

	auto add_path = [&](const std::string& p) {
		if (!p.empty() && IsTraversableDirectory(p) && seen_dirpaths.insert(p).second) {
			dirpaths.push_back(p);
		}
	};

	std::string xdg_data_home = XDGBasedAppProvider::GetEnv("XDG_DATA_HOME", "");
	if (!xdg_data_home.empty()) {
		add_path(xdg_data_home + "/mime");
	} else {
		add_path(XDGBasedAppProvider::GetEnv("HOME", "") + "/.local/share/mime");
	}

	std::string xdg_data_dirs = XDGBasedAppProvider::GetEnv("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
	for (const auto& dir : XDGBasedAppProvider::SplitString(xdg_data_dirs, ':')) {
		add_path(dir + "/mime");
	}
	return dirpaths;
}



// ****************************** Launch command constructing ******************************


// Performs lazy analysis of the Exec string: unescapes GKeyFile sequences, tokenizes,
// and scans for field codes to determine the ExecutionModel.
void XDGBasedAppProvider::AnalyzeExecLine(const DesktopEntry& desktop_entry)
{
	if (desktop_entry.is_exec_parsed) {
		return;
	}

	// 1. Unescape string values (e.g., \s, \n) as per GKeyFile format rules.
	// Note: This must happen *before* parsing arguments and quotes.
	std::string exec_unescaped = UnescapeGKeyFileString(desktop_entry.exec);

	// 2. Tokenize the command line adhering to the specific quoting rules.
	desktop_entry.arg_templates = TokenizeExecString(exec_unescaped);

	// 3. Determine the ExecutionModel by scanning for specific field codes (%f, %F, etc.).
	desktop_entry.execution_model = ExecutionModel::LegacyImplicit;

	for (const auto& arg_template : desktop_entry.arg_templates) {
		// Spec: field codes must not be used inside a quoted argument.
		if (arg_template.is_quoted_literal) continue;

		// Check for FileList codes (%F, %U) which dictate a single process invocation for multiple files.
		if (arg_template.value == "%F" || arg_template.value == "%U") {
			desktop_entry.execution_model = ExecutionModel::FileList;
			break;
		}

		// Check PerFile codes (%f, %u) only if FileList was not detected above.
		size_t pos = 0;
		while ((pos = arg_template.value.find('%', pos)) != std::string::npos) {
			if (pos + 1 < arg_template.value.length()) {
				char next = arg_template.value[pos + 1];
				if (next == 'f' || next == 'u') {
					desktop_entry.execution_model = ExecutionModel::PerFile;
					break;
				}
				// Skip escaped percent "%%" to avoid false positives.
				if (next == '%') pos++;
			}
			pos++;
		}
	}

	desktop_entry.is_exec_parsed = true;
}


// Parses the GKeyFile-unescaped 'Exec' key string into a sequence of argument templates.
std::vector<XDGBasedAppProvider::ArgTemplate> XDGBasedAppProvider::TokenizeExecString(const std::string& exec_value)
{
	if (exec_value.empty()) return {};

	std::vector<ArgTemplate> tokens;
	tokens.reserve(4);

	std::string token_buffer;
	token_buffer.reserve(32);

	bool escape_pending = false;
	bool is_inside_quotes = false;

	// Tracks if the current token involved quotes. This allows distinguishing
	// an explicit empty string ("") from a mere whitespace delimiter.
	bool contains_quoted_part = false;

	auto flush_token = [&]() {
		// Spec: arguments are separated by a space.
		// The check ensures that:
		// 1. Consecutive spaces are treated as a single delimiter (ignored).
		// 2. Explicit empty strings (e.g. "") are preserved as valid empty arguments.
		if (!token_buffer.empty() || contains_quoted_part) {
			tokens.push_back({std::move(token_buffer), contains_quoted_part});
			token_buffer.clear();
			contains_quoted_part = false;
		}
	};

	for (char c : exec_value) {
		if (escape_pending) {
			// Per Spec: inside quotes, backslash acts as an escape only for ` " $ \.
			// Otherwise, it is preserved as a literal char.
			if (is_inside_quotes && (c != '`' && c != '"' && c != '$' && c != '\\')) {
				token_buffer += '\\';
			}
			token_buffer += c;
			escape_pending = false;
			continue;
		}

		if (c == '\\') {
			escape_pending = true;
			continue;
		}

		if (is_inside_quotes) {
			if (c == '"') {
				is_inside_quotes = false;
			} else {
				token_buffer += c;
			}
		} else {
			if (c == '"') {
				is_inside_quotes = true;
				// Mark this token as semantically meaningful, even if empty.
				contains_quoted_part = true;
			} else if (c == ' ') {
				flush_token();
			} else {
				token_buffer += c;
			}
		}
	}

	// Flush the final argument.
	flush_token();

	return tokens;
}


// Constructs a final, shell-safe command line string for a specific invocation.
// This function handles argument expansion and the fallback logic for implicit file passing.
std::string XDGBasedAppProvider::AssembleLaunchCommand(const DesktopEntry& desktop_entry, const std::vector<std::string>& filepaths) const
{
	std::string command_line;

	auto append_argument = [&](const std::string& argument) {
		if (!command_line.empty()) {
			command_line += ' ';
		}
		command_line += argument;
	};

	// 1. Process the explicit command line template from the .desktop Exec key.
	for (const auto& arg_template : desktop_entry.arg_templates) {
		// ExpandArgumentTemplate handles field codes and quoting rules.
		for (const auto& expanded_arg : ExpandArgTemplate(arg_template, filepaths, desktop_entry)) {
			append_argument(expanded_arg);
		}
	}

	// 2. Legacy behavior: If no execution model was deduced from field codes, explicitly append native file paths.
	if (desktop_entry.execution_model == ExecutionModel::LegacyImplicit) {
		for (const auto& filepath : filepaths) {
			append_argument(EscapeArgForShell(filepath));
		}
	}

	return command_line;
}


// Expands a single argument template by substituting field codes with actual data.
std::vector<std::string> XDGBasedAppProvider::ExpandArgTemplate(const ArgTemplate& arg_template, const std::vector<std::string>& filepaths, const DesktopEntry& desktop_entry) const
{
	// Compliance: the spec forbids field code expansion inside quoted arguments. Also fast-path if no '%' is present.
	if (arg_template.is_quoted_literal || arg_template.value.find('%') == std::string::npos) {
		return { EscapeArgForShell(arg_template.value) };
	}

	const std::string& tmpl = arg_template.value;

	// Handle list codes (%F, %U).
	// These expand into multiple separate arguments, one for each file.
	if (tmpl == "%F" || tmpl == "%U") {
		std::vector<std::string> result;
		result.reserve(filepaths.size());
		const bool should_convert_to_uri = (tmpl == "%U" && !_treat_urls_as_paths);
		for (const auto& filepath : filepaths) {
			result.push_back(EscapeArgForShell(should_convert_to_uri ? PathToUri(filepath) : filepath));
		}
		return result;
	}

	// Handle other codes (%f, %u, %c, etc.).
	// These expand into a single argument string, potentially concatenating data.

	const std::string context_filepath = filepaths.empty() ? std::string() : filepaths.front();
	std::string expanded_token;

	// Reserve buffer space to minimize reallocations.
	expanded_token.reserve(tmpl.size() + 64);

	for (size_t i = 0; i < tmpl.size(); ++i) {
		if (tmpl[i] != '%' || i + 1 >= tmpl.size()) {
			expanded_token += tmpl[i];
			continue;
		}

		char code = tmpl[++i]; // advance to the character following '%'

		switch (code) {
		case '%':
			// "%%" expands to a literal "%".
			expanded_token += '%';
			break;

		case 'f':
			expanded_token += context_filepath;
			break;

		case 'u':
			expanded_token += (_treat_urls_as_paths ? context_filepath : PathToUri(context_filepath));
			break;

		case 'c':
			expanded_token += UnescapeGKeyFileString(desktop_entry.name);
			break;

		case 'k':
			expanded_token += desktop_entry.desktop_filepath;
			break;

		case 'i':
			// The %i code is supposed to expand to two arguments: "--icon" and the Icon key value.
			// We don't support icons, so we remove the entire argument.
			return {};

		case 'd': case 'D': case 'n': case 'N': case 'v': case 'm':
			// Deprecated codes: the spec states these should be removed and ignored.
			break;

		default:
			// Deviation from Spec: unknown codes are preserved as literals rather than
			// invalidating the command (for robustness).
			expanded_token += '%';
			expanded_token += code;
			break;
		}
	}

	if (expanded_token.empty()) {
		return {};
	}

	// Escape the final expanded token to ensure shell safety.
	return { EscapeArgForShell(expanded_token) };
}


// Converts absolute local filepath to a file:// URI.
std::string XDGBasedAppProvider::PathToUri(const std::string& path)
{
	if (path.empty() || path[0] != '/') {
		return {};
	}

	static constexpr char hex_digits[] = "0123456789ABCDEF";
	std::string uri;
	uri.reserve(7 + path.size() * 3 / 2);
	uri.append("file://");
	for (const unsigned char c : path) {
		// Perform a locale-independent check for unreserved characters (RFC 3986) plus the path separator '/'.
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
			|| c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
			uri.push_back(static_cast<char>(c));
		} else {
			// Percent-encode all other characters.
			uri.push_back('%');
			uri.push_back(hex_digits[(c >> 4) & 0xF]);
			uri.push_back(hex_digits[c & 0xF]);
		}
	}
	return uri;
}


// Performs the first-pass un-escaping for a raw string from a .desktop file.
// This handles general GKeyFile-style escape sequences like '\\' -> '\', '\s' -> ' ', etc.
std::string XDGBasedAppProvider::UnescapeGKeyFileString(const std::string& str)
{
	std::string unescaped;
	unescaped.reserve(str.length());

	for (size_t i = 0; i < str.length(); ++i) {
		if (str[i] == '\\') {
			if (i + 1 < str.length()) {
				i++; // move to the character after the backslash
				switch (str[i]) {
				case 's': unescaped += ' '; break;
				case 'n': unescaped += '\n'; break;
				case 't': unescaped += '\t'; break;
				case 'r': unescaped += '\r'; break;
				case '\\': unescaped += '\\'; break;
				default:
					// For unknown escape sequences, the backslash is dropped
					// and the character is preserved.
					unescaped += str[i];
					break;
				}
			} else {
				// A trailing backslash at the end of the string is treated as a literal.
				unescaped += '\\';
			}
		} else {
			unescaped += str[i];
		}
	}
	return unescaped;
}



// ****************************** System & Environment Helpers ******************************


// Checks if an executable exists and is runnable.
// If the command contains a slash, it's checked directly. Otherwise, it's searched in $PATH.
bool XDGBasedAppProvider::IsExecutableAvailable(const std::string& command)
{
	if (command.empty()) return false;

	auto check = [](const std::string& p) {
		struct stat st;
		return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0;
	};

	if (command.find('/') != std::string::npos) {
		return check(command);
	}

	if (const char* env_path = getenv("PATH")) {
		std::istringstream ss(env_path);
		std::string dir;
		while (std::getline(ss, dir, ':')) {
			if (!dir.empty() && check(dir + '/' + command)) {
				return true;
			}
		}
	}

	return false;
}


// Safe environment variable access with an optional default value.
std::string XDGBasedAppProvider::GetEnv(const char* var, const char* default_val)
{
	const char* val = getenv(var);
	return val ? val : default_val;
}


// Runs a shell command and captures its standard output.
std::string XDGBasedAppProvider::RunCommandAndCaptureOutput(const std::string& cmd)
{
	std::string result;
	return POpen(result, cmd.c_str()) ? Trim(result) : "";
}



// ****************************** Common helper functions ******************************

// Trims whitespace from both ends of a string.
std::string XDGBasedAppProvider::Trim(std::string str)
{
	str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) { return !std::isspace(ch); }));
	str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), str.end());
	return str;
}


// String splitting utility with trimming of individual tokens.
std::vector<std::string> XDGBasedAppProvider::SplitString(const std::string& str, char delimiter)
{
	if (str.empty()) return {};

	std::vector<std::string> tokens;
	std::istringstream stream(str);
	std::string token;

	while (std::getline(stream, token, delimiter)) {
		if (auto trimmed = Trim(token); !trimmed.empty()) {
			tokens.push_back(std::move(trimmed));
		}
	}
	return tokens;
}


// Escapes a single command-line argument for safe execution by the shell.
std::string XDGBasedAppProvider::EscapeArgForShell(const std::string& arg)
{
	if (arg.empty()) {
		return "''"; // An empty string is represented as '' in the shell.
	}

	bool is_safe = true;
	for (const unsigned char c : arg) {
		// Perform a locale-independent check for 7-bit ASCII characters.
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
			  || c == '/' || c == '.' || c == '_' || c == '-'))
		{
			is_safe = false;
			break;
		}
	}

	if (is_safe) {
		return arg;
	}

	std::string out;
	out.push_back('\'');
	for (const unsigned char uc : arg) {
		if (uc == '\'') {
			// Single quotes cannot be nested. Close the string, add an escaped literal quote,
			// and reopen. Ex: "it's" -> 'it'\''s'.
			out.append("'\\''");
		} else {
			out.push_back(static_cast<char>(uc));
		}
	}
	out.push_back('\'');
	return out;
}


// Extracts the basename (filename) from a full filepath.
std::string XDGBasedAppProvider::GetBaseName(const std::string& filepath)
{
	size_t pos = filepath.find_last_of('/');
	if (pos == std::string::npos) {
		return filepath;
	}
	return filepath.substr(pos + 1);
}


// Checks if a filepath is a regular file (S_ISREG) AND is readable (R_OK).
bool XDGBasedAppProvider::IsReadableFile(const std::string& filepath)
{
	struct stat st;
	// Use stat() to follow symlinks
	if (stat(filepath.c_str(), &st) == 0) {
		// Check type first, then check permissions
		if (S_ISREG(st.st_mode)) {
			return (access(filepath.c_str(), R_OK) == 0);
		}
	}
	return false;
}


// Checks if a dirpath is a directory (S_ISDIR) AND is traversable (X_OK).
bool XDGBasedAppProvider::IsTraversableDirectory(const std::string& dirpath)
{
	struct stat st;
	// Use stat() to follow symlinks
	if (stat(dirpath.c_str(), &st) == 0) {
		// Check type first, then check permissions
		if (S_ISDIR(st.st_mode)) {
			return (access(dirpath.c_str(), X_OK) == 0);
		}
	}
	return false;
}


// ****************************** RAII cache helper ******************************

// Constructor for the RAII helper. This populates all operation-scoped class fields.
XDGBasedAppProvider::OperationContext::OperationContext(XDGBasedAppProvider& p) : provider(p)
{
	// 1. Load MIME-related databases

	if (provider._load_mimetype_aliases) {
		// Pass 1: Build the forward (alias -> canonical) map.
		provider._op_alias_to_canonical_map = provider.LoadMimeAliases();

		// Pass 2: Build the reverse (canonical -> aliases) map.
		provider._op_canonical_to_aliases_map.emplace();
		auto& reverse_map = provider._op_canonical_to_aliases_map.value();

		for (const auto& [alias, canonical] : provider._op_alias_to_canonical_map.value()) {
			auto alias_major = GetMajorMimeType(alias);
			auto canonical_major = GetMajorMimeType(canonical);
			// Add the alias ONLY if its major type matches the canonical one.
			// This prevents adding, for example, "text/ico" for "image/vnd.microsoft.icon",
			// but allows adding "image/x-icon".
			if (!alias_major.empty() && alias_major == canonical_major) {
				reverse_map[canonical].push_back(alias);
			}
		}
	}

	if (provider._load_mimetype_subclasses) {
		provider._op_subclass_to_parent_map = provider.LoadMimeSubclasses();
	}

	// 2. Load system paths and association files
	provider._op_desktop_file_dirpaths = provider.GetDesktopFileSearchDirpaths();
	auto mimeapps_filepaths = provider.GetMimeappsListSearchFilepaths();
	provider._op_mimeapps_lists_data = provider.ParseMimeappsLists(mimeapps_filepaths);
	provider._op_current_desktop_env = provider._filter_by_show_in ? provider.GetEnv("XDG_CURRENT_DESKTOP", "") : "";

	// 3. Check for external tool availability *once* for this operation.
	// This sets the operation-scoped flags for use by MimeTypeFrom... functions.
	provider._op_xdg_mime_exists = provider.IsExecutableAvailable("xdg-mime");
	provider._op_file_tool_enabled_and_exists = provider._use_file_tool && provider.IsExecutableAvailable("file");
	provider._op_magika_tool_enabled_and_exists = provider._use_magika_tool && provider.IsExecutableAvailable("magika");

	provider._op_default_app_cache.clear();

	// 4. Build the primary application lookup cache
	// We build *either* the mimeinfo.cache or the full mime-to-app index, based on settings.
	if (provider._use_mimeinfo_cache) {
		// Attempt to load from mimeinfo.cache first, as per user setting.
		provider._op_mime_to_handlers_map = XDGBasedAppProvider::ParseAllMimeinfoCacheFiles(provider._op_desktop_file_dirpaths.value());

		// Fallback: If the cache is empty (files not found or all are empty),
		// reset the optional. This will trigger the full scan logic below.
		if (provider._op_mime_to_handlers_map.value().empty()) {
			provider._op_mime_to_handlers_map.reset();
		}
	}

	// If caching is disabled (by user) OR the fallback was triggered (cache was empty)...
	if (!provider._op_mime_to_handlers_map.has_value()) {
		// ...populate the index by performing a full scan.
		// This call will populate the _desktop_entry_cache as a side effect.
		provider._op_mime_to_desktop_entry_map = provider.FullScanDesktopFiles(provider._op_desktop_file_dirpaths.value());
	}
}


// Destructor for the RAII helper. This clears all operation-scoped class fields.
XDGBasedAppProvider::OperationContext::~OperationContext()
{
	provider._op_alias_to_canonical_map.reset();
	provider._op_canonical_to_aliases_map.reset();
	provider._op_subclass_to_parent_map.reset();
	provider._op_mimeapps_lists_data.reset();
	provider._op_desktop_file_dirpaths.reset();
	provider._op_current_desktop_env.reset();
	provider._op_mime_to_handlers_map.reset();
	provider._op_mime_to_desktop_entry_map.reset();

	provider._op_default_app_cache.clear();

	// Reset the operation-scoped tool availability flags
	provider._op_xdg_mime_exists = false;
	provider._op_file_tool_enabled_and_exists = false;
	provider._op_magika_tool_enabled_and_exists = false;
}

#endif
