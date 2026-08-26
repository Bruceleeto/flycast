TARGET = fpustall.elf
OBJS = main.o
KOS_BUILD_SUBARCHS = pristine
all: rm-elf $(TARGET)
include $(KOS_BASE)/Makefile.rules
clean:
	-rm -f $(TARGET) $(OBJS)
rm-elf:
	-rm -f $(TARGET)
$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) -lm
