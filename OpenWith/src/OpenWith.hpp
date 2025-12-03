#pragma once

#include "AppProvider.hpp"
#include "farplug-wide.h"
#include "WinCompat.h"
#include "WinPort.h"
#include "common.hpp"
#include "utils.h"
#include <string>
#include <vector>
#include <optional>

namespace OpenWith {

class OpenWithPlugin
{
public:

	static void SetStartupInfo(const PluginStartupInfo *plugin_startup_info);
	static void GetPluginInfo(PluginInfo *plugin_info);
	static HANDLE OpenPlugin(int open_from, INT_PTR item);
	static int Configure(int item_number);
	static void Exit();
	static const wchar_t* GetMsg(int msg_id);

private:

	// Result structure for the configuration dialog.
	struct ConfigureResult
	{
		bool settings_saved = false;  // True if the user clicked "Ok" and settings were successfully saved.
		bool refresh_needed = false;  // True if a setting affecting the candidate list (e.g. platform specifics) was changed.
	};

	// Global context provided by far2l.
	static PluginStartupInfo s_info;
	static FarStandardFunctions s_fsf;

	// Cached platform-independent configuration options.
	static bool s_use_external_terminal;
	static bool s_no_wait_for_command_completion;
	static bool s_clear_selection;
	static bool s_confirm_launch;
	static int s_confirm_launch_threshold;

	static void ProcessFiles(const std::vector<std::wstring>& filepaths);
	static void ShowNoAppsError(AppProvider* provider, std::optional<std::vector<std::wstring>>& mime_cache);
	static void FilterOutTerminalCandidates(std::vector<CandidateInfo> &candidates, size_t file_count);
	static const std::vector<std::wstring>& GetMimeProfiles(AppProvider* provider, std::optional<std::vector<std::wstring>>& cache);
	static bool AskForLaunchConfirmation(const CandidateInfo& app, const std::vector<std::wstring>& filepaths);
	static void LaunchApplication(const CandidateInfo& app, const std::vector<std::wstring>& cmds);
	static bool ShowDetailsDialog(const std::vector<std::wstring>& filepaths, const std::vector<std::wstring>& unique_mime_profiles, const std::vector<Field> &application_info, const std::vector<std::wstring>& cmds);
	static ConfigureResult ConfigureImpl();
	static void LoadOptions();
	static void SaveOptions();
	static void ShowError(const std::vector<std::wstring>& error_lines);
	static std::wstring JoinStrings(const std::vector<std::wstring>& vec, const std::wstring& delimiter);
	static size_t GetLabelCellWidth(const Field& field);
	static size_t GetMaxLabelCellWidth(const std::vector<Field>& fields);
	static int GetScreenWidth();
};


} // namespace OpenWith
