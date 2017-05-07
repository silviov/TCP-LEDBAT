#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
//#include <linux/tcp.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>

#define DIRSIZE     8192

typedef struct _options {
	char *hostname;
	uint16_t port;
	uint32_t count;
	uint16_t duration;
} options_data;

void end_program(int signal)
{
	fprintf(stderr, "exiting after duration!!!\n");
	exit(0);
}

void print_help(char *progname)
{
	printf("Usage: %s [-p port] [-t duration] [-c count] hostname\n",
	       progname);
}

void parse_opt(int argc, char *argv[], options_data * opts)
{
	int opt;

	opts->hostname = "";
	opts->port = 5001;	//same as iperf
	opts->count = 0;
	opts->duration = 0;

	while ((opt = getopt(argc, argv, "hp:c:t:")) != -1) {
		switch (opt) {
		case 'p':
			opts->port = atoi(optarg);
			break;
		case 'c':
			opts->count = atoi(optarg);
			break;
		case 't':
			opts->duration = atoi(optarg);
			break;
		default:
		case 'h':
			print_help(argv[0]);
			exit(EXIT_SUCCESS);
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "Expected hostname\n");
		exit(EXIT_FAILURE);
	}

	opts->hostname = argv[optind];
	return;
}

int main(int argc, char *argv[])
{
	char dir[DIRSIZE];
	int sd;
	struct sockaddr_in sin;
	struct sockaddr_in pin;
	struct hostent *hp;

	options_data opts;

	parse_opt(argc, argv, &opts);

	signal(SIGALRM, end_program);

	/* go find out about the desired host machine */
	if ((hp = gethostbyname(opts.hostname)) == 0) {
		perror("gethostbyname");
		exit(EXIT_FAILURE);
	}

	/* fill in the socket structure with host information */
	memset(&pin, 0, sizeof(pin));
	pin.sin_family = AF_INET;
	pin.sin_addr.s_addr = ((struct in_addr *)(hp->h_addr))->s_addr;
	pin.sin_port = htons(opts.port);

	/* grab an Internet domain socket */
	if ((sd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	/*set the congestion control algorithm to ledbat */
	if (setsockopt(sd, SOL_TCP, TCP_CONGESTION, "ledbat", 6) == -1) {
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}

	/* connect to the host */
	if (connect(sd, (struct sockaddr *)&pin, sizeof(pin)) == -1) {
		perror("connect");
		exit(EXIT_FAILURE);
	}

	if (opts.duration > 0) {
		alarm(opts.duration);
	}

	int n = DIRSIZE, count = 0;
	memset(dir, 1, DIRSIZE);
	while (1) {
		if (send(sd, dir, n, 0) == -1) {
			perror("send");
			exit(EXIT_FAILURE);
		}
		count += n;
		if (opts.count > 0 && count > opts.count) {
			exit(EXIT_SUCCESS);
		}
	}

	close(sd);
}
