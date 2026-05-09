cmake -S . -B build -DCMAKE_CXX_COMPILER=g++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --config Release
cd build
vdlisp.exe ../tests/pool_test.lisp