#pragma once

#include <Windows.h>

class BunkerExtDLL
{
public:
	static HANDLE hInstance;

	static constexpr size_t readLength = 2048;
	static char readBuffer[readLength];
	static wchar_t wideBuffer[readLength];
};
