#include "aws.h"
#include "http_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "util.h"
#include "debug.h"
#include "sock_util.h"
#include "w_epoll.h"
#include <string.h>
#include <libaio.h>
#include <sys/eventfd.h>

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static http_parser parser;
static char request_path[BUFSIZ];
enum connection_state {
	STATE_DATA_RECEIVED,
	STATE_DATA_SENT,
	STATE_CONNECTION_CLOSED,
    STATE_READING_REQUEST,
    STATE_SENDING_HEADER,
    STATE_SENDING_FILE,
    STATE_HEADER_SENT,
    STATE_FILE_SENT,
    STATE_RECEIVING_HEADER
};

static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
	assert(p == &parser);
	memcpy(request_path, buf, len);

	return 0;
}

static http_parser_settings settings_on_path = {
	/* on_message_begin */ 0,
	/* on_header_field */ 0,
	/* on_header_value */ 0,
	/* on_path */ on_path_cb,
	/* on_url */ 0,
	/* on_fragment */ 0,
	/* on_query_string */ 0,
	/* on_body */ 0,
	/* on_headers_complete */ 0,
	/* on_message_complete */ 0
};

/* structure acting as a connection handler */
struct connection {
	int sockfd;
	/* buffers used for receiving messages and then echoing them back */
	char recv_buffer[BUFSIZ];
	size_t recv_len;
	char send_buffer[BUFSIZ];
    char path[BUFSIZ];
	size_t send_len;
    ssize_t sent;
    ssize_t sent_header;
    ssize_t revc;
    int type; // 0 - error, 1 - static, 2 - dynamic
    int fd;
    int efd;
    io_context_t ctx;
    struct iocb *iocb;
    struct iocb **piocb;
    char **buffers;
    ssize_t recv_header;
    ssize_t file_size;
    int read_ops;
    int write_ops;
    int nr_buffers;
    int last;
    ssize_t *sent_buffers;
    ssize_t *sizes;
	enum connection_state state;
};

/*
 * Initialize connection structure on given socket.
 */

static struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(*conn));

	DIE(conn == NULL, "malloc");

	conn->sockfd = sockfd;
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
    conn->sent = 0;
    conn->file_size = 0;
    conn->recv_len = 0;
    conn->read_ops = 0;
    conn->nr_buffers = 0;
    conn->write_ops = 0;
    conn->last = 0;
	return conn;
}

/*
 * Remove connection handler.
 */

static void connection_remove(struct connection *conn)
{
	close(conn->sockfd);
    close(conn->fd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

/*
 * Handle a new connection request on the server socket.
 */

static void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	/* accept new connection */
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

    // setting the nonblock flag on the socket
    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

	/* instantiate new connection handler */
	conn = connection_create(sockfd);

	/* add socket to epoll */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

/*
 * Receive message on socket.
 * Store message in recv_buffer in struct connection.
 */

static enum connection_state receive_message(struct connection *conn)
{
	ssize_t bytes_recv;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}


	bytes_recv = recv(conn->sockfd, conn->recv_buffer + conn->recv_len, BUFSIZ, 0);
    conn->recv_len += bytes_recv;
    
    if (strcmp(conn->recv_buffer + conn->recv_len - 4, "\r\n\r\n") == 0) {
        conn->state = STATE_DATA_RECEIVED;
        return STATE_DATA_RECEIVED;
    } else {
        conn->state = STATE_RECEIVING_HEADER;
        return STATE_RECEIVING_HEADER;    
    }

	if (bytes_recv < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication from: %s\n", abuffer);
		goto remove_connection;
	}

	if (bytes_recv == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed from: %s\n", abuffer);
		goto remove_connection;
	}

	dlog(LOG_DEBUG, "Received message from: %s\n", abuffer);

	conn->recv_len = bytes_recv;
	conn->state = STATE_DATA_RECEIVED;

	return STATE_DATA_RECEIVED;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * Send message on socket.
 * Store message in send_buffer in struct connection.
 */

static enum connection_state send_message(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

    size_t bytes_sent_header = send(conn->sockfd, conn->send_buffer + conn->sent_header, conn->send_len - conn->sent_header, 0);
    conn->sent_header += bytes_sent_header;
    
    // the header was not entirely sent
    if (bytes_sent_header != 0) {
        conn->state = STATE_SENDING_HEADER;
        return STATE_SENDING_HEADER;
    }

    printf("--\n%s--\n", conn->send_buffer);
    // header has been sent; if we have an error
    // close the connection or start sending the file
    if (conn->type == 0) {
        conn->state = STATE_FILE_SENT;
        return STATE_FILE_SENT;
    } else {
        conn->state = STATE_HEADER_SENT;
        return STATE_HEADER_SENT;
    }

	if (bytes_sent < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_sent == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
		goto remove_connection;
	}

	dlog(LOG_DEBUG, "Sending message to %s\n", abuffer);


	/* all done - remove out notification */
	DIE(rc < 0, "w_epoll_update_ptr_in");

	conn->state = STATE_DATA_SENT;

	return STATE_DATA_SENT;

remove_connection:

	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * Handle a client request on a client connection.
 */

static void handle_client_request(struct connection *conn)
{
	int rc;
	enum connection_state ret_state;

	ret_state = receive_message(conn);
	if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_RECEIVING_HEADER)
		return;

    // initialize parser and parse the http request
    http_parser_init(&parser, HTTP_REQUEST);
    memset(request_path, 0, BUFSIZ);
    memset(conn->path, 0, BUFSIZ);
    http_parser_execute(&parser, &settings_on_path, conn->recv_buffer, strlen(conn->recv_buffer));

    // build the file path
    strcpy(conn->path, ".");
    strcat(conn->path, request_path);

    // open the file to be sent
    conn->fd = open(conn->path, O_RDWR);

    // constructing the headers; if we couldn't open the file, sending a 404 error
    // else sending a 200 OK response
    if (conn->fd == -1) {
        sprintf(conn->send_buffer, "HTTP/1.1 404 Not Found\r\n"
                                    "Date: Tue, 04 Aug 2009 07:59:32 GMT\r\n"
                                    "Server: Apache\r\n"
                                    "Content-Length: 0\r\n"
                                    "X-Powered-By: Servlet/2.5 JSP/2.1\r\n"
                                    "Content-Type: text/xml; charset=utf-8\r\n"
                                    "Connection: close\r\n"
                                    "\r\n");
        conn->send_len = strlen(conn->send_buffer);
        conn->type = 0;
    } else {
        // getting the size of the file
        struct stat stat_buf;
        fstat(conn->fd, &stat_buf);
        conn->file_size = stat_buf.st_size;

        // allocating buffers for aio
        conn->nr_buffers = conn->file_size / BUFSIZ + (conn->file_size % BUFSIZ != 0);
        conn->buffers = (char **) malloc(conn->nr_buffers * sizeof(char *));
        for (int i = 0; i < conn->nr_buffers; i++)
            conn->buffers[i] = (char *) malloc(BUFSIZ * sizeof(char));

        conn->sent_buffers = (ssize_t *) calloc(conn->nr_buffers, sizeof(ssize_t));
        conn->sizes = (ssize_t *) calloc(conn->nr_buffers, sizeof(ssize_t));
        sprintf(conn->send_buffer, "HTTP/1.1 200 OK\r\n"
                                    "Date: Tue, 04 Aug 2009 07:59:32 GMT\r\n"
                                    "Server: Apache\r\n"
                                    "Content-Length: %ld\r\n"
                                    "X-Powered-By: Servlet/2.5 JSP/2.1\r\n"
                                    "Content-Type: text/xml; charset=utf-8\r\n"
                                    "Connection: close\r\n"
                                    "\r\n", conn->file_size);
        conn->send_len = strlen(conn->send_buffer);

        // checking whether we have a static or dynamic file
        if (strstr(conn->path, "static")) {
            conn->type = 1;
        } else {
            conn->type = 2;
        }
    }

    /* add socket to epoll for out events */
    rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_add_ptr_inout");
}

// sending static file using sendfile function
static void send_file(struct connection *conn) {
    ssize_t bytes_sent = sendfile(conn->sockfd, conn->fd, &conn->sent, conn->file_size - conn->sent);
    if (bytes_sent == 0) {
        conn->state = STATE_FILE_SENT;
    } else {
        conn->state = STATE_SENDING_FILE;
    }
}

// waiting for aio events using io_getevents
static void wait_aio(io_context_t ctx, int n_ops) {
    struct io_event *events;
    int rc;

    events = (struct io_event *) malloc(n_ops * sizeof(struct io_event));

    rc = io_getevents(ctx, n_ops, n_ops, events, NULL);
    DIE(rc != n_ops, "io_getevents");
    free(events);
}

// reading from file using aio operations
static void read_aio(struct connection *conn) {

    int n_ops = io_submit(conn->ctx, conn->nr_buffers, conn->piocb);
    conn->read_ops += n_ops;
    wait_aio(conn->ctx, n_ops);

}

// allocating the structures needed for aio and preparing the read events, and the context
static void prepare_events(struct connection *conn) {
    conn->efd = eventfd(0, EFD_NONBLOCK);

    conn->iocb = (struct iocb *) malloc(conn->nr_buffers * sizeof(*(conn->iocb)));
    conn->piocb = (struct iocb **) malloc(conn->nr_buffers * sizeof(*(conn->piocb)));

    ssize_t total = 0;
    for (int i = 0; i < conn->nr_buffers; i++) {
        conn->sizes[i] = BUFSIZ;
        if (total + conn->sizes[i] > conn->file_size) {
            conn->sizes[i] = conn->file_size - total;
        }
        io_prep_pread(&(conn->iocb[i]), conn->fd, conn->buffers[i], BUFSIZ, i * BUFSIZ);
        conn->piocb[i] = &(conn->iocb[i]);
        io_set_eventfd(conn->piocb[i], conn->efd);
    }
    io_setup(conn->nr_buffers, &conn->ctx);
}

// sending dynamic file using nonblocking sockets
static void send_file_aio(struct connection *conn) {
    ssize_t bytes_sent = send(conn->sockfd, conn->buffers[conn->last] + conn->sent_buffers[conn->last],
                                 conn->sizes[conn->last] - conn->sent_buffers[conn->last], 0);
    conn->sent_buffers[conn->last] += bytes_sent;
    if (bytes_sent == 0) {
        conn->last++;
        if (conn->last == conn->nr_buffers)
            conn->state = STATE_FILE_SENT;
    }
}

int main(void)
{
	int rc;

	/* init multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* create server socket */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT,
		DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* wait for events */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/*
		 * switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */

		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			if (rev.events & EPOLLIN) {
				handle_client_request(rev.data.ptr);
			}
			if (rev.events & EPOLLOUT) {
                struct connection *conn = (struct connection *)(rev.data.ptr);

                // sending the header
                if (conn->state == STATE_DATA_RECEIVED || conn->state == STATE_SENDING_HEADER) {
				    send_message(rev.data.ptr);
                }
                
                // sending the file; if the connection type is 1 sends a static file, else a dynamic one
                if (conn->state == STATE_HEADER_SENT || conn->state == STATE_SENDING_FILE) {
                    if (conn->type == 1) {
                        send_file(rev.data.ptr);
                    } else {
                        if (conn->read_ops == 0) {
                            prepare_events(conn);
                        }

                        if (conn->read_ops != conn->nr_buffers) {
                            read_aio(conn);
                            continue;
                        }
                        
                        send_file_aio(conn);
                    }
                }
                
                // deallocating the structures and closing the file and socket when connection ends
                if (conn->state == STATE_FILE_SENT) {
                    rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
                    if (conn->type == 2) {
                        free(conn->iocb);
                        free(conn->piocb);
                        close(conn->efd);
                        io_destroy(conn->ctx);
                    }
                    for (int i = 0; i < conn->nr_buffers; i++)
                        free(conn->buffers[i]);
                    free(conn->sizes);
                    free(conn->buffers);
                    close(conn->sockfd);
                    close(conn->fd);
                    free(conn);
                }
			}
		}
	}
	return 0;
}
