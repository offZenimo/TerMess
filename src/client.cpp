#include "protocol.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>

namespace {

bool sendLine(int fd, std::string line) {
    line.push_back('\n');
    std::string_view remaining = line;
    while (!remaining.empty()) {
        const auto sent = send(fd, remaining.data(), remaining.size(), 0);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            std::cerr << "Could not send data to the server.\n";
            return false;
        }
        remaining.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

class Session {
public:
    Session(int fd, std::string_view name) : fd(fd), name(name) {}

    int run() {
        if (!sendLine(fd, "NAME " + name)) {
            return 1;
        }
        while (!finished) {
            pollfd watched[] = {{fd, POLLIN, 0}, {registered && !quitting ? STDIN_FILENO : -1, POLLIN, 0}};
            if (poll(watched, 2, -1) == -1) {
                if (errno == EINTR) {
                    continue;
                }
                std::perror("poll");
                return 1;
            }
            if (watched[0].revents & (POLLERR | POLLNVAL)) {
                std::cerr << "Connection lost.\n";
                return 1;
            }
            if ((watched[0].revents & (POLLIN | POLLHUP)) && !readServer()) {
                return 1;
            }
            if (!finished && (watched[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) && !readInput()) {
                return 1;
            }
        }
        return 0;
    }

private:
    const int fd;
    const std::string name;
    std::string input;
    std::string responses;
    bool registered = false;
    bool skippingLongLine = false;
    bool quitting = false;
    bool finished = false;

    bool readServer() {
        char buffer[4096];
        const auto received = recv(fd, buffer, sizeof(buffer), 0);
        if (received < 0 && errno == EINTR) {
            return true;
        }
        if (received <= 0) {
            std::cerr << "Connection lost.\n";
            return false;
        }

        responses.append(buffer, static_cast<std::size_t>(received));
        for (auto newline = responses.find('\n'); newline != std::string::npos; newline = responses.find('\n')) {
            if (newline > protocol::maxResponseLength) {
                break;
            }
            const std::string line = responses.substr(0, newline);
            responses.erase(0, newline + 1);
            if (!registered) {
                if (line != "OK NAME") {
                    std::cerr << "Could not set nickname: " << line << '\n';
                    return false;
                }
                registered = true;
                std::cout << "Connected as " << name << ". Use /msg <name> <message> or /quit." << std::endl;
            } else if (line == "BYE") {
                finished = true;
                return true;
            } else if (line.compare(0, 4, "MSG ") == 0) {
                const auto separator = line.find(' ', 4);
                if (separator != std::string::npos) {
                    std::cout << line.substr(4, separator - 4) << ": "
                              << line.substr(separator + 1) << std::endl;
                }
            } else if (line == "OK SEND") {
                std::cout << "Sent." << std::endl;
            } else {
                std::cerr << line << '\n';
            }
        }
        if (responses.size() > protocol::maxResponseLength) {
            std::cerr << "Server response is too long.\n";
            return false;
        }
        return true;
    }

    bool requestQuit() {
        quitting = true;
        return sendLine(fd, "QUIT");
    }

    bool handleInput() {
        if (input == "/quit") {
            return requestQuit();
        }
        if (input.compare(0, 5, "/msg ") == 0) {
            return sendLine(fd, "SEND " + input.substr(5));
        }
        if (!input.empty()) {
            std::cerr << "Use /msg <name> <message> or /quit.\n";
        }
        return true;
    }

    bool readInput() {
        char buffer[4096];
        const auto received = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (received < 0) {
            if (errno == EINTR) {
                return true;
            }
            std::perror("Could not read terminal input");
            return false;
        }
        if (received == 0) {
            if (!skippingLongLine && !finishInputLine()) {
                return false;
            }
            return quitting || requestQuit();
        }

        for (ssize_t index = 0; index < received && !quitting; ++index) {
            const char character = buffer[index];
            if (character == '\n') {
                if (!skippingLongLine && !finishInputLine()) {
                    return false;
                }
                input.clear();
                skippingLongLine = false;
            } else if (!skippingLongLine) {
                input.push_back(character);
                if (input.size() > protocol::maxLineLength + 1) {
                    std::cerr << "Command is too long.\n";
                    skippingLongLine = true;
                }
            }
        }
        return true;
    }

    bool finishInputLine() {
        protocol::trimCarriageReturn(input);
        if (input.size() > protocol::maxLineLength) {
            std::cerr << "Command is too long.\n";
            return true;
        }
        return handleInput();
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: termess-client <nickname> [port]\n";
        return 1;
    }
    const int port = argc == 3 ? protocol::parsePort(argv[2]) : protocol::defaultPort;
    if (port == 0 || !protocol::isValidName(argv[1])) {
        std::cerr << "Use a nickname with 1–24 letters (A–Z), digits, or underscores and a port from 1 to 65535.\n";
        return 1;
    }

    std::signal(SIGPIPE, SIG_IGN);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        std::perror("socket");
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == -1) {
        std::perror("Could not connect to server");
        close(fd);
        return 1;
    }
    const int status = Session(fd, argv[1]).run();
    close(fd);
    return status;
}
