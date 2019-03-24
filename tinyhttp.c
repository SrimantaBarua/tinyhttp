#define _GNU_SOURCE

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>


#define SERVER_NAME    "tinyhttp"
#define SERVER_VER     "0.0.1"
#define SERVER_AUTHOR  "Srimanta Barua <srimanta.barua1@gmail.com>"


#define DEFAULT_BACKLOG 32
#define BUFSZ           4096


// Current working directory. This is the root of HTTP server
const char *CWD = NULL;


// Information about a client
struct client {
	int                     fd;
	struct sockaddr_storage addr;
	socklen_t               addr_size;
	size_t                  rbuf_len;
	size_t                  wbuf_len;
	char                    rbuf[BUFSZ];
	char                    wbuf[BUFSZ];
};


// Get socket to listen on given port
static int get_socket(const char *port, int backlog) {
	int status, sock, yes = 1;
	struct addrinfo hints, *res, *ptr;
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if ((status = getaddrinfo(NULL, port, &hints, &res))) {
		fprintf(stderr, "[ERROR]: getaddrinfo(): %s\n", gai_strerror(status));
		return -1;
	}
	for (ptr = res; ptr;  ptr = ptr->ai_next) {
		if ((sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol)) < 0) {
			perror("[ERROR]: socket()");
			continue;
		}
		if (bind(sock, res->ai_addr, res->ai_addrlen) < 0) {
			perror("[ERROR]: bind()");
			close(sock);
			continue;
		}
		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
			perror("[ERROR]: setsockopt()");
			close(sock);
			continue;
		}
		if (listen(sock, backlog) < 0) {
			perror("[ERROR]: listen()");
			close(sock);
			continue;
		}
		freeaddrinfo(res);
		return sock;
	}
	fprintf(stderr, "[ERROR]: Could not listen on port: %s\n", port);
	freeaddrinfo(res);
	return -1;
}


// Send response to GET request
static void send_get_response(struct client *client, const char *uri, const char *proto,
		const char *ipstr, unsigned port) {
	time_t t;
	struct tm tm;
	char date[64], *path;
	// Get path
	if (asprintf(&path, "%s/%s", CWD, uri) < 0) {
		perror("[ERROR]: asprintf()");
		return;
	}
	free(path);
	// Send 404
	t = time(NULL);
	gmtime_r(&t, &tm);
	strftime(date, sizeof(date), "%a, %d %b %y %H:%M:%S", &tm);
	client->wbuf_len = snprintf(client->wbuf, BUFSZ,
			"%s 404 Not Found\r\nServer: %s %s\r\nDate: %s\r\n\r\n",
			proto, SERVER_NAME, SERVER_VER, date);
	fprintf(stderr, "[INFO]: Response to client: %s:%u\n%s\n", ipstr, port, client->wbuf);
	while (write(client->fd, client->wbuf, client->wbuf_len) < 0) {
		if (errno == EAGAIN) {
			continue;
		}
		perror("[ERROR]: write()");
		break;
	}
}


// Send 501 Not Implemented
static void send_unsupported(struct client *client, const char *proto, const char *ipstr,
		unsigned port) {
	time_t t;
	struct tm tm;
	char date[64];
	// No. Send 501
	t = time(NULL);
	gmtime_r(&t, &tm);
	strftime(date, sizeof(date), "%a, %d %b %y %H:%M:%S", &tm);
	client->wbuf_len = snprintf(client->wbuf, BUFSZ,
			"%s 501 Not Implemented\r\nServer: %s %s\r\nDate: %s\r\n\r\n",
			proto, SERVER_NAME, SERVER_VER, date);
	fprintf(stderr, "[INFO]: Response to client: %s:%u\n%s\n", ipstr, port, client->wbuf);
	while (write(client->fd, client->wbuf, client->wbuf_len) < 0) {
		if (errno == EAGAIN) {
			continue;
		}
		perror("[ERROR]: write()");
		break;
	}
}


// Client thread
static void* client_thread(void *arg) {
	ssize_t ret;
	struct client *client = (struct client*) arg;
	char ipstr[INET6_ADDRSTRLEN];
	const char *req, *uri, *proto, *ptr;
	void *ipaddr;
	unsigned port;

	if (client->addr.ss_family == AF_INET) {
		ipaddr = &((struct sockaddr_in*) &client->addr)->sin_addr;
		port = ((struct sockaddr_in*) &client->addr)->sin_port;
	} else {
		ipaddr = &((struct sockaddr_in6*) &client->addr)->sin6_addr;
		port = ((struct sockaddr_in*) &client->addr)->sin_port;
	}
	inet_ntop(client->addr.ss_family, ipaddr, ipstr, sizeof(ipstr));

	// Read message
	client->rbuf_len = 0;
	client->rbuf[client->rbuf_len] = '\0';
	while ((ret = read(client->fd, client->rbuf + client->rbuf_len,
				BUFSZ - client->rbuf_len - 1)) < 0) {
		if (errno == EAGAIN) {
			continue;
		}
		perror("[ERROR]: read()");
		close(client->fd);
		free(client);
		return NULL;
	}
	// Handle client closed
	if (ret == 0) {
		fprintf(stderr, "[INFO]: Connection closed from client: %s:%u\n", ipstr, port);
		close(client->fd);
		free(client);
		return NULL;
	}
	client->rbuf_len += ret;
	client->rbuf[client->rbuf_len] = '\0';
	fprintf(stderr, "[INFO]: Request from client: %s:%u\n%s\n", ipstr, port, client->rbuf);

	// Parse request
	ptr = client->rbuf;
	req = strtok((char*) ptr, " \t\r\n");
	uri = strtok(NULL, " \t");
	proto = strtok(NULL, " \t\r\n");

	// Do we know this protocol?
	if (strcmp(proto, "HTTP/1.1")) {
		send_unsupported(client, "HTTP/1.1", ipstr, port);
		close(client->fd);
		free(client);
		return NULL;
	}
	// Is it a request we support?
	if (!strcmp(req, "GET")) {
		// Send GET response
		send_get_response(client, uri, proto, ipstr, port);
	} else {
		send_unsupported(client, proto, ipstr, port);
	}
	close(client->fd);
	free(client);
	return NULL;
}


// Print usage/help
static void print_usage(const char *progname) {
	fprintf(stderr, "USAGE: %s <port>\n", progname);
}


int main(int argc, const char **argv) {
	int sock, backlog, client_fd;
	struct sockaddr_storage client_addr;
	socklen_t addr_size;
	struct client *client;
	char ipstr[INET6_ADDRSTRLEN];
	void *ipaddr;
	unsigned port;
	pthread_t tid;

	fprintf(stderr, "%s %s\n(C) 2019 %s\n\n", SERVER_NAME, SERVER_VER, SERVER_AUTHOR);
	if (argc != 2) {
		print_usage(argv[0]);
		return 1;
	}
	backlog = DEFAULT_BACKLOG;
	if ((sock = get_socket(argv[1], backlog)) < 0) {
		return 1;
	}
	fprintf(stderr, "[INFO]: Server started listening on port: %s\n", argv[1]);
	// Get CWD
	if (!(CWD = get_current_dir_name())) {
		perror("[ERROR]: get_current_dir_name()");
		close(sock);
		return 1;
	}
	while (1) {
		addr_size = sizeof(client_addr);
		// Accept connection
		while ((client_fd = accept(sock, (struct sockaddr*) &client_addr, &addr_size)) < 0) {
			if (errno == EAGAIN) {
				continue;
			}
			perror("[ERROR]: accept()");
			close(sock);
			return 1;
		}
		// Allocate client struct
		if (!(client = calloc(1, sizeof(struct client)))) {
			perror("[ERROR]: calloc()");
			close(client_fd);
			close(sock);
			return 1;
		}
		client->fd = client_fd;
		client->addr_size = addr_size;
		memcpy(&client->addr, &client_addr, sizeof(client_addr));
		if (client_addr.ss_family == AF_INET) {
			ipaddr = &((struct sockaddr_in*) &client_addr)->sin_addr;
			port = ((struct sockaddr_in*) &client->addr)->sin_port;
		} else {
			ipaddr = &((struct sockaddr_in6*) &client_addr)->sin6_addr;
			port = ((struct sockaddr_in*) &client->addr)->sin_port;
		}
		inet_ntop(client_addr.ss_family, ipaddr, ipstr, sizeof(ipstr));
		fprintf(stderr, "[INFO]: Incoming connection from: %s:%u\n", ipstr, port);
		// Create client thread
		if (pthread_create(&tid, NULL, client_thread, (void*) client)) {
			perror("[ERROR]: pthread_create()");
			free(client);
			close(client_fd);
			close(sock);
			return 1;
		}
		// Detach thread
		if (pthread_detach(tid)) {
			perror("[ERROR]: pthread_detach()");
			close(sock);
			return 1;
		}
	}
	close(sock);
	return 0;
}
