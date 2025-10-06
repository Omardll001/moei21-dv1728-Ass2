CC_FLAGS= -Wall -I.
LD_FLAGS= -Wall -L./ 


all: libcalc test tcpserver udpserver local_bulkUDPclient bulkUDPclient

tcpservermain.o: tcpservermain.cpp
	$(CXX)  $(CC_FLAGS) $(CFLAGS) -c tcpservermain.cpp

udpservermain.o: udpservermain.cpp
	$(CXX)  $(CC_FLAGS) $(CFLAGS) -c udpservermain.cpp 

main.o: main.cpp
	$(CXX) $(CC_FLAGS) $(CFLAGS) -c main.cpp 


test: main.o calcLib.o
	$(CXX) $(LD_FLAGS) -o test main.o -lcalc

tcpserver: tcpservermain.o calcLib.o
	$(CXX) $(LD_FLAGS) -o tcpserver tcpservermain.o -lcalc

udpserver: udpservermain.o calcLib.o
	$(CXX) $(LD_FLAGS) -o udpserver udpservermain.o -lcalc

local_bulkUDPclient.o: local_bulkUDPclient.cpp protocol.h
	$(CXX) $(CC_FLAGS) $(CFLAGS) -c local_bulkUDPclient.cpp

local_bulkUDPclient: local_bulkUDPclient.o calcLib.o
	$(CXX) $(LD_FLAGS) -o local_bulkUDPclient local_bulkUDPclient.o -lcalc

bulkUDPclient.o: bulkUDPclient.cpp protocol.h
	$(CXX) $(CC_FLAGS) $(CFLAGS) -c bulkUDPclient.cpp

bulkUDPclient: bulkUDPclient.o calcLib.o
	$(CXX) $(LD_FLAGS) -o bulkUDPclient bulkUDPclient.o -lcalc


calcLib.o: calcLib.c calcLib.h
	gcc -Wall -fPIC -c calcLib.c

libcalc: calcLib.o
	ar -rc libcalc.a -o calcLib.o

clean:
	rm -f *.o *.a test tcpserver udpserver local_bulkUDPclient bulkUDPclient 
