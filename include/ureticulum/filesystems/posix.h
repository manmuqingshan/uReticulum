#pragma once

#include "ureticulum/filesystem.h"

namespace RNS { namespace FileSystems {

    /* POSIX-stdio-backed FileSystemImpl. Used by host tests and any
     * non-embedded build. */
    class PosixFileSystem : public FileSystemImpl {
    public:
        bool   file_exists(const char* path) override;
        size_t read_file(const char* path, Bytes& out) override;
        size_t write_file(const char* path, const Bytes& data) override;
        bool   remove_file(const char* path) override;
    };

}}
