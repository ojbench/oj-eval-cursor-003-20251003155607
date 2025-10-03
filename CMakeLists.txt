cmake_minimum_required(VERSION 3.10)
project(icpc_management_system LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

# Optimize for speed and reasonable memory
add_compile_options(-O2 -pipe -static-libstdc++ -static-libgcc)

add_executable(code main.cpp)

# Ensure no pthread by default; link stdc++fs only if needed (C++17)
