#include <winsock2.h>

#include "cozyfs.h"

// This file implements an HTTP front-end for CozyFS instances.
// With this, you can access files using a RESTful API.
//
// NOTE: This is work in progress!!

#define CONN_TIMEOUT 60
#define RECV_TIMEOUT 5
#define SEND_TIMEOUT 5
#define INPUT_BUFFER_LIMIT (1<<20)

typedef struct {
	char *ptr;
	int   len;
} string;

#define S(X) ((string) {(X), sizeof(X)-1})

typedef struct {
	int sock_fd;

	char *input_buffer;
	int   input_count;
	int   input_capacity;

	char *output_buffer;
	int   output_count;
	int   output_capacity;

	int error;
	int close_when_flushed;

	int accept_time;
	int last_recv_time;
	int last_send_time;
} Connection;

char mem[1<<20];
CozyFS fs;

int num_conns = 0;
Connection conns[1<<10];

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
} Method;

typedef struct {
	string name;
	string value;
} Header;

#define MAX_HEADERS 256

typedef struct {
	Method method;
	string path;
	int num_headers;
	Header headers[MAX_HEADERS];
} Request;

static int
streq_case_insensitive(string a, string b)
{
	if (a.len != b.len)
		return 0;
	for (int i = 0; i < a.len; i++)
		if (to_lower(a.ptr[i]) != to_lower(b.ptr[i]))
			return 0;
	return 1;
}

static int
parse_content_length(Request *req)
{
	int i = 0;
	while (i < req->num_headers && !streq_case_insensitive(req->headers[i].name, S("Content-Length")))
		i++;
	if (i == req->num_headers)
		return -1;
	char *src = req->headers[i].value.ptr;
	int   len = req->headers[i].value.len;

	int i = 0;
	while (i < len && src[i] == ' ')
		i++;
	if (i == len || !is_digit(src[i]))
		return -1;

	int result = 0;
	do {
		int d = src[i++] - '0';
		if (result > (INT_MAX - d) / 10)
			return -1;
		result = result * 10 + d;
	} while (i < len && is_digit(src[i]));

	if (i < len)
		return -1;

	return result;
}

static int parse(char *src, int len, Request *req)
{
	int i = 0;
	if (2 < len - i
		&& src[i+0] == 'G'
		&& src[i+1] == 'E'
		&& src[i+2] == 'T') {
		req->method = M_GET;
		i += 3;
	} else if (3 < len - i
		&& src[i+0] == 'H'
		&& src[i+1] == 'E'
		&& src[i+2] == 'A'
		&& src[i+3] == 'D') {
		req->method = M_HEAD;
		i += 4;
	} else if (6 < len - i
		&& src[i+0] == 'O'
		&& src[i+1] == 'P'
		&& src[i+2] == 'T'
		&& src[i+3] == 'I'
		&& src[i+4] == 'O'
		&& src[i+5] == 'N'
		&& src[i+6] == 'S') {
		req->method = M_OPTIONS;
		i += 7;
	} else if (4 < len - i
		&& src[i+0] == 'T'
		&& src[i+1] == 'R'
		&& src[i+2] == 'A'
		&& src[i+3] == 'C'
		&& src[i+4] == 'E') {
		req->method = M_TRACE;
		i += 5;
	} else if (2 < len - i
		&& src[i+0] == 'P'
		&& src[i+1] == 'U'
		&& src[i+2] == 'T') {
		req->method = M_PUT;
		i += 3;
	} else if (5 < len - i
		&& src[i+0] == 'D'
		&& src[i+1] == 'E'
		&& src[i+2] == 'L'
		&& src[i+3] == 'E'
		&& src[i+4] == 'T'
		&& src[i+5] == 'E') {
		req->method = M_DELETE;
		i += 6;
	} else if (3 < len - i
		&& src[i+0] == 'P'
		&& src[i+1] == 'O'
		&& src[i+2] == 'S'
		&& src[i+3] == 'T') {
		req->method = M_POST;
		i += 4;
	} else if (i+4 < len
		&& src[i+0] == 'P'
		&& src[i+1] == 'A'
		&& src[i+2] == 'T'
		&& src[i+3] == 'C'
		&& src[i+4] == 'H') {
		req->method = M_PATCH;
		i += 5;
	} else if (i+6 < len
		&& src[i+0] == 'C'
		&& src[i+1] == 'O'
		&& src[i+2] == 'N'
		&& src[i+3] == 'N'
		&& src[i+4] == 'E'
		&& src[i+5] == 'C'
		&& src[i+6] == 'T') {
		req->method = M_CONNECT;
		i += 6;
	} else {
		return -1;
	}

	int off = i;
	while (i < len && src[i] != ' ')
		i++;
	if (i == len)
		return -1;
	req->path.ptr = src + off;
	req->path.len = i - off;

	if (5 >= len - i
		|| src[i+0] != ' '
		|| src[i+1] != 'H'
		|| src[i+2] != 'T'
		|| src[i+3] != 'T'
		|| src[i+4] != 'P'
		|| src[i+5] != '/')
		return -1;
	i += 6;

	int major;
	int minor;
	if (2 < len - i
		&& src[i+0] == '1'
		&& src[i+1] == '.'
		&& src[i+2] == '1') {
		major = 1;
		minor = 1;
		i += 3;
	} else if (2 < len - i
		&& src[i+0] == '1'
		&& src[i+1] == '.'
		&& src[i+2] == '0') {
		major = 1;
		minor = 0;
		i += 3;
	} else if (2 < len - i
		&& src[i+0] == '1') {
		major = 1;
		minor = 0;
		i++;
	} else {
		return -1;
	}

	req->num_headers = 0;
	for (;;) {

		if (1 < len - i
			&& src[i+0] == '\r'
			&& src[i+1] == '\n') {
			i += 2;
			break;
		}

		int off = i;
		while (i < len && src[i] != ':')
			i++;
		if (i == len)
			return -1; // Missing ':'
		char *name = src + off;
		int   name_len = i - off;

		i++;

		off = i;
		while (i < len && src[i] != '\r') // TODO: This is not good
			i++;
		if (i == len)
			return -1;
		char *value = src + off;
		int   value_len = i - off;

		if (1 >= len - i
			|| src[i+0] != '\r'
			|| src[i+1] != '\n')
			return -1;
		i += 2;

		if (req->num_headers < COUNT(req->headers)) {
			req->headers[req->num_headers++] = (Header) {
				(string) { name, name_len },
				(string) { value, value_len },
			};
		}
	}

	return 0;
}

static string status_text(int code)
{
	switch(code)
	{
		case 100: return S("Continue");
		case 101: return S("Switching Protocols");
		case 102: return S("Processing");

		case 200: return S("OK");
		case 201: return S("Created");
		case 202: return S("Accepted");
		case 203: return S("Non-Authoritative Information");
		case 204: return S("No Content");
		case 205: return S("Reset Content");
		case 206: return S("Partial Content");
		case 207: return S("Multi-Status");
		case 208: return S("Already Reported");

		case 300: return S("Multiple Choices");
		case 301: return S("Moved Permanently");
		case 302: return S("Found");
		case 303: return S("See Other");
		case 304: return S("Not Modified");
		case 305: return S("Use Proxy");
		case 306: return S("Switch Proxy");
		case 307: return S("Temporary Redirect");
		case 308: return S("Permanent Redirect");

		case 400: return S("Bad Request");
		case 401: return S("Unauthorized");
		case 402: return S("Payment Required");
		case 403: return S("Forbidden");
		case 404: return S("Not Found");
		case 405: return S("Method Not Allowed");
		case 406: return S("Not Acceptable");
		case 407: return S("Proxy Authentication Required");
		case 408: return S("Request Timeout");
		case 409: return S("Conflict");
		case 410: return S("Gone");
		case 411: return S("Length Required");
		case 412: return S("Precondition Failed");
		case 413: return S("Request Entity Too Large");
		case 414: return S("Request-URI Too Long");
		case 415: return S("Unsupported Media Type");
		case 416: return S("Requested Range Not Satisfiable");
		case 417: return S("Expectation Failed");
		case 418: return S("I'm a teapot");
		case 420: return S("Enhance your calm");
		case 422: return S("Unprocessable Entity");
		case 426: return S("Upgrade Required");
		case 429: return S("Too many requests");
		case 431: return S("Request Header Fields Too Large");
		case 449: return S("Retry With");
		case 451: return S("Unavailable For Legal Reasons");

		case 500: return S("Internal Server Error");
		case 501: return S("Not Implemented");
		case 502: return S("Bad Gateway");
		case 503: return S("Service Unavailable");
		case 504: return S("Gateway Timeout");
		case 505: return S("HTTP Version Not Supported");
		case 509: return S("Bandwidth Limit Exceeded");
	}
	return S("???");
}

static char *write_bytes_ptr(Connection *c, int mincap, int *cap)
{
	if (c->error) return NULL;
	if (c->output_capacity - c->output_count < mincap) {
		int x = MAX(mincap, 2 * c->output_capacity);
		void *p = malloc(x);
		if (p == NULL) {
			c->error = 1;
			return NULL;
		}
		if (c->output_capacity) {
			memcpy(p, c->output_buffer, c->output_count);
			free(c->output_buffer);
		}
		c->output_buffer = p;
		c->output_capacity = x;
	}
	*cap = c->output_capacity - c->output_count;
	return c->output_buffer + c->output_count;
}

static void write_bytes_ack(Connection *c, int num)
{
	if (c->error) return;
	c->output_count += num;
}

static void write_head(Connection *c, int status)
{
	if (c->error) return;

	int   cap;
	char *dst;
	
	dst = write_bytes_ptr(c, 1<<9, &cap);
	if (dst == NULL) return;

	int len = snprintf(dst, cap, "HTTP/1.1 %d %s\r\n", status, status_text(status));
	if (len < 0) {
		c->error = 1;
		return;
	}
	write_bytes_ack(c, len);
}

static void process_single_request(Connection *c, Request *req, CozyFS *fs)
{
	char path[1<<10];
	if (req->path.len >= sizeof(path)) {
		// TODO
	}
	memcpy(path, req->path.ptr, req->path.len);
	path[req->path.len] = '\0';

	switch (req->method) {

		case M_TRACE:
		case M_CONNECT:
		case M_POST:
		write_head(c, 405); // 405 Method Not Allowed
		write_bytes(c, S("Allow: OPTIONS, GET, HEAD, PUT, DELETE, PATCH\r\n"));
		break;

		case M_OPTIONS:
		{
			// TODO
		}
		break;

		case M_GET:
		case M_HEAD:
		{
			int fd = cozyfs_open(fs, path);
			if (fd < 0) {
				if (fd == -COZYFS_ENOENT) {
					write_head(c, 404);
					write_bytes(c, S("Connection: Close\r\n"));
					write_bytes(c, S("Content-Length: 0\r\n"));
					write_bytes(c, S("\r\n"));
				} else {
					write_head(c, 500);
					write_bytes(c, S("Connection: Close\r\n"));
					write_bytes(c, S("Content-Length: 0\r\n"));
					write_bytes(c, S("\r\n"));
				}
				return;
			}

			write_head(c, 200);
			write_bytes(c, S("Connection: Keep-Alive\r\n"));
			write_bytes(c, S("Content-Length: ???\r\n"));
			write_bytes(c, S("Content-Type: text/plain\r\n"));
			write_bytes(c, S("\r\n"));
			for (;;) {

				int cap;
				char * dst = write_bytes_ptr(c, 1<<9, &cap);
				if (dst == NULL) return;

				int num = cozyfs_read(fs, fd, dst, cap, 0);
				if (num < 0) {
					// TODO
				}
				if (num == 0)
					break;

				write_bytes_ack(c, num);
			}

			// TODO
		}
		break;

		case M_PUT:
		{
			// TODO
		}
		break;

		case M_DELETE:
		{
			// TODO
		}
		break;

		case M_PATCH:
		{
			// TODO
		}
		break;
	}

	// TODO
}

static int process_queued_requests(Connection *c, CozyFS *fs)
{
	for (;;) {

		// Look for the CRLFCRLF
		int k = 0;
		while (3 < c->input_count - k && (c->input_buffer[k+0] != '\r' || c->input_buffer[k+1] != '\n' || c->input_buffer[k+2] != '\r' || c->input_buffer[k+3] != '\n'))
			k++;
		if (3 >= c->input_count - k)
			break;

		char *head = c->input_buffer;
		int head_len = k + 4;

		Request req;
		int code = parse(head, head_len, &req);
		if (code < 0) {
			write_head(c, 400);
			write_bytes(c, S("Connection: Close\r\n"));
			write_bytes(c, S("Content-Length: 0\r\n"));
			write_bytes(c, S("\r\n"));
			c->close_when_flushed = 1;
			return 0;
		}

		int content_len = parse_content_length(&req);
		if (content_len < 0) {
			write_head(c, 411);
			write_bytes(c, S("Connection: Close\r\n"));
			write_bytes(c, S("Content-Length: 0\r\n"));
			write_bytes(c, S("\r\n"));
			c->close_when_flushed = 1;
			return 0;
		}

		int total_len = head_len + content_len;
		if (total_len > c->input_count) break;

		process_single_request(c, &req, fs);
		if (c->error) {
			write_head(c, 500);
			write_bytes(c, S("Connection: Close\r\n"));
			write_bytes(c, S("Content-Length: 0\r\n"));
			write_bytes(c, S("\r\n"));
			c->close_when_flushed = 1;
			return 0;
		}

		memcpy(c->input_buffer,
			c->input_buffer + total_len,
			c->input_count - total_len);
		c->input_count -= total_len;

	}
	return 0;
}

static int recv_from_conn(Connection *c)
{
	c->last_recv_time = current_time;
	while (c->input_count < INPUT_BUFFER_LIMIT) {

		if (c->input_capacity - c->input_count < 1<<8) {
			int x;
			x = MAX(1<<8, 2 * c->input_capacity);
			x = MIN(x, INPUT_BUFFER_LIMIT);
			void *p = malloc(x);
			if (p == NULL)
				return 1;
			if (c->output_capacity) {
				memcpy(p, c->output_buffer, c->output_count);
				free(c->output_buffer);
			}
			c->output_buffer = p;
			c->output_capacity = x;
		}

		int num = recv(c->sock_fd,
			c->input_buffer + c->input_count,
			c->input_capacity - c->input_count, 0);

		if (num < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return 1;
		}

		c->input_count += num;
	}
	return 0;
}

static int send_to_conn(Connection *c)
{
	c->last_send_time = current_time;

	int sent = 0;
	for (;;) {
		int num = send(c->sock_fd, c->output_buffer + sent, c->output_count - sent, 0);
		if (num < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return 1;
		}

		sent += num;
	}

	memcpy(c->output_buffer, c->output_buffer + sent, c->output_count - sent);
	c->output_count -= sent;

	if (c->output_count == 0 && c->close_when_flushed)
		return 1;
	return 0;
}

static int timeout_of(Connection *c)
{
	int timeout_ms = INT_MAX;
	timeout_ms = MIN(timeout_ms, c->accept_time    + CONN_TIMEOUT * 1000);
	timeout_ms = MIN(timeout_ms, c->last_recv_time + RECV_TIMEOUT * 1000);
	timeout_ms = MIN(timeout_ms, c->last_send_time + SEND_TIMEOUT * 1000);
	return timeout_ms;
}

int main(int argc, char **argv)
{
	int code;

	code = cozyfs_init(mem, sizeof(mem), 9, 0);
	if (code < 0)
		return -1;

	cozyfs_attach(&fs, mem, callback, NULL);

	int accept_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (accept_fd < 0)
		return -1;

	// TODO: Make accept_fd non-blocking

	unsigned short port = 8080;

	struct sockaddr_in bind_buf;
	bind_buf.sin_family = AF_INET;
	bind_buf.sin_port = htons(port);
	bind_buf.sin_addr.s_addr = htonl(INADDR_ANY);
	code = bind(accept_fd, (struct sockaddr*) &bind_buf, sizeof(bind_buf));
	if (code < 0)
		return -1;

	code = listen(accept_fd, 32);
	if (code < 0)
		return -1;

	for (;;) {

		struct pollfd pollarr[1024];
		int pollnum = 0;

		int timeout_ms = INT_MAX;
		for (int i = 0; i < num_conns; i++) {
			Connection *c = &conns[i];

			int events = 0;
			if (!c->close_when_flushed)
				events |= POLLIN;
			if (c->output_count > 0)
				events |= POLLOUT;

			timeout_ms = MIN(timeout_ms, timeout_of(c));
		}
		if (timeout_ms == INT_MAX)
			timeout_ms = -1;

		pollarr[pollnum++] = (struct pollfd) { .fd=accept_fd, .events=POLLIN, .revents=0 };

		int num = WSAPoll(pollarr, pollnum, timeout_ms);
		if (num < 0) continue;

		int current_time = ???;

		if (pollarr[pollnum-1].revents & POLLIN) {
			while (num_conns < COUNT(conns)) {

				int i = 0;
				while (conns[i].sock_fd >= 0)
					i++;

				int client_fd = accept(accept_fd, NULL, NULL);
				if (client_fd < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						break;
					continue;
				}

				// TODO: Mark client_fd as non-blocking

				Connection *c = &conns[num_conns];
				c->sock_fd = client_fd;
				c->input_buffer = NULL;
				c->input_count = 0;
				c->input_capacity = 0;
				c->output_buffer = NULL;
				c->output_count = 0;
				c->output_capacity = 0;
				c->error = 0;
				c->close_when_flushed = 0;
				c->accept_time = current_time;
				c->last_recv_time = current_time;
				c->last_send_time = current_time;

				num_conns++;
			}
		}

		for (int i = 0; i < pollnum-1; i++) {

			struct pollfd *pollitem = &pollarr[i];
			Connection *c = &conns[i];
			int remove = 0;

			if (current_time > timeout_of(c))
				remove = 1;

			if (!remove && (pollitem->revents & POLLIN)) {
				remove = recv_from_conn(c);
				if (!remove)
					remove = process_queued_requests(c, fs);
			}

			if (!remove && (pollitem->revents & POLLOUT))
				remove = send_to_conn(c);

			if (remove) {
				close(c->sock_fd);
				free(c->input_buffer);
				free(c->output_buffer);
				c->sock_fd = -1;
			}
		}
	}

	close(accept_fd);
	return 0;
}
