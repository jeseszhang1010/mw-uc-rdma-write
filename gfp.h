#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>


#define PORT 28515
#define PKTSZ 4096
#define GRH_HEADER 40
#define NPOSTRECV 32768

struct ib_info {
    uint16_t lid;
    uint32_t qpn;
    uint32_t psn;
    uint32_t qkey;
    union ibv_gid gid;
    uint64_t buf_va; 
    uint32_t buf_rkey; 
};

struct ib_res {
    struct ibv_device **dev_list;
    struct ibv_device *ib_dev;
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mw *mw;
    int gidx;
    int port;
    struct ib_info local_info;
};


struct fragment_info {
    char* frag_ptr;
    uint16_t chunk_id;
};


static inline long long gfp_get_time(void)
{
    struct timespec time;
    long long nano_total;
    clock_gettime(CLOCK_MONOTONIC, &time);
    nano_total = time.tv_sec;
    nano_total *= 1000000000;
    nano_total += time.tv_nsec;
    return nano_total;
}


void my_exit(const char *message) {
	fprintf(stderr,"Error: %s. Exiting.\n",message);
	exit(EXIT_FAILURE);
}

int prepare_ib_res(struct ib_res *ib_res) {
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_port_attr port_attr;
    int num_devices = 0;
    char gid[33];
    int ret = 0;
    ib_res->gidx = 0;
    ib_res->port = 1;

    ib_res->dev_list = ibv_get_device_list(&num_devices);
    if (!ib_res->dev_list) {
        perror("ibv_get_device_list");
        ret = -1;
        goto cleanup;
    }

    if (num_devices == 0) {
        fprintf(stderr, "No InfiniBand devices found\n");
        ret = -1;
        goto cleanup;
    }

    // Use the first device found
    ib_res->ib_dev = ib_res->dev_list[0];
    // Get device context
    ib_res->context = ibv_open_device(ib_res->ib_dev);
    if (!ib_res->context) {
        perror("ibv_open_device");
        ret = -1;
        goto cleanup;
    }

    // Allocate Protection Domain (PD)
    ib_res->pd = ibv_alloc_pd(ib_res->context);
    if (!ib_res->pd) {
        perror("ibv_alloc_pd");
        ret = -1;
        goto cleanup;
    }

    ib_res->cq = ibv_create_cq(ib_res->context, 512, NULL, NULL, 0);
    if (!ib_res->cq) {
        perror("ibv_create_cq");
        ret = -1;
        goto cleanup;
    }

    // Create Queue Pair (QP)
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = ib_res->cq;
    qp_init_attr.recv_cq = ib_res->cq;
    qp_init_attr.cap.max_send_wr = 512;
    qp_init_attr.cap.max_recv_wr = 512;
    qp_init_attr.cap.max_send_sge = 2;
    qp_init_attr.cap.max_recv_sge = 2;
    qp_init_attr.qp_type = IBV_QPT_UC;

    ib_res->qp = ibv_create_qp(ib_res->pd, &qp_init_attr);
    if (!ib_res->qp) {
        perror("ibv_create_qp");
        ret = -1;
        goto cleanup;
    }

    // Modify QP to INIT
    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = ib_res->port;
    qp_attr.qp_access_flags |= IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;

    // UD
    // ret = ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY);
    // UC
    ret = ibv_modify_qp(ib_res->qp, &qp_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret)  {
        perror("ibv_modify_qp to INIT\n");
        goto cleanup;
    }

    // Get local LID/GID
    memset(&port_attr, 0, sizeof(struct ibv_port_attr));
    ret = ibv_query_port(ib_res->context, ib_res->port, &port_attr);
    if (ret) {
        perror("ibv_query_port");
        goto cleanup;
    }
    ret = ibv_query_gid(ib_res->context, ib_res->port, ib_res->gidx, &ib_res->local_info.gid);
    if (ret) {
	    perror("ibv_query_gid");
	    goto cleanup;
    }
    ib_res->local_info.lid = port_attr.lid;
    ib_res->local_info.qpn = ib_res->qp->qp_num;
    ib_res->local_info.psn = 0;
    ib_res->local_info.qkey = qp_attr.qkey;

    inet_ntop(AF_INET6, &ib_res->local_info.gid, gid, sizeof gid);
    printf("local lid: %d, qpn: %d, psn: %d, qkey: %#010x, gid %s\n", 
           ib_res->local_info.lid, ib_res->local_info.qpn, ib_res->local_info.psn,
           ib_res->local_info.qkey, gid);

    return ret;

cleanup:
    if (ib_res->qp) ibv_destroy_qp(ib_res->qp);
    if (ib_res->cq) ibv_destroy_cq(ib_res->cq);
    if (ib_res->pd) ibv_dealloc_pd(ib_res->pd);
    if (ib_res->context) ibv_close_device(ib_res->context);
    if (ib_res->dev_list) ibv_free_device_list(ib_res->dev_list);
    return ret;
}

int poll_cq(struct ibv_cq *cq, struct ibv_wc *wc) {
    int ret = 0;

    memset(wc, 0, sizeof(struct ibv_wc));
    while (1) {
        ret = ibv_poll_cq(cq, 1, wc);
        if (ret < 0) {
            perror("ibv_poll_cq");
        } else if (ret == 1) {
            if (wc->status == IBV_WC_SUCCESS) {
                if (wc->wr_id == 100) {
                    printf("Read a CQE for ibv_bind_mw\n");
		            break;
                } else if (wc->wr_id == 10 || wc->wr_id == 11) {
                    printf("Received imm %d\n", ntohl(wc->imm_data));
		            break;
                } else if (wc->wr_id == 20) {
                    printf("Pkt sent successfully\n");
                    break;
                } else {
                    printf("Unkown CQE\n");
                    break;
                }
            } else {
                fprintf(stderr, "Failed to send/receive message: %s\n", ibv_wc_status_str(wc->status));
            }
        }
    }
    return ret;
}

int bind_mw_rkey(struct ib_res *ib_res, struct ibv_mw *mw, uint8_t mw_type, struct ibv_mw_bind_info *bind_info) {
    struct ibv_send_wr swr, *sbad_wr;
    struct ibv_wc wc;
    int wrid = 100;
    int ret = 0;
    long long start_time, end_time;

    memset(&swr, 0, sizeof(swr));
    if (mw_type == IBV_MW_TYPE_2) {
    	memset(&swr, 0, sizeof(swr));
    	swr.wr_id = wrid;
    	swr.sg_list = NULL;
    	swr.num_sge = 0;
    	swr.opcode = IBV_WR_BIND_MW;
    	swr.send_flags = IBV_SEND_SIGNALED;
    	swr.bind_mw.mw = mw;
    	swr.bind_mw.bind_info.mr = bind_info->mr;
    	swr.bind_mw.bind_info.addr = (uintptr_t)(bind_info->addr);
    	swr.bind_mw.bind_info.length = bind_info->length;
    	swr.bind_mw.bind_info.mw_access_flags = bind_info->mw_access_flags;

    	ret = ibv_post_send(ib_res->qp, &swr, &sbad_wr);
    	if (ret) {
    	    perror("ibv_post_send error");
    	    goto cleanup;
    	}
    } else {
    	struct ibv_mw_bind mw_bind = {
    	        .wr_id = wrid,
    	        .send_flags = IBV_SEND_SIGNALED,
    		    .bind_info.mr = bind_info->mr,
    		    .bind_info.addr = (uintptr_t)(bind_info->addr),
    		    .bind_info.length = bind_info->length,
    		    .bind_info.mw_access_flags = bind_info->mw_access_flags
    	};
        start_time = gfp_get_time();
    	ret = ibv_bind_mw(ib_res->qp, mw, &mw_bind);
    	if (ret) {
    	    perror("ibv_bind_mw");
    	    goto cleanup;
    	}
    }
    ret = poll_cq(ib_res->cq, &wc);
    if (ret < 0) {
        perror("poll cq failed");
        goto cleanup;
    }
    end_time = gfp_get_time();
    printf("ibv_bind_mw + poll it's cq takes %lld nanosec\n", end_time - start_time);
    return 0;

cleanup:
    return ret;
}


int exchange_info_server(struct ib_info *local_info, struct ib_info *client_info, in_port_t port) {
    int sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int ret = 0;
    char gid[33];

    // Create socket for connection
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        ret = 1;
        goto cleanup;
    }
    int option = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        ret = 1;
        goto cleanup;
    }

    if (listen(sock, 1) < 0) {
        perror("listen");
        ret = 1;
        goto cleanup;
    }
    //printf("server now is listening\n");

    client_sock = accept(sock, (struct sockaddr *)&client_addr, &client_addr_len);
    if (client_sock < 0) {
        perror("accept");
        ret = 1;
        goto cleanup;
    }
    //printf("server accepted a client\n");

    // Send local QP information
    if (send(client_sock, local_info, sizeof(*local_info), 0) != sizeof(*local_info)) {
        perror("send");
        ret = 1;
        goto cleanup;
    }
    //printf("server sent out local QP info\n");

    // Receive remote QP information
    if (recv(client_sock, client_info, sizeof(*client_info), 0) != sizeof(*client_info)) {
        perror("recv");
        ret = 1;
        goto cleanup;
    }

    inet_ntop(AF_INET6, &client_info->gid, gid, sizeof gid);
    printf("Info exchange: Server received remote QP info, %d, %d, %d, %#010x, %s\n",
            client_info->lid, client_info->qpn, client_info->psn, client_info->qkey, gid);

cleanup:
    close(client_sock);
    close(sock);
    return ret;
}

int exchange_info_client(struct ib_info *local_info, char *server_ip, struct ib_info *server_info, in_port_t port) {

    int sock;
    struct sockaddr_in server_addr;
    int ret = 0;
    char gid[33];

    // Establish socket connection to exchange QP information
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        ret = 1;
        goto cleanup;
    }
    int option = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        ret = 1;
        goto cleanup;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        ret = 1;
        goto cleanup;
    }
    //printf("client connected to server\n");

    // Send local QP information
    if (send(sock, local_info, sizeof(*local_info), 0) != sizeof(*local_info)) {
        perror("send");
        ret = 1;
        goto cleanup;
    }
    //printf("client sent out local QP info\n");

    // Receive remote QP information
    if (recv(sock, server_info, sizeof(*server_info), 0) != sizeof(*server_info)) {
        perror("recv");
        ret = 1;
        goto cleanup;
    }
    inet_ntop(AF_INET6, &server_info->gid, gid, sizeof gid);
    printf("Info exchange: Client received server QP info, %d, %d, %d, %#010x, %s; mr info, rkey: %d, buf %ld\n",
        server_info->lid, server_info->qpn, server_info->psn, server_info->qkey, gid,
        server_info->buf_rkey, server_info->buf_va);
cleanup:
    close(sock);
    return ret;
}

