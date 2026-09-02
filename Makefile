CC       ?= cc
CFLAGS   ?= -Wall -Wextra -pedantic -std=c99 -g -O0
LDFLAGS  ?= -lm

EXAMPLE_DIR = examples
TEST_DIR    = test

TESTS = $(TEST_DIR)/test_csv_roundtrip \
        $(TEST_DIR)/test_group_scale \
        $(TEST_DIR)/test_sort_determinism \
        $(TEST_DIR)/test_gather \
        $(TEST_DIR)/test_match_resample_stack \
        $(TEST_DIR)/test_dynamo_assignment

.PHONY: all test clean

all: $(EXAMPLE_DIR)/example_sum $(TESTS)

test: $(TESTS)
	./$(TEST_DIR)/test_csv_roundtrip
	./$(TEST_DIR)/test_group_scale
	./$(TEST_DIR)/test_sort_determinism
	./$(TEST_DIR)/test_gather
	./$(TEST_DIR)/test_match_resample_stack
	./$(TEST_DIR)/test_dynamo_assignment

$(EXAMPLE_DIR)/example_sum: $(EXAMPLE_DIR)/example_sum.c df/skn_df.h
	$(CC) $(CFLAGS) -I. -o $@ $< $(LDFLAGS)

$(TEST_DIR)/test_csv_roundtrip: $(TEST_DIR)/test_csv_roundtrip.c df/skn_df.h
	$(CC) $(CFLAGS) -I. -o $@ $< $(LDFLAGS)

$(TEST_DIR)/test_group_scale: $(TEST_DIR)/test_group_scale.c df/skn_df.h
	$(CC) $(CFLAGS) -I. -o $@ $< $(LDFLAGS)

$(TEST_DIR)/test_sort_determinism: $(TEST_DIR)/test_sort_determinism.c df/skn_df.h
	$(CC) $(CFLAGS) -I. -o $@ $< $(LDFLAGS)

$(TEST_DIR)/test_gather: $(TEST_DIR)/test_gather.c df/skn_df.h
	$(CC) $(CFLAGS) -I. -o $@ $< $(LDFLAGS)

$(TEST_DIR)/test_match_resample_stack: $(TEST_DIR)/test_match_resample_stack.c df/skn_df.h
	$(CC) $(CFLAGS) -I. -o $@ $< $(LDFLAGS)

$(TEST_DIR)/test_dynamo_assignment: $(TEST_DIR)/test_dynamo_assignment.c df/skn_df.h
	$(CC) $(CFLAGS) -I. -o $@ $< $(LDFLAGS)

clean:
	rm -f $(EXAMPLE_DIR)/example_sum $(TESTS)
	rm -f test_roundtrip_out.csv test_escaping_input.csv test_escaping_out.csv
