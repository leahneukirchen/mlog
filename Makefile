CFLAGS=-O2 -Wall -Wno-unused-parameter -Wextra -Wwrite-strings

ALL=mlog

all: $(ALL)

clean: FRC
	rm -f $(ALL)

FRC:
