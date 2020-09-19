#ifndef CURL_WRAPPER_HPP
#define CURL_WRAPPER_HPP
#include <thread>
#include <atomic>
#include <queue>
#include <string>
#include <mutex>
#include <memory>
#include <functional>
#include <curl/curl.h>

namespace kedixa {
class CurlWrapper {
public:
    // basic types
    class Session;
    struct SocketInfo;
    struct HandlerInfo;
    struct Request;
    struct Result;
    using request_finish_callback_t = std::function<void(CurlWrapper::Session*, CURL*)>;
    using make_handle_callback_t = std::function<void(CurlWrapper::Session*, CURL*)>;
    using retry_if_callback_t = std::function<bool(CurlWrapper::Session*, CURL*)>;
    using make_multihandle_callback_t = std::function<void(CURLM *)>;
    /**
     * Helper function to handle multi thread curl,
     * you are not suggested to use these functions
     */
    static CURL* make_handle(CurlWrapper::Session *);
    static void rmsocket(SocketInfo *, HandlerInfo*);
    static void setsocket(SocketInfo*, curl_socket_t, CURL*, int, HandlerInfo*);
    static void addsocket(curl_socket_t, CURL*, int, HandlerInfo*);
    static int socket_callback(CURL*, curl_socket_t, int, void*, void*);
    static int multi_timer_callback(CURLM*, long, void*);
    static void check_multi_info(HandlerInfo*, int);
    static void timer_callback(HandlerInfo*, int);
    static void event_callback(HandlerInfo*, int, int);
    static void locking_function(int, int, const char*, int);

    /**
     * Make and return a session for async curl request,
     * now it is just a new operation.
     */
    static Session* make_session() {
        return new Session();
    }

    // structures for request
    struct Request {
        std::string url;
        std::string user_agent;
        std::vector<std::string> headers;
        std::string body;
        std::string proxy;
        std::string request_method;
        long max_redirect{0};
        long max_retry{0};
        size_t timeout_ms{0};
        size_t connect_timeout_ms{0};
        bool verbose{false};
    };
    struct Result {
        std::string url;
        std::string final_url; // final url if redirected
        CURLcode curl_code{CURLE_OK};
        int http_code{0};
        int proxy_http_code{0};
        std::vector<std::string> headers;
        std::string body;
    };
    /**
     * Session is a class to manage one request and its response.
     */
    class Session {
    private:
        Request req;
        Result res;
        struct curl_slist *header_list;
        request_finish_callback_t finish_cb;
        void *context;
        long retry_cnt;
        friend class CurlWrapper;
        void clear_res() {
            res.url.clear();
            res.final_url.clear();
            res.curl_code = CURLE_OK;
            res.http_code = 0;
            res.proxy_http_code = 0;
            res.headers.clear();
            res.body.clear();
        }
    public:
        Session();
        Session(const Session &) = delete;
        Session& operator=(const Session &) = delete;
        ~Session();
        Request *get_req() { return &req; }
        Result *get_res() { return &res; }
        // cb will be called when the request is finished
        void set_finish_callback(request_finish_callback_t &&cb) { finish_cb = std::move(cb); }
        // You can use set and get context to save a pointer value in this session.
        void set_context(void *ctx) { context = ctx; }
        void *get_context() const { return context; }
        long get_retry_cnt() const { return retry_cnt; }
    };
private:
    static size_t members_counter;
    static std::mutex counter_mtx;
    static std::vector<std::mutex> crypto_mtx;
public:
    CurlWrapper(size_t thread_num = 0, size_t max_handling_per_thread = 200);
    CurlWrapper(const CurlWrapper &) = delete;
    CurlWrapper& operator=(const CurlWrapper &) = delete;
    ~CurlWrapper();
    bool start(); // call once and only once
    bool stop(); // call once and only once
    bool empty() const { // check whether all task done
        return handling_cnt == 0 && request_queue_cnt == 0;
    }
    bool put_request(Session *req);
    size_t get_handling_size() const {
        return handling_cnt;
    }
    size_t get_request_queue_size() const {
        return request_queue_cnt;
    }
    // This cb will be called just before we use this curl easy handle
    void set_make_handle_callback(make_handle_callback_t &&cb) {
        make_handle_cb = std::move(cb);
    }
    // This cb will be called after the request is done
    void set_retry_if_callback(retry_if_callback_t &&cb) {
        retry_if_cb = std::move(cb);
    }
    // This cb will be called at CurlWrapper::start, before handling requests,
    // in multi thread without locks
    void set_multihandle_callback(make_multihandle_callback_t &&cb) {
        make_multihandle_cb = std::move(cb);
    }
private:
    Session* get_request();
    void run(size_t index);

    size_t max_handling;
    size_t thread_num;
    std::vector<std::thread> checker_threads;
    // handler info for each thread
    std::vector<std::shared_ptr<HandlerInfo>> handler_info;
    std::queue<Session*> request_queue;
    std::mutex request_queue_mtx;
    std::atomic<bool> running;
    std::atomic<size_t> handling_cnt;
    std::atomic<size_t> request_queue_cnt;
    std::atomic<size_t> total_request;
    // callback
    make_handle_callback_t make_handle_cb;
    retry_if_callback_t retry_if_cb;
    make_multihandle_callback_t make_multihandle_cb;
};
} // namespace kedixa
#endif // CURL_WRAPPER_HPP
