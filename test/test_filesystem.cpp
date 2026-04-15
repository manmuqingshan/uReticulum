// Note: this file was generated with the help of offline AI.
#include "doctest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>

#include "ureticulum/filesystem.h"
#include "ureticulum/filesystems/posix.h"
#include "ureticulum/identity.h"

using namespace RNS;

namespace {
    std::string tmp_path(const char* tag) {
        char buf[256];
        snprintf(buf, sizeof(buf), "/tmp/ureticulum_test_%s_%d", tag, (int)getpid());
        return std::string(buf);
    }
}

TEST_CASE("PosixFileSystem write/read/remove round-trip") {
    auto fs = std::make_shared<FileSystems::PosixFileSystem>();
    FileSystem::set_impl(fs);
    REQUIRE(FileSystem::available());

    auto path = tmp_path("rwroundtrip");
    Bytes data("a few bytes worth of payload");
    CHECK(FileSystem::write_file(path.c_str(), data) == data.size());
    CHECK(FileSystem::file_exists(path.c_str()));

    Bytes loaded;
    CHECK(FileSystem::read_file(path.c_str(), loaded) == data.size());
    CHECK(loaded == data);

    CHECK(FileSystem::remove_file(path.c_str()));
    CHECK_FALSE(FileSystem::file_exists(path.c_str()));

    FileSystem::set_impl(nullptr);
}

TEST_CASE("Identity survives a save/reload cycle") {
    auto fs = std::make_shared<FileSystems::PosixFileSystem>();
    FileSystem::set_impl(fs);

    auto path = tmp_path("identity");

    Identity original;
    Bytes orig_pub  = original.get_public_key();
    Bytes orig_prv  = original.get_private_key();
    Bytes orig_hash = original.hash();

    REQUIRE(original.to_file(path.c_str()));

    Identity reloaded = Identity::from_file(path.c_str());
    REQUIRE(static_cast<bool>(reloaded));
    CHECK(reloaded.get_public_key()  == orig_pub);
    CHECK(reloaded.get_private_key() == orig_prv);
    CHECK(reloaded.hash()            == orig_hash);

    /* And the reloaded identity must still be able to sign/verify and
     * encrypt/decrypt. */
    Bytes msg("after the reload");
    Bytes sig = reloaded.sign(msg);
    CHECK(reloaded.validate(sig, msg));

    Bytes ct = reloaded.encrypt(msg);
    CHECK(reloaded.decrypt(ct) == msg);

    FileSystem::remove_file(path.c_str());
    FileSystem::set_impl(nullptr);
}
