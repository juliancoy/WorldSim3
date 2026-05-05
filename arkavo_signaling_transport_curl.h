#pragma once

#include "arkavo_realtime_client.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

class ArkavoSignalingTransportCurl final : public ArkavoSignalingTransport {
public:
    ArkavoSignalingTransportCurl();
    ~ArkavoSignalingTransportCurl() override;

    bool connect(const std::string& url, std::string& err) override;
    void close() override;
    bool sendText(const std::string& text, std::string& err) override;

private:
    void runLoop(std::string url);

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread io_thread_;

    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<std::string> outbox_;
};
