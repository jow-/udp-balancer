CC ?= gcc
CFLAGS ?= -Wall -pedantic -O2
DESTDIR ?= /usr/local

udp-balancer: udp-balancer.c
	$(CC) $(CFLAGS) -o $@ $<

install: udp-balancer
	install -m 0755 -d $(DESTDIR)/bin
	install -m 0755 -s ./udp-balancer $(DESTDIR)/bin/udp-balancer
