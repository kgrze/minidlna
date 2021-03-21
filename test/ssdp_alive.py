import socket

SSDP_BROADCAST_ADDR = '239.255.255.250'
SSDP_BROADCAST_PORT = 1900

msg = \
    'M-SEARCH * HTTP/1.1\r\n' \
    'HOST:239.255.255.250:1900\r\n' \
    'ST:upnp:rootdevice\r\n' \
    'MX:2\r\n' \
    'MAN:"ssdp:discover"\r\n' \
    '\r\n'

# Set up UDP socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
s.settimeout(5)
#print SSDP_BROADCAST_ADDR, msg
#s.sendto(msg, (SSDP_BROADCAST_ADDR, SSDP_BROADCAST_PORT) )
#test_result = False
print 'Please run DLNA server now!\n'

try:
    while True:
        print 'Waiting for alive signal...\n'
        # data, addr = s.recvfrom(65507)
        data= s.recv(65507)
        print data
except socket.timeout:
    pass