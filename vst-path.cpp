#include "headers/vst-path.h"

#include <windows.h>

namespace {

bool is_separator(wchar_t c)
{
	return c == L'\\' || c == L'/';
}

bool has_invalid_char(const std::wstring &s)
{
	for (wchar_t c : s) {
		if (c == L'"' || c < 0x20)
			return true;
	}
	return false;
}

bool has_dll_extension(const std::wstring &s)
{
	return s.size() > 4 && CompareStringOrdinal(s.c_str() + s.size() - 4, 4, L".dll", 4, TRUE) == CSTR_EQUAL;
}

// "X:\..." or "X:/...", with no further ':' (alternate data streams).
bool is_local_drive_path(const std::wstring &s)
{
	const bool drive_letter = !s.empty() && ((s[0] >= L'a' && s[0] <= L'z') || (s[0] >= L'A' && s[0] <= L'Z'));
	return drive_letter && s.size() >= 3 && s[1] == L':' && is_separator(s[2]) && s.find(L':', 2) == std::wstring::npos;
}

} // namespace

bool is_bare_vst_name(const std::wstring &name)
{
	if (has_invalid_char(name) || name.find_first_of(L"\\/:") != std::wstring::npos)
		return false;

	return has_dll_extension(name);
}

std::wstring vst_path_basename(const std::wstring &path)
{
	const size_t pos = path.find_last_of(L"\\/");
	return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

bool validate_vst_path(const std::wstring &path, std::wstring *normalized)
{
	if (has_invalid_char(path) || !is_local_drive_path(path))
		return false;

	const DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
	if (size == 0)
		return false;

	std::wstring full(size, L'\0');
	const DWORD length = GetFullPathNameW(path.c_str(), size, full.data(), nullptr);
	if (length == 0 || length >= size)
		return false;
	full.resize(length);

	// Normalizing can turn legacy device names such as "C:\CON.dll" into "\\.\CON".
	if (!is_local_drive_path(full) || !has_dll_extension(full))
		return false;

	if (normalized)
		*normalized = full;
	return true;
}
