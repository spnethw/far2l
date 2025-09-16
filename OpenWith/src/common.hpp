#pragma once

#include <string>

struct CandidateInfo
{
	bool terminal;
	std::wstring name;
	std::wstring exec;
	std::wstring mimetype;
	std::wstring desktop_file;
};

