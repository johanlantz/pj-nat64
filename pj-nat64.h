#ifndef PJ_NAT64_H_
#define PJ_NAT64_H_

/**
 * Options for NAT64 rewriting. Probably you want to enable all of them */
typedef enum nat64_options {
    /** Replace outgoing ipv6 with ipv4*/
    NAT64_REWRITE_OUTGOING_SDP          = 0x01,
    /** Replace incoming ipv4 with ipv6 */
    NAT64_REWRITE_INCOMING_SDP          = 0x02,
    /** Replace ipv4 address in 200 Ok for INVITE with ipv6 so ACK and BYE uses correct transport */
    NAT64_REWRITE_ROUTE_AND_CONTACT     = 0x04
} nat64_options;

/*
 * Enable nat64 rewriting module.
 * @param options       Bitmap of #nat64_options.
 *                      NAT64_REWRITE_OUTGOING_SDP | NAT64_REWRITE_INCOMING_SDP | NAT64_REWRITE_ROUTE_AND_CONTACT activates all options.
 * @default             0 - No nat64 rewriting is done.
 */
pj_status_t pj_nat64_enable_rewrite_module();

/*
 * Disable rewriting module, for instance when on a ipv4 network
 */
pj_status_t pj_nat64_disable_rewrite_module();

/*
 * Update rewriting options
 */
void pj_nat64_set_options(nat64_options options);

/*
 * Helper function that takes an outbound proxy address, resolves and synthesizes the host or IP and writes it to
 * the buffer.
 * @param proxy                 Outbound proxy address such as sips:my_host:443;transport=TLS
 * @param resolved_proxy_buf    Buffer to write the output which will be something like sips:[2001:2::aaaa:bbbb:cccc:dddd:eeee:ffff]:443;transport=TLS
 *                              when behind a NAT64
 */
pj_status_t pj_nat64_resolve_and_replace_hostname_with_ip_if_possible(char* proxy, char* resolved_proxy_buf);

/*
 * Helper function that takes an outbound proxy address and returns the hostname part of it (ip or fqdm)
 * the buffer.
 * @param proxy                 Outbound proxy address such as sips:my_host:443;transport=TLS
 * @param resolved_proxy_buf    Buffer to write the hostname to (my_host) unresolved.
 */
pj_status_t pj_nat64_get_hostname_from_proxy_string(char* proxy, char* hostname_buf);

/*
 * By default the nat64 rewriting will put a non routable address 192.168.1.1 in the outgoing sdp.
 * This means that latching will be used for the inbound media.
 * If you rely on sdp_nat_rewrite to put the IP address as seen in the Via header in the outbound sdp
 * you need to pass the active account id to this function so that address can be used instead.
 *
 * @param acc_id    The active account.
 */
void pj_nat64_set_active_account(pjsua_acc_id acc_id);
#endif
