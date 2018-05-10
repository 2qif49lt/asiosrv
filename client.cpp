//
// blocking_tcp_echo_client.cpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>

#include <boost/asio.hpp>

using boost::asio::ip::tcp;

typedef struct MsgHeadTag {
    uint32_t cmd;
    uint16_t len;
    uint16_t res;
}MsgHead, *PMsgHead;

enum { max_length = 10240 };

int main(int argc, char* argv[])
{
  try
  {
    if (argc != 3)
    {
      std::cerr << "Usage: blocking_tcp_echo_client <host> <port>\n";
      return 1;
    }

    boost::asio::io_context io_context;

    tcp::socket s(io_context);
    tcp::resolver resolver(io_context);
    boost::asio::connect(s, resolver.resolve(argv[1], argv[2]));

    
    char buff[max_length];
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100, max_length - sizeof(MsgHead)); 

    PMsgHead phead = (PMsgHead)&buff;
    while (1) 
    {
        phead->cmd = 1234;
        uint32_t len = sizeof(MsgHead) + dis(gen);
        phead->len = len;
        boost::asio::write(s, boost::asio::buffer(buff, len));
        std::cout << "write data cmd: " << phead->cmd << " length: " << phead->len << std::endl;
        
        size_t reply_length = boost::asio::read(s,
            boost::asio::buffer(buff, len));
        std::cout << "read data cmd: " << phead->cmd << " length: " << phead->len << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}