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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

static const char *server = "0.0.0.0";
static const char *port = "7471";

static struct rdma_cm_id *listen_id, *id;
static struct ibv_mr *mr, *send_mr, *rdma_write_dest_mr;
static int send_flags;
static struct ibv_mr send_msg;
static struct ibv_mr recv_msg;
static uint8_t *rdma_write_dest;
static int flush_access = IBV_ACCESS_FLUSH_GLOBAL_VISIBLITY;

static int run(void)
{
	struct rdma_addrinfo hints, *res;
	struct ibv_qp_init_attr init_attr;
	struct ibv_qp_attr qp_attr;
	struct ibv_wc wc;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = RAI_PASSIVE;
	hints.ai_port_space = RDMA_PS_TCP;
	ret = rdma_getaddrinfo(server, port, &hints, &res);
	if (ret) {
		printf("rdma_getaddrinfo: %s\n", gai_strerror(ret));
		return ret;
	}

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = init_attr.cap.max_recv_wr = 1;
	init_attr.cap.max_send_sge = init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_inline_data = sizeof(recv_msg);
	init_attr.sq_sig_all = 1;
	ret = rdma_create_ep(&listen_id, res, NULL, &init_attr);
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

	memset(&qp_attr, 0, sizeof(qp_attr));
	memset(&init_attr, 0, sizeof(init_attr));
	ret = ibv_query_qp(id->qp, &qp_attr, IBV_QP_CAP,
			   &init_attr);
	if (ret) {
		perror("ibv_query_qp");
		goto out_destroy_accept_ep;
	}
	if (init_attr.cap.max_inline_data >= sizeof(recv_msg))
		send_flags = IBV_SEND_INLINE;
	else
		printf("rdma_server: device doesn't support IBV_SEND_INLINE, using sge sends\n");

	mr = rdma_reg_msgs(id, &recv_msg, sizeof(recv_msg));
	if (!mr) {
		ret = -1;
		perror("rdma_reg_msgs for recv_msg");
		goto out_destroy_accept_ep;
	}
	if ((send_flags & IBV_SEND_INLINE) == 0) {
		send_mr = rdma_reg_msgs(id, &send_msg, sizeof(recv_msg));
		if (!send_mr) {
			ret = -1;
			perror("rdma_reg_msgs for send_msg");
			goto out_dereg_recv;
		}
	}

	ret = rdma_post_recv(id, NULL, &recv_msg, sizeof(recv_msg), mr);
	if (ret) {
		perror("rdma_post_recv");
		goto out_dereg_send;
	}

	ret = rdma_accept(id, NULL);
	if (ret) {
		perror("rdma_accept");
		goto out_dereg_send;
	}

	while ((ret = rdma_get_recv_comp(id, &wc)) == 0)
		;
	if (ret < 0 || wc.status) {
		printf("rdma_post_recv complete, ret: %d, wc.status: %d\n",
			ret, wc.status);
		goto out_disconnect;
	}

	// rdma write dest
	fprintf(stderr, "flush access %x\n", flush_access);
	rdma_write_dest_mr =
	ibv_reg_mr(id->pd, rdma_write_dest, 1024, IBV_ACCESS_LOCAL_WRITE |
						    IBV_ACCESS_REMOTE_WRITE |
						    flush_access);
	if (!rdma_write_dest_mr) {
		ret = -1;
		perror("rdma_reg_msgs for rdma_write_dest");
		goto out_disconnect;
	}

	memcpy(&send_msg, rdma_write_dest_mr, sizeof(*mr));

	ret = rdma_post_send(id, NULL, &send_msg, sizeof(recv_msg),
			     send_mr, send_flags);
	if (ret) {
		perror("rdma_post_send");
		goto out_dereg_mr;
	}

	while ((ret = rdma_get_send_comp(id, &wc)) == 0)
		;
	if (ret < 0 || wc.status) {
		printf("rdma_post_send complete, ret: %d, wc.status: %d\n",
			ret, wc.status);
	}
	ret = 0;

	/* wait flush */
	sleep(1);
	printf("server recv: %s\n", rdma_write_dest);

out_dereg_mr:
	rdma_dereg_mr(rdma_write_dest_mr);
out_disconnect:
	rdma_disconnect(id);
out_dereg_send:
	if ((send_flags & IBV_SEND_INLINE) == 0)
		rdma_dereg_mr(send_mr);
out_dereg_recv:
	rdma_dereg_mr(mr);
out_destroy_accept_ep:
	rdma_destroy_ep(id);
out_destroy_listen_ep:
	rdma_destroy_ep(listen_id);
out_free_addrinfo:
	rdma_freeaddrinfo(res);
	return ret;
}

#define MAP_LEN 0x1fffff
#define REG_PREFIX "Register MR flush access flags: "

int main(int argc, char **argv)
{
	int op, ret;
	char dax_file[1024] = { 0 };
	int fd = -1;

	while ((op = getopt(argc, argv, "s:p:f:t:")) != -1) {
		switch (op) {
		case 's':
			server = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		case 'f':
			// dev dax file
			strcpy(dax_file, optarg);
			break;
		case 't':
			switch (atoi(optarg)) {
			case 1:
				flush_access =
					IBV_ACCESS_FLUSH_GLOBAL_VISIBLITY;
				printf(REG_PREFIX"global visibility\n");
				break;
			case 2:
				flush_access = IBV_ACCESS_FLUSH_PERSISTENT;
				printf(REG_PREFIX"persistence\n");
				break;
			case 3:
				flush_access =
					IBV_ACCESS_FLUSH_GLOBAL_VISIBLITY |
					IBV_ACCESS_FLUSH_PERSISTENT;
				printf(REG_PREFIX"global visibility and persistence\n");
				break;
			default:
				flush_access = 0;
				break;
			}
			break;
		default:
			printf("usage: %s\n", argv[0]);
			printf("\t[-s server_address]\n");
			printf("\t[-p port_number]\n");
			printf("\t[-f dax file]\n");
			printf("\t[-t flush access flags, "
				"1: global visibility, 2: persistence, 3: both]\n");
			exit(1);
		}
	}

	if (dax_file[0]) {
		int mflags = MAP_SHARED;
#ifdef MAP_SYNC
		mflags |= MAP_SYNC;
#endif
		fd = open(dax_file, O_RDWR, 0);
		if (fd < 0) {
			perror("Failed to open daxfile\n");
			return -1;
		}
		printf("mmap dev dax file %s\n", dax_file);
		rdma_write_dest = mmap(NULL, MAP_LEN, PROT_WRITE | PROT_READ,
				       mflags, fd, 0);
		if (rdma_write_dest == MAP_FAILED) {
			perror("failed to mmap dax_file");
			return -1;
		}
	} else {
		rdma_write_dest = malloc(MAP_LEN);
	}

	printf("rdma_server: start\n");
	ret = run();
	printf("rdma_server: end %d\n", ret);
	if (fd > 0) {
		close(fd);
		munmap(rdma_write_dest, MAP_LEN);
	} else
		free(rdma_write_dest);
	return ret;
}
