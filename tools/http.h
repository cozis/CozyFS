// Copyright (c) 2025 Francesco Cozzuto
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this
// software and associated documentation files (the "Software"), to deal in the Software
// without restriction, including without limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
// to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
// PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT
// OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
#ifndef COZYFS_HTTP_H
#define COZYFS_HTTP_H
#include <cozyfs.h>

typedef enum {
	M_GET,
	M_HEAD,
	M_OPTIONS,
	M_TRACE,
	M_PUT,
	M_DELETE,
	M_POST,
	M_PATCH,
	M_CONNECT,
} HTTPMethod;

typedef struct {
	char *name;
	char *value;
	int name_len;
	int value_len;
} HTTPHeader;

#define MAX_HEADERS 256

typedef struct {
	HTTPMethod method;
	char      *path;
	int        path_len;
	int        major;
	int        minor;
	int        num_headers;
	HTTPHeader headers[MAX_HEADERS];
} HTTPRequest;

typedef struct HTTPResponse HTTPResponse;

typedef void (*HTTPCallback)(HTTPRequest *req, HTTPResponse *res, void *userptr);

typedef struct {
	const char *addr;
	int port;
	int conn_timeout_sec;
	int recv_timeout_sec;
	int send_timeout_sec;
	int input_buffer_limit;
	int connection_reuse_limit;
} HTTPServerConfig;

#define HTTP_SERVER_DEFAULT_CONFIG (HTTPServerConfig) {	\
	.addr="127.0.0.1",									\
	.port=8080,											\
	.conn_timeout_sec=60,								\
	.recv_timeout_sec=5,								\
	.send_timeout_sec=5,								\
	.input_buffer_limit=(1<<20),						\
	.connection_reuse_limit=100,						\
}

int   http_serve(HTTPServerConfig config, HTTPCallback callback, void *userptr);
void  http_write_head(HTTPResponse *res, int status);
void  http_write_header(HTTPResponse *res, const char *fmt, ...);
void  http_write_body(HTTPResponse *res, const char *str, int len);
void* http_write_body_ptr(HTTPResponse *res, int mincap, int *cap);
void  http_write_body_ack(HTTPResponse *res, int num);
void  http_restart_response(HTTPResponse *res);

int   cozyfs_http_serve(const char *addr, int port, CozyFS *fs);

#endif // COZYFS_HTTP_H