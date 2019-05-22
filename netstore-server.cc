#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <iostream>
#include <string>

#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>

#include "helper.h"

const int64_t MAX_SPACE_DEFAULT = 52428800;

std::string mcast_addr, shrd_fldr;
int32_t cmd_port, timeout = TIMEOUT_DEFAULT;
int64_t max_space = MAX_SPACE_DEFAULT;

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
    // TODO(lab) czy liczba bajtów może być 0?
    if (max_space <= 0) {
        throw po::validation_error(
                po::validation_error::invalid_option_value,
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

  // TODO(lab) czy moga byc dwa serwery na tej samej maszynie na tym samym porcie?
  /* connecting to a local address and port */
  struct sockaddr_in local_address;
  local_address.sin_family = AF_INET;
  local_address.sin_addr.s_addr = htonl(INADDR_ANY);
  local_address.sin_port = htons(port);
  if (bind(sock.sock,
          (struct sockaddr *)&local_address,
          sizeof local_address) < 0) {
    throw std::logic_error("Failed to connect to a local address and port");
  }

  /* connecting to multicast group */
  struct ip_mreq ip_mreq;
  ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  if (inet_aton(mcast_addr.c_str(), &ip_mreq.imr_multiaddr) == 0) {
      throw boost::program_options::validation_error(
              boost::program_options::validation_error::invalid_option_value,
              "g", mcast_addr);
  }

  if (setsockopt(sock.sock,
              IPPROTO_IP,
              IP_ADD_MEMBERSHIP,
              (void*)&ip_mreq,
              sizeof ip_mreq) < 0) {
      throw std::logic_error("Failed to connect to multicast group");
  }

  int result = sock.sock;
  sock.sock = 0;

  return result;
}

void read_packets(int sock) {
    std::cerr << "reading packets...\n";
    cmplx_cmd cmd;
    char address[INET_ADDRSTRLEN];
    while(true) {
        recv_cmd(cmd, sock);
        if (inet_ntop(AF_INET, (void*)(&cmd.addr.sin_addr), address, sizeof(address)) == NULL) {
            throw std::logic_error("this should not be happening");
        }
        std::cout << "Received (" << cmd.cmd << ", " << cmd.cmd_seq << ", " << cmd.param << ", " << cmd.data << ") from " << address << "\n";
    }
}

void reply_hello(int sock, const struct cmplx_cmd& cmd) {
    struct cmplx_cmd reply;
    reply.cmd = GOOD_DAY;
    reply.cmd_seq = cmd.cmd_seq;
    reply.param = max_space;
    reply.data = mcast_addr;
    reply.addr = cmd.addr;
    send_cmd(reply, sock);
}

void reply_list(int sock, const struct cmplx_cmd& cmd,
        std::vector<std::string> files) {
    struct simpl_cmd reply;
    reply.cmd = MY_LIST;
    reply.cmd_seq = cmd.cmd_seq;
    reply.addr = cmd.addr;
    // TODO(lab) jak dobrze ograniczac rozmiar wysylanego pakietu
    for (const auto& file : files) {
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

int main(int argc, char** argv) {
    namespace po = boost::program_options;
    namespace fs = boost::filesystem;

    po::options_description desc(argv[0] + std::string(" flags"));
    desc.add_options()
        (",g", po::value<std::string>(&mcast_addr)->required(), "MCAST_ADDR")
        (",p", po::value<int32_t>(&cmd_port)->required(),
            "CMD_PORT (range [1, 65535]")
        (",b", po::value<int64_t>(&max_space), "MAX_SPACE")
        (",f", po::value<std::string>(&shrd_fldr)->required(), "SHRD_FLDR")
        (",t", po::value<int32_t>(&timeout),
            "TIMEOUT (range [1, 300], default 5)");
    std::vector<std::string> files;
    Socket sock;

    try {
        parse_args(argc, argv, desc);
        files = list_files();
        for (const auto& file : files) {
            std::cout << file << "\n";
        }
        sock.sock = connect_to_mcast(mcast_addr, cmd_port);
    }

    catch (po::error& e) {
        std::cerr << "INCORRECT USAGE\n" << e.what() << "\n" << desc;
        return -1;
    }
    catch (fs::filesystem_error& e) {
        std::cerr << "INCORRECT USAGE\n" << e.what() << "\n" << desc;
        return -1;
    }
    catch (std::exception& e) {
        std::cerr << "ERROR\n" << e.what() << "\n" << desc;
        return -1;
    }
    while (true) {
        struct cmplx_cmd cmd;
        recv_cmd(cmd, sock.sock);
        if (cmd.cmd == HELLO) {
            reply_hello(sock.sock, cmd);
        }
        else if (cmd.cmd == LIST) {
            reply_list(sock.sock, cmd, files);
        }
        else {
            char address[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, (void*)(&cmd.addr.sin_addr), address,
                    sizeof(address)) == NULL) {
                throw std::logic_error("this should not be happening");
            }
            std::cerr << "[PCKG ERROR] Skipping invalid package from "
                << address <<":" << cmd.addr.sin_port << "(command "
                << cmd.cmd << " is unknown)\n";
        }
    }
}

