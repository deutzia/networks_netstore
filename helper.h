#include <unistd.h>
const int32_t TIMEOUT_DEFAULT = 5;
const int32_t TIMEOUT_MAX = 300;
const int32_t PORT_MAX = 65535;

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

struct simpl_cmd {
    char cmd[10];
    uint64_t cmd_seq;
    char* data;
};

struct cmplx_cmd {
    char cmd[10];
    uint64_t cmd_seq;
    uint64_t param;
    char* data;
};

void send_cmd(simpl_cmd, int sock);
void send_cmd(cmplx_cmd, int sock);
void recv_cmd(simpl_cmd, int sock);
void recv_cmd(cmplx_cmd, int sock);

