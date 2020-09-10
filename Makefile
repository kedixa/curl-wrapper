all: test_curl.out

test_curl.out: test_curl.cpp curl_wrapper.hpp curl_wrapper.cpp
	g++ -std=c++11 test_curl.cpp curl_wrapper.cpp -g -O2 -o test_curl.out -lcurl -lpthread -lcrypto

clean:
	rm test_curl.out