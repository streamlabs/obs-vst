// Checks which plug-in paths validate_vst_path() lets through: local paths keep working,
// network, device and relative paths are rejected.
//
// Build with -DBUILD_TESTING=ON and run: ctest -L sl-vst --output-on-failure

#include "../headers/vst-path.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                      \
	do {                                                                             \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "FAILED: %s (line %d)\n", #cond, __LINE__); \
			g_failures++;                                                    \
		}                                                                        \
	} while (0)

bool accepted(const wchar_t *path)
{
	return validate_vst_path(path, nullptr);
}

// Returns a drive letter that isn't in use, like "Q:", or an empty string.
std::wstring freeDriveLetter()
{
	const DWORD used = GetLogicalDrives();
	for (wchar_t letter = L'Z'; letter >= L'D'; letter--) {
		if (!(used & (1u << (letter - L'A'))))
			return std::wstring(1, letter) + L":";
	}
	return L"";
}

void testLocalPathsAreAccepted()
{
	// Outside any VST directory: existing users' paths keep loading.
	CHECK(accepted(L"C:\\Users\\someone\\Music\\Plugins\\Foo.dll"));
	CHECK(accepted(L"D:\\VST\\Foo.dll"));
	CHECK(accepted(L"c:/vst/Foo.DLL"));

	// Exactly the form the properties list stores: mixed separators and a double slash.
	std::wstring normalized;
	CHECK(validate_vst_path(L"C:\\Program Files/VSTPlugins//Foo.dll", &normalized));
	CHECK(normalized == L"C:\\Program Files\\VSTPlugins\\Foo.dll");
	CHECK(accepted(L"C:\\Program Files/Common Files/VST2//sub/Foo.dll"));
	CHECK(accepted(L"C:\\Program Files\\Common Files\\Steinberg\\VST2\\Foo.dll"));
}

void testMappedDriveIsAccepted()
{
	const std::wstring drive = freeDriveLetter();
	CHECK(!drive.empty());
	if (drive.empty())
		return;

	wchar_t temp[MAX_PATH];
	GetTempPathW(MAX_PATH, temp);
	const bool mapped = DefineDosDeviceW(0, drive.c_str(), temp) != FALSE;
	CHECK(mapped);

	CHECK(accepted((drive + L"\\Plugins\\Foo.dll").c_str()));

	if (mapped)
		DefineDosDeviceW(DDD_REMOVE_DEFINITION, drive.c_str(), temp);
}

void testRemoteAndDevicePathsAreRejected()
{
	CHECK(!accepted(L"\\\\host\\share\\Foo.dll"));
	CHECK(!accepted(L"//host/share/Foo.dll"));
	CHECK(!accepted(L"\\\\host@SSL\\DavWWWRoot\\Foo.dll"));
	CHECK(!accepted(L"\\\\host@80\\DavWWWRoot\\Foo.dll"));
	CHECK(!accepted(L"\\\\?\\UNC\\host\\share\\Foo.dll"));
	CHECK(!accepted(L"\\\\?\\C:\\Plugins\\Foo.dll"));
	CHECK(!accepted(L"\\\\.\\C:\\Plugins\\Foo.dll"));
	CHECK(!accepted(L"\\\\.\\pipe\\Foo.dll"));
	CHECK(!accepted(L"\\??\\UNC\\host\\share\\Foo.dll"));
	CHECK(!accepted(L"\\Plugins\\Foo.dll"));
}

void testRelativePathsAreRejected()
{
	CHECK(!accepted(L"..\\Foo.dll"));
	CHECK(!accepted(L"..\\..\\Users\\x\\Foo.dll"));
	CHECK(!accepted(L"sub\\Foo.dll"));
	CHECK(!accepted(L"./Foo.dll"));
	CHECK(!accepted(L"C:Foo.dll"));
	CHECK(!accepted(L"C:sub\\Foo.dll"));
	CHECK(!accepted(L"Foo.dll"));
	CHECK(!accepted(L""));
}

void testOtherUnsafeFormsAreRejected()
{
	CHECK(!accepted(L"C:\\Plugins\\Foo.dll:stream"));
	CHECK(!accepted(L"C:\\Plugins\\Foo.exe"));
	CHECK(!accepted(L"C:\\Plugins\\Foo"));
	CHECK(!accepted(L"C:\\Plugins\\Foo\".dll"));
	CHECK(!accepted(L"C:\\Plugins\\Foo.dll\" 1 2"));
	CHECK(!accepted(L"C:\\Plugins\\Foo\n.dll"));
}

void testBareNames()
{
	CHECK(is_bare_vst_name(L"Foo.dll"));
	CHECK(is_bare_vst_name(L"My Plugin (x64).DLL"));
	CHECK(!is_bare_vst_name(L""));
	CHECK(!is_bare_vst_name(L".dll"));
	CHECK(!is_bare_vst_name(L"Foo"));
	CHECK(!is_bare_vst_name(L"Foo.exe"));
	CHECK(!is_bare_vst_name(L".."));
	CHECK(!is_bare_vst_name(L"sub\\Foo.dll"));
	CHECK(!is_bare_vst_name(L"sub/Foo.dll"));
	CHECK(!is_bare_vst_name(L"C:Foo.dll"));
	CHECK(!is_bare_vst_name(L"Foo.dll:stream"));
	CHECK(!is_bare_vst_name(L"Fo\"o.dll"));
	CHECK(!is_bare_vst_name(L"Fo\to.dll"));
}

void testBasename()
{
	CHECK(vst_path_basename(L"..\\Foo.dll") == L"Foo.dll");
	CHECK(vst_path_basename(L"\\\\host@SSL\\DavWWWRoot\\Foo.dll") == L"Foo.dll");
	CHECK(vst_path_basename(L"C:/a//b/Foo.dll") == L"Foo.dll");
	CHECK(vst_path_basename(L"Foo.dll") == L"Foo.dll");
	CHECK(vst_path_basename(L"C:\\a\\") == L"");
}

void testProxyArgumentsRoundTrip()
{
	const wchar_t *paths[] = {L"C:\\Program Files\\VSTPlugins\\Foo.dll", L"D:\\a b\\c  d\\Foo Bar.dll", L"C:\\Plugins\\Foo.dll"};

	for (const wchar_t *path : paths) {
		std::wstring arguments;
		CHECK(make_proxy_arguments(path, 51234, 4321, &arguments));

		std::wstring parsedPath, parsedPort, parsedPid;
		CHECK(parse_proxy_arguments(arguments.c_str(), &parsedPath, &parsedPort, &parsedPid));
		CHECK(parsedPath == path);
		CHECK(parsedPort == L"51234");
		CHECK(parsedPid == L"4321");
	}
}

void testProxyArgumentsRejectQuotes()
{
	std::wstring arguments;
	CHECK(!make_proxy_arguments(L"C:\\Plugins\\Foo\".dll", 1234, 1, &arguments));
	CHECK(!make_proxy_arguments(L"C:\\Plugins\\Foo.dll\" 80 1 \"x", 1234, 1, &arguments));
	CHECK(!make_proxy_arguments(L"", 1234, 1, &arguments));
}

void testProxyRequiresExactlyThreeArguments()
{
	std::wstring path, port, pid;
	CHECK(parse_proxy_arguments(L"\"C:\\a b\\Foo.dll\" 1234 99", &path, &port, &pid));
	CHECK(!parse_proxy_arguments(L"", &path, &port, &pid));
	CHECK(!parse_proxy_arguments(L"\"C:\\Foo.dll\"", &path, &port, &pid));
	CHECK(!parse_proxy_arguments(L"\"C:\\Foo.dll\" 1234", &path, &port, &pid));
	CHECK(!parse_proxy_arguments(L"\"C:\\Foo.dll\" 1234 99 extra", &path, &port, &pid));
	// What a quote in the path used to do: the path ends early and the rest become the arguments.
	CHECK(!parse_proxy_arguments(L"\"C:\\Foo\" 80 1 \".dll\" 1234 99", &path, &port, &pid));
}

} // namespace

int main()
{
	testLocalPathsAreAccepted();
	testMappedDriveIsAccepted();
	testRemoteAndDevicePathsAreRejected();
	testRelativePathsAreRejected();
	testOtherUnsafeFormsAreRejected();
	testBareNames();
	testBasename();
	testProxyArgumentsRoundTrip();
	testProxyArgumentsRejectQuotes();
	testProxyRequiresExactlyThreeArguments();

	if (g_failures == 0)
		std::printf("vst path validation: all checks passed\n");
	return g_failures == 0 ? 0 : 1;
}
