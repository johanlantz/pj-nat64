#include <pjsua.h>
#include <pjsua-lib/pjsua_internal.h>
#include "pj-nat64.h"

#define THIS_FILE "pj_nat64.c"

static nat64_options module_options;

static pjsua_acc_id active_add_id = PJSUA_INVALID_ID;

/* Syntax error handler for parser. */
static void on_syntax_error(pj_scanner *scanner)
{
    PJ_UNUSED_ARG(scanner);
    PJ_LOG(4, (THIS_FILE, "Scanner syntax error at %s", scanner->curptr));
    PJ_THROW(PJ_EINVAL);
}

//Helper function to find a specific string using the scanner.
//It returns the pointer to where the string is found and the number of chars parsed (i.e. not the length of the string)
void scanner_find_string( pj_scanner *scanner, char* wanted, pj_str_t *result)
{
    pj_scan_state state;
    pj_scan_save_state( scanner, &state);
    do {
        pj_scan_get_until_ch(scanner, wanted[0], result);
        if (pj_scan_strcmp(scanner, wanted, (int)strlen(wanted)) == 0) {
            //Found wanted string, update real scanner
            unsigned steps_to_advance = (unsigned)(scanner->curptr - state.curptr);
            pj_scan_restore_state(scanner, &state);
            pj_scan_advance_n(scanner, steps_to_advance, PJ_FALSE);
            result->ptr = scanner->curptr;
            result->slen = steps_to_advance;
            break;
        } else {
            pj_scan_advance_n(scanner, 1, PJ_FALSE);
        }

        if (pj_scan_is_eof(scanner)) {
            PJ_LOG(4, (THIS_FILE, "Scanner EOF"));
            break;
        }

    } while (1);
}

static int calculate_new_content_length(char* buffer)
{
    char* body_delim = "\r\n\r\n"; //Sip message body starts with a newline and final empty line after sdp does not count
    char* body_start = strstr(buffer, body_delim);
    if (body_start != NULL) {
        body_start = body_start + strlen(body_delim);
        return (int)strlen(body_start);
    } else {
        PJ_LOG(1, (THIS_FILE,
                   "Error: Could not find Content-Length header. The correct length can not be set. This must never happen."));
        return -1;
    }
}

//Helper that will resolve or synthesize to ipv6. Output buffer will be null terminated
static void resolve_or_synthesize_ipv4_to_ipv6(pj_str_t* host_or_ip, char* buf, int buf_len)
{
    unsigned int count = 1;
    pj_addrinfo ai[1];
    pj_getaddrinfo(PJ_AF_UNSPEC, host_or_ip, &count, ai);

    if (count > 0) {
        if (ai[0].ai_addr.addr.sa_family == PJ_AF_INET) {
            pj_inet_ntop(PJ_AF_INET, &ai[0].ai_addr.ipv4.sin_addr, buf, PJ_INET_ADDRSTRLEN);
        } else if (ai[0].ai_addr.addr.sa_family == PJ_AF_INET6) {
            pj_inet_ntop(PJ_AF_INET6, &ai[0].ai_addr.ipv6.sin6_addr, buf,
                         PJ_INET6_ADDRSTRLEN);
        } else {
            PJ_LOG(1, (THIS_FILE, "Error: Unknown AF %d use original input", ai[0].ai_addr.addr.sa_family));
            pj_ansi_snprintf(buf, buf_len, "%.*s", (int)host_or_ip->slen, host_or_ip->ptr);
        }
    } else {
        PJ_LOG(1, (THIS_FILE, "Error: Synthesizing media ip failed, ai count = 0. Use original input"));
        pj_ansi_snprintf(buf, buf_len, "%.*s", (int)host_or_ip->slen, host_or_ip->ptr);
    }
}

static int update_content_length(char* buffer, pjsip_msg* msg)
{
#define CONTENT_LEN_BUF_SIZE 10
    PJ_USE_EXCEPTION;
    pj_scanner scanner;
    pj_str_t result = {NULL, 0};
    pj_str_t current_content_len;
    char* content_length = "Content-Length";
    char new_content_len_buf[CONTENT_LEN_BUF_SIZE];
    int new_content_len = calculate_new_content_length(buffer);
    if (new_content_len == -1) {
        return -1;
    }

    //Step 1: Update the pjsip_msg to reflect the new body size (for incoming messages only)
    if (msg != NULL) {
        msg->body->len = (int)new_content_len;
    }

    //Step 2: Rewrite the buffer so the Content-Length header is correct
    pj_scan_init(&scanner, buffer, strlen(buffer), 0, &on_syntax_error);

    pj_ansi_snprintf(new_content_len_buf, CONTENT_LEN_BUF_SIZE, "%d", new_content_len);

    PJ_TRY {
        scanner_find_string(&scanner, content_length, &result);

        pj_scan_get_until_chr(&scanner, "0123456789", &result);

        pj_scan_get_until_ch(&scanner, '\r', &result);
        pj_strset(&current_content_len, result.ptr, result.slen);
        PJ_LOG(4, (THIS_FILE, "Current Content-Length is: %.*s and new Content-Length is %d .", current_content_len.slen, current_content_len.ptr, new_content_len));
        if (strlen(new_content_len_buf) <= current_content_len.slen)
        {
            int len_offset = (int)(result.ptr - scanner.begin);
            PJ_LOG(4, (THIS_FILE, "Updated content length needs the same or less bytes, no need to do buffer copy"));
            //even though scanner and buffer are in different memory locations the content is identical
            pj_memset(buffer + len_offset, ' ', result.slen);
            pj_memcpy(buffer + len_offset, new_content_len_buf, strlen(new_content_len_buf));
        } else {
            PJ_TODO(SUPPORT_GROWING_THE_BUFFER);
            PJ_LOG(4, (THIS_FILE, "Updated content length needs more bytes than old one, we must do expand and copy. TODO"));
        }
    } PJ_CATCH_ANY {
        PJ_LOG(4, (THIS_FILE, "Exception thrown when searching for Content-Length, incorrect value will be used. Must never happen."));
    }
    PJ_END;
    pj_scan_fini(&scanner);
    return new_content_len;
}

//For outgoing INVITE and 200 OK
static void replace_sdp_ipv6_with_ipv4(pjsip_tx_data *tdata)
{
    PJ_USE_EXCEPTION;
    pj_scanner scanner;
    pj_str_t result = {NULL, 0};
    char* org_buffer = tdata->buf.start;
    char new_buffer[PJSIP_MAX_PKT_LEN];
    char* walker_p = new_buffer;
    pj_str_t ipv4_str = pj_str("IP4");
    pj_str_t unroutable_host = pj_str("192.168.1.1");
    pj_bzero(new_buffer, PJSIP_MAX_PKT_LEN);

    PJ_LOG(4, (THIS_FILE, "**********Outgoing INVITE or 200 with IPv6 address*************"));

    pj_scan_init(&scanner, org_buffer, strlen(org_buffer), 0, &on_syntax_error);

    PJ_TRY {
        do {
            //Find instance of IP6
            scanner_find_string(&scanner, "IP6", &result);
            pj_memcpy(walker_p, (result.ptr - result.slen), result.slen);
            walker_p = walker_p + result.slen;

            //Replace with IP6
            pj_memcpy(walker_p, ipv4_str.ptr, ipv4_str.slen);
            walker_p = walker_p + ipv4_str.slen;
            pj_scan_get_n(&scanner, (int)ipv4_str.slen, &result);

            //Find start of IP address and copy (hopefully only 1 whitespace)
            pj_scan_get_until_chr(&scanner, "0123456789abcdef", &result);
            pj_memcpy(walker_p, result.ptr, result.slen);
            walker_p = walker_p + result.slen;

            //Extract IP address
            pj_scan_get_until_ch( &scanner, '\r', &result);
            PJ_LOG(4, (THIS_FILE, "Extracted ip6 address as %.*s", result.slen, result.ptr));

            if (active_add_id != PJSUA_INVALID_ID && pjsua_var.acc[active_add_id].cfg.allow_sdp_nat_rewrite && pjsua_var.acc[active_add_id].reg_mapped_addr.slen)
            {
                PJ_LOG(4, (THIS_FILE, "Replace local ipv6 address with address from Via header (%.*s)",
                pjsua_var.acc[active_add_id].reg_mapped_addr.slen, pjsua_var.acc[active_add_id].reg_mapped_addr.ptr));
                pj_memcpy(walker_p, pjsua_var.acc[active_add_id].reg_mapped_addr.ptr,
                pjsua_var.acc[active_add_id].reg_mapped_addr.slen);
                walker_p = walker_p + pjsua_var.acc[active_add_id].reg_mapped_addr.slen;
            } else {
                //Replace with unroutable address so latching is used,
                pj_memcpy(walker_p, unroutable_host.ptr, unroutable_host.slen);
                walker_p = walker_p + unroutable_host.slen;
            }
            //In case this is the last occurance in the message, lets append the rest but do not advance walker_p in case there is more
            //The scanner string is always null terminated so include the terminating character as well
            pj_memcpy(walker_p, scanner.curptr, (scanner.end - scanner.curptr) +1);
        } while (!pj_scan_is_eof(&scanner));


    } PJ_CATCH_ANY {
        if (strlen(new_buffer) > 0)
        {
            pj_memcpy(org_buffer, new_buffer, strlen(new_buffer));
            org_buffer[strlen(new_buffer)] = '\0';
            update_content_length(tdata->buf.start, NULL);
            PJ_LOG(4, (THIS_FILE,
            "We have successfully parsed the INVITE/200 OK until EOF. Replace tx buffer. pjsip will now send the modified TX packet."));
            PJ_LOG(4, (THIS_FILE, "**********Modified outgoing INVITE or 200 with IPv6 address*************"));
            PJ_LOG(4, (THIS_FILE, "%s", org_buffer));
            PJ_LOG(4, (THIS_FILE, "***************************************************************"));
        } else {
            PJ_LOG(1, (THIS_FILE, "Error: Parsing of the outgoing INVITE/200 OK failed at %s. Leave incoming buffer as is", scanner.curptr));
        }
        pj_scan_fini(&scanner);
        return;
    }
    PJ_END;
    pj_scan_fini(&scanner);
}


//For incoming INVITE and 200 OK
static void replace_sdp_ipv4_with_ipv6(pjsip_rx_data *rdata)
{
    PJ_USE_EXCEPTION;
    pj_scanner scanner;
    pj_str_t result = {NULL, 0};
    char* org_buffer = rdata->msg_info.msg_buf;
    char new_buffer[PJSIP_MAX_PKT_LEN];
    char* walker_p = new_buffer;
    pj_str_t ipv6_str = pj_str("IP6");

    pj_bzero(new_buffer, PJSIP_MAX_PKT_LEN);

    PJ_LOG(4, (THIS_FILE, "**********Incoming INVITE or 200 with IPv4 address*************"));
    PJ_LOG(4, (THIS_FILE, "%s", org_buffer));
    PJ_LOG(4, (THIS_FILE, "***************************************************************"));

    pj_scan_init(&scanner, org_buffer, strlen(org_buffer), 0, &on_syntax_error);

    PJ_TRY {
        do {
            //Find instance of IP4
            scanner_find_string(&scanner, "IP4", &result);
            pj_memcpy(walker_p, (result.ptr - result.slen), result.slen);
            walker_p = walker_p + result.slen;

            //Replace with IP6
            pj_memcpy(walker_p, ipv6_str.ptr, ipv6_str.slen);
            walker_p = walker_p + ipv6_str.slen;
            pj_scan_get_n(&scanner, (int)ipv6_str.slen, &result);

            //Find start of IP address and copy (hopefully only 1 whitespace)
            pj_scan_get_until_chr(&scanner, "0123456789", &result);
            pj_memcpy(walker_p, result.ptr, result.slen);
            walker_p = walker_p + result.slen;

            //Extract IP address
            pj_scan_get_until_ch( &scanner, '\r', &result);
            PJ_LOG(4, (THIS_FILE, "Extracted ip4 address as %.*s", result.slen, result.ptr));

            resolve_or_synthesize_ipv4_to_ipv6(&result, walker_p, (PJSIP_MAX_PKT_LEN - (int)strlen(walker_p)));

            //walker_p is now null terminated, reset it to point at the end of the current buffer (without null termination)
            walker_p = walker_p + strlen(walker_p);

            //In case this is the last occurance in the message, lets append the rest but do not advance walker_p in case there is more
            //The scanner string is always null terminated so include the terminating character as well
            pj_memcpy(walker_p, scanner.curptr, (scanner.end - scanner.curptr) +1);
        } while (!pj_scan_is_eof(&scanner));


    } PJ_CATCH_ANY {
        if (strlen(new_buffer) > 0)
        {
            pj_memcpy(org_buffer, new_buffer, strlen(new_buffer));
            org_buffer[strlen(new_buffer)] = '\0';
            update_content_length(org_buffer, rdata->msg_info.msg);
            PJ_LOG(4, (THIS_FILE,
            "We have successfully parsed the INVITE/200 OK until EOF. Replace rx buffer. pjsip will now print the modified rx packet."));
            //Update all internal packet sizes (body->len has already been updated by update_content_length)
            rdata->pkt_info.len = strlen(rdata->pkt_info.packet);
            rdata->msg_info.len = (int)rdata->pkt_info.len;
            rdata->tp_info.transport->last_recv_len = rdata->pkt_info.len;
        } else {
            PJ_LOG(1, (THIS_FILE, "Error: Parsing of the incoming INVITE/200 OK failed at %s. Leave incoming buffer as is", scanner.curptr));
        }
        pj_scan_fini(&scanner);
        return;
    }
    PJ_END;
    pj_scan_fini(&scanner);
    return;
}

static void replace_route_and_contact_ipv4_with_ipv6(pjsip_rx_data *rdata)
{
    char ipv6_buf[PJ_INET6_ADDRSTRLEN] = {0} ;
    pjsip_route_hdr* current_route_hdr = NULL;

    pjsip_contact_hdr *current_contact_hdr = (pjsip_contact_hdr*)pjsip_msg_find_hdr( rdata->msg_info.msg, PJSIP_H_CONTACT,
            NULL);
    if (current_contact_hdr != NULL) {
        pjsip_sip_uri * sip_uri = NULL;
        sip_uri = (pjsip_sip_uri *)pjsip_uri_get_uri(current_contact_hdr->uri);
        PJ_LOG(4, (THIS_FILE, "Host in Contact header is %.*s",sip_uri->host.slen, sip_uri->host.ptr));

        resolve_or_synthesize_ipv4_to_ipv6(&sip_uri->host, ipv6_buf, PJ_INET6_ADDRSTRLEN);
        pj_strdup2(rdata->tp_info.pool, &sip_uri->host, ipv6_buf);
    }

    //This part is not tested since our messages does not contain Route headers.
    //According to pjsip Route header has prio over the Contact header.
    current_route_hdr = (pjsip_route_hdr*)pjsip_msg_find_hdr( rdata->msg_info.msg, PJSIP_H_ROUTE, NULL);
    if (current_route_hdr != NULL) {
        pjsip_sip_uri * sip_uri = NULL;
        sip_uri = (pjsip_sip_uri *)pjsip_uri_get_uri(current_route_hdr->name_addr.uri);
        PJ_LOG(4, (THIS_FILE, "Host in Route header is %.*s",sip_uri->host.slen, sip_uri->host.ptr));
        resolve_or_synthesize_ipv4_to_ipv6(&sip_uri->host, ipv6_buf, PJ_INET6_ADDRSTRLEN);
        pj_strdup2(rdata->tp_info.pool, &sip_uri->host, ipv6_buf);
    }
}


static pj_status_t ipv6_mod_on_rx(pjsip_rx_data *rdata)
{

    pjsip_cseq_hdr *cseq = rdata->msg_info.cseq;
    PJ_LOG(4, (THIS_FILE, "ipv6_mod_on_rx"));

    if (cseq != NULL && cseq->method.id == PJSIP_INVITE_METHOD) {
        PJ_LOG(4, (THIS_FILE, "Incoming INVITE or 200 OK. If they contain IPv4 addresses, we need to change to ipv6"));
        if (module_options & NAT64_REWRITE_INCOMING_SDP) {
            replace_sdp_ipv4_with_ipv6(rdata);
        }
        if (module_options &  NAT64_REWRITE_ROUTE_AND_CONTACT) {
            replace_route_and_contact_ipv4_with_ipv6(rdata);
        }
    }

    return PJ_FALSE;
}

pj_status_t ipv6_mod_on_tx(pjsip_tx_data *tdata)
{
    if (module_options & NAT64_REWRITE_OUTGOING_SDP) {
        pjsip_cseq_hdr *cseq = (pjsip_cseq_hdr*)pjsip_msg_find_hdr( tdata->msg, PJSIP_H_CSEQ, NULL);
        PJ_TODO(CHECK_OUTGOING_BUFFER_SIZE_HANDLING);
        PJ_LOG(4, (THIS_FILE, "ipv6_mod_on_tx"));

        if (cseq != NULL && cseq->method.id == PJSIP_INVITE_METHOD) {
            PJ_LOG(4, (THIS_FILE, "Outgoing INVITE or 200 OK. If it contains IPv6 addresses, we need to change to ipv4"));
            replace_sdp_ipv6_with_ipv4(tdata);
        }
    }
    return PJ_SUCCESS;
}

/* L1 rewrite module for sdp info.*/
static pjsip_module ipv6_module = {
    NULL, NULL,                     /* prev, next.      */
    { "mod-ipv6", 8},                 /* Name.        */
    -1,                             /* Id           */
    0,/* Priority            */
    NULL,                           /* load()       */
    NULL,                           /* start()      */
    NULL,                           /* stop()       */
    NULL,                           /* unload()     */
    &ipv6_mod_on_rx,                /* on_rx_request()  */
    &ipv6_mod_on_rx,                /* on_rx_response() */
    &ipv6_mod_on_tx,                /* on_tx_request.   */
    &ipv6_mod_on_tx,                /* on_tx_response() */
    NULL,                           /* on_tsx_state()   */
};

pj_status_t pj_nat64_enable_rewrite_module()
{
    module_options = 0;
    return pjsip_endpt_register_module(pjsua_get_pjsip_endpt(), &ipv6_module);
}

pj_status_t pj_nat64_disable_rewrite_module()
{
    return pjsip_endpt_unregister_module( pjsua_get_pjsip_endpt(),
                                          &ipv6_module);
}

char* find_addr_start(char *proxy)
{
    char *addr_start = NULL;
    //ignore the user if it's there
    addr_start = strchr(proxy, '@');
    //otherwise, go past sip[s]:
    if (addr_start == NULL)
        addr_start = strchr(proxy, ':');
    return addr_start;
}

char* find_addr_end(pj_bool_t proxy_is_static_ip_v6_address, char *addr_start)
{
    char *addr_end = NULL;
    if (proxy_is_static_ip_v6_address) {
        addr_end = strstr(addr_start, "]:");
    } else {
        addr_end = strstr(addr_start, ":");
        if (addr_end == NULL)
            addr_end = strstr(addr_start, ";");
    }
    return addr_end;
}

void pj_nat64_set_options(nat64_options options)
{
    module_options = options;
}

static void replace_hostname_with_ip(char* proxy_hostname)
{
    pj_str_t hostname = pj_str(proxy_hostname);
    unsigned int count = 1;
    pj_addrinfo ai[1];
    pj_getaddrinfo(PJ_AF_UNSPEC, &hostname, &count, ai);

    if (count > 0) {
        if (ai[0].ai_addr.addr.sa_family == PJ_AF_INET) {
            pj_inet_ntop(PJ_AF_INET, &ai[0].ai_addr.ipv4.sin_addr, proxy_hostname, PJ_MAX_HOSTNAME);
        } else if (ai[0].ai_addr.addr.sa_family == PJ_AF_INET6) {
            proxy_hostname[0] = '[';
            pj_inet_ntop(PJ_AF_INET6, &ai[0].ai_addr.ipv6.sin6_addr, proxy_hostname+1, PJ_MAX_HOSTNAME);
            strcat(proxy_hostname, "]");

        }
    }
}

pj_status_t pj_nat64_get_hostname_from_proxy_string(char* proxy, char* hostname_buf)
{
    pj_bool_t proxy_is_static_ip_v6_address = strchr(proxy, ']') != NULL ? PJ_TRUE : PJ_FALSE;
    char* addr_start = find_addr_start(proxy);
    char* addr_end = NULL;

    if (addr_start != NULL) {
        addr_start++;
        addr_end = find_addr_end(proxy_is_static_ip_v6_address, addr_start);
        if (addr_end != NULL) {
            strncpy(hostname_buf, addr_start, addr_end-addr_start);
            hostname_buf[addr_end - addr_start] = '\0';
            return PJ_SUCCESS;
        }
    }
    return PJ_EIGNORED;
}

pj_status_t pj_nat64_resolve_and_replace_hostname_with_ip_if_possible(char* proxy, char* resolved_proxy_buf)
{
    char hostname_buf[PJ_MAX_HOSTNAME];
    pj_bool_t proxy_is_static_ip_v6_address = strchr(proxy, ']') != NULL ? PJ_TRUE : PJ_FALSE;
    char* addr_start = find_addr_start(proxy);
    char* addr_end = NULL;

    if (addr_start != NULL) {
        addr_start++;
        addr_end = find_addr_end(proxy_is_static_ip_v6_address, addr_start);
        if (addr_end != NULL) {
            size_t len_sip_sips = addr_start - proxy;
            size_t len_port_and_tail = strlen(proxy) - (addr_end -
                                       proxy);
            strncpy(hostname_buf, addr_start, addr_end-addr_start);
            hostname_buf[addr_end - addr_start] = '\0';

            replace_hostname_with_ip(hostname_buf);
            pj_ansi_snprintf(resolved_proxy_buf, PJ_MAX_HOSTNAME, "%.*s%s%.*s",  (int)len_sip_sips,
                             proxy, hostname_buf, (int)len_port_and_tail, addr_end);
            return PJ_SUCCESS;
        }
    }
    return PJ_EIGNORED;
}

void pj_nat64_set_active_account(pjsua_acc_id acc_id)
{
    active_add_id = acc_id;
}
