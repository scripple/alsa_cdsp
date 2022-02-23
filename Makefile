CC = gcc
CFLAGS += -Wall -Wextra -fPIC -DPIC 
#LDFLAGS += -Wall -shared -lasound
#LDFLAGS below is needed in Debian Buster for certain sound players
LDFLAGS += -Wall -shared -Wl,--no-as-needed -lasound -Wl,--as-needed
#LIBDIR := lib/$(shell gcc --print-multiarch)
LIBDIR := $(shell pkg-config --variable=libdir alsa)

TARGET = libasound_module_pcm_cdsp

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET).so $(TARGET).c

install:
	mkdir -p  $(LIBDIR)/alsa-lib/
	install -m 644 $(TARGET).so $(LIBDIR)/alsa-lib/

uninstall:
	rm $(LIBDIR)/alsa-lib/$(TARGET).so

clean:
	rm $(TARGET).so
