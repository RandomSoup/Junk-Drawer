# QuickJS-Based Engine
An old QuickJS-based JavaScript engine used in older versions of Let's Code.

## Building
```sh
$ ./download-quickjs.sh
$ mkdir build && cd build
$ cmake ..
$ make -j32
```

## Usage
```sh
$ cat test.js
a.b = 0;
$ ./js test.js input
ReferenceError: 'a' is not defined
    at <eval> (input)
    at <internal> (native)
    at <anonymous> (<init>)
```

## WARNING
This isn't very well tested and could have bugs.
