#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>
int main(int argc, char** argv) {
    std::vector<std::string> v(argv + 1, argv + argc);
    std::sort(v.begin(), v.end());
    std::printf("hello from openkal-dos: musl and libc++ over DPMI, %d args\n", argc);
    for (auto& s : v) std::printf("  %s\n", s.c_str());
    try { throw std::runtime_error("an exception crossed a frame"); }
    catch (const std::exception& e) { std::printf("caught: %s\n", e.what()); }
    const char* p = std::getenv("PATH");
    std::printf("PATH=%s\n", p ? p : "(none)");
    return 7;
}
