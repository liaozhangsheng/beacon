// Drives the production HTTPS client for test/https_test.py:
//   https_probe URL LIMIT [cancel]      prints the body
//   https_probe URL SIZE DEST SHA256    downloads a verified artifact
#include <beacon/http/client.hpp>
#include <beacon/update/download.hpp>

#include <chrono>
#include <iostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 3 || argc > 5)
        return 2;
    const bool cancel = argc == 4;
    const bool slow_response = std::string_view(argv[1]).ends_with("/slow");
    beacon::http::Limits limits{.max_bytes = std::stoull(argv[2]),
                                .timeout_seconds = cancel          ? 10
                                                   : slow_response ? 2
                                                                   : 5,
                                .connect_timeout_seconds = 3,
                                .max_redirects = 3};
    if (argc == 5) {
        auto result = beacon::update::download_file(argv[1], argv[3], limits.max_bytes, argv[4], limits);
        if (!result) {
            std::cerr << result.error().message;
            return 1;
        }
        return 0;
    }
    std::stop_source stop;
    std::jthread canceller;
    if (cancel) {
        canceller = std::jthread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.request_stop();
        });
    }
    auto result = beacon::http::get_text(argv[1], limits, stop.get_token());
    if (!result) {
        std::cerr << result.error().message;
        return 1;
    }
    std::cout << *result;
}
