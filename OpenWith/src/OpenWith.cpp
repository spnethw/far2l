#include "AppProvider.hpp"
#include "farplug-wide.h"
#include "KeyFileHelper.h"
#include "WinCompat.h"
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

	struct Field
	{
		std::wstring di_text;
		std::wstring di_edit;
	};


	static void ShowDetailsDialogImpl(const std::vector<Field>& file_info, const std::vector<Field>& application_info,
								  const Field& launch_command)
	{
		constexpr int DIALOG_WIDTH = 70;
		int dialog_height = file_info.size() + application_info.size() + 9;

		auto max_in = [](const std::vector<Field>& v) -> size_t {
			if (v.empty()) return 0;
			return std::max_element(v.begin(), v.end(),
				[](const Field& x, const Field& y){ return x.di_text.size() < y.di_text.size(); })->di_text.size();
		};

		auto max_di_text_length = static_cast<int>(std::max(launch_command.di_text.size(), std::max(max_in(file_info), max_in(application_info))));

		int di_text_X2 = max_di_text_length + 4;
		int di_edit_X1 = max_di_text_length + 6;
		int di_edit_X2 = DIALOG_WIDTH - 6;

		std::vector<FarDialogItem> di;

		di.push_back({ DI_DOUBLEBOX, 3,  1, DIALOG_WIDTH - 4,  dialog_height - 2, FALSE, {}, 0, 0, GetMsg(MDetails), 0 });

		int cur_line = 2;

		for (auto &field : file_info) {
			int di_text_X1 = di_text_X2 - field.di_text.size() + 1;
			di.push_back({ DI_TEXT, di_text_X1, cur_line,  di_text_X2, cur_line, FALSE, {}, 0, 0, field.di_text.c_str(), 0 });
			di.push_back({ DI_EDIT, di_edit_X1, cur_line,  di_edit_X2, cur_line, FALSE, {}, DIF_READONLY, 0,  field.di_edit.c_str(), 0});
			++cur_line;
		}

		di.push_back({ DI_TEXT, 5,  cur_line,  0,  cur_line, FALSE, {}, DIF_SEPARATOR, 0, L"", 0 });
		++cur_line;

		for (auto &field : application_info) {
			int di_text_X1 = di_text_X2 - field.di_text.size() + 1;
			di.push_back({ DI_TEXT, di_text_X1, cur_line,  di_text_X2, cur_line, FALSE, {}, 0, 0, field.di_text.c_str(), 0 });
			di.push_back({ DI_EDIT, di_edit_X1, cur_line,  di_edit_X2, cur_line, FALSE, {}, DIF_READONLY, 0,  field.di_edit.c_str(), 0});
			++cur_line;
		}

		di.push_back({ DI_TEXT, 5,  cur_line,  0,  cur_line, FALSE, {}, DIF_SEPARATOR, 0, L"", 0 });
		++cur_line;

		int di_text_X1 = di_text_X2 - launch_command.di_text.size() + 1;
		di.push_back({ DI_TEXT, di_text_X1, cur_line,  di_text_X2, cur_line, FALSE, {}, 0, 0, launch_command.di_text.c_str(), 0 });
		di.push_back({ DI_EDIT, di_edit_X1, cur_line,  di_edit_X2, cur_line, FALSE, {}, DIF_READONLY, 0,  launch_command.di_edit.c_str(), 0});
		++cur_line;

		di.push_back({ DI_TEXT, 5,  cur_line,  0,  cur_line, FALSE, {}, DIF_SEPARATOR, 0, L"", 0 });
		++cur_line;

		di.push_back({ DI_BUTTON, 0,  cur_line,  0,  cur_line, FALSE, {}, DIF_CENTERGROUP, 0, GetMsg(MOk), 0 });

		di.back().DefaultButton = TRUE;

		HANDLE dlg = s_Info.DialogInit(s_Info.ModuleNumber, -1, -1, DIALOG_WIDTH, dialog_height, L"",
										di.data(), static_cast<int>(di.size()), 0, 0, nullptr, 0);
		if (dlg != INVALID_HANDLE_VALUE) {
			s_Info.DialogRun(dlg);
			s_Info.DialogFree(dlg);
		}
	}


	static void ShowDetailsDialog(AppProvider* provider, const CandidateInfo& app,
										  const std::wstring& pathname, const std::wstring& cmd)
	{
		std::vector<Field> file_info = {
			{ GetMsg(MPathname), pathname },
			{ GetMsg(MMimeType), provider->GetMimeType(pathname) }
		};

		std::vector<Field> application_info = {
			{ GetMsg(MDesktopFile), app.desktop_file.c_str() },
			{ L"Name =", app.name.c_str() },
			{ L"Exec =", app.exec.c_str() },
			{ L"Terminal =", app.terminal ? L"true" : L"false" },
			{ L"MimeType =", app.mimetype.c_str() },
		};

		Field launch_command { GetMsg(MLaunchCommand), cmd.c_str() };

		ShowDetailsDialogImpl(file_info, application_info, launch_command);
	}


	static void LaunchApplication(const CandidateInfo& app, const std::wstring& cmd)
	{
		unsigned int flags = 0;
		if (app.terminal) {
			flags = s_UseExternalTerminal ? EF_EXTERNALTERM : 0;
		} else {
			flags = s_NoWaitForCommandCompletion ? EF_NOWAIT : 0;
		}
		if (s_FSF.Execute(cmd.c_str(), flags) == -1) {
			ShowError(GetMsg(MError), GetMsg(MCannotExecute));
		}
	}


	static void ProcessFile(const std::wstring &pathname)
	{
		auto provider = AppProvider::CreateAppProvider();
		auto candidates = provider->GetAppCandidates(pathname);

		if (candidates.empty()) {
			ShowError(GetMsg(MError), GetMsg(MNoAppsFound));
			return;
		}

		std::vector<FarMenuItem> menu_items(candidates.size());
		for (std::size_t i = 0; i < candidates.size(); ++i) {
			menu_items[i].Text = candidates[i].name.c_str();
		}

		int BreakCode = -1;
		const int BreakKeys[] = {VK_F3, 0};
		int active_idx = 0;

		while(true) {
			menu_items[active_idx].Selected = true;

			int selected_idx = s_Info.Menu(s_Info.ModuleNumber, -1, -1, 0, FMENU_WRAPMODE | FMENU_SHOWAMPERSAND | FMENU_CHANGECONSOLETITLE,
							GetMsg(MChooseApplication), L"F3 Ctrl+Alt+F", nullptr, BreakKeys, &BreakCode, menu_items.data(), menu_items.size());

			if (selected_idx == -1) {
				break;
			}

			menu_items[active_idx].Selected = false;
			active_idx = selected_idx;

			const auto& selected_app = candidates[selected_idx];
			std::wstring cmd = provider->ConstructCommandLine(selected_app, pathname);

			if (BreakCode == 0) { // F3
				ShowDetailsDialog(provider.get(), selected_app, pathname, cmd);
			} else { // Enter
				LaunchApplication(selected_app, cmd);
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
			ShowError(GetMsg(MError), GetMsg(MSaveConfigError));
		}
	}

	static void ShowError(const wchar_t *title, const wchar_t *text)
	{
		const wchar_t *items[] = { title, text, GetMsg(MOk) };
		s_Info.Message(s_Info.ModuleNumber, FMSG_WARNING, nullptr, items, ARRAYSIZE(items), 1);
	}

public:
	static void SetStartupInfo(const PluginStartupInfo *info)
	{
		s_Info = *info;
		s_FSF = *info->FSF;
		s_Info.FSF = &s_FSF;
		LoadOptions();
	}

	static void GetPluginInfo(PluginInfo *info)
	{
		info->StructSize = sizeof(*info);
		info->Flags = 0;
		static const wchar_t *menuStr[1];
		menuStr[0] = GetMsg(MPluginTitle);
		info->PluginMenuStrings = menuStr;
		info->PluginMenuStringsNumber = ARRAYSIZE(menuStr);
		static const wchar_t *configStr[1];
		configStr[0] = GetMsg(MConfigTitle);
		info->PluginConfigStrings = configStr;
		info->PluginConfigStringsNumber = ARRAYSIZE(configStr);
		info->CommandPrefix = nullptr;
	}


	static HANDLE OpenPlugin(int openFrom, INT_PTR item)
	{
		if (openFrom != OPEN_PLUGINSMENU) {
			fprintf(stderr, "OpenWith: Invalid openFrom=%d\n", openFrom);
			return INVALID_HANDLE_VALUE;
		}

		PanelInfo pi = {};

		if (!s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, (LONG_PTR)&pi)) {
			fprintf(stderr, "OpenWith: Failed to get panel info\n");
			return INVALID_HANDLE_VALUE;
		}

		if (pi.PanelType != PTYPE_FILEPANEL) {
			fprintf(stderr, "OpenWith: Invalid panel type=%d, expected PTYPE_FILEPANEL\n", pi.PanelType);
			return INVALID_HANDLE_VALUE;
		}

		if (pi.ItemsNumber <= 0 || pi.CurrentItem < 0 || pi.CurrentItem >= pi.ItemsNumber) {
			fprintf(stderr, "OpenWith: Invalid panel state: ItemsNumber=%d, CurrentItem=%d\n", pi.ItemsNumber, pi.CurrentItem);
			return INVALID_HANDLE_VALUE;
		}

		int itemSize = s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELITEM, pi.CurrentItem, 0);
		if (itemSize <= 0) {
			fprintf(stderr, "OpenWith: Failed to get panel item size for CurrentItem=%d\n", pi.CurrentItem);
			return INVALID_HANDLE_VALUE;
		}

		auto item_buf = std::make_unique<unsigned char[]>(itemSize);
		PluginPanelItem *pi_item = reinterpret_cast<PluginPanelItem *>(item_buf.get());

		if (!s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELITEM, pi.CurrentItem, (LONG_PTR)pi_item)) {
			fprintf(stderr, "OpenWith: Failed to get panel item data for CurrentItem=%d\n", pi.CurrentItem);
			return INVALID_HANDLE_VALUE;
		}

		if (!pi_item->FindData.lpwszFileName) {
			fprintf(stderr, "OpenWith: Null filename for CurrentItem=%d\n", pi.CurrentItem);
			return INVALID_HANDLE_VALUE;
		}

		int dir_size = s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELDIR, 0, 0);
		if (dir_size <= 0) {
			fprintf(stderr, "OpenWith: Failed to get panel directory size\n");
			return INVALID_HANDLE_VALUE;
		}

		auto dir_buf = std::make_unique<wchar_t[]>(dir_size);
		if (!s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELDIR, dir_size, (LONG_PTR)dir_buf.get())) {
			fprintf(stderr, "OpenWith: Failed to get panel directory data\n");
			return INVALID_HANDLE_VALUE;
		}

		// POSIX-safe path concatenation
		std::wstring pathname(dir_buf.get());
		if (!pathname.empty() && pathname.back() != L'/') {
			pathname += L'/';
		}
		pathname += pi_item->FindData.lpwszFileName;

		ProcessFile(pathname);

		return INVALID_HANDLE_VALUE;
	}


	static int Configure(int itemNumber)
	{
		LoadOptions();

		FarDialogItem di[] = {
			{ DI_DOUBLEBOX,   3,  1, 66,  7, FALSE, {}, 0, 0, GetMsg(MConfigTitle), 0 },
			{ DI_CHECKBOX,    5,  2,  0,  0, TRUE,  {}, 0, 0, GetMsg(MUseExternalTerminal), 0 },
			{ DI_CHECKBOX,    5,  3,  0,  0, FALSE, {}, 0, 0, GetMsg(MNoWaitForCommandCompletion), 0},
			{ DI_TEXT,        5,  5,  0,  0, FALSE, {}, DIF_SEPARATOR, 0, L"", 0 },
			{ DI_BUTTON,      0,  6,  0,  0, FALSE, {}, DIF_CENTERGROUP, 0, GetMsg(MOk), 0 },
			{ DI_BUTTON,      0,  6,  0,  0, FALSE, {}, DIF_CENTERGROUP, 0, GetMsg(MCancel), 0 }
		};

		HANDLE dlg = s_Info.DialogInit(s_Info.ModuleNumber, -1, -1, 70, 9, L"OpenWithConfig", di, ARRAYSIZE(di), 0, 0, nullptr, 0);
		if (dlg == INVALID_HANDLE_VALUE) return FALSE;

		s_Info.SendDlgMessage(dlg, DM_SETCHECK, 1, s_UseExternalTerminal ? BSTATE_CHECKED : BSTATE_UNCHECKED);
		s_Info.SendDlgMessage(dlg, DM_SETCHECK, 2, s_NoWaitForCommandCompletion ? BSTATE_CHECKED : BSTATE_UNCHECKED);

		int exitCode = s_Info.DialogRun(dlg);
		if (exitCode == 4) { // OK button
			s_UseExternalTerminal = (s_Info.SendDlgMessage(dlg, DM_GETCHECK, 1, 0) == BSTATE_CHECKED);
			s_NoWaitForCommandCompletion = (s_Info.SendDlgMessage(dlg, DM_GETCHECK, 2, 0) == BSTATE_CHECKED);
			SaveOptions();
		}
		s_Info.DialogFree(dlg);
		return TRUE;
	}

	static void Exit() {}

	static const wchar_t* GetMsg(int MsgId)
	{
		return s_Info.GetMsg(s_Info.ModuleNumber, MsgId);
	}
};

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
