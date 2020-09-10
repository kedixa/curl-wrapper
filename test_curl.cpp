#include "curl_wrapper.hpp"
#include <iostream>
using namespace std;
using namespace kedixa;

void callback(CurlWrapper::Session *s, CURL*) {
    auto *res = s->get_res();
    cout << "curl code:" << res->curl_code << " http code:" << res->http_code << " proxy code:" << res->proxy_http_code << endl;
    cout << "url:" << res->url << " final url:" << res->final_url << endl;
    for(const auto &s : res->headers) {
        if(s.size() > 2) cout << s;
    }
    cout << "body size: " << res->body.size() << endl;
    cout << endl;
}
bool retry_if_callback(CurlWrapper::Session *s, CURL *) {
    auto *res = s->get_res();
    int http_code = res->http_code;
    int curl_code = res->curl_code;
    int proxy_http_code = res->proxy_http_code;
    cout << "retry if callback http:" << http_code << " curl:" << curl_code << endl;
    if(curl_code != 0) return true;
    return false;
}

void test_get(CurlWrapper &w, string url) {
    CurlWrapper::Session *s = CurlWrapper::make_session();
    auto *req = s->get_req();
    req->url = move(url);
    req->user_agent = "CurlWrapper";
    req->headers = {
        "Accept: */*",
    };
    req->request_method = "GET";
    req->body = "";
    req->timeout_ms = 30 * 1000;
    req->max_redirect = 0;
    req->max_retry = 4;
    // req->proxy = "your proxy";
    // req->verbose = true;
    s->set_context(nullptr);
    s->set_finish_callback(callback);
    w.put_request(s);
}
int main() {
    CurlWrapper w(4, 100);
    w.set_make_handle_callback([](CurlWrapper::Session *, CURL*){
        cout << "make handle callback" << endl;
    });
    w.set_retry_if_callback(retry_if_callback);
    w.start();
    string url;
    while(cin >> url) test_get(w, url);
    w.stop();
    return 0;
}
