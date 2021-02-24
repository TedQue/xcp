xcp: xcp.c
	gcc -g -m64 -o xcp xcp.c

run: xcp
	./xcp

TAGS:
	ctags -R .

install: xcp
	./xcp -vf ./xcp /usr/local/bin/

uninstall:
	-rm /usr/local/bin/xcp
