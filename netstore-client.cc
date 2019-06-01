#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/program_options.hpp>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/signalfd.h>
#include <sys/stat.h>

#include "helper.h"

const std::string DISCOVER = "discover";
const std::string SEARCH = "search";
const std::string FETCH = "fetch";
const std::string UPLOAD = "upload";
const std::string REMOVE = "remove";
const std::string EXIT = "exit";

std::string mcast_addr, out_fldr;
int32_t cmd_port, timeout = TIMEOUT_DEFAULT;

// main udp socket used for most of communications
int main_socket;
// 0 is signalfd
// 1 is main socket (or -1 if we're not listening on it)
// 2 is stdin
std::vector<struct pollfd> fds;
std::vector<ConnectionInfo> connections;
std::vector<std::pair<struct sockaddr_in, std::vector<std::string>>> files;
std::map<uint64_t, ConnectionInfo> seq_to_conn;
std::map<uint64_t, std::pair<std::vector<std::tuple<struct sockaddr_in,
                                                    std::string, uint64_t>>,
                             cmplx_cmd>>
    seq_to_servers;

std::vector<std::tuple<struct sockaddr_in, std::string, uint64_t>> servers;

// only ADD commands go here, so when they time out, because timeout after
// sending ADD to one server is equivalent to receiving NO_WAY from that server
// and should be handled accordingly
std::map<uint64_t, boost::posix_time::ptime> seq_to_starttime;

// list of files to send after pseudo-discover finishes
std::vector<std::string> files_to_upload;
// start time of last discover or not_a_datetime if no discover is running
boost::posix_time::ptime discover_start;
// output of pseudo-discover
std::vector<std::tuple<struct sockaddr_in, std::string, uint64_t>>
    discovered_servers;
// cmd_seq of pseudo_discover
uint64_t discover_seq;

void free_memory() {
    close(fds[0].fd);
    close(main_socket);
    for (const auto &conn : connections) {
        close(conn.fd);
        close(conn.sock_fd);
    }

    // free the memory
    fds.clear();
    connections.clear();
    files.clear();
}

void remove_connection(int i) {
    close(connections[i - 3].fd);
    close(connections[i - 3].sock_fd);
    connections.erase(connections.begin() + i - 3);
    fds.erase(fds.begin() + i);
}

void handle_interrupt() {
    std::cerr << "Received SIGINT, exiting\n";

    free_memory();

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

// returns servers, where first is sockaddr, and second is mcast_addr and third
// is their free space
std::vector<std::tuple<struct sockaddr_in, std::string, uint64_t>>
discover(int sock, const struct sockaddr_in &remote_address) {
    std::vector<std::tuple<struct sockaddr_in, std::string, uint64_t>> result;
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

            //                        std::cerr << "Received reply from " <<
            //                        address << ":"
            //                                  << ntohs(reply.addr.sin_port)
            //                                  << " with cmd = " << reply.cmd
            //                                  << " and seq = " <<
            //                                  reply.cmd_seq
            //                                  << " (original seq was " <<
            //                                  cmd.cmd_seq <<
            //                                  "). Data is \""
            //                                  << reply.data << "\"(" <<
            //                                  reply.data.size()
            //                                  << ") and param is " <<
            //                                  reply.param <<
            //                                  "\n";

            if (reply.cmd_seq != cmd.cmd_seq) {
                std::cerr << "[PCKG ERROR]  Skipping invalid package from "
                          << address << ":" << ntohs(reply.addr.sin_port)
                          << " (invalid cmd_seq)\n";
                continue;
            }
            if (reply.cmd != GOOD_DAY) {
                std::cerr << "[PCKG ERROR]  Skipping invalid package from "
                          << address << ":" << reply.addr.sin_port
                          << "(command is not GOOD_DAY when it should)\n";
                continue;
            }
            result.emplace_back(reply.addr, reply.data, reply.param);
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

void fetch(int sock, const struct sockaddr_in &remote_address,
           const std::string &filename) {
    simpl_cmd cmd;
    cmd.cmd = GET;
    cmd.cmd_seq = get_cmd_seq();
    cmd.addr = remote_address;
    cmd.data = filename;
    int fd = open(std::string(out_fldr + "/" + filename).c_str(),
                  O_WRONLY | O_CREAT, 0660);
    if (fd < 0) {
        int e = errno;
        close(fd);
        throw std::logic_error(
            std::string("Failed to open requested file for writing ") +
            strerror(e));
    }
    seq_to_conn[cmd.cmd_seq] =
        ConnectionInfo(boost::posix_time::microsec_clock::local_time(),
                       main_socket, fd, filename, false, true, "", 0);
    send_cmd(cmd, sock);
}

auto handle_no_way(uint64_t seq) {
    auto servers = seq_to_servers.find(seq)->second;
    cmplx_cmd cmd = servers.second;
    auto conn_it = seq_to_conn.find(seq);
    ConnectionInfo info = conn_it->second;
    seq_to_conn.erase(conn_it);
    seq_to_servers.erase(seq);
    auto ret = seq_to_starttime.erase(seq_to_starttime.find(seq));

    if (servers.first.empty()) {
        std::cout << "File " << cmd.data << " too big\n";
        return ret;
    }
    cmd.addr = std::get<0>(servers.first[servers.first.size() - 1]);
    cmd.cmd_seq = get_cmd_seq();
    servers.first.pop_back();
    send_cmd(cmd, info.sock_fd);
    auto now = boost::posix_time::microsec_clock::local_time();
    seq_to_conn[cmd.cmd_seq] = info;
    seq_to_servers[cmd.cmd_seq] = std::move(servers);
    seq_to_starttime[cmd.cmd_seq] = std::move(now);
    return ret;
}

void upload(
    int sock,
    std::vector<std::tuple<struct sockaddr_in, std::string, uint64_t>> servers,
    const std::string &filename) {

    cmplx_cmd cmd;
    cmd.cmd = ADD;
    cmd.cmd_seq = get_cmd_seq();
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cout << "File " << filename << " does not exist\n";
        return;
    }
    struct stat statbuf;
    if (fstat(fd, &statbuf) < 0) {
        close(fd);
        throw std::logic_error("Failed to get size of file");
    }
    cmd.param = statbuf.st_size;
    cmd.data = get_name_from_path(filename);
    auto now = boost::posix_time::microsec_clock::local_time();
    seq_to_conn[cmd.cmd_seq] =
        ConnectionInfo(now, sock, fd, filename, false, false, "", 0);
    seq_to_servers[cmd.cmd_seq] = std::make_pair(servers, cmd);
    seq_to_starttime[cmd.cmd_seq] = now;
    handle_no_way(cmd.cmd_seq);
}

void handle_server_answer() {
    cmplx_cmd cmd;
    recv_cmd(cmd, main_socket);
    char address[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, (void *)(&cmd.addr.sin_addr), address,
                  sizeof(address)) == NULL) {
        throw std::logic_error("inet_ntop failed unexpectedly");
    }
    if (discover_seq == cmd.cmd_seq) {
        if (cmd.cmd == GOOD_DAY) {
            // this is an answer to pre-run discover
            discovered_servers.emplace_back(cmd.addr, cmd.data, cmd.param);
            return;
        }
    }
    if (seq_to_conn.find(cmd.cmd_seq) != seq_to_conn.end()) {
        ConnectionInfo &info = seq_to_conn[cmd.cmd_seq];
        if (info.writing == true) {
            if (cmd.cmd == CONNECT_ME && cmd.data == info.filename) {
                // correct arguments
                int new_socket =
                    socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
                if (new_socket < 0) {
                    throw std::logic_error("creating new tcp socket failed");
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
                struct sockaddr_in remote_address = cmd.addr;
                remote_address.sin_port = htons(cmd.param);
                connect(new_socket, (struct sockaddr *)(&remote_address),
                        sizeof(remote_address));

                fds.push_back({new_socket, POLLIN, 0});
                connections.emplace_back(
                    boost::posix_time::microsec_clock::local_time(),
                    new_socket, info.fd, cmd.data, true, info.writing, address,
                    cmd.param);
                seq_to_conn.erase(cmd.cmd_seq);
                return;
            }
        }
        else {
            if (cmd.cmd == CAN_ADD && cmd.data.empty()) {
                int new_socket =
                    socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
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
                remote_address.sin_port = htons(cmd.param);
                connect(new_socket, (struct sockaddr *)(&remote_address),
                        sizeof(remote_address));
                fds.push_back({new_socket, POLLOUT, 0});
                connections.emplace_back(
                    boost::posix_time::microsec_clock::local_time(),
                    new_socket, info.fd, info.filename, true, info.writing,
                    address, cmd.param);
                seq_to_conn.erase(cmd.cmd_seq);
                seq_to_starttime.erase(cmd.cmd_seq);
                seq_to_servers.erase(cmd.cmd_seq);
                return;
            }
            else if (cmd.cmd == NO_WAY &&
                     cmd.data == get_name_from_path(info.filename)) {
                handle_no_way(cmd.cmd_seq);
                return;
            }
        }
    }
    std::cerr << "[PCKG ERROR]  Skipping invalid package from " << address
              << ":" << ntohs(cmd.addr.sin_port) << ".\n";
    return;
}

void pseudo_discover(int sock, const std::string &filename) {
    // check whether file exists
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cout << "File " << filename << " does not exist\n";
        return;
    }
    close(fd);
    if (discover_start.is_not_a_date_time()) {
        simpl_cmd cmd;
        cmd.cmd = HELLO;
        cmd.cmd_seq = get_cmd_seq();
        cmd.addr = get_remote_address(mcast_addr, cmd_port);
        send_cmd(cmd, sock);
        discover_start = boost::posix_time::microsec_clock::local_time();
        discover_seq = cmd.cmd_seq;
    }
    files_to_upload.push_back(filename);
}

void handle_user_input(const struct sockaddr_in &remote_address) {
    std::string line;
    std::getline(std::cin, line);

    // Assume discover cannot have whitespace after it
    if (boost::iequals(line, DISCOVER)) {
        servers = discover(main_socket, remote_address);

        char address[INET_ADDRSTRLEN];
        for (const auto &server_info : servers) {
            if (inet_ntop(AF_INET,
                          (void *)(&std::get<0>(server_info).sin_addr),
                          address, sizeof(address)) == NULL) {
                throw std::logic_error("inet_ntop failed unexpectedly");
            }

            std::cout << "Found " << address << " ("
                      << std::get<1>(server_info) << ") with free space "
                      << std::get<2>(server_info) << "\n";
        }
    }
    else if (boost::iequals(line.substr(0, SEARCH.size()), SEARCH)) {
        std::string needle;
        if (line.size() > SEARCH.size()) {
            if (line[SEARCH.size()] != ' ') {
                return;
            }
            needle = line.substr(SEARCH.size() + 1, line.size());
        }
        files = search(main_socket, remote_address, needle);
        for (const auto &package : files) {
            char address[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, (void *)(&package.first.sin_addr), address,
                          sizeof(address)) == NULL) {
                throw std::logic_error("inet_ntop failed unexpectedly");
            }
            for (const auto &file : package.second) {
                std::cout << file << " (" << address << ")\n";
            }
        }
    }
    else if (boost::iequals(line.substr(0, FETCH.size()), FETCH) &&
             line.size() >= FETCH.size() + 2 && line[FETCH.size()] == ' ') {
        std::string needle = line.substr(FETCH.size() + 1, line.size());
        for (const auto &package : files) {
            for (const auto &file : package.second) {
                if (file == needle) {
                    fetch(main_socket, package.first, file);
                    return;
                }
            }
        }
        std::cout << "Requested file is not in recently searched\n";
    }
    else if (boost::iequals(line.substr(0, UPLOAD.size()), UPLOAD) &&
             line.size() >= UPLOAD.size() + 2 && line[UPLOAD.size()] == ' ') {
        std::string needle = line.substr(UPLOAD.size() + 1, line.size());
        pseudo_discover(main_socket, needle);
    }
    else if (boost::iequals(line.substr(0, REMOVE.size()), REMOVE) &&
             line.size() >= REMOVE.size() + 2 && line[REMOVE.size()] == ' ') {
        std::string needle = line.substr(REMOVE.size() + 1, line.size());
        remove(main_socket, remote_address, needle);
    }
    else if (boost::iequals(line, EXIT)) {
        free_memory();
        exit(0);
    }
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

        fds.push_back({main_socket, POLLIN, 0});

        fds.push_back({STDIN_FILENO, POLLIN, 0});
    }
    catch (po::error &e) {
        std::cerr << "INCORRECT USAGE\n" << e.what() << "\n" << desc;
        exit(-1);
    }
}

void write_to_fd(int i) {
    ConnectionInfo &info = connections[i - 3];
    int len;
    if (info.position == info.buf_size) {
        info.buf_size = read(info.fd, info.buffer, sizeof(info.buffer));
        if (info.buf_size < 0) {
            std::cout << "File " << info.filename << " uploading failed ("
                      << info.ip << ":" << info.port
                      << ") Read from disk failed with \"" << strerror(errno)
                      << "\" error\n";
            remove_connection(i);
            return;
        }
        if (info.buf_size == 0) {
            std::cout << "File " << info.filename << " uploaded (" << info.ip
                      << ":" << info.port << ")\n";
            remove_connection(i);
            return;
        }
        info.position = 0;
    }
    len = write(info.sock_fd, info.buffer + info.position,
                info.buf_size - info.position);
    if (len < 0) {
        std::cout << "File " << info.filename << " uploading failed ("
                  << info.ip << ":" << info.port
                  << ") Write to socket failed with \"" << strerror(errno)
                  << "\" error\n";
        remove_connection(i);
        return;
    }
    info.position += len;
}

void read_from_fd(int i) {
    ConnectionInfo &info = connections[i - 3];
    int len;
    len = read(info.sock_fd, info.buffer, sizeof(info.buffer));
    if (len < 0) {
        std::cout << "File " << info.filename << " downloading failed ("
                  << info.ip << ":" << info.port
                  << ") Read from socket failed with \"" << strerror(errno)
                  << "\" error\n";
        remove_connection(i);
        return;
    }
    if (len == 0) {
        std::cout << "File " << info.filename << " downloaded (" << info.ip
                  << ":" << info.port << ")\n";
        remove_connection(i);
        return;
    }
    len = write(info.fd, info.buffer, len);
    if (len < 0) {
        std::cout << "File " << info.filename << " downloading failed ("
                  << info.ip << ":" << info.port
                  << ") Write to disk failed with \"" << strerror(errno)
                  << "\" error\n";
        remove_connection(i);
        return;
    }
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    init(argc, argv);

    struct sockaddr_in remote_address =
        get_remote_address(mcast_addr, cmd_port);

    int timeout_millis =
        compute_timeout(connections, seq_to_starttime, timeout);
    while (true) {
        int ready = poll(fds.data(), fds.size(), timeout_millis);
        auto now = boost::posix_time::microsec_clock::local_time();
        for (size_t i = fds.size() - 1; i >= 3; --i) {
            auto info = connections[i - 3];
            auto duration = now - connections[i - 3].start;
            if (duration.total_milliseconds() / 1000 > timeout) {
                remove_connection(i);
            }
        }
        for (auto start = seq_to_starttime.begin();
             start != seq_to_starttime.end();) {
            auto duration = now - start->second;
            if (duration.total_milliseconds() / 1000 > timeout) {
                // this invalidates iterator of the loop, so we need to
                // manually handle iterators
                start = handle_no_way(start->first);
            }
            else {
                start++;
            }
        }
        if (!discover_start.is_not_a_date_time()) {
            auto duration = now - discover_start;
            if (duration.total_milliseconds() / 1000 > timeout) {
                std::sort(discovered_servers.begin(), discovered_servers.end(),
                          [](const std::tuple<struct sockaddr_in, std::string,
                                              uint64_t> &a,
                             const std::tuple<struct sockaddr_in, std::string,
                                              uint64_t> &b) {
                              return std::get<2>(a) < std::get<2>(b);
                          });

                for (const auto &filename : files_to_upload) {
                    upload(main_socket, discovered_servers, filename);
                }
                files_to_upload.clear();
                discover_start = boost::posix_time::ptime();
                discovered_servers.clear();
            }
        }
        if (ready <= 0) {
            // timeout
            continue;
        }
        try {
            if (fds[0].revents & POLLIN) {
                handle_interrupt();
            }
            if (fds[1].revents & POLLIN) {
                handle_server_answer();
            }
            if (fds[2].revents & POLLIN) {
                handle_user_input(remote_address);
            }
        }
        catch (std::exception &e) {
            std::cerr << "Error occured: " << e.what() << "\n";
        }
        for (size_t i = 3; i < fds.size(); ++i) {
            try {
                if (fds[i].revents & POLLIN) {
                    connections[i - 3].start = now;
                    fds[i].revents = 0;
                    read_from_fd(i);
                }
                if (fds[i].revents & POLLOUT) {
                    connections[i - 3].start = now;
                    fds[i].revents = 0;
                    write_to_fd(i);
                }
            }
            catch (std::exception &e) {
                std::cerr << "Error occured: " << e.what() << "\n";
            }
        }
        timeout_millis =
            compute_timeout(connections, seq_to_starttime, timeout);
        if (!discover_start.is_not_a_date_time()) {
            auto duration = now - discover_start;
            int new_timeout = timeout * 1000 - duration.total_milliseconds();
            if (timeout_millis == -1 || timeout_millis < new_timeout) {
                timeout_millis = new_timeout;
            }
        }
    }
}
