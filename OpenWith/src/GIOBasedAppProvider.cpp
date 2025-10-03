#ifdef ENABLE_GIO_SUPPORT

#include "GIOBasedAppProvider.hpp"
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include "WideMB.h"
#include "lng.hpp"
#include "utils.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_set>


// A utility function to replace all occurrences of a substring within a string.
static void ReplaceAll(std::string& str, const std::string& from, const std::string& to) {
	if (from.empty())
		return;
	size_t start_pos = 0;
	while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
		str.replace(start_pos, from.length(), to);
		start_pos += to.length(); // Move past the last replaced segment to avoid infinite loops.
	}
}

// Smart pointers for GLib/GIO types to ensure automatic memory management via g_object_unref.
// GAppInfoPtr safely manages a GAppInfo object.
using GAppInfoPtr = std::unique_ptr<GAppInfo, decltype(&g_object_unref)>;
// GFilePtr safely manages a GFile object.
using GFilePtr = std::unique_ptr<GFile, decltype(&g_object_unref)>;


GIOBasedAppProvider::GIOBasedAppProvider(TMsgGetter msg_getter)
	: AppProvider(std::move(msg_getter))
{
	// Constructor initializes the base class with the message getter function.
}


std::wstring GIOBasedAppProvider::GetMimeType(const std::wstring& pathname)
{
	// Convert wide string path to a multibyte string suitable for GIO functions.
	std::string path_mb = StrWide2MB(pathname);
	gboolean uncertain = FALSE;

	// Use g_content_type_guess to determine the MIME type of the file.
	// A unique_ptr with a custom deleter (g_free) is used to manage the returned string.
	std::unique_ptr<char, decltype(&g_free)> mime_type(
		g_content_type_guess(path_mb.c_str(), nullptr, 0, &uncertain),
		g_free
		);

	if (mime_type) {
		// If a MIME type was found, convert it back to a wide string for the plugin.
		return StrMB2Wide(mime_type.get());
	}
	// Return a default string if no MIME type could be determined.
	return L"(none)";
}


std::vector<CandidateInfo> GIOBasedAppProvider::GetAppCandidates(const std::wstring& pathname)
{
	std::vector<CandidateInfo> candidates;
	// Use a hash set to track application IDs and prevent duplicates in the final list.
	std::unordered_set<std::wstring> seen_ids;

	// First, determine the MIME type for the given file path.
	std::wstring mime_type_ws = GetMimeType(pathname);
	if (mime_type_ws.empty() || mime_type_ws == L"(none)") {
		// If no MIME type is found, no applications can be suggested.
		return candidates;
	}

	std::string mime_type_mb = StrWide2MB(mime_type_ws);

	// Custom deleter for GList structures returned by GIO, ensuring all GObjects in the list are unreferenced.
	auto g_list_deleter = [](GList* list) {
		if (list) {
			g_list_free_full(list, g_object_unref);
		}
	};

	// Helper lambda to iterate over a GList of GAppInfo objects and add them to the candidate list.
	auto add_apps_from_list = [&](GList* app_list) {
		if (!app_list) return;
		for (GList* l = app_list; l != nullptr; l = l->next) {
			GAppInfo* app_info = G_APP_INFO(l->data);

			CandidateInfo ci = ConvertGAppInfoToCandidate(app_info);
			// Add the candidate only if it hasn't been seen before.
			if (seen_ids.find(ci.id) == seen_ids.end()) {
				candidates.push_back(ci);
				seen_ids.insert(ci.id);
			}
		}
	};

	// 1. Get the default application for the MIME type. This has the highest priority.
	GAppInfoPtr default_app(g_app_info_get_default_for_type(mime_type_mb.c_str(), FALSE), g_object_unref);
	if (default_app) {
		CandidateInfo ci = ConvertGAppInfoToCandidate(default_app.get());
		candidates.push_back(ci);
		seen_ids.insert(ci.id);
	}

	// 2. Get applications that are recommended for the MIME type.
	std::unique_ptr<GList, decltype(g_list_deleter)> recommended_list(
		g_app_info_get_recommended_for_type(mime_type_mb.c_str()), g_list_deleter
		);
	add_apps_from_list(recommended_list.get());

	// 3. Get all other applications that claim to support the MIME type.
	std::unique_ptr<GList, decltype(g_list_deleter)> all_list(
		g_app_info_get_all_for_type(mime_type_mb.c_str()), g_list_deleter
		);
	add_apps_from_list(all_list.get());

	return candidates;
}


std::wstring GIOBasedAppProvider::ConstructCommandLine(const CandidateInfo& candidate, const std::wstring& pathname)
{
	std::string id_mb = StrWide2MB(candidate.id);

	// Create a GDesktopAppInfo object from the application's ID (e.g., "firefox.desktop").
	GAppInfoPtr app_info(G_APP_INFO(g_desktop_app_info_new(id_mb.c_str())), g_object_unref);
	if (!app_info) {
		return L"";
	}

	// Retrieve the raw command-line template from the .desktop file's "Exec" key.
	const char* command_template_raw = g_app_info_get_commandline(app_info.get());
	if (!command_template_raw) {
		return L"";
	}

	std::string cmd_str = command_template_raw;

	// Step 1: Temporarily replace literal "%%" with a placeholder to prevent it from being misinterpreted.
	const std::string placeholder = "__PERCENT_PLACEHOLDER__";
	ReplaceAll(cmd_str, "%%", placeholder);

	// Step 2: Prepare file path and URI arguments, properly quoted for shell safety.
	std::string path_mb = StrWide2MB(pathname);
	GFilePtr file(g_file_new_for_path(path_mb.c_str()), g_object_unref);
	if (!file) {
		return L"";
	}

	// Get the local path and its shell-quoted version.
	std::unique_ptr<char, decltype(&g_free)> local_path(g_file_get_path(file.get()), g_free);
	std::unique_ptr<char, decltype(&g_free)> quoted_path(
		local_path ? g_shell_quote(local_path.get()) : nullptr, g_free);

	// Get the URI and its shell-quoted version.
	std::unique_ptr<char, decltype(&g_free)> uri(g_file_get_uri(file.get()), g_free);
	std::unique_ptr<char, decltype(&g_free)> quoted_uri(
		uri ? g_shell_quote(uri.get()) : nullptr, g_free);

	// Get other required values and their quoted versions.
	const char* app_name = g_app_info_get_name(app_info.get());
	std::unique_ptr<char, decltype(&g_free)> quoted_name(
		app_name ? g_shell_quote(app_name) : nullptr, g_free);

	std::unique_ptr<char, decltype(&g_free)> quoted_id(g_shell_quote(id_mb.c_str()), g_free);

	// Step 3: Check if any file-related field codes (%f, %F, %u, %U) are present in the command.
	bool has_file_related_code = (cmd_str.find("%f") != std::string::npos ||
								  cmd_str.find("%F") != std::string::npos ||
								  cmd_str.find("%u") != std::string::npos ||
								  cmd_str.find("%U") != std::string::npos);

	// Step 4: Substitute the field codes with their corresponding quoted values.
	if (quoted_path) {
		ReplaceAll(cmd_str, "%f", quoted_path.get()); // single file
		ReplaceAll(cmd_str, "%F", quoted_path.get()); // multiple files (we provide one)
	}
	if (quoted_uri) {
		ReplaceAll(cmd_str, "%u", quoted_uri.get()); // single URI
		ReplaceAll(cmd_str, "%U", quoted_uri.get()); // multiple URIs (we provide one)
	}
	if (quoted_name) {
		ReplaceAll(cmd_str, "%c", quoted_name.get()); // application name
	}
	if (quoted_id) {
		ReplaceAll(cmd_str, "%k", quoted_id.get()); // .desktop file path or ID
	}
	// Note: Deprecated field codes (%i, %d, etc.) are ignored, as per spec recommendations.
	// They are left as-is, which is the safest approach.

	// Step 5: If no file-related codes were found, append the quoted path to the end of the command.
	// This is required by the XDG Desktop Entry Specification.
	if (!has_file_related_code && quoted_path) {
		cmd_str += " ";
		cmd_str += quoted_path.get();
	}

	// Step 6: Restore the literal "%%" from the placeholder.
	ReplaceAll(cmd_str, placeholder, "%");

	return StrMB2Wide(cmd_str);
}


std::vector<Field> GIOBasedAppProvider::GetCandidateDetails(const CandidateInfo& candidate)
{
	std::vector<Field> details;
	std::string id_mb = StrWide2MB(candidate.id);

	// Use GDesktopAppInfo specifically, as it provides access to all .desktop file fields.
	std::unique_ptr<GDesktopAppInfo, void(*)(gpointer)> desktop_app_info(
		g_desktop_app_info_new(id_mb.c_str()),
		g_object_unref
		);

	if (!desktop_app_info) {
		return details;
	}

	// Cast to the base GAppInfo type to use common GAppInfo functions.
	GAppInfo* app_info = G_APP_INFO(desktop_app_info.get());

	const char* filename = g_desktop_app_info_get_filename(desktop_app_info.get());
	if (filename) {
		details.push_back({m_GetMsg(MDesktopFile), StrMB2Wide(filename)});
	}

	const char* name = g_app_info_get_name(app_info);
	details.push_back({L"Name =", StrMB2Wide(name ? name : "")});

	const char* generic_name = g_desktop_app_info_get_string(desktop_app_info.get(), "GenericName");
	if (generic_name) {
		details.push_back({L"GenericName =", StrMB2Wide(generic_name)});
	}

	const char* comment = g_app_info_get_description(app_info);
	if (comment) {
		details.push_back({L"Comment =", StrMB2Wide(comment)});
	}

	const char* exec_command = g_desktop_app_info_get_string(desktop_app_info.get(), "Exec");
	if (exec_command) {
		details.push_back({L"Exec =", StrMB2Wide(exec_command)});
	}

	const char* try_exec = g_desktop_app_info_get_string(desktop_app_info.get(), "TryExec");
	if (try_exec) {
		details.push_back({L"TryExec =", StrMB2Wide(try_exec)});
	}

	const char* categories = g_desktop_app_info_get_categories(desktop_app_info.get());
	if (categories) {
		details.push_back({L"Categories =", StrMB2Wide(categories)});
	}

	const char* mime_types = g_desktop_app_info_get_string(desktop_app_info.get(), "MimeType");
	if (mime_types) {
		details.push_back({L"MimeType =", StrMB2Wide(mime_types)});
	}

	gboolean needs_terminal = g_desktop_app_info_get_boolean(desktop_app_info.get(), "Terminal");
	details.push_back({L"Terminal =", needs_terminal ? L"true" : L"false"});

	return details;
}


// Helper function to convert a GAppInfo object into the plugin's internal CandidateInfo struct.
CandidateInfo GIOBasedAppProvider::ConvertGAppInfoToCandidate(GAppInfo* app_info)
{
	CandidateInfo ci;

	// Get the application's display name.
	const char* name = g_app_info_get_name(app_info);
	ci.name = StrMB2Wide(name ? name : "Unnamed");

	// Get the application's unique ID (e.g., the .desktop file name).
	const char* id = g_app_info_get_id(app_info);
	ci.id = StrMB2Wide(id ? id : "");

	// Check if the application is a desktop app to query the "Terminal" key.
	if (G_IS_DESKTOP_APP_INFO(app_info)) {
		GDesktopAppInfo* desktop_app_info = G_DESKTOP_APP_INFO(app_info);
		ci.terminal = g_desktop_app_info_get_boolean(desktop_app_info, "Terminal");
	} else {
		// Default to false if it's not a standard desktop application.
		ci.terminal = false;
	}

	return ci;
}

#endif // ENABLE_GIO_SUPPORT
