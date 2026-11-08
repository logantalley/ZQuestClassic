#ifndef PLATFORM_3DS_H_
#define PLATFORM_3DS_H_

#include <string>

// Call first thing in main. Mounts RomFS, makes the data folder on the SD card the working
// directory, and fills it from RomFS. Undone at exit.
//
// If the data folder has an args.txt, its arguments (split on whitespace, "quotes" group) are
// appended to argc/argv, stdout and stderr go to stdout.txt, and the exit code is written to
// stdout.txt and exit.txt: this is how scripts/3ds_emu.py runs a command line.
void zc_3ds_init(int& argc, char**& argv);

// The system is closing the app (ex: Close in the HOME menu). It must exit promptly, without
// asking anything: carrying on (a "quit?" prompt still drawing) can hang the console.
bool zc_3ds_closing();

// -jit-arm32-selftest: publishes and runs a tiny function through the arm32 JIT's executable
// memory. Appends what happened to `log`; returns whether it worked.
bool zc_3ds_jit_selftest(std::string& log);

#endif
