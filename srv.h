#ifndef XHBLIB_SRV_SRV_H_
#define XHBLIB_SRV_SRV_H_

/*
说明: 只支持最长65535数据包
*/

#include <boost/asio.hpp>

#include <iostream>
#include <thread>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <cstddef>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <map>
#include <functional>
#include <cassert>
#include <ctime>

#if defined(__GNUC__) || defined(__GNUG__) || defined(__clang__)

#ifndef likely
#define likely(x)       ::__builtin_expect(!!(x), 1)
#endif 

#ifndef unlikely
#define unlikely(x)     ::__builtin_expect(!!(x), 0)
#endif

#else
// msvc
#define likely(x) (x)
#define unlikely(x) (x)
#define __builtin_expect(exp, a)  (exp)

#endif

#ifdef _MSC_VER
#include <Windows.h>
#else
#include <xmmintrin.h>
#define YieldProcessor()  _mm_pause()
#endif

namespace xhb {
using std::uint32_t;
using std::uint16_t;

#pragma pack(push)
#pragma pack(1)

struct msg_head {
    uint32_t cmd;
    uint16_t len;
    uint16_t res;
};

#pragma(pop)

enum class close_reason :int16_t {
    active_close , // 0 主动关闭
    packet_error, // 包头错误
    time_out, // 超时, 以接收到合法的数据包为准。
    srv_stop, // 服务关闭
    io_error, // 网络错误
    close_by_peer, // 对方关闭
    no_accept, // 连接时被拒绝
};

template <typename Enumeration>
auto as_integer(Enumeration const value)
    -> typename std::underlying_type<Enumeration>::type
{
    return static_cast<typename std::underlying_type<Enumeration>::type>(value);
}

std::ostream& operator<<(std::ostream& o, const close_reason& r){
    auto i = as_integer(r);
    std:: string tmp;
    switch (r) {
        case close_reason::active_close:
            tmp = "active close";
            break;
        case close_reason::packet_error:
            tmp = "invalid msg head";
            break;
        case close_reason::time_out:
            tmp = "time out";
            break;
        case close_reason::srv_stop:
            tmp = "service stop";
            break;
        case close_reason::io_error:
            tmp = "network error";
            break;
        case close_reason::close_by_peer:
            tmp = "closed by peer";
            break;
        case close_reason::no_accept:
            tmp = "not accepted";
            break;
        default:
            tmp = "wrong";
            break;
    }

     return o << i << "(" << tmp << ")";
}
// base_handler 处理事件，
class base_handler {
public:
    base_handler() = default;
    virtual ~base_handler() {}

    // 阻塞连接的连接不会进入connect_handler。异步连接如果连接失败sockid为0，否则成功。
    virtual void connect_handler(uint32_t sockid, const std::string& addr, uint16_t port, intptr_t conn_data)  {

    }
    virtual bool accept_handler(uint32_t sockid, const std::string& addr, uint16_t port, intptr_t* conn_data) {
        // 设置携带数据
        *conn_data = sockid;
        return true;
    }
    // 返回false 会停止底层继续接收数据,但会继续等待发送给对方的数据发送完成,等待超时关闭。
    // 你也可以调用close()强行关闭。
    virtual bool msg_handler(uint32_t sockid, const std::string& addr, uint16_t port, char* msg, uint32_t len, intptr_t conn_data) {
        uint32_t tmp = static_cast<uint32_t>(conn_data);
        return true;
    }
    // 阻塞connect失败的不会进入close_handler
    virtual void close_handler(uint32_t sockid, const std::string& addr, uint16_t port, intptr_t conn_data, close_reason error) {
    }

    // 将底层内存管理接口开放给使用者，使框架保持zero-copy。使用者可以使用对象池等。
    virtual char* alloc(const char* data, uint32_t len) {
        auto tmp = new(std::nothrow) char[len];
        std::copy(data, data + len, tmp);
        return tmp;
    }
    virtual void free(const char* data, uint32_t len) {
        delete [] data;
    }
};

} // xhb ns
namespace xhb {
// memory order
// http://preshing.com/20120913/acquire-and-release-semantics/
// The role of a release fence, as defined by the C++11 standard, 
// is to prevent previous memory operations from moving past subsequent stores.

class spinlock {
    std::atomic<bool> _busy = { false };
public:
    spinlock() = default;
    spinlock(const spinlock&) = delete;
    ~spinlock() { assert(!_busy.load(std::memory_order_relaxed)); }
    void lock() noexcept {
        while (_busy.exchange(true, std::memory_order_acquire)) {
            YieldProcessor();
        }
    }
    void unlock() noexcept {
        _busy.store(false, std::memory_order_release);
    }
};
} // xhb ns

namespace xhb {

template <uint32_t POOL_SIZE = 128>
class aqueue final {
    static_assert((POOL_SIZE & (POOL_SIZE - 1)) == 0 && POOL_SIZE <= (2 << 12), "pool_size must be a power of 2.");
    static constexpr uint32_t MASK = POOL_SIZE - 1;
    static constexpr uint32_t cache_line_size = 64;

private:
    struct item {
        const char* _data = nullptr;
        uint32_t _len = 0;
        char _pad[cache_line_size - sizeof(char*) - sizeof(uint32_t)];
    };
    item _objs[POOL_SIZE];

    std::atomic<uint32_t> _prod_head = {0};
    std::atomic<uint32_t> _prod_tail = {0};

    std::atomic<uint32_t> _cons_head = {0};
    std::atomic<uint32_t> _cons_tail = {0};

    using my_type = aqueue<POOL_SIZE>;

public:

    aqueue() = default;
    ~aqueue() = default;

    aqueue(const my_type&) = delete;
    aqueue& operator=(const my_type&) = delete;

    uint32_t free_items() const {
        uint32_t prod_head = 0, cons_tail = 0;

        cons_tail = _cons_tail.load(std::memory_order_acquire);
        prod_head = _prod_head.load(std::memory_order_relaxed); 
        
        return MASK + cons_tail - prod_head;
    }

    uint32_t size() const {
        uint32_t prod_tail = 0, cons_head = 0;

        prod_tail = _prod_tail.load(std::memory_order_acquire); 
        cons_head = _cons_head.load(std::memory_order_relaxed);
        
        return  prod_tail - cons_head;
    }
    // push  满则返回false
    bool push(const char* ptr, uint32_t len) {
        uint32_t prod_head, prod_next;

        do {
            prod_head = _prod_head.load(std::memory_order_acquire);
            uint32_t cons_tail = _cons_tail.load(std::memory_order_relaxed);
            uint32_t free_entries = MASK + cons_tail - prod_head;
            
            if(unlikely(free_entries == 0)) {
                return false;
            }

            prod_next = prod_head + 1;
            
        } while (false == _prod_head.compare_exchange_weak(prod_head, prod_next,
            std::memory_order_release, std::memory_order_relaxed));
        
        uint32_t idx = prod_head & MASK;
        _objs[idx]._data = ptr;
        _objs[idx]._len = len;

        while (unlikely(_prod_tail.load(std::memory_order_relaxed) != prod_head)) {
            YieldProcessor();
        }
        _prod_tail.store(prod_next, std::memory_order_release);
        
        return true;
    }

    // pop 空则返回false
    bool pop(const char* &ptr, uint32_t& len) {
        uint32_t cons_head, cons_next;

        ptr = nullptr;
        len = 0;

        do {
            uint32_t prod_tail = _prod_tail.load(std::memory_order_acquire);
            cons_head = _cons_head.load(std::memory_order_relaxed);
            uint32_t entries = prod_tail - cons_head;
            
            if(unlikely(entries == 0)) {
                return false;
            }

            cons_next = cons_head + 1;

        } while (false == _cons_head.compare_exchange_weak(cons_head, cons_next, 
            std::memory_order_release, std::memory_order_relaxed));
        
        uint32_t idx = cons_head & MASK;
        ptr = _objs[idx]._data;
        len = _objs[idx]._len;

        while (unlikely(_cons_tail.load(std::memory_order_relaxed) != cons_head)) {
            YieldProcessor();
        }
        _cons_tail.store(cons_next, std::memory_order_relaxed);

        return true;
    }

    template<typename F /*void(const char* ptr, uint32_t len)*/>
    void clear(F&& fun) {
        const char* ptr = nullptr;
        uint32_t len = 0;
        while(pop(ptr, len)) {
            fun(ptr, len);
        }
    }
};

} // xhb ns


namespace xhb {

namespace asio = boost::asio;
namespace system = boost::system; 
namespace ip = boost::asio::ip;
using tcp = boost::asio::ip::tcp;

template<size_t, size_t> 
class tcpconn;

template <size_t HEADER_SIZE = sizeof(msg_head), 
    size_t SIZE_FIELD_OFFSET = offsetof(msg_head, len) > 
class tcpsrv final{
    using context_guard = typename asio::executor_work_guard<asio::io_context::executor_type>;
    using conn_type = tcpconn<HEADER_SIZE, SIZE_FIELD_OFFSET>;

    asio::io_context _ioc;
    std::unique_ptr<context_guard> _guard;
    tcp::acceptor _acceptor;
    tcp::endpoint _ep;
    bool _working = false;
    std::atomic<uint32_t> _sockid = {1};
    base_handler* _handler = nullptr;

    std::map<uint32_t, std::shared_ptr<conn_type>> _conn_map;
    std::mutex _map_mux;
    std::mutex _acceptor_mux; // for acceptor
    size_t _time_out_secs = 60;
public:
    tcpsrv(uint16_t port, const std::string& addr = "") : _acceptor(_ioc) {
        if (addr == "") {
            _ep = tcp::endpoint(tcp::v4(), port);
        } else {
            _ep = tcp::endpoint(ip::make_address(addr), port);
        }
    }
    ~tcpsrv() {
        if (_working) {
            stop();
            clean_map();
        }

    }
    tcpsrv(const tcpsrv&) = delete;
    tcpsrv& operator=(const tcpsrv&) = delete;

    void set_time_out(uint32_t time_out_secs = 60) {
        _time_out_secs = time_out_secs;
    }

    // run 开启n个工作线程
    int listen_serve(base_handler* handler, uint32_t n = std::thread::hardware_concurrency()) {
        _handler = handler;
        int ret = 0;

        ret = init();
        if (ret != 0) {
            return ret;
        }
        
        _working = true;

        std::mutex mux;
        std::condition_variable cv;
        uint32_t thread_num = 0;

        std::vector<std::shared_ptr<std::thread>> vec;
        for (uint32_t i = 0; i != n; ++i) {
            vec.push_back(std::make_shared<std::thread>([this, &mux, &cv, &thread_num](){
                {
                    std::lock_guard<std::mutex> lk(mux);
                    ++thread_num;
                }
                _ioc.run(); 
                {
                    std::lock_guard<std::mutex> lk(mux);
                    --thread_num;
                }
                cv.notify_all();
            }));
        }
        for (auto& pt : vec) {
            pt->detach();
        }
        post_accept();
        
        std::this_thread::sleep_for(std::chrono::seconds(1)); // for ioc.run thread switch;
        while(1) {
            std::unique_lock<std::mutex> lk(mux);
            if (cv.wait_for(lk, std::chrono::milliseconds(500), [&thread_num]{return thread_num == 0;}) == true) {
                break;
            }
            // do connections check
            check_connections();
        }
        _working = false;
        stop();
        clean_map();
        return 0;
    }

    // stop() 停止接收数据,处理完剩余的事件，停止服务。
    void stop() {
        system::error_code ec;
        {
            std::lock_guard<std::mutex> lk(_acceptor_mux);
            _acceptor.close(ec);
        }
        _guard->reset();
    }
    // shutdown() 立即停止服务,
    void shutdown() {
        _ioc.stop();
    }

    bool async_send(uint32_t sockid, const char* data, uint32_t len) {
        std::lock_guard<std::mutex> lk(_map_mux);
        auto fit= _conn_map.find(sockid);
        if (fit == _conn_map.end()) {
            return false;
        }
        return fit->second->send(data, len);
    }
    void close(uint32_t sockid) {
        std::lock_guard<std::mutex> lk(_map_mux);
        auto fit= _conn_map.find(sockid);
        if (fit == _conn_map.end()) {
            return;
        } 
        fit->second->close_sock(close_reason::active_close);
        _conn_map.erase(fit);
    }

    uint32_t connect(const std::string& addr, uint16_t port, intptr_t conn_data = 0) {
        system::error_code ec;
        auto ipaddr = ip::make_address(addr, ec);
        if (ec) {
            return 0;
        }
        tcp::endpoint ep(ipaddr, port);
        auto psock = std::make_shared<tcp::socket>(_ioc);
        
        psock->connect(ep, ec);
        if (ec) {
            return 0;
        }
        auto sockid = _sockid.fetch_add(1, std::memory_order_relaxed);
        auto pconn = std::make_shared<conn_type>(sockid, _ioc, psock, _handler);
        pconn->_data = conn_data;
        add_map(sockid, pconn);
        pconn->start();

        return sockid;
    }
    bool async_connect(const std::string& addr, uint16_t port, intptr_t conn_data) {
        system::error_code ec;
        auto ipaddr = ip::make_address(addr, ec);
        if (ec) {
            return false;
        }
        tcp::endpoint ep(ipaddr, port);
        auto psock = std::make_shared<tcp::socket>(_ioc);

        psock->async_connect(ep, [this, psock, conn_data, addr, port](const system::error_code& ec){
            uint32_t sockid = 0;
            if (!ec) {
                // success
                sockid = _sockid.fetch_add(1, std::memory_order_relaxed);
                auto pconn = std::make_shared<conn_type>(sockid, _ioc, psock, _handler);
                pconn->_data = conn_data;
                add_map(sockid, pconn);
                // 保证连接事件发生在消息事件之前。
                _handler->connect_handler(sockid, addr, port, conn_data);
                pconn->start();
            } else {
                _handler->connect_handler(sockid, addr, port, conn_data);
            }
        });
        return true;
    }

    // 活跃的连接数
    size_t alives() {
        return check_connections();
    }
private:

    int init() {
        system::error_code ec;

        _ioc.restart();
        // _guard = std::make_unique<context_guard>(asio::make_work_guard(_ioc));
        _guard = std::unique_ptr<context_guard>( new context_guard(asio::make_work_guard(_ioc)));
        _acceptor.open(_ep.protocol(), ec);
        if (ec) {
            return 1;
        }
        asio::socket_base::reuse_address option(true);
        _acceptor.set_option(option);
        _acceptor.bind(_ep, ec);
        if (ec) {
            return 2;
        }
        _acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            return 3;
        }
        return 0;
    }

    void clean_map() {
        std::lock_guard<std::mutex> lk(_map_mux);
        auto it = _conn_map.begin();
        while (it != _conn_map.end()) {
            it->second->close_sock(close_reason::srv_stop);
            ++it;
        }
        std::map<uint32_t, std::shared_ptr<conn_type>> empty_map;
        _conn_map.clear();
        _conn_map.swap(empty_map);
    }

    size_t check_connections() {
        std::lock_guard<std::mutex> lk(_map_mux);

        auto now = std::chrono::steady_clock::now();
        auto it = _conn_map.begin();
        auto time_out_threshold = std::chrono::seconds(_time_out_secs);

        while (it != _conn_map.end()) {
            auto diff = now - it->second->living_tick();
            auto is_closed = it->second->is_closed();

            if (diff > time_out_threshold) {
                it->second->close_sock(close_reason::time_out);
                it = _conn_map.erase(it);
            } else if (is_closed) {
                it = _conn_map.erase(it);
            }
            else 
                ++it;
        }
        return _conn_map.size();
    }
    void add_map(uint32_t sockid, std::shared_ptr<conn_type> conn) {
        std::lock_guard<std::mutex> lk(_map_mux);
        _conn_map[sockid] = conn;
    }
    void post_accept() {
        auto psock = std::make_shared<tcp::socket>(_ioc);
        
        std::lock_guard<std::mutex> lk(_acceptor_mux);
        _acceptor.async_accept(*psock, [this, psock](const system::error_code& ec){
            if (!ec) {
                system::error_code e;
                tcp::endpoint ep = psock->remote_endpoint(e);
                if (e) {
                    psock->close(e);
                } else {
                    auto sockid = _sockid.fetch_add(1, std::memory_order_relaxed);
                    auto pconn = std::make_shared<conn_type>(sockid, _ioc, psock, _handler);
                    if (_handler->accept_handler(sockid, ep.address().to_string(), ep.port(), &pconn->_data) == true) {
                        add_map(sockid, pconn);
                        pconn->start();
                    } else {
                        pconn->close_sock(close_reason::no_accept);
                    }
                }
            }
            post_accept();
        });
    }
};

template <size_t HEADER_SIZE, size_t LEN_FIELD_OFFSET>
class tcpconn : public std::enable_shared_from_this<tcpconn<HEADER_SIZE, LEN_FIELD_OFFSET>> {
    friend class tcpsrv<HEADER_SIZE, LEN_FIELD_OFFSET>;

    using time_point_t = std::chrono::time_point<std::chrono::steady_clock>;

    static constexpr uint32_t BUFF_SIZE = 1024 * 10;

    char _buff[BUFF_SIZE];
    
    std::shared_ptr<tcp::socket> _sock;
    asio::io_context& _ioc;
    uint32_t _sockid;

    bool _sending = false;
    bool _closed = false;
    spinlock _lock;

    aqueue<8> _queue;

    base_handler* _handler = nullptr;
    intptr_t _data = 0;
    std::string _remote_addr;
    uint16_t _remote_port;
    time_point_t _living_tick;

public:
    tcpconn(uint32_t sockid, asio::io_context& ioc, std::shared_ptr<tcp::socket> psock, base_handler* handler) :
        _sockid(sockid), _ioc(ioc), _sock(psock), _handler(handler) {
            _remote_addr = psock->remote_endpoint().address().to_string();
            _remote_port = psock->remote_endpoint().port();
            _living_tick = std::chrono::steady_clock::now();
    }
    ~tcpconn() { 
        close_sock(close_reason::active_close);
        _queue.clear([this](const char* data, uint32_t len){
            _handler->free(data, len);
        });
    }

    bool send(const char* data, uint32_t len) {
        {
            std::lock_guard<spinlock> lk(_lock);
            if (_closed == true) {
                return false;
            }
        }
        auto tmp = _handler->alloc(data, len);
        if(_queue.push(tmp, len)== false) {
            _handler->free(tmp, len);
            return false;
        }
        post_send();
        return true;
    }
    void close() {
        close_sock(close_reason::active_close);
    }
    void close_sock(close_reason cr) {
        std::lock_guard<spinlock> lk(_lock);
        if (_closed == true) {
            return;
        }
        _closed = true;

        system::error_code ec;
        _sock->shutdown(tcp::socket::shutdown_both, ec);
        _sock->close(ec);
        
        _handler->close_handler(_sockid, _remote_addr, _remote_port, 
            _data, cr);
        
    }
    bool is_closed() {
        std::lock_guard<spinlock> lk(_lock);
        return _closed;
    }
private:
    void start() {
        system::error_code ec;
        tcp::no_delay option(true); 
        _sock->set_option(option, ec);
        post_recv_header();
    }
    
    time_point_t living_tick() {
        std::lock_guard<spinlock> lk(_lock);
        return _living_tick;
    }
    void post_recv_header() {
        auto self = this->shared_from_this();
        std::lock_guard<spinlock> lk(_lock);
        asio::async_read(*_sock, asio::buffer(_buff, HEADER_SIZE), 
            [this, self](const system::error_code& ec, std::size_t bytes_transferred){
                if (!ec && bytes_transferred > 0) {
                    post_recv_body();
                } else {
                    handle_error(ec);
                }
            });
    }
    void post_recv_body(){
        auto self = this->shared_from_this();
        uint16_t* plen = (uint16_t*)(_buff + LEN_FIELD_OFFSET);
        if (*plen > BUFF_SIZE) {
            close_sock(close_reason::packet_error);
            return;
        }
        std::lock_guard<spinlock> lk(_lock);
        asio::async_read(*_sock, asio::buffer(_buff + HEADER_SIZE, *plen - HEADER_SIZE), 
            [this, self](const system::error_code& ec, std::size_t bytes_transferred){
                if (!ec && bytes_transferred > 0) {
                    {
                        std::lock_guard<spinlock> lk(_lock);
                        _living_tick = std::chrono::steady_clock::now();
                    }
                    if (_handler->msg_handler(_sockid, _remote_addr, _remote_port, _buff, HEADER_SIZE + bytes_transferred, _data) == true) {
                        post_recv_header();
                    }
                } else {
                    handle_error(ec);
                }
            });
    }
    void post_send() {
        const char* data;
        uint32_t len;

        std::lock_guard<spinlock> lk(_lock);
        if (_sending == false) {
            if (_queue.pop(data, len)) {
                _sending = true;
                // 可以修改为scatter-gather io方式
                auto self = this->shared_from_this();
                asio::async_write(*_sock, asio::buffer(data, len), 
                    [this, self, data, len](const system::error_code& ec, std::size_t bytes_transferred){
                        {
                            std::lock_guard<spinlock> lk(_lock);
                            _sending = false;
                        }
                        
                        _handler->free(data, len);

                        if (!ec && bytes_transferred > 0) {
                            post_send();
                        } else {
                            handle_error(ec);
                        }
                    });
            } 
        }
    }

    void handle_error(const system::error_code& ec) {
        if (ec == asio::error::operation_aborted) {
            return;
        }
        if (ec == asio::error::eof) {
            close_sock(close_reason::close_by_peer);
        } else {
            close_sock(close_reason::io_error);
        }
    }
};

} // namespace xhb

#endif // XHBLIB_SRV_SRV_H_