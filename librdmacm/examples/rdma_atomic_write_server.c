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
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

static const char *server = "0.0.0.0";
static const char *port = "7471";

static struct rdma_cm_id *listen_id, *id;
static struct ibv_mr *write_dst_mr;
static char write_dst[8];
struct priv_data {
	uint64_t buf_va;
	uint32_t buf_rkey;
};

static int run(void)
{
	struct rdma_addrinfo hints, *res;
	struct rdma_event_channel *channel;
	struct ibv_qp_init_attr attr;
	struct rdma_conn_param conn_param = {};
	struct rdma_cm_event *event;
	struct priv_data pdata;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = RAI_PASSIVE;
	hints.ai_port_space = RDMA_PS_TCP;
	ret = rdma_getaddrinfo(server, port, &hints, &res);
	if (ret) {
		printf("rdma_getaddrinfo: %s\n", gai_strerror(ret));
		return ret;
	}

	memset(&attr, 0, sizeof(attr));
	attr.cap.max_send_wr = attr.cap.max_recv_wr = 1;
	attr.cap.max_send_sge = attr.cap.max_recv_sge = 1;
	attr.sq_sig_all = 1;
	ret = rdma_create_ep(&listen_id, res, NULL, &attr);
	if (ret) {
		perror("rdma_create_ep");
		goto out_free_addrinfo;
	}

	ret = rdma_listen(listen_id, 0);
	if (ret) {
		perror("rdma_listen");
		goto out_destroy_listen_ep;
	}

	ret = rdma_get_request(listen_id, &id);
	if (ret) {
		perror("rdma_get_request");
		goto out_destroy_listen_ep;
	}

	write_dst_mr = rdma_reg_write(id, write_dst, 8);
	if (!write_dst_mr) {
		ret = -1;
		perror("rdma_reg_write for write_dst");
		goto out_destroy_accept_ep;
	}

	channel = rdma_create_event_channel();
	if (!channel) {
		perror("rdma_create_event_channel");
		goto out_destroy_accept_ep;
	}

	ret = rdma_migrate_id(id, channel);
	if (ret) {
		perror("rdma_migrate_id");
		goto out_destroy_evch;
	}

	pdata.buf_va = (uintptr_t)write_dst;
	pdata.buf_rkey = write_dst_mr->rkey;
	conn_param.private_data = &pdata;
	conn_param.private_data_len = sizeof(pdata);
	conn_param.responder_resources = 16;

	ret = rdma_accept(id, &conn_param);
	if (ret) {
		perror("rdma_accept");
		goto out_dereg_mr;
	}

	ret = rdma_get_cm_event(id->channel, &event);
	if (ret) {
		perror("rdma_get_cm_event");
		goto out_disconnect;
	}

	if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
		printf("not RDMA_CM_EVENT_ESTABLISHED\n");
		goto out_disconnect;
	}

	rdma_ack_cm_event(event);

	ret = rdma_get_cm_event(id->channel, &event);
	if (ret) {
		perror("rdma_get_cm_event");
		goto out_disconnect;
	}

	if (event->event != RDMA_CM_EVENT_DISCONNECTED) {
		printf("not RDMA_CM_EVENT_DISCONNECTED\n");
		goto out_disconnect;
	}

	rdma_ack_cm_event(event);

	printf("%s\n", write_dst);

out_disconnect:
	rdma_disconnect(id);
out_destroy_evch:
	rdma_destroy_event_channel(channel);
out_dereg_mr:
	rdma_dereg_mr(write_dst_mr);
out_destroy_accept_ep:
	rdma_destroy_ep(id);
out_destroy_listen_ep:
	rdma_destroy_ep(listen_id);
out_free_addrinfo:
	rdma_freeaddrinfo(res);
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

	printf("rdma_atomic_write_server: start\n");
	ret = run();
	printf("rdma_atomic_write_server: end %d\n", ret);
	return ret;
}
