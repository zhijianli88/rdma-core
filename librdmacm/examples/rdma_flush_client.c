// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright 2022, Fujitsu.  All rights reserved.
 *
 * This software is available to you under the OpenIB.org BSD license
 * below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AWV
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

static const char *server = "127.0.0.1";
static const char *port = "7471";

static struct rdma_cm_id *id;
static struct ibv_mr *mr, *send_mr, *rdma_write_src_mr;
static int send_flags;
static struct ibv_mr send_msg;
static struct ibv_mr recv_msg;
static struct ibv_sge sge = {};
static int placement_type = IB_EXT_PLT_GLB_VIS;
static int select_level = IB_EXT_SEL_MR_RANGE;
static const char *placement_type_str[4] = {
	"none", "Global visibility", "Persistence",
	"Global visibility and Persistence"
};
static const char *select_level_str[2] = {
	"Memory region range",
	"Whole memory region"
};

static int run(void)
{
	char rdma_write_src[1024] = {};
	struct rdma_addrinfo hints, *res = NULL;
	struct rdma_event_channel *channel = NULL;
	struct ibv_qp_init_attr_ex attr_ex;
	struct rdma_cm_event *event = NULL;
	struct ibv_qp_ex *qpx = NULL;
	struct ibv_wc wc;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_port_space = RDMA_PS_TCP;
	ret = rdma_getaddrinfo(server, port, &hints, &res);
	if (ret) {
		printf("rdma_getaddrinfo: %s\n", gai_strerror(ret));
		goto out;
	}

	ret = rdma_create_id(NULL, &id, NULL, RDMA_PS_TCP);
	if (ret) {
		perror("rdma_create_id");
		goto out_free_addrinfo;
	}

	ret = rdma_resolve_addr(id, res->ai_src_addr, res->ai_dst_addr, 2000);
	if (ret) {
		perror("rdma_resolve_addr");
		goto out_destroy_id;
	}

	ret  = rdma_resolve_route(id, 2000);
	if (ret) {
		perror("rdma_resolve_route");
		goto out_destroy_id;
	}

	memset(&attr_ex, 0, sizeof(attr_ex));
	attr_ex.cap.max_send_wr = attr_ex.cap.max_recv_wr = 2;
	attr_ex.cap.max_send_sge = attr_ex.cap.max_recv_sge = 2;
	attr_ex.cap.max_inline_data = 256;
	attr_ex.qp_context = id;
	attr_ex.sq_sig_all = 1;
	attr_ex.qp_type = IBV_QPT_RC;
	attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD |
			    IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
	attr_ex.send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE |
				 IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM |
				 IBV_QP_EX_WITH_SEND |
				 IBV_QP_EX_WITH_SEND_WITH_IMM |
				 IBV_QP_EX_WITH_RDMA_FLUSH;

	ret = rdma_create_qp_ex(id, &attr_ex);
	if (ret) {
		perror("rdma_create_qp_ex");
		goto out_destroy_id;
	}

	if (attr_ex.cap.max_inline_data >= sizeof(recv_msg))
		send_flags = IBV_SEND_INLINE;
	else
		printf("rdma_client: device doesn't support IBV_SEND_INLINE, using sge sends\n");

	mr = rdma_reg_msgs(id, &recv_msg, sizeof(recv_msg));
	if (!mr) {
		perror("rdma_reg_msgs for recv_msg");
		ret = -1;
		goto out_destroy_qp;
	}
	if ((send_flags & IBV_SEND_INLINE) == 0) {
		send_mr = rdma_reg_msgs(id, &send_msg, sizeof(recv_msg));
		if (!send_mr) {
			perror("rdma_reg_msgs for send_msg");
			ret = -1;
			goto out_dereg_recv;
		}
	}

	ret = rdma_post_recv(id, NULL, &recv_msg, sizeof(recv_msg), mr);
	if (ret) {
		perror("rdma_post_recv");
		goto out_dereg_send;
	}

	channel = rdma_create_event_channel();
	if (!channel) {
		perror("rdma_create_event_channel");
		goto out_destroy_qp;
	}

	ret = rdma_migrate_id(id, channel);
	if (ret) {
		perror("rdma_migrate_id");
		goto out_destroy_evch;
	}

	ret = rdma_connect(id, NULL);
	if (ret) {
		perror("rdma_connect");
		goto out_destroy_evch;
	}

	ret = rdma_get_cm_event(channel, &event);
	if (ret) {
		perror("rdma_get_cm_event");
		goto out_disconnect;
	}

	if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
		printf("not RDMA_CM_EVENT_ESTABLISHED %d, %d\n",
			RDMA_CM_EVENT_ESTABLISHED, event->event);
		goto out_disconnect;
	}

	rdma_ack_cm_event(event);

	ret = rdma_post_send(id, NULL, &send_msg, sizeof(recv_msg),
			     send_mr, send_flags);
	if (ret) {
		perror("rdma_post_send");
		goto out_disconnect;
	}

	while ((ret = rdma_get_send_comp(id, &wc)) == 0)
		;
	if (ret < 0 || wc.status) {
		printf("rdma_post_send complete, ret: %d, wc.status: %d\n",
			ret, wc.status);
		goto out_disconnect;
	}

	while ((ret = rdma_get_recv_comp(id, &wc)) == 0)
		;
	if (ret < 0 || wc.status) {
		printf("rdma_post_recv complete, ret: %d, wc.status: %d\n",
			ret, wc.status);
		goto out_disconnect;
	}

	// rdma write src
	sprintf(rdma_write_src, "%p: I'm from source", &wc);
	int src_len = strlen((const char *)rdma_write_src) + 1;

	rdma_write_src_mr = rdma_reg_write(id, rdma_write_src, src_len);
	if (!rdma_write_src_mr) {
		ret = -1;
		perror("rdma_reg_write for rdma_write_src");
		goto out_disconnect;
	}

	// source sge
	sge.addr =  (uint64_t)(uintptr_t)rdma_write_src;
	sge.length = src_len;
	sge.lkey = rdma_write_src_mr->lkey;

	ret = rdma_post_writev(id, (void *)&sge, &sge, 1, IBV_SEND_SIGNALED,
			       (uint64_t)(uintptr_t)recv_msg.addr, recv_msg.rkey);
	if (ret) {
		perror("failed to rdma_post_writev");
		goto out_dereg_mr;
	}
	printf("client RDMA WRITE: %s\n", rdma_write_src);

	while ((ret = rdma_get_send_comp(id, &wc)) == 0)
		;
	if (ret < 0 || wc.status) {
		printf("rdma_post_writev complete, ret: %d, wc.status: %d\n",
			ret, wc.status);
		goto out_dereg_mr;
	}

	qpx = ibv_qp_to_qp_ex(id->qp);
	if (!qpx) {
		perror("ibv_qp_to_qp_ex");
		goto out_dereg_mr;
	}

	ibv_wr_start(qpx);
	qpx->wr_flags = IBV_SEND_SIGNALED;
	ret = ibv_wr_rdma_flush(qpx, recv_msg.rkey, (uint64_t)(uintptr_t)recv_msg.addr,
				src_len, placement_type, select_level);
	ibv_wr_complete(qpx);

	if (ret) {
		perror("failed to ibv_wr_rdma_flush");
		goto out_dereg_mr;
	}

	while ((ret = rdma_get_send_comp(id, &wc)) == 0)
		;
	if (ret < 0 || wc.status) {
		printf("ibv_wr_rdma_flush complete, ret: %d, wc.status: %d\n",
			ret, wc.status);
	}

	ret = 0;

out_dereg_mr:
	rdma_dereg_mr(rdma_write_src_mr);
out_disconnect:
	rdma_disconnect(id);
out_dereg_send:
	if ((send_flags & IBV_SEND_INLINE) == 0)
		rdma_dereg_mr(send_mr);
out_dereg_recv:
	rdma_dereg_mr(mr);
out_destroy_evch:
	rdma_destroy_event_channel(channel);
out_destroy_qp:
	rdma_destroy_qp(id);
out_destroy_id:
	rdma_destroy_id(id);
out_free_addrinfo:
	rdma_freeaddrinfo(res);
out:
	return ret;
}

int main(int argc, char **argv)
{
	int op, ret;

	while ((op = getopt(argc, argv, "s:p:t:l:")) != -1) {
		switch (op) {
		case 's':
			server = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		case 't':
			switch (atoi(optarg)) {
			case 1:
				placement_type = IB_EXT_PLT_GLB_VIS;
				break;
			case 2:
				placement_type = IB_EXT_PLT_PERSIST;
				break;
			case 3:
				placement_type = IB_EXT_PLT_GLB_VIS |
						 IB_EXT_PLT_PERSIST;
				break;
			default:
				placement_type = 0;
				break;
			}
			break;
		case 'l':
			switch (atoi(optarg)) {
			case 0:
				select_level = IB_EXT_SEL_MR_RANGE;
				break;
			case 1:
				select_level = IB_EXT_SEL_MR_WHOLE;
				break;
			default:
				break;
			}
			break;
		default:
			printf("usage: %s\n", argv[0]);
			printf("\t[-s server_address]\n");
			printf("\t[-p port_number]\n");
			printf("\t[-l select level: 0: memory region range, 1: whole memory region]\n");
			printf("\t[-t placement type, 1: global visibility, 2: persistence, 3: both]\n");
			exit(1);
		}
	}

	printf("rdma_flush_client: start\n");
	printf("Placement type: %s\n", placement_type_str[placement_type]);
	printf("Selectivity level: %s\n", select_level_str[select_level]);
	ret = run();
	printf("rdma_flush_client: end %d\n", ret);
	return ret;
}
