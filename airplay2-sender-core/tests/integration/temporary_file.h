#ifndef TEST_INTEGRATION_TEMPORARY_FILE_H
#define TEST_INTEGRATION_TEMPORARY_FILE_H
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
/* MSVCRT tmpfile() places the scratch file at the root of the current drive,
   which fails without administrative rights; the system temporary directory works. */
static FILE *temporary_file(void)
{
    char directory[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(sizeof directory, directory) ||
        !GetTempFileNameA(directory, "w2a", 0, path)) return NULL;
    HANDLE handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (handle == INVALID_HANDLE_VALUE) return NULL;
    int descriptor = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY);
    if (descriptor == -1) { CloseHandle(handle); return NULL; }
    FILE *file = _fdopen(descriptor, "w+b");
    if (!file) { int code = errno; _close(descriptor); errno = code; return NULL; }
    return file;
}
#else
static FILE *temporary_file(void)
{
    return tmpfile();
}
#endif
#endif
