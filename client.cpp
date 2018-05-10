//
// blocking_tcp_echo_client.cpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// g++ -std=c++11 client.cpp -o cli -lpthread -lboost_system
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <chrono>
#include <mutex>
#include <vector>

#include <boost/asio.hpp>

using boost::asio::ip::tcp;

typedef struct MsgHeadTag {
    uint32_t cmd;
    uint16_t len;
    uint16_t res;
}MsgHead, *PMsgHead;

enum { max_length = 10240 };
std::mutex cout_mux;
std::atomic<uint32_t> thread_num = {0};
std::atomic<uint32_t> connection_id = {1};

void cli(const char* host, const char* srv, std::chrono::seconds sec ) {
    thread_num.fetch_add(1, std::memory_order_relaxed);
    try {
        auto cid = connection_id.fetch_add(1, std::memory_order_relaxed);

        boost::asio::io_context io_context;

        tcp::socket s(io_context);
        tcp::resolver resolver(io_context);
        boost::asio::connect(s, resolver.resolve(host, srv));

        
        char buff[max_length];
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(100, max_length - sizeof(MsgHead)); 

        PMsgHead phead = (PMsgHead)&buff;
        auto beg = std::chrono::steady_clock::now();
        uint32_t send_counter = 0;
        uint32_t recv_counter = 0;
        uint32_t bytes_send_counter = 0;
        uint32_t bytes_recv_counter = 0;
        while (1) 
        {
            auto end = std::chrono::steady_clock::now();
            auto diff = std::chrono::duration_cast<std::chrono::seconds>(end - beg);
            if (diff > sec) {
                break;
            }

            phead->cmd = 1234;
            uint32_t len = sizeof(MsgHead) + dis(gen);
            phead->len = len;

            boost::asio::write(s, boost::asio::buffer(buff, len));
            send_counter++;
            bytes_send_counter += len;

            size_t reply_length = boost::asio::read(s,
                boost::asio::buffer(buff, len));
            recv_counter++;
            bytes_recv_counter += reply_length;

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        {
            std::lock_guard<std::mutex> lk(cout_mux);
            std::cout << "cid: " << cid << " run: " << sec.count() << "s send_counter: " << send_counter << " recv_counter: " << recv_counter
                << " bytes_send_counter: " << bytes_send_counter << " bytes_recv_counter: "
                << bytes_recv_counter << std::endl;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << "\n";
    }
    thread_num.fetch_sub(1, std::memory_order_relaxed);
}


int main(int argc, char* argv[])
{

    if (argc != 3)
    {
      std::cerr << "Usage: blocking_tcp_echo_client <host> <port>\n";
      return 1;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1, 30); 
    std::vector<std::thread> vec;
    while (1) {
        if (thread_num.load(std::memory_order_relaxed) < 50) {
            vec.push_back(std::thread(&cli, argv[1], argv[2], std::chrono::seconds(dis(gen))));
            vec.back().detach();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

  return 0;
}