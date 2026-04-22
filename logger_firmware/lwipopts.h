// lwIP options for the standalone logger appliance firmware.
//
// We use pico-sdk's NO_SYS raw-API path with cyw43 + DHCP + DNS + raw TCP.

#ifndef LOGGER_FIRMWARE_LWIPOPTS_H
#define LOGGER_FIRMWARE_LWIPOPTS_H

#define NO_SYS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_RAW 1

#define LWIP_ALTCP 1
#define LWIP_ALTCP_TLS 1
#define LWIP_ALTCP_TLS_MBEDTLS 1

// MBEDTLS_SSL_VERIFY_REQUIRED
#define ALTCP_MBEDTLS_AUTHMODE 2

#define MEM_ALIGNMENT 4
#define MEM_SIZE (128 * 1024)

#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_ICMP 1
#define LWIP_DNS 1
#define LWIP_DHCP 1
#define LWIP_IPV4 1
#define LWIP_IPV6 0

#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_SINGLE_NETIF 1

#define TCP_MSS (1500 - 40)
#define TCP_WND (8 * TCP_MSS)
#define TCP_SND_BUF (8 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_QUEUE_OOSEQ 0

#define MEMP_NUM_TCP_PCB 8
#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_SYS_TIMEOUT 16
#define PBUF_POOL_SIZE 16

#define LWIP_PROVIDE_ERRNO 1

#endif