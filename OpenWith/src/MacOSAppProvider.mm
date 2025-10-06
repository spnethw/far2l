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

// A temporary structure to hold application details before final processing.
struct MacCandidateTempInfo {
    std::wstring name;
    std::wstring id; // The full path to the .app bundle
    std::wstring info; // Version string for disambiguation
};


// ****************************** Implementation ******************************

MacOSAppProvider::MacOSAppProvider(TMsgGetter msg_getter) : AppProvider(std::move(msg_getter))
{
}

// Helper to convert an NSURL object to a UTF-8 encoded std::string path.
static std::string NSURLToPath(NSURL *url) {
    if (!url) return {};
    NSString *path = [url path];
    return path ? std::string([path UTF8String]) : std::string{};
}

// Helper to extract application metadata from its bundle.
static MacCandidateTempInfo AppBundleToTempInfo(NSURL *appURL) {
    MacCandidateTempInfo c;

    NSBundle *bundle = [NSBundle bundleWithURL:appURL];
    NSString *bundleName = [[bundle infoDictionary] objectForKey:@"CFBundleName"];
    NSString *bundleShortVersion = [[bundle infoDictionary] objectForKey:@"CFBundleShortVersionString"];
    NSString *bundleVersion = [[bundle infoDictionary] objectForKey:@"CFBundleVersion"];

    c.name = StrMB2Wide(bundleName ? [bundleName UTF8String] : NSURLToPath(appURL));
    c.id = StrMB2Wide(NSURLToPath(appURL));
    c.info = bundleShortVersion ? StrMB2Wide([bundleShortVersion UTF8String])
             : ( bundleVersion ? StrMB2Wide([bundleVersion UTF8String]): L"") ;

    return c;
}

// Helper to safely escape a string argument for the shell.
static std::wstring EscapeForShell(const std::wstring& arg) {
    std::wstring out;
    out.push_back(L'\''); // Use single quotes for robust escaping
    for (wchar_t c : arg) {
        if (c == L'\'') {
            out.append(L"'\\''"); // Handle literal single quotes
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

    // Use a map to store app info and a set to find common apps across all files.
    std::map<std::wstring, MacCandidateTempInfo> all_apps_info;
    std::set<std::wstring> common_app_ids;

    // Process the first file to establish the initial set of candidates.
    NSString *nsPath = [NSString stringWithUTF8String:StrWide2MB(pathnames[0]).c_str()];
    NSURL *fileURL = [NSURL fileURLWithPath:nsPath];
    if (fileURL) {
        NSArray<NSURL *> *apps = [[NSWorkspace sharedWorkspace] URLsForApplicationsToOpenURL:fileURL];
        for (NSURL *appURL in apps) {
            MacCandidateTempInfo temp_info = AppBundleToTempInfo(appURL);
            common_app_ids.insert(temp_info.id);
            all_apps_info[temp_info.id] = temp_info;
        }
    }

    // Intersect the initial set with candidates for the remaining files.
    for (size_t i = 1; i < pathnames.size(); ++i) {
        if (common_app_ids.empty()) {
            break; // Optimization: no common apps left.
        }

        std::set<std::wstring> next_app_ids;
        nsPath = [NSString stringWithUTF8String:StrWide2MB(pathnames[i]).c_str()];
        fileURL = [NSURL fileURLWithPath:nsPath];
        if (fileURL) {
            NSArray<NSURL *> *apps = [[NSWorkspace sharedWorkspace] URLsForApplicationsToOpenURL:fileURL];
            for (NSURL *appURL in apps) {
                MacCandidateTempInfo temp_info = AppBundleToTempInfo(appURL);
                next_app_ids.insert(temp_info.id);
                // Store app info if not seen before.
                if (all_apps_info.find(temp_info.id) == all_apps_info.end()) {
                    all_apps_info[temp_info.id] = temp_info;
                }
            }
        }
        
        // Perform set intersection.
        std::set<std::wstring> intersection;
        std::set_intersection(common_app_ids.begin(), common_app_ids.end(),
                              next_app_ids.begin(), next_app_ids.end(),
                              std::inserter(intersection, intersection.begin()));
        common_app_ids = std::move(intersection);
    }
    
    // Build the final result vector from the common candidates.
    std::vector<CandidateInfo> result;
    result.reserve(common_app_ids.size());
    std::unordered_map<std::wstring, int> name_counts;
    for (const auto& app_id : common_app_ids) {
        name_counts[all_apps_info[app_id].name]++;
    }

    for (const auto& app_id : common_app_ids) {
        const auto& temp_info = all_apps_info[app_id];
        CandidateInfo final_c;
        final_c.id = temp_info.id;
        final_c.terminal = false;
        final_c.name = temp_info.name;
        
        // Append version info if multiple apps with the same name exist.
        if (name_counts[temp_info.name] > 1 && !temp_info.info.empty()) {
            final_c.name += L" (" + temp_info.info + L")";
        }
        result.push_back(final_c);
    }
    
    return result;
}

// Constructs a single command line for the 'open' utility, which handles multiple files.
std::vector<std::wstring> MacOSAppProvider::ConstructCommandLine(const CandidateInfo& candidate, const std::vector<std::wstring>& pathnames) {
    if (candidate.id.empty() || pathnames.empty()) {
        return {};
    }

    std::wstring cmd = L"open -a " + EscapeForShell(candidate.id);
    for (const auto& pathname : pathnames) {
        cmd += L" " + EscapeForShell(pathname);
    }
    
    return {cmd}; // Return a vector with the single constructed command.
}


std::vector<Field> MacOSAppProvider::GetCandidateDetails(const CandidateInfo& candidate) {
    std::vector<Field> details;

    NSString *nsPath = [NSString stringWithUTF8String:StrWide2MB(candidate.id).c_str()];
    NSURL *appURL = [NSURL fileURLWithPath:nsPath];
    if (!appURL) return details;
    
    NSBundle *bundle = [NSBundle bundleWithURL:appURL];
    if (!bundle) return details;

    NSDictionary *infoDict = [bundle infoDictionary];

    details.push_back({L"Full path:", candidate.id});
    details.push_back({L"Name:", candidate.name});

    NSString *execName = [infoDict objectForKey:@"CFBundleExecutable"];
    if (execName) {
        details.push_back({L"Executable file:", StrMB2Wide([execName UTF8String])});
    }

    NSString *bundleShortVersion = [infoDict objectForKey:@"CFBundleShortVersionString"];
    if (bundleShortVersion) {
        details.push_back({L"Version:", StrMB2Wide([bundleShortVersion UTF8String])});
    }
    
    NSString *bundleVersion = [infoDict objectForKey:@"CFBundleVersion"];
    if (bundleVersion) {
        details.push_back({L"Bundle version:", StrMB2Wide([bundleVersion UTF8String])});
    }

    return details;
}

// Collects unique MIME types for a list of files on macOS.
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

        NSString *uti = nil;
        NSError *error = nil;
        // Retrieve the Uniform Type Identifier (UTI) for the file.
        [fileURL getResourceValue:&uti forKey:NSURLTypeIdentifierKey error:&error];
        if (!uti) {
            unique_mimes.insert(fallback_mime);
            continue;
        }

        std::wstring result;

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000 // UTType is available on macOS 11.0+
        // Modern approach for macOS 11.0 and later.
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
            NSString *mimeStr = (__bridge_transfer NSString *)mimeType;
            result = StrMB2Wide([mimeStr UTF8String]);
        }
#endif
        unique_mimes.insert(result.empty() ? fallback_mime : result);
    }
    
    std::vector<std::wstring> result_vec;
    if (unique_mimes.empty() && !pathnames.empty()) {
        result_vec.push_back(fallback_mime);
    } else {
        result_vec.assign(unique_mimes.begin(), unique_mimes.end());
    }

    return result_vec;
}

#endif // __APPLE__
