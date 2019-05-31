#include <arpa/inet.h>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "helper.h"

const int64_t MAX_SPACE_DEFAULT = 52428800;

std::string mcast_addr, shrd_fldr;
int32_t cmd_port, timeout = TIMEOUT_DEFAULT;
int64_t max_space = MAX_SPACE_DEFAULT;

std::vector<std::string> files;

// 0 is signalfd
// 1 is udp socket for most of communications
// 2+ are sockets for connections with specific clients
std::vector<struct pollfd> fds;
std::vector<ConnectionInfo> connections;

void remove_connection(int i) {
    close(connections[i - 2].fd);
    close(connections[i - 2].sock_fd);
    connections.erase(connections.begin() + i - 2);
    fds.erase(fds.begin() + i);
}

void handle_interrupt() {
    std::cerr << "Received SIGINT, exiting\n";

    close(fds[0].fd);
    close(fds[1].fd);
    for (const auto &conn : connections) {
        close(conn.fd);
        close(conn.sock_fd);
    }

    // free the memory
    files.clear();
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
    // TODO(lab) czy liczba bajtów może być 0?
    if (max_space <= 0) {
        throw po::validation_error(po::validation_error::invalid_option_value,
                                   "b", std::to_string(max_space));
    }
}

// returns list of files in shrd_fldr, without prefix (only filenames)
std::vector<std::string> list_files() {
    namespace fs = boost::filesystem;
    std::vector<std::string> result;
    fs::path p(shrd_fldr);
    fs::directory_iterator end_it;

    for (fs::directory_iterator it(p); it != end_it; ++it) {
        if (fs::is_regular_file(it->path())) {
            result.push_back(it->path().filename().string());
            max_space -= fs::file_size(it->path());
        }
    }
    // TODO(lab) co się powinno dziać jak mamy za mało quoty?
    if (max_space < 0) {
        throw std::logic_error(
            "MAX_SPACE is smaller than sum of sizes of files"
            " in shared_dir");
    }
    return result;
}

int connect_to_mcast(std::string mcast_addr, int32_t port) {
    /* opening a socket */
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) {
        throw std::logic_error("Failed to create a socket");
    }

    // TODO(lab) czy moga byc dwa serwery na tej samej maszynie na tym samym
    // porcie?
    /* connecting to a local address and port */
    struct sockaddr_in local_address;
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *)&local_address, sizeof(local_address)) <
        0) {
        throw std::logic_error(
            "Failed to connect to a local address and port");
    }

    /* connecting to multicast group */
    struct ip_mreq ip_mreq;
    ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (inet_aton(mcast_addr.c_str(), &ip_mreq.imr_multiaddr) == 0) {
        throw boost::program_options::validation_error(
            boost::program_options::validation_error::invalid_option_value,
            "g", mcast_addr);
    }

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void *)&ip_mreq,
                   sizeof(ip_mreq)) < 0) {
        throw std::logic_error("Failed to connect to multicast group");
    }

    return sock;
}

void reply_hello(int sock, const cmplx_cmd &cmd) {
    if (!cmd.data.empty()) {
        throw std::runtime_error("Data field not empty in HELLO");
    }
    cmplx_cmd reply;
    reply.cmd = GOOD_DAY;
    reply.cmd_seq = cmd.cmd_seq;
    reply.param = max_space;
    reply.data = mcast_addr;
    reply.addr = cmd.addr;
    send_cmd(reply, sock);
}

void reply_list(int sock, const cmplx_cmd &cmd,
                const std::vector<std::string> &files) {
    simpl_cmd reply;
    reply.cmd = MY_LIST;
    reply.cmd_seq = cmd.cmd_seq;
    reply.addr = cmd.addr;
    for (const auto &file : files) {
        if (file.find(cmd.data) != std::string::npos) {
            if (reply.data.size() + file.size() +
                    (reply.data.empty() ? 0 : 1) >
                DATA_MAX) {
                send_cmd(reply, sock);
                reply.data = "";
            }
            if (!reply.data.empty()) {
                reply.data += "\n";
            }
            reply.data += file;
        }
    }
    if (reply.data.size() > 0) {
        send_cmd(reply, sock);
    }
}

void handle_del(const cmplx_cmd &cmd, std::vector<std::string> &files) {
    namespace fs = boost::filesystem;
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (*it == cmd.data) {
            fs::path p(shrd_fldr + "/" + *it);
            uint64_t change = fs::file_size(p);
            if (!fs::remove(p)) {
                std::cerr << "Failed to remove " << shrd_fldr + "/" + *it
                          << "\n";
            }
            else {
                max_space += change;
                files.erase(it);
            }
            return;
        }
    }
}

void reply_get(int sock, const cmplx_cmd &cmd,
               std::vector<std::string> files) {
    namespace fs = boost::filesystem;

    bool have_file = false;
    for (const auto &file : files) {
        if (file == cmd.data) {
            have_file = true;
            break;
        }
    }
    if (!have_file) {
        char address[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, (void *)(&cmd.addr.sin_addr), address,
                      sizeof(address)) == NULL) {
            throw std::logic_error("this should not be happening");
        }
        std::cerr << "[PCKG ERROR] Skipping invalid package from " << address
                  << ":" << cmd.addr.sin_port
                  << ". (Requested file does not exist)\n";
        return;
    }

    int new_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (new_socket < 0) {
        throw std::logic_error("Failed to create new socket");
    }
    struct sockaddr_in local_address;
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons(0);
    if (bind(new_socket, (struct sockaddr *)&local_address,
             sizeof(local_address)) < 0) {
        close(new_socket);
        throw std::logic_error("Failed to bind new socket");
    }
    if (listen(new_socket, 1) < 0) {
        close(new_socket);
        throw std::logic_error(
            "Failed to switch to listening on a new socket");
    }

    socklen_t len = sizeof(local_address);
    if (getsockname(new_socket, (sockaddr *)(&local_address), &len) < 0) {
        close(new_socket);
        throw std::logic_error("Failed to get port of the new socket");
    }
    cmplx_cmd reply{CONNECT_ME, cmd.cmd_seq, ntohs(local_address.sin_port),
                    cmd.data, cmd.addr};

    int fd = open(std::string(shrd_fldr + "/" + cmd.data).c_str(), O_RDONLY);
    if (fd < 0) {
        // TODO czy tu trzeba wysłać NO_WAY?
        close(new_socket);
        throw std::logic_error("Failed to open requested file");
    }
    std::cout << "Waiting for new connection on port "
              << ntohs(local_address.sin_port) << "\n";
    send_cmd(reply, sock);
    fds.push_back({new_socket, POLLIN, 0});
    connections.emplace_back(boost::posix_time::microsec_clock::local_time(),
                             new_socket, fd, cmd.data, false, true);
}

void reply_add(int sock, const cmplx_cmd &cmd,
               std::vector<std::string> &files) {
    namespace fs = boost::filesystem;

    if (cmd.param > (uint64_t)(max_space)) {
        simpl_cmd reply{NO_WAY, cmd.cmd_seq, cmd.data, cmd.addr};
        send_cmd(reply, sock);
        return;
    }

    for (const auto &file : files) {
        if (file == cmd.data) {
            simpl_cmd reply{NO_WAY, cmd.cmd_seq, cmd.data, cmd.addr};
            send_cmd(reply, sock);
            return;
        }
    }

    if (cmd.data.size() == 0 || cmd.data.find('/') != std::string::npos) {
        simpl_cmd reply{NO_WAY, cmd.cmd_seq, cmd.data, cmd.addr};
        send_cmd(reply, sock);
        return;
    }

    int new_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in local_address;
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons(0);
    if (bind(new_socket, (struct sockaddr *)&local_address,
             sizeof(local_address)) < 0) {
        close(new_socket);
        throw std::logic_error("Failed to bind new socket");
    }
    if (listen(new_socket, 1) < 0) {
        close(new_socket);
        throw std::logic_error(
            "Failed to switch to listening on a new socket");
    }

    int fd = open(std::string(shrd_fldr + "/" + cmd.data).c_str(),
                  O_WRONLY | O_CREAT, 0660);
    if (fd < 0) {
        int e = errno;
        close(new_socket);
        throw std::logic_error(
            std::string("Failed to open requested file for writing ") +
            strerror(e));
    }
    socklen_t len = sizeof(local_address);
    if (getsockname(new_socket, (sockaddr *)(&local_address), &len) < 0) {
        close(new_socket);
        throw std::logic_error("Failed to get port of the new socket");
    }

    cmplx_cmd reply{CAN_ADD, cmd.cmd_seq, ntohs(local_address.sin_port), "",
                    cmd.addr};
    max_space -= cmd.param;
    files.push_back(cmd.data);
    send_cmd(reply, sock);
    fds.push_back({new_socket, POLLIN, 0});
    connections.emplace_back(boost::posix_time::microsec_clock::local_time(),
                             new_socket, fd, cmd.data, false, false);
}

void accept_connection(int i) {
    int new_socket = accept4(fds[i].fd, NULL, NULL, SOCK_NONBLOCK);
    if (new_socket < 0) {
        throw std::logic_error("Failed to accept new connection");
    }

    fds[i].events = 0;
    ConnectionInfo info = connections[i - 2];

    if (info.writing) {
        fds.push_back({new_socket, POLLOUT, 0});
    }
    else {
        fds.push_back({new_socket, POLLIN, 0});
    }
    connections.push_back({boost::posix_time::microsec_clock::local_time(),
                           new_socket, info.fd, info.filename, true,
                           info.writing});
}

void write_to_fd(int i) {
    ConnectionInfo &info = connections[i - 2];
    int len;
    if (info.position == info.buf_size) {
        info.buf_size = read(info.fd, info.buffer, sizeof(info.buffer));
        if (info.buf_size < 0) {
            remove_connection(i);
            throw std::runtime_error("Failed to read requested file");
        }
        if (info.buf_size == 0) {
            remove_connection(i);
            return;
        }
        info.position = 0;
    }
    len = write(info.sock_fd, info.buffer + info.position,
                info.buf_size - info.position);
    if (len < 0) {
        remove_connection(i);
        throw std::runtime_error(std::string("Failed to send requested file ") + strerror(errno));
    }
    info.position += len;
}

void read_from_fd(int i) {
    ConnectionInfo &info = connections[i - 2];
    int len;
    len = read(info.sock_fd, info.buffer, sizeof(info.buffer));
    if (len < 0) {
        remove_connection(i);
        throw std::runtime_error("Failed to receive requested file");
    }
    if (len == 0) {
        remove_connection(i);
        return;
    }
    len = write(info.fd, info.buffer, len);
    if (len < 0) {
        remove_connection(i);
        throw std::runtime_error("Failed to write fileon the disk");
    }
}

void init(int argc, char **argv) {
    namespace po = boost::program_options;
    namespace fs = boost::filesystem;

    po::options_description desc(argv[0] + std::string(" flags"));
    desc.add_options()(",g", po::value<std::string>(&mcast_addr)->required(),
                       "MCAST_ADDR")(",p",
                                     po::value<int32_t>(&cmd_port)->required(),
                                     "CMD_PORT (range [1, 65535]")(
        ",b", po::value<int64_t>(&max_space),
        "MAX_SPACE")(",f", po::value<std::string>(&shrd_fldr)->required(),
                     "SHRD_FLDR")(",t", po::value<int32_t>(&timeout),
                                  "TIMEOUT (range [1, 300], default 5)");

    try {
        parse_args(argc, argv, desc);
        files = list_files();
        for (const auto &file : files) {
            std::cout << file << "\n";
        }
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
        fds.push_back({sfd, POLLIN, 0});

        int sock = connect_to_mcast(mcast_addr, cmd_port);
        fds.push_back({sock, POLLIN, 0});
    }
    catch (po::error &e) {
        std::cerr << "INCORRECT USAGE\n" << e.what() << "\n" << desc;
        exit(-1);
    }
    catch (fs::filesystem_error &e) {
        std::cerr << "INCORRECT USAGE\n" << e.what() << "\n" << desc;
        exit(-1);
    }
    catch (std::exception &e) {
        std::cerr << "ERROR\n" << e.what() << "\n" << desc;
        exit(-1);
    }
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    init(argc, argv);

    int timeout_millis = compute_timeout(connections, timeout);
    while (true) {
        int ready = poll(fds.data(), fds.size(), timeout_millis);
        auto now = boost::posix_time::microsec_clock::local_time();
        if (ready <= 0) {
            // timeout
            for (size_t i = fds.size() - 1; i >= 2; --i) {
                auto duration = now - connections[i - 2].start;
                if (duration.total_microseconds() / 1000000 > timeout) {
                    std::cerr << "Removing connection " << i << "\n";
                    remove_connection(i);
                }
            }
            continue;
        }
        if (fds[0].revents & POLLIN) {
            handle_interrupt();
        }
        if (fds[1].revents & POLLIN) {
            fds[1].revents = 0;
            cmplx_cmd cmd;
            try {
                recv_cmd(cmd, fds[1].fd);
                if (cmd.cmd == HELLO) {
                    reply_hello(fds[1].fd, cmd);
                }
                else if (cmd.cmd == LIST) {
                    reply_list(fds[1].fd, cmd, files);
                }
                else if (cmd.cmd == GET) {
                    reply_get(fds[1].fd, cmd, files);
                }
                else if (cmd.cmd == DEL) {
                    handle_del(cmd, files);
                }
                else if (cmd.cmd == ADD) {
                    reply_add(fds[1].fd, cmd, files);
                }
                else {
                    char address[INET_ADDRSTRLEN];
                    if (inet_ntop(AF_INET, (void *)(&cmd.addr.sin_addr),
                                  address, sizeof(address)) == NULL) {
                        throw std::logic_error("this should not be happening");
                    }
                    std::cerr << "[PCKG ERROR] Skipping invalid package from "
                              << address << ":" << ntohs(cmd.addr.sin_port)
                              << ". (Command " << cmd.cmd << " is unknown)\n";
                }
            }
            catch (std::runtime_error &e) {
                char address[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, (void *)(&cmd.addr.sin_addr), address,
                              sizeof(address)) == NULL) {
                    throw std::logic_error("this should not be happening");
                }
                std::cerr << "[PCKG ERROR] Skipping invalid package from "
                          << address << ":" << ntohs(cmd.addr.sin_port)
                          << ". (" << e.what() << ")\n";
            }
            catch (std::exception &e) {
                std::cerr << "Error occured: " << e.what() << "\n";
            }
        }
        for (size_t i = 2; i < fds.size(); ++i) {
            try {
                if (fds[i].revents & POLLIN) {
                    connections[i - 2].start = now;
                    if (!connections[i - 2].was_accepted) {
                        accept_connection(i);
                    }
                    else {
                        fds[i].revents = 0;
                        read_from_fd(i);
                    }
                }
                if (fds[i].revents & POLLOUT) {
                    connections[i - 2].start = now;
                    fds[i].revents = 0;
                    write_to_fd(i);
                }
            }
            catch (std::exception &e) {
                std::cerr << "Error occured: " << e.what() << "\n";
            }
        }
        timeout_millis = compute_timeout(connections, timeout);
    }
}
