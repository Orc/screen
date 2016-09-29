CFILES= screen.c ansi.c
OFILES= screen.o ansi.o
CFLAGS+= -std=c89

screen: $(OFILES)
	cc $(CFLAGS) -o screen $(OFILES) -ltermcap

screen.o: screen.c screen.h
	cc $(CFLAGS) -c screen.c

ansi.o: ansi.c screen.h
	cc $(CFLAGS) -c ansi.c
