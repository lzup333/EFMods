cmake_minimum_required(VERSION 4.4)
project("ChatCommands")
add_library(ChatCommands SHARED
            ChatCommands.cpp
)
target_include_directories(ChatCommands
        PUBLIC
        efmod-api/
        tefmod-api/
)
target_compile_options(ChatCommands PRIVATE -std=c++17)
