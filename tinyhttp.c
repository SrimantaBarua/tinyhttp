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
#include <dirent.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>


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
		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
			perror("[ERROR]: setsockopt()");
			close(sock);
			continue;
		}
		if (bind(sock, res->ai_addr, res->ai_addrlen) < 0) {
			perror("[ERROR]: bind()");
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


// List directory
static int send_dir(struct client *client, const char *path, const char *proto, const char *uri) {
	DIR *dir;
	size_t len = 0;
	struct dirent *ent;
	time_t t;
	struct tm tm;
	char date[64];
	int ret;

	// Get time
	t = time(NULL);
	gmtime_r(&t, &tm);
	strftime(date, sizeof(date), "%a, %d %b %y %H:%M:%S", &tm);

	// Get length
	if (!(dir = opendir(path))) {
		perror("[ERROR]: opendir()");
		return -1;
	}
	// Content length
	while ((ent = readdir(dir))) {
		len += 2 * strlen(ent->d_name); // Path + link
		len += 26; // HTML
	}
	closedir(dir);
	len += 31 + 278 + strlen(uri);
	// Write HTTP header and beginning message
	client->wbuf_len = snprintf(client->wbuf, BUFSZ, "%s 200 OK\r\n"
					"Server: %s %s\r\n"
					"Date: %s\r\n"
					"Content-type: text/html; charset=utf-8\r\n"
					"Content-Length: %lu\r\n\r\n"
					"<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
					"\"http://www.w3.org/TR/html4/strict.dtd\">\r\n"
					"<html>\r\n"
					"<head>\r\n"
					"<meta http-equiv=\"Content-Type\"content=\"text/html; "
					"charset=utf-8\">\r\n"
					"<title>Directory listing for %s</title>\r\n"
					"</head>\r\n"
					"<body>\r\n"
					"<h1>Directory listing for %s</h1>\r\n"
					"<hr>\r\n"
					"<ul>\r\n",
					proto, SERVER_NAME, SERVER_VER, date, len, uri, uri);
	while (write(client->fd, client->wbuf, client->wbuf_len) < 0) {
		if (errno == EAGAIN) {
			continue;
		}
		perror("[ERROR]: write()");
		return -1;
	}
	// Dir listing
	if (!(dir = opendir(path))) {
		perror("[ERROR]: opendir()");
		return -1;
	}
	client->wbuf_len = 0;
	while ((ent = readdir(dir))) {
		ret = snprintf(client->wbuf + client->wbuf_len, BUFSZ - client->wbuf_len,
				"<li><a href=\"%s\">%s</a></li>\r\n", ent->d_name, ent->d_name);
		if (ret >= (int) (BUFSZ - client->wbuf_len)) {
			client->wbuf[client->wbuf_len] = '\0';
			while (write(client->fd, client->wbuf, client->wbuf_len) < 0) {
				if (errno == EAGAIN) {
					continue;
				}
				perror("[ERROR]: write()");
				return -1;
			}
			client->wbuf_len = 0;
			ret = snprintf(client->wbuf + client->wbuf_len, BUFSZ - client->wbuf_len,
					"<li><a href=\"%s\">%s</a></li>\r\n",
					ent->d_name, ent->d_name);
		}
		client->wbuf_len += ret;
	}
	closedir(dir);
	if (client->wbuf_len > 0) {
		while (write(client->fd, client->wbuf, client->wbuf_len) < 0) {
			if (errno == EAGAIN) {
				continue;
			}
			perror("[ERROR]: write()");
			return -1;
		}
	}
	// End HTML
	client->wbuf_len = snprintf(client->wbuf, BUFSZ, "</ul>\r\n""<hr>\r\n</body>\r\n</html>\r\n");
	while (write(client->fd, client->wbuf, client->wbuf_len) < 0) {
		if (errno == EAGAIN) {
			continue;
		}
		perror("[ERROR]: write()");
		break;
	}
	return 0;
}


// Send file contents
static int send_file(struct client *client, const char *path, const char *proto) {
	FILE *f, *p;
	size_t flen;
	char *cmd = NULL, mimetype[256], *ptr;
	time_t t;
	struct tm tm;
	char date[64];

	// Get time
	t = time(NULL);
	gmtime_r(&t, &tm);
	strftime(date, sizeof(date), "%a, %d %b %y %H:%M:%S", &tm);

	// Open file
	if (!(f = fopen(path, "r"))) {
		perror("[ERROR]: fopen()");
		return -1;
	}
	// Get file size
	fseek(f, 0L, SEEK_END);
	flen = ftell(f);
	fseek(f, 0L, SEEK_SET);
	// Get mimetype (using "file" command")
	if (asprintf(&cmd, "file --mime-type %s | awk '{print $2}'", path) < 0) {
		perror("[ERROR]: asprintf()");
		fclose(f);
		return -1;
	}
	if (!(p = popen(cmd, "r"))) {
		perror("[ERROR]: popen()");
		free(cmd);
		fclose(f);
		return -1;
	}
	free(cmd);
	if (!fgets(mimetype, sizeof(mimetype), p)) {
		perror("[ERROR]: fgets()");
		pclose(p);
		fclose(f);
		return -1;
	}
	if ((ptr = strchr(mimetype, '\n'))) {
		*ptr = '\0';
	}
	pclose(p);
	// Send header
	client->wbuf_len = snprintf(client->wbuf, BUFSZ, "%s 200 OK\r\nServer: %s %s\r\n"
					"Date: %s\r\nContent-type: %s\r\nContent-Length: %lu\r\n\r\n",
					proto, SERVER_NAME, SERVER_VER, date, mimetype, flen);
	while (write(client->fd, client->wbuf, client->wbuf_len) < 0) {
		if (errno == EAGAIN) {
			continue;
		}
		perror("[ERROR]: write()");
		break;
	}
	// Send file
	while ((client->wbuf_len = fread(client->wbuf, 1, BUFSZ, f))) {
		// Write
		while (write(client->fd, client->wbuf, client->wbuf_len) < 0) {
			if (errno == EAGAIN) {
				continue;
			}
			perror("[ERROR]: write()");
			fclose(f);
			return 0;
		}
	}
	fclose(f);
	return 0;
}


// Send response to GET request
static void send_get_response(struct client *client, const char *uri, const char *proto) {
	time_t t;
	struct tm tm;
	char date[64], *path = NULL, *ptr;
	struct stat stat;

	// Get time
	t = time(NULL);
	gmtime_r(&t, &tm);
	strftime(date, sizeof(date), "%a, %d %b %y %H:%M:%S", &tm);

	// Get path
	if (asprintf(&path, "%s/%s", CWD, uri) < 0) {
		perror("[ERROR]: asprintf()");
		return;
	}
	// Check path
	if (lstat(path, &stat)) {
		fprintf(stderr, "[ERROR]: Could not access path: %s: %s\n", uri, strerror(errno));
		// Is if an index.html file? Not found? Send directory listing
		if ((ptr = strrchr(path, '/'))) {
			ptr++;
		} else {
			ptr = path;
		}
		if (strcmp(ptr, "index.html")) {
			goto send_404;
		}
		*ptr++ = '/';
		*ptr = '\0';
		if (send_dir(client, path, proto, uri) < 0) {
			goto send_404;
		}
		goto out;
	}
	if (S_ISDIR(stat.st_mode)) {
		if (send_dir(client, path, proto, uri) < 0) {
			goto send_404;
		}
		goto out;
	} else if (S_ISREG(stat.st_mode)) {
		if (send_file(client, path, proto) < 0) {
			goto send_404;
		}
		goto out;
	}

send_404:
	client->wbuf_len = snprintf(client->wbuf, BUFSZ,
			"%s 404 Not Found\r\nServer: %s %s\r\nDate: %s\r\n\r\n",
			proto, SERVER_NAME, SERVER_VER, date);
	while (write(client->fd, client->wbuf, client->wbuf_len) < 0) {
		if (errno == EAGAIN) {
			continue;
		}
		perror("[ERROR]: write()");
		break;
	}
out:
	// Free path
	free(path);
}


// Send 501 Not Implemented
static void send_unsupported(struct client *client, const char *proto) {
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
		goto out;
	}
	// Handle client closed
	if (ret == 0) {
		fprintf(stderr, "[INFO]: Connection closed from client: %s:%u\n", ipstr, port);
		goto out;
	}
	client->rbuf_len += ret;
	client->rbuf[client->rbuf_len] = '\0';
	fprintf(stderr, "[INFO]: Request from client: %s:%u\n%s", ipstr, port, client->rbuf);

	// Parse request
	ptr = client->rbuf;
	req = strtok((char*) ptr, " \t\r\n");
	uri = strtok(NULL, " \t");
	proto = strtok(NULL, " \t\r\n");

	// Do we know this protocol?
	if (strcmp(proto, "HTTP/1.1")) {
		send_unsupported(client, "HTTP/1.1");
		goto out;
	}
	// Is it a request we support?
	if (!strcmp(req, "GET")) {
		// Send GET response
		send_get_response(client, uri, proto);
	} else {
		send_unsupported(client, proto);
	}
out:
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
