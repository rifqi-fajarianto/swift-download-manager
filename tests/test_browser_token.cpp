// Headless checks for the browser pairing token (docs/spec/08-media-download.md
// §7). No GUI: the token is plain file + string handling.

#include <sys/stat.h>

#include <cstdio>
#include <string>

#include "core/browser_token.h"
#include "test_util.h"

using namespace swiftdm;

namespace {

void test_generate_shape() {
    const std::string token = BrowserToken::generate();
    CHECK_EQ(token.size(), size_t(43));
    CHECK(token.find_first_not_of(
              "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
          std::string::npos);

    const std::string other = BrowserToken::generate();
    CHECK(!token.empty());
    CHECK(token != other);
}

void test_create_and_load() {
    const std::string dir = testutil::make_temp_dir("browser-token");
    const std::string path = dir + "/browser-token";

    CHECK(BrowserToken::load(path).empty());

    std::string error;
    const std::string created = BrowserToken::create(path, &error);
    CHECK(!created.empty());
    CHECK(error.empty());
    CHECK_EQ(BrowserToken::load(path), created);

#ifndef _WIN32
    struct stat info {};
    CHECK(::stat(path.c_str(), &info) == 0);
    CHECK_EQ(info.st_mode & 0777, mode_t(0600));
#endif

    // Creating again replaces the token, which revokes the old pairing.
    const std::string second = BrowserToken::create(path, &error);
    CHECK(!second.empty());
    CHECK(second != created);
    CHECK_EQ(BrowserToken::load(path), second);

    testutil::cleanup_dir(dir);
}

void test_verify() {
    const std::string token = BrowserToken::generate();
    CHECK(BrowserToken::verify(token, token));
    CHECK(!BrowserToken::verify(token, ""));
    CHECK(!BrowserToken::verify("", token));
    CHECK(!BrowserToken::verify("", ""));

    std::string wrong = token;
    wrong[wrong.size() - 1] = wrong.back() == 'A' ? 'B' : 'A';
    CHECK(!BrowserToken::verify(token, wrong));
    CHECK(!BrowserToken::verify(token, token.substr(0, token.size() - 1)));
    CHECK(!BrowserToken::verify(token, token + "A"));
}

void test_default_path() {
    const std::string path = BrowserToken::default_path();
    CHECK(!path.empty());
    CHECK(path.size() > 13);
    CHECK_EQ(path.substr(path.size() - 13), std::string("browser-token"));
}

}  // namespace

int main() {
    test_generate_shape();
    test_create_and_load();
    test_verify();
    test_default_path();
    return testutil::finish("browser_token");
}
