/*
 * udp-balancer.c - Very simple UDP/GELF round robin load balancer
 *
 * Copyright (c) 2017 - Jo-Philipp Wich <jo@mein.io>
 *
 * This file falls under the MIT License.
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>

#define DEFAULT_CONFIG "/etc/udp-balancer.conf"
#define BUFFER_SIZE 65536


static uint64_t seqnr = 0;

static struct {
	bool handle_gelf;
	unsigned int send_buffer;
	unsigned int recv_buffer;
	uint8_t nremotes;
	struct sockaddr_in local;
	struct sockaddr_in remotes[256];
} conf;


/*
 * Parse addr:port notation into a struct sockaddr_in.
 *
 * Modifies the given string buffer, expects IPv4.
 */

static int parse_addr(char *addr, struct sockaddr_in *si)
{
	char *ptr, *e;
	unsigned int n;

	ptr = strtok(addr, ":");

	if (!ptr)
		return -1;

	if (!inet_pton(AF_INET, ptr, &si->sin_addr))
		return -1;

	ptr = strtok(NULL, " \t\n");

	if (!ptr)
		return -1;

	n = strtoul(ptr, &e, 10);

	if (e == ptr || *e || n > 65535)
		return -1;

	si->sin_port = htons(n);
	si->sin_family = AF_INET;

	return 0;
}

/*
 * Parse given configuration file and fill local structs.
 *
 * A configuration file consists of one "listen ipaddr:port" directive and
 * a series of "upstream ipaddr:port" directives in arbitrary order.
 *
 * Additionally supported directives are:
 *
 *   handle-gelf              Enables special handling of GELF chunk messages
 *   send-buffer #            Override the size of the socket send buffer
 *   recv-buffer #            Override the size of the socket recv buffer
 */

static int parse_config(const char *filename)
{
	char line[128], *ptr, *e;
	int ln = 0;
	FILE *fp;

	fp = fopen(filename, "r");

	if (!fp) {
		fprintf(stderr, "Unable to open file \"%s\": %s\n",
		        filename, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), fp))
	{
		ln++;
		ptr = strtok(line, " \t\n");

		if (!ptr)
			continue;

		if (!strcmp(ptr, "listen"))
		{
			ptr = strtok(NULL, " \t\n");

			if (!ptr || parse_addr(ptr, &conf.local)) {
				fprintf(stderr, "Invalid listen directive at line %d\n", ln);
				return -1;
			}
		}
		else if (!strcmp(ptr, "upstream"))
		{
			ptr = strtok(NULL, " \t\n");

			if (!ptr || parse_addr(ptr, &conf.remotes[conf.nremotes++])) {
				fprintf(stderr, "Invalid upstream directive at line %d\n", ln);
				return -1;
			}
		}
		else if (!strcmp(ptr, "handle-gelf"))
		{
			conf.handle_gelf = true;
		}
		else if (!strcmp(ptr, "send-buffer"))
		{
			ptr = strtok(NULL, " \t\n");
			conf.send_buffer = ptr ? strtoul(ptr, &e, 0) : 0;

			if (!ptr || e == ptr || *e || !conf.send_buffer) {
				fprintf(stderr, "Invalid send buffer value at line %d\n", ln);
				return -1;
			}
		}
		else if (!strcmp(ptr, "recv-buffer"))
		{
			ptr = strtok(NULL, " \t\n");
			conf.recv_buffer = ptr ? strtoul(ptr, &e, 0) : 0;

			if (!ptr || e == ptr || *e || !conf.recv_buffer) {
				fprintf(stderr, "Invalid recv buffer value at line %d\n", ln);
				return -1;
			}
		}
		else {
			fprintf(stderr, "Unknown keyword \"%s\" at line %d\n", ptr, ln);
			return -1;
		}
	}

	fclose(fp);
	return 0;
}

/*
 * Simple CRC8 implementation.
 *
 * Polynomial is 0x81
 */

static uint8_t crc8(const char *msg, ssize_t len)
{
	uint8_t crc = 0;
	uint8_t bit;

	while (len--) {
		crc ^= (uint8_t)*msg++;

		for (bit = 8; bit > 0; bit--) {
			crc = (crc << 1);

			if (crc & 0x80)
				crc ^= 0x81;
		}
	}

	return crc;
}

/*
 * Select upstream address to relay to.
 *
 * If the payload carries a GELF chunk magic, then calculate CRC8 over the
 * message ID (bytes 3-10) modulo the number of upstreams to ensure that
 * all chunks end up on the same backend.
 *
 * For all other payloads simply forward the packet to the next upstream
 * in a round robin fashion by taking the sent message counter modulo the
 * number of upstreams.
 */

static struct sockaddr *
toaddr(const char *payload)
{
	uint8_t remote;

	if (conf.handle_gelf && !memcmp(payload, "\036\017", 2))
		remote = crc8(payload + 2, 8) % conf.nremotes;
	else
		remote = seqnr++ % conf.nremotes;

	return (struct sockaddr *)&conf.remotes[remote];
}

/*
 * Format a sockaddr struct into a human readable notation using a static
 * fixed buffer.
 */

static char *addstr(const void *ptr)
{
	static char buf[INET6_ADDRSTRLEN + sizeof(":65535")];
	const struct sockaddr_in *sa = ptr;

	memset(buf, 0, sizeof(buf));
	inet_ntop(sa->sin_family, &sa->sin_addr, buf, sizeof(buf));
	sprintf(buf + strlen(buf), ":%u", ntohs(sa->sin_port));

	return buf;
}

int main(int argc, char *argv[])
{
	int sock;
	socklen_t fromlen;
	char pkt[BUFFER_SIZE];
	struct sockaddr_in from;
	ssize_t recvlen, sendlen;
	struct sockaddr *sa;

	const char *config = DEFAULT_CONFIG;

	if (argc > 1)
		config = argv[1];

	if (parse_config(config))
	{
		fprintf(stderr, "Failed to parse configuration\n");
		return -1;
	}

	if (!conf.local.sin_family) {
		fprintf(stderr, "No listen address defined\n");
		return -1;
	}

	if (!conf.nremotes) {
		fprintf(stderr, "No upstream addresses defined\n");
		return -1;
	}

	sock = socket(AF_INET, SOCK_DGRAM, 0);

	if (sock < 0) {
		fprintf(stderr, "socket(): %s (%d)\n", strerror(errno), errno);
		return -1;
	}

	if (conf.send_buffer &&
	    setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
	               &conf.send_buffer, sizeof(conf.send_buffer))) {
	    fprintf(stderr, "setsockopt(SO_SNDBUF): %s (%d)\n",
	            strerror(errno), errno);
	}

	if (conf.recv_buffer &&
	    setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
	               &conf.recv_buffer, sizeof(conf.recv_buffer))) {
	    fprintf(stderr, "setsockopt(SO_RCVBUF): %s (%d)\n",
	            strerror(errno), errno);
	}

	if (bind(sock, (struct sockaddr *)&conf.local, sizeof(conf.local)) < 0) {
		fprintf(stderr, "bind(%s): %s (%d)\n",
		        addstr(&conf.local), strerror(errno), errno);
		close(sock);
		return -1;
	}

	while (1) {
		fromlen = sizeof(from);
		recvlen = recvfrom(sock, pkt, sizeof(pkt), 0,
		                   (struct sockaddr *)&from, &fromlen);

		if (recvlen < 0) {
			fprintf(stderr, "recvfrom(%s): %s (%d)\n",
			        addstr(&from), strerror(errno), errno);
			break;
		}

		if (recvlen < 12) {
			fprintf(stderr, "recvfrom(%s): bad packet\n", addstr(&from));
			continue;
		}

		sa = toaddr(pkt);
		sendlen = sendto(sock, pkt, recvlen, 0, sa, sizeof(*sa));

		if (sendlen != recvlen) {
			fprintf(stderr, "sendto(%s): %s (%d)\n",
			        addstr(sa), strerror(errno), errno);
			continue;
		}
	}

	close(sock);
	return 0;
}
