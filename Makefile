CFLAGS ?= -W -Wall -Wextra -Werror -Wundef -Wshadow -Wdouble-promotion -Os $(EXTRA_CFLAGS)
BINDIR ?= .
CWD ?= $(realpath $(CURDIR))
DOCKER = docker run $(DA) --rm -e Tmp=. -e WINEDEBUG=-all -v $(CWD):$(CWD) -w $(CWD)
SERIAL_PORT ?= /dev/ttyUSB0

all: esputil

esputil: esputil.c
	$(CC) $(CFLAGS) $? -o $(BINDIR)/$@

esputil.exe: esputil.c
	$(DOCKER) mdashnet/vc98 wine cl /nologo /W3 /MD /Os $? ws2_32.lib /Fe$@

wintest: esputil.exe
	ln -fs $(SERIAL_PORT) ~/.wine/dosdevices/com55 && wine $? -p '\\.\COM55' -v info

clean:
	rm -rf esputil *.dSYM *.o *.obj _CL* *.exe
