.PHONY: xcp
xcp : xcp.c
	gcc -g -o xcp xcp.c

run: xcp
	./$<
