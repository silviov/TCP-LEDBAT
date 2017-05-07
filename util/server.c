/* Server code in C */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DIRSIZE     8192

typedef struct _options {
	//      char *hostname;
	uint16_t port;
	//      uint32_t count;
} options_data;

void print_help(char *progname)
{
	printf("Usage: %s [-p port]\n", progname);
}

void parse_opt(int argc, char *argv[], options_data * opts)
{
	int opt;

	opts->port = 5001;	//same as iperf

	while ((opt = getopt(argc, argv, "p:")) != -1) {
		switch (opt) {
		case 'p':
			opts->port = atoi(optarg);
			break;
		default:
		case 'h':
			print_help(argv[0]);
			exit(EXIT_SUCCESS);
		}
	}

	return;
}

void my_rec(int rec_socket)
{
	char buffer[DIRSIZE];
	for (;;) {
		if (recv(rec_socket, buffer, DIRSIZE, 0) < 0) {
			perror("recv");
			exit(1);
		}
	}
}

int main(int argc, char *argv[])
{
	options_data options;
	struct sockaddr_in sockaddr;
	int sfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	int cfd;
	pid_t pid;

	parse_opt(argc, argv, &options);

	if (-1 == sfd) {
		perror("Cannot create socket");
		exit(EXIT_FAILURE);
	}

	memset(&sockaddr, 0, sizeof(sockaddr));

	sockaddr.sin_family = AF_INET;
	sockaddr.sin_port = htons(options.port);
	sockaddr.sin_addr.s_addr = INADDR_ANY;

	if (-1 == bind(sfd, (const void *)&sockaddr, sizeof(sockaddr))) {
		perror("Socket bind failed");
		close(sfd);
		exit(EXIT_FAILURE);
	}

	if (-1 == listen(sfd, 10)) {
		perror("Listen failed");
		close(sfd);
		exit(EXIT_FAILURE);
	}

	for (;;) {
		int cfd = accept(sfd, NULL, NULL);

		if (0 > cfd) {
			perror("Accept failed");
			close(sfd);
			exit(EXIT_FAILURE);
		}

		switch (pid = fork()) {
		case 0:	//child process
			close(sfd);
			my_rec(cfd);
			exit(0);
			break;
		case -1:	//error
			perror("fork");
			close(cfd);
			exit(1);
			break;
		default:	//parent process
			shutdown(cfd, SHUT_RDWR);
			break;
		}

	}

	close(cfd);
	return 0;
}
