#include <arpa/inet.h>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <csignal>
#include <errno.h>
#include <future>
#include <iostream>
#include <list>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "helper.h"

const int64_t MAX_SPACE_DEFAULT = 52428800;

std::string mcast_addr, shrd_fldr;
int32_t cmd_port, timeout = TIMEOUT_DEFAULT;
int64_t max_space = MAX_SPACE_DEFAULT;

// global, shared among all threads, need to be cleaned up at exit
std::vector<std::string> files;
Socket sock;
std::set<int> active_sockets;
std::list<std::future<bool>> active_futures;

void interrupt_handler(int) {
    std::cerr << "Received SIGINT, exiting\n";

    close(sock.sock);
    sock.sock = 0;
    for (int sock : active_sockets) {
        close(sock);
    }

    // free the memory
    files.clear();
    active_sockets.clear();
    active_futures.clear();
    exit(0);
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
        throw std::logic_error("MAX_SPACE is smaller than sum of sizes of files"
                               " in shared_dir");
    }
    return result;
}

int connect_to_mcast(std::string mcast_addr, int32_t port) {
    /* opening a socket */
    Socket sock(socket(AF_INET, SOCK_DGRAM, 0));
    if (sock.sock < 0) {
        throw std::logic_error("Failed to create a socket");
    }

    // TODO(lab) czy moga byc dwa serwery na tej samej maszynie na tym samym
    // porcie?
    /* connecting to a local address and port */
    struct sockaddr_in local_address;
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons(port);
    if (bind(sock.sock, (struct sockaddr *)&local_address,
             sizeof(local_address)) < 0) {
        throw std::logic_error("Failed to connect to a local address and port");
    }

    /* connecting to multicast group */
    struct ip_mreq ip_mreq;
    ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (inet_aton(mcast_addr.c_str(), &ip_mreq.imr_multiaddr) == 0) {
        throw boost::program_options::validation_error(
            boost::program_options::validation_error::invalid_option_value, "g",
            mcast_addr);
    }

    if (setsockopt(sock.sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void *)&ip_mreq,
                   sizeof(ip_mreq)) < 0) {
        throw std::logic_error("Failed to connect to multicast group");
    }

    int result = sock.sock;
    sock.sock = 0;

    return result;
}

void reply_hello(int sock, const struct cmplx_cmd &cmd) {
    struct cmplx_cmd reply;
    reply.cmd = GOOD_DAY;
    reply.cmd_seq = cmd.cmd_seq;
    reply.param = max_space;
    reply.data = mcast_addr;
    reply.addr = cmd.addr;
    send_cmd(reply, sock);
}

void reply_list(int sock, const struct cmplx_cmd &cmd,
                const std::vector<std::string> &files) {
    struct simpl_cmd reply;
    reply.cmd = MY_LIST;
    reply.cmd_seq = cmd.cmd_seq;
    reply.addr = cmd.addr;
    // TODO(lab) jak dobrze ograniczac rozmiar wysylanego pakietu
    for (const auto &file : files) {
        if (file.find(cmd.data) != std::string::npos) {
            if (reply.data.size() + file.size() + (reply.data.empty() ? 0 : 1) >
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

void handle_del(const struct cmplx_cmd &cmd, std::vector<std::string> &files) {
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

void reply_get(int sock, const struct cmplx_cmd &cmd,
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

    auto f = std::async(std::launch::async, [&cmd, sock]() {
        struct cmplx_cmd reply;
        reply.cmd = CONNECT_ME;
        reply.data = cmd.data;
        int new_socket = socket(AF_INET, SOCK_DGRAM, 0);
        active_sockets.insert(new_socket);

        struct sockaddr_in local_address;
        local_address.sin_family = AF_INET;
        local_address.sin_addr.s_addr = htonl(INADDR_ANY);
        local_address.sin_port = htons(0);
        if (bind(new_socket, (struct sockaddr *)&local_address,
                 sizeof(local_address)) < 0) {
            close(new_socket);
            active_sockets.erase(new_socket);
            throw std::logic_error("Faled to bind new socket");
        }
        reply.param = local_address.sin_port;
        std::cout << "Waiting for new connection on port " << local_address.sin_port << "\n";
        send_cmd(reply, sock);

        // switch to listening (passive open)
        if (listen(new_socket, 1) < 0) {
            close(new_socket);
            active_sockets.erase(new_socket);
            throw std::logic_error("Faled to switch to listening on a new socket");
        }

        struct timeval tv;
        tv.tv_sec = timeout;
        tv.tv_usec = 0;
        // TODO(I) zrobić, to co jest w scenariuszu
        if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (void *)&tv,
                       sizeof(tv)) < 0) {
            close(new_socket);
            active_sockets.erase(new_socket);
            throw std::logic_error("Failed to set writing timeout");
        }
        return true;
    });
    active_futures.push_back(std::move(f));
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
        sock.sock = connect_to_mcast(mcast_addr, cmd_port);
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
    // TODO potencjalnie chcę to robić pollem i nie mieć wątków
    signal(SIGINT, interrupt_handler);
    signal(SIGPIPE, SIG_IGN);

    init(argc, argv);

    while (true) {
        struct cmplx_cmd cmd;
        try {
            recv_cmd(cmd, sock.sock);
        }
        catch (std::runtime_error &e) {
            char address[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, (void *)(&cmd.addr.sin_addr), address,
                          sizeof(address)) == NULL) {
                throw std::logic_error("this should not be happening");
            }
            std::cerr << "[PCKG ERROR] Skipping invalid package from "
                      << address << ":" << cmd.addr.sin_port << ". ("
                      << e.what() << ")\n";
        }
        catch (std::exception &e) {
            std::cerr << "Error occured: " << e.what() << "\n";
        }

        if (cmd.cmd == HELLO) {
            try {
                reply_hello(sock.sock, cmd);
            }
            catch (std::exception &e) {
                std::cerr << "Error occured: " << e.what() << "\n";
            }
        }
        else if (cmd.cmd == LIST) {
            try {
                reply_list(sock.sock, cmd, files);
            }
            catch (std::exception &e) {
                std::cerr << "Error occured: " << e.what() << "\n";
            }
        }
        else if (cmd.cmd == DEL) {
            try {
                handle_del(cmd, files);
            }
            catch (std::exception &e) {
                std::cerr << "Error occured: " << e.what() << "\n";
            }
        }
        else {
            char address[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, (void *)(&cmd.addr.sin_addr), address,
                          sizeof(address)) == NULL) {
                throw std::logic_error("this should not be happening");
            }
            std::cerr << "[PCKG ERROR] Skipping invalid package from "
                      << address << ":" << cmd.addr.sin_port << ". (Command "
                      << cmd.cmd << " is unknown)\n";
        }
    }
}
