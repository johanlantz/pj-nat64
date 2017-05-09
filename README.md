# pj-nat64
Helpers for NAT64 handling pending official solutions from pjsip

Apple requires NAT64 support for AppStore approval starting June. Pjsip has IPv6 support since many years but currently for NAT64 some things are not working.

The code in this repo should be seen as a temporary solution, there are more then one way to solve this problem but at least this approach does not require backend changes.

For the hostname resolution/synthezising to work you will need at least rev 5319 fixing these issues:
http://trac.pjsip.org/repos/ticket/1925

http://trac.pjsip.org/repos/ticket/1917#comment:1

For the SIP transport functionality pjsip version 2.5.5 has pretty good support for most things but as of August 2016, the media rewriting is not managed so you might have to implement something like this module.

## Activation
The pj_nat64 in this repo is implemented as a pjsip module meaning no code patching is necessary. There are many ways you can activate it and in the end it comes down to how your project is setup, weather you use bindings to another language or pjsua2 as well as how you manage connectivity changes. The example below is what I use and it is based on a C layer on top of pjsua, you might have to adapt to your specific situation.

1. Register the module when starting up pjsip, normally after pjsua_start has returned with PJ_SUCCESS.
```
#if defined(PJ_HAS_IPV6) && PJ_HAS_IPV6 == 1
    pj_nat64_enable_rewrite_module();
#endif
```

2. Once registration completes, you can check the transport type in the on_reg_state callback from pjsip and selectivly choose to activate nat64 rewriting and ipv6 media. Something like:
```
pjsip_transport* active_transport;
if (active_transport->factory->type & PJSIP_TRANSPORT_IPV6) {
  PJ_LOG(2,(THIS_FILE, "New transport is ipv6, activate ipv6 media"));
  pjsua_var.acc[your_active_account].cfg.ipv6_media_use = PJSUA_IPV6_ENABLED;
  if (your_config.nat_64_bitmap) {
    PJ_LOG(4,(THIS_FILE, "Activating NAT64 rewriting with value=%d", your_config.nat_64_bitmap));
    pj_nat64_set_options(your_config.nat_64_bitmap);
  }
}
```

You can not register the nat64 module from inside the registration callback since at that time the PJSUA_MUTEX is held by the stack and you will end up with a deadlock.
