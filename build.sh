#!/bin/bash
if [ -d "build" ]; then
	rm -r build
fi
mkdir build
cd build
cmake ..
make -j8 ser_cli