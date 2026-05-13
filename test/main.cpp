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

#include "common/factory.h"
#include "common/logger.h"
#include "latency_stats.h"
#include "test_common.h"

Timestamp g_last_update_milli = 0;
// static MktdataLatencyStats g_orderbook_stats_push;
static MktdataLatencyStats g_orderbook_stats_recv;
static MktdataLatencyStats g_orderbook_stats_parse;

// static MktdataLatencyStats g_place_ws_convert;
static MktdataLatencyStats g_place_ws_send;
static MktdataLatencyStats g_place_ws_response;

std::unordered_map<std::string, SpOrder> g_order_cache_;

std::atomic<bool> running{true};
static net::io_context g_ioc;

void run_test(net::io_context& ioc, SpExchangeClient& client);
void test_trading(net::io_context& ioc, SpExchangeClient& client, const Symbol& test_pair);

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
    // net::io_context g_ioc;
    ssl::context ctx{ssl::context::sslv23_client};
    ctx.set_default_verify_paths();

    auto secret = get_api_credentials();
    auto config = create_default_config();
    auto client = ExchangeFactory::instance().create(g_ioc, ctx, secret, config);
    if (!client) {
        std::cout << "Failed to create client" << std::endl;
        std::exit(-1);
    }

    bool ret = client->initialize();
    if (!ret) {
        INFRA_LOG_WARN("[test] init client failed, call shutdown");
        client->shutdown();
        std::exit(-1);
    }

    Symbols test_pairs = get_test_symbols();
    net::post(g_ioc, [&client, test_pairs]() {
        // case 1: 获取交易对基础信息
        client->fetch_pairs_info([test_pairs](Errno ec, const UMSymbolExInfo& ob) {
            if (ec != Errno::Ok) {
                INFRA_LOG_WARN("[test] fetch_pairs_info failed, because: {}", to_string(ec));
                return;
            }

            for (auto pair : test_pairs) {
                auto it = ob.find(pair);
                if (it != ob.end()) {
                    auto info = it->second;
                    INFRA_LOG_INFO("[test] fetch_pairs_info result: {}", info->to_json());
                } else {
                    INFRA_LOG_WARN("[test] not found pair {} in pairs_info", pair);
                }
            }
        });

        auto timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(3));
        timer->async_wait([timer, &client](const boost::system::error_code& ec) { run_test(g_ioc, client); });
    });

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

    // 后续改成链式异步调用
    // init_future.then([&g_ioc, init_promise](boost::future<bool>) {
    //     auto timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(5));
    //     timer->async_wait([timer](const boost::system::error_code& ec) { INFRA_LOG_INFO("[test] timer callback"); });
    // });

    // auto shutdown_timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(900));
    // shutdown_timer->async_wait([shutdown_timer, &client](const boost::system::error_code& ec) {
    //     INFRA_LOG_INFO("[test] call shutdown");
    //     client->shutdown();
    // });
    return 0;
}

void run_test(net::io_context& ioc, SpExchangeClient& client) {
    Symbol test_pair = get_primary_test_symbol();
    Symbols test_pairs = get_test_symbols();
    Currency test_currency = get_test_currency();

#if 0
    // case 3：订阅1档行情
    bool ret_sub = client->subscribe_orderbook({}, 1, [&client](SpOrderBook ob) {
        // 接收延迟
        Timestamp latency_b = ob->recv_milli - ob->update_milli;
        latency_b = latency_b * 1000; // ms转成us
        g_orderbook_stats_recv.add(latency_b, true);

        // 解析延迟
        Timestamp latency_c = latency_ns(ob->parsed_tsc, ob->recv_tsc);
        g_orderbook_stats_parse.add(latency_c, true);

        static int cnt = 0;
        cnt++;
        if (cnt % 5000 == 0) {
            ob->print();
        }

        // 自动选取30个币，总共下1万次IOC单
        // static int order_cnt = 0;
        // static int kMaxOrders = 500;
        // static uint64_t last_order_tsc = 0;
        // constexpr uint64_t kMinIntervalNs = 5000'000'000ULL; // 每5s下一单
        // if (order_cnt < kMaxOrders && tsc_to_ns(rdtsc() - last_order_tsc) >= kMinIntervalNs) {
        //     last_order_tsc = rdtsc();
        //     auto aa_order = std::make_shared<Order>();

        //     aa_order->pair = ob->pair;
        //     aa_order->client_oid = std::to_string(time_get_now_micro());
        //     aa_order->side = OrderSide::OpenShort;
        //     aa_order->type = OrderType::Limit;
        //     aa_order->tif = OrderTIF::IOC;
        //     aa_order->price = ob->ask_price + 1.234;
        //     aa_order->quantity = 11.23;
        //     aa_order->latency->master_order.send_tsc = rdtsc();
        //     client->place_order(aa_order, [](Errno err, SpOrder result) {
        //         if (err != Errno::Ok) {
        //             INFRA_LOG_WARN("place_order callback failed, because: {}, {}, {}", to_string(err),
        //                            to_string(result->ec), result->detail);
        //         } else {
        //             INFRA_LOG_INFO("place_order callback result: {}", to_string(result->ec));
        //         }

        //         // 计算请求-响应延迟
        //         auto it = g_order_cache_.find(result->client_oid);
        //         if (it != g_order_cache_.end()) {
        //             SpOrder local = it->second;
        //             local->update(*result);
        //             if (local->latency->master_order.ack_tsc == 0) {
        //                 local->latency->master_order.ack_tsc = infra::rdtsc();
        //                 Timestamp latency_d =
        //                     latency_ns(local->latency->master_order.ack_tsc, local->latency->master_order.sent_tsc);
        //                 latency_d = latency_d / 1'000; // ns转成us
        //                 g_place_ws_response.add(latency_d, true);
        //             }
        //             g_order_cache_.erase(it);
        //         }
        //     });
        //     aa_order->latency->master_order.sent_tsc = rdtsc();
        //     g_order_cache_[aa_order->client_oid] = aa_order;
        //     order_cnt++;
        //     // INFRA_LOG_INFO("place_order Latency: {} {} ns, total: {} ns", latency_a, latency_b, latency_c);
        //     // g_place_ws_convert.add(latency_a, true);
        //     Timestamp latency_c =
        //         latency_ns(aa_order->latency->master_order.sent_tsc, aa_order->latency->master_order.send_tsc);
        //     g_place_ws_send.add(latency_c, true);
        //     if (order_cnt % 20 == 0) {
        //         // g_orderbook_stats_push.print("orderbook push latency", "ms");
        //         g_orderbook_stats_recv.print("Exchange To Local latency", "us");
        //         g_orderbook_stats_parse.print("Ob Recv To Parsed latency", "ns");
        //         // g_place_ws_convert.print("Send To Serial latency", "ns");
        //         g_place_ws_send.print("Send To Sent latency", "ns");
        //         g_place_ws_response.print("Sent To Order Ack latency", "us");
        //     }
        //     if (order_cnt == kMaxOrders) {
        //         INFRA_LOG_INFO("reach max orders, call shutdown");
        //         client->shutdown();
        //     }
        // }
    });
    if (!ret_sub) {
        INFRA_LOG_WARN("[test] subscribe_orderbook failed");
    }

    // case 4: 测试取消订单簿行情订阅并获取本地缓存的订单簿数据
    // auto unsub_market_timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(900));
    // unsub_market_timer->async_wait([unsub_market_timer, test_pairs,
    // &client](const boost::system::error_code& ec) {
    //     INFRA_LOG_INFO("call unsubscribe_orderbook");
    //     client->unsubscribe_orderbook();

    //     INFRA_LOG_INFO("call get_orderbook");
    //     for (auto pair_ob : test_pairs) {
    //         SpOrderBook ob = client->get_orderbook(pair_ob);
    //         if (!ob) {
    //             INFRA_LOG_WARN("get_orderbook failed: {}", pair_ob);
    //         } else {
    //             ob->print();
    //         }
    //     }
    // });
#endif

#if 0
    // case 5: 测试异步获取指定币种的余额信息
    client->get_balance(test_currency, [](Errno ec, const UMCurrencyBalance& ob) {
        if (ec != Errno::Ok) {
            INFRA_LOG_WARN("[test] get_balance failed, because: {}", to_string(ec));
        } else {
            for (auto& [pair, sBalance] : ob) {
                INFRA_LOG_INFO("[test] get_balance, result: {} ", sBalance->to_json());
            }
        }
    });

    // case 6: 测试异步获取指定交易对的持仓信息
    client->get_position(test_pair, [](Errno ec, const UMSymbolPosition& ob) {
        if (ec != Errno::Ok) {
            INFRA_LOG_WARN("[test] get_position failed, because: {}", to_string(ec));
        } else {
            for (auto& [pair, sPosition] : ob) {
                INFRA_LOG_INFO("[test] get_position, result:{}", sPosition->to_json());
            }
        }
    });

    // case 6: 测试异步获取指定交易对的持仓信息
    client->get_margin_ratio([](Errno ec, const double& ratio) {
        if (ec != Errno::Ok) {
            INFRA_LOG_WARN("[test] get_margin_ratio failed, because: {}", to_string(ec));
        } else {
            INFRA_LOG_INFO("[test] get_margin_ratio, result:{}", ratio);
        }
    });

    // case 7: 杠杆倍数设置测试
    client->set_leverage(test_pair, 8, MarginMode::CROSS, [](Errno ec) {
        if (ec != Errno::Ok) {
            INFRA_LOG_WARN("[test] set_leverage failed, because: {}", to_string(ec));
        } else {
            INFRA_LOG_INFO("[test] set_leverage, success");
        }
    });

#endif

#if 1
    // case 9: 订阅订单状态变化推送，实时接收订单更新信息
    bool bre_o = client->subscribe_order([](Errno ec, SpOrder result) {
        if (ec != Errno::Ok) {
            INFRA_LOG_WARN("subscribe_order callback, because: {}, {}, {}", to_string(ec), to_string(result->ec),
                           result->detail);
        } else {
            INFRA_LOG_INFO("subscribe_order callback, result: {}", result->to_json());
        }
    });
    
    if (!bre_o) {
        INFRA_LOG_WARN("subscribe_order failed");
    }

    // case 10: 取消订阅
    // auto unsub_stream_timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(600));
    // unsub_stream_timer->async_wait([unsub_stream_timer, &client](const boost::system::error_code& ec) {
    //     INFRA_LOG_INFO("call unsubscribe_order");
    //     client->unsubscribe_order();
    //     // INFRA_LOG_INFO("call unsubscribe_trade");
    //     // client->unsubscribe_trade();
    // });

    // 延迟3秒后开始执行交易相关的测试用例
    auto trade_test_timer = std::make_shared<net::steady_timer>(ioc, std::chrono::seconds(3));
    trade_test_timer->async_wait([trade_test_timer, &ioc, &client, test_pair](const boost::system::error_code& ec) {
        test_trading(ioc, client, test_pair);
    });
#endif
}

void test_trading(net::io_context& ioc, SpExchangeClient& client, const Symbol& test_pair) {
#if 0
    // case 15: 测试限价的报单撤单，关注订单状态变化（CREATED -> NEW -> CANCELING -> CANCELED）
    for (int i = 1; i <= 2; i++) {
        auto timer = std::make_shared<net::steady_timer>(ioc, std::chrono::milliseconds(50 * i));
        timer->async_wait([timer, &client, test_pair, i](const boost::system::error_code& ec) {
            auto aa_order = std::make_shared<Order>();
            aa_order->pair = test_pair;
            aa_order->client_oid = std::to_string(time_get_now_micro());
            aa_order->type = OrderType::Limit;
            aa_order->tif = OrderTIF::IOC;
            aa_order->price = 1.123;
            aa_order->quantity = 11.23;
            aa_order->latency->master_order.send_tsc = rdtsc();
            client->place_order(aa_order, [](Errno err, SpOrder result) {
                if (err != Errno::Ok) {
                    INFRA_LOG_WARN("place_order callback failed, because: {}, {}, {}", to_string(err),
                                   to_string(result->ec), result->detail);
                } else {
                    INFRA_LOG_INFO("place_order callback result: {}", result->to_json());
                }
            });
            aa_order->latency->master_order.sent_tsc = rdtsc();
            Timestamp latency_a =
                latency_ns(aa_order->latency->master_order.serial_tsc, aa_order->latency->master_order.send_tsc);
            Timestamp latency_d =
                latency_ns(aa_order->latency->master_order.sent_tsc, aa_order->latency->master_order.serial_tsc);
            INFRA_LOG_INFO("place_order Latency: {} {} ns", latency_a, latency_d);
            // g_place_ws_convert.add(latency_a, true);
            // g_place_ws_send.add(latency_d, true);
            // if (i > 15) {
            //     g_place_ws_convert.print("g_place_ws_convert latency");
            //     g_place_ws_send.print("g_place_ws_send latency");
            // }
        });
    }
#endif

#if 0
    // case 16: 测试市价单的开多仓
    auto market_buy_timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(5));
    market_buy_timer->async_wait([market_buy_timer, &g_ioc, &client, test_pair](const boost::system::error_code& ec) {
        auto market_buy = std::make_shared<Order>();
        market_buy->pair = test_pair;
        market_buy->client_oid = std::to_string(time_get_now_milli());
        market_buy->type = OrderType::Market;
        market_buy->side = OrderSide::OpenShort;
        market_buy->price = 1.4321;
        market_buy->quantity = 20.3;

        INFRA_LOG_INFO("Testing market buy order: {}", market_buy->client_oid);
        client->place_order(market_buy, [&g_ioc, &client, test_pair](Errno err, SpOrder result) {
            if (err != Errno::Ok) {
                INFRA_LOG_WARN("place_order callback failed, because: {}, {}, {}", to_string(err),
                               to_string(result->ec), result->detail);
            } else {
                INFRA_LOG_INFO("place_order callback, Market buy result, order: {}", result->to_json());

                // case : 查询
                auto timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(2));
                timer->async_wait([timer, &client, result, test_pair](const boost::system::error_code& ec) {
                    client->query_order(result, [result](Errno err, SpOrder query_result) {
                        if (err != Errno::Ok) {
                            INFRA_LOG_WARN("query_order callback failed, because: {}, {}, {}", to_string(err),
                                           to_string(query_result->ec), query_result->detail);
                        } else {
                            INFRA_LOG_INFO("query_order callback, query_result: {}", query_result->to_json());
                        }
                    });
                });

                // case : 平仓，关注reduce_only字段
                auto close_timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(4));
                close_timer->async_wait([close_timer, &client, test_pair](const boost::system::error_code& ec) {
                    auto market_close = std::make_shared<Order>();
                    market_close->pair = test_pair;
                    market_close->client_oid = std::to_string(time_get_now_milli());
                    market_close->type = OrderType::Market;
                    market_close->side = OrderSide::CloseShort;
                    market_close->price = 1.441;
                    market_close->quantity = 25;

                    // INFRA_LOG_INFO("Testing market close order: {}", market_close->client_oid);
                    client->place_order(market_close, [](Errno err, SpOrder close_result) {
                        if (err != Errno::Ok) {
                            INFRA_LOG_WARN("place_order callback failed, Market close, because: {}, {}, {}",
                                           to_string(err), to_string(close_result->ec), close_result->detail);
                        } else {
                            INFRA_LOG_INFO("place_order callback, Market close, close_result: {}",
                                           close_result->to_json());
                        }
                    });
                });
            } // if
        });
    }); //  market_buy_timer
#endif

#if 0
    // case 19: 测试IOC、FOK、MAKER等类型
    struct TifTest {
        OrderTIF tif;
        std::string name;
        int delay;
    };

    std::vector<TifTest> tif_tests = {
        {OrderTIF::IOC, "IOC", 10}, {OrderTIF::FOK, "FOK", 12}, {OrderTIF::MAKER, "MAKER", 14}};

    // 调整价格，分别测试成功和失败场景
    for (const auto& test : tif_tests) {
        auto timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(test.delay));
        timer->async_wait([timer, &client, test_pair, test](const boost::system::error_code& ec) {
            auto order = std::make_shared<Order>();
            order->pair = test_pair;
            order->client_oid = std::to_string(time_get_now_milli());
            order->type = OrderType::Limit;
            order->tif = test.tif;
            order->side = OrderSide::OpenLong;
            order->par_leverage = "10";
            order->price = 1.369;
            order->quantity = 12;

            INFRA_LOG_INFO("Testing {} order: {}", test.name, order->client_oid);
            client->place_order(order, [test](Errno err, SpOrder result) {
                if (err != Errno::Ok) {
                    INFRA_LOG_WARN("place_order callback failed, because: {}, {}, {}", to_string(err),
                                   to_string(result->ec), result->detail);
                } else {
                    INFRA_LOG_INFO("place_order callback, result: {}", result->to_json());
                }
            });
        });
    }
#endif

#if 1
    // 查询不存在的订单，要求错误码解析正确
    auto test_query_timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(18));
    test_query_timer->async_wait([test_query_timer, &g_ioc, &client, test_pair](const boost::system::error_code& ec) {
        auto test_query_order = std::make_shared<Order>();
        test_query_order->pair = test_pair;
        test_query_order->market_oid = "1234567";
        client->query_order(test_query_order, [test_query_order](Errno err, SpOrder result) {
            if (err != Errno::Ok) {
                INFRA_LOG_WARN("query_order callback, because: {}, {}, {}", to_string(err), to_string(result->ec),
                               result->detail);
            } else {
                INFRA_LOG_INFO("query_order callback, result:{}", result->to_json());
            }
        });
    });

    // 测试下单和价格精度调整，期望报单成功
    auto wrong_precision_timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(16));
    wrong_precision_timer->async_wait([wrong_precision_timer, &g_ioc, &client,
                                       test_pair](const boost::system::error_code& ec) {
        auto wrong_precision = std::make_shared<Order>();
        wrong_precision->pair = test_pair;
        wrong_precision->client_oid = std::to_string(time_get_now_milli());
        wrong_precision->par_leverage = "10";
        wrong_precision->side = OrderSide::OpenLong;
        wrong_precision->price = 1.123456789;
        wrong_precision->quantity = 12.1234567;
        client->place_order(wrong_precision, [&g_ioc, &client, wrong_precision, test_pair](Errno err, SpOrder result) {
            if (err != Errno::Ok) {
                INFRA_LOG_WARN("place_order callback failed, because: {}, {}, {}", to_string(err),
                               to_string(result->ec), result->detail);
            } else {
                INFRA_LOG_INFO("place_order callback result: {}", result->to_json());
            }
        });
    });

    // 测试下单数量过小，要求错误码解析正确
    auto too_small_timer = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(20));
    too_small_timer->async_wait([too_small_timer, &g_ioc, &client, test_pair](const boost::system::error_code& ec) {
        auto too_small_order = std::make_shared<Order>();
        too_small_order->pair = test_pair;
        too_small_order->client_oid = std::to_string(time_get_now_milli());
        too_small_order->side = OrderSide::OpenLong;
        too_small_order->price = 1.234;
        too_small_order->quantity = 0.2;
        client->place_order(too_small_order, [&g_ioc, &client, too_small_order, test_pair](Errno err, SpOrder result) {
            if (err != Errno::Ok) {
                INFRA_LOG_WARN("place_order callback failed, because: {}, {}, {}", to_string(err),
                               to_string(result->ec), result->detail);
            } else {
                INFRA_LOG_INFO("place_order callback result: {}", result->to_json());
            }
        });
    });

    // 测试名义价值限制，如果下单失败，要求错误码解析正确
    auto too_small_timer_tmp = std::make_shared<net::steady_timer>(g_ioc, std::chrono::seconds(22));
    too_small_timer_tmp->async_wait([too_small_timer_tmp, &g_ioc, &client,
                                     test_pair](const boost::system::error_code& ec) {
        auto too_small_order = std::make_shared<Order>();
        too_small_order->pair = test_pair;
        too_small_order->client_oid = std::to_string(time_get_now_milli());
        too_small_order->side = OrderSide::OpenLong;
        too_small_order->price = 0.02;
        too_small_order->quantity = 13.3;
        client->place_order(too_small_order, [&g_ioc, &client, too_small_order, test_pair](Errno err, SpOrder result) {
            if (err != Errno::Ok) {
                INFRA_LOG_WARN("place_order callback failed, because: {}, {}, {}", to_string(err),
                               to_string(result->ec), result->detail);
            } else {
                INFRA_LOG_INFO("place_order callback result: {}", result->to_json());
            }
        });
    });
#endif
}
