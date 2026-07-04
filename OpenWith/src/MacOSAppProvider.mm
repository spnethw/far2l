#if defined(__APPLE__)

#include <AvailabilityMacros.h>

// If the compiler (e.g., GNU GCC) does not support Clang feature checking macros,
// safely define them to 0 to fallback to classic Objective-C.
#ifndef __has_feature
	#define __has_feature(x) 0
#endif
#ifndef __has_extension
	#define __has_extension __has_feature
#endif

#import <Cocoa/Cocoa.h>
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

#include "MacOSAppProvider.hpp"
#include "common.hpp"
#include "lng.hpp"
#include "utils.h"
#include "WideMB.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
#ifndef __clang__
	// GCC/MRC: @autoreleasepool does not drain on C++ exceptions.
	// This RAII wrapper ensures the pool is always drained, even when
	// OperationCancelledException propagates through the loop body.
	struct AutoreleasePoolGuard
	{
		NSAutoreleasePool *pool;
		AutoreleasePoolGuard() : pool([[NSAutoreleasePool alloc] init]) {}
		~AutoreleasePoolGuard() { [pool drain]; }
		AutoreleasePoolGuard(const AutoreleasePoolGuard&) = delete;
		AutoreleasePoolGuard& operator=(const AutoreleasePoolGuard&) = delete;
	};
#endif

	struct AppBundleMetadata
	{
		std::wstring name;
		std::wstring id;             // The full path to the .app bundle, used as a unique identifier.
		std::wstring version_string; // Used for disambiguation if names conflict.
	};

	std::string NSURLToPath(NSURL *url)
	{
		if (!url) return {};
		return std::string([[url path] UTF8String]);
	}

	AppBundleMetadata ParseAppBundleMetadata(NSURL *app_url)
	{
		AppBundleMetadata metadata;
		NSBundle *bundle = [NSBundle bundleWithURL:app_url];
		NSDictionary *info_dict = [bundle infoDictionary];

		// Prefer the display name, but fall back to the filename if it's not available.
		NSString *bundle_name = [info_dict objectForKey:@"CFBundleDisplayName"] ?: [info_dict objectForKey:@"CFBundleName"];

		// Get version strings. Prefer the short, user-facing version.
		NSString *bundle_short_version = [info_dict objectForKey:@"CFBundleShortVersionString"];
		NSString *bundle_version = [info_dict objectForKey:@"CFBundleVersion"];

		metadata.name = StrMB2Wide(bundle_name ? [bundle_name UTF8String] : NSURLToPath(app_url));
		metadata.id = StrMB2Wide(NSURLToPath(app_url));

		// Store the most descriptive version string available for disambiguation.
		if (bundle_short_version) {
			metadata.version_string = StrMB2Wide([bundle_short_version UTF8String]);
		} else if (bundle_version) {
			metadata.version_string = StrMB2Wide([bundle_version UTF8String]);
		}

		return metadata;
	}

	std::wstring EscapeForShell(const std::wstring& arg)
	{
		std::wstring out;
		out.push_back(L'\'');
		for (wchar_t c : arg) {
			if (c == L'\'') {
				out.append(L"'\\''");
			} else {
				out.push_back(c);
			}
		}
		out.push_back(L'\'');
		return out;
	}
}


namespace openwith
{
	MacOSAppProvider::MacOSAppProvider() = default;

	// Find application candidates that can open all specified files.
	// The logic uses a scoring system to rank candidates.
	// The default application for a file type receives a higher score, ensuring it appears first in the list.
	// To optimize performance for large file selections, parsed application metadata is cached locally
	// per-invocation based on the file's Uniform Type Identifier (UTI).

	AppProvider::GetCandidatesResult MacOSAppProvider::GetAppCandidates(const std::vector<std::wstring>& filepaths, ProgressCallback progress, const std::atomic<bool>* cancel_flag)
	{
		_last_uti_profiles.clear();

		GetCandidatesResult result;
		if (filepaths.empty()) {
			return result;
		}

		OperationGuard guard(*this, std::move(progress), cancel_flag);

#ifdef __clang__
		@autoreleasepool {
#else
		{
			AutoreleasePoolGuard outer_pool_guard;
#endif
			try {
				// --- Part 1: Candidate discovery and scoring with caching ---

				struct RankedCandidate
				{
					AppBundleMetadata metadata;
					int score = 0;
					int match_count = 0;
					bool operator<(const RankedCandidate& other) const
					{
						if (score != other.score) return score > other.score;
						return metadata.name < other.metadata.name;
					}
				};
				std::unordered_map<std::wstring, RankedCandidate> candidates_pool;
				constexpr int DEFAULT_APP_SCORE = 10;
				constexpr int OTHER_APP_SCORE   = 1;

				struct UtiAppList
				{
					std::optional<AppBundleMetadata> default_app_metadata;
					std::vector<AppBundleMetadata> compatible_apps_metadata;
				};
				std::unordered_map<std::string, UtiAppList> uti_to_apps_cache;

				ReportProgress({GetMsg(MsgID::DiscoveringApplications), nullptr});

				const size_t total = filepaths.size();
				size_t processed = 0;

				// Iterate through each selected file to find and score compatible applications.
				for (const auto& filepath : filepaths) {
					wchar_t status_buf[256];
					swprintf(status_buf, std::size(status_buf), GetMsg(MsgID::ProcessingFiles), ++processed, total);
					ReportProgress({nullptr, status_buf});
					CheckCancellation();

#ifdef __clang__
					@autoreleasepool {
#else
					{
						AutoreleasePoolGuard inner_pool_guard;
#endif
						NSString *path = [NSString stringWithUTF8String:StrWide2MB(filepath).c_str()];
						NSURL *file_url = [NSURL fileURLWithPath:path];

						if (!file_url) {
							_last_uti_profiles.insert({ std::string(""), false });
							continue;
						}

						// Determine the file's UTI to use it as a cache key.
						NSString *uti = nil;
						NSError *error = nil;
						BOOL success = [file_url getResourceValue:&uti forKey:NSURLTypeIdentifierKey error:&error];
						if (!success) {
						    _last_uti_profiles.insert({ "", false });
						    continue;
						}
						if (!uti) {
							_last_uti_profiles.insert({ "", true });
							continue;
						}
						auto uti_std_str = std::string([uti UTF8String]);
						_last_uti_profiles.insert({uti_std_str, true});

						// Fetch application lists from local UTI cache, falling back to system query on miss.
						auto cache_it = uti_to_apps_cache.find(uti_std_str);
						if (cache_it == uti_to_apps_cache.end()) {
							UtiAppList new_cache_entry;

							NSURL* default_app_url = [[NSWorkspace sharedWorkspace] URLForApplicationToOpenURL:file_url];
							if (default_app_url) {
								new_cache_entry.default_app_metadata = ParseAppBundleMetadata(default_app_url);
							}

#if __has_feature(objc_generics)
							NSArray<NSURL *>* all_app_urls;
#else
							NSArray *all_app_urls;
#endif

#if MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
							all_app_urls = [[NSWorkspace sharedWorkspace] URLsForApplicationsToOpenURL:file_url];
#elif __has_feature(objc_array_literals)
							all_app_urls = default_app_url ? @[default_app_url] : @[];
#else
							if (default_app_url) {
								all_app_urls = [NSArray arrayWithObject:default_app_url];
							} else {
								all_app_urls = [NSArray array];
							}
#endif

#ifdef __clang__
							for (NSURL *app_url in all_app_urls) {
#else
							for (NSUInteger i = 0; i < [all_app_urls count]; i++) {
								NSURL *app_url = [all_app_urls objectAtIndex:i];
#endif
								new_cache_entry.compatible_apps_metadata.push_back(ParseAppBundleMetadata(app_url));
							}

							cache_it = uti_to_apps_cache.insert({uti_std_str, std::move(new_cache_entry)}).first;
						}

						std::unordered_set<std::wstring> processed_app_ids;

						const UtiAppList& uti_app_list = cache_it->second;

						if (uti_app_list.default_app_metadata) {
							const auto& metadata = *uti_app_list.default_app_metadata;
							auto [it, inserted] = candidates_pool.try_emplace(metadata.id);
							RankedCandidate& ranked_candidate = it->second;
							if (inserted) {
								ranked_candidate.metadata = metadata;
							}
							ranked_candidate.score += DEFAULT_APP_SCORE;
							ranked_candidate.match_count++;
							processed_app_ids.insert(metadata.id);
						}

						for (const auto& metadata : uti_app_list.compatible_apps_metadata) {
							if (processed_app_ids.count(metadata.id)) {
								continue;
							}
							auto [it, inserted] = candidates_pool.try_emplace(metadata.id);
							RankedCandidate& ranked_candidate = it->second;
							if (inserted) {
								ranked_candidate.metadata = metadata;
							}
							ranked_candidate.score += OTHER_APP_SCORE;
							ranked_candidate.match_count++;
							processed_app_ids.insert(metadata.id);
						}

#ifdef __clang__
					} // end of inner @autoreleasepool
#else
					} // end of inner AutoreleasePoolGuard
#endif
				}

				ReportProgress({nullptr, GetMsg(MsgID::MatchingFilteringRanking)});

				// --- Part 2: Filtering and sorting ---

				std::vector<RankedCandidate> ranked_finalists;
				const size_t num_files = filepaths.size();

				// Filter the list, keeping only applications that can open every selected file.
				for (auto& [app_id, ranked_candidate] : candidates_pool) {
					if (ranked_candidate.match_count == num_files) {
						ranked_finalists.push_back(std::move(ranked_candidate));
					}
				}

				std::sort(ranked_finalists.begin(), ranked_finalists.end());

				// --- Part 3: Final list generation ---

				std::vector<CandidateInfo> out_candidates;
				if (!ranked_finalists.empty()) {
					out_candidates.reserve(ranked_finalists.size());

					// Count name occurrences to identify duplicates that need disambiguation.
					std::unordered_map<std::wstring, int> name_counts;
					for (const auto& ranked_finalist : ranked_finalists) {
						name_counts[ranked_finalist.metadata.name]++;
					}

					// Build the final list in the output format.
					for (const auto& ranked_finalist : ranked_finalists) {
						CandidateInfo out_candidate;
						out_candidate.id = ranked_finalist.metadata.id;
						out_candidate.terminal = false;
						out_candidate.name = ranked_finalist.metadata.name;
						out_candidate.multi_file_aware = true;

						// If an app name is duplicated, append its version string to make it unique in the UI.
						if (name_counts[ranked_finalist.metadata.name] > 1 && !ranked_finalist.metadata.version_string.empty()) {
							out_candidate.name += L" (" + ranked_finalist.metadata.version_string + L")";
						}
						out_candidates.push_back(out_candidate);
					}
				}

				// Populate candidates on normal successful exit
				result.candidates = std::move(out_candidates);

			} catch (const OperationCancelledException&) {
				result.was_cancelled = true;
				_last_uti_profiles.clear();
			}
#ifdef __clang__
		} // end of outer @autoreleasepool
#else
		} // end of outer AutoreleasePoolGuard
#endif

		return result;
	}


	// Constructs a single command line using the 'open' utility, which natively handles multiple files.
	std::vector<std::wstring> MacOSAppProvider::ConstructLaunchCommands(const CandidateInfo& candidate, const std::vector<std::wstring>& filepaths)
	{
		if (candidate.id.empty() || filepaths.empty()) {
			return {};
		}

		// The 'open -a <app_path>' command tells the system to open files with a specific application.
		std::wstring cmd = L"open -a " + EscapeForShell(candidate.id);
		for (const auto& filepath : filepaths) {
			cmd += L" " + EscapeForShell(filepath);
		}

		return {cmd};
	}


	// Fetches detailed information about a candidate application from its bundle.
	std::vector<Field> MacOSAppProvider::GetCandidateDetails(const CandidateInfo& candidate)
	{
		std::vector<Field> details;
#ifdef __clang__
		@autoreleasepool {
#else
		{
			AutoreleasePoolGuard pool_guard;
#endif
			NSString *ns_path = [NSString stringWithUTF8String:StrWide2MB(candidate.id).c_str()];
			NSURL *app_url = [NSURL fileURLWithPath:ns_path];
			if (!app_url) {
				return details;
			}

			NSBundle *bundle = [NSBundle bundleWithURL:app_url];
			if (!bundle) {
				return details;
			}

			NSDictionary *info_dict = [bundle infoDictionary];

			NSString *app_name = [info_dict objectForKey:@"CFBundleDisplayName"] ?: [info_dict objectForKey:@"CFBundleName"];
			if (app_name) {
				details.push_back({GetMsg(MsgID::AppName), StrMB2Wide([app_name UTF8String])});
			}

			details.push_back({GetMsg(MsgID::FullPath), candidate.id});

			NSString *exec_name = [info_dict objectForKey:@"CFBundleExecutable"];
			if (exec_name) {
				details.push_back({GetMsg(MsgID::ExecutableFile), StrMB2Wide([exec_name UTF8String])});
			}

			NSString *bundle_short_version = [info_dict objectForKey:@"CFBundleShortVersionString"];
			if (bundle_short_version) {
				details.push_back({GetMsg(MsgID::Version), StrMB2Wide([bundle_short_version UTF8String])});
			}

			NSString *bundle_version = [info_dict objectForKey:@"CFBundleVersion"];
			if (bundle_version) {
				details.push_back({GetMsg(MsgID::BundleVersion), StrMB2Wide([bundle_version UTF8String])});
			}
#ifdef __clang__
		} // end of @autoreleasepool
#else
		} // end of AutoreleasePoolGuard
#endif
		return details;
	}


	// Collects unique formatted profile strings based on the last GetAppCandidates call.
	// This function performs the UTI-to-MIME-type conversion on demand.
	std::vector<std::wstring> MacOSAppProvider::GetMimeTypes()
	{
		std::unordered_set<std::wstring> unique_profile_strings;
		unique_profile_strings.reserve(_last_uti_profiles.size());

#ifdef __clang__
		@autoreleasepool {
#else
		{
			AutoreleasePoolGuard pool_guard;
#endif
			for (const auto& profile : _last_uti_profiles) {
				if (!profile.accessible) {
					unique_profile_strings.insert(L"(inaccessible)");
					continue;
				}

				// File was accessible; convert its recorded UTI to a MIME type.
				std::wstring out_mime_wstr;
				NSString *uti = [NSString stringWithUTF8String:profile.uti.c_str()];

				// Use the appropriate API based on the target macOS version.
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 110000 // UTType is available on macOS 11.0+
				// Modern approach for macOS 11.0 and later, converting a UTI to a MIME type.
				UTType *type = [UTType typeWithIdentifier:uti];
				if (type) {
					NSString *ns_mime_str = type.preferredMIMEType;
					if (ns_mime_str) out_mime_wstr = StrMB2Wide([ns_mime_str UTF8String]);
				}
#else
				// Legacy approach for older macOS versions.
#if __has_feature(objc_arc)
				CFStringRef cf_mime_tag = UTTypeCopyPreferredTagWithClass((__bridge CFStringRef)uti,
																	   kUTTagClassMIMEType);
				if (cf_mime_tag) {
					// Transfer ownership of the CFStringRef to ARC.
					NSString *ns_mime_str = (__bridge_transfer NSString *)cf_mime_tag;
					out_mime_wstr = StrMB2Wide([ns_mime_str UTF8String]);
				}
#else // Manual Retain-Release (MRC) mode or GCC.
				CFStringRef cf_mime_tag = UTTypeCopyPreferredTagWithClass((CFStringRef)uti,
																	   kUTTagClassMIMEType);
				if (cf_mime_tag) {
					NSString *ns_mime_str = [(NSString *)cf_mime_tag autorelease];
					out_mime_wstr = StrMB2Wide([ns_mime_str UTF8String]);
				}
#endif
#endif

				if (out_mime_wstr.empty()) {
					unique_profile_strings.insert(L"(none)");
				} else {
					unique_profile_strings.insert(L"(" + out_mime_wstr + L")");
				}
			}
#ifdef __clang__
		} // end of @autoreleasepool
#else
		} // end of AutoreleasePoolGuard
#endif

		std::vector<std::wstring> result_vec(unique_profile_strings.begin(), unique_profile_strings.end());
		return result_vec;
	}
} // namespace openwith
#endif // __APPLE__
