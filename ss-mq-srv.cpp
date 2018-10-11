#include "http/httpd.hh"
#include "http/handlers.hh"
#include "http/function_handlers.hh"
#include "http/file_handler.hh"
#include "core/sleep.hh"
#include "core/sstring.hh"
#include "core/fstream.hh"
#include "core/shared_mutex.hh"
#include "core/thread.hh"

#include <iostream>
#include <memory>
#include <random>
#include <algorithm>
#include <chrono>
#include <list>
#include <fstream>

namespace bpo = boost::program_options;

namespace ss = seastar;
namespace sh = seastar::httpd;
using seastar::sstring;

class rnd {
    std::random_device _rd;
    std::mt19937 _gen;
    std::uniform_int_distribution<> _dis;
public:
    rnd(int min, int max) : _gen(_rd()), _dis(min, max){}
    ~rnd() = default;
    int operator()() { return _dis(_gen); }
};

thread_local rnd dura_rnder = {10, 20};
thread_local rnd mid_rander = {1, 1000};
thread_local rnd alloc_rander = {1024, 10240};
thread_local rnd cycle_rander = {10, 30};

struct msg {
    uint32_t _broad; // 是否是广播
    uint32_t _len;
    char _msg[1024];
};
// 消息队列
class mq {
public:
    uint32_t _id;
private:
    std::list<msg> _lst;
    std::chrono::time_point<std::chrono::steady_clock> _last_flush;
    ss::shared_mutex _mtx;
private:
    void flush_disk() {
        auto now = std::chrono::steady_clock::now();
        auto past = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_flush);
        if(_lst.size() > 0 && past.count() > 100) {
            return ss::with_lock(_mtx, [this]{
                auto beg = std::chrono::steady_clock::now();
                char path[256] = {};
                sprintf(path, "./ss-mq/%d.mq", _id);
                return ss::open_file_dma(path, ss::open_flags::wo | ss::open_flags::create).then([this, beg](ss::file f) {
                    return ss::do_with(ss::make_file_output_stream(f), _lst.begin(), [this,beg](auto& out, auto& it) {
                        return ss::repeat([this, &out, &it] {
                            if (it == _lst.end()) {
                                return ss::make_ready_future<ss::stop_iteration>(ss::stop_iteration::yes);
                            } else {
                                auto* p = reinterpret_cast<char*>(&(*it));
                                return out.write(p, sizeof(msg)).then([&it] { 
                                    ++it;
                                    return ss::stop_iteration::no; 
                                });
                            }
                        }).then([&out, this] { 
                            return out.close(); // 已报修复，增加flush_on_close flag或者增加flush_on_close()函数，避免写临时文件时频繁刷新硬盘
                        }).then([this, beg] {
                            _lst.clear(); 
                            _last_flush = std::chrono::steady_clock::now();
                  //          auto cost = std::chrono::duration_cast<std::chrono::milliseconds>(_last_flush - beg);
                    //        std::cout << "operate file cost " << cost.count() << "ms" << std::endl;
                        });
                    }); 
                });
            });
        }
        return ss::make_ready_future<>();
    }
public:
    mq() : _id(0) {}
    mq(uint32_t id) : _id(id), _last_flush(std::chrono::steady_clock::now()) {}
    ss::future<> push(msg m) {
        _lst.push_back(std::move(m));
        return ss::async( [this] {
            flush_disk().wait(); 
        });
    }
    ss::future<msg> pop() {
        auto m = _lst.front();
        _lst.pop_front();
        return ss::make_ready_future<msg>(std::move(m));
    }
    ss::future<> sim_cost_time() {
        for (auto i = 0; i < cycle_rander() - 1; i++) {
            auto len = alloc_rander();
            auto* buf = new char[len]{0};
            memset(buf, uint8_t(len), len);
            delete [] buf;
        }
        auto len = alloc_rander();
        auto* buf = new char[len]{0};
        memset(buf, uint8_t(len), len);
        msg m;
        m._len = std::min(int(sizeof(m._msg)), len);
        memcpy(m._msg, buf, m._len);
        delete [] buf;
        return push(m);
    }
};
class mqs {
    std::unordered_map<uint32_t, mq> _qs;
public:
    mq& get(uint32_t mid) {
        auto& q = _qs[mid];
        if (q._id == 0) q._id = mid;
        return q;
    }
    ss::future<> sim(uint32_t mid) {
        return get(mid).sim_cost_time();
    }
    ss::future<> stop() { return ss::make_ready_future<>(); }
};
uint32_t get_cpu(uint32_t mid) {
    return mid % ss::smp::count;
}
ss::distributed<mqs> smq;

ss::future<> sim_handle(uint32_t mid) {
    auto cpu = get_cpu(mid);
    return smq.invoke_on(cpu, &mqs::sim, mid);
}

class handl : public sh::handler_base {
public:
    virtual ss::future<std::unique_ptr<sh::reply>> handle(const sstring& path,
            std::unique_ptr<sh::request> req, std::unique_ptr<sh::reply> rep) override {
                auto dura = dura_rnder();  
                auto mid = mid_rander();   
                return sim_handle(mid).then([dura, rep = std::move(rep)] () mutable {
                    rep->_content = sstring(dura, 's');
                    rep->done("html");
                    return ss::make_ready_future<std::unique_ptr<sh::reply>>(std::move(rep));
                });
            }
};

void set_routes(sh::routes& r) {
    r.add(sh::operation_type::GET, sh::url("/"), new handl());
}

int main(int argc, char** argv) {
    ss::app_template app;
    app.add_options()("port", bpo::value<uint16_t>()->default_value(10000),
            "HTTP Server port");

    return app.run_deprecated(argc, argv, [&]{
        auto&& config = app.configuration();
        uint16_t port = config["port"].as<uint16_t>();
        auto server = new sh::http_server_control();
        smq.start().then([server] {
            ss::engine().at_exit([] { return smq.stop(); });
            return server->start();
        }).then([server] {
            return server->set_routes(set_routes);
        }).then([server, port] {
            return server->listen(port);
        }).then([server, port] {
            std::cout << "server listening on port: " << port << "..." << std::endl;
            ss::engine().at_exit([server] {
                return server->stop();
            });
        });
    });
}