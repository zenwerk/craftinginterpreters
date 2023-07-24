#ifndef clox_vm_h
#define clox_vm_h

/* A Virtual Machine vm-h < Calls and Functions vm-include-object
#include "chunk.h"
*/
#include "object.h"
#include "table.h"
#include "value.h"

/* A Virtual Machine stack-max < Calls and Functions frame-max
#define STACK_MAX 256
*/
#define FRAMES_MAX 64
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)

typedef struct {
/* Calls and Functions call-frame < Closures call-frame-closure
  ObjFunction* function;
*/
  ObjClosure *closure;
  uint8_t *ip; // 命令ポインタ(現在のバイトコード命令のアドレスを指す) -> 実行中のプログラムの「現在地」とも言える
  Value *slots;
} CallFrame;

// 仮想マシン
typedef struct {
/* A Virtual Machine vm-h < Calls and Functions frame-array
  Chunk* chunk;
*/
/* A Virtual Machine ip < Calls and Functions frame-array
  uint8_t* ip;
*/
  CallFrame frames[FRAMES_MAX];
  int frameCount;

  Value stack[STACK_MAX]; // デフォルトで (64*(255+1))
  Value *stackTop; // スタックポインタ
  Table globals; // グローバル変数
  Table strings; // 文字列プール
  ObjString *initString;
  ObjUpvalue *openUpvalues;

  size_t bytesAllocated; // 確保したメモリ領域
  size_t nextGC; // 次GCを起動するときのサイズ
  Obj *objects; // VMに確保されたオブジェクトの連結リストの先頭アドレス
  int grayCount;
  int grayCapacity;
  Obj **grayStack;
} VM;

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR
} InterpretResult;

extern VM vm; // グローバル変数vmを参照できるように.

void initVM();

void freeVM();

/* A Virtual Machine interpret-h < Scanning on Demand vm-interpret-h
InterpretResult interpret(Chunk* chunk);
*/
InterpretResult interpret(const char *source);

void push(Value value);

Value pop();

#endif
