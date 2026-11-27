/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 文本推送纯核 host 回归（B6.5 S1 完整能力版）。判据：
 *   基础：BEGIN 编解码 / CRC32 向量 / DATA seq 连续 / END 收全 / CRC 不符拒绝 / 长度拒绝 / BEGIN 守卫
 *   修正新增：五种 type 分流（URL→ERR FETCH、未知→ERR BAD_FRAME、WORD 按文本）/
 *             断点续传（RESUME_QUERY 空档 offset0、adopt 恢复、同 session BEGIN 续传、
 *             resume 续收 + 外部 CRC 收全、失败保留 .part）/
 *             512KB 边界 / cooldown 到期 / COMMIT 事件。
 * ASan/UBSan 下编译运行，无 ESP-IDF / 无硬件。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "push_proto.h"
#include "push_session.h"
#include "push_sink_core.h"

static int s_checks;
static int s_red;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++s_checks;                                                      \
        if (!(cond)) {                                                   \
            ++s_red;                                                     \
            printf("RED line %d: %s\n", __LINE__, #cond);                \
        }                                                                \
    } while (0)

/* ---- ① BEGIN 帧编解码 ---- */
static void test_begin_codec(void) {
    push_begin_t b;
    memset(&b, 0, sizeof(b));
    b.type = PUSH_TYPE_TEXT;
    b.token = 4821;
    b.total_len = 300000;
    b.crc32 = 0xdeadbeefu;
    b.session_id = 0x12345678u;
    strcpy(b.title, "测试长文");

    uint8_t frame[PUSH_BEGIN_LEN];
    size_t n = push_begin_encode(&b, frame, sizeof(frame));
    CHECK(n == PUSH_BEGIN_LEN);
    CHECK(frame[0] == 0x50 && frame[1] == 0x43);
    CHECK(frame[2] == PUSH_OP_BEGIN && frame[3] == PUSH_VERSION);
    CHECK(frame[4] == PUSH_TYPE_TEXT);
    CHECK(frame[5] == 0xD5 && frame[6] == 0x12);
    CHECK(frame[7] == 0xE0 && frame[8] == 0x93 && frame[9] == 0x04);
    CHECK(memcmp(frame + 19, "测试长文", strlen("测试长文") + 1) == 0);

    push_begin_t out;
    CHECK(push_begin_decode(frame, n, &out));
    CHECK(out.total_len == 300000 && out.crc32 == 0xdeadbeefu && out.session_id == 0x12345678u);
    CHECK(out.token == 4821 && out.type == PUSH_TYPE_TEXT);
    CHECK(strcmp(out.title, "测试长文") == 0);

    CHECK(!push_begin_decode(frame, PUSH_BEGIN_LEN - 1, &out));
    uint8_t bad[PUSH_BEGIN_LEN];
    memcpy(bad, frame, sizeof(bad)); bad[0] = 0x00;
    CHECK(!push_begin_decode(bad, PUSH_BEGIN_LEN, &out));
    memcpy(bad, frame, sizeof(bad)); bad[3] = 0x09;
    CHECK(!push_begin_decode(bad, PUSH_BEGIN_LEN, &out));
    memcpy(bad, frame, sizeof(bad)); bad[2] = PUSH_OP_END;
    CHECK(!push_begin_decode(bad, PUSH_BEGIN_LEN, &out));
    CHECK(!push_begin_encode(&b, frame, PUSH_BEGIN_LEN - 1));
}

/* ---- CRC32 已知向量 + 流式等价 ---- */
static void test_crc32(void) {
    CHECK(push_crc32("123456789", 9) == 0xCBF43926u);
    CHECK(push_crc32("", 0) == 0u);
    CHECK(push_crc32("a", 1) != push_crc32("b", 1));
    const char *whole = "the quick brown fox jumps over the lazy dog";
    size_t wl = strlen(whole);
    uint32_t one = push_crc32(whole, wl);
    uint32_t chain = push_crc32_continue(0u, whole, 10);
    chain = push_crc32_continue(chain, whole + 10, wl - 10);
    CHECK(chain == one);
}

/* 助手：走完 ARM→ADV_UP→CONNECT，进入 CONNECTED（带 cooldown=0） */
static void drive_to_connected(push_sink_core_t *c) {
    push_sink_core_init(c);
    CHECK((push_sink_core_arm(c, 4821, 0u, 0u, 1000u) & PUSH_ACT_ADV_ON) != 0);
    CHECK(c->sess.state == PUSH_STATE_ARMED);
    CHECK(push_sink_core_adv_up(c) == 0);
    CHECK(c->sess.state == PUSH_STATE_WINDOW);
    CHECK(push_sink_core_connect(c) == 0);
    CHECK(c->sess.state == PUSH_STATE_CONNECTED);
}

/* 构造 BEGIN 帧并送入 core（可指定 type/session_id） */
static uint32_t send_begin_ex(push_sink_core_t *c, uint32_t total, uint32_t crc,
                              uint8_t type, uint32_t sid) {
    push_begin_t b;
    memset(&b, 0, sizeof(b));
    b.type = type;
    b.token = 4821;
    b.total_len = total;
    b.crc32 = crc;
    b.session_id = sid;
    strcpy(b.title, "文章标题");
    uint8_t frame[PUSH_BEGIN_LEN];
    push_begin_encode(&b, frame, sizeof(frame));
    return push_sink_core_on_ctrl(c, frame, sizeof(frame));
}
static uint32_t send_begin(push_sink_core_t *c, uint32_t total, uint32_t crc) {
    return send_begin_ex(c, total, crc, PUSH_TYPE_TEXT, 0xAAu);
}

static size_t make_data(uint8_t *out, uint16_t seq, const uint8_t *p, size_t n) {
    out[0] = (uint8_t)(seq & 0xFFu);
    out[1] = (uint8_t)(seq >> 8);
    out[2] = (uint8_t)n;
    if (n) memcpy(out + 3, p, n);
    return 3u + n;
}
static uint32_t send_end(push_sink_core_t *c) {
    uint8_t end[PUSH_CTRL_HDR_LEN] = {PUSH_MAGIC0, PUSH_MAGIC1, PUSH_OP_END, PUSH_VERSION};
    return push_sink_core_on_ctrl(c, end, sizeof(end));
}

/* ---- ② DATA seq 连续性 ---- */
static void test_data_seq(void) {
    push_sink_core_t c;
    drive_to_connected(&c);
    CHECK((send_begin(&c, 3, 0u) & PUSH_ACT_READY) != 0);
    CHECK(c.sess.state == PUSH_STATE_RECEIVING);

    uint8_t f[64];
    const uint8_t a[] = {'a','b','c'};
    size_t n = make_data(f, 0, a, 3);
    CHECK((push_sink_core_on_data(&c, f, n) & PUSH_ACT_APPEND) != 0);
    CHECK(c.sess.received == 3 && c.sess.next_seq == 1);

    n = make_data(f, 2, a, 3);
    uint32_t acts = push_sink_core_on_data(&c, f, n);
    CHECK((acts & PUSH_ACT_ERR) != 0);
    CHECK((acts & PUSH_ACT_APPEND) == 0);
    CHECK(c.sess.received == 3);
    CHECK(c.sess.last_err == PUSH_ERR_BAD_FRAME);
}

/* ---- ③ END 触发收全判定（CRC + 长度都符） ---- */
static void test_end_accept(void) {
    push_sink_core_t c;
    drive_to_connected(&c);
    const char *body = "hello push";
    uint32_t crc = push_crc32(body, strlen(body));
    CHECK((send_begin(&c, (uint32_t)strlen(body), crc) & PUSH_ACT_READY) != 0);

    uint8_t f[64];
    size_t n = make_data(f, 0, (const uint8_t *)body, strlen(body));
    CHECK((push_sink_core_on_data(&c, f, n) & PUSH_ACT_APPEND) != 0);
    CHECK(c.sess.received == strlen(body));

    uint32_t acts = send_end(&c);
    CHECK((acts & PUSH_ACT_VERIFY) != 0);
    CHECK((acts & PUSH_ACT_OPEN_BOOK) != 0);
    CHECK((acts & PUSH_ACT_COMMIT) != 0);
    CHECK(c.open_ready == true && c.commit_ready == true);
    CHECK(c.sess.state == PUSH_STATE_COOLDOWN); // 成功进宽限

    char title[PUSH_TITLE_MAX];
    uint8_t type = 0;
    CHECK(push_sink_core_take_open(&c, title, sizeof(title), &type));
    CHECK(strcmp(title, "文章标题") == 0);
    CHECK(type == PUSH_TYPE_TEXT);
    CHECK(!push_sink_core_take_open(&c, title, sizeof(title), &type));
    CHECK(push_sink_core_take_commit(&c) == true);
    CHECK(push_sink_core_take_commit(&c) == false); // 一次性

    uint8_t st[16];
    size_t sl = push_sink_core_status(&c, st, sizeof(st));
    // 000-39b1: DONE(0x12) now carries a 1B PUSH_RES_* result code (OK=0).
    CHECK(sl == 2 && st[0] == PUSH_ST_DONE && st[1] == PUSH_RES_OK);
}

/* ---- ④ CRC 不匹配拒绝（保留 .part，不 DROP） ---- */
static void test_crc_reject(void) {
    push_sink_core_t c;
    drive_to_connected(&c);
    const char *body = "hello push";
    uint32_t wrong_crc = push_crc32(body, strlen(body)) ^ 0x1u;
    CHECK((send_begin(&c, (uint32_t)strlen(body), wrong_crc) & PUSH_ACT_READY) != 0);

    uint8_t f[64];
    size_t n = make_data(f, 0, (const uint8_t *)body, strlen(body));
    CHECK((push_sink_core_on_data(&c, f, n) & PUSH_ACT_APPEND) != 0);

    uint32_t acts = send_end(&c);
    CHECK((acts & PUSH_ACT_VERIFY) != 0);
    CHECK((acts & PUSH_ACT_OPEN_BOOK) == 0);
    CHECK((acts & PUSH_ACT_ERR) != 0);
    CHECK((acts & PUSH_ACT_DROP) == 0); // 失败保留 .part 供续传
    CHECK(c.sess.has_part == true);
    CHECK(c.open_ready == false && c.commit_ready == false);
    CHECK(c.sess.last_err == PUSH_ERR_CHECKSUM);
    CHECK(c.sess.state == PUSH_STATE_CONNECTED); // 可补包重发 END

    uint8_t st[16];
    size_t sl = push_sink_core_status(&c, st, sizeof(st));
    CHECK(sl == 2 && st[0] == PUSH_ST_ERR && st[1] == PUSH_ERR_CHECKSUM);
}

/* ---- 长度不符也拒 ---- */
static void test_length_reject(void) {
    push_sink_core_t c;
    drive_to_connected(&c);
    const char *body = "short";
    uint32_t crc = push_crc32(body, strlen(body));
    CHECK((send_begin(&c, 100, crc) & PUSH_ACT_READY) != 0);
    uint8_t f[64];
    size_t n = make_data(f, 0, (const uint8_t *)body, strlen(body));
    push_sink_core_on_data(&c, f, n);
    uint32_t acts = send_end(&c);
    CHECK((acts & PUSH_ACT_OPEN_BOOK) == 0);
    CHECK(c.sess.last_err == PUSH_ERR_CHECKSUM);
}

/* ---- BEGIN 会话级守卫 ---- */
static void test_begin_guard(void) {
    push_sink_core_t c;
    drive_to_connected(&c);
    push_begin_t b;
    memset(&b, 0, sizeof(b));
    b.type = PUSH_TYPE_TEXT; b.token = 9999; b.total_len = 10; b.crc32 = 0;
    uint8_t frame[PUSH_BEGIN_LEN];
    push_begin_encode(&b, frame, sizeof(frame));
    uint32_t acts = push_sink_core_on_ctrl(&c, frame, sizeof(frame));
    CHECK((acts & PUSH_ACT_ERR) != 0);
    CHECK(c.sess.last_err == PUSH_ERR_BAD_FRAME);
    b.token = 4821; b.total_len = PUSH_TOTAL_MAX + 1u;
    push_begin_encode(&b, frame, sizeof(frame));
    acts = push_sink_core_on_ctrl(&c, frame, sizeof(frame));
    CHECK((acts & PUSH_ACT_ERR) != 0);
    CHECK(c.sess.last_err == PUSH_ERR_TOO_LARGE);
}

/* ---- ⑤ 五种 type 分流 ---- */
static void test_type_dispatch(void) {
    // TEXT/MARKDOWN/HTML/WORD 都按文本收，落库 type 正确带出
    const uint8_t types[] = {PUSH_TYPE_TEXT, PUSH_TYPE_MARKDOWN, PUSH_TYPE_HTML, PUSH_TYPE_WORD};
    for (unsigned i = 0; i < sizeof(types); ++i) {
        push_sink_core_t c;
        drive_to_connected(&c);
        const char *body = "abc";
        uint32_t crc = push_crc32(body, 3);
        CHECK((send_begin_ex(&c, 3, crc, types[i], 0xB0u + i) & PUSH_ACT_READY) != 0);
        uint8_t f[16];
        size_t n = make_data(f, 0, (const uint8_t *)body, 3);
        push_sink_core_on_data(&c, f, n);
        uint32_t acts = send_end(&c);
        CHECK((acts & PUSH_ACT_OPEN_BOOK) != 0);
        char title[PUSH_TITLE_MAX]; uint8_t type = 0;
        CHECK(push_sink_core_take_open(&c, title, sizeof(title), &type));
        CHECK(type == types[i]); // 落库 type 与 BEGIN 一致
    }
    // URL(3)：协议认，业务回 ERR(FETCH)
    {
        push_sink_core_t c;
        drive_to_connected(&c);
        uint32_t acts = send_begin_ex(&c, 10, 0, PUSH_TYPE_URL, 0xC0u);
        CHECK((acts & PUSH_ACT_ERR) != 0);
        CHECK((acts & PUSH_ACT_READY) == 0);
        CHECK(c.sess.last_err == PUSH_ERR_FETCH);
        uint8_t st[16];
        size_t sl = push_sink_core_status(&c, st, sizeof(st));
        CHECK(sl == 2 && st[0] == PUSH_ST_ERR && st[1] == PUSH_ERR_FETCH);
    }
    // 未知 type(5)：明确 ERR(BAD_FRAME)，不当未知帧丢弃
    {
        push_sink_core_t c;
        drive_to_connected(&c);
        uint32_t acts = send_begin_ex(&c, 10, 0, 5, 0xD0u);
        CHECK((acts & PUSH_ACT_ERR) != 0);
        CHECK(c.sess.last_err == PUSH_ERR_BAD_FRAME);
    }
}

/* ---- ⑥ 断点续传 ---- */
static void test_resume(void) {
    // (a) CONNECTED 下 RESUME_QUERY 无 .part → 回 RESUME offset=0，状态不动
    {
        push_sink_core_t c;
        drive_to_connected(&c);
        uint8_t rq[PUSH_CTRL_HDR_LEN] = {PUSH_MAGIC0, PUSH_MAGIC1, PUSH_OP_RESUME_QUERY, PUSH_VERSION};
        uint32_t acts = push_sink_core_on_ctrl(&c, rq, sizeof(rq));
        CHECK((acts & PUSH_ACT_RESUME) != 0);
        CHECK(c.sess.state == PUSH_STATE_CONNECTED); // 无 part 不进 RECEIVING
        CHECK(c.resume_offset == 0 && c.resume_next_seq == 0);
        uint8_t st[16];
        size_t sl = push_sink_core_status(&c, st, sizeof(st));
        CHECK(sl == 1 + PUSH_ST_RESUME_BODY && st[0] == PUSH_ST_RESUME);
    }
    // (b) adopt 预装半成品 → ARM/ADV/CONNECT → RESUME_QUERY 带 offset 并进 RECEIVING
    {
        push_sink_core_t c;
        push_sink_core_init(&c);
        push_begin_t b;
        memset(&b, 0, sizeof(b));
        b.type = PUSH_TYPE_TEXT; b.token = 4821; b.total_len = 100; b.crc32 = 0x1111u;
        b.session_id = 0x777u; strcpy(b.title, "续传文");
        push_sink_core_adopt(&c, &b, 40, 4); // 已收 40 字节、下一包 seq=4
        CHECK(c.sess.has_part == true && c.sess.state == PUSH_STATE_IDLE);
        CHECK((push_sink_core_arm(&c, 4821, 0u, 0u, 1000u) & PUSH_ACT_ADV_ON) != 0);
        push_sink_core_adv_up(&c);
        push_sink_core_connect(&c);
        uint8_t rq[PUSH_CTRL_HDR_LEN] = {PUSH_MAGIC0, PUSH_MAGIC1, PUSH_OP_RESUME_QUERY, PUSH_VERSION};
        uint32_t acts = push_sink_core_on_ctrl(&c, rq, sizeof(rq));
        CHECK((acts & PUSH_ACT_RESUME) != 0);
        CHECK(c.sess.state == PUSH_STATE_RECEIVING);
        CHECK(c.resume_offset == 40 && c.resume_next_seq == 4);
    }
    // (c) 同 session_id 重发 BEGIN → RESUME 不重置计数（收全用真值走 test_resume_verify_external_crc）
    {
        push_sink_core_t c;
        drive_to_connected(&c);
        uint32_t crc = push_crc32("abcdef", 6);
        CHECK((send_begin_ex(&c, 6, crc, PUSH_TYPE_TEXT, 0x999u) & PUSH_ACT_READY) != 0);
        uint8_t f[16];
        const uint8_t ab[] = {'a','b'};
        push_sink_core_on_data(&c, f, make_data(f, 0, ab, 2));
        // 模拟断连 + 恢复簿记（received=2, next_seq=1）
        push_sink_core_disconnect(&c);       // → WINDOW
        push_sink_core_connect(&c);          // → CONNECTED
        uint32_t acts = send_begin_ex(&c, 6, crc, PUSH_TYPE_TEXT, 0x999u);
        CHECK((acts & PUSH_ACT_RESUME) != 0);
        CHECK((acts & PUSH_ACT_READY) == 0);
        CHECK(c.sess.received == 2 && c.sess.next_seq == 1); // 计数未重置
        CHECK(c.sess.state == PUSH_STATE_RECEIVING);
        // 续收剩余 4 字节（seq=1）
        const uint8_t cdef[] = {'c','d','e','f'};
        push_sink_core_on_data(&c, f, make_data(f, 1, cdef, 4));
        CHECK(c.sess.received == 6 && c.sess.next_seq == 2);
    }
}

/* ---- (c2) resume() 直入 RECEIVING + 外部 CRC 收全（真值 CRC） ---- */
static void test_resume_verify_external_crc(void) {
    push_sink_core_t c;
    push_sink_core_init(&c);
    const char *body = "abcdefgh";
    const uint32_t crc = push_crc32(body, 8);
    push_begin_t b;
    memset(&b, 0, sizeof(b));
    b.type = PUSH_TYPE_MARKDOWN; b.token = 4821; b.total_len = 8; b.crc32 = crc;
    b.session_id = 0x888u; strcpy(b.title, "外部CRC");
    // 模拟：已收前 4 字节后重启，设备从 NVS/.part 恢复
    push_sink_core_resume(&c, &b, 4, 2);
    CHECK(c.sess.state == PUSH_STATE_RECEIVING);
    CHECK(c.sess.received == 4 && c.sess.next_seq == 2);
    // 续收后 4 字节
    uint8_t f[16];
    push_sink_core_on_data(&c, f, make_data(f, 2, (const uint8_t *)"efgh", 4));
    CHECK(c.sess.received == 8);
    // 上层回读 .part（"abcdefgh"）算 CRC，比对通过 → 覆盖外部结论
    push_sink_core_set_verified_crc(&c, true);
    uint32_t acts = send_end(&c);
    CHECK((acts & PUSH_ACT_OPEN_BOOK) != 0);
    CHECK((acts & PUSH_ACT_COMMIT) != 0);
    char title[PUSH_TITLE_MAX]; uint8_t type = 0;
    CHECK(push_sink_core_take_open(&c, title, sizeof(title), &type));
    CHECK(type == PUSH_TYPE_MARKDOWN); // 续传也落库正确 type
}

/* ---- (c3) 外部 CRC 不符 → 拒、保留 .part ---- */
static void test_resume_bad_external_crc(void) {
    push_sink_core_t c;
    push_sink_core_init(&c);
    push_begin_t b;
    memset(&b, 0, sizeof(b));
    b.type = PUSH_TYPE_TEXT; b.token = 4821; b.total_len = 4; b.crc32 = 0x3155u;
    push_sink_core_resume(&c, &b, 4, 2); // 已满
    push_sink_core_set_verified_crc(&c, false);
    uint32_t acts = send_end(&c);
    CHECK((acts & PUSH_ACT_OPEN_BOOK) == 0);
    CHECK((acts & PUSH_ACT_DROP) == 0);
    CHECK(c.sess.has_part == true);
    CHECK(c.sess.last_err == PUSH_ERR_CHECKSUM);
}

/* ---- ⑦ 512KB 边界 ---- */
static void test_512k_boundary(void) {
    push_sink_core_t c;
    drive_to_connected(&c);
    CHECK((send_begin(&c, PUSH_TOTAL_MAX, 0) & PUSH_ACT_READY) != 0); // 恰上限接受
    CHECK(c.sess.begin.total_len == PUSH_TOTAL_MAX);
    drive_to_connected(&c);
    uint32_t acts = send_begin(&c, PUSH_TOTAL_MAX + 1u, 0);
    CHECK((acts & PUSH_ACT_ERR) != 0);
    CHECK(c.sess.last_err == PUSH_ERR_TOO_LARGE);
}

/* ---- ⑧ cooldown 到期 → 收摊 ---- */
static void test_cooldown(void) {
    push_sink_core_t c;
    push_sink_core_init(&c);
    // 纯核无时钟：VERIFY 的 now_ms 默认 0，故整条测试时间轴从 0 起，
    // arm 也注入 0。真实固件由 S2 全程注入同一 millis 时钟。
    CHECK((push_sink_core_arm(&c, 4821, 5000u, 1000u, 0u) & PUSH_ACT_ADV_ON) != 0);
    push_sink_core_adv_up(&c);
    push_sink_core_connect(&c);
    const char *body = "x";
    CHECK((send_begin(&c, 1, push_crc32(body, 1)) & PUSH_ACT_READY) != 0);
    uint8_t f[16];
    push_sink_core_on_data(&c, f, make_data(f, 0, (const uint8_t *)body, 1));
    uint32_t acts = send_end(&c);
    CHECK((acts & PUSH_ACT_OPEN_BOOK) != 0);
    CHECK(c.sess.state == PUSH_STATE_COOLDOWN);
    // 宽限(1000ms)未到：仍在 COOLDOWN
    push_sink_core_tick(&c, 500u);
    CHECK(c.sess.state == PUSH_STATE_COOLDOWN);
    // 宽限到但窗口(5000ms)未到：回 WINDOW
    push_sink_core_tick(&c, 1500u);
    CHECK(c.sess.state == PUSH_STATE_WINDOW);
    // 窗口也到：收摊 IDLE
    push_sink_core_tick(&c, 6100u);
    CHECK(c.sess.state == PUSH_STATE_IDLE);
}

int main(void) {
    test_begin_codec();
    test_crc32();
    test_data_seq();
    test_end_accept();
    test_crc_reject();
    test_length_reject();
    test_begin_guard();
    test_type_dispatch();
    test_resume();
    test_resume_verify_external_crc();
    test_resume_bad_external_crc();
    test_512k_boundary();
    test_cooldown();
    printf("checks=%d red=%d\n", s_checks, s_red);
    return s_red == 0 ? 0 : 1;
}
