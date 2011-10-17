CXXFLAGS = -O2 -g -Wall -fmessage-length=0

OBJS = js/libjs_static.a libev-4.04/ev.o

TARGET = servo

INCLUDE = -Ijs/dist/include

$(TARGET):
	$(CXX) $(INCLUDE) -o $(TARGET) $(OBJS) main.c

all:	$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
