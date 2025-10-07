#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "MacOSAppProvider.hpp"
#include "lng.hpp"
#include "WideMB.h"
#include "common.hpp"
#include "utils.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>

// A temporary structure to hold application details fetched from the system
// before they are processed into the final CandidateInfo format.
struct MacCandidateTempInfo {
    std::wstring name;
    std::wstring id; // The full path to the .app bundle, used as a unique identifier.
    std::wstring info; // Version string used for disambiguation if names conflict.
};


// ****************************** Implementation ******************************

MacOSAppProvider::MacOSAppProvider(TMsgGetter msg_getter) : AppProvider(std::move(msg_getter))
{
}

// Helper to convert an NSURL object to a UTF-8 encoded std::string path.
static std::string NSURLToPath(NSURL *url) {
    if (!url) return {};
    return std::string([[url path] UTF8String]);
}

// Helper to extract essential application metadata from its bundle.
static MacCandidateTempInfo AppBundleToTempInfo(NSURL *appURL) {
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

// Helper to safely escape a string argument for the shell.
// This wraps the argument in single quotes, which is a robust way to prevent shell interpretation.
static std::wstring EscapeForShell(const std::wstring& arg) {
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

// Finds applications that can open all specified files by intersecting candidate sets.
std::vector<CandidateInfo> MacOSAppProvider::GetAppCandidates(const std::vector<std::wstring>& pathnames) {
    if (pathnames.empty()) {
        return {};
    }

    // A map to store detailed info for each unique application, keyed by its path.
    std::map<std::wstring, MacCandidateTempInfo> all_apps_info;
    // A set to hold the IDs of applications that can open ALL files processed so far.
    std::set<std::wstring> common_app_ids;

    // --- Step 1: Process the first file to establish the initial set of candidates. ---
    NSString *firstPath = [NSString stringWithUTF8String:StrWide2MB(pathnames[0]).c_str()];
    NSURL *firstURL = [NSURL fileURLWithPath:firstPath];
    if (firstURL) {
        // Ask Launch Services for all apps that can open this URL.
        NSArray<NSURL *> *apps = [[NSWorkspace sharedWorkspace] URLsForApplicationsToOpenURL:firstURL];
        for (NSURL *appURL in apps) {
            MacCandidateTempInfo temp_info = AppBundleToTempInfo(appURL);
            common_app_ids.insert(temp_info.id);
            all_apps_info[temp_info.id] = temp_info;
        }
    }

    // --- Step 2: Intersect the initial set with candidates from the remaining files. ---
    for (size_t i = 1; i < pathnames.size(); ++i) {
        if (common_app_ids.empty()) {
            break; // Optimization: if no common apps remain, we can stop early.
        }

        std::set<std::wstring> next_app_ids;
        NSString *nsPath = [NSString stringWithUTF8String:StrWide2MB(pathnames[i]).c_str()];
        NSURL *fileURL = [NSURL fileURLWithPath:nsPath];
        if (fileURL) {
            NSArray<NSURL *> *apps = [[NSWorkspace sharedWorkspace] URLsForApplicationsToOpenURL:fileURL];
            for (NSURL *appURL in apps) {
                MacCandidateTempInfo temp_info = AppBundleToTempInfo(appURL);
                next_app_ids.insert(temp_info.id);
                // Store app info if we haven't seen this app before.
                if (all_apps_info.find(temp_info.id) == all_apps_info.end()) {
                    all_apps_info[temp_info.id] = temp_info;
                }
            }
        }
        
        // Perform set intersection to find apps common to both sets.
        std::set<std::wstring> intersection;
        std::set_intersection(common_app_ids.begin(), common_app_ids.end(),
                              next_app_ids.begin(), next_app_ids.end(),
                              std::inserter(intersection, intersection.begin()));
        common_app_ids = std::move(intersection);
    }
    
    // --- Step 3: Build the final result, handling name collisions by adding version info. ---
    std::vector<CandidateInfo> result;
    result.reserve(common_app_ids.size());
    
    // Count occurrences of each application name to detect duplicates.
    std::unordered_map<std::wstring, int> name_counts;
    for (const auto& app_id : common_app_ids) {
        name_counts[all_apps_info[app_id].name]++;
    }

    for (const auto& app_id : common_app_ids) {
        const auto& temp_info = all_apps_info[app_id];
        CandidateInfo final_c;
        final_c.id = temp_info.id;
        final_c.terminal = false; // GUI apps are the norm on macOS.
        final_c.name = temp_info.name;
        
        // If an app name is duplicated, append its version string to make it unique in the UI.
        if (name_counts[temp_info.name] > 1 && !temp_info.info.empty()) {
            final_c.name += L" (" + temp_info.info + L")";
        }
        result.push_back(final_c);
    }
    
    return result;
}

// Constructs a single command line using the 'open' utility, which natively handles multiple files.
std::vector<std::wstring> MacOSAppProvider::ConstructCommandLine(const CandidateInfo& candidate, const std::vector<std::wstring>& pathnames) {
    if (candidate.id.empty() || pathnames.empty()) {
        return {};
    }

    // The 'open -a <app_path>' command tells the system to open files with a specific application.
    std::wstring cmd = L"open -a " + EscapeForShell(candidate.id);
    for (const auto& pathname : pathnames) {
        cmd += L" " + EscapeForShell(pathname);
    }
    
    return {cmd}; // Return a vector containing the single constructed command.
}

// Fetches detailed information about a candidate application from its bundle.
std::vector<Field> MacOSAppProvider::GetCandidateDetails(const CandidateInfo& candidate) {
    std::vector<Field> details;

    NSString *nsPath = [NSString stringWithUTF8String:StrWide2MB(candidate.id).c_str()];
    NSURL *appURL = [NSURL fileURLWithPath:nsPath];
    if (!appURL) return details;
    
    NSBundle *bundle = [NSBundle bundleWithURL:appURL];
    if (!bundle) return details;

    NSDictionary *infoDict = [bundle infoDictionary];

    details.push_back({m_GetMsg(MPathname), candidate.id});

    NSString *execName = [infoDict objectForKey:@"CFBundleExecutable"];
    if (execName) {
        details.push_back({m_GetMsg(MExecutableFile), StrMB2Wide([execName UTF8String])});
    }

    NSString *bundleShortVersion = [infoDict objectForKey:@"CFBundleShortVersionString"];
    if (bundleShortVersion) {
        details.push_back({m_GetMsg(MVersion), StrMB2Wide([bundleShortVersion UTF8String])});
    }
    
    NSString *bundleVersion = [infoDict objectForKey:@"CFBundleVersion"];
    if (bundleVersion) {
        details.push_back({m_GetMsg(MBundleVersion), StrMB2Wide([bundleVersion UTF8String])});
    }

    return details;
}

// Collects unique MIME types for a list of files using macOS's Uniform Type Identifier (UTI) system.
std::vector<std::wstring> MacOSAppProvider::GetMimeTypes(const std::vector<std::wstring>& pathnames) {
    std::unordered_set<std::wstring> unique_mimes;
    const std::wstring fallback_mime = L"application/octet-stream";

    for (const auto& pathname : pathnames) {
        NSString *nsPath = [NSString stringWithUTF8String:StrWide2MB(pathname).c_str()];
        NSURL *fileURL = [NSURL fileURLWithPath:nsPath];
        if (!fileURL) {
            unique_mimes.insert(fallback_mime);
            continue;
        }

        // Retrieve the Uniform Type Identifier (UTI) for the file.
        NSString *uti = nil;
        NSError *error = nil;
        [fileURL getResourceValue:&uti forKey:NSURLTypeIdentifierKey error:&error];
        if (error || !uti) {
            unique_mimes.insert(fallback_mime);
            continue;
        }

        std::wstring result;

        // Use the appropriate API based on the target macOS version.
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000 // UTType is available on macOS 11.0+
        // Modern approach for macOS 11.0 and later, converting a UTI to a MIME type.
        UTType *type = [UTType typeWithIdentifier:uti];
        if (type) {
            NSString *mimeStr = type.preferredMIMEType;
            if (mimeStr) result = StrMB2Wide([mimeStr UTF8String]);
        }
#else
        // Legacy approach for older macOS versions.
        CFStringRef mimeType = UTTypeCopyPreferredTagWithClass((__bridge CFStringRef)uti,
                                                               kUTTagClassMIMEType);
        if (mimeType) {
            // Transfer ownership of the CFStringRef to ARC.
            NSString *mimeStr = (__bridge_transfer NSString *)mimeType;
            result = StrMB2Wide([mimeStr UTF8String]);
        }
#endif
        unique_mimes.insert(result.empty() ? fallback_mime : result);
    }
    
    std::vector<std::wstring> result_vec(unique_mimes.begin(), unique_mimes.end());
    if (result_vec.empty() && !pathnames.empty()) {
        result_vec.push_back(fallback_mime);
    }

    return result_vec;
}

#endif // __APPLE__
