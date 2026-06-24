// Unit tests for util helpers.
#include "capture.h"
#include <cstdio>
#include <string>

static int fails = 0;
static void eq(const std::string &got, const std::string &want, const char *what) {
    if (got != want) {
        std::printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got.c_str(), want.c_str());
        fails++;
    }
}

int main() {
    // bare device name -> driver=<name>
    eq(normalize_driver("hackrf"), "driver=hackrf", "bare name");
    eq(normalize_driver("rtlsdr"), "driver=rtlsdr", "bare name 2");
    // empty -> empty (first device)
    eq(normalize_driver(""), "", "empty");

    std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
