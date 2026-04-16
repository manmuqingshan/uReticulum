#include "rtreticulum/filesystems/posix.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace RNS { namespace FileSystems {

bool PosixFileSystem::file_exists(const char* path) {
    struct stat st;
    return ::stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

size_t PosixFileSystem::read_file(const char* path, Bytes& out) {
    FILE* f = ::fopen(path, "rb");
    if (!f) return 0;
    ::fseek(f, 0, SEEK_END);
    long size = ::ftell(f);
    if (size < 0) { ::fclose(f); return 0; }
    ::fseek(f, 0, SEEK_SET);
    uint8_t* buf = out.writable(static_cast<size_t>(size));
    size_t n = ::fread(buf, 1, static_cast<size_t>(size), f);
    ::fclose(f);
    return n;
}

size_t PosixFileSystem::write_file(const char* path, const Bytes& data) {
    FILE* f = ::fopen(path, "wb");
    if (!f) return 0;
    size_t n = ::fwrite(data.data(), 1, data.size(), f);
    ::fclose(f);
    return n;
}

bool PosixFileSystem::remove_file(const char* path) {
    return ::unlink(path) == 0;
}

}}
