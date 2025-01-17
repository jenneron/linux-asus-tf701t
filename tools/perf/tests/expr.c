// SPDX-License-Identifier: GPL-2.0
#include "util/debug.h"
#include "util/expr.h"
#include "util/smt.h"
#include "tests.h"
#include <stdlib.h>
#include <string.h>
#include <linux/zalloc.h>

static int test_ids_union(void)
{
	struct hashmap *ids1, *ids2;

	/* Empty union. */
	ids1 = ids__new();
	TEST_ASSERT_VAL("ids__new", ids1);
	ids2 = ids__new();
	TEST_ASSERT_VAL("ids__new", ids2);

	ids1 = ids__union(ids1, ids2);
	TEST_ASSERT_EQUAL("union", (int)hashmap__size(ids1), 0);

	/* Union {foo, bar} against {}. */
	ids2 = ids__new();
	TEST_ASSERT_VAL("ids__new", ids2);

	TEST_ASSERT_EQUAL("ids__insert", ids__insert(ids1, strdup("foo"), NULL), 0);
	TEST_ASSERT_EQUAL("ids__insert", ids__insert(ids1, strdup("bar"), NULL), 0);

	ids1 = ids__union(ids1, ids2);
	TEST_ASSERT_EQUAL("union", (int)hashmap__size(ids1), 2);

	/* Union {foo, bar} against {foo}. */
	ids2 = ids__new();
	TEST_ASSERT_VAL("ids__new", ids2);
	TEST_ASSERT_EQUAL("ids__insert", ids__insert(ids2, strdup("foo"), NULL), 0);

	ids1 = ids__union(ids1, ids2);
	TEST_ASSERT_EQUAL("union", (int)hashmap__size(ids1), 2);

	/* Union {foo, bar} against {bar,baz}. */
	ids2 = ids__new();
	TEST_ASSERT_VAL("ids__new", ids2);
	TEST_ASSERT_EQUAL("ids__insert", ids__insert(ids2, strdup("bar"), NULL), 0);
	TEST_ASSERT_EQUAL("ids__insert", ids__insert(ids2, strdup("baz"), NULL), 0);

	ids1 = ids__union(ids1, ids2);
	TEST_ASSERT_EQUAL("union", (int)hashmap__size(ids1), 3);

	ids__free(ids1);

	return 0;
}

static int test(struct expr_parse_ctx *ctx, const char *e, double val2)
{
	double val;

	if (expr__parse(&val, ctx, e, 1))
		TEST_ASSERT_VAL("parse test failed", 0);
	TEST_ASSERT_VAL("unexpected value", val == val2);
	return 0;
}

int test__expr(struct test *t __maybe_unused, int subtest __maybe_unused)
{
	struct expr_id_data *val_ptr;
	const char *p;
	double val;
	int ret;
	struct expr_parse_ctx *ctx;

	TEST_ASSERT_EQUAL("ids_union", test_ids_union(), 0);

	ctx = expr__ctx_new();
	TEST_ASSERT_VAL("expr__ctx_new", ctx);
	expr__add_id_val(ctx, strdup("FOO"), 1);
	expr__add_id_val(ctx, strdup("BAR"), 2);

	ret = test(ctx, "1+1", 2);
	ret |= test(ctx, "FOO+BAR", 3);
	ret |= test(ctx, "(BAR/2)%2", 1);
	ret |= test(ctx, "1 - -4",  5);
	ret |= test(ctx, "(FOO-1)*2 + (BAR/2)%2 - -4",  5);
	ret |= test(ctx, "1-1 | 1", 1);
	ret |= test(ctx, "1-1 & 1", 0);
	ret |= test(ctx, "min(1,2) + 1", 2);
	ret |= test(ctx, "max(1,2) + 1", 3);
	ret |= test(ctx, "1+1 if 3*4 else 0", 2);
	ret |= test(ctx, "1.1 + 2.1", 3.2);
	ret |= test(ctx, ".1 + 2.", 2.1);
	ret |= test(ctx, "d_ratio(1, 2)", 0.5);
	ret |= test(ctx, "d_ratio(2.5, 0)", 0);
	ret |= test(ctx, "1.1 < 2.2", 1);
	ret |= test(ctx, "2.2 > 1.1", 1);
	ret |= test(ctx, "1.1 < 1.1", 0);
	ret |= test(ctx, "2.2 > 2.2", 0);
	ret |= test(ctx, "2.2 < 1.1", 0);
	ret |= test(ctx, "1.1 > 2.2", 0);

	if (ret) {
		expr__ctx_free(ctx);
		return ret;
	}

	p = "FOO/0";
	ret = expr__parse(&val, ctx, p, 1);
	TEST_ASSERT_VAL("division by zero", ret == -1);

	p = "BAR/";
	ret = expr__parse(&val, ctx, p, 1);
	TEST_ASSERT_VAL("missing operand", ret == -1);

	expr__ctx_clear(ctx);
	TEST_ASSERT_VAL("find ids",
			expr__find_ids("FOO + BAR + BAZ + BOZO", "FOO",
					ctx, 1) == 0);
	TEST_ASSERT_VAL("find ids", hashmap__size(ctx->ids) == 3);
	TEST_ASSERT_VAL("find ids", hashmap__find(ctx->ids, "BAR",
						    (void **)&val_ptr));
	TEST_ASSERT_VAL("find ids", hashmap__find(ctx->ids, "BAZ",
						    (void **)&val_ptr));
	TEST_ASSERT_VAL("find ids", hashmap__find(ctx->ids, "BOZO",
						    (void **)&val_ptr));

	expr__ctx_clear(ctx);
	TEST_ASSERT_VAL("find ids",
			expr__find_ids("EVENT1\\,param\\=?@ + EVENT2\\,param\\=?@",
					NULL, ctx, 3) == 0);
	TEST_ASSERT_VAL("find ids", hashmap__size(ctx->ids) == 2);
	TEST_ASSERT_VAL("find ids", hashmap__find(ctx->ids, "EVENT1,param=3/",
						    (void **)&val_ptr));
	TEST_ASSERT_VAL("find ids", hashmap__find(ctx->ids, "EVENT2,param=3/",
						    (void **)&val_ptr));

	/* Only EVENT1 or EVENT2 need be measured depending on the value of smt_on. */
	expr__ctx_clear(ctx);
	TEST_ASSERT_VAL("find ids",
			expr__find_ids("EVENT1 if #smt_on else EVENT2",
				NULL, ctx, 0) == 0);
	TEST_ASSERT_VAL("find ids", hashmap__size(ctx->ids) == 1);
	TEST_ASSERT_VAL("find ids", hashmap__find(ctx->ids,
						  smt_on() ? "EVENT1" : "EVENT2",
						  (void **)&val_ptr));

	/* The expression is a constant 1.0 without needing to evaluate EVENT1. */
	expr__ctx_clear(ctx);
	TEST_ASSERT_VAL("find ids",
			expr__find_ids("1.0 if EVENT1 > 100.0 else 1.0",
			NULL, ctx, 0) == 0);
	TEST_ASSERT_VAL("find ids", hashmap__size(ctx->ids) == 0);

	expr__ctx_free(ctx);

	return 0;
}
