#include "helper.h"

#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>
#include <endian.h>
#include <errno.h>
#include <random>
#include <stdexcept>
#include <string.h>

const std::string ReceiveTimeOutException::what_ = "Timeout while reading";

ConnectionInfo::ConnectionInfo(const boost::posix_time::ptime &start_,
                               int sock_fd_, int fd_,
                               const std::string &filename_,
                               bool was_accepted_, bool writing_,
                               const std::string &ip_, uint16_t port_) {
    start = start_;
    sock_fd = sock_fd_;
    fd = fd_;
    filename = filename_;
    was_accepted = was_accepted_;
    writing = writing_;
    memset(buffer, '\0', BUFFER_SIZE);
    position = 0;
    buf_size = 0;
    ip = ip_;
    port = port_;
}

ConnectionInfo::ConnectionInfo() {
    sock_fd = 0;
    fd = 0;
    was_accepted = false;
    writing = false;
    memset(buffer, '\0', BUFFER_SIZE);
    position = 0;
    buf_size = 0;
    port = 0;
}

void send_cmd(const simpl_cmd &cmd, int sock) {
    size_t size = CMD_SIZE + sizeof(cmd.cmd_seq) + cmd.data.size();
    char buffer[size];
    memset(buffer, '\0', sizeof(buffer));
    strcpy(buffer, cmd.cmd.c_str());
    int64_t seq = htobe64(cmd.cmd_seq);
    memcpy(buffer + CMD_SIZE, &seq, sizeof(seq));
    memcpy(buffer + CMD_SIZE + sizeof(cmd.cmd_seq), cmd.data.c_str(),
           cmd.data.size());

    if (sendto(sock, buffer, size, 0, (const sockaddr *)(&cmd.addr),
               sizeof(cmd.addr)) < 0) {
        throw std::logic_error("Failed to send" + std::to_string(errno) + " " +
                               strerror(errno));
    }
}

void send_cmd(const cmplx_cmd &cmd, int sock) {
    size_t size =
        CMD_SIZE + sizeof(cmd.cmd_seq) + sizeof(cmd.param) + cmd.data.size();
    char buffer[size];
    memset(buffer, '\0', sizeof(buffer));
    strcpy(buffer, cmd.cmd.c_str());
    int64_t tmp = htobe64(cmd.cmd_seq);
    memcpy(buffer + CMD_SIZE, &tmp, sizeof(tmp));
    tmp = htobe64(cmd.param);
    memcpy(buffer + CMD_SIZE + sizeof(cmd.cmd_seq), &tmp, sizeof(tmp));
    memcpy(buffer + CMD_SIZE + sizeof(cmd.cmd_seq) + sizeof(cmd.param),
           cmd.data.c_str(), cmd.data.size());

    if (sendto(sock, buffer, size, 0, (const sockaddr *)(&cmd.addr),
               sizeof(cmd.addr)) < 0) {
        throw std::logic_error("Failed to send");
    }
}

// determine whether command cmd is a complex command
bool is_complex(const std::string &cmd) {
    return cmd == ADD || cmd == GOOD_DAY || cmd == CONNECT_ME ||
           cmd == CAN_ADD;
}

// cmd is the result
void recv_cmd(cmplx_cmd &cmd, int sock) {
    char buffer[BUFFER_SIZE];
    memset(buffer, '\0', BUFFER_SIZE);
    ssize_t rcv_len;
    socklen_t addrlen = sizeof(cmd.addr);
    if ((rcv_len = recvfrom(sock, buffer, BUFFER_SIZE, 0,
                            (struct sockaddr *)(&cmd.addr), &addrlen)) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            throw ReceiveTimeOutException();
        }
        throw std::logic_error("Failed to receive");
    }
    if (addrlen > sizeof(cmd.addr)) {
        throw std::logic_error("Something went terribly wrong");
    }

    if (rcv_len < CMD_SIZE) {
        throw std::runtime_error("Packet too small.");
    }
    char temp_buffer[CMD_SIZE + 1];
    memset(temp_buffer, '\0', sizeof(temp_buffer));
    memcpy(temp_buffer, buffer, CMD_SIZE);
    cmd.cmd = std::string(temp_buffer, CMD_SIZE + 1);
    int64_t tmp = 0;
    if (uint64_t(rcv_len) < CMD_SIZE + sizeof(cmd.cmd_seq)) {
        throw std::runtime_error("Packet too small.");
    }
    memcpy(&tmp, buffer + CMD_SIZE, sizeof(tmp));
    cmd.cmd_seq = be64toh(tmp);
    if (is_complex(cmd.cmd)) {
        if (uint64_t(rcv_len) <
            CMD_SIZE + sizeof(cmd.cmd_seq) + sizeof(cmd.param)) {
            throw std::runtime_error("Packet too small.");
        }
        memcpy(&tmp, buffer + CMD_SIZE + sizeof(tmp), sizeof(tmp));
        cmd.param = be64toh(tmp);
        cmd.data = std::string(buffer + CMD_SIZE + sizeof(cmd.cmd_seq) +
                               sizeof(cmd.param));
    }
    else {
        cmd.param = 0;
        cmd.data = std::string(buffer + CMD_SIZE + sizeof(cmd.param));
    }
}

uint64_t get_cmd_seq() {
    static std::random_device rd;
    static std::mt19937_64 rng(rd());
    return rng();
}

int compute_timeout(const std::vector<ConnectionInfo> &connections,
                    const std::map<uint64_t, boost::posix_time::ptime> &starts,
                    int timeout) {
    auto now = boost::posix_time::microsec_clock::local_time();
    auto mini = now;
    for (const auto &info : connections) {
        mini = std::min(mini, info.start);
    }
    for (const auto &start : starts) {
        mini = std::min(mini, start.second);
    }
    if (mini == now) {
        return -1;
    }
    auto elapsed = now - mini;
    int elapsed_miliseconds = elapsed.total_microseconds() / 1000;
    int new_timeout = timeout * 1000 - elapsed_miliseconds;
    if (new_timeout < 0) {
        return 0;
    }
    return new_timeout;
}

std::string get_name_from_path(const std::string &path) {
    std::vector<std::string> tmp;
    boost::split(tmp, path, [](char c) { return c == '/'; });
    return tmp[tmp.size() - 1];
}

std::vector<std::pair<struct sockaddr_in, std::vector<std::string>>>
search(int sock, const struct sockaddr_in &remote_address,
       const std::string &needle, int32_t timeout) {
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
