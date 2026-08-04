cmake_minimum_required(VERSION 4.4)
project("VeinMiner")
add_library(VeinMiner SHARED
            VeinMiner.cpp
)
target_include_directories(VeinMiner
        PUBLIC
        efmod-api/
        tefmod-api/
)
target_compile_options(VeinMiner PRIVATE -std=c++17)
