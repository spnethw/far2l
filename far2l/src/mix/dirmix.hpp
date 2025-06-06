#pragma once

/*
dirmix.hpp

Misc functions for working with directories
*/
/*
Copyright (c) 1996 Eugene Roshal
Copyright (c) 2000 Far Group
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <WinCompat.h>
#include "FARString.hpp"

enum TESTFOLDERCONST	// for TestFolder()
{
	TSTFLD_ERROR     = -2,
	TSTFLD_NOTACCESS = -1,
	TSTFLD_EMPTY     = 0,
	TSTFLD_NOTEMPTY  = 1,
	TSTFLD_NOTFOUND  = 2,
};

/*
	$ 15.02.2002 IS
	Установка нужного диска и каталога и установление соответствующей переменной
	окружения. В случае успеха возвращается не ноль.
	Если ChangeDir==FALSE, то не меняем текущий диск, а только устанавливаем
	переменные окружения.
*/
BOOL FarChDir(const wchar_t *NewDir, BOOL ChangeDir = TRUE);

TESTFOLDERCONST TestFolder(const wchar_t *Name);
int CheckShortcutFolder(FARString &strTestPath, bool IsHostFile, bool Silent = false);

void CreatePath(FARString &strPath);

std::string GetHelperPathName(const char *name);
std::string GetMyScriptQuoted(const char *name);

void PrepareTemporaryOpenPath(FARString &Path);
FARString DefaultPanelInitialDirectory();
