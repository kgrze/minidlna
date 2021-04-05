import urllib2
import os
import shutil
from xmldiff import main

soap_encoding = "http://schemas.xmlsoap.org/soap/encoding/"
soap_env = "http://schemas.xmlsoap.org/soap/envelope"
service_ns = "urn:schemas-upnp-org:service:ContentDirectory:1"
browse_args =   {
                'ObjectID': '64',
                'BrowseFlag': 'BrowseDirectChildren',
                'Filter': '@id,@parentID,@restricted,@childCount,dc:title,dc:creator,upnp:artist,upnp:class,dc:date,upnp:album,upnp:genre,res,res@size,res@duration,res@protection,res@bitrate,res@resolution,res@protocolInfo,res@nrAudioChannels,res@sampleFrequency,upnp:albumArtURI,upnp:albumArtURI@dlna:profileID, res@dlna:cleartextSize',
                'StartingIndex': 0,
                'RequestedCount': 24,
                'SortCriteria': '',
                }
arg_values = '\n'.join( ['<%s>%s</%s>' % (k, v, k) for k, v in browse_args.items()] )
body = \
    '<?xml version="1.0"?>\n' \
    '<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope" SOAP-ENV:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">\n' \
    '  <SOAP-ENV:Body>\n' \
    '    <m:%(action_name)s xmlns:u="%(service_type)s">\n' \
    '      %(arg_values)s\n' \
    '    </m:%(action_name)s>\n' \
    '   </SOAP-ENV:Body>\n' \
    '</SOAP-ENV:Envelope>\n' % {
        'action_name': 'Browse',
        'service_type': 'urn:schemas-upnp-org:service:ContentDirectory:1',
        'arg_values': arg_values,
    }
headers = {
    'SOAPAction': '"%s#%s"' % ('urn:schemas-upnp-org:service:ContentDirectory:1', 'Browse'),
    'Host': '192.168.1.11:8200',
    'Content-Type': 'text/xml',
    'Content-Length': len(body),
}

ctrl_url = "http://192.168.1.11:8200/ctl/ContentDir"

os.mkdir('dut_xml')

try:
    request = urllib2.Request(ctrl_url, body, headers)
    try:
        response = urllib2.urlopen(request)
        response_xml = open("./dut_xml/browse_BrowseDirectChildren_VIDEO.xml", "w")
        response_xml.write(response.read())
        response_xml.close()
        diff = main.diff_files('ref_xml/browse_BrowseDirectChildren_VIDEO.xml', 'dut_xml/browse_BrowseDirectChildren_VIDEO.xml')
        if len(diff) is 0:
            print '\nTEST PASSED\n' 
        else:
            print '\nTEST FAILED\n'
            print diff
    except urllib2.HTTPError, e:
        error_msg = e.read()
        print error_msg
        print '\nTEST FAILED\n' 
except urllib2.URLError:
    print '\nTEST FAILED\n' 

shutil.rmtree('dut_xml')