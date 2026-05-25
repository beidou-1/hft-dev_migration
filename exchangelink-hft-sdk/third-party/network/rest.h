#pragma once

#include <string>
#include <memory>
#include <functional>
#include <string_view>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

namespace infra {
constexpr auto HTTP_STATUS_OK = boost::beast::http::status::ok;
constexpr auto HTTP_GET = boost::beast::http::verb::get;
constexpr auto HTTP_POST = boost::beast::http::verb::post;
constexpr auto HTTP_PUT = boost::beast::http::verb::put;
constexpr auto HTTP_DELETE = boost::beast::http::verb::delete_;

using HttpRequestBody = boost::beast::http::request<boost::beast::http::string_body>;
using HttpResponseBody = boost::beast::http::response<boost::beast::http::dynamic_body>;

// HttpRequestBody get_request_body(const std::string& host, const std::string& path, const std::string& query = "");
// HttpRequestBody get_request_body_by_post(const std::string& host, const std::string& path, const std::string& body);

inline HttpRequestBody get_request_body(const std::string& host, const std::string& path,
                                        const std::string& query = "") {
    std::string url = query.empty() ? path : (path + "?" + query);
    using namespace boost::beast;
    HttpRequestBody req{http::verb::get, url, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    return req;
}

inline HttpRequestBody get_request_body_by_post(const std::string& host, const std::string& path,
                                                const std::string& body) {
    using namespace boost::beast;
    HttpRequestBody req{http::verb::post, path, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::content_type, "application/json");
    req.body() = body;
    req.prepare_payload();
    return req;
}
} // namespace infra

class HttpClient {
public:
    HttpClient(net::io_context& ioc, ssl::context& ctx) : m_ioc(ioc), m_ctx(ctx) {};

    template <typename T, typename F>
    void send(T&& req, F&& callback, uint16_t port = 443, size_t connect_timeout_s = 3,
              std::string_view from_ip = "0.0.0.0", uint32_t timeout_sec = 60) {
        send(std::forward<T>(req), std::move(callback), tcp::endpoint(net::ip::address(), port),
             tcp::endpoint(net::ip::make_address(from_ip), 0), connect_timeout_s, timeout_sec);
    }

    template <typename T, typename F>
    void send(T&& req, F&& callback, net::ip::tcp::endpoint&& remote, net::ip::tcp::endpoint&& local,
              size_t connnect_timeout_s, size_t timeout_sec) {
        net::co_spawn(
            m_ioc,
            [req = std::forward<T>(req), remote = std::move(remote), local = std::move(local),
             callback = std::forward<F>(callback), connnect_timeout_s, timeout_sec, this]() -> net::awaitable<void> {
                co_await this->send_(req, remote, local, callback, connnect_timeout_s, timeout_sec);
            },
            [](std::exception_ptr e) {
                if (!e)
                    return;
                else
                    std::rethrow_exception(e);
            });
    }

    std::string sync_send(const http::request<http::string_body>& req, beast::error_code& ec) {
        ec.clear();
        tcp::resolver sync_resolver(m_ioc);
        Stream sync_stream(m_ioc, m_ctx);
        auto& sync_socket = beast::get_lowest_layer(sync_stream);

        std::string_view host{req[http::field::host].data(), req[http::field::host].size()};
        auto const results = sync_resolver.resolve(host, "443", ec);
        if (ec)
            return "Resolve error: " + ec.message();

        sync_socket.connect(results, ec);
        if (ec)
            return "Connect error: " + ec.message();

        if (!SSL_set_tlsext_host_name(sync_stream.native_handle(), std::string(host).c_str()))
            return "Fail to set SNI Hostname";

        sync_stream.handshake(ssl::stream_base::client, ec);
        if (ec)
            return "SSL handshake error: " + ec.message();

        http::write(sync_stream, req, ec);
        if (ec)
            return "Write error: " + ec.message();

        beast::flat_buffer buffer;
        http::response<http::dynamic_body> res;
        http::read(sync_stream, buffer, res, ec);
        if (ec)
            return "Read error: " + ec.message();

        sync_stream.shutdown(ec);
        if (ec) {
            if (ec != net::ssl::error::stream_truncated)
                return "SSL shutdown error: " + ec.message();
            ec = {};
        }

        sync_socket.close();
        return beast::buffers_to_string(res.body().data());
    }

private:
    using Stream = beast::ssl_stream<beast::basic_stream<tcp, typename net::io_context::executor_type>>;
    using Timer = net::basic_waitable_timer<std::chrono::steady_clock, net::wait_traits<std::chrono::steady_clock>,
                                            typename net::io_context::executor_type>;

    struct Connection {
        Stream http_;
        beast::multi_buffer buffer_;
        bool is_using = false;
        //        uint64_t active_time;

        Connection(net::io_context& ioc, ssl::context& ctx) : http_(ioc, ctx) {}
    };

    struct Resolver {
        net::ip::basic_resolver<tcp, typename net::io_context::executor_type> resolver;
        bool is_using = false;

        Resolver(net::io_context& ioc) : resolver(ioc) {}
    };

    net::io_context& m_ioc;
    ssl::context& m_ctx;
    std::vector<std::unique_ptr<Resolver>> resolvers;
    std::unordered_map<std::string, std::vector<std::unique_ptr<Connection>>> connection_pool;

    static void error_to_resp(http::response<http::dynamic_body>& resp, const beast::error_code& ec) {
        resp.result(http::status::unknown);
        auto& b = resp.body();
        std::string s = ec.message();
        size_t n = net::buffer_copy(b.prepare(s.size()), net::buffer(s));
        b.commit(n);
    }

    static bool is_same_ip(const net::ip::tcp::endpoint& socket, const net::ip::tcp::endpoint& required) {
        if (socket.protocol() != required.protocol())
            return false;
        else if (required.address().is_unspecified())
            return true;
        else
            return socket.address() == required.address();
    }

    template <typename T, typename F>
    net::awaitable<void> send_(T&& req, const net::ip::tcp::endpoint& remote, const net::ip::tcp::endpoint& local,
                               F callback, size_t connect_timeout, uint32_t timeout_sec) {
        beast::error_code ec;
        std::string_view host{req[http::field::host].data(), req[http::field::host].size()};
        std::string port_str = std::to_string(remote.port());
        std::string_view port{port_str};
        std::string key;
        key.reserve(host.size() + 1 + port_str.size());
        key.append(host).append(1, ':').append(port_str);

        if (auto it = connection_pool.find(key); it != connection_pool.end()) {
            std::vector<std::unique_ptr<Connection>>& conn_vec = it->second;
            size_t i = 0;
            for (; i < conn_vec.size(); ++i) {
                Connection& c = *(conn_vec[i]);
                if (c.is_using)
                    continue;
                auto& socket = beast::get_lowest_layer(c.http_).socket();
                auto remote_endpoint = socket.remote_endpoint(ec);
                c.is_using = true;
                if (ec) {
                    co_await connect_send(c, std::forward<T>(req), host, port, remote, local, callback, connect_timeout,
                                          ec, timeout_sec);
                    break;
                }
                auto local_endpoint = socket.local_endpoint(ec);
                if (ec) {
                    co_await connect_send(c, std::forward<T>(req), host, port, remote, local, callback, connect_timeout,
                                          ec, timeout_sec);
                    break;
                }
                if (is_same_ip(remote_endpoint, remote) && is_same_ip(local_endpoint, local)) {
                    http::response_parser<http::dynamic_body> resp;
                    resp.body_limit(boost::none);
                    co_await process_send(std::forward<T>(req), c, resp, ec, timeout_sec);
                    if (!ec) {
                        callback(resp.get());
                        c.buffer_.consume(c.buffer_.size());
                        // c.active_time = get_timestamp_ns();
                        c.is_using = false;
                    } else {
                        // logw("error from server: {}, time diff {}", ec.message(), get_timestamp_ns() -
                        // c.active_time);
                        ec.clear();
                        co_await connect_send(c, std::forward<T>(req), host, port, remote, local, callback,
                                              connect_timeout, ec, timeout_sec);
                    }
                    break;
                }
                c.is_using = false;
            }
            if (i == conn_vec.size()) {
                std::unique_ptr<Connection>& p_c = conn_vec.emplace_back(std::make_unique<Connection>(m_ioc, m_ctx));
                p_c->is_using = true;
                co_await connect_send(*p_c, std::forward<T>(req), host, port, remote, local, callback, connect_timeout,
                                      ec, timeout_sec);
            }
        } else {
            it = connection_pool.emplace(key, std::vector<std::unique_ptr<Connection>>()).first;
            std::vector<std::unique_ptr<Connection>>& conn_vec = it->second;
            std::unique_ptr<Connection>& p_c = conn_vec.emplace_back(std::make_unique<Connection>(m_ioc, m_ctx));
            p_c->is_using = true;
            co_await connect_send(*p_c, std::forward<T>(req), host, port, remote, local, callback, connect_timeout, ec,
                                  timeout_sec);
        }

        if (auto it = connection_pool.find(key); it != connection_pool.end()) {
            auto* p_c = &(it->second.back());
            while (!(beast::get_lowest_layer((*p_c)->http_).socket().is_open()) && !((*p_c)->is_using)) {
                it->second.pop_back();
                if (it->second.empty()) {
                    connection_pool.erase(key);
                    break;
                } else
                    p_c = &(it->second.back());
            }
        }
    }

    template <typename T, typename F>
    net::awaitable<void> connect_send(Connection& conn, T&& req, std::string_view host, std::string_view port,
                                      const net::ip::tcp::endpoint& remote, const net::ip::tcp::endpoint& local,
                                      F callback, size_t connect_timeout, beast::error_code& ec, uint32_t timeout_sec) {
        co_await connect_(conn, host, port, remote, local, connect_timeout, ec);
        if (ec) {
            http::response<http::dynamic_body> resp;
            error_to_resp(resp, ec);
            callback(resp);
        } else {
            http::response_parser<http::dynamic_body> resp;
            resp.body_limit(boost::none);
            co_await process_send(std::forward<T>(req), conn, resp, ec, timeout_sec);
            if (ec)
                error_to_resp(resp.get(), ec);
            // else conn.active_time = get_timestamp_ns();
            callback(resp.get());
            conn.buffer_.consume(conn.buffer_.size());
        }

        conn.is_using = false;
    }

    template <typename T>
    static net::awaitable<void> process_send(T&& req, Connection& conn, http::response_parser<http::dynamic_body>& resp,
                                             beast::error_code& ec, uint32_t timeout_sec) {
        beast::get_lowest_layer(conn.http_).expires_after(std::chrono::seconds(timeout_sec));
        co_await http::async_write(conn.http_, std::forward<T>(req), net::redirect_error(net::use_awaitable, ec));
        if (!ec)
            co_await http::async_read(conn.http_, conn.buffer_, resp, net::redirect_error(net::use_awaitable, ec));
    }

    net::awaitable<void> connect_(Connection& conn, std::string_view host, std::string_view port,
                                  const net::ip::tcp::endpoint& remote, const net::ip::tcp::endpoint& local,
                                  size_t connect_timeout, beast::error_code& ec) {
        // std::cout << "Connect "<< host <<std::endl;

        conn.http_.~Stream();
        new (&(conn.http_)) Stream(m_ioc, m_ctx);

        auto& socket = beast::get_lowest_layer(conn.http_).socket();
        try {
            if (!socket.is_open())
                socket.open(local.protocol());
            socket.bind(local);
        } catch (const boost::system::system_error& e) {
            ec.assign(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
            co_return;
        }

        if (remote.address().is_unspecified()) {
            Resolver* p_resolver = nullptr;
            for (auto& s : resolvers) {
                if (!s->is_using) {
                    p_resolver = s.get();
                    break;
                }
            }
            if (p_resolver == nullptr) {
                auto& resolver = resolvers.emplace_back(std::make_unique<Resolver>(m_ioc));
                p_resolver = resolver.get();
            }
            p_resolver->is_using = true;
            const auto results =
                co_await p_resolver->resolver.async_resolve(host, port, net::redirect_error(net::use_awaitable, ec));
            p_resolver->is_using = false;
            if (ec)
                co_return;
            beast::get_lowest_layer(conn.http_).expires_after(std::chrono::seconds(connect_timeout));
            co_await beast::get_lowest_layer(conn.http_)
                .async_connect(results, net::redirect_error(net::use_awaitable, ec));
        } else {
            beast::get_lowest_layer(conn.http_).expires_after(std::chrono::seconds(connect_timeout));
            co_await beast::get_lowest_layer(conn.http_)
                .async_connect(remote, net::redirect_error(net::use_awaitable, ec));
        }

        if (ec)
            co_return;

        if (!SSL_set_tlsext_host_name(conn.http_.native_handle(), std::string(host).c_str())) {
            ec.assign(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
            co_return;
        }

        co_await conn.http_.async_handshake(ssl::stream_base::client, net::redirect_error(net::use_awaitable, ec));
    }
};