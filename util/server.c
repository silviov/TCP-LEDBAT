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
	//	char *hostname;
		uint16_t port;
	//	uint32_t count;
} options_data;

void print_help(char *progname) {
	printf("Usage: %s [-p port]\n", progname);
}

void parse_opt(int argc, char *argv[], options_data *opts)
{
		int opt;

		opts->port     = 5001; //same as iperf

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

void my_rec(int rec_socket) {
	char buffer[DIRSIZE];
	int quick_ack = 1;

//	setsockopt(rec_socket, IPPROTO_TCP, TCP_QUICKACK, 
			//&quick_ack, sizeof(quick_ack));
	for (;;) {
		if (recv(rec_socket, buffer, DIRSIZE, 0) < 0){
			perror("recv");
			exit(1);
		}
	}
}

int main(int argc, char *argv[])
{
	options_data options;
	struct sockaddr_in stSockAddr;
	int SocketFD = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	int ConnectFD;
	pid_t pid;


	parse_opt(argc, argv, &options);

	if(-1 == SocketFD)
	{
		perror("can not create socket");
		exit(EXIT_FAILURE);
	}

	memset(&stSockAddr, 0, sizeof(stSockAddr));

	stSockAddr.sin_family = AF_INET;
	stSockAddr.sin_port = htons(options.port);
	stSockAddr.sin_addr.s_addr = INADDR_ANY;

	if(-1 == bind(SocketFD,(const void *)&stSockAddr, sizeof(stSockAddr)))
	{
		perror("error bind failed");
		close(SocketFD);
		exit(EXIT_FAILURE);
	}

	if(-1 == listen(SocketFD, 10))
	{
		perror("error listen failed");
		close(SocketFD);
		exit(EXIT_FAILURE);
	}

	for(;;)
	{
		int ConnectFD = accept(SocketFD, NULL, NULL);

		if(0 > ConnectFD)
		{
			perror("error accept failed");
			close(SocketFD);
			exit(EXIT_FAILURE);
		}

		switch (pid = fork()) {
		case 0: //child process
			close(SocketFD);
			my_rec(ConnectFD);
			exit(0);
			break;
		case -1: //error
			perror("fork");
			close(ConnectFD);
			exit(1);
			break;
		default: //parent process
			shutdown(ConnectFD, SHUT_RDWR);
			break;
		}


	}

	close(ConnectFD);
	return 0;
}

