#include "server.hpp"
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <print>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    TcpServer server{};

    auto setup_result = server.setup_server();

    if (!setup_result.has_value()) {
        std::println(stderr, "{}", setup_result.error());
        return 1;
    }

    // Runs the connection accept loop
    server.run();

    return 0;
}
