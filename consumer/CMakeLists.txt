cmake_minimum_required(VERSION 3.20)
project(TosDownstreamConsumer LANGUAGES CXX)

find_package(TransportOffloadScheduler CONFIG REQUIRED)

add_executable(tos_consumer main.cpp)
target_link_libraries(tos_consumer PRIVATE SummonSoftwareLabs::TransportOffloadScheduler)
target_compile_features(tos_consumer PRIVATE cxx_std_20)
