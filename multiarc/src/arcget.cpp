#include "MultiArc.hpp"
#include "marclng.hpp"
#include <farkeys.h>
#include <utils.h>
#include <EnsureDir.h>
#include <errno.h>

class TRecur	//$ 07.04.2002 AA
{
public:
	static int Count;
	TRecur() { Count++; }
	~TRecur() { Count--; }
};
int TRecur::Count = 0;

inline void CreateDirectory(char *FullPath)		//$ 16.05.2002 AA
{
	EnsureDir(FullPath);
}

int PluginClass::GetFiles(PluginPanelItem *PanelItem, int ItemsNumber, int Move, char *DestPath, int OpMode)
{
	// костыль против зацикливания в FAR'е при Quick View архивов с паролем
	TRecur Recur;	//$ 07.04.2002 AA
	if (Recur.Count > 1 && OpMode & (OPM_VIEW | OPM_QUICKVIEW))
		return 0;

	char SaveDirBuf[NM];
	char *SaveDir = sdc_getcwd(SaveDirBuf, sizeof(SaveDirBuf));
	std::string Command, AllFilesMask;
	if (ItemsNumber == 0)
		return /*0*/ 1;		//$ 07.02.2002 AA чтобы многотомные CABы нормально распаковывались
	if (*DestPath)
		FSF.AddEndSlash(DestPath);
	const char *PathHistoryName = "ExtrDestPath";
	InitDialogItem InitItems[] = {
			/* 0 */ {DI_DOUBLEBOX, 3, 1, 72, 13, 0, 0, 0, 0, (char *)MExtractTitle},
			/* 1 */ {DI_TEXT, 5, 2, 0, 0, 0, 0, 0, 0, (char *)MExtractTo},
			/* 2 */ {DI_EDIT, 5, 3, 70, 3, 1, (DWORD_PTR)PathHistoryName, DIF_HISTORY, 0, DestPath},
			/* 3 */ {DI_TEXT, 3, 4, 0, 0, 0, 0, DIF_BOXCOLOR | DIF_SEPARATOR, 0, ""},
			/* 4 */ {DI_TEXT, 5, 5, 0, 0, 0, 0, 0, 0, (char *)MExtrPassword},
			/* 5 */ {DI_PSWEDIT, 5, 6, 35, 5, 0, 0, 0, 0, ""},
			/* 6 */ {DI_TEXT, 3, 7, 0, 0, 0, 0, DIF_BOXCOLOR | DIF_SEPARATOR, 0, ""},
			/* 7 */ {DI_CHECKBOX, 5, 8, 0, 0, 0, 0, 0, 0, (char *)MExtrWithoutPaths},
			/* 8 */ {DI_CHECKBOX, 5, 9, 0, 0, 0, 0, 0, 0, (char *)MBackground},
			/* 9 */ {DI_CHECKBOX, 5, 10, 0, 0, 0, 0, 0, 0, (char *)MExtrDel},
			/*10 */ {DI_TEXT, 3, 11, 0, 11, 0, 0, DIF_BOXCOLOR | DIF_SEPARATOR, 0, ""},
			/*11 */ {DI_BUTTON, 0, 12, 0, 0, 0, 0, DIF_CENTERGROUP, 1, (char *)MExtrExtract},
			/*12 */ {DI_BUTTON, 0, 12, 0, 0, 0, 0, DIF_CENTERGROUP, 0, (char *)MExtrCancel},
	};

	FarDialogItem DialogItems[ARRAYSIZE(InitItems)];
	InitDialogItems(InitItems, DialogItems, ARRAYSIZE(InitItems));

	int AskVolume = (OpMode & (OPM_FIND | OPM_VIEW | OPM_EDIT)) == 0 && CurArcInfo.Volume && *CurDir == 0;

	if (!AskVolume) {
		DialogItems[7].Selected = TRUE;
		for (int I = 0; I < ItemsNumber; I++)
			if (PanelItem[I].FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				DialogItems[7].Selected = FALSE;
				break;
			}
	}

	Opt.UserBackground = 0;			// $ 14.02.2001 raVen //сброс галки "фоновая архивация"
	if ((OpMode & ~OPM_SILENT) & ~OPM_TOPLEVEL)
		Opt.OldUserBackground = 0;	// $ 03.07.02 AY: если OPM_SILENT но не из за Shift-F2 при несколько выбраных архивах
	DialogItems[8].Selected = Opt.UserBackground;
	DialogItems[9].Selected = Move;

	if ((OpMode & OPM_SILENT) == 0) {
		int AskCode = Info.Dialog(Info.ModuleNumber, -1, -1, 76, 15, "ExtrFromArc", DialogItems, ARRAYSIZE(DialogItems));
		if (AskCode != 11)
			return -1;
		strcpy(DestPath, DialogItems[2].Data);
		FSF.Unquote(DestPath);
		Opt.UserBackground = DialogItems[8].Selected;
		Opt.OldUserBackground = Opt.UserBackground;		// $ 02.07.2002 AY: запомним и не будем нигде сбрасывать
														// SetRegKey(HKEY_CURRENT_USER,"","Background",Opt.UserBackground); // $ 06.02.2002 AA
	}

	LastWithoutPathsState = DialogItems[7].Selected;

	Opt.Background = OpMode & OPM_SILENT ? Opt.OldUserBackground : Opt.UserBackground;	// $ 02.07.2002 AY: Если OPM_SILENT значит выбрано несколько архивов

	/*int SpaceOnly=TRUE;
	for (int I=0;DestPath[I]!=0;I++)
	  if (DestPath[I]!=' ')
	  {
		SpaceOnly=FALSE;
		break;
	  }

	if (!SpaceOnly)
	{
	  for (char *ChPtr=DestPath;*ChPtr!=0;ChPtr++)
		if (*ChPtr=='/')
		{
		  *ChPtr=0;
		  CreateDirectory(DestPath,NULL);
		  *ChPtr='/';
		}
	  CreateDirectory(DestPath,NULL);
	}*/
	CreateDirectory(DestPath);	//$ 16.05.2002 AA

	if (*DestPath)
		FSF.AddEndSlash(DestPath);
	AllFilesMask = GetCommandFormat(CMD_ALLFILESMASK);

	PluginPanelItem MaskPanelItem;

	if (AskVolume) {
		int MsgCode;

		/*if(OpMode & OPM_TOPLEVEL) // $ 16.02.2002 AA
		{
		  //?? есть разница между извлечением выделенных файлов тома и
		  //извлечением из выделенных томов. здесь можно ее учесть.
		  //как минимум - нужно изменить надпись в мессаджбоксе
		  MsgCode=1;
		}
		else        */
		{
			const auto &NameMsg = FormatMessagePath(ArcName.c_str(), true);
			const auto &VolMsg = StrPrintf(GetMsg(MExtrVolume), NameMsg.c_str());
			const char *MsgItems[] = {GetMsg(MExtractTitle), VolMsg.c_str(), GetMsg(MExtrVolumeAsk1),
					GetMsg(MExtrVolumeAsk2), GetMsg(MExtrVolumeSelFiles), GetMsg(MExtrAllVolumes)};
			MsgCode = Info.Message(Info.ModuleNumber, 0, NULL, MsgItems, ARRAYSIZE(MsgItems), 2);
		}
		if (MsgCode < 0)
			return -1;
		if (MsgCode == 1) {
			ZeroFill(MaskPanelItem);
			CharArrayCpyZ(MaskPanelItem.FindData.cFileName, AllFilesMask.c_str());
			if (ItemsInfo.Encrypted)
				MaskPanelItem.Flags = F_ENCRYPTED;
			PanelItem = &MaskPanelItem;
			ItemsNumber = 1;
		}
	}

	int CommandType = LastWithoutPathsState ? CMD_EXTRACTWITHOUTPATH : CMD_EXTRACT;
	Command = GetCommandFormat(CommandType);
	if (*DialogItems[5].Data == 0 && Command.find("%%P") != std::string::npos)
		for (int I = 0; I < ItemsNumber; I++)
			if ((PanelItem[I].Flags & F_ENCRYPTED)
					|| (ItemsInfo.Encrypted
							&& (PanelItem[I].FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))) {
				std::string PasswordStr;
				if (OpMode & OPM_FIND || !GetPassword(PasswordStr, FSF.PointToName((char *)ArcName.c_str())))
					return -1;
				CharArrayCpyZ(DialogItems[5].Data, PasswordStr.c_str());
				break;
			}

	if (sdc_chdir(DestPath))
		fprintf(stderr, "sdc_chdir('%s') - %u\n", DestPath, errno);
	int SaveHideOut = Opt.HideOutput;
	if (OpMode & OPM_FIND)
		Opt.HideOutput = 2;
	int IgnoreErrors = (CurArcInfo.Flags & AF_IGNOREERRORS);

	ArcCommand ArcCmd(PanelItem, ItemsNumber, Command, ArcName, CurDir, DialogItems[5].Data, AllFilesMask,
			IgnoreErrors, CommandType /*(OpMode & OPM_VIEW) != 0*/, (OpMode & OPM_FIND), CurDir, ItemsInfo.Codepage);

	// последующие операции (тестирование и тд) не должны быть фоновыми
	Opt.Background = 0;		// $ 06.02.2002 AA

	Opt.HideOutput = SaveHideOut;
	if (!SaveDir || !*SaveDir) {
		fprintf(stderr, "%s: SaveDir not saved\n", __FUNCTION__);
	} else if (sdc_chdir(SaveDir) != 0) {
		fprintf(stderr, "%s: sdc_chdir('%s') - %u\n", __FUNCTION__, SaveDir, errno);
	}

	if (!IgnoreErrors && ArcCmd.GetExecCode() != 0)
		if (!(OpMode & OPM_VIEW))
			return 0;

	if (DialogItems[9].Selected)
		DeleteFiles(PanelItem, ItemsNumber, TRUE);

	if (Opt.UpdateDescriptions)
		for (int I = 0; I < ItemsNumber; I++)
			PanelItem[I].Flags|= PPIF_PROCESSDESCR;

	return 1;
}
