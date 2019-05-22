#include "helper.h"

#include <cstring>
#include <endian.h>
#include <errno.h>
#include <stdexcept>

const int32_t BUFFER_SIZE = 65535;
const uint32_t CMD_SIZE = 10;

const std::string ReceiveTimeOutException::what_ = "Timeout while reading";

void send_cmd(const simpl_cmd &cmd, int sock) {
    size_t size = CMD_SIZE + sizeof(cmd.cmd_seq) + cmd.data.size() + 1;
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
    return cmd == GOOD_DAY || cmd == CONNECT_ME || cmd == CAN_ADD;
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
        throw std::runtime_error("packet too small");
    }
    char temp_buffer[CMD_SIZE + 1];
    memset(temp_buffer, '\0', sizeof(temp_buffer));
    memcpy(temp_buffer, buffer, CMD_SIZE);
    cmd.cmd = std::string(temp_buffer);
    int64_t tmp = 0;
    if (uint64_t(rcv_len) < CMD_SIZE + sizeof(cmd.cmd_seq)) {
        throw std::runtime_error("packet too small");
    }
    memcpy(&tmp, buffer + CMD_SIZE, sizeof(tmp));
    cmd.cmd_seq = be64toh(tmp);
    if (is_complex(cmd.cmd)) {
        if (uint64_t(rcv_len) <
            CMD_SIZE + sizeof(cmd.cmd_seq) + sizeof(cmd.param)) {
            throw std::runtime_error("packet too small");
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

int64_t get_cmd_seq() {
    static int num = 0;
    return ++num;
}
