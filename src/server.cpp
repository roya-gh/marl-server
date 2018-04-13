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
#include "definitions.hpp"
#include <flog/flog.hpp>
#include <iostream>
#include <sstream>
#include <marl-protocols/message-base.hpp>
#include <marl-protocols/message-base-impl.hpp>
#include <cereal/archives/binary.hpp>

using level = flog::level_t;

#define DBUFSIZE 8192
#define HEADSIZE 4

marl::server::server():
    m_host {"localhost"},
    m_port {DEFAULT_PORT} {
}

marl::server::server(const std::string& host, uint16_t port,
                     const std::string& problem_path, uint32_t agents):
    m_host {host},
    m_port {port},
    m_problem {problem_path},
    m_agent_count{agents} {
    flog::logger* l = flog::logger::instance();
    l->set_backend(flog::backend_t::STDERR);
    l->log(level::TRACE, "Creating communication server. Problem instance: `%s', Agent count: %d.",
           m_problem.c_str(), m_agent_count);
}

marl::server::~server() {
    stop();
}


void marl::server::start() {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Initializing server on %s:%d...", m_host.c_str(), m_port);
    m_server = cpnet_socket(SOCK_STREAM);
    if(!m_server) {
        l->log(level::ERROR_, "Unable to create socket. Terminating...");
        return;
    }
    if(cpnet_bind(m_server, m_host.c_str(), &m_port) != 0) {
        l->log(level::ERROR_, "Unable to bind socket on server. Terminating...");
        return;
    }
    l->log(level::TRACE, "Successfully bound on %s:%d.", m_host.c_str(), m_port);
    uint32_t agent_count = 0;
    while(m_is_running && agent_count < m_agent_count) {
        l->log(level::TRACE, "Listening for new connections...");
        cpnet_listen(m_server, 1024);
        char buffer[46];
        uint16_t port;
        socket_t new_socket = cpnet_accept(m_server, buffer, &port);
        if(! new_socket) {
            l->log(level::ERROR_, "Failed to accept new connection.");
            continue;
        } else {
            l->log(level::TRACE, "Incomming connection from %s. Bound port number: %d", buffer, port);
            agent_count++;
        }
        // TODO: Check problem instance and agent id before adding sockets to the maps
        std::unique_lock<std::mutex> lck(m_maps_lock);
        m_socket_map.insert(std::make_pair(agent_count, new_socket));
        lck.unlock();
        m_readers.emplace_front(&server::reader, this, agent_count);
        m_writers.emplace_front(&server::writer, this, agent_count);
    }
}

void marl::server::stop() {
    flog::logger* l = flog::logger::instance();
    l->flush(std::chrono::milliseconds{100});
}

void marl::server::reader(uint32_t agent_id) const {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Initiating reader thread for agent %d...", agent_id);
    std::unique_lock<std::mutex> lck(m_maps_lock);
    socket_t s = m_socket_map.at(agent_id);
    lck.unlock();
    while(m_is_running) {
        std::unique_ptr<message_base> msg = read_message(s);
    }
}

void marl::server::writer(uint32_t agent_id) const {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Initiating writer thread for agent %d...", agent_id);
    std::unique_lock<std::mutex> lck(m_maps_lock);
    // socket_t s = m_socket_map.at(agent_id);
    lck.unlock();
}

std::unique_ptr<marl::message_base> marl::server::read_message(socket_t socket) const {
    flog::logger* l = flog::logger::instance();
    char data_buffer[DBUFSIZE];
    char header_buffer[HEADSIZE];
    ssize_t read_size = cpnet_read2(socket, header_buffer, HEADSIZE, MSG_WAITALL);
    if(read_size != HEADSIZE) {
        l->log(level::ERROR_,
               "Unable to read header from incomming connection. Error: %s",
               cpnet_last_error());
        return nullptr;
    }
    uint32_t expected_buffer =
        (static_cast<uint32_t>(header_buffer[0]) << 8 * 3)
        | (static_cast<uint32_t>(header_buffer[1]) << 8 * 2)
        | (static_cast<uint32_t>(header_buffer[2]) << 8 * 1)
        | (static_cast<uint32_t>(header_buffer[3]) << 8 * 0);
    size_t total_read = 0;
    std::stringstream istr;
    while(total_read < expected_buffer) {
        read_size = cpnet_read(socket, data_buffer, DBUFSIZE);
        if(read_size < 0) {
            l->log(level::ERROR_,
                   "Unable to read data from incomming connection. Error: %s",
                   cpnet_last_error());
            return nullptr;
        }
        total_read += read_size;
        istr.write(data_buffer, read_size);
    }
    if(expected_buffer != total_read) {
        l->log(level::ERROR_,
               "Expected incomming buffer size was %z, bit only %z received.",
               expected_buffer, total_read);
        return nullptr;
    }
    // Process incomming message: construct buffers
    std::unique_ptr<message_base> msg;
    try {
        cereal::BinaryInputArchive archiver(istr);
        archiver(msg);
    } catch(cereal::Exception &e) {
        l->log(level::ERROR_,
               "Unable to deserialize buffer into `message_base' object. "
               "Error: %s", e.what());
        return nullptr;
    }
    return msg;
}
