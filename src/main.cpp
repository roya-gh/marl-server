/*
 * Copyright 2017 - Roya Ghasemzade <roya@ametisco.ir>
 * Copyright 2017 - Soroush Rabiei <soroush@ametisco.ir>
 *
 * This file is part of marl_server.
 *
 * marl_server is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * marl_server is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with marl_server.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "server.hpp"
#include <iostream>
#include <string>
#include <algorithm>
#include <bitset>
#include <cstdint>
#include <getopt.h>

static std::string usage_message =
    "Usage: marl-server [OPTION]...\n"
    "\n"
    "  -h ADDRESS, --host=ADDRESS\n"
    "                 Host name or IP address to start server on.\n"
    "  -p N, --port=N\n"
    "                 port number of server's main control connection (tcp).\n"
    "  -m PATH, --problem=PATH\n"
    "                 file name of the markov decision problem in mdpx format.\n"
    "  -n N, --agents=N\n"
    "                 number of agents to solve the problem.\n"
    ;

void print_usage() {
    std::cerr << usage_message;
}

int main(int argc, char* argv[]) {
    // Options:
    std::string host;
    uint16_t port = 0;
    std::string problem_path;
    uint32_t agents = 0;
    int c;
    std::bitset<4> args;
    while(true) {
        static struct option long_options[] = {
            {"host",     required_argument, 0, 'h'},
            {"port",     required_argument, 0, 'p'},
            {"problem",  required_argument, 0, 'm'},
            {"agents",   required_argument, 0, 'n'},
            {0, 0, 0, 0}
        };
        int option_index = 0;
        c = getopt_long(argc, argv, "h:p:m:n:", long_options, &option_index);
        if(c == -1) {
            break;
        }
        switch(c) {
            case 'h':
                host = std::string{optarg};
                args[0] = true;
                break;
            case 'p':
                port = std::stoi(std::string{optarg});
                args[1] = true;
                break;
            case 'm':
                problem_path = std::string{optarg};
                args[2] = true;
                break;
            case 'n':
                agents = std::stoi(std::string{optarg});
                args[3] = true;
                break;
            default:
                std::cerr << "Unknown argument!\n";
                abort();
        }
    }
    if(!args.all()) {
        print_usage();
        return -1;
    }
    marl::server s{host, port, problem_path, agents};
    s.start();
    return 0;
}
