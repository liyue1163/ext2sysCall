FLAGS = -Wall -g
DEPENDENCIES = ext2.h util.h

all: ext2_ls ext2_cp ext2_mkdir ext2_ln ext2_rm

ext2_ls: ext2_ls.o util.o
	gcc ${FLAGS} -o $@ $^

ext2_cp: ext2_cp.o util.o
	gcc ${FLAGS} -o $@ $^

ext2_mkdir: ext2_mkdir.o util.o
	gcc ${FLAGS} -o $@ $^

ext2_ln: ext2_ln.o util.o
	gcc ${FLAGS} -o $@ $^

ext2_rm: ext2_rm.o util.o
	gcc ${FLAGS} -o $@ $^

%.o: %.c ${DEPENDENCIES}
	gcc ${FLAGS} -c $<

clean:
	rm *.o ext2_ls ext2_cp ext2_mkdir ext2_ln ext2_rm
