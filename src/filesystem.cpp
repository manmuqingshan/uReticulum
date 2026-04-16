#include "rtreticulum/filesystem.h"

namespace RNS {

std::shared_ptr<FileSystemImpl> FileSystem::_impl;

void FileSystem::set_impl(std::shared_ptr<FileSystemImpl> impl) {
    _impl = std::move(impl);
}

const std::shared_ptr<FileSystemImpl>& FileSystem::impl() { return _impl; }
bool FileSystem::available()                              { return _impl != nullptr; }

bool   FileSystem::file_exists(const char* path)                          { return _impl ? _impl->file_exists(path) : false; }
size_t FileSystem::read_file(const char* path, Bytes& out)                { return _impl ? _impl->read_file(path, out) : 0; }
size_t FileSystem::write_file(const char* path, const Bytes& data)        { return _impl ? _impl->write_file(path, data) : 0; }
bool   FileSystem::remove_file(const char* path)                          { return _impl ? _impl->remove_file(path) : false; }

}
