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
#include <arpa/inet.h>
#include <iostream>
#include <sstream>
#include <flog/flog.hpp>
#include <marl-protocols/message-base.hpp>
#include <marl-protocols/message-base-impl.hpp>
#include <marl-protocols/start-request-impl.hpp>
#include <cereal/archives/binary.hpp>

using level = flog::level_t;

#define DBUFSIZE 8192
#define HEADSIZE (sizeof(uint32_t))


template <typename T>
bool send_message(const T& msg, socket_t socket) {
    flog::logger* l = flog::logger::instance();
    std::stringstream str_out;
    cereal::BinaryOutputArchive ar_out(str_out);
    ar_out(msg);
    std::string buffer = str_out.str();
    size_t new_size = buffer.size() + HEADSIZE;
    char* raw_buffer = new char[new_size];
    uint32_t size = htonl(static_cast<uint32_t>(buffer.size()));
    memcpy(raw_buffer, &size, HEADSIZE);
    memcpy(raw_buffer + HEADSIZE, buffer.data(), buffer.size());
    l->log(flog::level_t::TRACE, "Sending message...");
    l->logc(flog::level_t::TRACE, "Data size : %d", buffer.size());
    l->logc(flog::level_t::TRACE, "Packet size : %d", new_size);
    if(cpnet_write(socket, raw_buffer, new_size) !=
            static_cast<ssize_t>(new_size)) {
        l->log(level::ERROR_, "Unable to write data on socket!");
        l->log(level::ERROR_, "Network backend returned error: %s",
               cpnet_last_error());
        delete[] raw_buffer;
        return false;
    }
    delete[] raw_buffer;
    return true;
}

marl::server::server():
    m_host {"localhost"},
    m_port {DEFAULT_PORT} {
}

void marl::server::initialize(const std::string& host, uint16_t port,
                              const std::string& problem_path, uint32_t agents) {
    m_host = host;
    m_port = port;
    m_problem = problem_path;
    m_agent_count = agents;
    flog::logger* l = flog::logger::instance();
    l->set_backend(flog::backend_t::STDERR);
    l->log(level::TRACE, "Creating communication server..");
    l->logc(level::TRACE, "Problem instance: `%s'", m_problem.c_str());
    l->logc(level::TRACE, "Agent count: %d", m_agent_count);
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
        // Initialize outgoing message queues
        m_response_queue_map.insert(std::make_pair(agent_count, new queue<action_select_rsp>{}));
        m_request_queue_map.insert(std::make_pair(agent_count, new queue<marl::action_select_req>{}));
        lck.unlock();
    }
    if(agent_count == m_agent_count) {
        l->log(flog::level_t::TRACE, "Sending start signal to all of them...");
        std::unique_lock<std::mutex> lck(m_maps_lock);
        for(const std::pair<uint32_t, socket_t>& kv : m_socket_map) {
            if(send_start(kv.second)) {
                m_readers.emplace_front(&server::reader, this, kv.first);
                m_writers.emplace_front(&server::writer, this, kv.first);
            }
        }
        lck.unlock();
    }
}

void marl::server::wait() {
    for(std::thread& t : m_readers) {
        t.join();
    }
    for(std::thread& t : m_writers) {
        t.join();
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
        if(!msg) {
            l->log(level::ERROR_, "Unable to read message from remote agent `%d'. "
                   "Terminating connection...",
                   agent_id);
            terminate_agent(agent_id);
        }
        std::this_thread::sleep_for(std::chrono::seconds{3});
        switch(msg->type()) {
            case MARL_ACTION_SELECT_REQ: {
                marl::action_select_req* r;
                r = dynamic_cast<marl::action_select_req*>(msg.get());
                if(r) {
                    process(*r);
                }
            }
            break;
            case MARL_ACTION_SELECT_RSP: {
                marl::action_select_rsp* r;
                r = dynamic_cast<marl::action_select_rsp*>(msg.get());
                if(r) {
                    process(*r);
                }
            }
            break;
            default:
                break;
        }
    }
}

void marl::server::writer(uint32_t agent_id) const {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Initiating writer thread for agent %d...", agent_id);
    std::unique_lock<std::mutex> lck(m_maps_lock);
    socket_t s = m_socket_map.at(agent_id);
    while(m_is_running) {
        marl::action_select_req req;
        marl::action_select_rsp rsp;
        std::chrono::milliseconds timeout{5};
        if(m_request_queue_map.at(agent_id)->wait_pop(req, timeout)) {
            send_message<action_select_req>(req, s);
        }
        if(m_response_queue_map.at(agent_id)->wait_pop(rsp, timeout)) {
            send_message<action_select_rsp>(rsp, s);
        }
    }
    lck.unlock();
}

std::unique_ptr<marl::message_base> marl::server::read_message(socket_t socket) const {
    flog::logger* l = flog::logger::instance();
    char data_buffer[DBUFSIZE];
    char header_buffer[HEADSIZE];
    ssize_t read_size = cpnet_read(socket, header_buffer, HEADSIZE);
    if(read_size != HEADSIZE) {
        l->log(level::ERROR_,
               "Unable to read header from incomming connection. Error: %s",
               cpnet_last_error());
        l->logc(level::ERROR_, "Read header size: %z", read_size);
        return nullptr;
    }
    uint32_t expected_size = 0;
    memcpy(&expected_size, header_buffer, HEADSIZE);
    expected_size = ntohl(expected_size);
    uint32_t total_read = 0;
    std::stringstream istr;
    while(total_read < expected_size) {
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
    if(expected_size != total_read) {
        l->log(level::ERROR_,
               "Expected incomming buffer size was %z, bit only %z received.",
               expected_size, total_read);
        return nullptr;
    }
    // Process incomming message: construct buffers
    std::unique_ptr<message_base> msg;
    try {
        cereal::BinaryInputArchive archiver(istr);
        archiver(msg);
    } catch(cereal::Exception& e) {
        l->log(level::ERROR_,
               "Unable to deserialize buffer into `message_base' object. "
               "Error: %s", e.what());
        return nullptr;
    }
    return msg;
}

void marl::server::terminate_agent(uint32_t agent_id) const {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Closing socket for agent `%d'...", agent_id);
    std::unique_lock<std::mutex> lck(m_maps_lock);
    socket_t s = m_socket_map.at(agent_id);
    lck.unlock();
    cpnet_close(s);
    l->logc(level::TRACE, "Connection closed");
}

bool marl::server::send_start(socket_t socket) const {
    marl::start_req req;
    req.agent_count = m_agent_count;
    return send_message<marl::start_req>(req, socket);
}

void marl::server::process(const marl::action_select_req& r) const {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Incomming action select request...");
    l->logc(level::TRACE, "Sender: %d", r.agent_id);
    for(uint32_t i = 0; i < m_agent_count; ++i) {
        if(i == r.agent_id) {
            // Dont send message back to the original sender
            continue;
        }
        m_request_queue_map.at(i)->push(r);
    }
}

void marl::server::process(const marl::action_select_rsp& r) const {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Incomming action select response...");
    l->logc(level::TRACE, "Sender: %d", r.agent_id);
    l->logc(level::TRACE, "Request Number: %d", r.request_number);
    // TODO: the key is wrong ...
    std::pair<uint32_t, uint32_t> key = std::make_pair(r.agent_id, r.request_number);
    if(m_response_map.find(key) == m_response_map.end()) {
        m_response_map.insert(std::make_pair(key, new std::vector<action_select_rsp>));
    }
    m_response_map.at(key)->push_back(r);
    // iterate over and check if responses are completed or not
    std::vector<action_select_rsp>* vec = m_response_map.at(key);
    if(vec->size() == m_agent_count - 1) {
        action_select_rsp aggregation;
        aggregation.agent_id = 0;
        aggregation.confidence = 0.0;
        aggregation.request_number = r.request_number;
        for(const action_select_rsp& asr : *vec) {
            for(const action_info& a : asr.info) {
                aggregation.info.push_back(a);
            }
        }
        //send_message
    }
}

