#include <boost/program_options.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/algorithm/string.hpp>
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

void send_packets(int sock, const struct sockaddr_in& remote_address) {
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

// returns servers, where first is sockaddr, and second is their free space
std::vector<std::pair<struct sockaddr_in, int64_t>> discover(
        int sock,
        const struct sockaddr_in& remote_address) {
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

        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (void *)&tval, sizeof(tval)) < 0) {
	        throw std::logic_error("setsockopt " + std::to_string(errno) + " " + strerror(errno));
        }
        try {
            recv_cmd(reply, sock);
            char address[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, (void*)(&reply.addr.sin_addr), address,
                    sizeof(address)) == NULL) {
                throw std::logic_error("this should not be happening");
            }

            if (reply.cmd_seq != cmd.cmd_seq) {
                std::cerr <<  "[PCKG ERROR]  Skipping invalid package from "
                    << address << ":" << reply.addr.sin_port
                    << "(invalid cmd_seq)\n";
                continue;
            }
            if (reply.cmd != GOOD_DAY) {
                std::cerr <<  "[PCKG ERROR]  Skipping invalid package from "
                    << address << ":" << reply.addr.sin_port
                    << "(command is not GOOD_DAY when it should)\n";
                continue;
            }
            result.emplace_back(reply.addr, reply.param);
            std::cout << "Found " << address << " (" << reply.data
                << ") with free space " << reply.param <<"\n";
        }
        catch (ReceiveTimeOutException& e) {
            break;
        }
    } while (true);
    return result;
}

std::vector<std::string> search(int sock,
        const struct sockaddr_in& remote_address,
        const std::string& needle) {
    std::vector<std::string> result;
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

        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (void *)&tval, sizeof(tval)) < 0) {
	        throw std::logic_error("setsockopt " + std::to_string(errno) + " " + strerror(errno));
        }
        try {
            recv_cmd(reply, sock);
            char address[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, (void*)(&reply.addr.sin_addr), address,
                    sizeof(address)) == NULL) {
                throw std::logic_error("this should not be happening");
            }

            if (reply.cmd_seq != cmd.cmd_seq) {
                std::cerr <<  "[PCKG ERROR]  Skipping invalid package from "
                    << address << ":" << reply.addr.sin_port
                    << "(invalid cmd_seq)\n";
                continue;
            }
            if (reply.cmd != MY_LIST) {
                std::cerr <<  "[PCKG ERROR]  Skipping invalid package from "
                    << address << ":" << reply.addr.sin_port
                    << "(command is not MY_LIST when it should)\n";
                continue;
            }
            std::vector<std::string> tmp;
            boost::split(tmp, reply.data, [](char c) { return c == '\n';});
            result.insert(result.end(), tmp.begin(), tmp.end());
        }
        catch (ReceiveTimeOutException& e) {
            break;
        }
    } while (true);
    return result;
}

void remove(int sock, const struct sockaddr_in& remote_address,
        const std::string& filename) {
    struct simpl_cmd cmd;
    cmd.cmd = DEL;
    cmd.cmd_seq = get_cmd_seq();
    cmd.addr = remote_address;
    cmd.data = filename;
    send_cmd(cmd, sock);
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

        discover(sock.sock, remote_address);
        const auto files = search(sock.sock, remote_address, "");
        for (const auto& file : files) {
            std::cout << file << "\n";
        }
        remove(sock.sock, remote_address, "helper.o");
    }
    catch (po::error& e) {
        std::cerr << "INCORRECT USAGE\n" << e.what() << "\n" << desc;
        return -1;
    }
}

