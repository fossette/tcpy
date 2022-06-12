tcpy: tcpy.c
	cc -g -v -o tcpy tcpy.c

clean:
	rm tcpy

install:
	cp tcpy /usr/bin
	chmod a+rx /usr/bin/tcpy

uninstall:
	rm /usr/bin/tcpy
