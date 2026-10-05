// 配置热加载那条链的单元测试。
//
// Config 早就有观察者机制（addObserver / removeObserver / notifyObservers），
// 但 addObserver 在全仓没有任何调用点 —— 于是 EventLoop 每 10 秒调一次的那次
// reload() 遍历的是一张空表，log.h 和 docs/architecture/network.md 承诺的
// "配置热加载、日志级别自动生效"从来没成立过。
//
// 这个文件钉三件事：
//   1. 注册过的观察者，reload() 之后真的被叫到，并且拿到的是新值；
//   2. Logger::bindToConfigHotReload() 之后，改文件里的 log_level 会让 Logger 的
//      级别真的变化（这一条要求 Logger 有读取口 —— 没有读取口时这事不可观测，
//      所以那条链也一直没人验证过）；
//   3. reload() 读不到文件时必须保住原来那一份。旧代码先 clear() 再丢掉
//      loadInternal() 的返回值，conf 一旦被改名/读不出来，整张表就只剩四个
//      log_* 默认值，port / max_entries / 缓冲上限对这些后来的读取者静默消失。

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "../trace/test_assertions.h"
#include "base/config.h"
#include "base/log.h"

namespace cc_server {
namespace testing {

namespace {

    constexpr const char* kConfPath = "cc_config_reload_test.conf";

    void write_conf(const std::string& body) {
        std::ofstream out(kConfPath, std::ios::binary | std::ios::trunc);
        out << body;
        out.close();
    }

    /// @brief 记录自己被叫到过的键值，用来判断 reload() 有没有真的通知
    class RecordingObserver : public ConfigObserver {
    public:
        void onConfigChange(const std::string& key, const std::string& value) override {
            seen_.emplace_back(key, value);
        }
        void reset() { seen_.clear(); }
        [[nodiscard]] int count_for(const std::string& key) const {
            int n = 0;
            for (const auto& kv : seen_) {
                if (kv.first == key) ++n;
            }
            return n;
        }
        [[nodiscard]] std::string value_for(const std::string& key) const {
            for (const auto& kv : seen_) {
                if (kv.first == key) return kv.second;
            }
            return "<没被叫到>";
        }

    private:
        std::vector<std::pair<std::string, std::string>> seen_;
    };

    void test_reload_notifies_registered_observer() {
        TEST_SUITE("Config 热加载");

        write_conf("port = 6399\nlog_level = info\n");
        EXPECT_TRUE(Config::instance().load(kConfPath));

        RecordingObserver observer;
        Config::instance().addObserver("log_level", &observer);

        write_conf("port = 6399\nlog_level = warning\n");
        Config::instance().reload();

        // 回调必须发生，而且拿到的是新值而不是旧值
        EXPECT_EQ(observer.count_for("log_level"), 1);
        EXPECT_EQ(observer.value_for("log_level"), std::string("warning"));
        EXPECT_EQ(Config::instance().getString("log_level", ""), std::string("warning"));

        Config::instance().removeObserver("log_level", &observer);
    }

    void test_removed_observer_stops_receiving_notifications() {
        TEST_SUITE("Config 热加载");

        write_conf("port = 6399\nlog_level = info\n");
        EXPECT_TRUE(Config::instance().load(kConfPath));

        RecordingObserver observer;
        Config::instance().addObserver("log_level", &observer);
        Config::instance().removeObserver("log_level", &observer);

        write_conf("port = 6399\nlog_level = error\n");
        Config::instance().reload();

        // 摘掉之后还收到通知，说明 removeObserver 没真的摘干净
        EXPECT_EQ(observer.count_for("log_level"), 0);
    }

    void test_logger_level_follows_hot_reload() {
        TEST_SUITE("Config 热加载");

        write_conf("port = 6399\nlog_level = info\n");
        EXPECT_TRUE(Config::instance().load(kConfPath));

        Logger::instance().setLevel(LogLevel::INFO);
        Logger::instance().bindToConfigHotReload();

        write_conf("port = 6399\nlog_level = debug\n");
        Config::instance().reload();

        // 这一条是整条链的终点：改文件之后 Logger 真的换了级别。
        // 接线之前 reload 遍历的是空表，级别会停在 INFO。
        EXPECT_EQ(static_cast<int>(Logger::instance().level()), static_cast<int>(LogLevel::DEBUG));

        // 认不出来的值不许把级别改掉（保持原级别，并且仍旧是 DEBUG）
        write_conf("port = 6399\nlog_level = 完全不认识的级别\n");
        Config::instance().reload();
        EXPECT_EQ(static_cast<int>(Logger::instance().level()), static_cast<int>(LogLevel::DEBUG));

        Logger::instance().setLevel(LogLevel::INFO);
    }

    void test_failed_reload_keeps_previous_values() {
        TEST_SUITE("Config 热加载");

        write_conf("port = 6399\nmax_entries = 50000\nlog_level = info\n");
        EXPECT_TRUE(Config::instance().load(kConfPath));
        EXPECT_EQ(Config::instance().getInt("port", -1), 6399);

        // 把文件挪走：reload() 读不到，就必须原样保住表里的内容
        std::error_code ec;
        std::filesystem::rename(kConfPath, std::string(kConfPath) + ".gone", ec);
        EXPECT_TRUE(!ec);

        Config::instance().reload();

        EXPECT_EQ(Config::instance().getInt("port", -1), 6399);
        EXPECT_EQ(Config::instance().getInt("max_entries", -1), 50000);
        EXPECT_EQ(Config::instance().getString("log_level", ""), std::string("info"));

        std::filesystem::rename(std::string(kConfPath) + ".gone", kConfPath, ec);
    }

}  // namespace

void run_all_config_tests() {
    std::cout << "\n========================================\n";
    std::cout << "Running Config Hot-Reload Tests\n";
    std::cout << "========================================\n\n";

    test_reload_notifies_registered_observer();
    test_removed_observer_stops_receiving_notifications();
    test_logger_level_follows_hot_reload();
    test_failed_reload_keeps_previous_values();

    std::error_code ec;
    std::filesystem::remove(kConfPath, ec);

    std::cout << "\n========================================\n";
    std::cout << "All Config Hot-Reload Tests Done!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
