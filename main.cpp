// g++ -std=c++11 -O2 main.cpp -o srv -I../ -lboost_system -lpthread

#include "srv.h"

#include <iostream>
#include <mutex>
#include <atomic>
#include <chrono>

typedef struct MsgHeadTag {
    uint32_t cmd;
    uint16_t len;
    uint16_t res;
}MsgHead, *PMsgHead;
class handler :public xhb::base_handler{
public:
    std::mutex _mux;
    std::atomic<uint32_t> _counter_memory = {0};
    std::atomic<uint32_t> _counter_accept = {0};
    std::atomic<uint32_t> _counter_close = {0};
    std::atomic<uint32_t> _counter_msg_recv = {0};
    std::atomic<uint32_t> _counter_msg_send = {0};
    xhb::tcpsrv<>& _srv;
public:
    handler(xhb::tcpsrv<>& srv) : _srv(srv) {}

    virtual void connect_handler(uint32_t sockid, const std::string& addr, uint16_t port, intptr_t conn_data)  {
    /*
        std::lock_guard<std::mutex> lk(_mux);
        std::cout << "connect_handler sockid: " << sockid << " addr: " << addr << " port: " << port << std::endl;
    */
    }
    virtual bool accept_handler(uint32_t sockid, const std::string& addr, uint16_t port, intptr_t* conn_data) {
        // 设置携带数据
        /*
        std::lock_guard<std::mutex> lk(_mux);
        std::cout << "accept_handler sockid: " << sockid << " addr: " << addr << " port: " << port << std::endl;
        if (sockid == 2) {
            return false;
        }
        */
        _counter_accept.fetch_add(1,std::memory_order_relaxed);

        return true;
    }
    virtual bool msg_handler(uint32_t sockid, const std::string& addr, uint16_t port, char* msg, uint32_t len, intptr_t conn_data) {
     /*
        const PMsgHead phead = reinterpret_cast<PMsgHead>(msg);
        std::lock_guard<std::mutex> lk(_mux);
        std::cout << "msg_handler sockid: " << sockid << " addr: " << addr << " port: " << port << " msg length: " << phead->len << std::endl;
    */  
        _counter_msg_recv.fetch_add(1,std::memory_order_relaxed); 
        // 模拟耗时15ms
      
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        const PMsgHead phead = reinterpret_cast<PMsgHead>(msg);
        phead->cmd++;
        bool bsend = _srv.async_send(sockid, msg, len);
        if (bsend)
            _counter_msg_send.fetch_add(1,std::memory_order_relaxed);  
        else {
            std::lock_guard<std::mutex> lk(_mux);
            std::cout << "msg_handler send queue is full. sockid: " << sockid << " addr: " << addr << " port: " << port << std::endl;
        }
        return true;
    }
    virtual void close_handler(uint32_t sockid, const std::string& addr, uint16_t port, intptr_t conn_data,xhb::close_reason cr) {
    /*
        std::lock_guard<std::mutex> lk(_mux);
        std::cout << "close_handler sockid: " << sockid << " addr: " << addr << " port: " << port << " close_reason: " << cr << std::endl;
    */
        _counter_close.fetch_add(1,std::memory_order_relaxed);
    }

    virtual char* alloc(const char* data, uint32_t len) {
        auto tmp = new(std::nothrow) char[len];
        std::copy(data, data + len, tmp);
        _counter_memory.fetch_add(len + 1,std::memory_order_relaxed);
        return tmp;
    }
    virtual void free(const char* data, uint32_t len) {
        _counter_memory.fetch_sub(len,std::memory_order_relaxed);
        delete [] data;
    }
};
int main() {
    xhb::tcpsrv<> srv(9980);
    handler hdler(srv);

    std::thread([&srv, &hdler](){
        while(1) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            {
                auto memory = hdler._counter_memory.load(std::memory_order_relaxed);
                auto accept = hdler._counter_accept.load(std::memory_order_relaxed);
                auto close = hdler._counter_close.load(std::memory_order_relaxed);
                auto recv = hdler._counter_msg_recv.load(std::memory_order_relaxed);
                auto send = hdler._counter_msg_send.load(std::memory_order_relaxed);
                auto alives = srv.alives();
                std::lock_guard<std::mutex> lk(hdler._mux);
                std::cout << "alives: " << alives <<  " memory: " << memory << " accept: " << accept << " close: " << close 
                    << " recv: " << recv << " send: " << send << std::endl;
            }
        }
    }).detach();
    return srv.listen_serve(&hdler);
}