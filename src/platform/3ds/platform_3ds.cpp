#include "platform/3ds/platform_3ds.h"
#include "base/version.h"

#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// The game reads its resources from the working directory and writes its config and saves
// next to them. Resources ship read-only in RomFS, so they are copied here.
#define DATA_DIR "sdmc:/3ds/zquestclassic"
#define STAMP_FILE ".romfs_version"

// libctru reports the working directory with its device ("sdmc:/..."), which neither
// std::filesystem nor Allegro treat as absolute. Drop it; "/..." resolves against the same
// (default) device. Needs -Wl,--wrap=getcwd (see CMakeLists.txt).
extern "C" char* __real_getcwd(char* buf, size_t size);
extern "C" char* __wrap_getcwd(char* buf, size_t size)
{
	char* cwd = __real_getcwd(buf, size);
	if (cwd && strncmp(cwd, "sdmc:/", 6) == 0)
		memmove(cwd, cwd + 5, strlen(cwd + 5) + 1);
	return cwd;
}

static bool exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

static std::string read_file(const char* path)
{
	std::string text;
	if (FILE* f = fopen(path, "rb"))
	{
		char buf[256];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			text.append(buf, n);
		fclose(f);
	}
	return text;
}

static void copy_file(const std::string& from, const std::string& to)
{
	FILE* in = fopen(from.c_str(), "rb");
	if (!in)
		return;
	if (FILE* out = fopen(to.c_str(), "wb"))
	{
		static char buf[64 * 1024];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), in)) > 0 && fwrite(buf, 1, n, out) == n) {}
		fclose(out);
	}
	fclose(in);
}

// Files the player may change, which are only ever seeded.
static bool is_user_file(const std::string& name)
{
	return name == "zc.cfg" || name == "controls.cfg";
}

// Copies the `from` tree into `to` (both empty or ending in '/'). Missing files are always
// copied; existing ones only when `update` is set.
static void sync_dir(const std::string& from, const std::string& to, bool update)
{
	DIR* dir = opendir(from.c_str());
	if (!dir)
		return;
	if (!to.empty())
		mkdir(to.c_str(), 0777);
	while (dirent* ent = readdir(dir))
	{
		std::string name = ent->d_name;
		if (name == "." || name == "..")
			continue;
		std::string src = from + name, dst = to + name;
		struct stat st;
		if (stat(src.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			sync_dir(src + "/", dst + "/", update);
		else if (!exists(dst) || (update && !is_user_file(name)))
			copy_file(src, dst);
	}
	closedir(dir);
}

void zc_3ds_init()
{
	osSetSpeedupEnable(true);
	romfsInit();
	atexit([]{ romfsExit(); });

	mkdir("sdmc:/3ds", 0777);
	mkdir(DATA_DIR, 0777);
	chdir(DATA_DIR);

	// Refresh resources once per build.
	bool update = read_file(STAMP_FILE) != getVersionString();
	sync_dir("romfs:/", "", update);
	mkdir("quests", 0777);
	mkdir("saves", 0777);
	if (update)
	{
		if (FILE* f = fopen(STAMP_FILE, "wb"))
		{
			fputs(getVersionString(), f);
			fclose(f);
		}
	}
}
