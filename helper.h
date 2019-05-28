#include <iostream>

#include <arpa/inet.h>
#include <string>
#include <unistd.h>
const int32_t TIMEOUT_DEFAULT = 5;
const int32_t TIMEOUT_MAX = 300;
const int32_t PORT_MAX = 65535;
const int32_t DATA_MAX = 558;
const uint32_t CMD_SIZE = 10;

const std::string HELLO = std::string("HELLO\0\0\0\0\0\0", CMD_SIZE + 1);
const std::string GOOD_DAY = std::string("GOOD_DAY\0\0\0", CMD_SIZE + 1);
const std::string LIST = std::string("LIST\0\0\0\0\0\0\0", CMD_SIZE + 1);
const std::string MY_LIST = std::string("MY_LIST\0\0\0\0", CMD_SIZE + 1);
const std::string GET = std::string("GET\0\0\0\0\0\0\0\0", CMD_SIZE + 1);
const std::string CONNECT_ME = std::string("CONNECT_ME\0", CMD_SIZE + 1);
const std::string DEL = std::string("DEL\0\0\0\0\0\0\0\0", CMD_SIZE + 1);
const std::string ADD = std::string("ADD\0\0\0\0\0\0\0\0", CMD_SIZE + 1);
const std::string NO_WAY = std::string("NO_WAY\0\0\0\0\0", CMD_SIZE + 1);
const std::string CAN_ADD = std::string("CAN_ADD\0\0\0\0", CMD_SIZE + 1);

class Socket {
  public:
    int sock;
    Socket() : sock(0) {
    }
    Socket(int sock) : sock(sock) {
    }

    ~Socket() {
        if (sock > 0) {
            close(sock);
        }
    }
};

class ReceiveTimeOutException : public std::exception {
  private:
    static const std::string what_;
    virtual const char *what() const noexcept {
        return what_.c_str();
    }
};

class simpl_cmd {
  public:
    std::string cmd;
    uint64_t cmd_seq;
    std::string data;

    // used to determine who to send to when sending
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
void send_cmd(const simpl_cmd &cmd, int sock);
void send_cmd(const cmplx_cmd &cmd, int sock);

// cmd is the result
// if the command received was simpl_cmd, then param is equal to 0
void recv_cmd(cmplx_cmd &cmd, int sock);

int64_t get_cmd_seq();
