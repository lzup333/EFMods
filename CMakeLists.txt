cmake_minimum_required(VERSION 4.4)
project("AutoFisher")
add_library(AutoFisher SHARED
            AutoFisher.cpp
)
target_include_directories(AutoFisher
        PUBLIC
        efmod-api/
        tefmod-api/
)
target_compile_options(AutoFisher PRIVATE -std=c++17)
