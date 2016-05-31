# pj-nat64
Helpers for NAT64 handling pending official solutions from pjsip

Apple requires NAT64 support for AppStore approval starting June. Pjsip has IPv6 support since many years but currently for NAT64 some things are not working.

The code in this repo should be seen as a temporary solution but hopefully it will be of help.

For the hostname resolution/synthezising to work you will need at least rev 5319 fixing these issues:
http://trac.pjsip.org/repos/ticket/1925

http://trac.pjsip.org/repos/ticket/1917#comment:1
