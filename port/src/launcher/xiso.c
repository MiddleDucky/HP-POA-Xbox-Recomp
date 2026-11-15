// xiso.c - unpacking an Xbox disc image into the game folder, for hppoa_launcher.exe (xiso.h)
//
// XDVDFS, as the console reads it:
//   2048-byte sectors; the volume descriptor is sector 32 of the game partition: "MICROSOFT*XBOX*MEDIA", root
//   directory sector (u32), root directory size (u32).
//   A directory is a table of entries forming a binary search tree:
//     +0 u16 left subtree (offset into the table in 4-byte units, 0 = none)   +2 u16 right subtree
//     +4 u32 first sector   +8 u32 size in bytes   +C u8 attributes (0x10 = directory)   +D u8 name length, name
//   0xFFFF fills unused space; an empty directory has size 0.
// Images come trimmed (an xiso: the game partition at byte 0) or as full Redump dumps, where the game partition
// starts at a fixed offset per disc generation; both are probed.
#include "xiso.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define XISO_SECTOR   2048ull
#define XISO_MAGIC    "MICROSOFT*XBOX*MEDIA"
#define XISO_ATTR_DIR 0x10
#define XISO_BUF      (4u << 20)

typedef struct {
    char name[256];
    uint8_t attr;
    uint32_t sector, size;
    int parent;
} Node;

typedef struct {
    HANDLE h;
    ULONGLONG base;     // byte offset of the game partition
    Node* nodes;
    int count, cap;
} Image;

static BOOL ReadAt(HANDLE h, ULONGLONG off, void* buf, DWORD len) {
    OVERLAPPED ov;
    DWORD got = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    return ReadFile(h, buf, len, &got, &ov) && got == len;
}

static int AddNode(Image* im, const uint8_t* e, int parent) {
    if (im->count == im->cap) {
        int cap = im->cap ? im->cap * 2 : 512;
        Node* n = (Node*)realloc(im->nodes, (size_t)cap * sizeof *n);
        if (!n) return -1;
        im->nodes = n;
        im->cap = cap;
    }
    Node* n = &im->nodes[im->count];
    memcpy(n->name, e + 14, e[13]);
    n->name[e[13]] = 0;
    n->attr = e[12];
    memcpy(&n->sector, e + 4, 4);
    memcpy(&n->size, e + 8, 4);
    n->parent = parent;
    return im->count++;
}

// in-order walk of one directory table
static BOOL Walk(Image* im, const uint8_t* table, uint32_t size, uint32_t off4, int parent, int depth, int* budget) {
    uint32_t off = off4 * 4u;
    if (depth > 512 || --*budget < 0 || off + 14 > size) return FALSE;
    const uint8_t* e = table + off;
    uint16_t left = (uint16_t)(e[0] | e[1] << 8), right = (uint16_t)(e[2] | e[3] << 8);
    if (left == 0xFFFF) return TRUE;    // padding: no entry here
    if (!e[13] || off + 14 + e[13] > size) return FALSE;
    if (left && !Walk(im, table, size, left, parent, depth + 1, budget)) return FALSE;
    if (AddNode(im, e, parent) < 0) return FALSE;
    if (right && right != 0xFFFF && !Walk(im, table, size, right, parent, depth + 1, budget)) return FALSE;
    return TRUE;
}

static void Close(Image* im) {
    if (im->h && im->h != INVALID_HANDLE_VALUE) CloseHandle(im->h);
    free(im->nodes);
    memset(im, 0, sizeof *im);
}

static BOOL Open(Image* im, const WCHAR* path) {
    static const ULONGLONG kBases[] = {0, 0x18300000ull, 0x0FD90000ull, 0x02080000ull};  // xiso, XGD1/2/3 dumps
    memset(im, 0, sizeof *im);
    im->h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (im->h == INVALID_HANDLE_VALUE) return FALSE;
    uint8_t vd[0x20];
    uint32_t root_sector = 0, root_size = 0;
    BOOL found = FALSE;
    for (size_t i = 0; i < sizeof kBases / sizeof kBases[0] && !found; i++)
        if (ReadAt(im->h, kBases[i] + 32 * XISO_SECTOR, vd, sizeof vd) && !memcmp(vd, XISO_MAGIC, 20)) {
            im->base = kBases[i];
            memcpy(&root_sector, vd + 0x14, 4);
            memcpy(&root_size, vd + 0x18, 4);
            found = TRUE;
        }
    if (!found) { Close(im); return FALSE; }

    // breadth first: node i's entries are appended when i is reached; -1 is the root
    BOOL ok = TRUE;
    for (int i = -1; ok && i < im->count; i++) {
        uint32_t sector = i < 0 ? root_sector : im->nodes[i].sector;
        uint32_t size = i < 0 ? root_size : im->nodes[i].size;
        if ((i >= 0 && !(im->nodes[i].attr & XISO_ATTR_DIR)) || !size) continue;
        if (size > 64u << 20) { ok = FALSE; break; }
        uint8_t* table = (uint8_t*)malloc(size);
        int budget = (int)(size / 14) + 1;
        ok = table && ReadAt(im->h, im->base + sector * XISO_SECTOR, table, size) &&
             Walk(im, table, size, 0, i, 0, &budget);
        free(table);
    }
    if (!ok) Close(im);
    return ok;
}

static BOOL NodePath(const Image* im, int node, const WCHAR* dir, WCHAR* out) {
    int chain[64], depth = 0;
    for (int n = node; n >= 0 && depth < 64; n = im->nodes[n].parent) chain[depth++] = n;
    if (wcscpy_s(out, MAX_PATH, dir)) return FALSE;
    while (depth--) {
        WCHAR name[256];
        if (MultiByteToWideChar(CP_ACP, 0, im->nodes[chain[depth]].name, -1, name, 256) <= 0) return FALSE;
        if (wcscat_s(out, MAX_PATH, L"\\") || wcscat_s(out, MAX_PATH, name)) return FALSE;
    }
    return TRUE;
}

static BOOL Whole(const WCHAR* path, uint32_t size) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    return GetFileAttributesExW(path, GetFileExInfoStandard, &fad) && !fad.nFileSizeHigh && fad.nFileSizeLow == size;
}

static void SetError(XisoProgress* p, const WCHAR* what, const WCHAR* path, DWORD err) {
    WCHAR sys[160] = L"";
    if (err) FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, sys, 160, NULL);
    for (WCHAR* c = sys; *c; c++)
        if (*c == L'\r' || *c == L'\n') *c = L' ';
    swprintf_s(p->error, sizeof p->error / sizeof p->error[0], L"%ls %ls%ls%ls", what, path ? path : L"",
               err ? L": " : L"", sys);
}

static BOOL ExtractFile(const Image* im, const Node* n, const WCHAR* path, XisoProgress* p, uint8_t* buf) {
    if (Whole(path, n->size)) {     // left whole by an earlier run that stopped part-way
        InterlockedAdd64(&p->done, n->size);
        return TRUE;
    }
    HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        SetError(p, L"Could not create", path, GetLastError());
        return FALSE;
    }
    ULONGLONG at = im->base + n->sector * XISO_SECTOR;
    BOOL ok = TRUE;
    for (DWORD left = n->size; ok && left;) {
        DWORD want = left < XISO_BUF ? left : XISO_BUF, put = 0;
        if (p->cancel) {
            SetError(p, L"Unpacking was cancelled.", NULL, 0);
            ok = FALSE;
        } else if (!ReadAt(im->h, at, buf, want)) {
            SetError(p, L"Could not read the disc image", NULL, GetLastError());
            ok = FALSE;
        } else if (!WriteFile(out, buf, want, &put, NULL) || put != want) {
            SetError(p, L"Could not write", path, GetLastError());
            ok = FALSE;
        } else {
            at += want;
            left -= want;
            InterlockedAdd64(&p->done, want);
        }
    }
    CloseHandle(out);
    if (!ok) DeleteFileW(path);     // never leave a short file that a later run would have to tell apart
    return ok;
}

static BOOL HasImageExtension(const WCHAR* name) {
    const WCHAR* dot = wcsrchr(name, L'.');
    return dot && (!_wcsicmp(dot, L".iso") || !_wcsicmp(dot, L".xiso"));
}

BOOL xiso_Find(const WCHAR* dir, WCHAR* image) {
    WCHAR path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%ls\\default.xbe", dir);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return FALSE;     // already unpacked
    return xiso_FindAny(dir, image);
}

BOOL xiso_FindAny(const WCHAR* dir, WCHAR* image) {
    WCHAR path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(path, &fd);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    BOOL found = FALSE;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !HasImageExtension(fd.cFileName)) continue;
        swprintf_s(image, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        Image im;
        if (Open(&im, image)) {
            Close(&im);
            found = TRUE;
        }
    } while (!found && FindNextFileW(f, &fd));
    FindClose(f);
    return found;
}

BOOL xiso_Extract(const WCHAR* image, const WCHAR* dir, XisoProgress* p) {
    p->done = 0;
    p->total = 0;
    p->error[0] = 0;
    Image im;
    if (!Open(&im, image)) {
        SetError(p, L"Not a readable Xbox disc image:", image, 0);
        return FALSE;
    }
    int xbe = -1;
    for (int i = 0; i < im.count; i++)
        if (im.nodes[i].parent < 0 && !(im.nodes[i].attr & XISO_ATTR_DIR) && !_stricmp(im.nodes[i].name, "default.xbe"))
            xbe = i;
    if (xbe < 0) {
        SetError(p, L"The disc image has no default.xbe.", NULL, 0);
        Close(&im);
        return FALSE;
    }

    // room for what is not there yet
    LONG64 need = 0;
    for (int i = 0; i < im.count; i++) {
        WCHAR path[MAX_PATH];
        if (im.nodes[i].attr & XISO_ATTR_DIR) continue;
        p->total += im.nodes[i].size;
        if (!NodePath(&im, i, dir, path) || !Whole(path, im.nodes[i].size)) need += im.nodes[i].size;
    }
    ULARGE_INTEGER avail;
    if (GetDiskFreeSpaceExW(dir, &avail, NULL, NULL) && avail.QuadPart < (ULONGLONG)need + (64u << 20)) {
        swprintf_s(p->error, sizeof p->error / sizeof p->error[0],
                   L"Not enough free space: unpacking needs %.1f GB, the drive has %.1f GB free.", need / 1e9,
                   avail.QuadPart / 1e9);
        Close(&im);
        return FALSE;
    }

    uint8_t* buf = (uint8_t*)malloc(XISO_BUF);
    BOOL ok = buf != NULL;
    if (!ok) SetError(p, L"Out of memory.", NULL, 0);
    // folders first (breadth-first order: parents before children), then the files, default.xbe last
    for (int i = 0; ok && i < im.count; i++) {
        WCHAR path[MAX_PATH];
        if (!(im.nodes[i].attr & XISO_ATTR_DIR)) continue;
        if (!NodePath(&im, i, dir, path) || (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)) {
            SetError(p, L"Could not create the folder", path, GetLastError());
            ok = FALSE;
        }
    }
    for (int i = 0; ok && i < im.count; i++) {
        WCHAR path[MAX_PATH];
        if ((im.nodes[i].attr & XISO_ATTR_DIR) || i == xbe) continue;
        if (!NodePath(&im, i, dir, path)) {
            SetError(p, L"A path in the image is too long.", NULL, 0);
            ok = FALSE;
        } else {
            ok = ExtractFile(&im, &im.nodes[i], path, p, buf);
        }
    }
    if (ok) {   // under a stand-in name, renamed once whole
        WCHAR path[MAX_PATH], part[MAX_PATH];
        swprintf_s(path, MAX_PATH, L"%ls\\default.xbe", dir);
        swprintf_s(part, MAX_PATH, L"%ls\\default.xbe.part", dir);
        DeleteFileW(part);
        ok = ExtractFile(&im, &im.nodes[xbe], part, p, buf);
        if (ok && !MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING)) {
            SetError(p, L"Could not create", path, GetLastError());
            ok = FALSE;
        }
    }
    free(buf);
    Close(&im);
    return ok;
}
