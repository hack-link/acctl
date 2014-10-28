/*
 * ============================================================================
 *
 *       Filename:  process.c
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  2014年08月26日 10时04分23秒
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  jianxi sun (jianxi), ycsunjane@gmail.com
 *   Organization:  
 *
 * ============================================================================
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <assert.h>

#include "msg.h"
#include "log.h"
#include "dllayer.h"
#include "netlayer.h"
#include "link.h"
#include "net.h"
#include "arg.h"
#include "thread.h"
#include "process.h"
#include "apstatus.h"

#define SYSSTAT_LOCK() 	(pthread_mutex_lock(&sysstat.lock))
#define SYSSTAT_UNLOCK() (pthread_mutex_unlock(&sysstat.lock))

struct sysstat_t sysstat = {
	.acuuid = {0},
	.isreg = 0,
	.sock = -1,
	.dmac = {0},
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

static void __fill_msg_header(struct msg_head_t *msg, int msgtype)
{
	memcpy(&msg->acuuid[0], 
		&sysstat.acuuid[0], UUID_LEN);
	memcpy(&msg->mac[0], 
		&argument.mac[0], ETH_ALEN);
	msg->msg_type = msgtype; 
}

static void ac_reconnect()
{
	if(!sysstat.server.sin_addr.s_addr)
		return;

	int ret;
	struct nettcp_t tcp;
	SYSSTAT_LOCK();
	/* other process have reconnect */
	if(sysstat.sock >= 0)
		goto unlock;

	tcp.addr = sysstat.server;
	ret = tcp_connect(&tcp);
	if(ret < 0)
		goto unlock;

	pr_ipv4(&tcp.addr);
	sysstat.sock = ret;
	SYSSTAT_UNLOCK();
	sys_debug("connect success: %d\n", sysstat.sock);
	insert_sockarr(tcp.sock, __net_netrcv, NULL);
	return;

unlock:
	SYSSTAT_UNLOCK();
	return;
}

static void *report_apstatus(void *arg)
{
	int ret;
	struct apstatus_t *ap;

	int bufsize = sizeof(struct msg_ap_status_t) +
		sizeof(struct apstatus_t);
	char *buf = malloc(bufsize);
	if(buf == 0) {
		sys_err("Malloc for report apstatus failed:%s ", 
			strerror(errno));
		exit(-1);
	}

	int proto;
	struct msg_ap_status_t *msg = (void *)buf;
	__fill_msg_header(&msg->header, MSG_AP_STATUS);

	/* ap will first connect remote ac, but if find local ac, ap will
	 * connect to local ac */
	while(1) {
		proto = (sysstat.sock >= 0) ? MSG_PROTO_TCP : MSG_PROTO_ETH;
		if(proto == MSG_PROTO_ETH) {
			ac_reconnect();
			goto wait;
		}

		ap = get_apstatus();
		memcpy(buf + sizeof(struct msg_ap_status_t),
			ap, sizeof(struct apstatus_t));

		ret = net_send(proto, sysstat.sock, sysstat.dmac, buf, bufsize);
		if(ret <= 0 && proto == MSG_PROTO_TCP) {
			ac_lost();
			ac_reconnect();
		}
wait:
		sys_debug("report ap status (next %d seconds later)\n", 
			argument.reportitv);
		sleep(argument.reportitv);
	}
	return NULL;
}

static int __uuid_equ(char *src, char *dest)
{
	return !strncmp(src, dest, UUID_LEN - 1);
}

static void _proc_brd(struct msg_ac_brd_t *msg, int proto)
{
	/* send current ipv4 */
	struct msg_ap_reg_t *data = 
		malloc(sizeof(struct msg_ap_reg_t));
	if(data == NULL) {
		sys_warn("Malloc for response failed:%s\n", 
			strerror(errno));
		return;
	}
	__fill_msg_header((void *)data, MSG_AP_REG);
	data->ipv4 = argument.addr;
	net_send(proto, -1, &msg->header.mac[0], 
		(void *)data, sizeof(struct msg_ap_reg_t));
	free(data);
}

static void _proc_brd_isreg(struct msg_ac_brd_t *msg, int proto)
{
	if(!__uuid_equ(&msg->header.acuuid[0], &sysstat.acuuid[0])) {
		if(__uuid_equ(&msg->takeover[0], &sysstat.acuuid[0])) {
			if(sysstat.sock >= 0) {
				close(sysstat.sock);
				sysstat.sock = -1;
			}
			_proc_brd(msg, proto);
		} else {
			struct msg_ap_resp_t *data = 
				malloc(sizeof(struct msg_ap_resp_t));
			if(data == NULL) {
				sys_warn("Malloc for response failed:%s\n", 
					strerror(errno));
				return;
			}
			__fill_msg_header(data, MSG_AP_RESP);
			net_send(proto, -1, &sysstat.dmac[0], 
				(void *)data, sizeof(struct msg_ap_resp_t));
			free(data);
		}
	}
}

/*
 * reponse_brd recv broadcast msg from ac and update sysstat
 * */
static void proc_brd(struct msg_ac_brd_t *msg, int proto)
{
	assert(proto == MSG_PROTO_ETH);

	sys_debug("receive ac broadcast packet\n");
	if(sysstat.isreg)
		_proc_brd_isreg(msg, proto);
	else
		_proc_brd(msg, proto);
}

static int setaddr(struct sockaddr *addr)
{
	struct ifreq req;
	strncpy(req.ifr_name, argument.nic, IFNAMSIZ);
	req.ifr_addr = *addr;

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if(sockfd < 0) {
		sys_err("Create socket failed: %s(%d)\n",
			strerror(errno), errno);
		return 0;
	}

	int ret;
	ret = ioctl(sockfd, SIOCSIFADDR, &req);
	if(ret < 0) {
		sys_err("Set ap addr failed: %s(%d)\n",
			strerror(errno), errno);
		close(sockfd);
		return 0;
	}
	close(sockfd);
	return 1;
}

static void proc_reg_resp(struct msg_ac_reg_resp_t *msg, int proto)
{
	sys_debug("Recive ac reg response packet\n");
	strncpy(sysstat.acuuid, msg->header.acuuid, UUID_LEN);
	memcpy(sysstat.dmac, msg->header.mac, ETH_ALEN);
	if(msg->acaddr.sin_addr.s_addr)
		sysstat.server = msg->acaddr;
	if(msg->apaddr.sin_addr.s_addr) 
		setaddr((void *)&msg->apaddr);

	pr_ipv4(&msg->acaddr);
	pr_ipv4(&msg->apaddr);

	/* if have connect to remote ac, close it. 
	 * and connect to local ac */
	if(sysstat.sock >= 0)
		close(sysstat.sock);

	ac_reconnect();
	sysstat.isreg = 1;
}

static void __exec_cmd(struct msg_ac_cmd_t *cmd)
{
	sys_debug("receive ac command packet\n");
	printf("cmd: %s\n", cmd->cmd);
}

static int is_mine(struct msg_head_t *msg)
{
	if(!sysstat.isreg) {
		return 1;
	} else if(strncmp(msg->acuuid, sysstat.acuuid, UUID_LEN)) {
		sys_warn("receive invalid packet\n");
		return 0;
	} else {
		return 1;
	}
	return 0;
}

void msg_proc(struct msg_head_t *msg, int proto)
{
	if(!is_mine(msg))
		return;

	switch(msg->msg_type) {
	case MSG_AC_BRD:
		proc_brd((void *)msg, proto);
		break;
	case MSG_AC_REG_RESP:
		proc_reg_resp((void *)msg, proto);
		break;
	case MSG_AC_CMD:
		__exec_cmd((struct msg_ac_cmd_t *)msg);
		break;
	default:
		break;
	}
}

void ac_lost()
{
	sys_debug("ac lost\n");
	delete_sockarr(sysstat.sock);
	sysstat.sock = -1;
}

void init_report()
{
	/* init remote ac address */
	sysstat.server = argument.acaddr;
	__create_pthread(report_apstatus, NULL);
}
