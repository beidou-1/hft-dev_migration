#pragma once

#include <atomic>
#include <queue>
#include <functional>
#include <string_view>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

enum class Action : uint8_t { NONE, PING, PONG, CLOSE, RECEIVE };

class SendSizeQueue {
private:
    static constexpr size_t QUEUE_CAP = 256;
    size_t send_size_buf[QUEUE_CAP];
    size_t sq_head = 0;
    size_t sq_tail = 0;

public:
    void push(size_t n) { send_size_buf[sq_tail++ % QUEUE_CAP] = n; }
    size_t front() const { return send_size_buf[sq_head % QUEUE_CAP]; }
    void pop() { ++sq_head; }

    bool empty() const { return sq_head == sq_tail; }
    bool full() const { return (sq_tail - sq_head) == QUEUE_CAP; }
    void clear() { sq_head = sq_tail = 0; }
};

class Wss {
public:
    Wss() { m_buffer_send.reserve(1024 * 1024); }

    Action ping(std::string&& msg) {
        ping_message = std::move(msg);
        return Action::PING;
    }

    Action pong(std::string&& msg) {
        pong_message = std::move(msg);
        return Action::PONG;
    }

    void save(std::string_view msg) {
        size_t n = msg.size();
        auto buf = m_buffer_send.prepare(n);
        std::memcpy(buf.data(), msg.data(), n);
        m_buffer_send.commit(n);
        send_sizes.push(n);
    }

    void set_index(size_t index) { connection_index = index; }

    size_t get_index() const { return connection_index; }

    void reset() {
        m_writing = false;
        m_buffer_send.clear();
        send_sizes.clear();
        m_reading = false;
        m_buffer_recv.clear();
        ping_message.clear();
        pong_message.clear();
    }

public:
    bool m_writing = false;
    boost::beast::flat_buffer m_buffer_send;
    SendSizeQueue send_sizes;

    bool m_reading = false;
    boost::beast::multi_buffer m_buffer_recv;

    std::string ping_message;
    std::string pong_message;

    size_t connection_index = 0;
};

class WssHandler {
public:
    virtual ~WssHandler() = default;
    virtual Action on_connect(Wss* ws) = 0;
    virtual Action on_ping(Wss* ws, std::string_view payload) = 0;
    virtual Action on_pong(Wss* ws, std::string_view payload) = 0;
    virtual void on_close(Wss* ws) = 0;
    virtual void on_error(Wss* ws, std::string_view err) = 0;
    virtual Action on_message(Wss* ws, std::string_view msg) = 0;
};

template <class T> class WssClient {
private:
    net::io_context& m_ioc;
    ssl::context& m_ctx;
    T& m_handler;

    Wss wss;
    size_t n_callbacks{0};

    using Resolver = net::ip::basic_resolver<tcp, typename net::io_context::executor_type>;
    Resolver m_resolver;
    using Stream =
        websocket::stream<beast::ssl_stream<beast::basic_stream<tcp, typename net::io_context::executor_type>>>;
    Stream m_ws;

    websocket::permessage_deflate pmd_;
    websocket::stream_base::timeout op;

    // 重连相关
    net::steady_timer m_reconnect_timer;
    std::string conn_host_{};
    std::string conn_port_{};
    std::string conn_path_{};
    bool stopping_{false};

    net::steady_timer m_ping_timer;

    // 设置wss请求头
    std::function<void(websocket::request_type& req)> set_ws_header_cb;

public:
    WssClient(net::io_context& ioc, ssl::context& ctx, T& handler, bool permessage_deflate = false,
              bool auto_ping = true, size_t idle_timeout_seconds = 15, size_t handshake_timeout_seconds = 5)
        : m_ioc(ioc), m_ctx(ctx), m_handler(handler), m_resolver(ioc), m_ws(ioc, ctx), m_reconnect_timer(ioc),
          m_ping_timer(ioc), set_ws_header_cb(nullptr) {

        pmd_.client_enable = permessage_deflate;

        op.handshake_timeout = std::chrono::seconds(handshake_timeout_seconds);
        op.idle_timeout = std::chrono::seconds(idle_timeout_seconds);
        op.keep_alive_pings = auto_ping;
    }

    void resolve_connect(std::string_view host, std::string_view port, std::string_view path,
                         std::string_view from_ip = "0.0.0.0") {
        net::dispatch(m_ioc, [this, host, port, path, from_ip]() {
            bind_resolve_connect_(host, port, path, tcp::endpoint(net::ip::make_address(from_ip), 0));
        });
    }

    void resolve_reconnect(std::string_view host, std::string_view port, std::string_view path,
                           std::string_view from_ip = "0.0.0.0") {
        net::dispatch(m_ioc, [this, host, port, path, from_ip]() {
            m_ws.~Stream();
            new (&m_ws) Stream(m_ioc, m_ctx);
            wss.reset();
            n_callbacks = 0;
            bind_resolve_connect_(host, port, path, tcp::endpoint(net::ip::make_address(from_ip), 0));
        });
    }

    // NOTE：明确是单线程环境，去掉dispatch，提高性能
    void send(std::string_view content) {
        wss.save(content);
        send_();
    }

    char* get_buf() {
        auto buf = wss.m_buffer_send.prepare(512);
        return static_cast<char*>(buf.data());
    }

    void send_buf(size_t n) {
        wss.m_buffer_send.commit(n);
        wss.send_sizes.push(n);
        send_();
    }

    void receive() {
        net::dispatch(m_ioc, [this]() { this->receive_(); });
    }

    void close() {
        stopping_ = true;
        net::dispatch(m_ioc, [this]() { this->close_(); });
        m_ping_timer.cancel();
    }

    void start_ping_pong(const std::string& ping_msg, int seconds) {
        m_ping_timer.expires_after(std::chrono::seconds(seconds));
        m_ping_timer.async_wait([this, ping_msg, seconds](const boost::system::error_code& ec) {
            if (ec) {
                m_handler.on_error(&wss, ec.message() + ", line: " + std::to_string(__LINE__));
                return;
            }
            this->send(ping_msg);
            start_ping_pong(ping_msg, seconds);
        });
    }

    bool is_socket_open() { return m_ws.is_open(); }

    void enable_client_permessage_deflate() { pmd_.client_enable = true; }

    void disable_client_permessage_deflate() { pmd_.client_enable = false; }

    size_t get_user_data() { return wss.get_index(); }

    void set_user_data(size_t idx) { wss.set_index(idx); }

    void set_ws_header_field(std::function<void(websocket::request_type& req)> cb) { set_ws_header_cb = cb; }

private:
    void bind_resolve_connect_(std::string_view host, std::string_view port, std::string_view path,
                               net::ip::tcp::endpoint&& local) {
        if (bind_address_(local)) {
            conn_host_ = host;
            conn_port_ = port;
            conn_path_ = path;
            ++n_callbacks;
            m_resolver.async_resolve(
                host, port, [this, host, port, path](beast::error_code ec, tcp::resolver::results_type results) {
                    this->on_resolve(host, port, path, ec, results);
                });
        }
    }

    bool bind_address_(const net::ip::tcp::endpoint& local) {
        auto& socket = beast::get_lowest_layer(m_ws).socket();
        try {
            if (!socket.is_open())
                socket.open(local.protocol());
            socket.bind(local);
        } catch (const boost::system::system_error& e) {
            m_handler.on_error(&wss, std::to_string(__LINE__) + e.what());
            return false;
        }
        return true;
    }

    void start_reconnect_() {
        if (stopping_)
            return;

        ++n_callbacks;
        int delay_ms = 500;
        m_reconnect_timer.expires_after(std::chrono::milliseconds(delay_ms));
        m_reconnect_timer.async_wait([this](beast::error_code ec) {
            if (!ec && !stopping_) {
                try_reconnect_();
            }
        });
    }

    void try_reconnect_() {
        if (stopping_)
            return;

        if (this->is_socket_open()) {
            this->close_();
        }

        resolve_reconnect(conn_host_, conn_port_, conn_path_);
    }

    void process_error_(beast::error_code ec, const std::string& line) {
        m_handler.on_error(&wss, ec.message() + ", line: " + line);
        start_reconnect_();
    }

    void set_ws_option() {
        m_ws.set_option(pmd_);
        m_ws.set_option(op);
        if (op.keep_alive_pings) {
            m_ws.control_callback(std::function<void(websocket::frame_type, boost::beast::string_view)>(
                [this](websocket::frame_type ft, boost::beast::string_view payload) {
                    std::string_view std_sv(payload.data(), payload.size());
                    switch (ft) {
                        case websocket::frame_type::ping:
                            this->process_on_fun(this->m_handler.on_ping(&(this->wss), std_sv));
                            break;
                        case websocket::frame_type::pong:
                            this->process_on_fun(this->m_handler.on_pong(&(this->wss), std_sv));
                            break;
                        default:
                            break;
                    }
                }));
        }

        m_ws.set_option(websocket::stream_base::decorator([this](websocket::request_type& req) {
            req.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-async-ssl");
            if (set_ws_header_cb)
                set_ws_header_cb(req);
        }));

        beast::get_lowest_layer(m_ws).socket().set_option(tcp::no_delay(true));
    }

    void leave_callback() {
        --n_callbacks;
        if (n_callbacks == 0) {
            m_handler.on_close(&wss);
        }
    }

    void ping_() {
        ++n_callbacks;
        m_ws.async_ping({wss.ping_message.data(), wss.ping_message.size()},
                        [this](beast::error_code ec) { this->on_ping_sent(ec); });
    }

    void pong_() {
        ++n_callbacks;
        m_ws.async_pong({wss.pong_message.data(), wss.pong_message.size()},
                        [this](beast::error_code ec) { this->on_pong_sent(ec); });
    }

    void send_() {
        if (!wss.m_writing) [[likely]] {
            wss.m_writing = true;
            ++n_callbacks;
            m_ws.async_write(
                beast::buffers_prefix(wss.send_sizes.front(), wss.m_buffer_send.data()),
                [this](beast::error_code ec, std::size_t bytes_transferred) { this->on_write(ec, bytes_transferred); });
        }
    }

    void receive_() {
        if (!wss.m_reading) {
            wss.m_reading = true;
            ++n_callbacks;
            m_ws.async_read(wss.m_buffer_recv, [this](beast::error_code ec, std::size_t bytes_transferred) {
                this->on_read(ec, bytes_transferred);
            });
        }
    }

    void close_() {
        // Perform the close handshake
        ++n_callbacks;
        m_ws.async_close(websocket::close_code::normal, [this](beast::error_code ec) { this->on_close(ec); });
    }

    void process_on_fun(Action act) {
        switch (act) {
            case Action::NONE:
                break;
            case Action::PING: {
                ping_();
                break;
            }
            case Action::PONG: {
                pong_();
                break;
            }
            case Action::CLOSE: {
                close_();
                break;
            }
            case Action::RECEIVE: {
                receive_();
                break;
            }
        }
    }

    template <typename EndPoint>
    void direct_connect(EndPoint&& remote, std::string_view host_name, std::string_view port, std::string_view path) {
        // Set a timeout on the operation
        beast::get_lowest_layer(m_ws).expires_after(std::chrono::seconds(5));
        // Make the connection on the IP address we get from a lookup
        ++n_callbacks;
        beast::get_lowest_layer(m_ws).async_connect(
            std::forward<EndPoint>(remote),
            [this, host_name, port, path](beast::error_code ec) { this->on_connect(host_name, port, path, ec); });
    }

    void on_resolve(std::string_view host, std::string_view port, std::string_view path, beast::error_code ec,
                    const tcp::resolver::results_type& results) {
        if (ec) {
            m_handler.on_error(&wss, std::to_string(__LINE__) + ec.message());
        } else {
            direct_connect(*results.begin(), host, port, path);
        }
        leave_callback();
    }

    void on_connect(std::string_view host, std::string_view port, std::string_view path, beast::error_code ec) {
        if (ec) {
            process_error_(ec, std::to_string(__LINE__));
        } else {
            if (!SSL_set_tlsext_host_name(m_ws.next_layer().native_handle(), std::string(host).c_str())) {
                // beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
                m_handler.on_error(&wss, "Fail to set SNI Hostname");
            }

            ++n_callbacks;
            m_ws.next_layer().async_handshake(ssl::stream_base::client, [this, host, path](beast::error_code ec) {
                this->on_ssl_handshake(host, path, ec);
            });
        }
        leave_callback();
    }

    void on_ssl_handshake(std::string_view host, std::string_view path, beast::error_code ec) {
        if (ec) {
            process_error_(ec, std::to_string(__LINE__));
        } else {

            // Turn off the timeout on the tcp_stream, because
            // the websocket stream has its own timeout system.
            beast::get_lowest_layer(m_ws).expires_never();

            set_ws_option();

            ++n_callbacks;
            // m_ws.async_handshake(host, path, [this](beast::error_code ec) {this->on_handshake(ec);});
            m_ws.async_handshake(boost::beast::string_view(host.data(), host.size()),
                                 boost::beast::string_view(path.data(), path.size()),
                                 [this](beast::error_code ec) { this->on_handshake(ec); });
        }
        leave_callback();
    }

    void on_handshake(beast::error_code ec) {
        if (ec) {
            process_error_(ec, std::to_string(__LINE__));
        } else {
            process_on_fun(m_handler.on_connect(&wss));
            receive_();
        }
        leave_callback();
    }

    void on_write(beast::error_code ec, std::size_t bytes_transferred) {
        if (ec) [[unlikely]] {
            wss.m_writing = false;
            process_error_(ec, std::to_string(__LINE__));
        } else {
            wss.m_buffer_send.consume(wss.send_sizes.front());
            wss.send_sizes.pop();
            if (!wss.send_sizes.empty()) {
                ++n_callbacks;
                m_ws.async_write(beast::buffers_prefix(wss.send_sizes.front(), wss.m_buffer_send.data()),
                                 [this](beast::error_code ec, std::size_t bytes_transferred) {
                                     this->on_write(ec, bytes_transferred);
                                 });
            } else {
                wss.m_writing = false;
            }
        }
        leave_callback();
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        if (ec) [[unlikely]] {
            wss.m_reading = false;
            process_error_(ec, std::to_string(__LINE__));
        } else {
            process_on_fun(m_handler.on_message(&wss, beast::buffers_to_string(wss.m_buffer_recv.data())));

            wss.m_buffer_recv.consume(wss.m_buffer_recv.size());
            ++n_callbacks;
            m_ws.async_read(wss.m_buffer_recv, [this](beast::error_code ec, std::size_t bytes_transferred) {
                this->on_read(ec, bytes_transferred);
            });
        }
        leave_callback();
    }

    void on_close(beast::error_code ec) {
        if (ec) {
            process_error_(ec, std::to_string(__LINE__));
        } else {
            ++n_callbacks;
            m_ws.next_layer().async_shutdown([this](beast::error_code ec) { this->on_ssl_shutdown(ec); });
        }
        leave_callback();
    }

    void on_ssl_shutdown(beast::error_code ec) {
        if (ec) {
            m_handler.on_error(&wss, ec.message() + ", line: " + std::to_string(__LINE__));
        } else {
            beast::get_lowest_layer(m_ws).socket().shutdown(tcp::socket::shutdown_both, ec);
            if (ec) {
                m_handler.on_error(&wss, std::to_string(__LINE__) + ec.message());
            }
        }
        leave_callback();
    }

    void on_pong_sent(beast::error_code ec) {
        if (ec) {
            m_handler.on_error(&wss, ec.message() + ", line: " + std::to_string(__LINE__));
        }
        leave_callback();
    }

    void on_ping_sent(beast::error_code ec) {
        if (ec) {
            m_handler.on_error(&wss, ec.message() + ", line: " + std::to_string(__LINE__));
        }
        leave_callback();
    }
};
