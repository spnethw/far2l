#include "OpenWith.hpp"
#include "AppProvider.hpp"
#include "farplug-wide.h"
#include "KeyFileHelper.h"
#include "WinCompat.h"
#include "WinPort.h"
#include "lng.hpp"
#include "common.hpp"
#include "utils.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <optional>

// Configuration file location relative to the user's profile.
#define INI_LOCATION InMyConfig("plugins/openwith/config.ini")
#define INI_SECTION  "Settings"

namespace OpenWith {

// ****************************** Public API ******************************


// Initializes the plugin structure, saves global function pointers, and loads settings.
void OpenWithPlugin::SetStartupInfo(const PluginStartupInfo *plugin_startup_info)
{
	s_info = *plugin_startup_info;
	s_fsf = *plugin_startup_info->FSF;
	s_info.FSF = &s_fsf;
	LoadOptions();
}


// Populates the plugin information structure required by far2l.
// Sets up the menu strings displayed in the Plugins menu and Configuration menu.
void OpenWithPlugin::GetPluginInfo(PluginInfo *plugin_info)
{
	plugin_info->StructSize = sizeof(*plugin_info);
	plugin_info->Flags = 0;
	static const wchar_t *s_menu_strings[1];
	s_menu_strings[0] = GetMsg(MPluginTitle);
	plugin_info->PluginMenuStrings = s_menu_strings;
	plugin_info->PluginMenuStringsNumber = ARRAYSIZE(s_menu_strings);
	static const wchar_t *s_config_strings[1];
	s_config_strings[0] = GetMsg(MPluginTitle);
	plugin_info->PluginConfigStrings = s_config_strings;
	plugin_info->PluginConfigStringsNumber = ARRAYSIZE(s_config_strings);
	plugin_info->CommandPrefix = nullptr;
}


// Called to configure the plugin via the Plugin menu.  Returns 1 if settings were saved, 0 otherwise.
int OpenWithPlugin::Configure(int item_number)
{
	return ConfigureImpl().settings_saved;
}


// Called when the user activates the plugin. Validates the panel state, retrieves selected files
// (or the file under cursor), and initiates the processing workflow.
HANDLE OpenWithPlugin::OpenPlugin(int open_from, INT_PTR item)
{
	if (open_from != OPEN_PLUGINSMENU) {
		return INVALID_HANDLE_VALUE;
	}

	PanelInfo pi = {};

	if (!s_info.Control(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, (LONG_PTR)&pi)) {
		return INVALID_HANDLE_VALUE;
	}

	if (pi.PanelType != PTYPE_FILEPANEL || pi.ItemsNumber <= 0) {
		return INVALID_HANDLE_VALUE;
	}

	// The plugin requires real paths to pass to external applications.
	if (pi.Plugin && !(pi.Flags & PFLAGS_REALNAMES)) {
		ShowError(GetMsg(MError), {GetMsg(MNotRealNames)});
		return INVALID_HANDLE_VALUE;
	}

	std::vector<std::wstring> selected_filepaths;

	int dir_size = s_info.Control(PANEL_ACTIVE, FCTL_GETPANELDIR, 0, 0);
	if (dir_size <= 0) {
		return INVALID_HANDLE_VALUE;
	}

	auto dir_buf = std::make_unique<wchar_t[]>(dir_size);
	if (!s_info.Control(PANEL_ACTIVE, FCTL_GETPANELDIR, dir_size, (LONG_PTR)dir_buf.get())) {
		return INVALID_HANDLE_VALUE;
	}

	std::wstring base_path(dir_buf.get());
	if (!base_path.empty() && base_path.back() != L'/') {
		base_path += L'/';
	}

	// Iterate over selected items.
	// If no specific selection exists, 'SelectedItemsNumber' is 1, and the item is the one under the cursor.
	if (pi.SelectedItemsNumber > 0) {
		selected_filepaths.reserve(pi.SelectedItemsNumber);
		for (int i = 0; i < pi.SelectedItemsNumber; ++i) {
			// Query item buffer size.
			int item_size = s_info.Control(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, i, 0);
			if (item_size <= 0) {
				continue;
			}

			auto item_buf = std::make_unique<unsigned char[]>(item_size);
			PluginPanelItem* pi_item = reinterpret_cast<PluginPanelItem*>(item_buf.get());

			// Retrieve item data and construct the full path.
			if (s_info.Control(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, i, (LONG_PTR)pi_item) && pi_item->FindData.lpwszFileName) {
				selected_filepaths.push_back(base_path + pi_item->FindData.lpwszFileName);
			}
		}
	}

	if (!selected_filepaths.empty()) {
		ProcessFiles(selected_filepaths);
	}

	// Return INVALID_HANDLE_VALUE because this plugin acts as a command, not as a panel plugin (VFS).
	return INVALID_HANDLE_VALUE;
}


// Cleanup routine before the plugin is unloaded.
void OpenWithPlugin::Exit()
{
}


// Retrieves a localized message string from the language file by its ID.
const wchar_t* OpenWithPlugin::GetMsg(int msg_id)
{
	return s_info.GetMsg(s_info.ModuleNumber, msg_id);
}


// ****************************** Private implementation ******************************


// Implementation of the configuration dialog.  Dynamically builds the UI based on general options and platform-specific settings
// provided by AppProvider. Returns status indicating if the candidate list should be refreshed.
OpenWithPlugin::ConfigureResult OpenWithPlugin::ConfigureImpl()
{
	constexpr int CONFIG_DIALOG_WIDTH = 70;

	// Load platform-independent settings.
	LoadOptions();

	// Create a temporary provider to access platform-specific settings.
	auto provider = AppProvider::CreateAppProvider(&OpenWithPlugin::GetMsg);
	provider->LoadPlatformSettings();

	const bool old_use_external_terminal = s_use_external_terminal;
	const std::vector<ProviderSetting> old_platform_settings = provider->GetPlatformSettings();

	std::vector<FarDialogItem> config_dialog_items;
	int current_y = 1;

	auto add_item = [&config_dialog_items](const FarDialogItem& item) -> size_t {
		config_dialog_items.push_back(item);
		auto item_idx = config_dialog_items.size() - 1;
		return item_idx;
	};

	auto add_checkbox = [&add_item, &current_y](const wchar_t* text, bool is_checked, bool is_disabled = false) -> size_t {
		FarDialogItem chkbox = { DI_CHECKBOX, 5, current_y, 0, current_y, FALSE, {}, is_disabled ? DIF_DISABLE : DIF_NONE, FALSE, text, 0 };
		chkbox.Param.Selected = is_checked;
		auto item_idx = add_item(chkbox);
		current_y++;
		return item_idx;
	};

	auto add_separator = [&add_item, &current_y]() -> size_t {
		auto item_idx = add_item({ DI_TEXT, 5, current_y, 0, current_y, FALSE, {}, DIF_SEPARATOR, FALSE, L"", 0 });
		current_y++;
		return item_idx;
	};


	add_item({ DI_DOUBLEBOX, 3, current_y++, CONFIG_DIALOG_WIDTH - 4, 0, FALSE, {}, DIF_NONE, FALSE, GetMsg(MConfigTitle), 0 });

	// ----- Add general platform-independent settings. -----
	auto use_external_terminal_idx          = add_checkbox(GetMsg(MUseExternalTerminal), s_use_external_terminal);
	auto no_wait_for_command_completion_idx = add_checkbox(GetMsg(MNoWaitForCommandCompletion), s_no_wait_for_command_completion);
	auto clear_selection_idx                = add_checkbox(GetMsg(MClearSelection), s_clear_selection);

	auto threshold_str = std::to_wstring(s_confirm_launch_threshold);
	const wchar_t* confirm_launch_label = GetMsg(MConfirmLaunchOption);
	int confirm_launch_label_width = static_cast<int>(s_fsf.StrCellsCount(confirm_launch_label, wcslen(confirm_launch_label)));

	FarDialogItem confirm_launch_chkbx = { DI_CHECKBOX, 5, current_y, 0, current_y, FALSE, {}, DIF_NONE, FALSE, confirm_launch_label, 0 };
	confirm_launch_chkbx.Param.Selected  = s_confirm_launch;
	auto confirm_launch_chkbx_idx = add_item(confirm_launch_chkbx);
	auto confirm_launch_edit_idx  = add_item({ DI_FIXEDIT, confirm_launch_label_width + 10, current_y, confirm_launch_label_width + 13, current_y, FALSE, {(DWORD_PTR)L"9999"}, DIF_MASKEDIT, FALSE, threshold_str.c_str(), 0 });
	current_y++;

	// ----- Add Platform-Specific Settings. -----
	std::vector<std::pair<size_t, ProviderSetting>> dynamic_settings;
	dynamic_settings.reserve(old_platform_settings.size());

	if (!old_platform_settings.empty()) {
		add_separator();
		for (const auto& s : old_platform_settings) {
			dynamic_settings.emplace_back(add_checkbox(s.display_name.c_str(), s.value, s.disabled), s);
		}
	}

	add_separator();
	auto ok_btn_idx = add_item({ DI_BUTTON, 0, current_y, 0, current_y, FALSE, {}, DIF_CENTERGROUP, TRUE, GetMsg(MOk), 0 });
	add_item({ DI_BUTTON, 0, current_y, 0, current_y, FALSE, {}, DIF_CENTERGROUP, FALSE, GetMsg(MCancel), 0 });

	int config_dialog_height = current_y + 3;
	config_dialog_items[0].Y2 = config_dialog_height - 2;

	HANDLE dlg = s_info.DialogInit(s_info.ModuleNumber, -1, -1, CONFIG_DIALOG_WIDTH, config_dialog_height, L"ConfigurationDialog",
								   config_dialog_items.data(), static_cast<unsigned int>(config_dialog_items.size()), 0, 0, nullptr, 0);
	if (dlg == INVALID_HANDLE_VALUE) {
		return {};
	}

	int exit_code = s_info.DialogRun(dlg);
	ConfigureResult result;

	// ----- Process results if "OK" was pressed. -----
	if (exit_code == static_cast<int>(ok_btn_idx)) {
		result.settings_saved = true;

		auto is_checked = [&dlg](size_t i) -> bool {
			return s_info.SendDlgMessage(dlg, DM_GETCHECK, i, 0) == BSTATE_CHECKED;
		};

		s_use_external_terminal          = is_checked(use_external_terminal_idx);
		s_no_wait_for_command_completion = is_checked(no_wait_for_command_completion_idx);
		s_clear_selection                = is_checked(clear_selection_idx);
		s_confirm_launch                 = is_checked(confirm_launch_chkbx_idx);

		auto threshold_str = (const wchar_t*)s_info.SendDlgMessage(dlg, DM_GETCONSTTEXTPTR, confirm_launch_edit_idx, 0);
		s_confirm_launch_threshold = wcstol(threshold_str, nullptr, 10);

		SaveOptions();

		bool is_platform_settings_changed = false;

		if (!dynamic_settings.empty()) {
			std::vector<ProviderSetting> new_platform_settings;
			new_platform_settings.reserve(dynamic_settings.size());

			for (const auto& [idx, setting] : dynamic_settings) {
				bool new_value = is_checked(idx);
				if (setting.value != new_value) {
					is_platform_settings_changed = true;
				}
				new_platform_settings.push_back({ setting.internal_key, setting.display_name, new_value });
			}

			if (is_platform_settings_changed) {
				provider->SetPlatformSettings(new_platform_settings);
				provider->SavePlatformSettings();
			}
		}

		if (is_platform_settings_changed || (old_use_external_terminal != s_use_external_terminal)) {
			result.refresh_needed = true;
		}
	}
	s_info.DialogFree(dlg);
	return result;
}


// Low-level implementation of the details dialog. Calculates layout, constructs dialog items, and handles
// the execution result. Returns true if the user clicked "Launch", false otherwise (e.g. "Close" or Esc).
bool OpenWithPlugin::ShowDetailsDialogImpl(const std::vector<Field>& file_info,
										   const std::vector<Field>& application_info,
										   const Field& launch_command)
{
	constexpr int DETAILS_DIALOG_MIN_WIDTH = 40;
	constexpr int DETAILS_DIALOG_DESIRED_WIDTH = 90;

	const int screen_width = GetScreenWidth();
	const int details_dialog_max_width = std::max(DETAILS_DIALOG_MIN_WIDTH, screen_width - 4);
	const int details_dialog_width = std::clamp(DETAILS_DIALOG_DESIRED_WIDTH, DETAILS_DIALOG_MIN_WIDTH, details_dialog_max_width);
	const int details_dialog_height = static_cast<int>(file_info.size() + application_info.size() + 9);

	// Calculate the column width for labels to align fields nicely.
	const auto max_label_cell_width = static_cast<int>(std::max({
		GetMaxLabelCellWidth(file_info),
		GetMaxLabelCellWidth(application_info),
		GetLabelCellWidth(launch_command)
	}));

	const int label_end_x = max_label_cell_width + 4;
	const int edit_start_x = max_label_cell_width + 6;
	const int edit_end_x = details_dialog_width - 6;

	std::vector<FarDialogItem> details_dialog_items;
	details_dialog_items.reserve(file_info.size() * 2 + application_info.size() * 2 + 8);

	int current_y = 1;

	// Lambda to add a label/value pair.
	auto add_field_row = [&details_dialog_items, &current_y, label_end_x, edit_start_x, edit_end_x](const Field& field) {
		int label_start_x = label_end_x - static_cast<int>(GetLabelCellWidth(field)) + 1;
		details_dialog_items.push_back({ DI_TEXT, label_start_x, current_y, label_end_x, current_y, FALSE, {}, 0, 0, field.label.c_str(), 0 });
		details_dialog_items.push_back({ DI_EDIT, edit_start_x, current_y, edit_end_x, current_y, FALSE, {}, DIF_READONLY | DIF_SELECTONENTRY, 0, field.content.c_str(), 0 });
		current_y++;
	};

	auto add_separator = [&details_dialog_items, &current_y]() {
		details_dialog_items.push_back({ DI_TEXT, 5, current_y, 0, current_y, FALSE, {}, DIF_SEPARATOR, 0, L"", 0 });
		current_y++;
	};

	details_dialog_items.push_back({ DI_DOUBLEBOX, 3, current_y++, details_dialog_width - 4, details_dialog_height - 2, FALSE, {}, 0, 0, GetMsg(MDetails), 0 });
	for (const auto& field : file_info) {
		add_field_row(field);
	}
	add_separator();
	for (const auto& field : application_info) {
		add_field_row(field);
	}
	add_separator();
	add_field_row(launch_command);
	add_separator();
	details_dialog_items.push_back({ DI_BUTTON, 0, current_y, 0, current_y, TRUE, {}, DIF_CENTERGROUP, 0, GetMsg(MClose), 0 });
	details_dialog_items.back().DefaultButton = TRUE; // Default action is "Close" for safety.
	details_dialog_items.push_back({ DI_BUTTON, 0, current_y, 0, current_y, FALSE, {}, DIF_CENTERGROUP, 0, GetMsg(MLaunch), 0 });
	const int launch_btn_idx = static_cast<int>(details_dialog_items.size()) - 1;

	HANDLE dlg = s_info.DialogInit(s_info.ModuleNumber, -1, -1, details_dialog_width, details_dialog_height, L"InformationDialog",
								   details_dialog_items.data(), static_cast<unsigned int>(details_dialog_items.size()), 0, 0, nullptr, 0);

	if (dlg != INVALID_HANDLE_VALUE) {
		int exit_code = s_info.DialogRun(dlg);
		s_info.DialogFree(dlg);
		return (exit_code == launch_btn_idx);
	}
	return false;
}


// High-level wrapper for the details dialog. Prepares formatted data before calling the implementation.
bool OpenWithPlugin::ShowDetailsDialog(AppProvider* provider, const CandidateInfo& app,
									   const std::vector<std::wstring>& filepaths,
									   const std::vector<std::wstring>& cmds,
									   const std::vector<std::wstring>& unique_mime_profiles)
{
	std::vector<Field> file_info;
	if (filepaths.size() == 1) {
		// For a single file, show its full path.
		file_info.push_back({ GetMsg(MPathname), filepaths[0] });
	} else {
		// For multiple files, show a summary count.
		std::wstring count_msg = std::wstring(GetMsg(MFilesSelected)) + std::to_wstring(filepaths.size());
		file_info.push_back({ GetMsg(MPathname), count_msg });
	}

	file_info.push_back({ GetMsg(MMimeType), JoinStrings(unique_mime_profiles, L"; ") });

	std::wstring all_cmds = JoinStrings(cmds, L"; ");
	std::vector<Field> application_info = provider->GetCandidateDetails(app);
	Field launch_command { GetMsg(MLaunchCommand), all_cmds.c_str() };

	return ShowDetailsDialogImpl(file_info, application_info, launch_command);
}


// Prompts the user for confirmation if the number of files exceeds the configured threshold.
bool OpenWithPlugin::AskForLaunchConfirmation(const CandidateInfo& app, const std::vector<std::wstring>& filepaths)
{
	if (!s_confirm_launch || filepaths.size() <= static_cast<size_t>(s_confirm_launch_threshold)) {
		return true;
	}
	wchar_t message[255] = {};
	s_fsf.snprintf(message, ARRAYSIZE(message) - 1, GetMsg(MConfirmLaunchMessage), filepaths.size(), app.name.c_str());
	const wchar_t* items[] = { GetMsg(MConfirmLaunchTitle), message };
	int res = s_info.Message(s_info.ModuleNumber, FMSG_MB_YESNO, nullptr, items, ARRAYSIZE(items), 2);
	return (res == 0);
}


// Executes one or more command lines to launch the selected application.
void OpenWithPlugin::LaunchApplication(const CandidateInfo& app, const std::vector<std::wstring>& cmds)
{
	if (cmds.empty()) {
		return;
	}

	// If we have multiple commands to run, force asynchronous execution to avoid blocking.
	bool force_no_wait = cmds.size() > 1;

	unsigned int flags {};
	if (app.terminal) {
		flags = s_use_external_terminal ? EF_EXTERNALTERM : 0;
	} else {
		flags = (s_no_wait_for_command_completion || force_no_wait) ? EF_NOWAIT | EF_HIDEOUT : 0;
	}

	for (const auto& cmd : cmds) {
		if (s_fsf.Execute(cmd.c_str(), flags) == -1) {
			ShowError(GetMsg(MError), { GetMsg(MCannotExecute), cmd.c_str() });
			break; // Stop on the first error.
		}
	}

	if (s_clear_selection) {
		s_info.Control(PANEL_ACTIVE, FCTL_UPDATEPANEL, 0, 0);
	}
}


// Main workflow orchestrator: resolves application candidates, displays the selection menu,
// and handles user actions: F3 (Details), F9 (Settings), Enter (Launch).
void OpenWithPlugin::ProcessFiles(const std::vector<std::wstring>& filepaths)
{
	if (filepaths.empty()) {
		return;
	}

	auto provider = AppProvider::CreateAppProvider(&OpenWithPlugin::GetMsg);
	std::optional<std::vector<std::wstring>> unique_mime_profiles_cache;
	std::vector<CandidateInfo> app_candidates;

	constexpr int BREAK_KEYS[] = {VK_F3, VK_F9, 0};
	constexpr int KEY_F3_DETAILS = 0;
	constexpr int KEY_F9_OPTIONS = 1;

	int menu_break_code = -1;
	int active_menu_idx = 0;

	UpdateAppCandidates(provider.get(), filepaths, app_candidates);

	// Main application selection menu loop.
	while(true) {

		if (app_candidates.empty()) {
			std::vector<std::wstring> error_lines = { GetMsg(MNoAppsFound) };
			const auto& unique_mimes = GetMimeProfiles(provider.get(), unique_mime_profiles_cache);
			error_lines.push_back(JoinStrings(unique_mimes, L"; "));
			ShowError(GetMsg(MError), error_lines);
			return;	// No application candidates; exit the plugin entirely.
		}

		std::vector<FarMenuItem> menu_items(app_candidates.size());
		for (size_t i = 0; i < app_candidates.size(); ++i) {
			menu_items[i].Text = app_candidates[i].name.c_str();
		}

		menu_items[active_menu_idx].Selected = true;

		// Display the menu and get the user's selection.
		int selected_menu_idx = s_info.Menu(s_info.ModuleNumber, -1, -1, 0, FMENU_WRAPMODE | FMENU_SHOWAMPERSAND | FMENU_CHANGECONSOLETITLE, GetMsg(MChooseApplication),
											L"F3 F9 Ctrl+Alt+F", L"Contents", BREAK_KEYS, &menu_break_code, menu_items.data(), static_cast<int>(menu_items.size()));

		if (selected_menu_idx == -1) {
			return; // User cancelled the menu (e.g., with Esc); exit the plugin entirely
		}

		active_menu_idx = selected_menu_idx;
		const auto& selected_app = app_candidates[selected_menu_idx];

		if (menu_break_code == KEY_F3_DETAILS) {
			std::vector<std::wstring> cmds = provider->ConstructLaunchCommands(selected_app, filepaths);
			// Repeat until user either launches the application or closes the dialog to go back.
			while (true) {
				bool wants_to_launch = ShowDetailsDialog(provider.get(), selected_app, filepaths, cmds,
														 GetMimeProfiles(provider.get(), unique_mime_profiles_cache));
				if (!wants_to_launch) {
					break; // User clicked "Close", break the inner loop to return to the main menu.
				}

				if (AskForLaunchConfirmation(selected_app, filepaths)) {
					LaunchApplication(selected_app, cmds); // Launch the application and exit the plugin entirely.
					return;
				}
			}

		} else if (menu_break_code == KEY_F9_OPTIONS) {
			const auto configure_result = ConfigureImpl();
			// Refresh is needed if any setting that affects the candidate list has been changed.
			if (configure_result.settings_saved && configure_result.refresh_needed) {
				provider->LoadPlatformSettings();
				UpdateAppCandidates(provider.get(), filepaths, app_candidates);
				active_menu_idx = 0;
				unique_mime_profiles_cache.reset();
			}

		} else { // Enter to launch.
			if (AskForLaunchConfirmation(selected_app, filepaths)) {
				std::vector<std::wstring> cmds = provider->ConstructLaunchCommands(selected_app, filepaths);
				LaunchApplication(selected_app, cmds);
				return; // Exit the plugin after a successful launch.
			}
		}
	}
}


// Lazy loader for MIME profiles. Fetches data from the provider only if requested.
const std::vector<std::wstring>& OpenWithPlugin::GetMimeProfiles(AppProvider* provider,  std::optional<std::vector<std::wstring>>& cache)
{
	if (!cache.has_value()) {
		cache = provider->GetMimeTypes();
	}
	return *cache;
}


// Fetch and filter application candidates.
void OpenWithPlugin::UpdateAppCandidates(AppProvider* provider, const std::vector<std::wstring>& filepaths, std::vector<CandidateInfo>& candidates)
{
	// Fetch the raw list of candidates from the platform-specific provider.
	candidates = provider->GetAppCandidates(filepaths);

	// When multiple files are selected and the internal far2l console is used, we must filter out terminal-based applications
	// because the internal console cannot manage multiple concurrent instances.
	if (filepaths.size() > 1 && !s_use_external_terminal) {
		candidates.erase(
			std::remove_if(candidates.begin(), candidates.end(),
						   [](const CandidateInfo& c) {
							   return c.terminal && !c.multi_file_aware;
						   }),
			candidates.end());
	}
}


// Loads platform-independent configuration from the INI file.
void OpenWithPlugin::LoadOptions()
{
	KeyFileReadSection kfh(INI_LOCATION, INI_SECTION);
	s_use_external_terminal = kfh.GetInt("UseExternalTerminal", 0) != 0;
	s_no_wait_for_command_completion = kfh.GetInt("NoWaitForCommandCompletion", 1) != 0;
	s_clear_selection = kfh.GetInt("ClearSelection", 0) != 0;
	s_confirm_launch = kfh.GetInt("ConfirmLaunch", 1) != 0;
	s_confirm_launch_threshold = kfh.GetInt("ConfirmLaunchThreshold", 10);
	s_confirm_launch_threshold = std::clamp(s_confirm_launch_threshold, 1, 9999);
}


// Saves current platform-independent configuration to the INI file.
void OpenWithPlugin::SaveOptions()
{
	KeyFileHelper kfh(INI_LOCATION);
	kfh.SetInt(INI_SECTION, "UseExternalTerminal", s_use_external_terminal);
	kfh.SetInt(INI_SECTION, "NoWaitForCommandCompletion", s_no_wait_for_command_completion);
	kfh.SetInt(INI_SECTION, "ClearSelection", s_clear_selection);
	kfh.SetInt(INI_SECTION, "ConfirmLaunch", s_confirm_launch);
	s_confirm_launch_threshold = std::clamp(s_confirm_launch_threshold, 1, 9999);
	kfh.SetInt(INI_SECTION, "ConfirmLaunchThreshold", s_confirm_launch_threshold);
	if (!kfh.Save()) {
		ShowError(GetMsg(MError), { GetMsg(MSaveConfigError) });
	}
}


// Error reporting wrapper around the far2l message box API.
void OpenWithPlugin::ShowError(const wchar_t *title, const std::vector<std::wstring>& text)
{
	std::vector<const wchar_t*> items;
	items.reserve(text.size() + 2);
	items.push_back(title);
	for (const auto &line : text) items.push_back(line.c_str());
	items.push_back(GetMsg(MOk));
	s_info.Message(s_info.ModuleNumber, FMSG_WARNING, nullptr, items.data(), items.size(), 1);
}


// Joins a vector of strings with a specified delimiter.
std::wstring OpenWithPlugin::JoinStrings(const std::vector<std::wstring>& vec, const std::wstring& delimiter)
{
	if (vec.empty()) {
		return L"";
	}
	std::wstring result = vec[0];
	for (size_t i = 1; i < vec.size(); ++i) {
		result += delimiter;
		result += vec[i];
	}
	return result;
}


// Gets the console cell width of a field's label string.
size_t OpenWithPlugin::GetLabelCellWidth(const Field& field)
{
	return s_fsf.StrCellsCount(field.label.c_str(), field.label.size());
}


// Finds the maximum label width (in cells) in a vector of Fields for alignment.
size_t OpenWithPlugin::GetMaxLabelCellWidth(const std::vector<Field>& fields)
{
	size_t max_width = 0;
	for (const auto& field : fields) {
		max_width = std::max(max_width, GetLabelCellWidth(field));
	}
	return max_width;
}


// Retrieves the current console width using AdvControl API.
int OpenWithPlugin::GetScreenWidth()
{
	SMALL_RECT rect;
	if (s_info.AdvControl(s_info.ModuleNumber, ACTL_GETFARRECT, &rect, 0)) {
		return rect.Right - rect.Left + 1;
	}
	return 0;
}


// Static member initialization.
PluginStartupInfo OpenWithPlugin::s_info = {};
FarStandardFunctions OpenWithPlugin::s_fsf = {};
bool OpenWithPlugin::s_use_external_terminal = false;
bool OpenWithPlugin::s_no_wait_for_command_completion = true;
bool OpenWithPlugin::s_clear_selection = false;
bool OpenWithPlugin::s_confirm_launch = true;
int OpenWithPlugin::s_confirm_launch_threshold = 10;



// Plugin entry points

SHAREDSYMBOL void WINAPI SetStartupInfoW(const PluginStartupInfo *Info)
{
	OpenWith::OpenWithPlugin::SetStartupInfo(Info);
}

SHAREDSYMBOL void WINAPI GetPluginInfoW(PluginInfo *Info)
{
	OpenWith::OpenWithPlugin::GetPluginInfo(Info);
}

SHAREDSYMBOL HANDLE WINAPI OpenPluginW(int OpenFrom, INT_PTR Item)
{
	return OpenWith::OpenWithPlugin::OpenPlugin(OpenFrom, Item);
}

SHAREDSYMBOL int WINAPI ConfigureW(int ItemNumber)
{
	return OpenWith::OpenWithPlugin::Configure(ItemNumber);
}

SHAREDSYMBOL void WINAPI ExitFARW()
{
	OpenWith::OpenWithPlugin::Exit();
}

SHAREDSYMBOL int WINAPI GetMinFarVersionW()
{
	return FARMANAGERVERSION;
}

} // namespace OpenWith
