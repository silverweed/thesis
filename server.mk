CFLAGS = -std=c++14 -Wall -pedantic -Wextra -ggdb 
INC = -Isrc -Isrc/third_party -I/usr/local/src/VulkanSDK/1.0.10.1/x86_64/include
LIB = -lpthread

server: server.o src/model.o src/vertex.o src/endpoint.o src/server_endpoint.o src/camera.o src/serialization.o src/server_appstage.o src/endpoint_xplatform.o
	g++ $(CFLAGS) $(INC) -o $@ $^ $(LIB)

server.o: server.cpp
	g++ $(CFLAGS) $(INC) -c server.cpp -o $@

%.o: %.cpp %.hpp src/config.hpp
	g++ -c $(CFLAGS) $(INC) $< -o $@

.PHONY: clean
clean:
	rm src/*.o server -f
