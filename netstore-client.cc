#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/program_options.hpp>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/signalfd.h>

#include "helper.h"

std::string mcast_addr, out_fldr;
int32_t cmd_port, timeout = TIMEOUT_DEFAULT;

// main udp socket used for most of communications
int main_socket;
// 0 is signalfd
// 1 is main socket (or -1 if we're not listening on it)
// 2 is stdin
std::vector<struct pollfd> fds;
std::vector<ConnectionInfo> connections;

void remove_connection(int i) {
    close(connections[i - 3].fd);
    close(connections[i - 3].sock_fd);
    connections.erase(connections.begin() + i - 3);
    fds.erase(fds.begin() + i);
}

void handle_interrupt() {
    std::cerr << "Received SIGINT, exiting\n";

    close(fds[0].fd);
    close(main_socket);
    for (const auto &conn : connections) {
        close(conn.fd);
        close(conn.sock_fd);
    }

    // free the memory
    fds.clear();
    connections.clear();
    exit(EXIT_INTERRUPT);
}

void parse_args(int argc, char **argv,
                const boost::program_options::options_description &desc) {
    namespace po = boost::program_options;
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (timeout <= 0 || timeout > TIMEOUT_MAX) {
        throw po::validation_error(po::validation_error::invalid_option_value,
                                   "t", std::to_string(timeout));
    }
    if (cmd_port <= 0 || cmd_port > PORT_MAX) {
        throw po::validation_error(po::validation_error::invalid_option_value,
                                   "p", std::to_string(cmd_port));
    }
}

struct sockaddr_in get_remote_address(const std::string &colon_address,
                                      in_port_t port) {
    struct sockaddr_in remote_address;
    remote_address.sin_family = AF_INET;
    if (inet_pton(AF_INET, colon_address.c_str(), &remote_address.sin_addr) <
        0) {
        throw boost::program_options::validation_error(
            boost::program_options::validation_error::invalid_option_value,
            "g", colon_address);
    }
    remote_address.sin_port = htons(port);
    return remote_address;
}

// returns servers, where first is sockaddr, and second is their free space
std::vector<std::pair<struct sockaddr_in, int64_t>>
discover(int sock, const struct sockaddr_in &remote_address) {
    std::vector<std::pair<struct sockaddr_in, int64_t>> result;
    simpl_cmd cmd;
    cmd.cmd = HELLO;
    cmd.cmd_seq = get_cmd_seq();
    cmd.addr = remote_address;
    send_cmd(cmd, sock);

    cmplx_cmd reply;
    struct timeval tval;
    auto start = boost::posix_time::microsec_clock::local_time();
    do {
        tval.tv_sec = timeout;
        tval.tv_usec = 0;
        auto elapsed = boost::posix_time::microsec_clock::local_time() - start;
        int64_t total_microsec = elapsed.total_microseconds();
        tval.tv_sec -= total_microsec / 1000000;
        if (tval.tv_sec < 0) {
            break;
        }

        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (void *)&tval,
                       sizeof(tval)) < 0) {
            throw std::logic_error("setsockopt " + std::to_string(errno) +
                                   " " + strerror(errno));
        }
        try {
            recv_cmd(reply, sock);
            char address[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, (void *)(&reply.addr.sin_addr), address,
                          sizeof(address)) == NULL) {
                throw std::logic_error("this should not be happening");
            }

            if (reply.cmd_seq != cmd.cmd_seq) {
                std::cerr << "[PCKG ERROR]  Skipping invalid package from "
                          << address << ":" << reply.addr.sin_port
                          << "(invalid cmd_seq)\n";
                continue;
            }
            if (reply.cmd != GOOD_DAY) {
                std::cerr << "[PCKG ERROR]  Skipping invalid package from "
                          << address << ":" << reply.addr.sin_port
                          << "(command is not GOOD_DAY when it should)\n";
                continue;
            }
            result.emplace_back(reply.addr, reply.param);
            std::cout << "Found " << address << " (" << reply.data
                      << ") with free space " << reply.param << "\n";
        }
        catch (ReceiveTimeOutException &e) {
            break;
        }
    } while (true);
    return result;
}

// return pairs of <server addres, list of files there>
std::vector<std::pair<struct sockaddr_in, std::vector<std::string>>>
search(int sock, const struct sockaddr_in &remote_address,
       const std::string &needle) {
    std::vector<std::pair<struct sockaddr_in, std::vector<std::string>>>
        result;
    simpl_cmd cmd;
    cmd.cmd = LIST;
    cmd.cmd_seq = get_cmd_seq();
    cmd.addr = remote_address;
    cmd.data = needle;
    send_cmd(cmd, sock);

    cmplx_cmd reply;
    struct timeval tval;
    auto start = boost::posix_time::microsec_clock::local_time();
    do {
        tval.tv_sec = timeout;
        tval.tv_usec = 0;
        auto elapsed = boost::posix_time::microsec_clock::local_time() - start;
        int64_t total_microsec = elapsed.total_microseconds();
        tval.tv_sec -= total_microsec / 1000000;
        if (tval.tv_sec < 0) {
            break;
        }

        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (void *)&tval,
                       sizeof(tval)) < 0) {
            throw std::logic_error("setsockopt " + std::to_string(errno) +
                                   " " + strerror(errno));
        }
        try {
            recv_cmd(reply, sock);
            char address[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, (void *)(&reply.addr.sin_addr), address,
                          sizeof(address)) == NULL) {
                throw std::logic_error("this should not be happening");
            }

            if (reply.cmd_seq != cmd.cmd_seq) {
                std::cerr << "[PCKG ERROR]  Skipping invalid package from "
                          << address << ":" << reply.addr.sin_port
                          << "(invalid cmd_seq)\n";
                continue;
            }
            if (reply.cmd != MY_LIST) {
                std::cerr << "[PCKG ERROR]  Skipping invalid package from "
                          << address << ":" << reply.addr.sin_port
                          << "(command is not MY_LIST when it should)\n";
                continue;
            }
            std::vector<std::string> tmp;
            boost::split(tmp, reply.data, [](char c) { return c == '\n'; });
            result.emplace_back(reply.addr, tmp);
        }
        catch (ReceiveTimeOutException &e) {
            break;
        }
    } while (true);
    return result;
}

void remove(int sock, const struct sockaddr_in &remote_address,
            const std::string &filename) {
    simpl_cmd cmd;
    cmd.cmd = DEL;
    cmd.cmd_seq = get_cmd_seq();
    cmd.addr = remote_address;
    cmd.data = filename;
    send_cmd(cmd, sock);
}

// TODO te funkcje muszą zapisywać te swoje cmd_seq i dodawać odpowiednie
// rzeczy, jak dostaną odpowiedź od serwera na głównym sockecie, to muszą
// otworzyć te odpowiednie deskryptory i porobić, co trzeba
void fetch(int sock, const struct sockaddr_in &remote_address,
           const std::string &filename) {
    simpl_cmd cmd;
    cmd.cmd = GET;
    cmd.cmd_seq = get_cmd_seq();
    cmd.addr = remote_address;
    cmd.data = filename;
    send_cmd(cmd, sock);
}

void upload(int sock, const struct sockaddr_in &remote_address,
            const std::string &filename) {
    cmplx_cmd cmd;
    cmd.cmd = ADD;
    cmd.cmd_seq = get_cmd_seq();
    cmd.param = 50; // TODO rozmiar pliku
    std::cerr << "Filename = " << filename << "\n";
    std::cerr << "seq = " << cmd.cmd_seq << "\n";
    cmd.data = filename;
    cmd.addr = remote_address;
    send_cmd(cmd, sock);
}

void handle_server_answer() {
    cmplx_cmd cmd;
    recv_cmd(cmd, main_socket);
    if (cmd.cmd == CONNECT_ME) {
        //    int new_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        int new_socket = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in local_address;
        local_address.sin_family = AF_INET;
        local_address.sin_addr.s_addr = htonl(INADDR_ANY);
        local_address.sin_port = htons(0);
        if (bind(new_socket, (struct sockaddr *)&local_address,
                 sizeof(local_address)) < 0) {
            close(new_socket);
            throw std::logic_error("Failed to bind new socket");
        }
        struct sockaddr_in remote_address = cmd.addr;
        remote_address.sin_port = cmd.param;
        std::cerr << "Connecting new socket to port " << cmd.param << "\n";
        connect(new_socket, (struct sockaddr *)(&remote_address),
                sizeof(remote_address));
        std::string full_name = out_fldr + "/" + cmd.data;
        int fd = open(full_name.c_str(), O_WRONLY | O_CREAT, 0660);
        if (fd < 0) {
            close(new_socket);
            throw std::logic_error("Failed to open file for writing");
        }

        char buffer[BUFFER_SIZE];
        ssize_t len;
        do {
            len = read(new_socket, buffer, sizeof(buffer));
            if (len < 0) {
                int e = errno;
                close(new_socket);
                close(fd);
                throw std::logic_error(std::string("Failed to read ") +
                                       strerror(e));
            }
            if (len == 0) {
                close(new_socket);
                close(fd);
                return;
            }

            if (write(fd, buffer, len) != len) {
                close(new_socket);
                close(fd);
                throw std::logic_error("Failed to write");
            }
        } while (true);
    }
    else if (cmd.cmd == CAN_ADD) {
        std::cerr << "Received CAN_ADD\n";
        //    int new_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        int new_socket = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in local_address;
        local_address.sin_family = AF_INET;
        local_address.sin_addr.s_addr = htonl(INADDR_ANY);
        local_address.sin_port = htons(0);
        if (bind(new_socket, (struct sockaddr *)&local_address,
                 sizeof(local_address)) < 0) {
            close(new_socket);
            throw std::logic_error("Failed to bind new socket");
        }
        struct sockaddr_in remote_address = cmd.addr;
        remote_address.sin_port = cmd.param;
        std::cerr << "Connecting new socket to port " << cmd.param << "\n";
        connect(new_socket, (struct sockaddr *)(&remote_address),
                sizeof(remote_address));
        std::string full_name = out_fldr + "/" + cmd.data;
        std::cerr << "Opening file " << full_name << "\n";
        int fd = open(full_name.c_str(), O_RDONLY);
        if (fd < 0) {
            close(new_socket);
            throw std::logic_error("Failed to open file for reading");
        }

        char buffer[BUFFER_SIZE];
        ssize_t len;
        do {
            len = read(fd, buffer, sizeof(buffer));
            if (len < 0) {
                int e = errno;
                close(new_socket);
                close(fd);
                throw std::logic_error(std::string("Failed to read!! ") +
                                       strerror(e));
            }
            if (len == 0) {
                close(new_socket);
                close(fd);
                return;
            }

            if (write(new_socket, buffer, len) != len) {
                close(new_socket);
                close(fd);
                throw std::logic_error("Failed to write");
            }
        } while (true);
    }
    else {
        std::cerr << "Received " << cmd.cmd << "\n";
    }
}

void test_fetch_file() {
    std::cerr << "Test fetching file\n";
    const auto files_list =
        search(main_socket, get_remote_address(mcast_addr, cmd_port), "");
    for (const auto &package : files_list) {
        char address[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, (void *)(&package.first.sin_addr), address,
                      sizeof(address)) == NULL) {
            throw std::logic_error("this should not be happening");
        }
        for (const auto &file : package.second) {
            std::cout << file << " (" << address << ")\n";
        }
    }
    fetch(main_socket, files_list[0].first, "tescik");
    handle_server_answer();
}

void test_upload_file() {
    std::cerr << "Test uploading file\n";
    const auto servers =
        discover(main_socket, get_remote_address(mcast_addr, cmd_port));
    upload(main_socket, servers[0].first, "tescik");
    handle_server_answer();
}

void handle_user_input() {
}

void init(int argc, char **argv) {
    namespace po = boost::program_options;
    po::options_description desc(argv[0] + std::string(" flags"));
    desc.add_options()(",g", po::value<std::string>(&mcast_addr)->required(),
                       "MCAST_ADDR")(",p",
                                     po::value<int32_t>(&cmd_port)->required(),
                                     "CMD_PORT (range [1, 65535]")(
        ",o", po::value<std::string>(&out_fldr)->required(), "OUT_FLDR")(
        ",t", po::value<int32_t>(&timeout), "TIMEOUT (range [1, 300])");

    try {
        parse_args(argc, argv, desc);

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
            throw std::logic_error("Failed to block default SIGINT handling");
        }
        int sfd = signalfd(-1, &mask, SFD_NONBLOCK);
        if (sfd < 0) {
            throw std::logic_error("Failed to open signalfd");
        }
        main_socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (main_socket < 0) {
            throw std::logic_error("Failed to create a socket");
        }
        /* connecting to a local address and port */
        struct sockaddr_in local_address;
        local_address.sin_family = AF_INET;
        local_address.sin_addr.s_addr = htonl(INADDR_ANY);
        local_address.sin_port = htons(0);
        if (bind(main_socket, (struct sockaddr *)&local_address,
                 sizeof(local_address)) < 0) {
            throw std::logic_error("Failed to connect to a local address "
                                   "and port");
        }

        fds.push_back({sfd, POLLIN, 0});

        // this will be main_socket when we want to receive answer from servers
        fds.push_back({-1, POLLIN, 0});

        fds.push_back({STDIN_FILENO, POLLIN, 0});
    }
    catch (po::error &e) {
        std::cerr << "INCORRECT USAGE\n" << e.what() << "\n" << desc;
        exit(-1);
    }
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    init(argc, argv);

    struct sockaddr_in remote_address =
        get_remote_address(mcast_addr, cmd_port);

    test_upload_file();

    /*    int timeout_millis = compute_timeout(connections, timeout);
        while (true) {
            int ready = poll(fds.data(), fds.size(), timeout_millis);
            if (ready <= 0) {
                // timeout
                auto now = boost::posix_time::microsec_clock::local_time();
                for (size_t i = fds.size() - 1; i >= 2; --i) {
                    auto duration = connections[i - 2].start - now;
                    if (duration.total_microseconds() / 1000000 < timeout) {
                        remove_connection(i);
                    }
                }
                continue;
            }
            if (fds[0].revents & POLLIN) {
                handle_interrupt();
            }
            if (fds[1].revents & POLLIN) {
                handle_server_answer();
            }
            if (fds[2].revents & POLLIN) {
                handle_user_input();
            }
        }
        discover(main_socket, remote_address);
        const auto files_list = search(main_socket, remote_address, "");
        for (const auto &package : files_list) {
            char address[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, (void *)(&package.first.sin_addr), address,
                          sizeof(address)) == NULL) {
                throw std::logic_error("this should not be happening");
            }
            for (const auto &file : package.second) {
                std::cout << file << " (" << address << ")\n";
            }
        }
        remove(main_socket, remote_address, "helper.o");
        */
}
