/*
 * push_scene_host_test.c — 0x1A scene 编码器的纯主机侧判据（batch B4）。
 *
 * 编译（与参考固件 push_sink_host_test 同构，gcc + ASan/UBSan）：
 *   cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
 *      -Iinclude src/push_scene.c test/push_scene_host_test.c -o /tmp/psc_test
 *
 * 判据五向量（B4 施工卡 / 点核 §三）：
 *   28B→1帧/payload36、300B→2帧、1024B→5帧、空词拒、199B 词截 63B。
 * 全绿 = 编码器可交付；真机项（APK 实际收到并弹卡）归 B11/哨位轮。
 */
#include <stdio.h>
#include <string.h>

#include "push_scene.h"

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

int main(void) {
    uint8_t frame[PSC_FRAME_MAX];

    /* ---- 向量 1：28B 词 → 1 帧 / payload 36 ----
       word_len=28, ctx="" (0B 也可成帧)。首帧 payload = 1+28+2+1+ctx_len。
       取 ctx_len=4 得 payload=4+28+4=36（施工卡口径）。 */
    {
        uint8_t word[28];
        memset(word, 'a', sizeof(word));
        const uint8_t ctx[4] = {'t', 'e', 's', 't'};
        psc_plan_t plan;
        int n = psc_plan(ctx, 4, 28, &plan);
        CHECK(n == 1);
        CHECK(psc_frame_encode(0, word, 28, 0, 0, ctx, 4, &plan, frame) == 241);
        CHECK(frame[0] == 0x1A);
        CHECK(frame[1] == 0 && frame[2] == 0);          /* seq=0 LE */
        CHECK(frame[3] == 1 && frame[4] == 0);          /* total=1 LE */
        uint16_t payload = (uint16_t)(frame[5] | (frame[6] << 8));
        CHECK(payload == 36);                           /* 判据数字 */
        CHECK(frame[7] == 28);                          /* word_len */
        CHECK(memcmp(frame + 8, word, 28) == 0);        /* word 原样 */
        CHECK(frame[8 + 28 + 2] == 0);                  /* flags */
        CHECK(memcmp(frame + 11 + 28, ctx, 4) == 0);    /* ctx 原样 */
        /* 零填充纪律：有效区之后应全 0 */
        for (size_t i = 11u + 28u + 4u; i < PSC_FRAME_MAX; ++i) CHECK(frame[i] == 0);
    }

    /* ---- 向量 2：300B ctx → 2 帧 ---- */
    {
        uint8_t word[4] = {'w', 'o', 'r', 'd'};
        uint8_t ctx[300];
        for (size_t i = 0; i < sizeof(ctx); ++i) ctx[i] = 'x';
        psc_plan_t plan;
        int n = psc_plan(ctx, 300, 4, &plan);
        CHECK(n == 2);
        CHECK(plan.len[0] + plan.len[1] == 300);
        CHECK(plan.off[0] == 0 && plan.off[1] == plan.len[0]);
        /* 首帧 ctx 容量 = 241-(11+4)=226；顺序帧 = 241-7=234 */
        CHECK(plan.len[0] == 226);
        CHECK(plan.len[1] == 74);
        uint16_t cap0 = 241u - (11u + 4u), capN = 241u - 7u;
        CHECK(plan.len[0] <= cap0 && plan.len[1] <= capN);
        for (int s = 0; s < n; ++s) {
            CHECK(psc_frame_encode((uint16_t)s, word, 4, 0, 0, ctx, 300, &plan, frame) == 241);
            CHECK(frame[0] == 0x1A && frame[2] == 0 && frame[3] == 2 && frame[4] == 0);
            uint16_t p = (uint16_t)(frame[5] | (frame[6] << 8));
            CHECK(p == plan.len[s] + (s == 0 ? 8u : 0u)); /* 首帧多 8B 元数据 */
        }
        /* 后续帧重编码并解码：word_len 报告为 0、无元数据区 */
        CHECK(psc_frame_encode(1, word, 4, 0, 0, ctx, 300, &plan, frame) == 241);
        uint8_t dec_word[8]; size_t dec_wlen; const uint8_t* dec_ctx; size_t dec_clen;
        int kind = psc_frame_decode(frame, 241, NULL, NULL, NULL,
                                    dec_word, &dec_wlen, NULL, NULL, &dec_ctx, &dec_clen);
        CHECK(kind == 0 && dec_wlen == 0); /* 后续帧无 word_len 字段 */
        CHECK(dec_clen == 74);             /* 纯 ctx 切片 */
    }

    /* ---- 向量 3：1024B ctx → 5 帧（上限） ---- */
    {
        const uint8_t word[2] = {'h', 'i'};
        (void)word;
        uint8_t ctx[1024];
        for (size_t i = 0; i < sizeof(ctx); ++i) ctx[i] = 'z';
        psc_plan_t plan;
        int n = psc_plan(ctx, 1024, 2, &plan);
        CHECK(n == 5);
        size_t sum = 0;
        for (int s = 0; s < n; ++s) sum += plan.len[s];
        CHECK(sum == 1024);
        /* >1024 拒 */
        CHECK(psc_frame_count(ctx, 1025, 2) == 0);
    }

    /* ---- 向量 4：空词拒 ---- */
    {
        const uint8_t ctx[8] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
        CHECK(psc_frame_count(ctx, 8, 0) == 0);          /* word_len=0 拒 */
        CHECK(psc_plan(ctx, 8, 0, &(psc_plan_t){0}) == 0);
        uint8_t frame_local[241];
        CHECK(psc_frame_encode(0, ctx, 0, 0, 0, ctx, 8, NULL, frame_local) == 0);
    }

    /* ---- 向量 5：199B 词截 63B ---- */
    {
        uint8_t word[199];
        memset(word, 'a', sizeof(word));
        uint8_t clamped[PSC_WORD_MAX];
        size_t n = psc_word_clamp(word, 199, clamped);
        CHECK(n == 63);
        CHECK(clamped[0] == 'a' && clamped[62] == 'a');
        /* 超长词仍可被 plan 接受（按 63 算），但原长 199 必须被拒 */
        const uint8_t ctx[4] = {'w', 'x', 'y', 'z'};
        CHECK(psc_frame_count(ctx, 4, 199) == 0);        /* 未 clamp 的超长词拒 */
        CHECK(psc_frame_count(ctx, 4, 63) == 1);         /* clamp 后可编 */
    }

    /* ---- UTF-8 整字符边界：汉字 3B 不被切断 ---- */
    {
        /* 6 个汉字 = 18 字节；首帧按 cap 切，边界必须落在 3 字节倍数 */
        const uint8_t ctx[18] = {
            0xE4,0xB8,0xAD, 0xE6,0x96,0x87, 0xE6,0xB5,0x8B,
            0xE8,0xAF,0x95, 0xE5,0x88,0x86, 0xE6,0x9E,0x90};
        const uint8_t word[2] = {'h', 'i'};
        (void)word;
        psc_plan_t plan;
        int n = psc_plan(ctx, 18, 2, &plan);
        CHECK(n == 1);
        CHECK(plan.len[0] == 18 && plan.len[0] % 3 == 0);
    }
    {
        /* 构造 ctx 使首帧 cap 落在多字节中间：cap=226 对 3B 字符不整除 */
        uint8_t ctx[400];
        for (size_t i = 0; i < 400; ++i) ctx[i] = (uint8_t)(0xE4u + (i % 3)); /* 伪 UTF-8 流 */
        /* 用真实汉字重复：'中'=E4 B8 AD */
        for (size_t i = 0; i + 2 < 400; i += 3) {
            ctx[i] = 0xE4; ctx[i+1] = 0xB8; ctx[i+2] = 0xAD;
        }
        const uint8_t word[4] = {'w','o','r','d'};
        (void)word;
        psc_plan_t plan;
        int n = psc_plan(ctx, 399, 4, &plan); /* 399 = 133 汉字 */
        CHECK(n >= 2);
        for (uint8_t s = 0; s < plan.n; ++s) CHECK(plan.len[s] % 3 == 0); /* 每帧整字符 */
        size_t sum = 0;
        for (uint8_t s = 0; s < plan.n; ++s) sum += plan.len[s];
        CHECK(sum == 399);
    }

    /* ---- decode 往返：首帧字段回读 ---- */
    {
        uint8_t word[4] = {'P', 'C', '!', '!'};
        const uint8_t ctx[3] = {'o', 'k', '!'};
        CHECK(psc_frame_encode(0, word, 4, 0x1234, PSC_FLAG_CTX_TRUNCATED,
                               ctx, 3, NULL, frame) == 241);
        uint16_t seq, total, payload, word_off;
        uint8_t rword[8], rflags;
        size_t wlen;
        const uint8_t* rctx; size_t rclen;
        int kind = psc_frame_decode(frame, 241, &seq, &total, &payload,
                                    rword, &wlen, &word_off, &rflags, &rctx, &rclen);
        CHECK(kind == 1);
        CHECK(seq == 0 && total == 1);
        CHECK(payload == 8 + 3);
        CHECK(wlen == 4 && memcmp(rword, word, 4) == 0);
        CHECK(word_off == 0x1234);
        CHECK(rflags == PSC_FLAG_CTX_TRUNCATED);
        CHECK(rclen == 3 && memcmp(rctx, ctx, 3) == 0);
        /* 非法帧：错 opcode */
        uint8_t bad[241]; memcpy(bad, frame, 241); bad[0] = 0x00;
        CHECK(psc_frame_decode(bad, 241, NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL) == -1);
    }

    printf("push_scene host test: %d checks, %d red\n", s_checks, s_red);
    return s_red ? 1 : 0;
}
