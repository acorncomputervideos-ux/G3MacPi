// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/http_server.h — one HTTP/1.1 connection, independent of
// the socket layer: received bytes go in (feed), response bytes come out
// through a callback. The request body is streamed to the handler as it
// arrives (a disk image of gigabytes never has to fit in memory); requests
// without a known length carry no body. One request per connection
// ("Connection: close"). RFC 9112 message syntax, the parts used here:
// request line, header fields, Content-Length.
//
// Freestanding: fixed-size buffers, no allocation.

#ifndef G3_PLATFORM_COMMON_HTTP_SERVER_H
#define G3_PLATFORM_COMMON_HTTP_SERVER_H

#include "core/common/types.h"

namespace g3 {

struct HttpRequest {
    enum class Method : u8 { Get, Head, Put, Post, Delete, Other };
    Method method = Method::Other;
    char path[256] = {};    // without the query, percent-decoded
    char query[512] = {};   // as sent (decoded by param())
    u64 content_length = 0;
    bool has_length = false;
    char authorization[256] = {};

    // A query parameter, percent-decoded ('+' is a space). False if absent
    // or too long for cap.
    bool param(const char* name, char* out, u32 cap) const;
};

struct HttpResponse {
    u32 status = 200;
    const char* content_type = "text/plain; charset=utf-8";
    const u8* body = nullptr;
    u32 body_len = 0;
    const char* extra_header = nullptr;  // a whole line without CRLF, e.g. WWW-Authenticate
    void set(u32 st, const char* type, const char* text);  // body = text (not copied)
};

class HttpHandler {
public:
    // The request line and header fields have arrived. Return true to take
    // the body: then body() gets it in pieces and finish() makes the
    // response. Return false to answer at once with resp (a body, if any,
    // is not read; the connection closes).
    virtual bool begin(const HttpRequest& req, HttpResponse& resp) = 0;
    // A piece of the body; false stops taking it (finish() is then called
    // with complete = false).
    virtual bool body(const u8* data, u32 len) = 0;
    // complete: the whole body arrived (Content-Length bytes).
    virtual void finish(bool complete, HttpResponse& resp) = 0;

protected:
    ~HttpHandler() = default;
};

class HttpConnection {
public:
    using SendFn = bool (*)(void* context, const u8* data, u32 len);
    static constexpr u32 kMaxHead = 8192;

    HttpConnection(HttpHandler& handler, SendFn send, void* context);

    // Bytes from the client. Returns false once the response has been sent
    // (or the request was bad): close the connection.
    bool feed(const u8* data, u32 len);
    // The client went away: an unfinished body is abandoned.
    void closed();
    bool finished() const { return state_ == State::Done; }

private:
    enum class State : u8 { Head, Body, Done };
    bool parse_head();
    void respond(const HttpResponse& resp);
    void fail(u32 status, const char* text);

    HttpHandler& h_;
    SendFn send_;
    void* ctx_;
    State state_ = State::Head;
    char head_[kMaxHead] = {};
    u32 head_len_ = 0;
    HttpRequest req_;
    u64 body_left_ = 0;
    bool body_ok_ = true;
    bool chunked_ = false;
};

// Text helpers shared with the web interface.
namespace http {
// Percent-decoding into out; false if it does not fit or is malformed.
bool url_decode(const char* in, u32 in_len, char* out, u32 cap, bool plus_is_space);
// RFC 4648 base64 decoding; false on bad input or no room. *out_len = bytes.
bool base64_decode(const char* in, u8* out, u32 cap, u32* out_len);
const char* status_text(u32 status);
} // namespace http

} // namespace g3

#endif // G3_PLATFORM_COMMON_HTTP_SERVER_H
