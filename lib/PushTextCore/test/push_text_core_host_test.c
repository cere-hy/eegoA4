/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 文本推送裁剪纯核 host 回归。四项硬判据（BEGIN 编解码 / DATA seq 连续 /
 * END 收全判定 / CRC 不匹配拒绝）逐字节断言；坏魔数、跳号、超长、错 CRC
 * 必须红。ASan/UBSan 下编译运行，无 ESP-IDF / 无硬件。
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
    b.token = 4821;                 // 0x12D5
    b.total_len = 300000;           // 0x000493E0
    b.crc32 = 0xdeadbeefu;
    b.session_id = 0x12345678u;
    strcpy(b.title, "测试长文");

    uint8_t frame[PUSH_BEGIN_LEN];
    size_t n = push_begin_encode(&b, frame, sizeof(frame));
    CHECK(n == PUSH_BEGIN_LEN);
    CHECK(frame[0] == 0x50 && frame[1] == 0x43);
    CHECK(frame[2] == PUSH_OP_BEGIN && frame[3] == PUSH_VERSION);
    CHECK(frame[4] == PUSH_TYPE_TEXT);
    CHECK(frame[5] == 0xD5 && frame[6] == 0x12);               // token LE
    CHECK(frame[7] == 0xE0 && frame[8] == 0x93 && frame[9] == 0x04); // total_len LE
    CHECK(memcmp(frame + 19, "测试长文", strlen("测试长文") + 1) == 0);

    push_begin_t out;
    CHECK(push_begin_decode(frame, n, &out));
    CHECK(out.total_len == 300000 && out.crc32 == 0xdeadbeefu && out.session_id == 0x12345678u);
    CHECK(out.token == 4821 && out.type == PUSH_TYPE_TEXT);
    CHECK(strcmp(out.title, "测试长文") == 0);

    // 坏输入必须拒绝
    CHECK(!push_begin_decode(frame, PUSH_BEGIN_LEN - 1, &out)); // 长度不足
    uint8_t bad[PUSH_BEGIN_LEN];
    memcpy(bad, frame, sizeof(bad)); bad[0] = 0x00;
    CHECK(!push_begin_decode(bad, PUSH_BEGIN_LEN, &out));       // 坏 magic0
    memcpy(bad, frame, sizeof(bad)); bad[3] = 0x09;
    CHECK(!push_begin_decode(bad, PUSH_BEGIN_LEN, &out));       // 坏 version
    memcpy(bad, frame, sizeof(bad)); bad[2] = PUSH_OP_END;
    CHECK(!push_begin_decode(bad, PUSH_BEGIN_LEN, &out));       // 非 BEGIN op
    CHECK(!push_begin_encode(&b, frame, PUSH_BEGIN_LEN - 1));   // cap 不够
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
    CHECK(chain == one); // 流式 == 一次性
}

/* 助手：走完 ARM→ADV_UP→CONNECT，进入 CONNECTED */
static void drive_to_connected(push_sink_core_t *c) {
    push_sink_core_init(c);
    CHECK((push_sink_core_arm(c, 4821, 0u, 1000u) & PUSH_ACT_ADV_ON) != 0);
    CHECK(c->sess.state == PUSH_STATE_ARMED);
    CHECK(push_sink_core_adv_up(c) == 0);
    CHECK(c->sess.state == PUSH_STATE_WINDOW);
    CHECK(push_sink_core_connect(c) == 0);
    CHECK(c->sess.state == PUSH_STATE_CONNECTED);
}

/* 构造 BEGIN 帧并送入 core */
static uint32_t send_begin(push_sink_core_t *c, uint32_t total, uint32_t crc) {
    push_begin_t b;
    memset(&b, 0, sizeof(b));
    b.type = PUSH_TYPE_TEXT;
    b.token = 4821;
    b.total_len = total;
    b.crc32 = crc;
    strcpy(b.title, "文章标题");
    uint8_t frame[PUSH_BEGIN_LEN];
    push_begin_encode(&b, frame, sizeof(frame));
    return push_sink_core_on_ctrl(c, frame, sizeof(frame));
}

/* 构造 DATA 帧：seq(2 LE) + len(1) + payload */
static size_t make_data(uint8_t *out, uint16_t seq, const uint8_t *p, size_t n) {
    out[0] = (uint8_t)(seq & 0xFFu);
    out[1] = (uint8_t)(seq >> 8);
    out[2] = (uint8_t)n;
    if (n) memcpy(out + 3, p, n);
    return 3u + n;
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

    // seq 跳号（期望 1，给 2）必须拒
    n = make_data(f, 2, a, 3);
    uint32_t acts = push_sink_core_on_data(&c, f, n);
    CHECK((acts & PUSH_ACT_ERR) != 0);
    CHECK((acts & PUSH_ACT_APPEND) == 0);
    CHECK(c.sess.received == 3); // 跳号包没记账
    CHECK(c.sess.last_err == PUSH_ERR_BAD_FRAME);

    // 单独验证正确序号 1 在足够大 total 下能被接受（跳号拒绝不影响后续正确帧的语义）
    push_sink_core_t d;
    drive_to_connected(&d);
    CHECK((send_begin(&d, 600, 0u) & PUSH_ACT_READY) != 0);
    size_t m = make_data(f, 0, a, 3);
    CHECK((push_sink_core_on_data(&d, f, m) & PUSH_ACT_APPEND) != 0);
    m = make_data(f, 1, a, 3);
    CHECK((push_sink_core_on_data(&d, f, m) & PUSH_ACT_APPEND) != 0);
    CHECK(d.sess.received == 6 && d.sess.next_seq == 2);
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

    uint8_t end[PUSH_CTRL_HDR_LEN] = {PUSH_MAGIC0, PUSH_MAGIC1, PUSH_OP_END, PUSH_VERSION};
    uint32_t acts = push_sink_core_on_ctrl(&c, end, sizeof(end));
    CHECK((acts & PUSH_ACT_VERIFY) != 0);
    CHECK((acts & PUSH_ACT_OPEN_BOOK) != 0); // 收全通过
    CHECK(c.open_ready == true);
    CHECK(c.sess.state == PUSH_STATE_IDLE);

    char title[PUSH_TITLE_MAX];
    uint8_t type = 0;
    CHECK(push_sink_core_take_open(&c, title, sizeof(title), &type));
    CHECK(strcmp(title, "文章标题") == 0);
    CHECK(type == PUSH_TYPE_TEXT);
    CHECK(!push_sink_core_take_open(&c, title, sizeof(title), &type)); // 一次性

    uint8_t st[16];
    size_t sl = push_sink_core_status(&c, st, sizeof(st));
    CHECK(sl == 1 && st[0] == PUSH_ST_DONE);
}

/* ---- ④ CRC 不匹配拒绝 ---- */
static void test_crc_reject(void) {
    push_sink_core_t c;
    drive_to_connected(&c);
    const char *body = "hello push";
    uint32_t wrong_crc = push_crc32(body, strlen(body)) ^ 0x1u; // 故意错
    CHECK((send_begin(&c, (uint32_t)strlen(body), wrong_crc) & PUSH_ACT_READY) != 0);

    uint8_t f[64];
    size_t n = make_data(f, 0, (const uint8_t *)body, strlen(body));
    CHECK((push_sink_core_on_data(&c, f, n) & PUSH_ACT_APPEND) != 0);

    uint8_t end[PUSH_CTRL_HDR_LEN] = {PUSH_MAGIC0, PUSH_MAGIC1, PUSH_OP_END, PUSH_VERSION};
    uint32_t acts = push_sink_core_on_ctrl(&c, end, sizeof(end));
    CHECK((acts & PUSH_ACT_VERIFY) != 0);
    CHECK((acts & PUSH_ACT_OPEN_BOOK) == 0); // 不得开书
    CHECK((acts & PUSH_ACT_ERR) != 0);
    CHECK((acts & PUSH_ACT_DROP) != 0);
    CHECK(c.open_ready == false);
    CHECK(c.sess.last_err == PUSH_ERR_CHECKSUM);
    CHECK(c.sess.state == PUSH_STATE_CONNECTED); // 可重推

    uint8_t st[16];
    size_t sl = push_sink_core_status(&c, st, sizeof(st));
    CHECK(sl == 2 && st[0] == PUSH_ST_ERR && st[1] == PUSH_ERR_CHECKSUM);
}

/* ---- 长度不符也拒（收全判据的另一半） ---- */
static void test_length_reject(void) {
    push_sink_core_t c;
    drive_to_connected(&c);
    const char *body = "short";
    uint32_t crc = push_crc32(body, strlen(body));
    CHECK((send_begin(&c, 100, crc) & PUSH_ACT_READY) != 0); // 声明 100，实收 5

    uint8_t f[64];
    size_t n = make_data(f, 0, (const uint8_t *)body, strlen(body));
    push_sink_core_on_data(&c, f, n);

    uint8_t end[PUSH_CTRL_HDR_LEN] = {PUSH_MAGIC0, PUSH_MAGIC1, PUSH_OP_END, PUSH_VERSION};
    uint32_t acts = push_sink_core_on_ctrl(&c, end, sizeof(end));
    CHECK((acts & PUSH_ACT_OPEN_BOOK) == 0);
    CHECK(c.sess.last_err == PUSH_ERR_CHECKSUM); // 长度不符归入收全失败
}

/* ---- BEGIN 会话级校验：token 不符 / 超长 ---- */
static void test_begin_guard(void) {
    push_sink_core_t c;
    drive_to_connected(&c);
    // token 不符
    push_begin_t b;
    memset(&b, 0, sizeof(b));
    b.type = PUSH_TYPE_TEXT; b.token = 9999; b.total_len = 10; b.crc32 = 0;
    uint8_t frame[PUSH_BEGIN_LEN];
    push_begin_encode(&b, frame, sizeof(frame));
    uint32_t acts = push_sink_core_on_ctrl(&c, frame, sizeof(frame));
    CHECK((acts & PUSH_ACT_ERR) != 0);
    CHECK(c.sess.last_err == PUSH_ERR_BAD_FRAME);
    // 超长
    b.token = 4821; b.total_len = PUSH_TOTAL_MAX + 1u;
    push_begin_encode(&b, frame, sizeof(frame));
    acts = push_sink_core_on_ctrl(&c, frame, sizeof(frame));
    CHECK((acts & PUSH_ACT_ERR) != 0);
    CHECK(c.sess.last_err == PUSH_ERR_TOO_LARGE);
}

int main(void) {
    test_begin_codec();
    test_crc32();
    test_data_seq();
    test_end_accept();
    test_crc_reject();
    test_length_reject();
    test_begin_guard();
    printf("checks=%d red=%d\n", s_checks, s_red);
    return s_red == 0 ? 0 : 1;
}
