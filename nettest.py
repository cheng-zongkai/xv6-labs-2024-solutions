#!/usr/bin/env python3

#
# net tests
# to be used with user/nettest.c
#

import socket
import sys
import time
import os

# qemu listens for packets sent to FWDPORT,
# and re-writes them so they arrive in
# xv6 with destination port 2000.
FWDPORT1 = (os.getuid() % 5000) + 25999
FWDPORT2 = (os.getuid() % 5000) + 30999

# xv6's nettest.c tx sends to SERVERPORT.
SERVERPORT = (os.getuid() % 5000) + 25099

def usage():
    sys.stderr.write("Usage: nettest.py txone\n")
    sys.stderr.write("       nettest.py rxone\n")
    sys.stderr.write("       nettest.py rx\n")
    sys.stderr.write("       nettest.py rx2\n")
    sys.stderr.write("       nettest.py rxburst\n")
    sys.stderr.write("       nettest.py tx\n")
    sys.stderr.write("       nettest.py ping\n")
    sys.stderr.write("       nettest.py grade\n")
    sys.exit(1)

if len(sys.argv) != 2:
    usage()

if sys.argv[1] == "txone":
    #
    # listen for a single UDP packet sent by xv6's nettest txone.
    # nettest.py must be started before xv6's nettest txone.
    #
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', SERVERPORT))
    print("tx: listening for a UDP packet")
    buf0, raddr0 = sock.recvfrom(4096)
    if buf0 == b'txone':
        print("txone: OK")
    else:
        print("txone: unexpected payload %s" % (buf0))
elif sys.argv[1] == "rxone":
    #
    # send a single UDP packet to xv6 to test e1000_recv().
    # should result in arp_rx() printing
    #   arp_rx: received an ARP packet
    # and ip_rx() printing
    #   ip_rx: received an IP packet
    #
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(b"xyz", ("127.0.0.1", FWDPORT1))
elif sys.argv[1] == "rx":
    #
    # test the xv6 receive path by sending a slow
    # stream of UDP packets, which should appear
    # on port 2000.
    #
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    i = 0
    while True:
        txt = "packet %d" % (i)
        sys.stderr.write("%s\n" % txt)
        buf = txt.encode('ascii', 'ignore')
        sock.sendto(buf, ("127.0.0.1", FWDPORT1))
        time.sleep(1)
        i += 1
elif sys.argv[1] == "rx2":
    #
    # send to two different UDP ports, to see
    # if xv6 keeps them separate.
    #
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    i = 0
    while True:
        txt = "one %d" % (i)
        sys.stderr.write("%s\n" % txt)
        buf = txt.encode('ascii', 'ignore')
        sock.sendto(buf, ("127.0.0.1", FWDPORT1))

        txt = "two %d" % (i)
        sys.stderr.write("%s\n" % txt)
        buf = txt.encode('ascii', 'ignore')
        sock.sendto(buf, ("127.0.0.1", FWDPORT2))

        time.sleep(1)
        i += 1
elif sys.argv[1] == "rxburst":
    #
    # send a big burst of packets to 2001, then
    # a packet to 2000.
    #
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    i = 0
    while True:
        
        for ii in range(0, 32):
            txt = "packet %d" % (i)
            # sys.stderr.write("%s\n" % txt)
            buf = txt.encode('ascii', 'ignore')
            sock.sendto(buf, ("127.0.0.1", FWDPORT2))

        txt = "packet %d" % (i)
        sys.stderr.write("%s\n" % txt)
        buf = txt.encode('ascii', 'ignore')
        sock.sendto(buf, ("127.0.0.1", FWDPORT1))

        time.sleep(1)
        i += 1
elif sys.argv[1] == "tx":
    #
    # listen for UDP packets sent by xv6's nettest tx.
    # nettest.py must be started before xv6's nettest tx.
    #
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', SERVERPORT))
    print("tx: listening for UDP packets")
    buf0, raddr0 = sock.recvfrom(4096)
    buf1, raddr1 = sock.recvfrom(4096)
    if buf0 == b't 0' and buf1 == b't 1':
        print("tx: OK")
    else:
        print("tx: unexpected packets %s and %s" % (buf0, buf1))
elif sys.argv[1] == "ping":
    #
    # listen for UDP packets sent by xv6's nettest ping,
    # and send them back.
    # nettest.py must be started before xv6's nettest.
    #
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', SERVERPORT))
    print("ping: listening for UDP packets")
    while True:
        buf, raddr = sock.recvfrom(4096)
        sock.sendto(buf, raddr)
elif sys.argv[1] == "grade":
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', SERVERPORT))

    # first, listen for a single UDP packet sent by xv6,
    # in order to test only e1000_transmit(), in a situation
    # where perhaps e1000_recv() has not yet been implemented.
    buf, raddr = sock.recvfrom(4096)
    if buf == b'txone':
        print("txone: OK")
    else:
        print("txone: received incorrect payload %s" % (buf))
    sys.stdout.flush()
    sys.stderr.flush()

    # second, send a single UDP packet, to test
    # e1000_recv() -- received by user/nettest.c's rxone().
    print("rxone: sending one UDP packet")
    sock1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock1.sendto(b'rxone', ("127.0.0.1", FWDPORT2))

    # third, act as a ping reflector.
    while True:
        buf, raddr = sock.recvfrom(4096)
        print(f'recv: {raddr[0]}:{raddr[1]}')
        sock.sendto(buf, raddr)
else:
    usage()
