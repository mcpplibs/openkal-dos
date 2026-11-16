// The reproduction of openkal-dos#3: sixty-four frames of 8 KiB each, 512 KiB of stack. Within the
// default stack (1 MiB) the recursion returns; tests/stack-small declares 64 KiB and faults.
#include <cstdio>

namespace {

int descend(int depth) {
    volatile char frame[8192];
    frame[0] = static_cast<char>(depth);
    frame[sizeof frame - 1] = static_cast<char>(depth);
    if (depth == 0) return frame[0];
    return descend(depth - 1) + frame[sizeof frame - 1];
}

}  // namespace

int main() {
    std::printf("before the recursion\n");
    std::fflush(stdout);
    std::printf("sum %d\n", descend(64));
    return 0;
}
