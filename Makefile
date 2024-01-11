ALL=mlog

CFLAGS=-g -O2 -Wall -Wno-unused-parameter -Wextra -Wwrite-strings

all: $(ALL)

README: mlog.1
	mandoc -Tutf8 $< | col -bx >$@

clean: FRC
	rm -f $(ALL)

FRC:
