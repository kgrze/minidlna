import socket
import wget
import os
import shutil
from xmldiff import main

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

# Set the time-to-live for messages to 1 so they do not
# go past the local network segment.
s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
s.settimeout(2)

print SSDP_BROADCAST_ADDR, msg

s.sendto(msg, (SSDP_BROADCAST_ADDR, SSDP_BROADCAST_PORT) )

os.mkdir('dut_xml')

try:
    while True:
        data, addr = s.recvfrom(65507)
        print addr, data
        if ('DLNA' in data) and ('rootDesc' in data):
            start = data.find('http://')
            end = data.find('xml', start) + 3
            rootDescUrl = data[start:end]
            print rootDescUrl
            wget.download(rootDescUrl, 'dut_xml/rootDesc.xml')
            diff = main.diff_files('ref_xml/rootDesc.xml', 'dut_xml/rootDesc.xml')
            if len(diff) is 0:
                print '\nTEST PASSED\n' 
            else:
                print '\nTEST FAILED\n'
                print diff
            break
except socket.timeout:
    print '\nTEST FAILED\n'
    pass

shutil.rmtree('dut_xml')
