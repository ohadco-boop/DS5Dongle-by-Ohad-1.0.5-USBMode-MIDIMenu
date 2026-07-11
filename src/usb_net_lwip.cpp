#include "usb_net_lwip.h"
#include "telnet_settings.h"
#include "tusb.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include <cstdio>
#include <cstring>

namespace {
netif g_netif{};
bool g_lwip_inited = false;
char g_status[32] = "USBNet init";

void set_status(const char* s) { std::snprintf(g_status, sizeof(g_status), "%s", s); }

void make_ip(ip4_addr_t* out, const uint8_t ip[4]) {
    IP4_ADDR(out, ip[0], ip[1], ip[2], ip[3]);
}

err_t usb_linkoutput(netif*, pbuf* p) {
    if (!tud_ready()) return ERR_CONN;
    if (!tud_network_can_xmit((uint16_t)p->tot_len)) return ERR_USE;
    return tud_network_xmit(p, (uint16_t)p->tot_len) ? ERR_OK : ERR_IF;
}

err_t usb_netif_init(netif* n) {
    n->name[0] = 'u';
    n->name[1] = 's';
    n->linkoutput = usb_linkoutput;
    n->output = etharp_output;
    n->mtu = 1500;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    n->hwaddr_len = 6;
    extern uint8_t tud_network_mac_address[6];
    std::memcpy(n->hwaddr, tud_network_mac_address, 6);
    set_status("USBNet up");
    return ERR_OK;
}
}

// Locally administered MAC. Keep stable so Windows does not create a new network every boot.
extern "C" uint8_t tud_network_mac_address[6] = {0x02, 0xD5, 0xA2, 0x00, 0x07, 0x02};

void usb_net_lwip_init() {
    // pico_cyw43_arch_lwip_poll initializes lwIP inside cyw43_arch_init().
    // Do not call lwip_init() a second time here.
    g_lwip_inited = true;

    const auto& s = telnet_settings_get();
    ip4_addr_t ip, nm, gw;
    make_ip(&ip, s.pico_ip);
    make_ip(&nm, s.netmask);
    make_ip(&gw, s.gateway);

    netif_add(&g_netif, &ip, &nm, &gw, nullptr, usb_netif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);
    set_status("USBNet ready");
}

void usb_net_lwip_reconfigure() {
    const auto& s = telnet_settings_get();
    ip4_addr_t ip, nm, gw;
    make_ip(&ip, s.pico_ip);
    make_ip(&nm, s.netmask);
    make_ip(&gw, s.gateway);
    netif_set_addr(&g_netif, &ip, &nm, &gw);
    set_status("USBNet changed");
}

void usb_net_lwip_tick() { sys_check_timeouts(); }
const char* usb_net_status() { return g_status; }

extern "C" void tud_network_init_cb(void) {
    set_status("USBNet host");
}

extern "C" bool tud_network_recv_cb(const uint8_t* src, uint16_t size) {
    if (!g_lwip_inited || size == 0) {
        tud_network_recv_renew();
        return true;
    }

    pbuf* p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (p) {
        pbuf_take(p, src, size);
        if (g_netif.input(p, &g_netif) != ERR_OK) {
            pbuf_free(p);
        }
    }
    tud_network_recv_renew();
    return true;
}

extern "C" uint16_t tud_network_xmit_cb(uint8_t* dst, void* ref, uint16_t arg) {
    pbuf* p = static_cast<pbuf*>(ref);
    return (uint16_t)pbuf_copy_partial(p, dst, arg, 0);
}

extern "C" void tud_network_tx_complete_cb(void) {}
