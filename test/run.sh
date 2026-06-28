#!/usr/bin/env bash
# 主机侧纯逻辑单元测试（需要 g++，C++17）。
set -e
cd "$(dirname "$0")/.."
g++ -std=c++17 -O2 -Wall test/run_tests.cpp -o test/run_tests
./test/run_tests
