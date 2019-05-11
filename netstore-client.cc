#include <boost/program_options.hpp>
#include <iostream>
#include <string>

// TODO(lab) czy "wartość maksymalna 300" oznacza, że 300 jest ok?
const int32_t TIMEOUT_DEFAULT = 5;
const int32_t TIMEOUT_MAX = 300;
const int32_t PORT_MAX = 65535;

// TODO(lab) jak mają się nazywać nasze binarki?
// TODO(lab) czy -okatalog jest równoważne -o katalog? Czy oba są poprawne?

std::string mcast_addr, out_fldr;
int32_t cmd_port, timeout = TIMEOUT_DEFAULT;

int main(int argc, char** argv) {
    namespace po = boost::program_options;
    po::options_description desc(argv[0] + std::string(" flags"));
    desc.add_options()
        (",g", po::value<std::string>(&mcast_addr)->required(), "MCAST_ADDR")
        (",p", po::value<int32_t>(&cmd_port)->required(), "CMD_PORT (range [1, 65535]")
        (",o", po::value<std::string>(&out_fldr)->required(), "OUT_FLDR")
        (",t", po::value<int32_t>(&timeout), "TIMEOUT (range [1, 300])");

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
    }
    catch (po::error& e) {
        std::cerr << "INCORRECT USAGE\n" << e.what() << "\n" << desc;
        return -1;
    }
    std::cout << "mcast_addr = " << mcast_addr << "\n";
    std::cout << "timeout = " << timeout << "\n";
    std::cout << "cmd_port = " << cmd_port << "\n";
}

