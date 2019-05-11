#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <iostream>
#include <string>

#include "helper.h"

const int32_t MAX_SPACE_DEFAULT = 52428800;

std::string mcast_addr, shrd_fldr;
int32_t cmd_port, timeout = TIMEOUT_DEFAULT, max_space = MAX_SPACE_DEFAULT;

// TODO(lab) czy maksymalna liczba bajtów udostępnianej przestrzeni dyskowej zmieści się w 31 bitach (int32_t)?

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

int main(int argc, char** argv) {
    namespace po = boost::program_options;
    namespace fs = boost::filesystem;

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
        parse_args(argc, argv, desc);
        std::vector<std::string> files = list_files();
        for (const auto& file : files) {
            std::cout << file << "\n";
        }
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
}

