TARGET = sqmark.elf
OBJS = main.o
KOS_BUILD_SUBARCHS = pristine
CFLAGS += -std=gnu2x -O2 -DDCBENCH_BUILD
all: rm-elf $(TARGET)
include $(KOS_BASE)/Makefile.rules
clean:
	-rm -f $(TARGET) $(OBJS)
rm-elf:
	-rm -f $(TARGET)
$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) -lm
