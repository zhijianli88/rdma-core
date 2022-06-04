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
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

static const char *server = "127.0.0.1";
static const char *port = "7471";

static struct rdma_cm_id *id;
static char atomic_wr[8] = "4168";
struct priv_data {
	uint64_t buf_va;
	uint32_t buf_rkey;
};

static int run(void)
{
	struct rdma_addrinfo hints, *res = NULL;
	struct rdma_event_channel *channel = NULL;
	struct ibv_qp_init_attr_ex attr_ex;
	struct rdma_cm_event *event = NULL;
	struct priv_data pdata;
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
	attr_ex.cap.max_send_wr = attr_ex.cap.max_recv_wr = 1;
	attr_ex.cap.max_send_sge = attr_ex.cap.max_recv_sge = 1;
	attr_ex.qp_context = id;
	attr_ex.sq_sig_all = 0;
	attr_ex.qp_type = IBV_QPT_RC;
	attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
	attr_ex.send_ops_flags = IBV_QP_EX_WITH_RDMA_ATOMIC_WRITE;

	ret = rdma_create_qp_ex(id, &attr_ex);
	if (ret) {
		perror("rdma_create_qp_ex");
		goto out_destroy_id;
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

	memcpy(&pdata, event->param.conn.private_data, sizeof(pdata));

	rdma_ack_cm_event(event);

	qpx = ibv_qp_to_qp_ex(id->qp);
	if (!qpx) {
		perror("ibv_qp_to_qp_ex");
		goto out_disconnect;
	}

	ibv_wr_start(qpx);
	qpx->wr_flags = IBV_SEND_SIGNALED;
	ibv_wr_rdma_atomic_write(qpx, pdata.buf_rkey, pdata.buf_va, atomic_wr);
	ibv_wr_complete(qpx);

	do {
		ret = rdma_get_send_comp(id, &wc);
		if (ret < 0) {
			perror("rdma_get_send_comp");
			goto out_disconnect;
		}
	} while (!ret);

	printf("wc.opcode %d %s\n", wc.opcode, atomic_wr);

out_disconnect:
	rdma_disconnect(id);
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

	while ((op = getopt(argc, argv, "s:p:")) != -1) {
		switch (op) {
		case 's':
			server = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		default:
			printf("usage: %s\n", argv[0]);
			printf("\t[-s server_address]\n");
			printf("\t[-p port_number]\n");
			exit(1);
		}
	}

	printf("rdma_atomic_write_client: start\n");
	ret = run();
	printf("rdma_atomic_write_client: end %d\n", ret);
	return ret;
}
