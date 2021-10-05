# Duktape-Based Engine
An old Duktape-based JavaScript engine used in older versions of Let's Code.

## Building
```sh
$ ./download-duktape.sh
$ mkdir build && cd build
$ cmake ..
$ make -j32
```

## Usage
```sh
$ cat test.js
a.b = 0;
$ ./js test.js <Path To data/babel.min.js> <Path To data/polyfill.min.js>
[logcat] Transpiling...
[logcat] Running...
ReferenceError: identifier 'a' undefined
    at [anon] (duk_js_var.c:1234) internal
    at global (input:1)
    at [anon] (input:1) preventsyield
[logcat] Finalizing the Javascript interpreter
```

## WARNING
This isn't very well tested and could have bugs.
