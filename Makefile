CFLAGS = -g -Wall -MMD
LDFLAGS=

CC     = gcc
CCLD   = gcc
LEX    = lex
YACC   = yacc
RM     = rm -rf

lasm: lasm.yy.o
lasm.yy.o : lasm.tab.h lasm.tab.c

%.o : %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.yy.c: %.l
	$(LEX) -t $< | sed 's/<stdout>/$@/g' > $@

%.tab.h %.tab.c: %.y
	$(YACC) -dtv $< -b $(<:.y=)

clean:
	$(RM) *.o *.tab.[ochd] *.yy.[ochd] lasm

%:
	$(CCLD) $(CFLAGS) $(LDFLAGS) -o $@ $<

-include $(wildcard *.d)
