#include <boost/program_options.hpp>
#include <iostream>
#include <string>

#include "helper.h"

std::string mcast_addr, out_fldr;
int32_t cmd_port, timeout = TIMEOUT_DEFAULT;

void parse_args(int argc, char** argv,
        const boost::program_options::options_description& desc) {
    namespace po = boost::program_options;
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (timeout <= 0 || timeout > TIMEOUT_MAX) {
        throw po::validation_error(
                po::validation_error::invalid_option_value,
                "t", std::to_string(timeout));
    }
    if (cmd_port <= 0 || cmd_port > PORT_MAX) {
        throw po::validation_error(
                po::validation_error::invalid_option_value,
                "p", std::to_string(cmd_port));
    }
}

struct sockaddr_in get_remote_address(const std::string& colon_address,
        in_port_t port) {
    struct sockaddr_in remote_address;
    remote_address.sin_family = AF_INET;
    if (inet_pton(AF_INET, colon_address.c_str(), &remote_address.sin_addr) < 0) {
        throw boost::program_options::validation_error(
                boost::program_options::validation_error::invalid_option_value,
                "g", colon_address);
    }
    remote_address.sin_port = htons(port);
    return remote_address;
}

void send_packets(int sock, struct sockaddr_in remote_address) {
    cmplx_cmd cmd;
    for (int i = 0; i < 10; ++i) {
        cmd.cmd = "CAN_ADD";
        cmd.cmd_seq = i;
        cmd.param = i * i;
        cmd.data = std::string(i * 3, char('a' + i));
        cmd.addr = remote_address;
        send_cmd(cmd, sock);
    }
}

int main(int argc, char** argv) {
    namespace po = boost::program_options;
    po::options_description desc(argv[0] + std::string(" flags"));
    desc.add_options()
        (",g", po::value<std::string>(&mcast_addr)->required(), "MCAST_ADDR")
        (",p", po::value<int32_t>(&cmd_port)->required(),
            "CMD_PORT (range [1, 65535]")
        (",o", po::value<std::string>(&out_fldr)->required(), "OUT_FLDR")
        (",t", po::value<int32_t>(&timeout), "TIMEOUT (range [1, 300])");

    try {
        parse_args(argc, argv, desc);
        struct sockaddr_in remote_address = get_remote_address(mcast_addr, cmd_port);

        Socket sock;
        sock.sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock.sock < 0) {
            throw std::logic_error("Failed to create a socket");
        }
        /* connecting to a local address and port */
        struct sockaddr_in local_address;
        local_address.sin_family = AF_INET;
        local_address.sin_addr.s_addr = htonl(INADDR_ANY);
        local_address.sin_port = htons(0);
        if (bind(sock.sock,
                (struct sockaddr *)&local_address,
                sizeof local_address) < 0) {
            throw std::logic_error("Failed to connect to a local address and port");
        }

        send_packets(sock.sock, remote_address);
    }
    catch (po::error& e) {
        std::cerr << "INCORRECT USAGE\n" << e.what() << "\n" << desc;
        return -1;
    }
    std::cout << "mcast_addr = " << mcast_addr << "\n";
    std::cout << "timeout = " << timeout << "\n";
    std::cout << "cmd_port = " << cmd_port << "\n";
}

