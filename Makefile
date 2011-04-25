CC=msp430-gcc
CFLAGS=-Os -Wall -g -mmcu=msp430x2012

OBJS=main.o


all: $(OBJS)
	$(CC) $(CFLAGS) -o main.elf $(OBJS)
	msp430-size main.elf

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -fr main.elf $(OBJS)

run: all
	mspdebug rf2500 "prog main.elf" run
