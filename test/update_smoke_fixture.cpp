#include <string_view>

int main(int argc, char** argv) {
    return argc == 2 && std::string_view(argv[1]) == "--smoke-test" ? 0 : 1;
}
