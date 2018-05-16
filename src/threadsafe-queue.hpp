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

#ifndef MARL_TS_QUEUE_HPP
#define MARL_TS_QUEUE_HPP

#include <queue>
#include <chrono>
#include <mutex>
#include "semaphore.hpp"

namespace marl {
template <typename T>
class queue {
public:
    queue() = default;
    queue(const queue&) = delete;
    ~queue() = default;
    void push(const T& v);
    void push(const T&& v);
    T pop();
    bool wait_pop(T&, const std::chrono::nanoseconds& timeout);
private:
    std::queue<T> m_q;
    tidm::semaphore m_sem;
    std::mutex m_mtx;
};

template<typename T>
void queue<T>::push(const T& v) {
    std::lock_guard<std::mutex> lck{m_mtx};
    m_q.push(v);
    m_sem.notify();
}

template<typename T>
void queue<T>::push(const T&& v) {
    std::lock_guard<std::mutex> lck{m_mtx};
    m_q.push(v);
    m_sem.notify();
}

template<typename T>
T queue<T>::pop() {
    m_sem.wait();
    std::lock_guard<std::mutex> lck{m_mtx};
    T x = m_q.front();
    m_q.pop();
    return x;
}

template<typename T>
bool queue<T>::wait_pop(T& x, const std::chrono::nanoseconds& timeout) {
    if(!m_sem.wait_for(timeout)) {
        return false;
    } else {
        std::lock_guard<std::mutex> lck{m_mtx};
        x = m_q.front();
        m_q.pop();
        return true;
    }
}

}

#endif // MARL_TS_QUEUE_HPP
