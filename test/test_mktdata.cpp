#include <iostream>
#include <string>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <numeric>
#include <memory>
#include <atomic>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/post.hpp>

#include "common/factory.h"
#include "common/logger.h"
#include "latency_stats.h"
#include "test_common.h"

Timestamp g_last_update_milli = 0;
MktdataLatencyStats g_orderbook_stats_push;
MktdataLatencyStats g_orderbook_stats_recv;
MktdataLatencyStats g_orderbook_stats_parse;

std::atomic<bool> running{true};
static net::io_context g_ioc;

void test_mktdata(net::io_context& ioc, SpExchangeClient& client);
void print_stat(net::io_context& ioc);

Timestamp latency_ns(uint64_t end, uint64_t start) {
    if (end == 0 || start == 0) {
        return 0;
    }
    return tsc_to_ns(end - start);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <cpu_id>\n", argv[0]);
        return -1;
    }
    const int core = std::stoi(argv[1]);

    // 日志
    infra::init_logger("infra_test.run.log");

    // tsc
    infra::tsc_calibrate();
    const double cpu_ghz = infra::get_cpu_freq_ghz();
    INFRA_LOG_INFO("[main] CPU frequency (calibrated): {:.3f} GHz", cpu_ghz);

    // asio
    // net::io_context ioc;
    ssl::context ctx{ssl::context::sslv23_client};
    ctx.set_default_verify_paths();

    auto secret = get_api_credentials();
    auto config = create_default_config();
    auto client = ExchangeFactory::instance().create(g_ioc, ctx, secret, config);
    if (!client) {
        std::cout << "Failed to create client" << std::endl;
        std::exit(-1);
    }

    bool ret = client->initialize(false);
    if (!ret) {
        INFRA_LOG_WARN("[test] init client failed, call shutdown");
        client->shutdown();
        std::exit(-1);
    }

    Symbols fetch_pairs = get_test_symbols();
    net::post(g_ioc, [&client, fetch_pairs]() {
        // case 1: 获取交易对基础信息
        client->fetch_pairs_info([fetch_pairs](Errno ec, const UMSymbolExInfo& ob) {
            if (ec != Errno::Ok) {
                INFRA_LOG_WARN("fetch_pairs_info failed, because: {}", to_string(ec));
                return;
            }

            for (auto pair : fetch_pairs) {
                auto it = ob.find(pair);
                if (it != ob.end()) {
                    auto info = it->second;
                    INFRA_LOG_INFO("[test] fetch_pairs_info result: {}", info->to_json());
                } else {
                    INFRA_LOG_WARN("not found pair {} in pairs_info", pair);
                }
            }
        });

        auto timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(3));
        timer->async_wait([timer, &client](const boost::system::error_code& ec) { test_mktdata(g_ioc, client); });
    });

    // ioc.run();
    // 绑核
    // int core = 1;
    try {
        infra::bind_cpu(core);
        INFRA_LOG_INFO("[test] bind cpu {} successfully", core);
    } catch (const std::exception& ex) {
        INFRA_LOG_WARN("[test] bind cpu {} failed, {}", core, ex.what());
        std::exit(-1);
    }

    while (running) {
        g_ioc.poll();
    }
    return 0;
}

void test_mktdata(net::io_context& ioc, SpExchangeClient& client) {
    Symbols test_pairs = {"btc-usdt"};
    bool ret = client->subscribe_orderbook(test_pairs, 1, [](SpOrderBook ob) {
        if (g_last_update_milli == 0) {
            g_last_update_milli = ob->update_milli;
            return;
        }

        // 推送延迟
        Timestamp latency_a = ob->update_milli - g_last_update_milli;
        g_last_update_milli = ob->update_milli;
        g_orderbook_stats_push.add(latency_a, true);

        // 接收延迟
        Timestamp latency_b = ob->recv_milli - ob->update_milli;
        g_orderbook_stats_recv.add(latency_b, true);

        // 解析延迟
        Timestamp latency_c = latency_ns(ob->parsed_tsc, ob->recv_tsc);
        g_orderbook_stats_parse.add(latency_c, true);

        static int cnt = 0;
        cnt++;
        if (cnt % 100 == 0) {
            ob->print();
        }
    });

    if (!ret) {
        INFRA_LOG_WARN("subscribe_orderbook failed");
        return;
    }

    // 24小时之后取消订阅
    auto unsub_ob_timer = std::make_shared<net::steady_timer>(ioc, std::chrono::hours(24));
    unsub_ob_timer->async_wait([unsub_ob_timer, &client](const boost::system::error_code& ec) {
        INFRA_LOG_INFO("call unsubscribe_orderbook");
        client->unsubscribe_orderbook();
    });

    // 统计输出
    print_stat(ioc);
}

void print_stat(net::io_context& ioc) {
    // 每5分钟输出一次统计信息
    auto timer = std::make_shared<net::steady_timer>(ioc, std::chrono::minutes(5));
    timer->async_wait([&ioc, timer](const boost::system::error_code& ec) {
        if (!ec) {
            g_orderbook_stats_push.print("g_orderbook_stats_push latency");
            g_orderbook_stats_recv.print("g_orderbook_stats_recv latency");
            g_orderbook_stats_parse.print("g_orderbook_stats_parse latency");
            print_stat(ioc);
        }
    });
}
