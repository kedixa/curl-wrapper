#include "curl_wrapper.hpp"
#include <openssl/err.h>
#include <cassert>
#include <ctime>
#include <vector>
#include <cstring>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <unistd.h>
using namespace std;

namespace kedixa {
#define LOG_INFO printf

struct CurlWrapper::SocketInfo {
    curl_socket_t sockfd;
    CURL *easy;
    int action;
    long timeout;
    HandlerInfo *handler;
};
struct CurlWrapper::HandlerInfo {
    int epfd; // epoll fd
    int tfd; // timer fd
    int still_running;
    size_t handling_cnt;
    CURLM *multi_handle;
    CurlWrapper *wrapper;
    std::queue<CURL*> retry_que;
    HandlerInfo() {
        epfd = 0;
        tfd = 0;
        still_running = 0;
        handling_cnt = 0;
        multi_handle = nullptr;
        wrapper = nullptr;
    }
};

// static member(s) for CurlWrapper
size_t CurlWrapper::members_counter = 0;
std::mutex CurlWrapper::counter_mtx;
std::vector<std::mutex> CurlWrapper::crypto_mtx;

// static helper functions
static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    CurlWrapper::Session* s = static_cast<CurlWrapper::Session*>(userdata);
    auto *res = s->get_res();
    if(!res->headers.empty() && res->headers.back() == "\r\n") {
        // clear header and body if redirected
        res->headers.clear();
        res->body.clear();
    }
    res->headers.emplace_back(buffer, nitems * size);
    return nitems * size;
}
static size_t write_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    CurlWrapper::Session* s = static_cast<CurlWrapper::Session*>(userdata);
    s->get_res()->body.append(buffer, nitems * size);
    return nitems * size;
}
static int sockopt_callback(void *clientp, curl_socket_t curlfd, curlsocktype purpose) {
    (void)clientp;
    (void)purpose;
    int reuse = 1;
    int ret = setsockopt(curlfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&reuse, sizeof(reuse));
    if(ret == -1) {
        return CURL_SOCKOPT_ERROR;
    }
    return CURL_SOCKOPT_OK;
}
CURL* CurlWrapper::make_handle(CurlWrapper::Session *s) {
    CURL *handle = curl_easy_init();
    auto *req = s->get_req();
    // curl basic options
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, (void*)s); // header callback pointer
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, (void*)s);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_callback); // header callback
    curl_easy_setopt(handle, CURLOPT_PRIVATE, (void*)s);
    // curl_easy_setopt(handle, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    // curl_easy_setopt(handle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4); // ipv4 only
    // curl_easy_setopt(handle, CURLOPT_INTERFACE, "your interface"); // bind interface
    curl_easy_setopt(handle, CURLOPT_SOCKOPTFUNCTION, sockopt_callback);

    // user set options
    curl_easy_setopt(handle, CURLOPT_URL, req->url.c_str());
    if(!req->user_agent.empty()) {
        curl_easy_setopt(handle, CURLOPT_USERAGENT, const_cast<char*>(req->user_agent.c_str()));
    }
    if(!req->headers.empty()) {
        struct curl_slist *slist = nullptr;
        for(const std::string &h : req->headers) {
            if(!h.empty()) {
                slist = curl_slist_append(slist, h.c_str());
            }
        }
        s->header_list = slist; // release at the end
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, slist);
    }
    else {
        s->header_list = nullptr;
    }
    if(!req->body.empty()) {
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, const_cast<char*>(req->body.c_str()));
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body.length());
    }
    if(!req->proxy.empty()) {
        curl_easy_setopt(handle, CURLOPT_PROXY, req->proxy.c_str());
    }
    if(req->request_method.empty()) req->request_method = "GET";
    if(req->request_method == "HEAD") {
        curl_easy_setopt(handle, CURLOPT_NOBODY, 1L); // HEAD request
    }
    else {
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, const_cast<char*>(req->request_method.c_str()));
    }
    if(req->max_redirect != 0) {
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, (long)(req->max_redirect));
    }
    else {
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    }
    if(req->timeout_ms > 0) {
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, (long)req->timeout_ms);
    }
    if(req->connect_timeout_ms > 0) {
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, (long)req->connect_timeout_ms);
    }
    if(req->verbose) curl_easy_setopt(handle, CURLOPT_VERBOSE, 1L);

    return handle;
}
void CurlWrapper::rmsocket(SocketInfo *f, HandlerInfo *h) {
    if(f) {
        if(f->sockfd) {
            if(epoll_ctl(h->epfd, EPOLL_CTL_DEL, f->sockfd, NULL)) { // del fail
            }
        }
        delete f;
    }
}
void CurlWrapper::setsocket(SocketInfo *f, curl_socket_t s, CURL *e, int act, HandlerInfo *h) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(struct epoll_event));
    int kind = ((act & CURL_POLL_IN) ? (int)EPOLLIN : 0) |
               ((act & CURL_POLL_OUT) ? (int)EPOLLOUT : 0);
    if(f->sockfd) {
        if(epoll_ctl(h->epfd, EPOLL_CTL_DEL, f->sockfd, NULL)) { // del fail
        }
    }
    f->sockfd = s;
    f->action = act;
    f->easy = e;
    ev.events = kind;
    ev.data.fd = s;
    if(epoll_ctl(h->epfd, EPOLL_CTL_ADD, s, &ev)) { // add fail
    }
}
void CurlWrapper::addsocket(curl_socket_t s, CURL *easy, int action, HandlerInfo *h) {
    SocketInfo *f = new SocketInfo();
    memset(f, 0, sizeof(SocketInfo));
    f->handler = h;
    setsocket(f, s, easy, action, h);
    curl_multi_assign(h->multi_handle, s, f);
}
int CurlWrapper::socket_callback(CURL *e, curl_socket_t s, int what, void *p, void *sockp) {
    HandlerInfo *h = static_cast<HandlerInfo*>(p);
    SocketInfo *f = static_cast<SocketInfo*>(sockp);
    if(what == CURL_POLL_REMOVE) {
        rmsocket(f, h);
    }
    else {
        if(!f) {
            addsocket(s, e, what, h);
        }
        else {
            setsocket(f, s, e, what, h);
        }
    }
    return 0;
}
int CurlWrapper::multi_timer_callback(CURLM *multi, long timeout_ms, void *p) {
    //LOG_INFO("CurlWrapper %s timeout:%d\n", __FUNCTION__, (int)timeout_ms);
    static_cast<void>(multi);
    HandlerInfo *h = static_cast<HandlerInfo*>(p);
    struct itimerspec its;
    if(timeout_ms > 0) {
        its.it_interval.tv_sec = 1;
        its.it_interval.tv_nsec = 0;
        its.it_value.tv_sec = timeout_ms / 1000;
        its.it_value.tv_nsec = (timeout_ms % 1000) * 1000 * 1000;
    }
    else if(timeout_ms == 0) {
        its.it_interval.tv_sec = 1;
        its.it_interval.tv_nsec = 0;
        its.it_value.tv_sec = 0;
        its.it_value.tv_nsec = 1;
    }
    else {
        memset(&its, 0, sizeof(struct itimerspec));
    }
    timerfd_settime(h->tfd, 0, &its, NULL);
    return 0;
}

void CurlWrapper::check_multi_info(HandlerInfo *h, int max_cnt) {
    CurlWrapper *wrapper = h->wrapper;
    CURLMsg *msg = NULL;
    int msgs_left;
    int cnt = 0;
    while((msg = curl_multi_info_read(h->multi_handle, &msgs_left))) {
        // LOG_INFO("CurlWrapper check multi info (%d/%d)\n", cnt, msgs_left);
        if(msg->msg == CURLMSG_DONE) {
            CURL *handle = msg->easy_handle;
            if(handle == nullptr) {
                continue;
            }
            Session *s = nullptr;
            curl_easy_getinfo(handle, CURLINFO_PRIVATE, &s);
            assert(s);
            long res_status = 0, proxy_http_code = 0; // http response code
            curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &res_status);
            curl_easy_getinfo(handle, CURLINFO_HTTP_CONNECTCODE, &proxy_http_code);
            auto *res = s->get_res();
            auto *req = s->get_req();
            // make result
            res->url = req->url;
            char *pfinal = nullptr;
            curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &pfinal);
            if(pfinal) res->final_url.assign(pfinal);
            else res->final_url = req->url;
            res->curl_code = msg->data.result;
            res->http_code = res_status;
            res->proxy_http_code = proxy_http_code;
            // header and body are already set when perform curl
            // res->headers; res->body;
            bool should_retry = false;
            if(s->retry_cnt < req->max_retry && wrapper->retry_if_cb) {
                should_retry = wrapper->retry_if_cb(s, handle);
            }
            if(should_retry) {
                s->retry_cnt++;
                s->clear_res();
                CURLMcode rc = curl_multi_remove_handle(h->multi_handle, handle);
                if(rc != CURLM_OK) {
                    // LOG_INFO("CurlWrapper curl multi remove return %d\n", (int)rc);
                }
                // handle will be added to multi handle at main loop
                h->retry_que.push(handle);
            }
            else {
                // callback
                if(s->finish_cb) {
                    s->finish_cb(s, handle);
                }
                // release temp data
                struct curl_slist *list = (struct curl_slist*)(s->header_list);
                if(list) curl_slist_free_all(list);
                delete s;
                CURLMcode rc = curl_multi_remove_handle(h->multi_handle, handle);
                if(rc != CURLM_OK) {
                    // LOG_INFO("CurlWrapper curl multi remove return %d\n", (int)rc);
                }
                curl_easy_cleanup(handle);
                --(h->handling_cnt);
                --(h->wrapper->handling_cnt);
            }
        }
        cnt++;
        (void)max_cnt;
        // if(cnt > max_cnt) break;
    }
}
void CurlWrapper::timer_callback(HandlerInfo *h, int revents) {
    static_cast<void>(revents);
    uint64_t count = 0;
    ssize_t err = 0;
    CURLMcode rc;
    err = read(h->tfd, &count, sizeof(uint64_t));
    if(err == -1) {
        if(errno == EAGAIN) {
            //LOG_INFO("CurlWrapper read tfd err %ld\n", err);
            return;
        }
    }
    if(err != sizeof(uint64_t)) {
        // LOG_INFO("CurlWrapper read tfd size error %ld\n", err);
    }
    rc = curl_multi_socket_action(h->multi_handle, CURL_SOCKET_TIMEOUT, 0, &(h->still_running));
    if(rc != CURLM_OK) {
        // LOG_INFO("CurlWrapper curl_multi_socket_action return %d\n", (int)rc);
    }
    check_multi_info(h, 1024);
}
void CurlWrapper::event_callback(HandlerInfo *h, int fd, int revents) {
    CURLMcode rc;
    struct itimerspec its;
    int action = ((revents & EPOLLIN) ? CURL_CSELECT_IN : 0) |
                 ((revents & EPOLLOUT) ? CURL_CSELECT_OUT : 0);
    rc = curl_multi_socket_action(h->multi_handle, fd, action, &(h->still_running));
    if(rc != CURLM_OK) {
        // LOG_INFO("CurlWrapper event_callback curl_multi_socket_action return %d\n", (int)rc);
    }
    check_multi_info(h, 1024);
    if(h->still_running <= 0) {
        memset(&its, 0, sizeof(struct itimerspec));
        timerfd_settime(h->tfd, 0, &its, NULL);
    }
}
void CurlWrapper::locking_function(int mode, int n, const char*, int) {
    // openssl <= 1.0.2 is not thread safe
    if(n >= (int)crypto_mtx.size()) {
        // LOG_INFO("CurlWrapper locking_function n(%d) >= mtxs(%zu)\n", n, crypto_mtx.size());
        return;
    }
    if(mode & CRYPTO_LOCK) {
        CurlWrapper::crypto_mtx[n].lock();
    }
    else {
        CurlWrapper::crypto_mtx[n].unlock();
    }
}
static void ssl_threadid_callback(CRYPTO_THREADID *id) {
    // openssl >= 1.0
    CRYPTO_THREADID_set_numeric(id, (unsigned long)pthread_self());
}

// class Session
CurlWrapper::Session::Session() {
    this->header_list = nullptr;
    this->context = nullptr;
    this->retry_cnt = 0;
}
CurlWrapper::Session::~Session() { }

// CurlWrapper member functions
CurlWrapper::CurlWrapper(size_t thread_num, size_t max_handling_per_thread) {
    if(thread_num == 0) thread_num = thread::hardware_concurrency();
    if(thread_num == 0) thread_num = 6;
    this->thread_num = thread_num;
    this->max_handling = max_handling_per_thread;
    counter_mtx.lock();
    if(members_counter == 0) {
        vector<mutex>(CRYPTO_num_locks()).swap(crypto_mtx);
        CRYPTO_THREADID_set_callback(ssl_threadid_callback);
        CRYPTO_set_locking_callback(locking_function);
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ++members_counter;
    counter_mtx.unlock();
    running = false;
}
CurlWrapper::~CurlWrapper() {
    stop();
    counter_mtx.lock();
    --members_counter;
    // there must no other threads use liburl at this time
    if(members_counter == 0) {
        curl_global_cleanup();
        CRYPTO_THREADID_set_callback(nullptr);
        CRYPTO_set_locking_callback(nullptr);
        vector<mutex>().swap(crypto_mtx);
    }
    counter_mtx.unlock();
}
bool CurlWrapper::start() {
    HandlerInfo info;
    info.wrapper = this;
    handler_info.resize(thread_num);
    for(auto &hinfo : handler_info)
        hinfo = make_shared<HandlerInfo>(info);
    checker_threads.resize(thread_num);
    request_queue_cnt = 0;
    total_request = 0;
    handling_cnt = 0;
    running = true;
    for(size_t i = 0; i < checker_threads.size(); i++)
        checker_threads[i] = std::thread(&CurlWrapper::run, this, i);
    return true;
}
bool CurlWrapper::stop() {
    if(running == false) return true;
    running = false;
    for(size_t i = 0; i < checker_threads.size(); i++)
        checker_threads[i].join();
    handler_info.clear();
    checker_threads.clear();
    return true;
}
bool CurlWrapper::put_request(Session *req) {
    // queue may become very large
    std::lock_guard<std::mutex> lg(request_queue_mtx);
    request_queue.push(req);
    ++request_queue_cnt;
    ++total_request;
    return true;
}
CurlWrapper::Session* CurlWrapper::get_request() {
    lock_guard<mutex> lg(request_queue_mtx);
    if(request_queue.empty()) return nullptr;
    else {
        Session *req = request_queue.front();
        request_queue.pop();
        --request_queue_cnt;
        return req;
    }
}

void CurlWrapper::run(size_t index) {
    // HandlerInfo save this thread status
    HandlerInfo &info = *(handler_info[index].get());
    info.epfd = epoll_create1(EPOLL_CLOEXEC);
    if(info.epfd == -1) {
        // LOG_INFO("CurlWrapper create epfd fail %d, thread exit\n", info.epfd);
        return;
    }
    info.tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if(info.tfd == -1) {
        // LOG_INFO("CurlWrapper create tfd fail %d, thread exit\n", info.tfd);
        close(info.epfd);
        info.epfd = -1;
        return;
    }
    struct epoll_event ev;
    struct epoll_event events[1024];
    struct itimerspec its;
    memset(&its, 0, sizeof(struct itimerspec));
    memset(&ev, 0, sizeof(struct epoll_event));
    its.it_interval.tv_sec = 1;
    its.it_value.tv_sec = 1;
    timerfd_settime(info.tfd, 0, &its, NULL);
    ev.events = EPOLLIN;
    ev.data.fd = info.tfd;
    epoll_ctl(info.epfd, EPOLL_CTL_ADD, info.tfd, &ev);

    info.multi_handle = curl_multi_init();
    curl_multi_setopt(info.multi_handle, CURLMOPT_SOCKETFUNCTION, socket_callback);
    curl_multi_setopt(info.multi_handle, CURLMOPT_SOCKETDATA, &info);
    curl_multi_setopt(info.multi_handle, CURLMOPT_TIMERFUNCTION, multi_timer_callback);
    curl_multi_setopt(info.multi_handle, CURLMOPT_TIMERDATA, &info);
    while(running || info.handling_cnt != 0 || request_queue_cnt != 0) {
        bool should_wait = true;
        int err = epoll_wait(info.epfd, events, sizeof(events)/sizeof(struct epoll_event), 10);
        if(err == -1) { }
         // LOG_INFO("CurlWrapper check epoll wait return %d\n", err);
        for(int idx = 0; idx < err; idx++) {
            if(events[idx].data.fd == info.tfd) {
                // LOG_INFO("CurlWrapper check timer\n");
                timer_callback(&info, events[idx].events);
            }
            else {
                // LOG_INFO("CurlWrapper check event(%d/%d)\n", idx, err);
                event_callback(&info, events[idx].data.fd, events[idx].events);
            }
            should_wait = false;
        }
        while(!info.retry_que.empty()) {
            CURL *curl = info.retry_que.front();
            info.retry_que.pop();
            curl_multi_add_handle(info.multi_handle, curl);
            // LOG_INFO("Curl Wrapper add retry handle\n");
        }
        // handle request queue
        while(info.handling_cnt < max_handling) {
            Session* req_session = get_request();
            if(req_session == nullptr) break;
            should_wait = false;
            CURL* handle = make_handle(req_session);
            assert(handle);
            if(make_handle_cb) {
                make_handle_cb(req_session, handle);
            }
            ++info.handling_cnt;
            ++handling_cnt;
            curl_multi_add_handle(info.multi_handle, handle);
        }
        if(running && should_wait) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // close
    close(info.tfd);
    close(info.epfd);
    info.tfd = info.epfd = -1;
    if(info.multi_handle) {
        curl_multi_cleanup(info.multi_handle);
        info.multi_handle = nullptr;
    }
    // LOG_INFO("CurlWrapper check thread %zu exit\n", index);
}
#undef LOG_INFO
} // namespace kedixa
