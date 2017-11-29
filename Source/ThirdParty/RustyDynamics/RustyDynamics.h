#pragma once

extern "C" {

    typedef struct RustVec {
        size_t vec_ptr;
        size_t vec_cap;
        size_t vec_len;
    } RustVec;

    typedef struct AvatarPack {
        uint32_t id;
        float root_px, root_py, root_pz, root_pw;
        float root_rx, root_ry, root_rz, root_rw;
        float head_px, head_py, head_pz, head_pw;
        float head_rx, head_ry, head_rz, head_rw;
        float handL_px, handL_py, handL_pz, handL_pw;
        float handL_rx, handL_ry, handL_rz, handL_rw;
        float handR_px, handR_py, handR_pz, handR_pw;
        float handR_rx, handR_ry, handR_rz, handR_rw;
        float height, floor, GripL, GripR;
	
    } AvatarPack;

    typedef struct RigidbodyPack {
        uint32_t id;
        float px, py, pz, pw;
        float lx, ly, lz, lw;
        float rx, ry, rz, rw;
        float ax, ay, az, aw;
    } RigidbodyPack;

    void rb_log_fn(void (*log_fn)(const char*));

    uint32_t rb_uuid();

    void* rd_netclient_open(const char* local_addr, const char* server_addr, const char* mumble_addr);
    void rd_netclient_drop(void* client);

    void rd_netclient_msg_push(void* client, const uint8* bytes, uint32_t count);
    RustVec* rd_netclient_msg_pop(void* client);
    void rd_netclient_msg_drop(RustVec* msg);

    void rd_netclient_push_avatar(void* client, const AvatarPack* avatar);
    AvatarPack* rd_netclient_dec_avatar(const uint8* bytes, uint32_t count);
    void rd_netclient_drop_avatar(AvatarPack* avatar);

    void rd_netclient_push_rigidbody(void* client, const RigidbodyPack* rigidbody);
    RigidbodyPack* rd_netclient_dec_rigidbody(const uint8* bytes, uint32_t count);
    void rd_netclient_drop_rigidbody(RigidbodyPack* rigidbody);

}
