#pragma once

#include <string>
#include <functional>

struct CandidateInfo
{
	bool terminal;
	std::wstring name;
	std::wstring exec;
	std::wstring mimetype;
	std::wstring desktop_file;

	// for deduplication by executable file
	bool operator==(const CandidateInfo& other) const
	{
		return exec == other.exec;
	}
};

// hasher for using CandidateInfo in unordered_set
struct CandidateInfoHasher
{
	std::size_t operator()(const CandidateInfo& c) const
	{
		return std::hash<std::wstring>()(c.exec);
	}
};


	struct Token
	{
		std::wstring text;
		bool quoted;
		bool single_quoted; // Added for single quotes support
	};
