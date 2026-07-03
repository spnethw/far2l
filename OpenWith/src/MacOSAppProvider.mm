#if defined(__APPLE__)

#include <AvailabilityMacros.h>
#import <Cocoa/Cocoa.h>
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif
#include "MacOSAppProvider.hpp"
#include "lng.hpp"
#include "WideMB.h"
#include "common.hpp"
#include "utils.h"
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
	struct AutoreleasePoolGuard {
		NSAutoreleasePool *pool;
		AutoreleasePoolGuard() : pool([[NSAutoreleasePool alloc] init]) {}
		~AutoreleasePoolGuard() { [pool drain]; }
		AutoreleasePoolGuard(const AutoreleasePoolGuard&) = delete;
		AutoreleasePoolGuard& operator=(const AutoreleasePoolGuard&) = delete;
	};
#endif

	// A temporary structure to hold application details fetched from the system
	// before they are processed into the final CandidateInfo format.
	struct MacCandidateTempInfo
	{
		std::wstring name;
		std::wstring id;   // The full path to the .app bundle, used as a unique identifier.
		std::wstring info; // Version string used for disambiguation if names conflict.
	};

	// Helper to convert an NSURL object to a UTF-8 encoded std::string path.
	std::string NSURLToPath(NSURL *url)
	{
		if (!url) return {};
		return std::string([[url path] UTF8String]);
	}

	// Helper to extract essential application metadata from its bundle.
	MacCandidateTempInfo AppBundleToTempInfo(NSURL *appURL)
	{
		MacCandidateTempInfo c;
		NSBundle *bundle = [NSBundle bundleWithURL:appURL];
		NSDictionary *infoDict = [bundle infoDictionary];

		// Prefer the display name, but fall back to the filename if it's not available.
		NSString *bundleName = [infoDict objectForKey:@"CFBundleDisplayName"] ?: [infoDict objectForKey:@"CFBundleName"];

		// Get version strings. Prefer the short, user-facing version.
		NSString *bundleShortVersion = [infoDict objectForKey:@"CFBundleShortVersionString"];
		NSString *bundleVersion = [infoDict objectForKey:@"CFBundleVersion"];

		c.name = StrMB2Wide(bundleName ? [bundleName UTF8String] : NSURLToPath(appURL));
		c.id = StrMB2Wide(NSURLToPath(appURL));

		// Store the most descriptive version string available for disambiguation.
		if (bundleShortVersion) {
			c.info = StrMB2Wide([bundleShortVersion UTF8String]);
		} else if (bundleVersion) {
			c.info = StrMB2Wide([bundleVersion UTF8String]);
		}

		return c;
	}

	std::wstring EscapeForShell(const std::wstring& arg)
	{
		std::wstring out;
		out.push_back(L'\'');
		for (wchar_t c : arg) {
			if (c == L'\'') {
				// A single quote is escaped by closing the quote, adding an escaped quote,
				// and then re-opening the quote (e.g., 'it's' becomes 'it'\''s').
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
	// To optimize performance when handling many files, this function caches the application lists
	// based on the file's Uniform Type Identifier (UTI).
	AppProvider::GetCandidatesResult MacOSAppProvider::GetAppCandidates(const std::vector<std::wstring>& filepaths, ProgressCallback progress, const std::atomic<bool>* cancel_flag)
	{
		// Purge state from previous invocations. As a long-lived singleton instance,
		// clearing the cache prevents cross-pollination of UTI profiles.
		_last_uti_profiles.clear();

		GetCandidatesResult final_result;

		if (filepaths.empty()) {
			return final_result;
		}

		OperationGuard guard(*this, std::move(progress), cancel_flag);

#ifdef __clang__
		@autoreleasepool {
#else
		{
			AutoreleasePoolGuard outer_pool_guard;
#endif
			try {
				// --- Part 1: Candidate Discovery and Scoring with Caching ---

				// A map to store definitive metadata for every unique app encountered.
				// Key: application ID (full path), Value: application metadata.
				std::unordered_map<std::wstring, MacCandidateTempInfo> all_apps_info;

				// A map to accumulate scores for each candidate application.
				std::unordered_map<std::wstring, int> app_scores;

				// A map to count how many of the selected files each application can open.
				std::unordered_map<std::wstring, int> app_occurrence_count;

				// Base scoring weights: Default applications receive a dominant multiplier (10) to enforce priority
				// ranking over generic handlers, even in multi-file selections.
				constexpr int DEFAULT_APP_SCORE = 10;
				constexpr int OTHER_APP_SCORE   = 1;

				// A cache to store the list of applications for a given Uniform Type Identifier (UTI).
				// This dramatically speeds up processing when many files of the same type are selected.
				struct CachedAppList
				{
					std::optional<MacCandidateTempInfo> default_app;
					std::vector<MacCandidateTempInfo> all_apps;
				};
				std::unordered_map<std::string, CachedAppList> uti_cache;

				ReportProgress({GetMsg(MsgID::DiscoveringApplications), nullptr});

				size_t file_index = 0;
				const size_t total_files = filepaths.size();

				// Iterate through each selected file to find and score compatible applications.
				for (const auto& filepath : filepaths) {
					CheckCancellation();

					file_index++;
					wchar_t status_msg[256];
					swprintf(status_msg, std::size(status_msg), GetMsg(MsgID::ProcessingFiles), file_index, total_files);
					ReportProgress({nullptr, status_msg});

#ifdef __clang__
					@autoreleasepool {
#else
					{
						AutoreleasePoolGuard inner_pool_guard;
#endif
						NSString *path = [NSString stringWithUTF8String:StrWide2MB(filepath).c_str()];
						NSURL *fileURL = [NSURL fileURLWithPath:path];

						if (!fileURL) {
							_last_uti_profiles.insert({ std::string(""), false });
							continue;
						}

						// Determine the file's UTI to use it as a cache key.
						NSString *uti = nil;
						NSError *error = nil;
						[fileURL getResourceValue:&uti forKey:NSURLTypeIdentifierKey error:&error];

						const char* uti_cstr = (uti != nil) ? [uti UTF8String] : "";
						std::string uti_std_str = uti_cstr ? uti_cstr : "";

						if (error || !uti) {
							// Failed to get UTI (e.g., file not found, permissions), cache as inaccessible
							_last_uti_profiles.insert({ uti_std_str, false });
							continue;
						}

						// --- Begin: Profile Caching Logic ---
						// Cache the file profile (UTI and accessible status).
						_last_uti_profiles.insert({ uti_std_str, true });
						// --- End: Profile Caching Logic ---

						// Fetch application lists from local UTI cache, falling back to system query on miss.
						auto cache_it = uti_cache.find(uti_std_str);
						if (cache_it == uti_cache.end()) {
							CachedAppList entry;

							NSURL* defaultAppURL = [[NSWorkspace sharedWorkspace] URLForApplicationToOpenURL:fileURL];
							if (defaultAppURL) {
								entry.default_app = AppBundleToTempInfo(defaultAppURL);
							}

#if MAC_OS_X_VERSION_MAX_ALLOWED >= 101000 && defined(__clang__)
							NSArray<NSURL *>* allAppURLs;
#else
							NSArray *allAppURLs;
#endif

#if MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
							allAppURLs = [[NSWorkspace sharedWorkspace] URLsForApplicationsToOpenURL:fileURL];
#elif MAC_OS_X_VERSION_MAX_ALLOWED >= 1080 && defined(__clang__)
							allAppURLs = defaultAppURL ? @[defaultAppURL] : @[];
#else
							if (defaultAppURL) {
								allAppURLs = [NSArray arrayWithObject:defaultAppURL];
							} else {
								allAppURLs = [NSArray array];
							}
#endif

#ifdef __clang__
							for (NSURL *appURL in allAppURLs) {
#else
							for (NSUInteger i = 0; i < [allAppURLs count]; i++) {
								NSURL *appURL = [allAppURLs objectAtIndex:i];
#endif
								entry.all_apps.push_back(AppBundleToTempInfo(appURL));
							}

							cache_it = uti_cache.insert({uti_std_str, std::move(entry)}).first;
						}

						std::unordered_set<std::wstring> processed_apps_for_this_file;

						// Process the default application.
						if (cache_it->second.default_app) {
							const auto& info = *cache_it->second.default_app;
							all_apps_info.try_emplace(info.id, info);
							app_scores[info.id] += DEFAULT_APP_SCORE;
							processed_apps_for_this_file.insert(info.id);
							app_occurrence_count[info.id]++;
						}

						// Process all other compatible applications.
						for (const auto& info : cache_it->second.all_apps) {
							if (processed_apps_for_this_file.count(info.id)) {
								continue;
							}
							all_apps_info.try_emplace(info.id, info);
							app_scores[info.id] += OTHER_APP_SCORE;
							processed_apps_for_this_file.insert(info.id);
							app_occurrence_count[info.id]++;
						}

#ifdef __clang__
					} // end of inner @autoreleasepool
#else
					} // end of inner AutoreleasePoolGuard
#endif
				}

				ReportProgress({nullptr, GetMsg(MsgID::MatchingFilteringRanking)});

				// --- Part 2: Filtering and Sorting ---

				// A temporary structure to hold candidates that can open all files, along with their final score.
				struct RankedCandidate
				{
					MacCandidateTempInfo info;
					int score;
					bool operator<(const RankedCandidate& other) const
					{
						if (score != other.score) return score > other.score;
						return info.name < other.info.name;
					}
				};

				std::vector<RankedCandidate> finalists;
				const size_t num_files = filepaths.size();

				// Filter the list, keeping only applications that can open every selected file.
				for (const auto& [app_id, count] : app_occurrence_count) {
					if (count == num_files) {
						finalists.push_back({ all_apps_info.at(app_id), app_scores.at(app_id) });
					}
				}

				std::sort(finalists.begin(), finalists.end());

				// --- Part 3: Final List Generation ---

				std::vector<CandidateInfo> result;
				if (!finalists.empty()) {
					result.reserve(finalists.size());

					// Count name occurrences to identify duplicates that need disambiguation.
					std::unordered_map<std::wstring, int> name_counts;
					for (const auto& candidate : finalists) {
						name_counts[candidate.info.name]++;
					}

					// Build the final list in the correct format.
					for (const auto& candidate : finalists) {
						CandidateInfo final_c;
						final_c.id = candidate.info.id;
						final_c.terminal = false;
						final_c.name = candidate.info.name;
						final_c.multi_file_aware = true;

						// If an app name is duplicated, append its version string to make it unique in the UI.
						if (name_counts[candidate.info.name] > 1 && !candidate.info.info.empty()) {
							final_c.name += L" (" + candidate.info.info + L")";
						}
						result.push_back(final_c);
					}
				}

				// Populate candidates on normal successful exit
				final_result.candidates = std::move(result);

			} catch (const OperationCancelledException&) {
				_last_uti_profiles.clear();
				final_result.was_cancelled = true;
			}
#ifdef __clang__
		} // end of outer @autoreleasepool
#else
		} // end of outer AutoreleasePoolGuard
#endif

		return final_result;
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
		NSString *nsPath = [NSString stringWithUTF8String:StrWide2MB(candidate.id).c_str()];
		NSURL *appURL = [NSURL fileURLWithPath:nsPath];
		if (!appURL) return details;

		NSBundle *bundle = [NSBundle bundleWithURL:appURL];
		if (!bundle) return details;

		NSDictionary *infoDict = [bundle infoDictionary];

		NSString *appName = [infoDict objectForKey:@"CFBundleDisplayName"] ?: [infoDict objectForKey:@"CFBundleName"];
		if (appName) {
			details.push_back({GetMsg(MsgID::AppName), StrMB2Wide([appName UTF8String])});
		}

		details.push_back({GetMsg(MsgID::FullPath), candidate.id});

		NSString *execName = [infoDict objectForKey:@"CFBundleExecutable"];
		if (execName) {
			details.push_back({GetMsg(MsgID::ExecutableFile), StrMB2Wide([execName UTF8String])});
		}

		NSString *bundleShortVersion = [infoDict objectForKey:@"CFBundleShortVersionString"];
		if (bundleShortVersion) {
			details.push_back({GetMsg(MsgID::Version), StrMB2Wide([bundleShortVersion UTF8String])});
		}

		NSString *bundleVersion = [infoDict objectForKey:@"CFBundleVersion"];
		if (bundleVersion) {
			details.push_back({GetMsg(MsgID::BundleVersion), StrMB2Wide([bundleVersion UTF8String])});
		}

		return details;
	}


	// Collects unique formatted profile strings based on the last GetAppCandidates call.
	// This function performs the UTI-to-MIME-type conversion on demand.
	std::vector<std::wstring> MacOSAppProvider::GetMimeTypes()
	{
		std::unordered_set<std::wstring> unique_profile_strings;
		unique_profile_strings.reserve(_last_uti_profiles.size());

		for (const auto& profile : _last_uti_profiles) {
			if (!profile.accessible) {
				// File was inaccessible (invalid path, permissions, etc.)
				unique_profile_strings.insert(L"(inaccessible)");
				continue;
			}

			// File was accessible, convert its cached UTI to a MIME type.
			std::wstring result_mime;
			NSString *uti = [NSString stringWithUTF8String:profile.uti.c_str()];

			// Use the appropriate API based on the target macOS version.
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 110000 // UTType is available on macOS 11.0+
			// Modern approach for macOS 11.0 and later, converting a UTI to a MIME type.
			UTType *type = [UTType typeWithIdentifier:uti];
			if (type) {
				NSString *mimeStr = type.preferredMIMEType;
				if (mimeStr) result_mime = StrMB2Wide([mimeStr UTF8String]);
			}
#else
			// Legacy approach for older macOS versions.
#ifdef __clang__
			CFStringRef mimeType = UTTypeCopyPreferredTagWithClass((__bridge CFStringRef)uti,
																   kUTTagClassMIMEType);
			if (mimeType) {
				// Transfer ownership of the CFStringRef to ARC.
				NSString *mimeStr = (__bridge_transfer NSString *)mimeType;
				result_mime = StrMB2Wide([mimeStr UTF8String]);
			}
#else // gcc does not support ARC.
			CFStringRef mimeType = UTTypeCopyPreferredTagWithClass((CFStringRef)uti,
																   kUTTagClassMIMEType);
			if (mimeType) {
				NSString *mimeStr = [(NSString *)mimeType autorelease];
				result_mime = StrMB2Wide([mimeStr UTF8String]);
			}
#endif
#endif

			if (result_mime.empty()) {
				// File was accessible, UTI was found, but no MIME type equivalent.
				unique_profile_strings.insert(L"(none)");
			} else {
				// File was accessible and had a corresponding MIME type.
				unique_profile_strings.insert(L"(" + result_mime + L")");
			}
		}

		std::vector<std::wstring> result_vec(unique_profile_strings.begin(), unique_profile_strings.end());
		return result_vec;
	}
} // namespace openwith
#endif // __APPLE__