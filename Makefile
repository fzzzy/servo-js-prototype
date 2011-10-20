
all:	deps/dom.js servo

deps/dom.js:
	mkdir -p deps
	cd deps && git clone git://github.com/andreasgal/dom.js.git && cd dom.js && git submodule init && git submodule update && make

deps/mozilla-central:
	cd deps && hg clone http://hg.mozilla.org/mozilla-central
	cd deps/mozilla-central/js/src && autoconf-2.13 && mkdir build-servo && cd build-servo && ../configure && make

deps/libev-4.04:
	cd deps && curl -O http://dist.schmorp.de/libev/libev-4.04.tar.gz && tar xzf libev-4.04.tar.gz && rm libev-4.04.tar.gz
	cd deps/libev-4.04 && ./configure && make

clean:
	rm -f $(OBJS) $(TARGET)

CXXFLAGS = -O2 -g -Wall -fmessage-length=0

OBJS = deps/mozilla-central/js/src/build-servo/libjs_static.a deps/libev-4.04/ev.o

INCLUDE = -Ideps/mozilla-central/js/src/build-servo/dist/include -Ideps/mozilla-central/js/src/build-servo

servo: deps/mozilla-central deps/libev-4.04 main.c
	g++-4.2 $(INCLUDE) -o servo $(OBJS) main.c

