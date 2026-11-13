#include <cstdio>

int main() {
    std::printf("before the fault\n");
    std::fflush(stdout);
    volatile int* nowhere = reinterpret_cast<int*>(0xfffffff0u);
    *nowhere = 1;
    std::printf("after the fault\n");
    return 3;
}
