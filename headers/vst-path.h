#pragma once

#include <string>

// Windows only. Shared by the plugin and the proxy, so it must not depend on libobs.

// A plain "Name.dll", to be looked up in the approved VST directories.
bool is_bare_vst_name(const std::wstring &name);

// Part after the last path separator.
std::wstring vst_path_basename(const std::wstring &path);

// Accepts a local "X:\...\Name.dll" (including mapped drives). Rejects UNC, WebDAV, \\?\ and \\.\ forms
// and relative paths. On success `normalized` (optional) is the path to hand to LoadLibrary.
bool validate_vst_path(const std::wstring &path, std::wstring *normalized);
