CPP = g++
CFLAGS =-Wall -I$(CURDIR) -I$(CURDIR)/libs

ifndef $(BUILD_DIR)
	BUILD_DIR=$(CURDIR)
endif

.PHONY: all
all: buffer_packet_rule radio_interface_rule target_receiver_rule ml_predictor_rule channel_model_rule

buffer_packet_rule: buffer_packet/Makefile
	$(MAKE) CFLAGS="$(CFLAGS)" BUILD_DIR=$(BUILD_DIR) -C buffer_packet

radio_interface_rule: radio_interface/Makefile
	$(MAKE) CFLAGS="$(CFLAGS)" BUILD_DIR=$(BUILD_DIR) -C radio_interface

target_receiver_rule: target_receiver/Makefile
	$(MAKE) CFLAGS="$(CFLAGS)" BUILD_DIR=$(BUILD_DIR) -C target_receiver

ml_predictor_rule: ml_predictor/Makefile
	$(MAKE) CFLAGS="$(CFLAGS)" BUILD_DIR=$(BUILD_DIR) -C ml_predictor

channel_model_rule: channel_model/Makefile
	$(MAKE) CFLAGS="$(CFLAGS)" BUILD_DIR=$(BUILD_DIR) -C channel_model

clean:
	$(RM) *.o

