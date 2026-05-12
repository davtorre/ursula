CC       ?= cc
CFLAGS   ?= -Wall -Wextra -pedantic -std=c99 -g -O0
LDFLAGS  ?= -lm

EXAMPLE_DIR = examples

.PHONY: all clean

all: $(EXAMPLE_DIR)/example_sum

$(EXAMPLE_DIR)/example_sum: $(EXAMPLE_DIR)/example_sum.c df/skn_df.h
	$(CC) $(CFLAGS) -I. -o $@ $< $(LDFLAGS)

clean:
	rm -f $(EXAMPLE_DIR)/example_sum
