ifndef BUILD_DIR
	BUILD_DIR=$(CUR_DIR)
endif

CPP=g++

LDFLAGS=

.PHONY: all
all: earliest_deadline_first_rule rate_monotonic_rule chedf_rule charm_rule scs_rule bcs_rule cats_rule

earliest_deadline_first_rule: earliest_deadline_first/Makefile
	$(MAKE) CFLAGS="$(CFLAGS)" BUILD_DIR=$(BUILD_DIR) -C earliest_deadline_first

rate_monotonic_rule: rate_monotonic/Makefile
	$(MAKE) CFLAGS="$(CFLAGS)" BUILD_DIR=$(BUILD_DIR) -C rate_monotonic

charm_rule: charm/Makefile
	$(MAKE) CFLAGS="$(CFLAGS)" BUILD_DIR=$(BUILD_DIR) -C charm

chedf_rule: chedf/Makefile
	$(MAKE) CFLAGS="$(CFLAGS)" BUILD_DIR=$(BUILD_DIR) -C chedf

scs_rule: scs/Makefile
	$(MAKE) CFLAGS="$(CFLAGS)" BUILD_DIR=$(BUILD_DIR) -C scs

bcs_rule: bcs/Makefile
	$(MAKE) CFLAGS="$(CFLAGS)" BUILD_DIR=$(BUILD_DIR) -C bcs

cats_rule: cats/Makefile
	$(MAKE) CFLAGS="$(CFLAGS)" BUILD_DIR=$(BUILD_DIR) -C cats

.PHONY: clean
clean:
	rm $(BUILD_DIR)/*.o

