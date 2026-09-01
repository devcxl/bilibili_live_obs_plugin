cmake_minimum_required(VERSION 3.16)
project(azure_tts_poc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Multimedia)

add_executable(azure_tts_poc
    main.cpp
    azure_tts_client.h
    azure_tts_client.cpp
    pcm_audio_player.h
    pcm_audio_player.cpp
)

target_link_libraries(azure_tts_poc PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Multimedia
)
