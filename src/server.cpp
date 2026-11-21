#include "protocol.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t maxClients = 64;
constexpr std::size_t maxPendingBytes = 64 * 1024;

struct Client {
    explicit Client(int socket) : fd(socket) {}

    int fd;
    std::string name;
    std::string input;
    std::string output;
    bool closeAfterWrite = false;
    bool disconnected = false;

    bool queue(std::string_view line) {
        if (line.size() + 1 > maxPendingBytes - output.size()) {
            disconnected = true;
            return false;
        }
        output.append(line);
        output.push_back('\n');
        return true;
    }
};

bool setNonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

int startListener(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }

    const int reuseAddress = 1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress)) == -1 ||
        !setNonblocking(fd) ||
        bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == -1 ||
        listen(fd, maxClients) == -1) {
        const int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

auto findClient(std::vector<Client>& clients, std::string_view name) {
    return std::find_if(clients.begin(), clients.end(), [name](const Client& peer) {
        return peer.name == name && !peer.disconnected && !peer.closeAfterWrite;
    });
}

void sendMessage(Client& client, std::string_view arguments, std::vector<Client>& clients) {
    if (client.name.empty()) {
        client.queue("ERR NAME_REQUIRED");
        return;
    }

    const auto separator = arguments.find(' ');
    if (separator == std::string_view::npos) {
        client.queue("ERR USAGE_SEND");
        return;
    }
    const auto recipient = arguments.substr(0, separator);
    const auto message = arguments.substr(separator + 1);
    if (!protocol::isValidName(recipient) || message.find_first_not_of(' ') == std::string_view::npos) {
        client.queue("ERR USAGE_SEND");
        return;
    }
    for (const unsigned char byte : message) {
        if (byte < 0x20 || byte == 0x7f) {
            client.queue("ERR INVALID_MESSAGE");
            return;
        }
    }

    const auto peer = findClient(clients, recipient);
    if (peer == clients.end() || !peer->queue("MSG " + client.name + " " + std::string(message))) {
        client.queue("ERR RECIPIENT_OFFLINE");
        return;
    }
    client.queue("OK SEND");
}

void handleLine(Client& client, const std::string& line, std::vector<Client>& clients) {
    if (line == "QUIT") {
        client.queue("BYE");
        client.closeAfterWrite = true;
        return;
    }
    if (line == "SEND" || line.compare(0, 5, "SEND ") == 0) {
        const auto arguments = line == "SEND" ? std::string_view{} : std::string_view(line).substr(5);
        sendMessage(client, arguments, clients);
        return;
    }
    if (line.compare(0, 5, "NAME ") != 0) {
        client.queue("ERR UNKNOWN_COMMAND");
        return;
    }
    if (!client.name.empty()) {
        client.queue("ERR ALREADY_NAMED");
        return;
    }

    const std::string name = line.substr(5);
    if (!protocol::isValidName(name)) {
        client.queue("ERR INVALID_NAME");
        return;
    }
    if (findClient(clients, name) != clients.end()) {
        client.queue("ERR NAME_IN_USE");
        return;
    }
    client.name = name;
    client.queue("OK NAME");
}

void readClient(Client& client, std::vector<Client>& clients) {
    char buffer[4096];
    const auto received = recv(client.fd, buffer, sizeof(buffer), 0);
    if (received == 0) {
        client.closeAfterWrite = true;
        return;
    }
    if (received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            client.disconnected = true;
        }
        return;
    }

    client.input.append(buffer, static_cast<std::size_t>(received));
    while (!client.closeAfterWrite && !client.disconnected) {
        const auto newline = client.input.find('\n');
        if (newline == std::string::npos && client.input.size() <= protocol::maxLineLength + 1) {
            return;
        }
        if (newline != std::string::npos && newline <= protocol::maxLineLength + 1) {
            std::string line = client.input.substr(0, newline);
            client.input.erase(0, newline + 1);
            protocol::trimCarriageReturn(line);
            if (line.size() <= protocol::maxLineLength) {
                handleLine(client, line, clients);
                continue;
            }
        }
        client.queue("ERR LINE_TOO_LONG");
        client.closeAfterWrite = true;
    }
}

void writeClient(Client& client) {
    const auto sent = send(client.fd, client.output.data(), client.output.size(), 0);
    if (sent > 0) {
        client.output.erase(0, static_cast<std::size_t>(sent));
    } else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        client.disconnected = true;
    }
}

int serve(int listener) {
    std::vector<Client> clients;
    std::vector<pollfd> watched;
    clients.reserve(maxClients);
    watched.reserve(maxClients + 1);

    for (;;) {
        watched.clear();
        watched.push_back({listener, POLLIN, 0});
        for (const auto& client : clients) {
            short events = client.closeAfterWrite ? 0 : POLLIN;
            if (!client.output.empty()) {
                events |= POLLOUT;
            }
            watched.push_back({client.fd, events, 0});
        }

        if (poll(watched.data(), watched.size(), -1) == -1) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("poll");
            break;
        }
        if (watched.front().revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "Listener stopped working.\n";
            break;
        }
        for (std::size_t index = 0; index < clients.size(); ++index) {
            auto& client = clients[index];
            const short events = watched[index + 1].revents;
            if (events & (POLLERR | POLLNVAL)) {
                client.disconnected = true;
            }
            if (!client.disconnected && !client.closeAfterWrite && (events & (POLLIN | POLLHUP))) {
                readClient(client, clients);
            }
            if (!client.disconnected && !client.output.empty() && (events & (POLLOUT | POLLHUP))) {
                writeClient(client);
            }
        }
        clients.erase(std::remove_if(clients.begin(), clients.end(), [](const Client& client) {
            if (client.disconnected || (client.closeAfterWrite && client.output.empty())) {
                close(client.fd);
                return true;
            }
            return false;
        }), clients.end());

        if (watched.front().revents & POLLIN) {
            const int fd = accept(listener, nullptr, nullptr);
            if (fd != -1) {
                if (clients.size() < maxClients && setNonblocking(fd)) {
                    clients.emplace_back(fd);
                } else {
                    close(fd);
                }
            }
        }
    }
    for (const auto& client : clients) {
        close(client.fd);
    }
    return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    const int port = argc == 2 ? protocol::parsePort(argv[1]) : protocol::defaultPort;
    if (argc > 2 || port == 0) {
        std::cerr << "Usage: termess-server [port]\n";
        return 1;
    }

    std::signal(SIGPIPE, SIG_IGN);
    const int listener = startListener(port);
    if (listener == -1) {
        std::perror("Could not start server");
        return 1;
    }
    std::cout << "Listening on 127.0.0.1:" << port << '\n' << std::flush;
    const int status = serve(listener);
    close(listener);
    return status;
}
