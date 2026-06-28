# 主机侧纯逻辑单元测试（需要 g++，C++17）。
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    & g++ -std=c++17 -O2 -Wall test/run_tests.cpp -o test/run_tests.exe
    if ($LASTEXITCODE -ne 0) { throw "compile failed" }
    & ./test/run_tests.exe
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
