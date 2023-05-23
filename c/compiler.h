#ifndef clox_compiler_h
#define clox_compiler_h

#include "object.h"
#include "vm.h"

/* Scanning on Demand compiler-h < Compiling Expressions compile-h
void compile(const char* source);
*/
/* Compiling Expressions compile-h < Calls and Functions compile-h
bool compile(const char* source, Chunk* chunk);
*/
ObjFunction* compile(const char* source);
void markCompilerRoots();

#endif
