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

#define INI_LOCATION InMyConfig("plugins/openwith/config.ini")
#define INI_SECTION  "Settings"

namespace OpenWith {


class OpenWithPlugin
{
private:
	static PluginStartupInfo s_Info;
	static FarStandardFunctions s_FSF;
	static bool s_UseExternalTerminal;
	static bool s_NoWaitForCommandCompletion;

	// return true if exit by button "Launch", false otherwise
	static bool ShowDetailsDialogImpl(const std::vector<Field>& file_info,
									  const std::vector<Field>& application_info,
									  const Field& launch_command)
	{
		constexpr int MIN_DIALOG_WIDTH = 40;
		constexpr int DESIRED_DIALOG_WIDTH = 90;

		int max_dialog_width = std::max(MIN_DIALOG_WIDTH, GetScreenWidth() - 4);
		int dialog_width = std::clamp(DESIRED_DIALOG_WIDTH, MIN_DIALOG_WIDTH, max_dialog_width);

		int dialog_height = file_info.size() + application_info.size() + 9;

		// Helper lambda to find the maximum label length in a vector of Fields for alignment.
		auto max_in = [](const std::vector<Field>& v) -> size_t {
			if (v.empty()) return 0;
			return std::max_element(v.begin(), v.end(),
									[](const Field& x, const Field& y){ return x.label.size() < y.label.size(); })->label.size();
		};

		auto max_di_text_length = static_cast<int>(std::max({
			launch_command.label.size(),
			max_in(file_info),
			max_in(application_info)
		}));

		// Calculate coordinates for dialog items to right-align all text labels.
		int di_text_X2 = max_di_text_length + 4;
		int di_edit_X1 = max_di_text_length + 6;
		int di_edit_X2 = dialog_width - 6;

		std::vector<FarDialogItem> di;

		di.push_back({ DI_DOUBLEBOX, 3,  1, dialog_width - 4,  dialog_height - 2, FALSE, {}, 0, 0, GetMsg(MDetails), 0 });

		int cur_line = 2;

		for (auto &field : file_info) {
			int di_text_X1 = di_text_X2 - field.label.size() + 1;
			di.push_back({ DI_TEXT, di_text_X1, cur_line,  di_text_X2, cur_line, FALSE, {}, 0, 0, field.label.c_str(), 0 });
			di.push_back({ DI_EDIT, di_edit_X1, cur_line,  di_edit_X2, cur_line, FALSE, {}, DIF_READONLY | DIF_SELECTONENTRY, 0,  field.content.c_str(), 0});
			++cur_line;
		}

		di.push_back({ DI_TEXT, 5,  cur_line,  0,  cur_line, FALSE, {}, DIF_SEPARATOR, 0, L"", 0 });
		++cur_line;

		for (auto &field : application_info) {
			int di_text_X1 = di_text_X2 - field.label.size() + 1;
			di.push_back({ DI_TEXT, di_text_X1, cur_line,  di_text_X2, cur_line, FALSE, {}, 0, 0, field.label.c_str(), 0 });
			di.push_back({ DI_EDIT, di_edit_X1, cur_line,  di_edit_X2, cur_line, FALSE, {}, DIF_READONLY | DIF_SELECTONENTRY, 0,  field.content.c_str(), 0});
			++cur_line;
		}

		di.push_back({ DI_TEXT, 5,  cur_line,  0,  cur_line, FALSE, {}, DIF_SEPARATOR, 0, L"", 0 });
		++cur_line;

		int di_text_X1 = di_text_X2 - launch_command.label.size() + 1;
		di.push_back({ DI_TEXT, di_text_X1, cur_line,  di_text_X2, cur_line, FALSE, {}, 0, 0, launch_command.label.c_str(), 0 });
		di.push_back({ DI_EDIT, di_edit_X1, cur_line,  di_edit_X2, cur_line, FALSE, {}, DIF_READONLY | DIF_SELECTONENTRY, 0,  launch_command.content.c_str(), 0});
		++cur_line;

		di.push_back({ DI_TEXT, 5,  cur_line,  0,  cur_line, FALSE, {}, DIF_SEPARATOR, 0, L"", 0 });
		++cur_line;

		di.push_back({ DI_BUTTON, 0,  cur_line,  0,  cur_line, TRUE, {}, DIF_CENTERGROUP, 0, GetMsg(MClose), 0 });

		di.back().DefaultButton = TRUE;

		di.push_back({ DI_BUTTON, 0,  cur_line,  0,  cur_line, TRUE, {}, DIF_CENTERGROUP, 0, GetMsg(MLaunch), 0 });

		HANDLE dlg = s_Info.DialogInit(s_Info.ModuleNumber, -1, -1, dialog_width, dialog_height, L"InformationDialog",
									   di.data(), static_cast<int>(di.size()), 0, 0, nullptr, 0);
		if (dlg != INVALID_HANDLE_VALUE) {
			int exitCode = s_Info.DialogRun(dlg);
			s_Info.DialogFree(dlg);
			return (exitCode == (int)di.size() - 1); // last element is button "Launch"
		}
		return false;
	}


	// Shows the details dialog with file and application information.
	// For a single file, it shows the full path. For multiple files, it shows a count.
	// It also removes ambiguous information (like association source) for multi-file selections.
	static bool ShowDetailsDialog(AppProvider* provider, const CandidateInfo& app,
								  const std::vector<std::wstring>& pathnames,
								  const std::vector<std::wstring>& cmds)
	{
		// Helper lambda to join a vector of wstrings into a single wstring.
		auto join_strings = [](const std::vector<std::wstring>& vec, const std::wstring& delimiter) -> std::wstring {
			if (vec.empty()) return L"";
			std::wstring result;
			for (size_t i = 0; i < vec.size(); ++i) {
				if (i > 0) result += delimiter;
				result += vec[i];
			}
			return result;
		};

		std::vector<Field> file_info;
		if (pathnames.size() == 1) {
			// For a single file, show its full path.
			file_info.push_back({ GetMsg(MPathname), pathnames[0] });
		} else {
			// For multiple files, show a summary count.
			std::wstring count_msg = std::wstring(GetMsg(MFilesSelected)) + L" " + std::to_wstring(pathnames.size());
			file_info.push_back({ GetMsg(MPathname), count_msg });
		}

		// Get unique MIME types for all files and join them for display.
		std::vector<std::wstring> unique_mimes = provider->GetMimeTypes(pathnames);
		file_info.push_back({ GetMsg(MMimeType), join_strings(unique_mimes, L"; ") });

		std::wstring all_cmds = join_strings(cmds, L"; ");

		std::vector<Field> application_info = provider->GetCandidateDetails(app);
		if (pathnames.size() > 1) {
			// For multiple files, "Source" is ambiguous and should be removed.
			const wchar_t* source_msg = GetMsg(MSource);
			application_info.erase(
				std::remove_if(application_info.begin(), application_info.end(),
							   [source_msg](const Field& f){ return f.label == source_msg; }),
				application_info.end()
				);
		}

		Field launch_command { GetMsg(MLaunchCommand), all_cmds.c_str() };

		return ShowDetailsDialogImpl(file_info, application_info, launch_command);
	}



	// Executes one or more command lines to launch the application.
	// If multiple commands are provided, it forces asynchronous execution to avoid blocking the UI.
	static void LaunchApplication(const CandidateInfo& app, const std::vector<std::wstring>& cmds)
	{
		if (cmds.empty()) return;

		// If we have multiple commands to run, force asynchronous execution to avoid blocking.
		bool force_no_wait = cmds.size() > 1;

		unsigned int flags = 0;
		if (app.terminal) {
			flags = s_UseExternalTerminal ? EF_EXTERNALTERM : 0;
		} else {
			flags = (s_NoWaitForCommandCompletion || force_no_wait) ? EF_NOWAIT : 0;
		}

		for (const auto& cmd : cmds) {
			if (s_FSF.Execute(cmd.c_str(), flags) == -1) {
				ShowError(GetMsg(MError), { GetMsg(MCannotExecute), cmd.c_str() });
				break; // Stop on the first error.
			}
		}
	}


	// A simple wrapper for ProcessFiles to handle single-file cases.
	static void ProcessFile(const std::wstring &pathname)
	{
		ProcessFiles({pathname});
	}


	// The main logic handler for both single and multiple files.
	// It gets candidate applications, displays a menu, and handles user actions.
	static void ProcessFiles(const std::vector<std::wstring>& pathnames)
	{
		if (pathnames.empty()) {
			return;
		}

		auto provider = AppProvider::CreateAppProvider(&OpenWithPlugin::GetMsg);
		auto candidates = provider->GetAppCandidates(pathnames);

		// For multiple files, filter out terminal-based apps if the internal console is used,
		// as we cannot launch multiple instances of it.
		if (pathnames.size() > 1 && !s_UseExternalTerminal) {
			candidates.erase(
				std::remove_if(candidates.begin(), candidates.end(),
							   [](const CandidateInfo& c){ return c.terminal; }),
				candidates.end());
		}

		int BreakCode = -1;
		const int BreakKeys[] = {VK_F3, VK_F9, 0};
		int active_idx = 0;

		// A local helper lambda to join strings, needed for the error message.
		auto join_strings = [](const std::vector<std::wstring>& vec, const std::wstring& delimiter) -> std::wstring {
			if (vec.empty()) return L"";
			std::wstring result;
			for (size_t i = 0; i < vec.size(); ++i) {
				if (i > 0) result += delimiter;
				result += vec[i];
			}
			return result;
		};

		while(true) {
			if (candidates.empty()) {
				std::vector<std::wstring> error_lines = { GetMsg(MNoAppsFound) };

				auto unique_mimes = provider->GetMimeTypes(pathnames);

				if (pathnames.size() == 1) {
					// For a single file, show its list of MIME types.
					if (!unique_mimes.empty()) {
						error_lines.push_back(join_strings(unique_mimes, L"; "));
					}
				} else {
					// For multiple files, show a more informative count of unique MIME types found.
					std::wstring count_msg = std::wstring(GetMsg(MNumberOfMimeTypesFound)) + L" " + std::to_wstring(unique_mimes.size());
					error_lines.push_back(count_msg);
				}

				ShowError(GetMsg(MError), error_lines);
				break;
			}

			std::vector<FarMenuItem> menu_items(candidates.size());
			for (size_t i = 0; i < candidates.size(); ++i) {
				menu_items[i].Text = candidates[i].name.c_str();
			}

			menu_items[active_idx].Selected = true;

			int selected_idx = s_Info.Menu(s_Info.ModuleNumber, -1, -1, 0, FMENU_WRAPMODE | FMENU_SHOWAMPERSAND | FMENU_CHANGECONSOLETITLE,
										   GetMsg(MChooseApplication), L"F3 F9 Ctrl+Alt+F", L"Contents", BreakKeys, &BreakCode, menu_items.data(), menu_items.size());

			if (selected_idx == -1) {
				break;
			}

			menu_items[active_idx].Selected = false;
			active_idx = selected_idx;
			const auto& selected_app = candidates[selected_idx];

			if (BreakCode == 0) { // F3 for Details
				std::vector<std::wstring> cmds = provider->ConstructCommandLine(selected_app, pathnames);
				if (ShowDetailsDialog(provider.get(), selected_app, pathnames, cmds)) {
					LaunchApplication(selected_app, cmds);
					break;
				}
			} else if (BreakCode == 1) { // F9 for Options
				const auto configure_result = ConfigureImpl();
				// If settings affecting the app list were changed, refresh the list.
				if (configure_result.settings_saved && configure_result.refresh_needed) {
					provider->LoadPlatformSettings();
					candidates = provider->GetAppCandidates(pathnames);
					if (pathnames.size() > 1 && !s_UseExternalTerminal) {
						candidates.erase(
							std::remove_if(candidates.begin(), candidates.end(),
										   [](const CandidateInfo& c){ return c.terminal; }),
							candidates.end());
					}
					active_idx = 0;
				}
			} else { // Enter to launch
				std::vector<std::wstring> cmds = provider->ConstructCommandLine(selected_app, pathnames);
				LaunchApplication(selected_app, cmds);
				break;
			}
		}
	}



	static void LoadOptions()
	{
		KeyFileReadSection kfh(INI_LOCATION, INI_SECTION);
		s_UseExternalTerminal = kfh.GetInt("UseExternalTerminal", 0) != 0;
		s_NoWaitForCommandCompletion = kfh.GetInt("NoWaitForCommandCompletion", 1) != 0;
	}


	static void SaveOptions()
	{
		KeyFileHelper kfh(INI_LOCATION);
		kfh.SetInt(INI_SECTION, "UseExternalTerminal", s_UseExternalTerminal);
		kfh.SetInt(INI_SECTION, "NoWaitForCommandCompletion", s_NoWaitForCommandCompletion);
		if (!kfh.Save()) {
			ShowError(GetMsg(MError), { GetMsg(MSaveConfigError) });
		}
	}


	static void ShowError(const wchar_t *title, const std::vector<std::wstring>& text)
	{
		std::vector<const wchar_t*> items;
		items.reserve(text.size() + 2);
		items.push_back(title);
		for (const auto &line : text) items.push_back(line.c_str());
		items.push_back(GetMsg(MOk));
		s_Info.Message(s_Info.ModuleNumber, FMSG_WARNING, nullptr, items.data(), items.size(), 1);
	}


	static int GetScreenWidth()
	{
		CONSOLE_SCREEN_BUFFER_INFO ConsoleScreenBufferInfo;
		if (WINPORT(GetConsoleScreenBufferInfo)(NULL, &ConsoleScreenBufferInfo))
			return ConsoleScreenBufferInfo.dwSize.X;
		return 0;
	}


public:
	// Standard far2l plugin entry point for initialization.
	static void SetStartupInfo(const PluginStartupInfo *info)
	{
		s_Info = *info;
		s_FSF = *info->FSF;
		s_Info.FSF = &s_FSF;
		LoadOptions();
	}


	// Standard far2l plugin entry point to provide information about the plugin.
	static void GetPluginInfo(PluginInfo *info)
	{
		info->StructSize = sizeof(*info);
		info->Flags = 0;
		static const wchar_t *menuStr[1];
		menuStr[0] = GetMsg(MPluginTitle);
		info->PluginMenuStrings = menuStr;
		info->PluginMenuStringsNumber = ARRAYSIZE(menuStr);
		static const wchar_t *configStr[1];
		configStr[0] = GetMsg(MPluginTitle);
		info->PluginConfigStrings = configStr;
		info->PluginConfigStringsNumber = ARRAYSIZE(configStr);
		info->CommandPrefix = nullptr;
	}


	// Main plugin entry point, called when the user activates the plugin from the menu.
	// It collects selected file paths from the active panel and initiates processing.
	static HANDLE OpenPlugin(int openFrom, INT_PTR item)
	{
		if (openFrom != OPEN_PLUGINSMENU) {
			return INVALID_HANDLE_VALUE;
		}

		PanelInfo pi = {};
		if (!s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, (LONG_PTR)&pi)) {
			return INVALID_HANDLE_VALUE;
		}

		if (pi.PanelType != PTYPE_FILEPANEL || pi.ItemsNumber <= 0) {
			return INVALID_HANDLE_VALUE;
		}

		std::vector<std::wstring> selected_pathnames;

		// First, get the panel's directory path.
		int dir_size = s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELDIR, 0, 0);
		if (dir_size <= 0) {
			return INVALID_HANDLE_VALUE;
		}
		auto dir_buf = std::make_unique<wchar_t[]>(dir_size);
		if (!s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELDIR, dir_size, (LONG_PTR)dir_buf.get())) {
			return INVALID_HANDLE_VALUE;
		}
		std::wstring base_path(dir_buf.get());
		if (!base_path.empty() && base_path.back() != L'/') {
			base_path += L'/';
		}

		// If files are selected, iterate through them.
		if (pi.SelectedItemsNumber > 0) {
			selected_pathnames.reserve(pi.SelectedItemsNumber);
			for (size_t i = 0; i < pi.SelectedItemsNumber; ++i) {
				int itemSize = s_Info.Control(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, i, 0);
				if (itemSize <= 0) continue;

				auto item_buf = std::make_unique<unsigned char[]>(itemSize);
				PluginPanelItem* pi_item = reinterpret_cast<PluginPanelItem*>(item_buf.get());

				if (s_Info.Control(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, i, (LONG_PTR)pi_item) && pi_item->FindData.lpwszFileName) {
					selected_pathnames.push_back(base_path + pi_item->FindData.lpwszFileName);
				}
			}
		}
		// Otherwise, use the current file under the cursor.
		else if (pi.CurrentItem >= 0 && pi.CurrentItem < pi.ItemsNumber) {
			int itemSize = s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELITEM, pi.CurrentItem, 0);
			if (itemSize > 0) {
				auto item_buf = std::make_unique<unsigned char[]>(itemSize);
				PluginPanelItem* pi_item = reinterpret_cast<PluginPanelItem*>(item_buf.get());
				if (s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELITEM, pi.CurrentItem, (LONG_PTR)pi_item) && pi_item->FindData.lpwszFileName) {
					selected_pathnames.push_back(base_path + pi_item->FindData.lpwszFileName);
				}
			}
		}

		if (!selected_pathnames.empty()) {
			ProcessFiles(selected_pathnames);
		}

		return INVALID_HANDLE_VALUE;
	}


	// A structure to hold detailed results from the configuration dialog.
	// This allows communicating more than just a simple success/failure status.
	struct ConfigureResult
	{
		bool settings_saved = false;  // True if the user clicked "Ok" and settings were saved.
		bool refresh_needed = false; // True if a setting affecting the candidate list was changed.
	};


	// The core implementation of the configuration logic.
	// It returns a detailed result, allowing the caller to know if the application list needs to be refreshed.
	static ConfigureResult ConfigureImpl()
	{
		LoadOptions();

		auto provider = AppProvider::CreateAppProvider(&OpenWithPlugin::GetMsg);
		provider->LoadPlatformSettings();
		// Store the state of platform-specific settings *before* showing the dialog.
		// This is crucial for detecting changes later.
		std::vector<ProviderSetting> old_platform_settings = provider->GetPlatformSettings();

		std::vector<FarDialogItem> di;
		int y = 1;

		di.push_back({ DI_CHECKBOX, 5, ++y, 0, 0, TRUE, { s_UseExternalTerminal }, 0, 0, GetMsg(MUseExternalTerminal), 0 });
		di.push_back({ DI_CHECKBOX, 5, ++y, 0, 0, 0, { s_NoWaitForCommandCompletion },  0, 0, GetMsg(MNoWaitForCommandCompletion), 0});

		if (!old_platform_settings.empty()) {
			di.push_back({ DI_TEXT, 5, ++y, 0, 0, FALSE, {}, DIF_SEPARATOR, 0, L"", 0 });
			for (const auto& setting : old_platform_settings) {
				di.push_back({ DI_CHECKBOX, 5, ++y, 0, 0, FALSE, { setting.value }, 0, 0, setting.display_name.c_str(), 0 });
			}
		}

		di.push_back({ DI_TEXT, 5, ++y, 0, 0, FALSE, {}, DIF_SEPARATOR, 0, L"", 0 });
		y++;
		di.push_back({ DI_BUTTON, 0, y, 0, 0, FALSE, {}, DIF_CENTERGROUP, 0, GetMsg(MOk), 0 });
		di.back().DefaultButton = TRUE;
		di.push_back({ DI_BUTTON, 0, y, 0, 0, FALSE, {}, DIF_CENTERGROUP, 0, GetMsg(MCancel), 0 });

		int dialog_height = y + 3;
		int dialog_width = 70;
		di.insert(di.begin(), { DI_DOUBLEBOX, 3, 1, dialog_width - 4, dialog_height - 2, FALSE, {}, 0, 0, GetMsg(MConfigTitle), 0 });

		HANDLE dlg = s_Info.DialogInit(s_Info.ModuleNumber, -1, -1, dialog_width, dialog_height, L"ConfigurationDialog", di.data(), di.size(), 0, 0, nullptr, 0);
		if (dlg == INVALID_HANDLE_VALUE) {
			return {}; // Return a default (all false) result on dialog initialization failure.
		}

		int exitCode = s_Info.DialogRun(dlg);
		ConfigureResult result;

		// The index of the 'OK' button is determined by its position in the 'di' vector.
		if (exitCode == (int)di.size() - 2) { // OK was clicked
			result.settings_saved = true;

			// Save platform-independent settings
			s_UseExternalTerminal = (s_Info.SendDlgMessage(dlg, DM_GETCHECK, 1, 0) == BSTATE_CHECKED);
			s_NoWaitForCommandCompletion = (s_Info.SendDlgMessage(dlg, DM_GETCHECK, 2, 0) == BSTATE_CHECKED);
			SaveOptions();

			if (!old_platform_settings.empty()) {
				std::vector<ProviderSetting> new_settings;
				int first_platform_item_idx = 4; // Index of the first platform-specific checkbox
				bool list_needs_refresh = false;

				for (size_t i = 0; i < old_platform_settings.size(); ++i) {
					bool new_value = (s_Info.SendDlgMessage(dlg, DM_GETCHECK, first_platform_item_idx + i, 0) == BSTATE_CHECKED);
					// If any platform-specific setting has changed, the candidate list must be regenerated.
					if (old_platform_settings[i].value != new_value) {
						list_needs_refresh = true;
					}
					new_settings.push_back({ old_platform_settings[i].internal_key, old_platform_settings[i].display_name, new_value });
				}
				provider->SetPlatformSettings(new_settings);
				provider->SavePlatformSettings();

				if (list_needs_refresh) {
					result.refresh_needed = true;
				}
			}
		}

		s_Info.DialogFree(dlg);
		return result;
	}


	// The public Configure function called by far2l.
	// It acts as a simple wrapper around the core implementation, returning only
	// the TRUE/FALSE value required by the API.
	static int Configure(int itemNumber)
	{
		return ConfigureImpl().settings_saved;
	}




	static void Exit() {}


	static const wchar_t* GetMsg(int MsgId)
	{
		return s_Info.GetMsg(s_Info.ModuleNumber, MsgId);
	}
};

// Static member initialization.
PluginStartupInfo OpenWithPlugin::s_Info = {};
FarStandardFunctions OpenWithPlugin::s_FSF = {};
bool OpenWithPlugin::s_UseExternalTerminal = false;
bool OpenWithPlugin::s_NoWaitForCommandCompletion = true;

// Plugin entry points

SHAREDSYMBOL void WINAPI SetStartupInfoW(const PluginStartupInfo *info)
{
	OpenWith::OpenWithPlugin::SetStartupInfo(info);
}

SHAREDSYMBOL void WINAPI GetPluginInfoW(PluginInfo *info)
{
	OpenWith::OpenWithPlugin::GetPluginInfo(info);
}

SHAREDSYMBOL HANDLE WINAPI OpenPluginW(int openFrom, INT_PTR item)
{
	return OpenWith::OpenWithPlugin::OpenPlugin(openFrom, item);
}

SHAREDSYMBOL int WINAPI ConfigureW(int itemNumber)
{
	return OpenWith::OpenWithPlugin::Configure(itemNumber);
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
