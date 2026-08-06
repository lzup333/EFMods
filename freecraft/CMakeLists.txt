cmake_minimum_required(VERSION 4.4)
project("FreeCraft")
add_library(FreeCraft SHARED
            FreeCraft.cpp
)
target_include_directories(FreeCraft
        PUBLIC
        efmod-api/
        tefmod-api/
)
target_compile_options(FreeCraft PRIVATE -std=c++17)
