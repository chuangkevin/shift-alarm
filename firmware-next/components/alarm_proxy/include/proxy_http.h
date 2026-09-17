#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace alarm_proxy {
constexpr size_t MAX_HEADER = 16384;
constexpr uint64_t MAX_BODY = 12ULL * 1024 * 1024;
struct Request {
    std::string header;
    std::string method;
    std::string target;
    std::string host;
    std::string authorization;
    bool local = false;
    uint64_t content_length = 0;
};
/* Input is exactly one CRLF-terminated header block, not body bytes.
 * No request pipelining: output forces Connection: close. */
/* Validates one complete upstream header and returns its status. 101 is rejected. */
bool response_status(const std::string &input, unsigned &status);
bool remote_path(const std::string &target);
bool rewrite_request(const std::string &input, const std::string &lan_authority,
                     const std::string &backend_authority, Request &out,
                     bool allow_foreign_host = false);
}
