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

#include <string>
#include <map>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <memory>
#include <forward_list>
#include <cpnet/cpnet-network.h>
#include <marl-protocols/response-base.hpp>
#include "blockingconcurrentqueue.h"

namespace marl {

using response_queue_t =
    moodycamel::BlockingConcurrentQueue<std::unique_ptr<marl::response_base>>;

class server {
public:
    server();
    server(const std::string& host, uint16_t port,
           const std::string& problem_path, uint32_t agents);
    ~server();
    void start();
    void stop();
    /**
       Thread callbacks for reading data from remote sockets.
    */
    void reader(uint32_t agent_id) const;
    /**
       Thread callbacks for writing data to remote sockets.
    */
    void writer(uint32_t agent_id) const;
    /**
      Helper functions
      */
    std::unique_ptr<message_base> read_message(socket_t socket) const;
private:
    std::string m_host;
    uint16_t m_port;
    socket_t m_server;
    std::string m_problem;
    uint32_t m_agent_count;
    /**
       ID to socket map
    */
    std::map<uint32_t, socket_t> m_socket_map;
    std::atomic_bool m_is_running;
    /**
       A mutex to protect all maps in the server.
     */
    mutable std::mutex m_maps_lock;
    /**
       List of all reader threads,
    */
    std::forward_list<std::thread> m_readers;
    std::forward_list<std::thread> m_writers;

    /**
       Map of (agent id, request number) pairs to a vector of responses. This will
       be used as a buffer to determine completed response vectors.
    */
    std::map<std::pair<uint32_t, uint32_t>, std::forward_list<marl::response_base*>> m_response_map;


    /**
       Map of agent id to a concurrent queue of outgoing
       messages. This will be used as a buffer to transmit accumulated
       responses to asking agents.
    */
    std::map<uint32_t, response_queue_t> m_response_queue_map;


};
}