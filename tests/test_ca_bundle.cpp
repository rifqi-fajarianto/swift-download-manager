// CA bundle resolution for libcurl: the Windows package ships ca-bundle.crt
// next to the executable, and a path that names no file must never be handed
// to libcurl (it would trade the handshake failure for error 77).

#include <cstdlib>
#include <fstream>
#include <string>

#include "core/util.h"
#include "test_util.h"

using namespace swiftdm;

namespace {

void write_file(const std::string& path, const std::string& contents) {
    std::ofstream out(path_from_utf8(path), std::ios::binary);
    out << contents;
    CHECK(out.good());
}

}  // namespace

int main() {
    const std::string directory = testutil::make_temp_dir("ca-bundle");
    CHECK(ensure_directory(directory));

    // Nothing shipped next to the executable: libcurl keeps its built-in
    // default, which is what Linux relies on.
    CHECK(resolve_ca_bundle(directory, "").empty());

    // An override that names no file is ignored rather than returned.
    const std::string missing = path_to_utf8(path_from_utf8(directory) / "missing.crt");
    CHECK(resolve_ca_bundle(directory, missing).empty());

    // A directory instead of a file is ignored too.
    const std::string dir_override = path_to_utf8(path_from_utf8(directory) / "not-a-file");
    CHECK(ensure_directory(dir_override));
    CHECK(resolve_ca_bundle(directory, dir_override).empty());

    // The bundle shipped next to the executable is picked up.
    const std::string bundle = path_to_utf8(path_from_utf8(directory) / "ca-bundle.crt");
    write_file(bundle, "test bundle");
    CHECK_EQ(resolve_ca_bundle(directory, ""), bundle);

    // An existing override wins over the shipped bundle.
    const std::string override_bundle =
        path_to_utf8(path_from_utf8(directory) / "override.crt");
    write_file(override_bundle, "override");
    CHECK_EQ(resolve_ca_bundle(directory, override_bundle), override_bundle);

    // A stale override falls back to the shipped bundle instead of failing.
    CHECK_EQ(resolve_ca_bundle(directory, missing), bundle);

    // The executable directory is always resolvable so the shipped bundle
    // can be found at runtime.
    CHECK(!executable_dir().empty());

    testutil::cleanup_dir(directory);
    return testutil::finish("ca_bundle");
}
