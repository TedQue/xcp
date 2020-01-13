xcp: xcp.c
	gcc -g -m64 -o xcp xcp.c

run: xcp
	./xcp

TAGS:
	ctags -R .

install: xcp
	./xcp -v ./xcp /usr/local/bin/
