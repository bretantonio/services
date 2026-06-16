#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/reboot.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static void safe_mkdir(const char* path, mode_t mode)
{
    if (mkdir(path, mode) < 0)
    {
        if (errno != EEXIST) printf("%s",path);
    }
}

static pid_t spawn(const char* path, char* const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0)
    {
        execve(path, argv, environ);
        _exit(127);
    }

    return pid;
}

static void wait_for_socket(const char* path)
{
    struct stat st;
    while (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode))
    {
        usleep(100000);
    }
}

static void wait_for_interface(const char* ifname)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    strcpy(ifr.ifr_name, ifname);

    while (1)
    {
        if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0)
        {
            if (ifr.ifr_flags & IFF_UP) break;
        }
        usleep(200000);
    }

    close(fd);
}

static char* wait_for_lease(const char* ifname)
{
    int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sock < 0)
    {
        perror("socket");
        return NULL;
    }

    struct sockaddr_nl addr = {0};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_IPV4_IFADDR;  // subscribe to IPv4 addr changes

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(sock);
        return NULL;
    }

    char buf[4096];
    for (;;)
    {
        ssize_t len = recv(sock, buf, sizeof(buf), 0);
        if (len < 0)
        {
            perror("recv");
            close(sock);
            return NULL;
        }

        struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
        for (; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len))
        {
            if (nlh->nlmsg_type == RTM_NEWADDR)
            {
                struct ifaddrmsg* ifa = NLMSG_DATA(nlh);
                if (ifa->ifa_family != AF_INET) continue;

                char ifname_buf[IF_NAMESIZE];
                if_indextoname(ifa->ifa_index, ifname_buf);

                if (strcmp(ifname_buf, ifname) == 0)
                {
                    struct rtattr* rta = IFA_RTA(ifa);
                    int rta_len = IFA_PAYLOAD(nlh);
                    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len))
                    {
                        if (rta->rta_type == IFA_LOCAL)
                        {
                            char ip[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, RTA_DATA(rta), ip, sizeof(ip));
                            close(sock);
                            return strdup(ip);  // caller must free
                        }
                    }
                }
            }
        }
    }
}

static void sigchld_handler(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
    {
        // reaped
    }
}

int main() {
    struct sigaction sa = {0};
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* ensure machine-id */
    char* uuid_argv[] = {"/usr/bin/dbus-uuidgen", "--ensure=/var/lib/dbus/machine-id", NULL};
    spawn("/usr/bin/dbus-uuidgen", uuid_argv);

    /* -------------------------
     * Start dbus-daemon
     * ------------------------- */
    safe_mkdir("/run/dbus", 0755);
    char* dbus_argv[] = {"/usr/bin/dbus-daemon", "--system",
                         "--address=unix:path=/run/dbus/system_bus_socket", NULL};
    spawn("/usr/bin/dbus-daemon", dbus_argv);

    wait_for_socket("/run/dbus/system_bus_socket");

    /* -------------------------
     * Start wmt_manager
     * ------------------------- */
    pid_t wmt = fork();
    if (wmt == 0)
    {
        execl("/sbin/wmt_manager", "wmt_manager", NULL);
        _exit(127);
    }

    /* Wait for wmt_manager to finish */
    int status;
    waitpid(wmt, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        printf("wmt_manager failed\n");
        return 0;
    }

    /* wlan0 is now available */
    char* ip_up_argv[] = {"/usr/sbin/ip", "link", "set", "lo", "up", NULL};
    spawn("/usr/sbin/ip", ip_up_argv);

    ip_up_argv[3] = "wlan0";
    spawn("/usr/sbin/ip", ip_up_argv);

    wait_for_interface("wlan0");

    /* -------------------------
     * Start wpa_supplicant
     * ------------------------- */
    safe_mkdir("/run/wpa_supplicant", 0755);
    char* wpa_argv[] = {"/usr/sbin/wpa_supplicant", "-B", "-i",      "wlan0", "-c",
                        "/etc/wpa_supplicant.conf", "-D", "nl80211", NULL};
    spawn("/usr/sbin/wpa_supplicant", wpa_argv);

    wait_for_socket("/var/run/wpa_supplicant/wlan0");

    /* -------------------------
     * DHCP
     * ------------------------- */
    char* dh_argv[] = {"/usr/sbin/dhcpcd", "wlan0", NULL};
    spawn("/usr/sbin/dhcpcd", dh_argv);

    char* ip = wait_for_lease("wlan0");

    /* -------------------------
     * dnsmasq
     * ------------------------- */
    char dhcp_option[64];
    snprintf(dhcp_option, sizeof(dhcp_option), "6,%s", ip);

    char dns_address[128];
    snprintf(dns_address, sizeof(dns_address), "/tazi.lan/%s", ip);

    struct in_addr addr;
    inet_aton(ip, &addr);
    uint32_t base = ntohl(addr.s_addr) & 0xFFFFFF00;  // /24 mask
    char dhcp_range[64];
    snprintf(dhcp_range, sizeof(dhcp_range), "%u.%u.%u.50,%u.%u.%u.150,24h",
             (base >> 24) & 0xFF, (base >> 16) & 0xFF, (base >> 8) & 0xFF, (base >> 24) & 0xFF,
             (base >> 16) & 0xFF, (base >> 8) & 0xFF);

    char* argv[] = {"dnsmasq", "-F",        dhcp_range,         "-O", dhcp_option,
                    "-A",      dns_address, "--server=8.8.8.8", "-d", NULL};

    spawn("/usr/sbin/dnsmasq", argv);

    free(ip);

    while (true)
    {
        struct addrinfo hints = {0}, *res;
        hints.ai_family = AF_INET;
        int ret = getaddrinfo("google.com", NULL, &hints, &res);
        if (ret == 0)
        {
            freeaddrinfo(res);
            break;  // DNS resolution works
        }
        sleep(1);
    }

    /* -------------------------
     * ntpd -gq (one-shot)
     * ------------------------- */
    char* ntp_argv[] = {"/usr/sbin/ntpd", "-gq", NULL};
    spawn("/usr/sbin/ntpd", ntp_argv);

    /* -------------------------
     * sshd
     * ------------------------- */
    char* sshd_argv[] = {"/usr/sbin/sshd", NULL};
    spawn("/usr/sbin/sshd", sshd_argv);

    /* -------------------------
     * nginx
     * ------------------------- */
    char* nginx_argv[] = {"/usr/sbin/nginx", NULL};
    spawn("/usr/sbin/nginx", nginx_argv);

    return 0;
}
