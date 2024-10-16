#include "gfp.h"


int main() {
    struct ib_res ib_res;
    char *buffer = NULL;
    char *prebuffer = NULL;
    struct ibv_mr *mr = NULL;
    struct ibv_mr *premr = NULL;
    struct ib_info client_info;
    struct ibv_qp_attr qp_attr;
    struct ibv_sge sg;
    struct ibv_recv_wr rwr, *rbad_wr;
    struct ibv_wc wc;
    struct ibv_mw *mw = NULL;
    int pagesize = getpagesize();
    int ret;
    long long start_time, end_time;

    memset(&client_info, 0, sizeof(struct ib_info));
    memset(&ib_res, 0, sizeof(struct ib_res));

    buffer = memalign(pagesize, PKTSZ);
    if (!buffer) {
        perror("memalign");
        goto cleanup;
    }
    memset(buffer, 0, PKTSZ);
    prebuffer = memalign(pagesize, PKTSZ);
    if (!prebuffer) {
        perror("memalign");
        goto cleanup;
    }
    memset(prebuffer, 0, PKTSZ);

    ret = prepare_ib_res(&ib_res);
    if (ret) {
        perror("prepare_ib_res failed");
        goto cleanup;
    }

    mr = ibv_reg_mr(ib_res.pd, buffer, PKTSZ, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_MW_BIND);
    if (!mr) {
        perror("ibv_reg_mr");
        ret = -1;
        goto cleanup;
    }
    premr = ibv_reg_mr(ib_res.pd, prebuffer, PKTSZ, IBV_ACCESS_LOCAL_WRITE);
    if (!premr) {
        perror("ibv_reg_mr");
        ret = -1;
        goto cleanup;
    }
    // Server and client exchange info
    ret = exchange_info_server(&ib_res.local_info, &client_info, 28515);
    if (ret) {
        perror("server exchange info failed\n");
        goto cleanup;
    }

    // Modify QP to RTR
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTR;
    qp_attr.path_mtu = IBV_MTU_4096;
    /* require peer info: qpn, psn, lid */ 
    qp_attr.dest_qp_num	= client_info.qpn;
    qp_attr.rq_psn = client_info.psn;
    qp_attr.ah_attr.dlid = client_info.lid;
    qp_attr.ah_attr.is_global = 0;
    qp_attr.ah_attr.src_path_bits = 0;
    qp_attr.ah_attr.port_num = 1;

    if (ib_res.gidx > 0) {
        qp_attr.ah_attr.is_global = 1;
        qp_attr.ah_attr.grh.hop_limit = 1;
        qp_attr.ah_attr.grh.dgid = client_info.gid;
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
        perror("ibv_modify_qp");
        goto cleanup;
    }

    // Create a memory window (MW) of type 1 or 2
    uint8_t mw_type = IBV_MW_TYPE_1;
    mw = ibv_alloc_mw(ib_res.pd, mw_type);
    if (!mw) {
        perror("Couldn't allocate memory window");
        goto cleanup;
    }
    struct ibv_mw_bind_info bind_info = {
    		.mr = mr,
    		.addr = (uintptr_t)buffer,
    	    .length = 4096,
    	    .mw_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
    };
    ret = bind_mw_rkey(&ib_res, mw, mw_type, &bind_info);
    if (ret) {
        perror("bind_mw and get rkey failed");
        goto cleanup;
    }
    
    // Pre-post receive buffers
    memset(&sg, 0, sizeof(sg));
    memset(&rwr, 0, sizeof(rwr));

    for (int i = 10; i < 12; i++) {
        sg.addr = (uintptr_t)prebuffer;
        sg.length = PKTSZ;
        sg.lkey = premr->lkey;
        rwr.wr_id = i;
        rwr.sg_list = &sg;
        rwr.num_sge = 1;

        ret = ibv_post_recv(ib_res.qp, &rwr, &rbad_wr);
        if (ret) {
            perror("ibv_post_recv");
            goto cleanup;
        }
    }

    // Server/client exchange MW rkey and buffer addr
    ib_res.local_info.buf_rkey = mw->rkey;
    ib_res.local_info.buf_va = (uintptr_t)buffer;
    printf("mr's rkey %d, mw's rkey %d\n", mr->rkey, mw->rkey);

    ret = exchange_info_server(&ib_res.local_info, &client_info, 28517);
    if (ret) {
        perror("server exchange info failed\n");
        goto cleanup;
    }

    // Poll RDMA Write with Immediate message
    ret = poll_cq(ib_res.cq, &wc);
    if (ret < 0) {
        perror("poll cq failed\n");
        goto cleanup;
    }
    printf("buffer: %s\n", buffer);

#if 0
    // Invalidate MW type 1 with 0 length
    bind_info.mr = mr;
    bind_info.addr = (uintptr_t)buffer;
    bind_info.length = 0;
    bind_info.mw_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;

    ret = bind_mw_rkey(&ib_res, mw, mw_type, &bind_info);
    if (ret) {
        perror("bind_mw and get rkey failed");
        goto cleanup;
    }
    printf("Invalidated MW rkey\n");
#endif

    // Poll RDMA Write with Immediate message
    ret = poll_cq(ib_res.cq, &wc);
    if (ret < 0) {
        perror("poll cq failed\n");
        goto cleanup;
    }
    printf("buffer: %s\n", buffer);

cleanup:
    if (mw) ibv_dealloc_mw(mw);
    if (buffer) free(buffer);
    if (prebuffer) free(prebuffer);
    start_time = gfp_get_time();
    if (mr) ibv_dereg_mr(mr);
    end_time = gfp_get_time();
    printf("ibv_dereg_mr takes %lld nanosec\n", end_time - start_time);
    if (premr) ibv_dereg_mr(premr);
    if (ib_res.qp) ibv_destroy_qp(ib_res.qp);
    if (ib_res.cq) ibv_destroy_cq(ib_res.cq);
    if (ib_res.pd) ibv_dealloc_pd(ib_res.pd);
    if (ib_res.context) ibv_close_device(ib_res.context);
    if (ib_res.dev_list) ibv_free_device_list(ib_res.dev_list);

    return 0;
}

