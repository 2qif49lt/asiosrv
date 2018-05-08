// g++ -std=c++11 -O2 main.cpp -o srv -I../ -lboost_system -lpthread

#include "srv.h"

#include <iostream>
#include <mutex>
typedef struct MsgHeadTag {
    uint32_t cmd;
    uint16_t len;
    uint16_t res;
}MsgHead, *PMsgHead;
class handler :public xhb::base_handler{
    std::mutex _mux;
public:
    // 阻塞连接的连接不会进入connect_handler,如果连接失败sockid为0，否则成功。
    virtual void connect_handler(uint32_t sockid, const std::string addr, uint16_t port, intptr_t conn_data)  {
        std::lock_guard<std::mutex> lk(_mux);
        std::cout << "connect_handler sockid: " << sockid << " addr: " << addr << " port: " << port << std::endl;
    }
    virtual bool accept_handler(uint32_t sockid, const std::string addr, uint16_t port, intptr_t* conn_data) {
        // 设置携带数据
        std::lock_guard<std::mutex> lk(_mux);
        std::cout << "accept_handler sockid: " << sockid << " addr: " << addr << " port: " << port << std::endl;
        return true;
    }
    // 返回false 会停止底层继续接收数据,但会继续等待发送给对方的数据发送完成,等待超时关闭。
    // 你也可以调用close()强行关闭。
    virtual bool msg_handler(uint32_t sockid, const std::string addr, uint16_t port, char* msg, uint32_t len, intptr_t conn_data) {
        const PMsgHead phead = reinterpret_cast<PMsgHead>(msg);
        std::lock_guard<std::mutex> lk(_mux);
        std::cout << "msg_handler sockid: " << sockid << " addr: " << addr << " port: " << port << " msg length: " << phead->len << std::endl;
        
        return true;
    }
    // accept_handler处理返回false、connect失败不会进入close_handler
    virtual void close_handler(uint32_t sockid, const std::string addr, uint16_t port, intptr_t conn_data,xhb::close_reason cr) {
        std::lock_guard<std::mutex> lk(_mux);
        std::cout << "close_handler sockid: " << sockid << " addr: " << addr << " port: " << port << " close_reason: " << cr << std::endl;
        
    }

    // 
    virtual char* alloc(const char* data, uint32_t len) {
        auto tmp = new(std::nothrow) char[len];
        std::copy(data, data + len, tmp);
        return tmp;
    }
    virtual void free(const char* data, uint32_t len) {
        delete [] data;
    }
};
int main() {
    handler hdler;
    xhb::tcpsrv<> srv(9980);

    return srv.listen_serve(&hdler);
}