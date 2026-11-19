/*
 * B6.5 S3 — PushTextFilter host 断言。gcc + ASan/UBSan，无硬件。
 * 覆盖：六步逐条 / 代码块红线 / 幂等 5 组 / 未闭合围栏 / UTF-8 残字节。
 */
#include "PushTextFilter.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_red = 0;

#define CHECK(cond, msg)                                       \
    do {                                                       \
        ++g_checks;                                            \
        if (!(cond)) {                                         \
            ++g_red;                                           \
            printf("  RED: %s (line %d)\n", (msg), __LINE__);  \
        }                                                      \
    } while (0)

/// 跑一次清洗，断言输出等于 expect，返回 stats 供进一步断言。
static push_filter_stats_t expect_clean(const char* in, const char* expect, const char* label) {
    char* out = NULL;
    push_filter_stats_t st;
    size_t n = push_text_filter(in, strlen(in), &out, &st);
    printf("[%s] in=%zu out=%zu\n", label, strlen(in), n);
    CHECK(out != NULL, "non-null output");
    if (out) {
        CHECK(n == strlen(expect), "output length");
        if (n == strlen(expect)) {
            CHECK(memcmp(out, expect, n) == 0, "output bytes");
            if (memcmp(out, expect, n) != 0) {
                printf("    expect=[%s]\n    got   =[%s]\n", expect, out);
            }
        }
        push_text_filter_free(out);
    }
    return st;
}

/// 幂等：清洗结果再清洗一次，逐字不变。
static void expect_idempotent(const char* in, const char* label) {
    char* o1 = NULL;
    size_t n1 = push_text_filter(in, strlen(in), &o1, NULL);
    CHECK(o1 != NULL && n1 == strlen(o1 ? o1 : ""), "idempotent pass1 non-null");
    if (!o1) return;
    char* o2 = NULL;
    size_t n2 = push_text_filter(o1, n1, &o2, NULL);
    CHECK(o2 != NULL && n2 == n1, "idempotent length");
    if (o2 && n2 == n1) {
        CHECK(memcmp(o1, o2, n1) == 0, "idempotent bytes");
        if (memcmp(o1, o2, n1) != 0) {
            printf("    [%s] pass1=[%s]\n    [%s] pass2=[%s]\n", label, o1, label, o2);
        }
    }
    push_text_filter_free(o1);
    push_text_filter_free(o2);
}

/// 纯文本向量：清洗后逐字不变（强幂等，输入==输出）。
static void expect_unchanged(const char* plain, const char* label) {
    expect_clean(plain, plain, label);
}

int main(void) {
    printf("=== PushTextFilter host test ===\n");

    /* --- ① 控制字符过滤 --- */
    {
        expect_clean("a\x01" "b\x7F" "c", "abc", "step1-c0-del");
        /* 零宽 U+200B + BOM 在行首应被移除；tab 保留 */
        expect_clean("\xEF\xBB\xBF" "hi\xE2\x80\x8B" "there", "hithere", "step1-zwsp-bom");
        expect_clean("a\tb", "a\tb", "step1-tab-kept");
    }

    /* --- ② UTF-8 边界修复：末尾半个汉字丢弃 --- */
    {
        char in[64];
        memcpy(in, "你好", 6);
        memcpy(in + 6, "\xE4", 1); /* '你' 的首字节，残 1 字节 */
        in[7] = 0;
        char* out = NULL;
        push_filter_stats_t st;
        size_t n = push_text_filter(in, 7, &out, &st);
        CHECK(out != NULL, "utf8-tail non-null");
        CHECK(n == 6 && memcmp(out, "你好", 6) == 0, "utf8-tail dropped half-char");
        CHECK(st.dropped_utf8_tail == 1, "utf8-tail dropped count=1");
        push_text_filter_free(out);
    }

    /* --- ③ 换行统一：CRLF/CR → LF，3+ 连续换行压成 2 --- */
    {
        expect_clean("a\r\nb\rc", "a\nb\nc", "step3-newline-unify");
        expect_clean("a\n\n\n\nb", "a\n\nb", "step3-blank-compress");
    }

    /* --- ④ 空白折叠：行首(含全角)/行尾/连续空格 --- */
    {
        /* 段首半角与全角空格削除、行尾削除、连续空格压缩 */
        expect_clean("  hello   world  ", "hello world", "step4-ascii-fold");
        expect_clean("\xE3\x80\x80" "begin", "begin", "step4-ideographic-leading");
    }

    /* --- ⑤ Markdown 剥除 --- */
    {
        expect_clean("### Title", "Title", "step5-heading");
        expect_clean("**bold** and __strong__", "bold and strong", "step5-emphasis-strong");
        expect_clean("*em* and _it_", "em and it", "step5-emphasis-em");
        expect_clean("use `code` here", "use code here", "step5-codespan");
        expect_clean("> quoted", "quoted", "step5-quote");
        expect_clean("- apple", "\xC2\xB7" " apple", "step5-list-dash");
        expect_clean("1. first", "1. first", "step5-ordered-kept");
        expect_clean("[label](http://x.com)", "label", "step5-link");
        expect_clean("---", "", "step5-hr");
        expect_clean("<b>text</b>", "text", "step5-html");
    }

    /* --- ⑥ 图片占位符剔除 --- */
    {
        expect_clean("![alt](x.png)", "", "step6-md-image");
        expect_clean("[Image]", "", "step6-image-word");
        expect_clean("[图片]", "", "step6-image-cn");
        expect_clean("[图片: cover]", "", "step6-image-cn-desc");
        /* 混合：图片夹在文字中，文字保留 */
        expect_clean("before [图片] after", "before  after", "step6-image-inline");
    }

    /* --- 代码块红线：``` 内缩进/行尾空格/连续空格/markdown 全保留 --- */
    {
        const char* in =
            "```\n"
            "  int x = 1;   \n"
            "    **keep** and # not-heading\n"
            "```\n"
            "outside **bold**";
        const char* exp =
            "  int x = 1;   \n"
            "    **keep** and # not-heading\n"
            "outside bold";
        expect_clean(in, exp, "codeblock-preserve");
    }

    /* --- 未闭合 ```：到文末都当代码块，正文标记原样保留、缩进不削 --- */
    {
        expect_clean("```\n  raw **stars**  ", "  raw **stars**  ", "unclosed-fence");
    }

    /* --- 诗歌短行：逐行保留，不合并 --- */
    {
        expect_clean("床前明月光\n疑是地上霜", "床前明月光\n疑是地上霜", "poetry-lines");
    }

    /* --- 幂等 5 组 --- */
    {
        expect_idempotent("这是一段纯中文文本，没有多余空白。", "idem-cn");
        expect_idempotent("This is a plain English sentence.", "idem-en");
        expect_idempotent("hello 世界 mixed 123 text", "idem-mixed");
        expect_idempotent("标点：，。！？、；：（）「」 —— test.", "idem-punct");
        expect_idempotent("数字 0123 和 45.67 percent 100%", "idem-digit");
    }

    /* --- 无标记纯文本逐字不变（强幂等） --- */
    {
        expect_unchanged("第一行内容。\n第二行内容。", "plain-unchanged");
    }

    printf("=== %d checks, %d RED ===\n", g_checks, g_red);
    return g_red ? 1 : 0;
}
