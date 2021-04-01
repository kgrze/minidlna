import urllib2

soap_encoding = "http://schemas.xmlsoap.org/soap/encoding/"
soap_env = "http://schemas.xmlsoap.org/soap/envelope"
service_ns = "urn:schemas-upnp-org:service:ContentDirectory:1"
body = \
    '<?xml version="1.0"?>\n' \
    '<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope" SOAP-ENV:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">\n' \
    '  <SOAP-ENV:Body>\n' \
    '    <m:%(action_name)s xmlns:u="%(service_type)s">\n' \
    '      %(arg_values)s\n' \
    '    </m:%(action_name)s>\n' \
    '   </SOAP-ENV:Body>\n' \
    '</SOAP-ENV:Envelope>\n' % {
        'action_name': 'GetSystemUpdateID',
        'service_type': 'urn:schemas-upnp-org:service:ContentDirectory:1',
        'arg_values': '',
    }
headers = {
    'SOAPAction': '"%s#%s"' % ('urn:schemas-upnp-org:service:ContentDirectory:1', 'GetSystemUpdateID'),
    'Host': '192.168.1.11:8200',
    'Content-Type': 'text/xml',
    'Content-Length': len(body),
}

ctrl_url = "http://192.168.1.11:8200/ctl/ContentDir"

try:
    request = urllib2.Request(ctrl_url, body, headers)
    try:
        response = urllib2.urlopen(request)
        #print response.read()
    except urllib2.HTTPError, e:
        error_msg = e.read()
        if 'UPnPError' in error_msg:
            print '\nTEST PASSED\n' 
        else:
            print '\nTEST FAILED\n' 
except urllib2.URLError:
    print '\nTEST FAILED\n' 
