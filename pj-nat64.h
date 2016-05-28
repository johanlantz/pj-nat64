#ifndef PJ_NAT64_H_
#define PJ_NAT64_H_

pj_status_t pj_nat64_enable_rewrite_module();

pj_status_t pj_nat64_disable_rewrite_module();

pj_status_t pj_nat64_resolve_and_replace_hostname_with_ip_if_possible(char* proxy, char* resolved_proxy_buf);
#endif