#ifndef TEST_FIXTURE_FILE_CREATE_H
#define TEST_FIXTURE_FILE_CREATE_H
#include <stdio.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif
static FILE *fixture_file_create(const char *path)
{
#ifdef _WIN32
    int descriptor=_open(path,_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
    if(descriptor<0) return NULL;
    FILE *file=_fdopen(descriptor,"wb");
    if(!file) { int code=errno; _close(descriptor); errno=code; }
    return file;
#else
    return fopen(path,"wbx");
#endif
}
#endif
