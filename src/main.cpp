#include "server.hpp"
#include "server_config.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <netdb.h>
#include <pthread.h>
#include <print>
#include <stop_token>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);

    const int signal_mask_result = pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

    if (signal_mask_result != 0) {
        std::println(stderr, "Failed to configure shutdown signals: {}",
                     std::strerror(signal_mask_result));
        return 1;
    }

    auto config = parse_arguments(argc, argv);

    if (!config) {
        std::println(stderr, "{}", config.error());
        return 1;
    }

    TcpServer server(std::move(*config));

    auto persistence_result = server.initialize_persistence();

    if (!persistence_result) {
        std::println(stderr, "{}", persistence_result.error());
        return 1;
    }

    auto server_setup_result = server.setup_server();

    if (!server_setup_result.has_value()) {
        std::println(stderr, "{}", server_setup_result.error());
        return 1;
    }

    auto replication_setup_result = server.setup_replication();

    if (!replication_setup_result.has_value()) {
        std::println(stderr, "{}", replication_setup_result.error());
        return 1;
    }

    std::jthread signal_waiter(
        [&server, shutdown_signals](std::stop_token stop_token) {
            constexpr timespec wait_interval{
                .tv_sec = 0,
                .tv_nsec = 500'000'000,
            };

            while (!stop_token.stop_requested()) {
                const int received_signal =
                    sigtimedwait(&shutdown_signals, nullptr, &wait_interval);

                if (received_signal == SIGINT || received_signal == SIGTERM) {
                    server.stop();
                    return;
                }

                if (received_signal < 0 && errno != EAGAIN && errno != EINTR) {
                    std::println(stderr, "Failed while waiting for shutdown signal: {}",
                                 std::strerror(errno));
                    return;
                }
            }
        });

    // Runs the connection accept loop
    server.run();
    signal_waiter.request_stop();

    return 0;
}
