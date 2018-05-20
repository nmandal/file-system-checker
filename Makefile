xcheck:
	gcc xcheck.c -o xcheck -Wall -Werror -g
test:
	xcheck ./image
clean:
	rm xcheck

