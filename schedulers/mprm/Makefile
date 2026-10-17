TARGET=mprm_scheduler

ifndef BUILD_DIR
	BUILD_DIR=$(CUR_DIR)
endif

CPP=g++

LDFLAGS=

.PHONY: all
all: $(BUILD_DIR)/$(TARGET).o

$(BUILD_DIR)/$(TARGET).o: $(TARGET).cpp
	$(CPP) $(CFLAGS) -c $(TARGET).cpp -o $(BUILD_DIR)/$(TARGET).o

.PHONY: clean
clean:
	rm $(BUILD_DIR)/*.o
