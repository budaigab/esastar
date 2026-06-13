all:
	g++ query.cpp -o query.out -std=c++20 -pthread -lssl -lcrypto