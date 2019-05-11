#include <boost/program_options.hpp>
#include <iostream>
#include <string>

#include "helper.h"

const int32_t MAX_SPACE_DEFAULT = 52428800;

std::string mcast_addr, shrd_fldr;
int32_t cmd_port, timeout = TIMEOUT_DEFAULT, max_space = MAX_SPACE_DEFAULT;

// TODO(lab) czy maksymalna liczba bajtów udostępnianej przestrzeni dyskowej zmieści się w 31 bitach (int32_t)?

int main(int argc, char** argv) {
    namespace po = boost::program_options;
    po::options_description desc(argv[0] + std::string(" flags"));
    desc.add_options()
        (",g", po::value<std::string>(&mcast_addr)->required(), "MCAST_ADDR")
        (",p", po::value<int32_t>(&cmd_port)->required(),
            "CMD_PORT (range [1, 65535]")
        (",b", po::value<int32_t>(&max_space), "MAX_SPACE")
        (",f", po::value<std::string>(&shrd_fldr)->required(), "SHRD_FLDR")
        (",t", po::value<int32_t>(&timeout),
            "TIMEOUT (range [1, 300], default 5)");

    try {
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
    catch (po::error& e) {
        std::cerr << "INCORRECT USAGE\n" << e.what() << "\n" << desc;
        return -1;
    }
    std::cout << "mcast_addr = " << mcast_addr << "\n";
    std::cout << "timeout = " << timeout << "\n";
    std::cout << "cmd_port = " << cmd_port << "\n";
}

