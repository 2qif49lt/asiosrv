//
// Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: HTTP server, coroutine
//
//------------------------------------------------------------------------------
#define BOOST_COROUTINES_NO_DEPRECATION_WARNING

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/config.hpp>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <random>
#include <list>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <fstream>

using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
namespace http = boost::beast::http;    // from <boost/beast/http.hpp>

// Return a reasonable mime type based on the extension of a file.
boost::beast::string_view
mime_type(boost::beast::string_view path)
{
    using boost::beast::iequals;
    auto const ext = [&path]
    {
        auto const pos = path.rfind(".");
        if(pos == boost::beast::string_view::npos)
            return boost::beast::string_view{};
        return path.substr(pos);
    }();
    if(iequals(ext, ".htm"))  return "text/html";
    if(iequals(ext, ".html")) return "text/html";
    if(iequals(ext, ".php"))  return "text/html";
    if(iequals(ext, ".css"))  return "text/css";
    if(iequals(ext, ".txt"))  return "text/plain";
    if(iequals(ext, ".js"))   return "application/javascript";
    if(iequals(ext, ".json")) return "application/json";
    if(iequals(ext, ".xml"))  return "application/xml";
    if(iequals(ext, ".swf"))  return "application/x-shockwave-flash";
    if(iequals(ext, ".flv"))  return "video/x-flv";
    if(iequals(ext, ".png"))  return "image/png";
    if(iequals(ext, ".jpe"))  return "image/jpeg";
    if(iequals(ext, ".jpeg")) return "image/jpeg";
    if(iequals(ext, ".jpg"))  return "image/jpeg";
    if(iequals(ext, ".gif"))  return "image/gif";
    if(iequals(ext, ".bmp"))  return "image/bmp";
    if(iequals(ext, ".ico"))  return "image/vnd.microsoft.icon";
    if(iequals(ext, ".tiff")) return "image/tiff";
    if(iequals(ext, ".tif"))  return "image/tiff";
    if(iequals(ext, ".svg"))  return "image/svg+xml";
    if(iequals(ext, ".svgz")) return "image/svg+xml";
    return "application/text";
}

// Append an HTTP rel-path to a local filesystem path.
// The returned path is normalized for the platform.
std::string
path_cat(
    boost::beast::string_view base,
    boost::beast::string_view path)
{
    if(base.empty())
        return path.to_string();
    std::string result = base.to_string();
#if BOOST_MSVC
    char constexpr path_separator = '\\';
    if(result.back() == path_separator)
        result.resize(result.size() - 1);
    result.append(path.data(), path.size());
    for(auto& c : result)
        if(c == '/')
            c = path_separator;
#else
    char constexpr path_separator = '/';
    if(result.back() == path_separator)
        result.resize(result.size() - 1);
    result.append(path.data(), path.size());
#endif
    return result;
}

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
private:
    std::mutex _lock;
    std::list<msg> _lst;
    std::chrono::time_point<std::chrono::steady_clock> _last_flush;
public:
    uint32_t _id;
private:
    void flush_disk() {
        auto now = std::chrono::steady_clock::now();
        auto past = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_flush);
        if(_lst.size() > 0 && past.count() > 100) {
            std::ofstream of;
            char path[256] = {};
            sprintf(path, "./asio-mq/%d.mq", _id);
            of.open(path, std::ios_base::binary | std::ios_base::app);
            if (of.is_open() == false) {
                return;
            }
            for (auto m : _lst) {
                of.write(reinterpret_cast<char*>(&m), sizeof(m));
            }
            of.close();
            _lst.clear();
            _last_flush = std::chrono::steady_clock::now();
        }
    }
public:
    mq() : _id(0) {}
    mq(uint32_t id) : _id(id), _last_flush(std::chrono::steady_clock::now()) {}
    
    void push(msg m) {
        std::lock_guard<std::mutex> lock(_lock);
        _lst.push_back(std::move(m));
        flush_disk();
    }
    msg pop() {
        std::lock_guard<std::mutex> lock(_lock);
        auto m = _lst.front();
        _lst.pop_front();
        return m;
    }
    void sim_cost_time() {
        bool once = false;
        for (auto i = 0; i < cycle_rander(); i++) {
            auto len = alloc_rander();
            auto* buf = new char[len]{0};
            memset(buf, uint8_t(len), len);
            
            if (once == false) {
                once = true;
            
                msg m;
                m._len = std::min(int(sizeof(m._msg)), len);
                memcpy(m._msg, buf, m._len);
                push(m);
            }
            delete [] buf;
         }
    }
};
class mqs {
    std::mutex _lock;
    std::unordered_map<uint32_t, mq> _qs;
public:
    mq& get(uint32_t mid) {
        std::lock_guard<std::mutex> lock(_lock);
        auto& q = _qs[mid];
        if (q._id == 0) q._id = mid;
        return q;
    }
};

mqs qs;

// This function produces an HTTP response for the given
// request. The type of the response object depends on the
// contents of the request, so the interface requires the
// caller to pass a generic lambda for receiving the response.
template<
    class Body, class Allocator,
    class Send>
void
handle_request(
    boost::beast::string_view doc_root,
    http::request<Body, http::basic_fields<Allocator>>&& req,
    Send&& send,
    int content_length,
    int mid)
{
    // Returns a bad request response
    auto const bad_request =
    [&req](boost::beast::string_view why)
    {
        http::response<http::string_body> res{http::status::bad_request, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = why.to_string();
        res.prepare_payload();
        return res;
    };

    // Returns a not found response
    auto const not_found =
    [&req](boost::beast::string_view target)
    {
        http::response<http::string_body> res{http::status::not_found, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = "The resource '" + target.to_string() + "' was not found.";
        res.prepare_payload();
        return res;
    };

    // Returns a server error response
    auto const server_error =
    [&req](boost::beast::string_view what)
    {
        http::response<http::string_body> res{http::status::internal_server_error, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = "An error occurred: '" + what.to_string() + "'";
        res.prepare_payload();
        return res;
    };

    // Make sure we can handle the method
    if( req.method() != http::verb::get &&
        req.method() != http::verb::head)
        return send(bad_request("Unknown HTTP-method"));

    // Request path must be absolute and not contain "..".
    if( req.target().empty() ||
        req.target()[0] != '/' ||
        req.target().find("..") != boost::beast::string_view::npos)
        return send(bad_request("Illegal request-target"));

    // Build the path to the requested file
    std::string path = path_cat(doc_root, req.target());
    if(req.target().back() == '/')
        path.append("index.html");

    // Respond to HEAD request
    if(req.method() == http::verb::head)
    {
        http::response<http::empty_body> res{http::status::ok, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, mime_type(path));
        res.content_length(content_length);
        res.keep_alive(req.keep_alive());
        return send(std::move(res));
    }

    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(req.keep_alive());

    auto& q = qs.get(mid);
    q.sim_cost_time();
    res.body() = std::string(content_length, 's');
    res.prepare_payload();
    return send(std::move(res));
    
}

//------------------------------------------------------------------------------

// Report a failure
void
fail(boost::system::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

// This is the C++11 equivalent of a generic lambda.
// The function object is used to send an HTTP message.
template<class Stream>
struct send_lambda
{
    Stream& stream_;
    bool& close_;
    boost::system::error_code& ec_;
    boost::asio::yield_context yield_;

    explicit
    send_lambda(
        Stream& stream,
        bool& close,
        boost::system::error_code& ec,
        boost::asio::yield_context yield)
        : stream_(stream)
        , close_(close)
        , ec_(ec)
        , yield_(yield)
    {
    }

    template<bool isRequest, class Body, class Fields>
    void
    operator()(http::message<isRequest, Body, Fields>&& msg) const
    {
        // Determine if we should close the connection after
        close_ = msg.need_eof();

        // We need the serializer here because the serializer requires
        // a non-const file_body, and the message oriented version of
        // http::write only works with const messages.
        http::serializer<isRequest, Body, Fields> sr{msg};
        http::async_write(stream_, sr, yield_[ec_]);
    }
};

// Handles an HTTP server connection
void
do_session(
    tcp::socket& socket,
    std::shared_ptr<std::string const> const& doc_root,
    boost::asio::yield_context yield)
{
    bool close = false;
    boost::system::error_code ec;

    // This buffer is required to persist across reads
    boost::beast::flat_buffer buffer;

    // This lambda is used to send messages
    send_lambda<tcp::socket> lambda{socket, close, ec, yield};

    for(;;)
    {
        // Read a request
        http::request<http::string_body> req;
        http::async_read(socket, buffer, req, yield[ec]);
        if(ec == http::error::end_of_stream)
            break;
        if(ec)
            return fail(ec, "read");

        auto dura = dura_rnder();
        auto mid = mid_rander();

/*
        boost::asio::deadline_timer timer{socket.get_executor().context()};
        timer.expires_from_now( boost::posix_time::millisec(dura), ec); 
        timer.async_wait(yield[ec]);
*/
        
        // Send the response
        handle_request(*doc_root, std::move(req), lambda, dura, mid);
        if(ec)
            return fail(ec, "write");
        if(close)
        {
            // This means we should close the connection, usually because
            // the response indicated the "Connection: close" semantic.
            break;
        }
    }

    // Send a TCP shutdown
    socket.shutdown(tcp::socket::shutdown_send, ec);

    // At this point the connection is closed gracefully
}

//------------------------------------------------------------------------------

// Accepts incoming connections and launches the sessions
void
do_listen(
    boost::asio::io_context& ioc,
    tcp::endpoint endpoint,
    std::shared_ptr<std::string const> const& doc_root,
    boost::asio::yield_context yield)
{
    boost::system::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(ioc);
    acceptor.open(endpoint.protocol(), ec);
    if(ec)
        return fail(ec, "open");

    // Allow address reuse
    acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if(ec)
        return fail(ec, "set_option");

    // Bind to the server address
    acceptor.bind(endpoint, ec);
    if(ec)
        return fail(ec, "bind");

    // Start listening for connections
    acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if(ec)
        return fail(ec, "listen");

    for(;;)
    {
        tcp::socket socket(ioc);
        acceptor.async_accept(socket, yield[ec]);
        if(ec)
            fail(ec, "accept");
        else
            boost::asio::spawn(
                acceptor.get_executor().context(),
                std::bind(
                    &do_session,
                    std::move(socket),
                    doc_root,
                    std::placeholders::_1));
    }
}

int main(int argc, char* argv[])
{
    // Check command line arguments.
    if (argc != 2)
    {
        std::cerr <<
            "Usage: http-server-coro <port> \n" <<
            "Example:\n" <<
            "    http-server-coro 8080\n";
        return EXIT_FAILURE;
    }
    auto const address = boost::asio::ip::make_address("0.0.0.0");
    auto const port = static_cast<unsigned short>(std::atoi(argv[1]));
    auto const doc_root = std::make_shared<std::string>("/");
    auto const threads = std::thread::hardware_concurrency();


    // The io_context is required for all I/O
    boost::asio::io_context ioc(threads);

    // Spawn a listening port
    boost::asio::spawn(ioc,
        std::bind(
            &do_listen,
            std::ref(ioc),
            tcp::endpoint{address, port},
            doc_root,
            std::placeholders::_1));

    // Run the I/O service on the requested number of threads
    std::vector<std::thread> v;
    v.reserve(threads - 1);
    for(auto i = threads - 1; i > 0; --i)
        v.emplace_back(
        [&ioc]
        {
            ioc.run();
        });
    ioc.run();

    return EXIT_SUCCESS;
}

// g++ -std=c++14 asio.cpp -O2 -o asiosrv -lboost_system -lpthread -lboost_context -lboost_coroutine