CC = gcc
# CFLAGS += -Wall -fPIC -DPIC -std=c99
CFLAGS += -Wall -fPIC -DPIC 
LDFLAGS += -Wall -shared -lasound
LIBDIR := lib/$(shell gcc --print-multiarch)

TARGET = libasound_module_pcm_cdsp

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET).so $(TARGET).c

install:
	mkdir -p  /usr/$(LIBDIR)/alsa-lib/
	install -m 644 $(TARGET).so /usr/$(LIBDIR)/alsa-lib/

uninstall:
	rm /usr/$(LIBDIR)/alsa-lib/$(TARGET).so

clean:
	rm $(TARGET).so
