/*
 * A network for a blackhole (bh open --net), for disposable test
 * environments rather than for malware.
 *
 * pasta (from passt) joins the blackhole's user and network namespaces
 * and gives it an interface whose traffic pasta carries out through
 * ordinary sockets of its own: nothing is routed or forwarded by the host
 * kernel, so the firewall, `worm anon` and `worm panic` treat it like any
 * program of the user's. Inside, the
 * blackhole has BH_NET_ADDR/BH_NET_PLEN with the gateway BH_NET_GW;
 * name lookups to BH_NET_DNS go to the host's resolver (the local
 * encrypted one). Nothing can connect in: no ports are forwarded and the
 * host's loopback is not mapped.
 *
 * The local network stays out: before the sandbox, init adds "prohibit"
 * routes for private, link-local, shared and multicast ranges and for the
 * host's own addresses, which programs inside cannot change (no
 * CAP_NET_ADMIN). --lan leaves out the private ranges, never the host.
 * There is no IPv6.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bh.h"

#define PASTA "/usr/bin/pasta"
#define MAX_HOST 32

static const struct { const char *net; int len; } private_ranges[] = {
	{ "0.0.0.0", 8 }, { "10.0.0.0", 8 }, { "100.64.0.0", 10 }, { "169.254.0.0", 16 },
	{ "172.16.0.0", 12 }, { "192.0.0.0", 24 }, { "192.0.2.0", 24 }, { "192.168.0.0", 16 },
	{ "198.18.0.0", 15 }, { "198.51.100.0", 24 }, { "203.0.113.0", 24 },
	{ "224.0.0.0", 4 }, { "240.0.0.0", 4 },
};

/* The host's own IPv4 addresses, collected before the clone. */
static struct in_addr host_addr[MAX_HOST];
static int host_count;

int net_collect_host(void)
{
	struct ifaddrs *list, *i;
	struct stat st;

	if (stat(PASTA, &st) < 0) {
		warn("no network without pasta (%s): install the passt package", PASTA);
		return -1;
	}
	if (getifaddrs(&list) < 0)
		return 0;
	for (i = list; i && host_count < MAX_HOST; i = i->ifa_next) {
		struct in_addr a;

		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
			continue;
		a = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
		if ((ntohl(a.s_addr) >> 24) == 127)
			continue;
		host_addr[host_count++] = a;
	}
	freeifaddrs(list);
	return 0;
}

/* Run pasta for child's namespaces; it stays in the background until child exits. */
int net_start(pid_t child)
{
	char spid[16], netns[64];
	int status;
	pid_t pid;

	snprintf(spid, sizeof(spid), "%d", (int)child);
	snprintf(netns, sizeof(netns), "/proc/%d/ns/net", (int)child);
	if ((pid = fork()) < 0)
		return -1;
	if (pid == 0) {
		char *argv[40] = {
			"pasta", "--config-net", "--quiet", "--ipv4-only",
			"--address", BH_NET_ADDR, "--netmask", BH_NET_PLEN,
			"--gateway", BH_NET_GW, "--dns-forward", BH_NET_DNS,
			"--no-map-gw", "--no-dhcp",
			"--tcp-ports", "none", "--udp-ports", "none",
			"--tcp-ns", "none", "--udp-ns", "none",
			"--ns-ifname", "eth0",
		};
		int n = 0;

		while (argv[n])
			n++;
		if (getuid() == 0) {
			/*
			 * Given a user namespace, pasta drops to nobody before
			 * joining it, and nobody can join neither root's
			 * namespace nor see its process. Root's pasta joins only
			 * the network namespace and stays root; it then shuts
			 * itself in a namespace of its own without capabilities.
			 */
			argv[n++] = "--runas";
			argv[n++] = "0";
			argv[n++] = "--netns";
			argv[n++] = netns;
			argv[n++] = "--netns-only";
		} else {
			argv[n++] = spid;
		}
		argv[n] = NULL;
		execv(PASTA, argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		warn("pasta could not set up the network");
		return -1;
	}
	return 0;
}

static int nl_fd = -1;
static unsigned nl_seq;

/* Add a prohibit route for net/len (EACCES for anything sent there). */
static int prohibit(struct in_addr net, int len)
{
	struct {
		struct nlmsghdr nh;
		struct rtmsg rt;
		char attrs[64];
	} req = { 0 };
	struct rtattr *rta;
	char reply[4096];
	ssize_t r;

	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	req.nh.nlmsg_type = RTM_NEWROUTE;
	req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE;
	req.nh.nlmsg_seq = ++nl_seq;
	req.rt.rtm_family = AF_INET;
	req.rt.rtm_dst_len = len;
	req.rt.rtm_table = RT_TABLE_MAIN;
	req.rt.rtm_protocol = RTPROT_STATIC;
	req.rt.rtm_scope = RT_SCOPE_NOWHERE;
	req.rt.rtm_type = RTN_PROHIBIT;
	rta = (struct rtattr *)((char *)&req + NLMSG_ALIGN(req.nh.nlmsg_len));
	rta->rta_type = RTA_DST;
	rta->rta_len = RTA_LENGTH(sizeof(net));
	memcpy(RTA_DATA(rta), &net, sizeof(net));
	req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);

	if (send(nl_fd, &req, req.nh.nlmsg_len, 0) < 0)
		return -1;
	if ((r = recv(nl_fd, reply, sizeof(reply), 0)) < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr)))
		return -1;
	struct nlmsghdr *nh = (struct nlmsghdr *)reply;
	if (nh->nlmsg_type == NLMSG_ERROR) {
		int err = ((struct nlmsgerr *)NLMSG_DATA(nh))->error;

		if (err) {
			errno = -err;
			return -1;
		}
	}
	return 0;
}

int net_routes(int lan)
{
	struct in_addr a;
	int ret = 0;

	if ((nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)) < 0) {
		warn("cannot configure the network: %s", strerror(errno));
		return -1;
	}
	for (size_t i = 0; !lan && i < sizeof(private_ranges) / sizeof(private_ranges[0]); i++) {
		inet_pton(AF_INET, private_ranges[i].net, &a);
		if (prohibit(a, private_ranges[i].len) < 0) {
			warn("cannot keep %s/%d out: %s", private_ranges[i].net,
			     private_ranges[i].len, strerror(errno));
			ret = -1;
		}
	}
	for (int i = 0; i < host_count; i++)
		if (prohibit(host_addr[i], 32) < 0) {
			warn("cannot keep the host out: %s", strerror(errno));
			ret = -1;
		}
	close(nl_fd);
	return ret;
}
