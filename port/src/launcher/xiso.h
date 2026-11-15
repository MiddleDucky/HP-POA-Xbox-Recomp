// xiso.h - unpacking an Xbox disc image into the game folder (xiso.c), for hppoa_launcher.exe
#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile LONG64 done;   // bytes written (or found already whole) so far
    LONG64 total;           // bytes of all the files in the image
    volatile LONG cancel;   // set from another thread: stops at the next block, the unfinished file is removed
    WCHAR error[320];       // why xiso_Extract failed
} XisoProgress;

// The first dir\*.iso / dir\*.xiso that holds an Xbox disc, when dir\default.xbe is absent: its full path in
// image (MAX_PATH), TRUE. FALSE when there is nothing to unpack.
BOOL xiso_Find(const WCHAR* dir, WCHAR* image);

// The same, whether or not default.xbe is there: the image to write damaged files again from (gamefiles.h).
BOOL xiso_FindAny(const WCHAR* dir, WCHAR* image);

// Unpacks image into dir. Files already there whole are kept (a stopped unpack carries on), default.xbe is
// written last, so a folder whose unpacking stopped part-way is never taken for a complete one.
BOOL xiso_Extract(const WCHAR* image, const WCHAR* dir, XisoProgress* p);

#ifdef __cplusplus
}
#endif
