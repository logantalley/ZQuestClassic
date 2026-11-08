#include "platform/3ds/platform_3ds.h"
#include "base/util.h"
#include "base/version.h"

#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <new>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// The game reads its resources from the working directory and writes its config and saves
// next to them. Resources ship read-only in RomFS, so they are copied here.
#define DATA_DIR "sdmc:/3ds/zquestclassic"
#define STAMP_FILE ".romfs_version"
#define ARGS_FILE DATA_DIR "/args.txt"
#define STDOUT_FILE DATA_DIR "/stdout.txt"
#define EXIT_FILE DATA_DIR "/exit.txt"

// Set when the command line came from args.txt: a script is waiting for the exit code.
static bool report_exit_code;

// Every exit path (zc_exit, Z_error_fatal, returning from main) ends in exit(). Needs
// -Wl,--wrap=exit (see CMakeLists.txt).
extern "C" void __real_exit(int code) __attribute__((noreturn));
extern "C" void __wrap_exit(int code)
{
	if (report_exit_code)
	{
		printf("zplayer exit code: %d\n", code);
		fflush(stdout);
		fflush(stderr);
		if (FILE* f = fopen(EXIT_FILE, "wb"))
		{
			fprintf(f, "%d\n", code);
			fclose(f);
		}
	}
	__real_exit(code);
}

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

// libctru splits the memory left after loading between the heap and the linear heap, up
// to 32 MB of linear. Only the framebuffers and audio buffers use linear memory, and ZC
// needs ~20 MB of heap for combos alone, so keep the linear heap small.
extern "C" u32 __ctru_linear_heap_size = 8 * 1024 * 1024;
extern "C" u32 __ctru_heap_size;
// The main thread's stack (taken from the heap) defaults to 32 KB, far below what ZC
// assumes (1 MB on Windows, 5 MB on the web).
extern "C" u32 __stacksize__ = 4 * 1024 * 1024;

// Below this the player runs out of memory loading the engine's tables, before it can
// show anything. A 64 MB app (the .3dsx from most launchers) has ~20 MB of heap; the .cia
// on a New 3DS has ~80 MB.
#define MIN_HEAP_MB 40

static void wait_for_start()
{
	while (aptMainLoop())
	{
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		gspWaitForVBlank();
	}
}

static void check_memory()
{
	u32 heap_mb = __ctru_heap_size >> 20;
	if (heap_mb >= MIN_HEAP_MB)
		return;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	printf("ZQuest Classic needs at least %d MB\n"
		"of memory, but was given %lu MB.\n\n"
		"Install and run zplayer.cia on a\n"
		"New 3DS / New 2DS XL instead.\n\n"
		"Press START to exit.\n", MIN_HEAP_MB, (unsigned long)heap_mb);
	wait_for_start();
	gfxExit();
	exit(0);
}

// Out of memory would otherwise abort with nothing on screen; leave a note in the data
// folder (the working directory by then).
static void on_out_of_memory()
{
	if (FILE* f = fopen("error.log", "a"))
	{
		fprintf(f, "Out of memory (heap is %lu MB).\n", (unsigned long)(__ctru_heap_size >> 20));
		fclose(f);
	}
	std::set_new_handler(nullptr);
	throw std::bad_alloc();
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

// Files the player may change, which are only ever seeded: the top-level configs. Their
// defaults (base_config/zc.cfg, ...) are resources like any other.
static bool is_user_file(const std::string& path)
{
	return path == "zc.cfg" || path == "controls.cfg";
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
		else if (!exists(dst) || (update && !is_user_file(dst)))
			copy_file(src, dst);
	}
	closedir(dir);
}

// Appends the arguments in args.txt, if any, to the command line.
static void read_args_file(int& argc, char**& argv)
{
	if (!exists(ARGS_FILE))
		return;

	std::vector<std::string> args = util::split_args(read_file(ARGS_FILE));
	// Launched without a path (ex: some emulators), there may be no argv[0].
	int base = argc > 0 ? argc : 1;
	char** new_argv = new char*[base + args.size() + 1];
	new_argv[0] = argc > 0 ? argv[0] : strdup("zplayer");
	for (int i = 1; i < argc; i++)
		new_argv[i] = argv[i];
	for (size_t i = 0; i < args.size(); i++)
		new_argv[base + i] = strdup(args[i].c_str());
	argc = base + args.size();
	new_argv[argc] = nullptr;
	argv = new_argv;

	// stderr shares stdout's file descriptor (and so its offset), so their output
	// interleaves in order. (The SD card driver ignores O_APPEND.)
	remove(EXIT_FILE);
	if (freopen(STDOUT_FILE, "wb", stdout))
	{
		setvbuf(stdout, nullptr, _IOLBF, 4096);
		dup2(fileno(stdout), STDERR_FILENO);
	}
	report_exit_code = true;

	printf("args.txt:");
	for (size_t i = 0; i < args.size(); i++)
		printf(" [%s]", args[i].c_str());
	printf("\n");
}

void zc_3ds_init(int& argc, char**& argv)
{
	osSetSpeedupEnable(true);
	check_memory();
	romfsInit();
	atexit([]{ romfsExit(); });

	mkdir("sdmc:/3ds", 0777);
	mkdir(DATA_DIR, 0777);
	chdir(DATA_DIR);
	read_args_file(argc, argv);
	std::set_new_handler(on_out_of_memory);

	// Refresh resources whenever they change. The staged RomFS carries a hash of its
	// contents (packaging/3ds/stage_romfs.py): the version string is the same across
	// local and CI builds.
	std::string romfs_id = read_file("romfs:/romfs_id.txt");
	if (romfs_id.empty())
		romfs_id = getVersionString();
	bool update = read_file(STAMP_FILE) != romfs_id;
	sync_dir("romfs:/", "", update);
	mkdir("quests", 0777);
	mkdir("saves", 0777);
	// Quest loading goes through temp files; std::filesystem::temp_directory_path() throws
	// without a TMPDIR that exists.
	mkdir("tmp", 0777);
	setenv("TMPDIR", "/3ds/zquestclassic/tmp", 1);
	if (update)
	{
		if (FILE* f = fopen(STAMP_FILE, "wb"))
		{
			fputs(romfs_id.c_str(), f);
			fclose(f);
		}
	}
}

bool zc_3ds_closing()
{
	return aptShouldClose();
}
