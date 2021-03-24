import socket
import struct

SSDP_BROADCAST_ADDR = '239.255.255.250'
SSDP_BROADCAST_PORT = 1900

notify_byebye = False
notify_alive = False

# Set up UDP socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('', SSDP_BROADCAST_PORT))

# Tell the operating system to add the socket to
# the multicast group on all interfaces.
group = socket.inet_aton(SSDP_BROADCAST_ADDR)
mreq = struct.pack('4sL', group, socket.INADDR_ANY)
s.setsockopt(
    socket.IPPROTO_IP,
    socket.IP_ADD_MEMBERSHIP,
    mreq)

s.settimeout(10)

print 'Please start tested DLNA server within 10 seconds ...'

try:
    while True:
        data, addr = s.recvfrom(512)
        print addr, data
        if 'byebye' in data:
            notify_byebye = True
        if 'alive' in data:
            notify_alive = True
        if notify_byebye is True:
            if notify_alive is True:
                print '\nTEST PASSED\n' 
                break
except socket.timeout:
    print '\nTEST FAILED\n'
    pass