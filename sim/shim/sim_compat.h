/* Force-included in SkyFrame's own sources when built for the simulator (Windows). */
#pragma once
#define _POSIX_THREAD_SAFE_FUNCTIONS 1   /* gmtime_r, localtime_r */
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#define mkdir(path, mode) _mkdir(path)   /* POSIX takes a mode; Windows doesn't */
