//===-- main.cpp ------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <cstdio>

int
main(int argc, char const *argv[])
{
    int a  = 10;

    printf("argc=%d\n", argc); // BP_printf
    return 0;
}
