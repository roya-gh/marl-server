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
#include <marl-protocols/terminate-response.hpp>
#include <marl-protocols/terminate-response-impl.hpp>
#include <marl-protocols/terminate-request-impl.hpp>
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
        m_starters.emplace_front(&server::starter, this, new_socket);
    }
    // Wait for connection negotiations to complete
    for(std::thread& t : m_starters) {
        t.join();
    }
    if(agent_count == m_agent_count) {
        l->log(level::TRACE, "All agents are connected and ready. Initiating IO threads...");
        for(auto& entry : m_socket_map) {
            m_readers.emplace_front(&server::reader, this, entry.first /*Agent ID*/);
            m_writers.emplace_front(&server::writer, this, entry.first /*Agent ID*/);
        }
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

void marl::server::starter(socket_t s) {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Initiating reader thread...");
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
        }
        if(m_socket_map.size() == m_agent_count) {
            m_all_agents_ready.store(true);
        }
        lck.unlock();
    }
    // Acknowledge join request
    send_message_helper<marl::join_rsp>(rsp, s);
}

void marl::server::writer(uint32_t agent_id) {
    /* This thread is supposed to be called after all agents are connected and
     * ready. So both the queu and socket maps must be already populated.
     * */
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Initiating writer thread for agent %d...", agent_id);
    // Search maps for sanity check
    std::unique_lock<std::mutex> lck(m_maps_lock);
    std::map<uint32_t, socket_t>::iterator found = m_socket_map.find(agent_id);
    if(found == m_socket_map.end()) {
        l->log(level::ERROR_, "Unable to find socket descriptor for agent %d! "
               "Terminating writer thread...", agent_id);
        return;
    }
    socket_t endpoint = found->second;
    std::map<uint32_t, marl::queue<action_select_req>*>::iterator found_queue =
        m_request_queue_map.find(agent_id);
    if(found_queue == m_request_queue_map.end()) {
        l->log(level::ERROR_, "Unable to find request buffer for agent %d! Terminating...", agent_id);
        return;
    }
    marl::queue<action_select_req>* req_queue = found_queue->second;
    lck.unlock();
    // Operate till the end
    bool has_error = false;
    while(m_is_running.load() && !has_error) {
        action_select_req req;
        bool fetched = req_queue->wait_pop(req, std::chrono::milliseconds{100});
        if(!fetched) {
            continue;
        }
        if(!send_message_helper<action_select_req>(req, endpoint)) {
            l->log(level::ERROR_, "Unable to send request for agent %d! Terminating writer thread...",
                   agent_id);
            has_error = true;
        }
    }
}

void marl::server::reader(uint32_t agent_id) {
    /* This thread is supposed to be called after all agents are connected and
     * ready. So both the queu and socket maps must be already populated.
     * */
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Sending start to %d...", agent_id);

    std::unique_lock<std::mutex> lck(m_maps_lock);
    std::map<uint32_t, socket_t>::iterator found = m_socket_map.find(agent_id);
    if(found == m_socket_map.end()) {
        l->log(level::ERROR_, "Unable to find socket descriptor for agent %d! "
               "Terminating reader thread...", agent_id);
        return;
    }
    socket_t endpoint = found->second;
    lck.unlock();
    // Send start command if everything is OK
    send_start(endpoint);
    while(m_is_running.load()) {
        std::unique_ptr<message_base> msg = marl::receive_message(endpoint);
        if(!msg) {
            l->log(level::ERROR_, "Unable to read message from remote agent `%d'. "
                   "Terminating connection...", agent_id);
            terminate_agent(agent_id);
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
            case MARL_TERMINATE_REQ: {
                marl::terminate_request* r;
                r = dynamic_cast<marl::terminate_request*>(msg.get());
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

void marl::server::terminate_agent(uint32_t agent_id) const {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Closing socket for agent `%d'...", agent_id);
    std::unique_lock<std::mutex> lck(m_maps_lock);
    socket_t s = m_socket_map.at(agent_id);
    lck.unlock();
    cpnet_close(s);
    l->logc(level::TRACE, "Connection closed");
}

void marl::server::process(const terminate_request& req) const {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Requesting termination for agent `%d'...", req.agent_id);
    static std::mutex mtx;
    std::lock_guard<std::mutex> lck(mtx);
    static uint32_t count{0};
    count++;
    if(count == m_agent_count) {
        // terminate all
        l->log(level::TRACE, "All agents requested for disconnect.");
        for(auto& pair : m_socket_map) {
            l->log(level::TRACE, "Closing socket for agent `%d'...", pair.first);
            marl::terminate_response rsp;
            rsp.agent_id = req.agent_id;
            send_message_helper<marl::terminate_response>(rsp, pair.second);
            cpnet_close(pair.second);
            l->logc(level::TRACE, "Connection closed for agent `%d'...", pair.first);
        }
    }
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
    std::for_each(m_request_queue_map.begin(), m_request_queue_map.end(), pusher);
}

void marl::server::process(const marl::action_select_rsp& r) const {
    flog::logger* l = flog::logger::instance();
    l->log(level::TRACE, "Incomming action select response...");
    l->logc(level::TRACE, "Sender: %d (ID,seq) = (%d, %d)",
            r.agent_id,
            r.requester_id, r.request_number);
    // TODO: the key is wrong ...
    std::lock_guard<std::mutex> lk(m_response_map_lck);
    std::pair<uint32_t, uint32_t> key = std::make_pair(r.requester_id, r.request_number);
    if(m_response_map.find(key) == m_response_map.end()) {
        l->log(level::TRACE, "Request key was not found in the map. Insering...");
        l->logc(level::TRACE, "Request key was not found in the map. (%d, %d)",
                key.first, key.second);
        m_response_map.insert(std::make_pair(key, new std::vector<action_select_rsp>));
    }
    std::vector<action_select_rsp>* vec = m_response_map.at(key);
    vec->push_back(r);
    // iterate over and check if responses are completed or not
    if(vec->size() == m_agent_count - 1) {
        // TODO: Do a sanity check
        l->log(level::TRACE, "Message queue is completed for (%d, %d)",
               key.first, key.second);
        action_select_rsp aggregation;
        aggregation.agent_id = 0;
        aggregation.confidence = 0.0;
        aggregation.request_number = r.request_number;
        for(const action_select_rsp& asr : *vec) {
            for(const action_info& a : asr.info) {
                aggregation.info.push_back(a);
            }
        }
        bool success = send_message_helper<marl::action_select_rsp>(aggregation,
                       this->m_socket_map.at(r.requester_id));
        if(!success) {
            l->log(level::ERROR_, "Unable to send response to %d for request %d",
                   key.first, key.second);
        }
        // cleanup
        vec->clear();
        m_response_map.erase(key);
    }
}

