#include <csignal>
#include <cstdlib>
#include <atomic>
#include <iostream>
#include <execinfo.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/post.hpp>

#include "common/logger.h"
#include "common/factory.h"
#include "test_common.h"
#include "latency_stats.h"

namespace asio = boost::asio;

static net::io_context g_ioc;
static std::shared_ptr<asio::steady_timer> g_shutdown_timer;
static std::atomic<bool> g_running{true};

// 统计指标
static LatencyStats g_exchange_to_local;
static LatencyStats g_ob_recv_to_parsed;
static LatencyStats g_send_to_sent;
static LatencyStats g_sent_to_ack;

std::unordered_map<std::string, SpOrder> g_order_cache_;
UMSymbolExInfo g_symbol_info;

void run_test(net::io_context& ioc, SpExchangeClient& client);

Timestamp latency_ns(uint64_t end, uint64_t start) {
    if (end == 0 || start == 0) {
        return 0;
    }
    return tsc_to_ns(end - start);
}

static void signal_handler(int sig) {
    static bool handling = false;
    if (handling) {
        ::exit(-1);
    }
    handling = true;

    if (sig == SIGSEGV || sig == SIGABRT) {
        void* frames[32];
        const int n = ::backtrace(frames, 32);
        INFRA_LOG_ERROR("signal {} received, backtrace:", sig);
        ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
    } else {
        INFRA_LOG_INFO("signal {} received, graceful shutdown", sig);
    }

    g_running = false;
    g_shutdown_timer = std::make_shared<asio::steady_timer>(g_ioc, std::chrono::seconds(5));
    g_shutdown_timer->async_wait([](const boost::system::error_code&) {
        INFRA_LOG_ERROR("signal handler: exiting");
        ::exit(0);
    });
}

int main(int argc, char* argv[]) {
    const int core = (argc > 1) ? std::stoi(argv[1]) : 0;

    // ── 1. 日志 ──────────────────────────────────────────────
    infra::init_logger("perf.run.log");

    // ── 2. tsc 标定 + CPU 频率 ────────────────────────────────
    infra::tsc_calibrate();
    const double cpu_ghz = infra::get_cpu_freq_ghz();
    INFRA_LOG_INFO("[main] CPU frequency (calibrated): {:.3f} GHz", cpu_ghz);

    // ── 3. 绑核 ──────────────────────────────────────────────
    try {
        infra::bind_cpu(core);
        INFRA_LOG_INFO("[main] bind cpu {} successfully", core);
    } catch (const std::exception& ex) {
        INFRA_LOG_ERROR("[main] bind cpu {} failed, {}", core, ex.what());
        return -1;
    }

    // ── 4. 信号处理 ───────────────────────────────────────────
    std::signal(SIGSEGV, signal_handler);
    std::signal(SIGABRT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    // ── 5. SSL context ────────────────────────────────────────
    ssl::context ctx{ssl::context::sslv23_client};
    ctx.set_default_verify_paths();

    // ── 6. infra性能测试代码 ───────────────────────────────────────────
    auto secret = get_api_credentials();
    auto config = create_default_config();
    auto client = ExchangeFactory::instance().create(g_ioc, ctx, secret, config);
    if (!client) {
        INFRA_LOG_ERROR("Failed to create client");
        return -1;
    }

    bool ret = client->initialize();
    if (!ret) {
        INFRA_LOG_ERROR("init client failed, call shutdown");
        client->shutdown();
        return -1;
    }

    Symbols test_pairs = get_test_symbols();
    net::post(g_ioc, [&client, test_pairs]() {
        // case 1: 获取交易对基础信息
        client->fetch_pairs_info([test_pairs](Errno ec, const UMSymbolExInfo& ob) {
            if (ec != Errno::Ok) {
                INFRA_LOG_WARN("[main] fetch_pairs_info failed, because: {}", to_string(ec));
                return;
            }

            g_symbol_info = ob;
            for (auto pair : test_pairs) {
                auto it = ob.find(pair);
                if (it != ob.end()) {
                    auto info = it->second;
                    INFRA_LOG_INFO("[main] fetch_pairs_info result: {}", info->to_json());
                } else {
                    INFRA_LOG_WARN("[main] not found pair {} in pairs_info", pair);
                }
            }
        });

        auto timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(3));
        timer->async_wait([timer, &client](const boost::system::error_code& ec) { run_test(g_ioc, client); });
    });

    // ── 7. 事件循环 ──────────────────────────────────────────────
    while (g_running) {
        g_ioc.poll();
    }
    return 0;
}

void run_test(net::io_context& ioc, SpExchangeClient& client) {
    Symbols test_pairs = get_test_symbols();

    // case 2：订阅1档行情
    bool ret_sub = client->subscribe_orderbook(test_pairs, 1, [&client](SpOrderBook ob) {
        // 计算行情延迟
        Timestamp latency_a = (ob->recv_milli - ob->update_milli) * 1000; // ms转us
        g_exchange_to_local.add(latency_a);

        // 计算解析延迟
        Timestamp latency_b = latency_ns(ob->parsed_tsc, ob->recv_tsc);
        g_ob_recv_to_parsed.add(latency_b);

        static int order_cnt = 0;
        static int kMaxOrders = 1500;
        static uint64_t last_order_tsc = 0;
        constexpr uint64_t kMinIntervalNs = 5'000'000'000ULL; // 每5s下一单

        if (order_cnt < kMaxOrders && tsc_to_ns(infra::rdtsc() - last_order_tsc) >= kMinIntervalNs) {
            last_order_tsc = infra::rdtsc();
            auto t_order = std::make_shared<Order>();
            t_order->pair = ob->pair;
            t_order->client_oid = std::to_string(time_get_now_micro());
            t_order->side = OrderSide::OpenShort;
            t_order->type = OrderType::Limit;
            t_order->tif = OrderTIF::IOC;
            t_order->price = ob->ask_price + 123.456;
            t_order->quantity = g_symbol_info[ob->pair]->step_size_base * 2.1;
            g_order_cache_[t_order->client_oid] = t_order;

            // 下单
            t_order->latency->master_order.send_tsc = infra::rdtsc();
            client->place_order(t_order, [](Errno err, SpOrder result) {
                auto it = g_order_cache_.find(result->client_oid);
                if (it != g_order_cache_.end()) {
                    auto& local = it->second;
                    local->update(*result);
                    if (local->latency->master_order.ack_tsc == 0) {
                        // 计算请求-响应延迟
                        local->latency->master_order.ack_tsc = infra::rdtsc();
                        Timestamp latency_d =
                            latency_ns(local->latency->master_order.ack_tsc, local->latency->master_order.sent_tsc);
                        latency_d = latency_d / 1'000; // ns转us
                        g_sent_to_ack.add(latency_d);
                    }
                    g_order_cache_.erase(it);
                }

                if (err != Errno::Ok) {
                    INFRA_LOG_WARN("place_order callback failed, because: {}, {}, {}", to_string(err),
                                   to_string(result->ec), result->detail);
                } else {
                    INFRA_LOG_INFO("place_order callback result: {}", to_string(result->ec));
                }
            });
            t_order->latency->master_order.sent_tsc = infra::rdtsc();

            // 计算下单延迟
            Timestamp latency_c =
                latency_ns(t_order->latency->master_order.sent_tsc, t_order->latency->master_order.send_tsc);
            g_send_to_sent.add(latency_c);

            // 每5分钟输出1次延迟统计结果
            order_cnt++;
            if (order_cnt % 50 == 0) {
                g_exchange_to_local.print("Exchange To Local", "us");
                g_ob_recv_to_parsed.print("Ob Recv To Parsed", "ns");
                g_send_to_sent.print("Send To Sent", "ns");
                g_sent_to_ack.print("Sent To Order Ack", "us");
            }

            if (order_cnt >= kMaxOrders) {
                INFRA_LOG_INFO("reach max orders, call shutdown");
                client->shutdown();
                g_running = false;
            }
        }
    });
    if (!ret_sub) {
        INFRA_LOG_WARN("[test] subscribe_orderbook failed");
    }

    bool ret_so = client->subscribe_order([](Errno ec, SpOrder result) {});
    if (!ret_so) {
        INFRA_LOG_WARN("[test] subscribe_order failed");
    }
}
