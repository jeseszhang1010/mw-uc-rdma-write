#include "gfp.h"


int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        return -1;
    }

    char *server_ip = argv[1];
    struct ib_res ib_res;
    struct ib_info server_info;
    struct ibv_qp_attr qp_attr;
    struct ibv_mr *mr = NULL;
    struct ibv_sge sg;
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_wc wc;
    char *buffer = NULL;
    int pagesize = getpagesize();
    int ret;

    memset(&server_info, 0, sizeof(struct ib_info));
    memset(&ib_res, 0, sizeof(struct ib_res));

    buffer = (char *)memalign(pagesize, PKTSZ);
    if (!buffer) {
        perror("memalign");
        goto cleanup;
    }
    memset(buffer, 0, PKTSZ);
    usleep(2);
    memcpy(buffer, "Hello, this is UC infiniband with IBV_WR_RDMA_WRITE_WITH_IMM!", 100);
    //goto cleanup;

    ret = prepare_ib_res(&ib_res);
    if (ret) {
        perror("prepare_ib_res failed");
        goto cleanup;
    }
    mr = ibv_reg_mr(ib_res.pd, buffer, PKTSZ, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) {
        perror("ibv_reg_mr");
        goto cleanup;
    }
    // Server and client exchange info
    ret = exchange_info_client(&ib_res.local_info, server_ip, &server_info, 28515);
    if (ret) {
        perror("client exchange info failed\n");
        goto cleanup;
    }

    // Modify QP to RTR
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTR;
    qp_attr.path_mtu = IBV_MTU_4096;
    /* require peer info: qpn, psn, lid */
    qp_attr.dest_qp_num	= server_info.qpn;
    qp_attr.rq_psn = server_info.psn;
    qp_attr.ah_attr.dlid = server_info.lid;
    qp_attr.ah_attr.sl = 0;
    qp_attr.ah_attr.src_path_bits = 0;
    qp_attr.ah_attr.port_num = 1;

    if (ib_res.gidx > 0) {
        qp_attr.ah_attr.is_global = 1;
        qp_attr.ah_attr.grh.hop_limit = 1;
        qp_attr.ah_attr.grh.dgid = server_info.gid;
        qp_attr.ah_attr.grh.sgid_index = ib_res.gidx;
    }

    ret = ibv_modify_qp(ib_res.qp, &qp_attr, IBV_QP_STATE |
			                                 IBV_QP_AV    |
			                                 IBV_QP_PATH_MTU |
			                                 IBV_QP_DEST_QPN |
			                                 IBV_QP_RQ_PSN);
    if (ret) {
        perror("ibv_modify_qp to RTR\n");
        goto cleanup;
    }

    // Modify QP to RTS
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = ib_res.local_info.psn;
    ret = ibv_modify_qp(ib_res.qp, &qp_attr, IBV_QP_STATE | IBV_QP_SQ_PSN);
    if (ret) {
        perror("ibv_modify_qp to RTS");
        goto cleanup;
    }

    sleep(2);
    // Server and client exchange info FOR MW rkey and buffer addr
    ret = exchange_info_client(&ib_res.local_info, server_ip, &server_info, 28517);
    if (ret) {
        perror("client exchange info failed\n");
        goto cleanup;
    }

    memset(&sg, 0, sizeof(sg));
    memset(&wr, 0, sizeof(wr));

    sg.addr = (uintptr_t)buffer;
    sg.length = 1024;
    sg.lkey = mr->lkey;

    wr.wr_id = 20;
    wr.sg_list = &sg;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.imm_data = htonl(20241012);
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = server_info.buf_va;
    wr.wr.rdma.rkey = server_info.buf_rkey;

    // UD
    //wr.wr.ud.ah = ah;
    //wr.wr.ud.remote_qpn = server_info.qpn;
    //wr.wr.ud.remote_qkey = server_info.qkey;

    ret = ibv_post_send(ib_res.qp, &wr, &bad_wr);
    if (ret) {
        perror("ibv_post_send");
        goto cleanup;
    }

    // Poll for completion
    ret = poll_cq(ib_res.cq, &wc);
    if (ret < 0) {
        perror("poll cq failed");
        goto cleanup;
    }

    // wait for server to invalidate the rkey
    sleep(2);

    // RDMA write with imm. after rkey invalidation
    memset(buffer, 0, PKTSZ);
    usleep(2);
    memcpy(buffer, "After invalidating rkey", 64);
    memset(&sg, 0, sizeof(sg));
    memset(&wr, 0, sizeof(wr));

    sg.addr = (uintptr_t)buffer;
    sg.length = 1024;
    sg.lkey = mr->lkey;

    wr.wr_id = 20;
    wr.sg_list = &sg;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.imm_data = htonl(1999);
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = server_info.buf_va;
    wr.wr.rdma.rkey = server_info.buf_rkey;

    ret = ibv_post_send(ib_res.qp, &wr, &bad_wr);
    if (ret) {
        perror("ibv_post_send");
        goto cleanup;
    }

    // Poll for completion
    ret = poll_cq(ib_res.cq, &wc);
    if (ret < 0) {
        perror("poll cq failed");
        goto cleanup;
    }

    // Clean up
cleanup:
    if (buffer) free(buffer);
    if (mr) ibv_dereg_mr(mr);
    //if (ah) ibv_destroy_ah(ah);
    if (ib_res.qp) ibv_destroy_qp(ib_res.qp);
    if (ib_res.cq) ibv_destroy_cq(ib_res.cq);
    if (ib_res.pd) ibv_dealloc_pd(ib_res.pd);
    if (ib_res.context) ibv_close_device(ib_res.context);
    if (ib_res.dev_list) ibv_free_device_list(ib_res.dev_list);

    return 0;
}