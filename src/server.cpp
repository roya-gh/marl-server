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
#include <algorithm>
#include <iostream>
#include <sstream>
#include <flog/flog.hpp>
#include <marl-protocols/message-base.hpp>
#include <marl-protocols/message-base-impl.hpp>
#include <marl-protocols/start-request-impl.hpp>
#include <marl-protocols/join-request-impl.hpp>
#include <marl-protocols/join-response-impl.hpp>
#include <marl-protocols/message-utilities.hpp>
#include <cereal/archives/binary.hpp>

using level = flog::level_t;

marl::server::server():
    m_host {"localhost"},
    m_port {DEFAULT_PORT} {
    m_all_agents_ready.store(false);
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
    std::vector<socket_t> all_sockets;
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
        all_sockets.push_back(new_socket);
        m_readers.emplace_front(&server::reader, this, new_socket);
    }
//    if(agent_count == m_agent_count) {
//        for(socket_t& s : all_sockets) {
//            //m_writers.emplace_front(&server::writer, this, s);
//        }
//    }
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

void marl::server::reader(socket_t s) {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Initiating reader thread...");
    uint32_t id;
    join_req req;
    join_rsp rsp;
    if(!receive_message_helper<marl::join_req>(req, s)) {
        l->log(level::ERROR_,
               "Unable to get join request from client.");
        stop();
    } else {
        l->log(level::TRACE, "Join request received. Checking ability to join.");
        l->logc(level::TRACE, "Agetnt ID: %d.", req.agent_id);
        l->logc(level::TRACE, "Problem name: `%s'.", req.problem_name.c_str());
        std::unique_lock<std::mutex> lck(m_maps_lock);
        if(m_socket_map.find(req.agent_id) != m_socket_map.end()) {
            l->log(level::ERROR_,
                   "Agent ID `%d' is already registered...", req.agent_id);
            rsp.success = false;
            rsp.error_message = "Duplicated agent ID.";
            stop();
        } else {
            m_socket_map.insert(std::make_pair(req.agent_id, s));
            // Initialize outgoing message queues
            m_response_queue_map.insert(std::make_pair(req.agent_id, new queue<action_select_rsp> {}));
            m_request_queue_map.insert(std::make_pair(req.agent_id, new queue<action_select_req> {}));
            rsp.success = true;
            id = req.agent_id;
        }
        if(m_socket_map.size() == m_agent_count) {
            m_all_agents_ready.store(true);
        }
        lck.unlock();
    }
    send_message_helper<marl::join_rsp>(rsp, s);
    l->log(level::TRACE, "Waiting for all agents to get ready.");
    while(m_is_running & !m_all_agents_ready) {
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }
    l->log(level::TRACE, "All agents are already joined. Sending start to %d...", id);
    send_start(s);
    while(m_is_running) {
        std::unique_ptr<message_base> msg = marl::receive_message(s);
        if(!msg) {
            l->log(level::ERROR_, "Unable to read message from remote agent `%d'. "
                   "Terminating connection...", id);
            terminate_agent(id);
            return;
        }
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

void marl::server::writer(socket_t s) {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Initiating writer thread...", s);
    std::unique_lock<std::mutex> lck(m_maps_lock);
    //socket_t s = m_socket_map.at(s);
    while(m_is_running) {
        marl::action_select_req req;
        marl::action_select_rsp rsp;
        std::chrono::milliseconds timeout{5};
        if(m_request_queue_map.at(s)->wait_pop(req, timeout)) {
            send_message_helper<action_select_req>(req, s);
        }
        if(m_response_queue_map.at(s)->wait_pop(rsp, timeout)) {
            send_message_helper<action_select_rsp>(rsp, s);
        }
    }
    lck.unlock();
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
    return send_message_helper<marl::start_req>(req, socket);
}

void marl::server::process(const marl::action_select_req& r) const {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Incomming action select request...");
    l->logc(level::TRACE, "Sender: %d", r.agent_id);
    auto pusher = [&](std::pair<uint32_t, queue<action_select_req>*> element) {
        if(r.agent_id != element.first) {
            element.second->push(r);
        }
    };
    std::for_each(m_request_queue_map.begin(), m_request_queue_map.end(),pusher);
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

