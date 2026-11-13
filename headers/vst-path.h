#pragma once

#include <cstdint>
#include <string>

// Windows only. Shared by the plugin and the proxy, so it must not depend on libobs.

// A plain "Name.dll", to be looked up in the approved VST directories.
bool is_bare_vst_name(const std::wstring &name);

// Part after the last path separator.
std::wstring vst_path_basename(const std::wstring &path);

// Accepts a local "X:\...\Name.dll" (including mapped drives). Rejects UNC, WebDAV, \\?\ and \\.\ forms
// and relative paths. On success `normalized` (optional) is the path to hand to LoadLibrary.
bool validate_vst_path(const std::wstring &path, std::wstring *normalized);

// The proxy's arguments: "<path>" <port> <owner pid>. Fails if the path can't be quoted safely.
bool make_proxy_arguments(const std::wstring &path, int32_t port, unsigned long ownerPid, std::wstring *arguments);

// Fails unless the proxy got exactly the three arguments make_proxy_arguments() produces.
bool parse_proxy_arguments(const wchar_t *commandLine, std::wstring *path, std::wstring *port, std::wstring *ownerPid);
