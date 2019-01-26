CFLAGS=-Wall -g

all: ext2_mkdir ext2_cp ext2_ln ext2_rm ext2_restore ext2_checker

ext2_mkdir :  ext2_mkdir.c helper.c
	gcc $(CFLAGS) -o ext2_mkdir $^

ext2_cp :  ext2_cp.c helper.c
	gcc $(CFLAGS) -o ext2_cp $^

ext2_ln :  ext2_ln.c helper.c
	gcc $(CFLAGS) -o ext2_ln $^

ext2_rm :  ext2_rm.c helper.c
	gcc $(CFLAGS) -o ext2_rm $^

ext2_restore :  ext2_restore.c helper.c
	gcc $(CFLAGS) -o ext2_restore $^

ext2_checker :  ext2_checker.c helper.c
	gcc $(CFLAGS) -o ext2_checker $^

clean :
	rm ext2_mkdir ext2_cp ext2_ln ext2_rm ext2_restore ext2_checker
