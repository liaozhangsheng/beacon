#include <beacon/ui/profile.hpp>
#include <chrono>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4)
        return 2;
    std::stop_source stop;
    std::jthread cancel;
    if (argc == 4) {
        cancel = std::jthread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.request_stop();
        });
    }
    auto data = beacon::download_profile_asset(argv[1], std::stoull(argv[2]), stop.get_token());
    if (!data)
        return 1;
    std::cout << *data;
}
