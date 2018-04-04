#ifndef _HTTP2_WARPPER_H
#define _HTTP2_WARPPER_H

#include <stdint.h>

#ifndef ssize_t
typedef  size_t ssize_t;
#endif

#include <iostream>
#include <string>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <functional> 
#include <stdlib.h>
#include <string.h>

#include <brynet/net/SocketLibFunction.h>
#include <brynet/net/http/HttpService.h>
#include <brynet/net/http/HttpFormat.h>
#include <brynet/net/http/WebSocketFormat.h>
#include <brynet/net/Connector.h>  
#include <brynet/utils/packet.h>

#include "nghttp2/nghttp2.h"

using namespace brynet;
using namespace brynet::net;

typedef struct {
    /* The NULL-terminated URI string to retrieve. */
    const char *uri;
    /* Parsed result of the |uri| */
    struct http_parser_url *u;
    /* The authority portion of the |uri|, not NULL-terminated */
    char *authority;
    /* The path portion of the |uri|, including query, not
    NULL-terminated */
    char *path;
    /* The length of the |authority| */
    size_t authoritylen;
    /* The length of the |path| */
    size_t pathlen;
    /* The stream ID of this stream */
    int32_t stream_id;
} http2_stream_data;

typedef std::function<void(const uint8_t *, size_t)> HTTP2_DATA_CALLBACK;

struct http2_session_data {
    ~http2_session_data()
    {
    }

    // TODO::destroy session memory
    nghttp2_session *session;
    std::unordered_map<int, HTTP2_DATA_CALLBACK> dataCallbacks;
    TCPSession::PTR tcpsession;
    std::string packet_data;
};

static void print_header(FILE *f, const uint8_t *name, size_t namelen,
    const uint8_t *value, size_t valuelen) {
    fwrite(name, 1, namelen, f);
    fprintf(f, ": ");
    fwrite(value, 1, valuelen, f);
    fprintf(f, "\n");
}

static ssize_t send_callback(nghttp2_session *session, const uint8_t *data,
    size_t length, int flags, void *user_data) {
    http2_session_data* session_data = (http2_session_data*)user_data;
    session_data->tcpsession->send((const char*)data, length);

    return length;
}

static int on_header_callback(nghttp2_session *session,
    const nghttp2_frame *frame, const uint8_t *name,
    size_t namelen, const uint8_t *value,
    size_t valuelen, uint8_t flags, void *user_data) {
    http2_session_data *session_data = (http2_session_data *)user_data;
    return (int)0;
}

static int on_begin_headers_callback(nghttp2_session *session,
    const nghttp2_frame *frame,
    void *user_data) {
    return 0;
}

static int on_frame_recv_callback(nghttp2_session *session,
    const nghttp2_frame *frame, void *user_data) {
    return 0;
}

static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags,
    int32_t stream_id, const uint8_t *data,
    size_t len, void *user_data) {
    http2_session_data *session_data = (http2_session_data *)user_data;
    (void)session;
    (void)flags;

    auto it = session_data->dataCallbacks.find(stream_id);
    if (it != session_data->dataCallbacks.end() && (*it).second != nullptr)
    {
        (*it).second(data, len);
    }
    return 0;
}

static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
    uint32_t error_code, void *user_data) {
    return 0;
}

static void send_client_connection_header(http2_session_data *session_data) {
    nghttp2_settings_entry iv[1] = {
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 } };
    int rv;

    /* client 24 bytes magic string will be sent by nghttp2 library */
    rv = nghttp2_submit_settings(session_data->session, NGHTTP2_FLAG_NONE, iv,
        1);
    if (rv != 0) {
    }
}

#define MAKE_NV(NAME, VALUE, VALUELEN)                                         \
  {                                                                            \
    (uint8_t *)NAME, (uint8_t *)VALUE, sizeof(NAME) - 1, VALUELEN,             \
        NGHTTP2_NV_FLAG_NONE                                                   \
  }

#define MAKE_NV2(NAME, VALUE)                                                  \
  {                                                                            \
    (uint8_t *)NAME, (uint8_t *)VALUE, sizeof(NAME) - 1, sizeof(VALUE) - 1,    \
        NGHTTP2_NV_FLAG_NONE                                                   \
  }


#define ARRLEN(x) (sizeof(x) / sizeof(x[0]))
struct header_value {
    // header field value
    std::string value;
    // true if the header field value is sensitive information, such as
    // authorization information or short length secret cookies.  If
    // true, those header fields are not indexed by HPACK (but still
    // huffman-encoded), which results in lesser compression.
    bool sensitive;
};

using header_map = std::unordered_map<std::string, header_value>;

template <size_t N>
nghttp2_nv make_nv_ls(const char(&name)[N], const std::string &value) {
    return { (uint8_t *)name, (uint8_t *)value.c_str(), N - 1, value.size(),
        NGHTTP2_NV_FLAG_NO_COPY_NAME };
}

static int session_send(http2_session_data *session_data) {
    int rv;

    rv = nghttp2_session_send(session_data->session);
    if (rv != 0) {
        return -1;
    }
    return 0;
}

static ssize_t grpc_data_provider(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length,
    uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    *data_flags = NGHTTP2_FLAG_END_STREAM;
    http2_session_data *session_data = static_cast<http2_session_data*>(user_data);
    memcpy(buf, static_cast<const void*>(session_data->packet_data.c_str()), session_data->packet_data.size());
    return session_data->packet_data.size();
}

static int sendRpcRequest(http2_session_data *session_data, 
    const std::string& path, 
    const std::string& binary,
    const HTTP2_DATA_CALLBACK& callback) {
    auto nva = std::vector<nghttp2_nv>();
    nva.push_back(MAKE_NV(":method", "POST", strlen("POST")));
    nva.push_back(MAKE_NV(":scheme", "http", strlen("http")));
    nva.push_back(MAKE_NV(":authority", "Bearer", strlen("Bearer")));
    nva.push_back(MAKE_NV(":path", path.c_str(), path.size()));
    nva.push_back(MAKE_NV("content-type", "application/grpc+proto", strlen("application/grpc+proto")));
    nva.push_back(MAKE_NV("grpc-encoding", "gzip", strlen("gzip")));

    char tmp[2014];
    BasePacketWriter packetWriter(tmp, sizeof(tmp), true, true);
    packetWriter.writeUINT8(0);
    packetWriter.writeUINT32(binary.size());
    packetWriter.writeBuffer(binary.c_str(), binary.size());

    session_data->packet_data = std::string(packetWriter.getData(), packetWriter.getPos());
    auto provider = std::make_shared<nghttp2_data_provider>();
    provider->read_callback = grpc_data_provider;

    auto stream_id = nghttp2_submit_request(session_data->session, 
        nullptr, 
        nva.data(),
        nva.size(), 
        provider.get(), 
        session_data);
    session_data->dataCallbacks[stream_id] = callback;

    return stream_id;
}

static bool setupHttp2Connection(const TCPSession::PTR& session, http2_session_data *session_data) {
    session_data->tcpsession = session;

    nghttp2_session_callbacks *callbacks;

    nghttp2_session_callbacks_new(&callbacks);

    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);

    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
        on_frame_recv_callback);

    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        callbacks, on_data_chunk_recv_callback);

    nghttp2_session_callbacks_set_on_stream_close_callback(
        callbacks, on_stream_close_callback);

    nghttp2_session_callbacks_set_on_header_callback(callbacks,
        on_header_callback);

    nghttp2_session_callbacks_set_on_begin_headers_callback(
        callbacks, on_begin_headers_callback);

    nghttp2_session_client_new(&session_data->session, callbacks, session_data);

    nghttp2_session_callbacks_del(callbacks);

    send_client_connection_header(session_data);
    if (session_send(session_data) != 0)
    {
        fprintf(stderr, "has error in session_send\r\n");
        return false;
    }

    return true;
}

#endif // !_HTTP2_WARPPER_H