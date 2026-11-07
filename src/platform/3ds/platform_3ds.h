#ifndef PLATFORM_3DS_H_
#define PLATFORM_3DS_H_

// Call first thing in main. Mounts RomFS, makes the data folder on the SD card the working
// directory, and fills it from RomFS. Undone at exit.
void zc_3ds_init();

// The system is closing the app (ex: Close in the HOME menu). It must exit promptly, without
// asking anything: carrying on (a "quit?" prompt still drawing) can hang the console.
bool zc_3ds_closing();

#endif
