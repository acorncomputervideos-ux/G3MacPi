// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/http_server.cpp — see http_server.h.

#include "platform/common/http_server.h"

namespace g3 {

namespace {

u32 slen(const char* s)
{
    u32 n = 0;
    while (s[n])
        ++n;
    return n;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool ieq(const char* a, u32 alen, const char* b)
{
    if (alen != slen(b))
        return false;
    for (u32 i = 0; i < alen; ++i)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}

int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Small formatting into a fixed buffer.
struct Out {
    char* p;
    u32 cap, n = 0;
    void str(const char* s)
    {
        while (*s && n + 1 < cap)
            p[n++] = *s++;
        p[n] = 0;
    }
    void num(u64 v)
    {
        char t[24];
        u32 k = 0;
        do {
            t[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (k && n + 1 < cap)
            p[n++] = t[--k];
        p[n] = 0;
    }
};

} // namespace

namespace http {

bool url_decode(const char* in, u32 in_len, char* out, u32 cap, bool plus_is_space)
{
    u32 n = 0;
    for (u32 i = 0; i < in_len; ++i) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= in_len)
                return false;  // "%" needs two hex digits after it
            const int hi = hexval(in[i + 1]), lo = hexval(in[i + 2]);
            if (hi < 0 || lo < 0)
                return false;
            c = static_cast<char>(hi * 16 + lo);
            i += 2;
        } else if (c == '+' && plus_is_space) {
            c = ' ';
        }
        if (c == 0 || n + 1 >= cap)
            return false;  // no NUL inside names; no room
        out[n++] = c;
    }
    out[n] = 0;
    return true;
}

bool base64_decode(const char* in, u8* out, u32 cap, u32* out_len)
{
    u32 acc = 0, bits = 0, n = 0;
    for (; *in && *in != '='; ++in) {
        const char c = *in;
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else return false;
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap)
                return false;
            out[n++] = static_cast<u8>(acc >> bits);
        }
    }
    *out_len = n;
    return true;
}

const char* status_text(u32 status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 507: return "Insufficient Storage";
    default: return "Error";
    }
}

} // namespace http

bool HttpRequest::param(const char* name, char* out, u32 cap) const
{
    const u32 nlen = slen(name);
    const char* p = query;
    while (*p) {
        const char* end = p;
        while (*end && *end != '&')
            ++end;
        const char* eq = p;
        while (eq < end && *eq != '=')
            ++eq;
        if (static_cast<u32>(eq - p) == nlen) {
            bool match = true;
            for (u32 i = 0; i < nlen; ++i)
                match = match && p[i] == name[i];
            if (match) {
                const char* v = eq < end ? eq + 1 : end;
                return http::url_decode(v, static_cast<u32>(end - v), out, cap, true);
            }
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

void HttpResponse::set(u32 st, const char* type, const char* text)
{
    status = st;
    content_type = type;
    body = reinterpret_cast<const u8*>(text);
    body_len = slen(text);
}

HttpConnection::HttpConnection(HttpHandler& handler, SendFn send, void* context)
    : h_(handler), send_(send), ctx_(context)
{
}

void HttpConnection::respond(const HttpResponse& r)
{
    char head[512];
    Out o{head, sizeof head};
    o.str("HTTP/1.1 ");
    o.num(r.status);
    o.str(" ");
    o.str(http::status_text(r.status));
    o.str("\r\nContent-Type: ");
    o.str(r.content_type);
    o.str("\r\nContent-Length: ");
    o.num(r.body_len);
    o.str("\r\nCache-Control: no-store\r\nConnection: close\r\n");
    if (r.extra_header) {
        o.str(r.extra_header);
        o.str("\r\n");
    }
    o.str("\r\n");
    send_(ctx_, reinterpret_cast<const u8*>(head), o.n);
    if (r.body_len && req_.method != HttpRequest::Method::Head)
        send_(ctx_, r.body, r.body_len);
    state_ = State::Done;
}

void HttpConnection::fail(u32 status, const char* text)
{
    HttpResponse r;
    r.set(status, "text/plain; charset=utf-8", text);
    respond(r);
}

bool HttpConnection::parse_head()
{
    // Request line: METHOD SP target SP version
    char* p = head_;
    char* line_end = p;
    while (*line_end && *line_end != '\r' && *line_end != '\n')
        ++line_end;
    char* sp1 = p;
    while (sp1 < line_end && *sp1 != ' ')
        ++sp1;
    char* target = sp1 + 1;
    char* sp2 = target;
    while (sp2 < line_end && *sp2 != ' ')
        ++sp2;
    if (sp1 >= line_end || sp2 >= line_end || target[0] != '/')
        return false;
    const u32 mlen = static_cast<u32>(sp1 - p);
    using M = HttpRequest::Method;
    req_.method = ieq(p, mlen, "GET") ? M::Get : ieq(p, mlen, "HEAD") ? M::Head : ieq(p, mlen, "PUT") ? M::Put
                  : ieq(p, mlen, "POST") ? M::Post : ieq(p, mlen, "DELETE") ? M::Delete : M::Other;
    char* q = target;
    while (q < sp2 && *q != '?')
        ++q;
    if (!http::url_decode(target, static_cast<u32>(q - target), req_.path, sizeof req_.path, false))
        return false;
    if (q < sp2) {
        const u32 qn = static_cast<u32>(sp2 - q - 1);
        if (qn + 1 > sizeof req_.query)
            return false;
        for (u32 i = 0; i < qn; ++i)
            req_.query[i] = q[1 + i];
        req_.query[qn] = 0;
    }
    // Header fields: name ":" OWS value OWS
    p = line_end;
    while (*p) {
        while (*p == '\r' || *p == '\n')
            ++p;
        if (!*p)
            break;
        char* end = p;
        while (*end && *end != '\r' && *end != '\n')
            ++end;
        char* colon = p;
        while (colon < end && *colon != ':')
            ++colon;
        if (colon < end) {
            char* v = colon + 1;
            while (v < end && (*v == ' ' || *v == '\t'))
                ++v;
            char* ve = end;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
                --ve;
            const u32 nlen = static_cast<u32>(colon - p), vlen = static_cast<u32>(ve - v);
            if (ieq(p, nlen, "content-length")) {
                u64 n = 0;
                if (vlen == 0 || vlen > 19)
                    return false;
                for (u32 i = 0; i < vlen; ++i) {
                    if (v[i] < '0' || v[i] > '9')
                        return false;
                    n = n * 10 + static_cast<u64>(v[i] - '0');
                }
                req_.content_length = n;
                req_.has_length = true;
            } else if (ieq(p, nlen, "transfer-encoding")) {
                chunked_ = true;  // not taken: answered with 411
            } else if (ieq(p, nlen, "authorization") && vlen < sizeof req_.authorization) {
                for (u32 i = 0; i < vlen; ++i)
                    req_.authorization[i] = v[i];
                req_.authorization[vlen] = 0;
            }
        }
        p = end;
    }
    return true;
}

bool HttpConnection::feed(const u8* data, u32 len)
{
    while (len && state_ != State::Done) {
        if (state_ == State::Head) {
            // Collect up to the blank line.
            const u8 c = *data++;
            --len;
            if (head_len_ + 1 >= kMaxHead) {
                fail(431, "Request header too large\n");
                return false;
            }
            head_[head_len_++] = static_cast<char>(c);
            head_[head_len_] = 0;
            if (head_len_ >= 4 && head_[head_len_ - 4] == '\r' && head_[head_len_ - 3] == '\n' &&
                head_[head_len_ - 2] == '\r' && head_[head_len_ - 1] == '\n') {
                if (!parse_head()) {
                    fail(400, "Bad request\n");
                    return false;
                }
                if (chunked_) {
                    fail(411, "Send the body with a Content-Length (chunked transfer is not supported)\n");
                    return false;
                }
                HttpResponse r;
                if (!h_.begin(req_, r)) {
                    respond(r);
                    return false;
                }
                body_left_ = req_.has_length ? req_.content_length : 0;
                state_ = State::Body;
                if (body_left_ == 0) {
                    h_.finish(true, r);
                    respond(r);
                    return false;
                }
            }
        } else {  // Body
            const u32 n = body_left_ < len ? static_cast<u32>(body_left_) : len;
            if (body_ok_)
                body_ok_ = h_.body(data, n);
            data += n;
            len -= n;
            body_left_ -= n;
            if (body_left_ == 0 || !body_ok_) {
                HttpResponse r;
                h_.finish(body_left_ == 0 && body_ok_, r);
                respond(r);
                return false;
            }
        }
    }
    return state_ != State::Done;
}

void HttpConnection::closed()
{
    if (state_ == State::Body) {
        HttpResponse r;
        h_.finish(false, r);  // nothing to send it to
        state_ = State::Done;
    }
}

} // namespace g3
