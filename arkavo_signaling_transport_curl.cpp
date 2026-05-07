#include "arkavo_signaling_transport_curl.h"

#include "thread_utils.h"

#include <curl/curl.h>

#include <chrono>
#include <cstring>

ArkavoSignalingTransportCurl::ArkavoSignalingTransportCurl() = default;

ArkavoSignalingTransportCurl::~ArkavoSignalingTransportCurl() {
    close();
}

bool ArkavoSignalingTransportCurl::connect(const std::string& url, std::string& err) {
    if (running_.load(std::memory_order_relaxed)) {
        err = "already running";
        return false;
    }
    if (io_thread_.joinable() && io_thread_.get_id() != std::this_thread::get_id()) {
        io_thread_.join();
    }
    running_.store(true, std::memory_order_relaxed);
    connected_.store(false, std::memory_order_relaxed);
    io_thread_ = std::thread([this, url]() {
        setCurrentThreadName("ws3-ark-curl");
        runLoop(url);
    });
    return true;
}

void ArkavoSignalingTransportCurl::close() {
    running_.store(false, std::memory_order_relaxed);
    q_cv_.notify_all();
    if (io_thread_.joinable()) io_thread_.join();
    connected_.store(false, std::memory_order_relaxed);
}

bool ArkavoSignalingTransportCurl::sendText(const std::string& text, std::string& err) {
    if (!running_.load(std::memory_order_relaxed)) {
        err = "transport not running";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        outbox_.push_back(text);
    }
    q_cv_.notify_one();
    return true;
}

void ArkavoSignalingTransportCurl::runLoop(std::string url) {
#if LIBCURL_VERSION_NUM < 0x080400
    if (on_error) on_error("libcurl websocket API requires curl >= 8.4.0");
    running_.store(false, std::memory_order_relaxed);
    return;
#else
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (on_error) on_error("curl_easy_init failed");
        running_.store(false, std::memory_order_relaxed);
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "WorldSim3-Arkavo/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        if (on_error) on_error(std::string("WebSocket connect failed: ") + curl_easy_strerror(rc));
        curl_easy_cleanup(curl);
        running_.store(false, std::memory_order_relaxed);
        if (on_close) on_close();
        return;
    }

    connected_.store(true, std::memory_order_relaxed);
    if (on_open) on_open();

    std::string recv_text;
    while (running_.load(std::memory_order_relaxed)) {
        // flush outbox
        for (;;) {
            std::string msg;
            {
                std::lock_guard<std::mutex> lk(q_mu_);
                if (outbox_.empty()) break;
                msg = std::move(outbox_.front());
                outbox_.pop_front();
            }
            size_t sent = 0;
            rc = curl_ws_send(curl, msg.data(), msg.size(), &sent, 0, CURLWS_TEXT);
            if (rc != CURLE_OK) {
                if (on_error) on_error(std::string("WebSocket send failed: ") + curl_easy_strerror(rc));
                running_.store(false, std::memory_order_relaxed);
                break;
            }
        }
        if (!running_.load(std::memory_order_relaxed)) break;

        char buf[64 * 1024];
        size_t nrecv = 0;
        const struct curl_ws_frame* meta = nullptr;
        rc = curl_ws_recv(curl, buf, sizeof(buf), &nrecv, &meta);
        if (rc == CURLE_AGAIN) {
            std::unique_lock<std::mutex> lk(q_mu_);
            q_cv_.wait_for(lk, std::chrono::milliseconds(15));
            continue;
        }
        if (rc != CURLE_OK) {
            if (on_error) on_error(std::string("WebSocket recv failed: ") + curl_easy_strerror(rc));
            running_.store(false, std::memory_order_relaxed);
            break;
        }
        if (nrecv > 0 && meta && (meta->flags & (CURLWS_TEXT | CURLWS_CONT))) {
            recv_text.append(buf, nrecv);
            if (meta->bytesleft == 0 && on_message) {
                on_message(recv_text);
                recv_text.clear();
            }
        } else if (nrecv > 0 && on_message) {
            on_message(std::string(buf, nrecv));
        }
        if (meta && (meta->flags & CURLWS_CLOSE)) {
            running_.store(false, std::memory_order_relaxed);
            break;
        }
    }

    curl_easy_cleanup(curl);
    connected_.store(false, std::memory_order_relaxed);
    if (on_close) on_close();
#endif
}
