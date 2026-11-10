// Drives vst_update() with different plugin_path values against a temporary Program Files
// tree: bare names resolve to the installed plug-in (and are written back), local paths
// anywhere keep loading as they are, and network or relative paths never load themselves.
//
// There is no module binary path in this process, so loadEffectLocked() stops before
// launching a proxy. The plug-in still counts as loaded, which is what the "update again"
// check needs. The load generation counts the loads that were attempted.
//
// Build with -DBUILD_TESTING=ON and run: ctest -L sl-vst --output-on-failure

#include "../obs-vst.cpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;

class VSTPluginTestAccess {
public:
	static uint64_t loadGeneration(VSTPlugin &plugin) { return plugin.m_loadGeneration; }
};

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                      \
	do {                                                                             \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "FAILED: %s (line %d)\n", #cond, __LINE__); \
			g_failures++;                                                    \
		}                                                                        \
	} while (0)

#define CHECK_EQ(actual, expected)                                                                                                                    \
	do {                                                                                                                                          \
		const std::string a = (actual);                                                                                                       \
		const std::string e = (expected);                                                                                                     \
		if (a != e) {                                                                                                                         \
			std::fprintf(stderr, "FAILED: %s == %s (line %d)\n  got:      %s\n  expected: %s\n", #actual, #expected, __LINE__, a.c_str(), \
				     e.c_str());                                                                                                      \
			g_failures++;                                                                                                                 \
		}                                                                                                                                     \
	} while (0)

void touch(const fs::path &file)
{
	fs::create_directories(file.parent_path());
	std::ofstream(file, std::ios::binary) << "not a real plug-in";
}

struct Update {
	std::unique_ptr<VSTPlugin, void (*)(VSTPlugin *)> plugin{new VSTPlugin(nullptr), [](VSTPlugin *p) { vst_destroy(p); }};
	obs_data_t *settings = obs_data_create();

	~Update() { obs_data_release(settings); }

	void run(const std::string &pluginPath)
	{
		obs_data_set_string(settings, "plugin_path", pluginPath.c_str());
		vst_update(plugin.get(), settings);
	}

	std::string storedPath() { return obs_data_get_string(settings, "plugin_path"); }
	uint64_t loads() { return VSTPluginTestAccess::loadGeneration(*plugin); }
};

std::string resolve(const std::string &path)
{
	std::string resolved;
	return resolve_plugin_path(path, resolved) ? resolved : "<not found>";
}

} // namespace

int main()
{
	const fs::path root = fs::temp_directory_path() / ("sl-vst-path-resolution-" + std::to_string(GetCurrentProcessId()));
	fs::remove_all(root);

	const fs::path programFiles = root / "pf";
	const fs::path commonFiles = root / "cf";
	touch(programFiles / "VSTPlugins" / "sub" / "Foo.dll");
	touch(commonFiles / "Steinberg" / "VST2" / "Bar.dll");
	touch(root / "elsewhere" / "Local.dll");

	_putenv_s("ProgramFiles", programFiles.string().c_str());
	_putenv_s("CommonProgramFiles", commonFiles.string().c_str());

	const std::string foo = programFiles.string() + "/VSTPlugins//sub/Foo.dll";
	const std::string bar = commonFiles.string() + "/Steinberg/VST2//Bar.dll";
	const std::string local = (root / "elsewhere" / "Local.dll").string();

	// Dropdown enumeration lists the extra VST2 directory too.
	{
		std::vector<vst_data> list;
		for (const std::string &dir : win32_build_dir_list())
			find_plugins(list, dir.c_str());
		bool listedBar = false;
		for (const vst_data &vst : list)
			listedBar |= vst.second == bar;
		CHECK(listedBar);
	}

	// Bare names resolve to the dropdown value, case-insensitively.
	CHECK_EQ(resolve("Foo.dll"), foo);
	CHECK_EQ(resolve("foo.DLL"), foo);
	CHECK_EQ(resolve("Bar.dll"), bar);
	CHECK_EQ(resolve("Missing.dll"), "<not found>");
	CHECK_EQ(resolve("Foo"), "<not found>");

	// Local paths are kept as they are, wherever they are, installed or not.
	CHECK_EQ(resolve(local), local);
	CHECK_EQ(resolve(foo), foo);
	CHECK_EQ(resolve("C:\\Somewhere\\Gone\\Foo.dll"), "C:\\Somewhere\\Gone\\Foo.dll");

	// Rejected paths fall back to the installed plug-in with the same file name, or nothing.
	CHECK_EQ(resolve("\\\\host@SSL\\DavWWWRoot\\Foo.dll"), foo);
	CHECK_EQ(resolve("\\\\host\\share\\Bar.dll"), bar);
	CHECK_EQ(resolve("\\\\?\\UNC\\host\\share\\Foo.dll"), foo);
	CHECK_EQ(resolve("\\\\.\\C:\\Plugins\\Foo.dll"), foo);
	CHECK_EQ(resolve("..\\Foo.dll"), foo);
	CHECK_EQ(resolve("sub\\Foo.dll"), foo);
	CHECK_EQ(resolve("\\\\host\\share\\Missing.dll"), "<not found>");
	CHECK_EQ(resolve("..\\Missing.dll"), "<not found>");
	CHECK_EQ(resolve("\\\\host\\share\\Foo.exe"), "<not found>");

	// A bare name loads once and is written back as the dropdown value.
	{
		Update u;
		u.run("Foo.dll");
		CHECK_EQ(u.storedPath(), foo);
		CHECK_EQ(u.plugin->getPluginPath(), foo);
		CHECK(u.plugin->hasEffect());
		CHECK(u.loads() == 1);

		// Desktop updates again with the settings it originally had.
		u.run("Foo.dll");
		CHECK_EQ(u.storedPath(), foo);
		CHECK(u.loads() == 1);

		u.run(foo);
		CHECK(u.loads() == 1);
	}

	// A local path outside the approved directories loads as it is.
	{
		Update u;
		u.run(local);
		CHECK_EQ(u.storedPath(), local);
		CHECK_EQ(u.plugin->getPluginPath(), local);
		CHECK(u.loads() == 1);
	}

	// A rejected path loads the installed plug-in with the same name.
	{
		Update u;
		u.run("\\\\host@SSL\\DavWWWRoot\\Bar.dll");
		CHECK_EQ(u.storedPath(), bar);
		CHECK_EQ(u.plugin->getPluginPath(), bar);
		CHECK(u.loads() == 1);
	}

	// Nothing installed under that name: nothing loads, the settings stay as they were.
	for (const char *path : {"Missing.dll", "\\\\host\\share\\Missing.dll", "..\\Missing.dll", "\\\\?\\UNC\\host\\share\\Missing.dll"}) {
		Update u;
		u.run(path);
		CHECK_EQ(u.storedPath(), path);
		CHECK_EQ(u.plugin->getPluginPath(), "");
		CHECK(!u.plugin->hasEffect());
		CHECK(u.loads() == 0);
	}

	// Switching from a loaded plug-in to a name that isn't installed unloads it.
	{
		Update u;
		u.run("Foo.dll");
		CHECK(u.plugin->hasEffect());
		u.run("Missing.dll");
		CHECK(!u.plugin->hasEffect());
		CHECK(u.loads() == 1);
	}

	// Without ProgramFiles nothing is enumerated or resolved, and nothing crashes.
	{
		_putenv_s("ProgramFiles", "");
		CHECK_EQ(resolve("Foo.dll"), "<not found>");
		CHECK_EQ(resolve("Bar.dll"), bar);

		Update u;
		u.run("Foo.dll");
		CHECK_EQ(u.storedPath(), "Foo.dll");
		CHECK(!u.plugin->hasEffect());
		CHECK(u.loads() == 0);
		_putenv_s("ProgramFiles", programFiles.string().c_str());
	}
	fs::remove_all(root);

	if (g_failures == 0)
		std::printf("vst path resolution: all checks passed\n");
	return g_failures == 0 ? 0 : 1;
}
