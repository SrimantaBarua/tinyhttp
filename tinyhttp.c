#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>


#define SERVER_NAME    "tinyhttp"
#define SERVER_VER     "0.0.1"
#define SERVER_AUTHOR  "Srimanta Barua <srimanta.barua1@gmail.com>"


#define DEFAULT_BACKLOG 32


// Information about a client
struct client {
	int                     fd;
	struct sockaddr_storage addr;
	socklen_t               addr_size;
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
		printf("[ERROR]: getaddrinfo(): %s\n", gai_strerror(status));
		return -1;
	}
	for (ptr = res; ptr;  ptr = ptr->ai_next) {
		if ((sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol)) < 0) {
			continue;
		}
		if (bind(sock, res->ai_addr, res->ai_addrlen) < 0) {
			close(sock);
			continue;
		}
		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
			close(sock);
			continue;
		}
		if (listen(sock, backlog) < 0) {
			close(sock);
			continue;
		}
		freeaddrinfo(res);
		return sock;
	}
	printf("[ERROR]: Could not listen on port: %s\n", port);
	freeaddrinfo(res);
	return -1;
}


// Print usage/help
static void print_usage(const char *progname) {
	printf("USAGE: %s <port>\n", progname);
}


int main(int argc, const char **argv) {
	int sock, backlog, client_fd;
	struct sockaddr_storage client_addr;
	socklen_t addr_size;
	struct client *client;
	char ipstr[INET6_ADDRSTRLEN];
	void *ipaddr;

	printf("%s %s\n(C) 2019 %s\n\n", SERVER_NAME, SERVER_VER, SERVER_AUTHOR);
	if (argc != 2) {
		print_usage(argv[0]);
		return 1;
	}
	backlog = DEFAULT_BACKLOG;
	if ((sock = get_socket(argv[1], backlog)) < 0) {
		return 1;
	}
	printf("[INFO]: Server started listening on port: %s\n", argv[1]);
	while (1) {
		addr_size = sizeof(client_addr);
		while ((client_fd = accept(sock, (struct sockaddr*) &client_addr, &addr_size)) < 0) {
			if (errno == EAGAIN) {
				continue;
			}
			perror("[ERROR]: accept()");
			close(client_fd);
			close(sock);
			return 1;
		}
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
		} else {
			ipaddr = &((struct sockaddr_in6*) &client_addr)->sin6_addr;
		}
		inet_ntop(client_addr.ss_family, ipaddr, ipstr, sizeof(ipstr));
		printf("[INFO]: Incoming connection from: %s\n", ipstr);
		free(client);
	}
	close(sock);
	return 0;
}
