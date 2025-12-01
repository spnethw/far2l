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

// Main class implementing the plugin logic.
// Handles integration with Far Manager API, UI rendering, and workflow orchestration.
class OpenWithPlugin
{
public:

	// Result structure for the configuration dialog.
	struct ConfigureResult
	{
		bool settings_saved = false;  // True if the user clicked "Ok" and settings were successfully saved.
		bool refresh_needed = false;  // True if a setting affecting the candidate list (e.g. platform specifics) was changed.
	};

	// Far Manager API: Saves startup info and initializes the plugin options.
	static void SetStartupInfo(const PluginStartupInfo *plugin_startup_info);

	// Far Manager API: Provides general information about the plugin (menu strings, etc.).
	static void GetPluginInfo(PluginInfo *plugin_info);

	// Far Manager API: Main entry point called when the user activates the plugin.
	static HANDLE OpenPlugin(int open_from, INT_PTR item);

	// Internal implementation of the configuration logic.
	// Returns a detailed result to determine if the application list needs a refresh.
	static ConfigureResult ConfigureImpl();

	// Far Manager API: Called to configure the plugin via the Plugin menu.
	static int Configure(int item_number);

	// Far Manager API: Cleanup routine before the plugin is unloaded.
	static void Exit();

	// Helper to retrieve a localized message string by its ID.
	static const wchar_t* GetMsg(int msg_id);

private:
	// Global context provided by Far Manager.
	static PluginStartupInfo s_info;
	static FarStandardFunctions s_fsf;

	// Cached configuration options.
	static bool s_use_external_terminal;
	static bool s_no_wait_for_command_completion;
	static bool s_clear_selection;
	static bool s_confirm_launch;
	static int s_confirm_launch_threshold;

	// Low-level implementation of the details dialog rendering.
	// Calculates layout based on content length and handles user input.
	static bool ShowDetailsDialogImpl(const std::vector<Field>& file_info, const std::vector<Field>& application_info, const Field& launch_command);

	// High-level wrapper for the details dialog.
	// Prepares formatted data (e.g. file counts, MIME strings) before calling the implementation.
	static bool ShowDetailsDialog(AppProvider* provider, const CandidateInfo& app, const std::vector<std::wstring>& filepaths,  const std::vector<std::wstring>& cmds, const std::vector<std::wstring>& unique_mime_profiles);

	// Prompts the user for confirmation if the number of files exceeds the configured threshold.
	static bool AskForLaunchConfirmation(const CandidateInfo& app, const std::vector<std::wstring>& filepaths);

	// Executes the launch commands for the selected application.
	// Handles flags for external terminal execution and asynchronous launching.
	static void LaunchApplication(const CandidateInfo& app, const std::vector<std::wstring>& cmds);

	// Main workflow: resolves candidates, displays the selection menu, and handles user actions (F3/F9/Enter).
	static void ProcessFiles(const std::vector<std::wstring>& filepaths);

	// Lazy loader for MIME profiles. Fetches data from the provider only if requested.
	static const std::vector<std::wstring>& GetOrUpdateMimeProfiles(AppProvider* provider, std::optional<std::vector<std::wstring>>& cache);

	// Fetches application candidates from the provider and applies plugin-specific filtering
	// (e.g. removing terminal apps when multiple files are selected in internal terminal mode).
	static void UpdateAppCandidates(AppProvider* provider,  const std::vector<std::wstring>& filepaths, std::vector<CandidateInfo>& candidates);

	// Loads configuration settings from the INI file.
	static void LoadOptions();

	// Saves current configuration settings to the INI file.
	static void SaveOptions();

	// Helper to display a standard Far Manager error message box.
	static void ShowError(const wchar_t *title, const std::vector<std::wstring>& text);

	// Utility: Joins a vector of strings with a specified delimiter.
	static std::wstring JoinStrings(const std::vector<std::wstring>& vec, const std::wstring& delimiter);

	// Utility: Calculates the display width (in console cells) of a field label.
	static size_t GetLabelCellWidth(const Field& field);

	// Utility: Finds the maximum label width in a list of fields for alignment purposes.
	static size_t GetMaxLabelCellWidth(const std::vector<Field>& fields);

	// Utility: Returns the current width of the console screen.
	static int GetScreenWidth();
};


} // namespace OpenWith
