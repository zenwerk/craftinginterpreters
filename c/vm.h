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

// CallFrame は呼び出し中(実行中)の関数を表す構造体 -> 関数呼び出しのたびに一つ生成される.
// slots -> 関数のローカル変数がスタックのどこから始まるのか.
// 関数の呼び出し毎にリターンアドレスを追跡していなければならない.
// - 呼び出されたCallFrameがReturnしたとき, 呼び出した側の CallFrame の *ip から動作を再開すればよい.
// - よって呼び出し側CFの *ip がリターンアドレスと同じ意味をもつ.
typedef struct {
  ObjClosure *closure; // 呼び出されるクロージャ(関数)へのポインタ
  uint8_t *ip;  // 命令ポインタ(現在のバイトコード命令のアドレスを指す) -> 実行中のプログラムの「現在地」とも言える.
  Value *slots; // この関数が使用できる最初のスロット `VM{Value stack[];}` への Valueポインタで指す.
} CallFrame;

// 仮想マシン
typedef struct {
/* A Virtual Machine vm-h < Calls and Functions frame-array
  Chunk* chunk;
*/
/* A Virtual Machine ip < Calls and Functions frame-array
  uint8_t* ip;
*/
  CallFrame frames[FRAMES_MAX]; // 関数呼び出しは核なので毎回ヒープを確保すると遅いので事前にスタック(配列)として確保しておく.
  int frameCount; // CallFrameスタックの現在の高さ(進行中の関数呼び出しの数).

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
