#pragma once

#define NO_SYS                          1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_RAW                        1
#define LWIP_TCP                        1
#define LWIP_UDP                        0
#define LWIP_ICMP                       1
#define LWIP_DHCP                       0
#define LWIP_DNS                        0
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_ETHERNET                   1
#define LWIP_ARP                        1
#define ETH_PAD_SIZE                    0

#define MEM_LIBC_MALLOC                 0
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (24 * 1024)
#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_SEG                16
#define MEMP_NUM_SYS_TIMEOUT            8
#define PBUF_POOL_SIZE                  16
#define PBUF_POOL_BUFSIZE               1536
#define TCP_MSS                         536
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                16
#define TCP_QUEUE_OOSEQ                 0
#define LWIP_TCP_KEEPALIVE              1

#define LWIP_STATS                      0
#define LWIP_DEBUG                      0
#define LWIP_ASSERT_CORE_LOCKED()       do { } while (0)
#define sys_check_core_locking()        do { } while (0)
