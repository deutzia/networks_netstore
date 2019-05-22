#include <iostream>

#include <string>
#include <arpa/inet.h>
#include <unistd.h>
const int32_t TIMEOUT_DEFAULT = 5;
const int32_t TIMEOUT_MAX = 300;
const int32_t PORT_MAX = 65535;

const std::string HELLO = "HELLO";
const std::string GOOD_DAY = "GOOD_DAY";
const std::string LIST = "LIST";
const std::string MY_LIST = "MY_LIST";
const std::string GET = "GET";
const std::string CONNECT_ME = "CONNECT_ME";
const std::string DEL = "DEL";
const std::string ADD = "ADD";
const std::string NO_WAY = "NO_WAY";
const std::string CAN_ADD = "CAN_ADD";

class Socket {
public:
    int sock;
    Socket()
    : sock(0) {}
    Socket(int sock)
    : sock(sock) {}

    ~Socket() {
        if (sock > 0) {
            close(sock);
        }
    }
};

class simpl_cmd {
public:
    std::string cmd;
    uint64_t cmd_seq;
    std::string data;

    // used to determine who to send to when sending, filled in when receiving
    struct sockaddr_in addr;
};

struct cmplx_cmd {
    std::string cmd;
    uint64_t cmd_seq;
    uint64_t param;
    std::string data;

    // used to determine who to send to when sending, filled in when receiving
    struct sockaddr_in addr;
};

// those functions do not split cmd, assume data fits in size of one udp packet
void send_cmd(const simpl_cmd& cmd, int sock);
void send_cmd(const cmplx_cmd& cmd, int sock);

// cmd is the result
// if the command received was simpl_cmd, then param is equal to 0
void recv_cmd(cmplx_cmd& cmd, int sock);

