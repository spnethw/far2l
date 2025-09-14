// file: OpenWith.cpp - plugin for far2l : https://github.com/elfmz/far2l

#include "farplug-wide.h"
#include "KeyFileHelper.h"
#include "utils.h"
#include "AppProvider.hpp"
#include "lng.hpp"

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

		do {
			menu_items[active_idx].Selected = true;

			int selected_idx = s_Info.Menu(s_Info.ModuleNumber, -1, -1, 0, FMENU_WRAPMODE | FMENU_SHOWAMPERSAND,
										   GetMsg(MMenuTitle), L"Up/Down Enter Esc F3", nullptr, BreakKeys, &BreakCode, &menu_items[0], menu_items.size());

			if (selected_idx == -1) { break; }

			menu_items[active_idx].Selected = false;
			active_idx = selected_idx;


			auto selected_app = candidates[selected_idx];
			std::wstring cmd = provider->ConstructCommandLine(selected_app, pathname);

			if (BreakCode == 0) { // F3

				auto mimetype = StrMB2Wide(provider->IdentifyFileMimeType(pathname));

				FarDialogItem di[] = {
					{ DI_DOUBLEBOX,   3,  1, 66,  12, FALSE, {}, 0, 0, L"Application Info", 0 },
					{ DI_TEXT,        5,  2,  20,  2, FALSE, {}, 0, 0, L"  Desktop file:", 0 },
					{ DI_EDIT,        21, 2,  64,  2, FALSE, {}, DIF_READONLY, 0,  selected_app.desktop_file.c_str()},
					{ DI_TEXT,        5,  3,  20,  3, FALSE, {}, 0, 0, L"         Name =", 0 },
					{ DI_EDIT,        21, 3,  64,  3, FALSE, {}, DIF_READONLY, 0,  selected_app.name.c_str()},
					{ DI_TEXT,        5,  4,  20,  4, FALSE, {}, 0, 0, L"         Exec =", 0 },
					{ DI_EDIT,        21, 4,  64,  4, FALSE, {}, DIF_READONLY, 0, selected_app.exec.c_str()},
					{ DI_TEXT,        5,  5,  20,  5, FALSE, {}, 0, 0, L"     Terminal =", 0 },
					{ DI_EDIT,        21, 5,  64,  5, FALSE, {}, DIF_READONLY, 0, selected_app.terminal ? L"true" : L"false"},
					{ DI_TEXT,        5,  6,  20,  6, FALSE, {}, 0, 0, L"     MimeType =", 0 },
					{ DI_EDIT,        21, 6,  64,  6, FALSE, {}, DIF_READONLY, 0, selected_app.mimetype.c_str()},
					{ DI_TEXT,        5,  7,  0,  7, FALSE, {}, DIF_SEPARATOR, 0, L"", 0 },

					{ DI_TEXT,        5,  8,  20,  8, FALSE, {}, 0, 0, L" File MIME type:", 0 },
					{ DI_EDIT,        21, 8,  64,  8, FALSE, {}, DIF_READONLY, 0, mimetype.c_str() },


					{ DI_TEXT,        5,  9,  20,  9, FALSE, {}, 0, 0, L"Command to run:", 0 },
					{ DI_EDIT,        21, 9,  64,  9, FALSE, {}, DIF_READONLY, 0, cmd.c_str() },
					{ DI_TEXT,        5,  10,  0,  10, FALSE, {}, DIF_SEPARATOR, 0, L"", 0 },
					{ DI_BUTTON,      0,  11,  0,  11, FALSE, {}, DIF_CENTERGROUP, 0, GetMsg(MOk), 0 },
				};

				HANDLE dlg = s_Info.DialogInit(s_Info.ModuleNumber, -1, -1, 70, 14, L"ApplicationInfo", di, ARRAYSIZE(di), 0, 0, nullptr, 0);
				if (dlg != INVALID_HANDLE_VALUE) {
					s_Info.DialogRun(dlg);
					s_Info.DialogFree(dlg);
				}

			} else {
				unsigned int flags = 0;
				if (selected_app.terminal) {
					flags = s_UseExternalTerminal ? EF_EXTERNALTERM : 0;
				} else {
					flags = s_NoWaitForCommandCompletion ? EF_NOWAIT : 0;
				}
				if (s_FSF.Execute(cmd.c_str(), flags) == -1) {
					ShowError(GetMsg(MError), GetMsg(MCannotExecute));
				}
				break;
			}
		} while (true);
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

		if (!s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, (LONG_PTR)&pi) || pi.PanelType != PTYPE_FILEPANEL) {
			ShowError(GetMsg(MError), L"File panels only");
			return INVALID_HANDLE_VALUE;
		}

		if (pi.SelectedItemsNumber > 1) {
			// TODO: (not now, in the distant future) handle multiple files
		}

		if (pi.SelectedItemsNumber == 0 && pi.CurrentItem < 0) {
			ShowError(GetMsg(MError), GetMsg(MNoFileSelected));
			return INVALID_HANDLE_VALUE;
		}

		int item_idx = pi.CurrentItem;

		int itemSize = s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELITEM, item_idx, 0);
		if (itemSize <= 0) return INVALID_HANDLE_VALUE;

		auto item_buf = std::make_unique<unsigned char[]>(itemSize);
		PluginPanelItem *pi_item = reinterpret_cast<PluginPanelItem *>(item_buf.get());
		if (!s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELITEM, item_idx, (LONG_PTR)pi_item)) {
			return INVALID_HANDLE_VALUE;
		}

		if (pi_item->FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			ShowError(GetMsg(MError), GetMsg(MDirectoryError));
			return INVALID_HANDLE_VALUE;
		}

		if (!pi_item->FindData.lpwszFileName) return INVALID_HANDLE_VALUE;

		int dirSize = s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELDIR, 0, 0);
		if (dirSize <= 0) return INVALID_HANDLE_VALUE;

		auto dir_buf = std::make_unique<wchar_t[]>(dirSize);
		if (!s_Info.Control(PANEL_ACTIVE, FCTL_GETPANELDIR, dirSize, (LONG_PTR)dir_buf.get())) {
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

	static const wchar_t* __attribute__((noinline)) GetMsg(int MsgId)
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
