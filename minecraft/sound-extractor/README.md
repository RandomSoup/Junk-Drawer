# MCPE Sound Extractor
This program extracts raw sound data from a pre-v0.12.0 `libminecraftpe.so` file into a C file that can be compiled into a fake `libminecraftpe.so` that only contains sound data.

## Usage
```sh
$ clang++ -o extract ./extract.cpp 
$ ./extract [Path To libminecraftpe.so] > minecraftpe.c
$ arm-linux-gnueabihf-gcc -shared -o libminecraftpe.so minecraftpe.c 
```
