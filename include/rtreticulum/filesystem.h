#pragma once

#include <memory>

#include "rtreticulum/bytes.h"

namespace RNS {

    /* Pluggable file storage interface. The library never touches files
     * directly — firmware (or host tests) provides a FileSystemImpl that
     * forwards to whatever backing store is available (LittleFS, SPIFFS,
     * FatFS, POSIX stdio, etc.). */
    class FileSystemImpl {
    public:
        virtual ~FileSystemImpl() = default;

        virtual bool   file_exists(const char* path) = 0;
        virtual size_t read_file(const char* path, Bytes& out) = 0;
        virtual size_t write_file(const char* path, const Bytes& data) = 0;
        virtual bool   remove_file(const char* path) = 0;
    };

    class FileSystem {
    public:
        static void                            set_impl(std::shared_ptr<FileSystemImpl> impl);
        static const std::shared_ptr<FileSystemImpl>& impl();
        static bool                            available();

        static bool   file_exists(const char* path);
        static size_t read_file(const char* path, Bytes& out);
        static size_t write_file(const char* path, const Bytes& data);
        static bool   remove_file(const char* path);

    private:
        static std::shared_ptr<FileSystemImpl> _impl;
    };

}
