# udp-balancer

The `udp-balancer` service is a tiny, standalone round-robin balancer for
arbitrary UDP packets.

The main use case is to split up incoming UDP traffic into evenly sized
flows which are distributed among defined upstream destinations.

Since this program was written to allow for distributed log shipping,
it also supports handling of chunked GELF messages to ensure that all
fragments of a multi chunk message are sent to the same upstream.


## Installation

Simply clone this repository and perform a `make install` to compile the
`udp-balancer` binary and install it into the filesystem.

You may override the `DESTDIR` variable to specify an alternative to the
default `/usr/local` prefix or invoke `make udp-balancer` to only compile,
but not install the program.

    $ make install
    $ make install DESTDIR=/usr
    $ make udp-balancer


## Configuration

By default, the `udp-balancer` executable attempts to open the configuration
at `/etc/udp-balancer.conf`, an alternative location can be specified as
first argument.

    $ udp-balancer
    $ udp-balancer ./alternative-config.conf

The configuration file consists of a series of statements, mainly a single
`listen` directive and one or more `upstream` declarations. Additional
options are `handle-gelf`, `send-buffer` and `recv-buffer`.

An example configuration may look like:

    handle-gelf
    send-buffer 0x10000000
    recv-buffer 0x10000000
    
    listen 0.0.0.0:12201
    upstream 1.1.1.1:12201
    upstream 2.2.2.2:12201
    upstream 3.3.3.3:12201

This configuration will instruct `udp-balancer` to bind a listening socket
to port `12201` on all local interfaces and to distribute incoming UDP
packets to the hosts `1.1.1.1`, `2.2.2.2` and `3.3.3.3` on port `12201`
respectively.

Additionally support for chunked GELF messages is enabled and the send-
receive buffers of the socket are set to 256MB each.


## Limitations

The `udp-balancer` program does not attempt to implement the complete GELF
specification, it merely checks if the first two bytes of a message match
the magic bytes `0x1f 0x0f`.

If a GELF chunk message is identified, `udp-balancer` will calculate a
CRC8 hash of the message ID at bytes 3..10 and perform a module operation
over it to choose an upstream.

As the GELF specification requires all chunks of a message to carry the
same ID, all chunks will be sent to the same upstream, leaving it to the
remote listener to properly reassemble (or discard) messages.
